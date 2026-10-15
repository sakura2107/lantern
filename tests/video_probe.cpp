#include "lantern/win.hpp"
#include <fstream>
#include <iostream>
#include <thread>
using namespace lantern;
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 3)
            throw Error(Code::invalid, "video-probe OUTPUT.h264 REPORT.json");
        auto stream = open_desktop_stream(0, 1920, 60, 12'000'000);
        std::ofstream output(fs::path(argv[1]), std::ios::binary);
        if (!output)
            throw Error(Code::io, "Probe output unavailable");
        const auto& config = stream->config();
        output.write((const char*)config.headers.data(), config.headers.size());
        uint64_t bytes = 0, fresh = 0;
        double capture = 0, encode = 0;
        auto began = std::chrono::steady_clock::now();
        for (int i = 0; i < 120; i++) {
            auto deadline = began + std::chrono::microseconds((i + 1) * 1'000'000ll / config.fps);
            auto frame = stream->next();
            bytes += frame.annex_b.size();
            fresh += !(frame.flags & 2);
            capture += frame.capture_ms;
            encode += frame.encode_ms;
            output.write((const char*)frame.annex_b.data(), frame.annex_b.size());
            std::this_thread::sleep_until(deadline);
        }
        double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
        std::ofstream report{fs::path(argv[2])};
        report << "{\"kind\":\"measured\",\"hardware_encode\":true,\"capture\":\"" << config.capture_provider
               << "\",\"encoder\":\"" << config.encoder_provider << "\",\"width\":" << config.width
               << ",\"height\":" << config.height
               << ",\"frames\":120,\"fresh_desktop_acquisitions\":" << fresh << ",\"seconds\":" << seconds
               << ",\"encoded_fps\":" << 120 / seconds << ",\"annex_b_bytes\":" << bytes
               << ",\"capture_gpu_scale_mean_ms\":" << capture / 120
               << ",\"encode_bitstream_mean_ms\":" << encode / 120 << ",\"optical_latency_measured\":false}";
        std::cout << "Hardware probe complete, " << config.width << "x" << config.height << ", "
                  << 120 / seconds << " encoded fps\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
