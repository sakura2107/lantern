#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mmsystem.h>
#include <vector>
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4)
        return 1;
    for (int i = 0; i < 1800 && !std::filesystem::exists(argv[1]); i++)
        Sleep(50);
    if (!std::filesystem::exists(argv[1]))
        return 2;
    constexpr int rate = 48000;
    int seconds = argc == 4 ? _wtoi(argv[3]) : 15;
    if (seconds < 5 || seconds > 60)
        return 1;
    std::vector<short> pcm(rate * seconds * 2);
    for (int i = 0; i < rate * seconds; i++) {
        double fade = std::min(1.0, std::min(i / 480.0, (rate * seconds - i) / 480.0));
        short value = short(std::sin(i * 6.283185307179586 * 997 / rate) * 32767 * 0.04 * fade);
        pcm[i * 2] = pcm[i * 2 + 1] = value;
    }
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = rate * 4;
    HWAVEOUT output = nullptr;
    if (waveOutOpen(&output, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR)
        return 3;
    WAVEHDR buffer{};
    buffer.lpData = (LPSTR)pcm.data();
    buffer.dwBufferLength = DWORD(pcm.size() * 2);
    if (waveOutPrepareHeader(output, &buffer, sizeof(buffer)) != MMSYSERR_NOERROR ||
        waveOutWrite(output, &buffer, sizeof(buffer)) != MMSYSERR_NOERROR) {
        waveOutClose(output);
        return 4;
    }
    {
        std::ofstream report{std::filesystem::path(argv[2])};
        report << "{\"generated_tone\":true,\"frequency_hz\":997,\"source_rate\":48000,\"source_channels\":2,"
                  "\"amplitude\":0.04,\"duration_s\":"
               << seconds << ",\"waveOutWrite_accepted\":true}";
    }
    for (int i = 0; i < (seconds + 5) * 20 && !(buffer.dwFlags & WHDR_DONE); i++)
        Sleep(50);
    waveOutReset(output);
    waveOutUnprepareHeader(output, &buffer, sizeof(buffer));
    waveOutClose(output);
    return 0;
}
