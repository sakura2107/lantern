#include "lantern/win.hpp"
#include <fstream>
#include <iomanip>
#include <iostream>
#include <psapi.h>
#include <thread>
using namespace lantern;
static double seconds(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "benchmark WORK-DIRECTORY REPORT.json\n";
        return 2;
    }
    try {
        auto work = fs::absolute(argv[1]);
        fs::create_directories(work);
        auto root = work / L"share", state = work / L"state", local = work / L"received";
        fs::create_directories(root);
        fs::create_directories(state);
        fs::create_directories(local);
        Bytes block(1024 * 1024);
        for (size_t i = 0; i < block.size(); ++i)
            block[i] = uint8_t((i * 179 + i / 313) % 256);
        {
            std::ofstream f(root / L"large.bin", std::ios::binary);
            for (int i = 0; i < 64; ++i)
                f.write((char*)block.data(), block.size());
        }
        std::string pin;
        {
            Identity id(state / L"identity.dpapi", true);
            pin = id.fingerprint();
        }
        auto token = random_token();
        save_config(state / L"config.dpapi", {root, token, 15});
        Host host(state, "127.0.0.1", 47450);
        std::thread thread([&] { host.run(); });
        struct Stop {
            Host& h;
            std::thread& t;
            ~Stop() {
                h.stop();
                t.join();
            }
        } stop{host, thread};
        auto t = std::chrono::steady_clock::now();
        Client files("127.0.0.1", 47450, pin, token, files_read | files_write);
        double connect_ms = seconds(t) * 1000;
        Cancel cancel;
        auto destination = local / (L"large-" + wide(random_token().substr(0, 8)) + L".bin");
        t = std::chrono::steady_clock::now();
        files.download("large.bin", destination, cancel);
        double download_s = seconds(t);
        auto uploadname = "upload-" + random_token().substr(0, 8) + ".bin";
        t = std::chrono::steady_clock::now();
        files.upload(destination, uploadname, cancel);
        double upload_s = seconds(t);
        t = std::chrono::steady_clock::now();
        auto hash = hash_file(destination);
        double hash_s = seconds(t);
        bool verified = hash == hash_file(root / L"large.bin") && hash == hash_file(root / wide(uploadname));
        auto small = root / L"small";
        fs::create_directories(small);
        Bytes tiny(4096, 0x69);
        for (int i = 0; i < 100; ++i)
            write_file(small / (std::to_wstring(i) + L".bin"), tiny);
        auto small_out = local / (L"small-" + wide(random_token().substr(0, 8)));
        fs::create_directories(small_out);
        t = std::chrono::steady_clock::now();
        for (int i = 0; i < 100; ++i)
            files.download("small/" + std::to_string(i) + ".bin", small_out / (std::to_wstring(i) + L".bin"),
                           cancel);
        double small_s = seconds(t);
        auto partial = local / (L"resume-" + wide(random_token().substr(0, 8)) + L".bin");
        Cancel pause;
        try {
            files.download("large.bin", partial, pause, [&](uint64_t n, uint64_t) {
                if (n >= 8 * 1024 * 1024)
                    pause.requested = true;
            });
        } catch (const Error& e) {
            if (e.code != Code::cancelled)
                throw;
        }
        pause.requested = false;
        uint64_t resumed = 0;
        t = std::chrono::steady_clock::now();
        {
            Client reconnect("127.0.0.1", 47450, pin, token);
            reconnect.download("large.bin", partial, pause, [&](uint64_t n, uint64_t) {
                if (!resumed)
                    resumed = n;
            });
        }
        double resume_s = seconds(t);
        Client desktop("127.0.0.1", 47450, pin, token, view);
        std::vector<double> latency;
        uint64_t jpeg_bytes = 0;
        std::string desktop_error;
        auto capture_start = std::chrono::steady_clock::now();
        try {
            for (int i = 0; i < 60; ++i) {
                t = std::chrono::steady_clock::now();
                Writer w;
                w.u32(0);
                w.u32(1920);
                w.u32(70);
                auto b = desktop.call(Op::frame, std::move(w));
                Reader r{b};
                auto jpg = r.blob();
                jpeg_bytes += jpg.size();
                latency.push_back(seconds(t) * 1000);
                if (i == 0)
                    write_file(work / L"actual-desktop.jpg", jpg);
            }
        } catch (const std::exception& e) {
            desktop_error = e.what();
        }
        double capture_s = seconds(capture_start);
        std::sort(latency.begin(), latency.end());
        std::atomic_bool transfer_done = false;
        std::exception_ptr transfer_error;
        auto parallel_out = local / (L"parallel-" + wide(random_token().substr(0, 8)) + L".bin");
        auto parallel_start = std::chrono::steady_clock::now();
        std::thread bulk([&] {
            try {
                Client peer("127.0.0.1", 47450, pin, token);
                Cancel c;
                peer.download("large.bin", parallel_out, c);
            } catch (...) {
                transfer_error = std::current_exception();
            }
            transfer_done = true;
        });
        std::vector<double> parallel_latency;
        try {
            while (!transfer_done && desktop_error.empty()) {
                auto frame_start = std::chrono::steady_clock::now();
                Writer w;
                w.u32(0);
                w.u32(1920);
                w.u32(70);
                desktop.call(Op::frame, std::move(w));
                parallel_latency.push_back(seconds(frame_start) * 1000);
            }
        } catch (...) {
            bulk.join();
            throw;
        }
        bulk.join();
        if (transfer_error)
            std::rethrow_exception(transfer_error);
        double parallel_s = seconds(parallel_start);
        std::sort(parallel_latency.begin(), parallel_latency.end());
        PROCESS_MEMORY_COUNTERS memory{};
        GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory));
        std::ofstream report(argv[2]);
        report
            << std::fixed << std::setprecision(3)
            << "{\n  \"kind\": \"measured\",\n  \"network\": \"Windows loopback, same machine, shared budget "
               "40 MiB/s bulk share 25 percent\",\n  \"file_bytes\": 67108864,\n  \"connect_tls_auth_ms\": "
            << connect_ms << ",\n  \"download_with_hash_flush_commit_s\": " << download_s
            << ",\n  \"download_MiB_per_s\": " << 64 / download_s
            << ",\n  \"upload_with_hash_flush_commit_s\": " << upload_s
            << ",\n  \"upload_MiB_per_s\": " << 64 / upload_s << ",\n  \"standalone_SHA256_s\": " << hash_s
            << ",\n  \"both_directions_verified\": " << (verified ? "true" : "false")
            << ",\n  \"small_100_x_4KiB_download_s\": " << small_s
            << ",\n  \"resume_offset_bytes\": " << resumed
            << ",\n  \"reconnect_plus_resume_remaining_56MiB_s\": " << resume_s
            << ",\n  \"desktop_frames\": " << latency.size()
            << ",\n  \"desktop_request_capture_encode_tls_fps\": "
            << (capture_s ? latency.size() / capture_s : 0)
            << ",\n  \"desktop_request_ms_p50\": " << (latency.empty() ? 0 : latency[latency.size() / 2])
            << ",\n  \"desktop_request_ms_p95\": "
            << (latency.empty() ? 0 : latency[size_t((latency.size() - 1) * .95)])
            << ",\n  \"desktop_total_jpeg_bytes\": " << jpeg_bytes
            << ",\n  \"desktop_available\": " << (desktop_error.empty() ? "true" : "false")
            << ",\n  \"parallel_64MiB_download_s\": " << parallel_s
            << ",\n  \"parallel_desktop_frames\": " << parallel_latency.size()
            << ",\n  \"parallel_desktop_request_fps\": " << parallel_latency.size() / parallel_s
            << ",\n  \"parallel_desktop_request_ms_p95\": "
            << (parallel_latency.empty() ? 0 : parallel_latency[size_t((parallel_latency.size() - 1) * .95)])
            << ",\n  \"benchmark_combined_peak_working_set_bytes\": " << memory.PeakWorkingSetSize
            << ",\n  \"unmeasured\": [\"desktop presentation latency\",\"audio\",\"GPU "
               "utilization\",\"Android thermal\",\"4K decode/playback\",\"physical LAN\"]\n}\n";
        if (!report)
            throw Error(Code::io, "Write benchmark report");
        std::cout << "Measured transfer and " << latency.size() << " real desktop frames. "
                  << (desktop_error.empty() ? "" : desktop_error) << "\n";
        return verified ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
}
