#include "lantern/windows_playback.hpp"
#include <audioclient.h>
#include <codecapi.h>
#include <cstring>
#include <d3d11.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mmdeviceapi.h>
#include <thread>
#include <wmcodecdsp.h>
#include <wrl/client.h>
#ifdef LANTERN_WITH_OPUS
#include <opus.h>
#endif
namespace lantern {
using Microsoft::WRL::ComPtr;
namespace {
void check(HRESULT hr, const char* operation) {
    if (FAILED(hr))
        throw Error(Code::unsupported, std::string(operation) + " HRESULT=" + std::to_string(uint32_t(hr)));
}
} // namespace
Viewport Viewport::fit(int cw, int ch, int sw, int sh, double zoom) {
    if (cw <= 0 || ch <= 0 || sw <= 0 || sh <= 0)
        return {};
    int w = cw, h = int(int64_t(cw) * sh / sw);
    if (h > ch) {
        h = ch;
        w = int(int64_t(ch) * sw / sh);
    }
    w = int(w * std::clamp(zoom, 1.0, 3.0));
    h = int(h * std::clamp(zoom, 1.0, 3.0));
    return {(cw - w) / 2, (ch - h) / 2, w, h};
}
bool Viewport::map(int x, int y, int32_t& nx, int32_t& ny, bool clamp) const {
    if (width <= 0 || height <= 0)
        return false;
    if (!clamp && (x < left || y < top || x >= left + width || y >= top + height))
        return false;
    nx = int32_t(int64_t(std::clamp(x - left, 0, width - 1)) * 65535 / std::max(1, width - 1));
    ny = int32_t(int64_t(std::clamp(y - top, 0, height - 1)) * 65535 / std::max(1, height - 1));
    return true;
}
struct WindowsVideo::Impl {
    HWND window;
    std::shared_ptr<AvTimeline> timeline;
    std::function<bool()> current;
    VideoConfig cfg;
    DecodeStats counters;
    bool mf = false;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VideoDevice> video;
    ComPtr<ID3D11VideoContext> video_context;
    ComPtr<IMFDXGIDeviceManager> manager;
    ComPtr<IMFTransform> decoder;
    ComPtr<IDXGISwapChain> swap;
    ComPtr<ID3D11VideoProcessorEnumerator> enumerator;
    ComPtr<ID3D11VideoProcessor> processor;
    int swap_w = 0, swap_h = 0;
    UINT decoded_w = 0, decoded_h = 0;
    double zoom = 1;
    ~Impl() {
        if (decoder)
            decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        decoder.Reset();
        manager.Reset();
        if (mf)
            MFShutdown();
    }
    void output_type() {
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> type;
            if (FAILED(decoder->GetOutputAvailableType(0, i, &type)))
                break;
            GUID subtype{};
            type->GetGUID(MF_MT_SUBTYPE, &subtype);
            if (subtype == MFVideoFormat_NV12) {
                check(decoder->SetOutputType(0, type.Get(), 0), "NV12 output");
                MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &decoded_w, &decoded_h);
                return;
            }
        }
        throw Error(Code::unsupported, "H.264 NV12 decoder output unavailable");
    }
    void render(IMFSample* sample, bool visible) {
        ComPtr<IMFMediaBuffer> buffer;
        check(sample->GetBufferByIndex(0, &buffer), "Decoded buffer");
        ComPtr<IMFDXGIBuffer> dxgi;
        check(buffer.As(&dxgi), "Decoder did not return a GPU surface");
        ComPtr<ID3D11Texture2D> texture;
        UINT slice = 0;
        check(dxgi->GetResource(IID_PPV_ARGS(&texture)), "Decoded texture");
        check(dxgi->GetSubresourceIndex(&slice), "Decoded slice");
        ++counters.decoded;
        ++counters.gpu_surfaces;
        if (!visible || IsIconic(GetAncestor(window, GA_ROOT)))
            return;
        RECT r{};
        GetClientRect(window, &r);
        int w = r.right, h = r.bottom;
        if (w <= 0 || h <= 0)
            return;
        if (!swap) {
            ComPtr<IDXGIDevice> gd;
            check(device.As(&gd), "DXGI device");
            ComPtr<IDXGIAdapter> adapter;
            gd->GetAdapter(&adapter);
            ComPtr<IDXGIFactory> factory;
            check(adapter->GetParent(IID_PPV_ARGS(&factory)), "DXGI factory");
            DXGI_SWAP_CHAIN_DESC desc{};
            desc.BufferDesc.Width = w;
            desc.BufferDesc.Height = h;
            desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2;
            desc.OutputWindow = window;
            desc.Windowed = TRUE;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            check(factory->CreateSwapChain(device.Get(), &desc, &swap), "Desktop swap chain");
            factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
            swap_w = w;
            swap_h = h;
        }
        if (w != swap_w || h != swap_h) {
            check(swap->ResizeBuffers(2, w, h, DXGI_FORMAT_UNKNOWN, 0), "Resize desktop");
            swap_w = w;
            swap_h = h;
            processor.Reset();
            enumerator.Reset();
        }
        if (!processor) {
            D3D11_TEXTURE2D_DESC td{};
            texture->GetDesc(&td);
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC content{};
            content.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            content.InputFrameRate = {cfg.fps, 1};
            content.OutputFrameRate = {cfg.fps, 1};
            content.InputWidth = td.Width;
            content.InputHeight = td.Height;
            content.OutputWidth = w;
            content.OutputHeight = h;
            content.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
            check(video->CreateVideoProcessorEnumerator(&content, &enumerator),
                  "Video processor enumeration");
            check(video->CreateVideoProcessor(enumerator.Get(), 0, &processor), "Video processor");
        }
        ComPtr<ID3D11Texture2D> target;
        check(swap->GetBuffer(0, IID_PPV_ARGS(&target)), "Swap target");
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC input{};
        input.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        input.Texture2D.ArraySlice = slice;
        ComPtr<ID3D11VideoProcessorInputView> iv;
        check(video->CreateVideoProcessorInputView(texture.Get(), enumerator.Get(), &input, &iv),
              "GPU input view");
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC output{};
        output.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11VideoProcessorOutputView> ov;
        check(video->CreateVideoProcessorOutputView(target.Get(), enumerator.Get(), &output, &ov),
              "GPU output view");
        auto fit = Viewport::fit(w, h, int(cfg.width), int(cfg.height), zoom);
        RECT dst{fit.left, fit.top, fit.left + fit.width, fit.top + fit.height},
            src{0, 0, LONG(cfg.width), LONG(cfg.height)};
        // VideoProcessorBlt requires a destination inside its output surface.
        // Crop the source for zoom instead of passing a negative destination rectangle.
        RECT clipped{std::max(0L, dst.left), std::max(0L, dst.top), std::min(LONG(w), dst.right),
                     std::min(LONG(h), dst.bottom)};
        src.left = LONG(int64_t(clipped.left - dst.left) * cfg.width / fit.width);
        src.top = LONG(int64_t(clipped.top - dst.top) * cfg.height / fit.height);
        src.right = LONG(int64_t(clipped.right - dst.left) * cfg.width / fit.width);
        src.bottom = LONG(int64_t(clipped.bottom - dst.top) * cfg.height / fit.height);
        dst = clipped;
        D3D11_VIDEO_COLOR black{};
        black.RGBA.A = 1;
        video_context->VideoProcessorSetOutputBackgroundColor(processor.Get(), FALSE, &black);
        video_context->VideoProcessorSetStreamSourceRect(processor.Get(), 0, TRUE, &src);
        video_context->VideoProcessorSetStreamDestRect(processor.Get(), 0, TRUE, &dst);
        video_context->VideoProcessorSetOutputTargetRect(processor.Get(), TRUE, &r);
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE cs{};
        cs.YCbCr_Matrix = 1; // NVENC emits BT.709 limited range.
        video_context->VideoProcessorSetStreamColorSpace(processor.Get(), 0, &cs);
        D3D11_VIDEO_PROCESSOR_STREAM stream{};
        stream.Enable = TRUE;
        stream.pInputSurface = iv.Get();
        check(video_context->VideoProcessorBlt(processor.Get(), ov.Get(), 0, 1, &stream), "GPU NV12 render");
        LONGLONG timestamp = 0;
        if (timeline && SUCCEEDED(sample->GetSampleTime(&timestamp))) {
            auto at = timeline->deadline(uint64_t(timestamp / 10), monotonic_us() * 1000);
            if (at < 0)
                return;
            counters.scheduled_wait_us+=uint64_t(std::max(int64_t(0),at-int64_t(monotonic_us()*1000)))/1000;
            while (int64_t(monotonic_us() * 1000) < at) {
                if (current && !current())
                    return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (current && !current())
            return;
        check(swap->Present(0, 0), "Desktop present");
        if (timeline && timestamp > 0)
            timeline->observe(uint64_t(timestamp / 10), monotonic_us() * 1000);
        ++counters.presented;
    }
    void drain(bool visible) {
        for (int i = 0; i < 16; ++i) {
            MFT_OUTPUT_DATA_BUFFER output{};
            DWORD flags = 0;
            HRESULT hr = decoder->ProcessOutput(0, 1, &output, &flags);
            ComPtr<IMFSample> sample;
            sample.Attach(output.pSample);
            if (output.pEvents)
                output.pEvents->Release();
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
                return;
            if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
                output_type();
                processor.Reset();
                enumerator.Reset();
                continue;
            }
            check(hr, "H.264 decode output");
            if (sample)
                render(sample.Get(), visible);
        }
        throw Error(Code::invalid, "Decoder output exceeded bounded drain");
    }
};
WindowsVideo::WindowsVideo(HWND h, const VideoConfig& cfg, std::shared_ptr<AvTimeline> timeline,
                           std::function<bool()> current)
    : p_(std::make_unique<Impl>()) {
    if (!cfg.width || !cfg.height || cfg.width > 4096 || cfg.height > 4096 || !cfg.fps || cfg.fps > 120 ||
        cfg.headers.size() > 65536)
        throw Error(Code::invalid, "Video configuration bound");
    auto& p = *p_;
    p.window = h;
    p.timeline = std::move(timeline);
    p.current = std::move(current);
    p.cfg = cfg;
    check(MFStartup(MF_VERSION), "Media Foundation startup");
    p.mf = true;
    check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                            D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                            D3D11_SDK_VERSION, &p.device, nullptr, &p.context),
          "D3D11 hardware device");
    ComPtr<ID3D10Multithread> threaded;
    check(p.device.As(&threaded), "D3D multithread");
    threaded->SetMultithreadProtected(TRUE);
    check(p.device.As(&p.video), "D3D11 video device");
    check(p.context.As(&p.video_context), "D3D11 video context");
    UINT token = 0;
    check(MFCreateDXGIDeviceManager(&token, &p.manager), "DXGI manager");
    check(p.manager->ResetDevice(p.device.Get(), token), "DXGI manager device");
    check(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p.decoder)),
          "Windows H.264 decoder");
    ComPtr<IMFAttributes> attrs;
    check(p.decoder->GetAttributes(&attrs), "Decoder attributes");
    if (!MFGetAttributeUINT32(attrs.Get(), MF_SA_D3D11_AWARE, 0))
        throw Error(Code::unsupported, "H.264 D3D11 decode unavailable");
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    check(
        p.decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(p.manager.Get())),
        "Decoder GPU manager");
    ComPtr<IMFMediaType> input;
    check(MFCreateMediaType(&input), "H.264 type");
    input->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    input->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(input.Get(), MF_MT_FRAME_SIZE, cfg.width, cfg.height);
    MFSetAttributeRatio(input.Get(), MF_MT_FRAME_RATE, cfg.fps, 1);
    input->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    input->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
    check(p.decoder->SetInputType(0, input.Get(), 0), "H.264 input");
    p.output_type();
    check(p.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0), "Decoder begin");
    check(p.decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0), "Decoder start");
}
WindowsVideo::~WindowsVideo() = default;
void WindowsVideo::frame(std::span<const uint8_t> b, uint64_t pts, bool visible, double zoom) {
    p_->zoom = zoom;
    if (b.empty() || b.size() > max_frame)
        throw Error(Code::invalid, "H.264 packet bound");
    ComPtr<IMFSample> sample;
    check(MFCreateSample(&sample), "Compressed sample");
    ComPtr<IMFMediaBuffer> buffer;
    check(MFCreateMemoryBuffer(DWORD(b.size()), &buffer), "Compressed buffer");
    BYTE* data = nullptr;
    check(buffer->Lock(&data, nullptr, nullptr), "Compressed lock");
    std::memcpy(data, b.data(), b.size());
    buffer->Unlock();
    buffer->SetCurrentLength(DWORD(b.size()));
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(LONGLONG(pts) * 10);
    sample->SetSampleDuration(10'000'000 / p_->cfg.fps);
    auto hr = p_->decoder->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        p_->drain(visible);
        hr = p_->decoder->ProcessInput(0, sample.Get(), 0);
    }
    check(hr, "H.264 decode input");
    p_->drain(visible);
}
DecodeStats WindowsVideo::stats() const {
    return p_->counters;
}
std::string WindowsVideo::provider() const {
    return "Windows-MF-H264-D3D11/DXVA/GPU-video-processor";
}
struct WindowsAudio::Impl {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<IAudioClock> clock;
    uint64_t frequency = 0, host_base = 0, owner = 0;
    std::shared_ptr<AvTimeline> timeline;
    RenderStats counters;
    UINT32 capacity = 0, rate = 0;
    uint32_t codec = 0, skip = 0;
    bool started = false;
#ifdef LANTERN_WITH_OPUS
    OpusDecoder* opus = nullptr;
#endif
    ~Impl() {
        if (timeline)
            timeline->end_audio(owner);
        if (started)
            client->Stop();
#ifdef LANTERN_WITH_OPUS
        if (opus)
            opus_decoder_destroy(opus);
#endif
    }
};
bool WindowsAudio::supports_opus() {
#ifdef LANTERN_WITH_OPUS
    return true;
#else
    return false;
#endif
}
WindowsAudio::WindowsAudio(uint32_t rate, uint32_t codec, uint32_t lookahead,
                           std::shared_ptr<AvTimeline> timeline)
    : p_(std::make_unique<Impl>()) {
    if (rate < 8000 || rate > 96000 || rate % 100 || codec > 1 || lookahead > 5760)
        throw Error(Code::invalid, "Audio configuration bound");
    auto& p = *p_;
    p.rate = rate;
    p.timeline = std::move(timeline);
    if (p.timeline)
        p.owner = p.timeline->begin_audio();
    p.codec = codec;
    p.skip = lookahead;
#ifdef LANTERN_WITH_OPUS
    if (codec) {
        int error = 0;
        p.opus = opus_decoder_create(int(rate), 2, &error);
        if (!p.opus || error != OPUS_OK)
            throw Error(Code::unsupported, "Opus decoder unavailable");
    }
#else
    if (codec)
        throw Error(Code::unsupported, "Build does not include Opus");
#endif
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> endpoint;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)),
          "Audio enumerator");
    check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint), "Audio output");
    check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &p.client), "WASAPI output");
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = rate * 4;
    check(p.client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                               1000000, 0, &format, nullptr),
          "WASAPI initialize");
    check(p.client->GetBufferSize(&p.capacity), "Audio buffer");
    check(p.client->GetService(IID_PPV_ARGS(&p.render)), "Audio render service");
    check(p.client->GetService(IID_PPV_ARGS(&p.clock)), "Audio clock service");
    check(p.clock->GetFrequency(&p.frequency), "Audio clock frequency");
    if (!p.frequency)
        throw Error(Code::unsupported, "Audio clock frequency zero");
}
WindowsAudio::~WindowsAudio() = default;
void WindowsAudio::packet(std::span<const uint8_t> b, uint32_t flags, Cancel& cancel, uint64_t host_pts) {
    auto& p = *p_;
    if (b.empty()) {
        if (p.timeline && p.host_base) {
            if (p.started) {
                p.client->Stop();
                p.client->Reset();
                p.started = false;
            }
            p.host_base = 0;
            p.timeline->end_audio(p.owner);
            p.owner = p.timeline->begin_audio();
        }
        return;
    }
    std::vector<int16_t> pcm;
    uint32_t frames = 0;
    if (p.codec) {
#ifdef LANTERN_WITH_OPUS
        if (b.size() > 4096)
            throw Error(Code::invalid, "Opus packet bound");
        pcm.resize(5760 * 2);
        int got = opus_decode(p.opus, b.data(), opus_int32(b.size()), pcm.data(), 5760, 0);
        if (got < 0)
            throw Error(Code::invalid, "Invalid Opus data");
        frames = uint32_t(got);
#endif
    } else {
        if (b.size() % 4 || b.size() > p.rate * 4 / 5)
            throw Error(Code::invalid, "PCM packet bound");
        frames = uint32_t(b.size() / 4);
        pcm.resize(frames * 2);
        std::memcpy(pcm.data(), b.data(), b.size());
    }
    uint32_t offset = std::min(p.skip, frames);
    p.skip -= offset;
    auto skipped = offset;
    if (flags & 1) {
        if (p.started) {
            p.client->Stop();
            p.client->Reset();
            p.started = false;
            p.host_base = 0;
            if (p.timeline) {
                p.timeline->end_audio(p.owner);
                p.owner = p.timeline->begin_audio();
            }
            ++p.counters.resets;
        }
    }
    while (offset < frames) {
        cancel.check();
        UINT32 padding = 0;
        check(p.client->GetCurrentPadding(&padding), "Audio padding");
        // Keep at most ~100 ms in the endpoint; drop stale queued audio when latency exceeds 80 ms.
        if (padding > p.rate * 80 / 1000) {
            p.client->Stop();
            p.client->Reset();
            p.started = false;
            padding = 0;
            p.host_base = 0;
            if (p.timeline) {
                p.timeline->end_audio(p.owner);
                p.owner = p.timeline->begin_audio();
            }
            ++p.counters.overruns;
        }
        auto n = std::min(frames - offset, p.capacity - padding);
        if (!n) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (!p.host_base && host_pts)
            p.host_base = host_pts + (offset ? uint64_t(offset - skipped) * 1'000'000 / p.rate : 0);
        BYTE* data = nullptr;
        check(p.render->GetBuffer(n, &data), "Audio write buffer");
        std::memcpy(data, pcm.data() + offset * 2, n * 4);
        check(p.render->ReleaseBuffer(n, 0), "Audio commit");
        offset += n;
        p.counters.frames += n;
        if (!p.started) {
            check(p.client->Start(), "Audio start");
            p.started = true;
        }
        p.counters.queued_frames = padding + n;
        UINT64 position = 0, qpc = 0;
        if (p.timeline && p.host_base && SUCCEEDED(p.clock->GetPosition(&position, &qpc)))
            p.timeline->anchor(p.owner,
                               p.host_base + position / p.frequency * 1'000'000 +
                                   (position % p.frequency) * 1'000'000 / p.frequency,
                               qpc * 100);
    }
}
RenderStats WindowsAudio::stats() const {
    return p_->counters;
}
} // namespace lantern
