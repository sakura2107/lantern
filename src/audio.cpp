#include "lantern/win.hpp"
#include <audioclient.h>
#include <cmath>
#include <cstring>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <thread>
#include <wrl/client.h>
namespace lantern {
using Microsoft::WRL::ComPtr;
static void audio_hr(HRESULT result) {
    if (FAILED(result))
        throw Error(Code::unsupported, "System audio endpoint/format unavailable (WASAPI " +
                                           std::to_string(uint32_t(result)) + ")");
}
static uint64_t qpc_us() {
    LARGE_INTEGER counter{}, frequency{};
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return uint64_t(counter.QuadPart / frequency.QuadPart) * 1000000 +
           uint64_t(counter.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart;
}
class WasapiSource final : public AudioSource {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    bool com = false, started = false, floating = false;
    uint32_t source_channels = 0, source_bits = 0, block = 0, chunk = 0;
    AudioConfig cfg{};
    Bytes pending;
    size_t offset = 0;
    uint64_t pending_pts = 0, last_pts = 0;
    uint32_t flags = 0;
    int16_t sample(const BYTE* data) {
        if (floating) {
            float value;
            std::memcpy(&value, data, 4);
            return std::isfinite(value) ? int16_t(std::clamp(value, -1.0f, 1.0f) * 32767) : 0;
        }
        if (source_bits == 16) {
            int16_t value;
            std::memcpy(&value, data, 2);
            return value;
        }
        if (source_bits == 24) {
            int32_t value = int32_t(data[0]) | (int32_t(data[1]) << 8) | (int32_t(data[2]) << 16);
            if (value & 0x800000)
                value |= int32_t(0xff000000);
            return int16_t(value >> 8);
        }
        int32_t value;
        std::memcpy(&value, data, 4);
        return int16_t(value >> 16);
    }

  public:
    WasapiSource() {
        HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(init);
        audio_hr(init);
        try {
            audio_hr(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator)));
            audio_hr(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device));
            audio_hr(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client));
            WAVEFORMATEX* mix = nullptr;
            audio_hr(client->GetMixFormat(&mix));
            struct Free {
                WAVEFORMATEX* format;
                ~Free() {
                    CoTaskMemFree(format);
                }
            } free{mix};
            source_channels = mix->nChannels;
            source_bits = mix->wBitsPerSample;
            block = mix->nBlockAlign;
            cfg.sample_rate = mix->nSamplesPerSec;
            WORD kind = mix->wFormatTag;
            if (kind == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22) {
                auto* extended = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix);
                if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                    kind = WAVE_FORMAT_IEEE_FLOAT;
                else if (extended->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
                    kind = WAVE_FORMAT_PCM;
            }
            floating = kind == WAVE_FORMAT_IEEE_FLOAT;
            bool supported =
                (floating && source_bits == 32) ||
                (kind == WAVE_FORMAT_PCM && (source_bits == 16 || source_bits == 24 || source_bits == 32));
            if (!supported || source_channels < 1 || source_channels > 2 || cfg.sample_rate < 8000 ||
                cfg.sample_rate > 96000 || cfg.sample_rate % 100 ||
                block != source_channels * source_bits / 8)
                throw Error(Code::unsupported, "Loopback requires mono/stereo PCM16/24/32 or float32 at an "
                                               "8-96kHz multiple of 100; change Windows output format");
            chunk = cfg.sample_rate / 100;
            pending.reserve(cfg.sample_rate * 4 / 5);
            audio_hr(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 1000000, 0,
                                        mix, nullptr));
            audio_hr(client->GetService(IID_PPV_ARGS(&capture)));
            audio_hr(client->Start());
            started = true;
        } catch (...) {
            cleanup();
            throw;
        }
    }
    void cleanup() noexcept {
        if (started)
            client->Stop();
        started = false;
        capture.Reset();
        client.Reset();
        device.Reset();
        enumerator.Reset();
        if (com)
            CoUninitialize();
        com = false;
    }
    ~WasapiSource() override {
        cleanup();
    }
    const AudioConfig& config() const override {
        return cfg;
    }
    AudioFrame next() override {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        while (pending.size() - offset < chunk * 4) {
            UINT32 frames = 0;
            audio_hr(capture->GetNextPacketSize(&frames));
            if (!frames) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    if (pending.size() > offset) {
                        AudioFrame partial{std::max(last_pts + 1, pending_pts), flags | 1,
                                           Bytes(pending.begin() + offset, pending.end())};
                        pending.clear();
                        offset = 0;
                        flags = 0;
                        last_pts = partial.pts_us;
                        return partial;
                    }
                    last_pts = std::max(last_pts + 1, qpc_us());
                    return {last_pts, 2, {}};
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            BYTE* data = nullptr;
            DWORD status = 0;
            UINT64 position = 0, qpc = 0;
            audio_hr(capture->GetBuffer(&data, &frames, &status, &position, &qpc));
            if (frames > cfg.sample_rate / 5 ||
                pending.size() - offset + frames * 4 > cfg.sample_rate * 4 / 5) {
                capture->ReleaseBuffer(frames);
                throw Error(Code::busy, "Audio capture backlog exceeds 200ms");
            }
            if (offset) {
                pending.erase(pending.begin(), pending.begin() + offset);
                offset = 0;
            }
            if (pending.empty())
                pending_pts = (status & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ? qpc_us() : qpc / 10;
            if (status & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                flags |= 1;
            size_t old = pending.size();
            pending.resize(old + frames * 4);
            for (UINT32 i = 0; i < frames; i++)
                for (uint32_t channel = 0; channel < 2; channel++) {
                    int16_t value = (status & AUDCLNT_BUFFERFLAGS_SILENT)
                                        ? 0
                                        : sample(data + i * block +
                                                 std::min(channel, source_channels - 1) * source_bits / 8);
                    pending[old + i * 4 + channel * 2] = uint8_t(value);
                    pending[old + i * 4 + channel * 2 + 1] = uint8_t(uint16_t(value) >> 8);
                }
            audio_hr(capture->ReleaseBuffer(frames));
        }
        AudioFrame frame{std::max(last_pts + 1, pending_pts), flags,
                         Bytes(pending.begin() + offset, pending.begin() + offset + chunk * 4)};
        last_pts = frame.pts_us;
        pending_pts += 10000;
        flags = 0;
        offset += chunk * 4;
        if (offset == pending.size()) {
            pending.clear();
            offset = 0;
        }
        return frame;
    }
};
std::unique_ptr<AudioSource> open_system_audio() {
    return std::make_unique<WasapiSource>();
}
} // namespace lantern
