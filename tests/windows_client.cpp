#include "lantern/windows_client.hpp"
#include "lantern/windows_media.hpp"
#include "lantern/windows_playback.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <winioctl.h>
using namespace lantern;
static int checks = 0;
static void check(bool b, const char* description) {
    ++checks;
    if (!b)
        throw std::runtime_error(description);
}
template <class F> void expect(Code code, F f, const char* s) {
    bool b = false;
    try {
        f();
    } catch (const Error& e) {
        b = e.code == code;
    }
    check(b, s);
}
static void pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
static Bytes http(const std::string& url, const std::string& request) {
    auto slash = url.find('/', 7), colon = url.find(':', 7);
    SOCKET s = connect_socket("127.0.0.1", uint16_t(std::stoul(url.substr(colon + 1, slash - colon - 1))));
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Headers need not arrive at accept().
    std::string target = url.substr(slash);
    auto wire = request + " " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n";
    auto sep = request.find('|');
    if (sep != std::string::npos)
        wire = request.substr(0, sep) + " " + target +
               " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n" + request.substr(sep + 1) + "\r\n";
    wire += "\r\n";
    send(s, wire.data(), int(wire.size()), 0);
    Bytes out;
    uint8_t b[4096];
    for (;;) {
        int n = recv(s, (char*)b, sizeof(b), 0);
        if (n <= 0)
            break;
        out.insert(out.end(), b, b + n);
    }
    closesocket(s);
    return out;
}
template <class F> void until(F predicate, const char* reason, int ms = 15000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!predicate()) {
        pump();
        if (std::chrono::steady_clock::now() > end)
            throw std::runtime_error(reason);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
struct RunningHost {
    std::unique_ptr<Host> host;
    std::thread thread;
    fs::path state;
    uint16_t port;
    RunningHost(fs::path s, uint16_t p) : state(s), port(p) {
        start();
    }
    ~RunningHost() {
        stop();
    }
    void start() {
        host = std::make_unique<Host>(state, "127.0.0.1", port);
        thread = std::thread([this] { host->run(); });
    }
    void stop() {
        if (host) {
            host->stop();
            if (thread.joinable())
                thread.join();
            host.reset();
        }
    }
};
static LRESULT CALLBACK canvas(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT p;
        auto dc = BeginPaint(h, &p);
        RECT r;
        GetClientRect(h, &r);
        auto brush = CreateSolidBrush(RGB(22, 65, 125));
        FillRect(dc, &r, brush);
        DeleteObject(brush);
        SetTextColor(dc, RGB(250, 250, 250));
        SetBkMode(dc, TRANSPARENT);
        TextOutW(dc, 40, 40, L"Lantern generated Windows decode fixture", 39);
        EndPaint(h, &p);
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2)
        return 2;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        auto root = fs::absolute(argv[1]) / wide(random_token().substr(0, 16));
        auto share = root / L"share", state = root / L"host", client_state = root / L"client";
        fs::create_directories(share);
        fs::create_directories(state);
        fs::create_directories(client_state);
        Identity id(state / L"identity.dpapi", true);
        Config cfg;
        cfg.root = share;
        cfg.token = random_token();
        cfg.scopes = 31;
        save_config(state / L"config.dpapi", cfg);
        ClientCredentials c{"127.0.0.1", id.fingerprint(), cfg.token, 47445};
        save_credentials(client_state / L"device.dpapi", c);
        auto stored = load_credentials(client_state / L"device.dpapi");
        check(stored.token == c.token && stored.port == 47445, "DPAPI credentials round trip");
        auto cipher = read_file(client_state / L"device.dpapi");
        check(std::search(cipher.begin(), cipher.end(), c.token.begin(), c.token.end()) == cipher.end(),
              "credential ciphertext has no plaintext secret");
        Bytes data(16 * 1024 * 1024);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = uint8_t((i * 33 + (i >> 15)) % 251);
        write_file(share / L"large.bin", data);
        auto expected = sha256(data);
        write_file(share / L"note.txt", Bytes{'t', 'e', 's', 't'});
        fs::create_directories(share / L"nested");
        write_file(share / L"nested" / L"inside.txt", Bytes{'a'});
        RunningHost host(state, c.port);
        bool media_only = argc > 2 && std::wstring(argv[2]) == L"--media-only";
        if (!media_only) {
            auto cli = c.connect(files_read | files_write, 0);
            auto pg = client_page(*cli, "", 0, "", 0, false, "");
            check(pg.total == 3 && pg.entries.size() == 3, "independent generation paged listing");
            check(client_page(*cli, "", 0, ".txt", 0, false, "").entries.size() == 1, "host name filter");
            cli->mutate(Op::mkdir, "new-dir");
            expect(
                Code::conflict, [&] { client_page(*cli, "", 0, "", 0, false, pg.generation); },
                "directory changes invalidate paging generation");
            auto search = client_search(*cli, 0, "", "inside");
            std::vector<Entry> found = search.entries;
            while (!search.done) {
                search = client_search(*cli, 1);
                found.insert(found.end(), search.entries.begin(), search.entries.end());
            }
            check(found.size() == 1 && found[0].name == "nested/inside.txt", "incremental nested search");
            client_search(*cli, 0, "", "note");
            check(client_search(*cli, 2).done, "search cancellation");
            expect(
                Code::unauthorized, [&] { client_page(*cli, "../", 0, "", 0, false, ""); },
                "client cannot escape host root");
            auto job = client_job(*cli, 0, "nested", "copied");
            FileJobInfo info;
            until(
                [&] {
                    info = client_job_poll(*cli, job);
                    return uint32_t(info.state) >= 4;
                },
                "host copy timeout");
            check(info.state == TaskState::completed && fs::exists(share / L"copied/inside.txt"),
                  "directory copies on host without routing content through client");
            job = client_job(*cli, 1, "copied", "");
            until([&] { return uint32_t(client_job_poll(*cli, job).state) >= 4; }, "host delete timeout");
            check(!fs::exists(share / L"copied"), "host directory delete");
            auto fit = Viewport::fit(1000, 700, 1920, 1080);
            int32_t nx = 0, ny = 0;
            check(fit.height == 562 && fit.top == 69, "letterboxed viewport");
            check(!fit.map(500, 20, nx, ny), "letterbox rejects pointer");
            check(fit.map(fit.left, fit.top, nx, ny) && nx == 0 && ny == 0, "top left exact coordinates");
            check(fit.map(fit.left + fit.width - 1, fit.top + fit.height - 1, nx, ny) && nx == 65535 &&
                      ny == 65535,
                  "bottom right exact coordinates");
            auto z = Viewport::fit(1000, 700, 1920, 1080, 2);
            check(z.map(500, 350, nx, ny) && std::abs(nx - 32767) < 80 && std::abs(ny - 32767) < 80,
                  "zoom input shares render viewport");
            check(!Viewport{}.map(0, 0, nx, ny), "zero viewport safe");
            auto journal = client_state / L"transfers.dpapi", dest = root / L"download.bin";
            std::string task;
            {
                WindowsTransfers queue(journal, [] {}, [](auto&, bool) {}, [] { return true; });
                task = queue.add(c, "large.bin", dest, false);
                until(
                    [&] {
                        auto t = queue.snapshot();
                        return t[0].current >= 2 * 1024 * 1024;
                    },
                    "download progress");
                queue.pause(task);
                until([&] { return fs::exists(fs::path(dest.wstring() + L".lantern-part")); },
                      "partial file");
                queue.stop();
                check(queue.snapshot()[0].state == TaskState::paused, "manual pause keeps paused state");
                check(fs::file_size(fs::path(dest.wstring() + L".lantern-part")) >= 2 * 1024 * 1024,
                      "download partial survives interruption");
            }
            {
                WindowsTransfers queue(journal, [] {}, [](auto&, bool) {}, [] { return false; });
                auto t = queue.snapshot();
                check(t.size() == 1 && t[0].state == TaskState::paused,
                      "journal reload pauses incomplete task");
                queue.resume(task);
                until([&] { return queue.snapshot()[0].state == TaskState::completed; },
                      "resumed download completion");
                check(hash_file(dest) == expected, "resumed download full SHA-256");
                auto upload = queue.add(c, "upload.bin", dest, true);
                until(
                    [&] {
                        auto v = queue.snapshot();
                        return v[1].state == TaskState::completed;
                    },
                    "upload complete");
                check(hash_file(share / L"upload.bin") == expected, "upload committed hash");
                auto conflict = queue.add(c, "note.txt", dest, true);
                until([&] { return queue.snapshot()[2].state == TaskState::failed; }, "conflict detected");
                check(read_file(share / L"note.txt") == Bytes({'t', 'e', 's', 't'}),
                      "same name conflict preserves original");
                queue.rename_destination(conflict, "renamed-upload.bin", dest);
                queue.resume(conflict);
                until([&] { return queue.snapshot()[2].state == TaskState::completed; },
                      "renamed upload complete");
                check(hash_file(share / L"renamed-upload.bin") == expected, "conflict new destination");
                auto network_dest = root / L"network.bin";
                auto n = queue.add(c, "large.bin", network_dest, false);
                until([&] { return queue.snapshot().back().current > 0; }, "network transfer started");
                host.stop();
                std::this_thread::sleep_for(std::chrono::milliseconds(550));
                host.start();
                until([&] { return queue.snapshot().back().state == TaskState::completed; },
                      "automatic network retry", 30000);
                check(hash_file(network_dest) == expected, "host restart automatically resumes download");
                ClientCredentials denied = c;
                denied.token = random_token();
                queue.add(denied, "note.txt", root / L"denied.txt", false);
                until([&] { return queue.snapshot().back().state == TaskState::failed; },
                      "bad identity rejected");
                check(!fs::exists(root / L"denied.txt"), "bad identity never commits file");
                auto tree = queue.add(c, "nested", root / L"tree-download", false, false, false, true);
                until(
                    [&] {
                        auto v = queue.snapshot();
                        return v.back().state == TaskState::completed || v.back().state == TaskState::failed;
                    },
                    "tree download");
                check(fs::exists(root / L"tree-download/inside.txt"),
                      "directory download commits complete staged tree");
                auto up = queue.add(c, "tree-upload", root / L"tree-download", true, false, false, true);
                until(
                    [&] {
                        auto v = queue.snapshot();
                        return v.back().state == TaskState::completed || v.back().state == TaskState::failed;
                    },
                    "tree upload");
                check(read_file(share / L"tree-upload/inside.txt") == Bytes({'a'}),
                      "directory upload full verified files");
                queue.add(c, "nested", root / L"tree-conflict", true, false, false, true);
                until([&] { return queue.snapshot().back().state == TaskState::failed; }, "tree conflict");
                check(fs::exists(share / L"nested/inside.txt"), "directory conflict preserves original");
                fs::create_directories(share / L"resumable-tree");
                write_file(share / L"resumable-tree/large.bin", data);
                auto tree_dest = root / L"resumed-tree";
                auto tree_id = queue.add(c, "resumable-tree", tree_dest, false, false, false, true);
                until([&] { return queue.snapshot().back().current >= 2 * 1024 * 1024; },
                      "directory partial progress");
                queue.pause(tree_id);
                queue.stop();
                check(!fs::exists(tree_dest), "paused directory has no final destination");
                queue.stop();
            }
            {
                WindowsTransfers queue(journal, [] {}, [](auto&, bool) {}, [] { return false; });
                auto tree_task = queue.snapshot().back();
                check(tree_task.directory && tree_task.state == TaskState::paused,
                      "directory manifest survives queue restart");
                queue.resume(tree_task.id);
                until([&] { return queue.snapshot().back().state == TaskState::completed; },
                      "directory resume complete");
                check(hash_file(root / L"resumed-tree/large.bin") == expected, "resumed directory SHA-256");
                queue.stop();
            }
            {
                WindowsMediaBridge media(c, "large.bin");
                auto range = http(media.url(), "GET|Range: bytes=12345-22344");
                std::string text(range.begin(), range.end());
                auto at = text.find("\r\n\r\n");
                check(text.starts_with("HTTP/1.1 206") && at != std::string::npos,
                      "original-media range response");
                Bytes payload(range.begin() + at + 4, range.end());
                check(payload == Bytes(data.begin() + 12345, data.begin() + 22345),
                      "original-media random read bytes exact");
                auto head = http(media.url(), "HEAD");
                check(std::string(head.begin(), head.end()).find("Content-Length: 16777216") !=
                          std::string::npos,
                      "media HEAD metadata");
                auto invalid = http(media.url(), "GET|Range: bytes=99999999-");
                check(std::string(invalid.begin(), invalid.end()).starts_with("HTTP/1.1 416"),
                      "invalid media seek rejected");
                auto unknown = http(media.url() + "-invalid", "GET");
                check(std::string(unknown.begin(), unknown.end()).starts_with("HTTP/1.1 404"),
                      "media URL nonce required");
                auto origin = http(media.url(), "GET|Origin: https://untrusted.invalid");
                check(std::string(origin.begin(), origin.end()).starts_with("HTTP/1.1 403"),
                      "cross-origin web reads denied");
                media.stop();
            }
            {
                auto control_client = c.connect(control, 0);
                Writer w;
                w.u32(16);
                w.u32('A');
                expect(
                    Code::invalid, [&] { control_client->call(Op::shortcut, w); },
                    "shortcut modifiers bounded");
                w.b.clear();
                w.u32(3);
                w.u32(VK_DELETE);
                expect(
                    Code::unsupported, [&] { control_client->call(Op::shortcut, w); },
                    "secure attention not simulated");
            }
            Writer corrupt;
            {
                fs::create_directories(client_state / L"cache");
                auto budget = client_state / L"cache/budget.bin";
                Handle f(CreateFileW(budget.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                     FILE_ATTRIBUTE_NORMAL, nullptr));
                DWORD returned = 0;
                check(f.h != INVALID_HANDLE_VALUE &&
                          DeviceIoControl(f, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr),
                      "cache budget fixture is sparse");
                LARGE_INTEGER size;
                size.QuadPart = 2ll * 1024 * 1024 * 1024;
                check(SetFilePointerEx(f, size, nullptr, FILE_BEGIN) && SetEndOfFile(f),
                      "cache fixture logical 2GiB bound");
            }
            {
                WindowsTransfers q(
                    client_state / L"cache-test.dpapi", [] {}, [](auto&, bool) {}, [] { return false; });
                q.add(c, "note.txt", client_state / L"cache/new.txt", false, true);
                until([&] { return q.snapshot()[0].state == TaskState::failed; },
                      "hard cache bound rejects queued task");
                check(!fs::exists(client_state / L"cache/new.txt"),
                      "queued cache downloads cannot exceed 2GiB");
                q.stop();
            }
            fs::remove(client_state / L"cache/budget.bin");
            corrupt.u32(99);
            write_file(root / L"bad.dpapi", protect(corrupt.b));
            expect(
                Code::unsupported,
                [&] {
                    WindowsTransfers q(root / L"bad.dpapi", [] {}, [](auto&, bool) {}, [] { return false; });
                },
                "incompatible journal rejected");
        }
        std::ofstream metadata(root / L"locations.txt");
        metadata << utf8(client_state.wstring()) << "\n" << utf8(state.wstring()) << "\n";
        metadata.close();
        DecodeStats decoded{};
        RenderStats rendered{};
        double seconds = 0;
        uint64_t bytes_ = 0;
        bool hardware = false;
        size_t clock_samples = 0;
        double clock_p95 = 0;
        std::string video_error;
        if (argc > 2 && (std::wstring(argv[2]) == L"--media" || media_only)) {
            WNDCLASSW wc{};
            wc.lpfnWndProc = canvas;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = L"LanternValidationSurface";
            RegisterClassW(&wc);
            auto window =
                CreateWindowW(wc.lpszClassName, L"Lantern generated decoder test", WS_OVERLAPPEDWINDOW, 20,
                              20, 900, 600, nullptr, nullptr, wc.hInstance, nullptr);
            ShowWindow(window, SW_SHOW);
            UpdateWindow(window);
            try {
                auto stream = c.connect(view, 1);
                Writer request;
                request.u32(2);
                request.u32(0);
                request.u32(1920);
                request.u32(60);
                request.u32(12'000'000);
                auto b = stream->call(Op::desktop_stream, std::move(request));
                Reader r{b};
                check(r.u32() == 2, "desktop shared clock stream version");
                VideoConfig v;
                v.width = r.u32();
                v.height = r.u32();
                v.fps = r.u32();
                v.bitrate = r.u32();
                v.capture_provider = r.str();
                v.encoder_provider = r.str();
                v.headers = r.blob(65536);
                check(r.str() == "host-qpc-us", "desktop host QPC timestamp domain");
                r.end();
                std::cout << "stage: decoder and audio clock initialize" << std::endl;
                auto init_started = std::chrono::steady_clock::now();
                auto timeline = std::make_shared<AvTimeline>();
                timeline->synchronize(true);
                WindowsVideo decoder(window, v, timeline);
                std::atomic_bool audio_ready = false, audio_failed = false;
                std::jthread audio_master([&](std::stop_token stop) {
                    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    try {
                        WindowsAudio master(48000, 0, 0, timeline);
                        Cancel cancellation;
                        Bytes silent(48000 * 4 / 100);
                        auto next = std::chrono::steady_clock::now();
                        while (!stop.stop_requested()) {
                            master.packet(silent, 0, cancellation, monotonic_us());
                            audio_ready = true;
                            next += std::chrono::milliseconds(10);
                            std::this_thread::sleep_until(next);
                        }
                    } catch (...) {
                        audio_failed = true;
                        audio_ready = true;
                    }
                    CoUninitialize();
                });
                until([&] { return audio_ready.load(); }, "independent audio clock worker ready");
                check(!audio_failed, "independent real WASAPI renderer initialized");
                WindowsTransfers parallel(
                    client_state / L"parallel.dpapi", [] {}, [](auto&, bool) {}, [] { return true; });
                auto parallel_id = parallel.add(c, "large.bin", root / L"parallel.bin", false);
                Writer ready;
                ready.u32(1);
                std::cout << "decoder/audio initialization ms: "
                          << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                       init_started)
                                 .count()
                          << std::endl;
                stream->send(Op::desktop_ready, std::move(ready));
                auto start = std::chrono::steady_clock::now();
                for (int i = 0; i < 300; ++i) {
                    pump();
                    auto packet = stream->receive(Op::desktop_packet);
                    Reader p{packet};
                    auto seq = p.u64();
                    auto pts = p.u64();
                    if (i < 3 || i % 60 == 0)
                        std::cout << "hardware packet " << i << " pts " << pts << std::endl;
                    p.u32();
                    auto payload = p.blob();
                    p.end();
                    check(seq == uint64_t(i), "desktop stream sequence");
                    bytes_ += payload.size();
                    if (!i)
                        payload.insert(payload.begin(), v.headers.begin(), v.headers.end());
                    auto render_started = std::chrono::steady_clock::now();
                    decoder.frame(payload, pts, true, i > 180 ? 1.5 : 1);
                    auto render_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - render_started)
                                         .count();
                    if (render_ms > 100)
                        std::cout << "slow render " << i << " ms " << render_ms << " scheduled_us_total "
                                  << decoder.stats().scheduled_wait_us << std::endl;
                    if (i < 3)
                        std::cout << "hardware decoded " << decoder.stats().decoded << " presented "
                                  << decoder.stats().presented << std::endl;
                    if (i == 90)
                        SetWindowPos(window, nullptr, 20, 20, 1000, 750, SWP_NOZORDER);
                    if (i == 150)
                        SetWindowPos(window, nullptr, 20, 20, 720, 950, SWP_NOZORDER);
                }
                seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                decoded = decoder.stats();
                auto samples = timeline->samples();
                clock_samples = samples.size();
                std::vector<double> errors;
                for (auto value : samples)
                    errors.push_back(std::abs(double(value)) / 1000);
                std::sort(errors.begin(), errors.end());
                if (!errors.empty())
                    clock_p95 = errors[size_t((errors.size() - 1) * .95)];
                check(clock_samples > 100, "real WASAPI clock anchors shared GPU presentation timeline");
                check(decoded.decoded >= 295 && decoded.gpu_surfaces == decoded.decoded &&
                          decoded.cpu_surfaces == 0,
                      "real DXVA output surfaces with no CPU decoded-pixel copy");
                check(decoded.presented >= 290 && decoded.presented <= decoded.decoded,
                      "actual GPU presents");
                hardware = true;
                stream->interrupt();
                until([&] { return parallel.snapshot()[0].state == TaskState::completed; },
                      "parallel video and file task complete");
                check(hash_file(root / L"parallel.bin") == expected,
                      "GPU desktop and paced file transfer coexist");
                parallel.stop();
            } catch (const std::exception& e) {
                video_error = e.what();
            }
            DestroyWindow(window);
            Cancel cancel;
            WindowsAudio audio(48000, 0, 0);
            Bytes silence(480 * 4);
            for (int i = 0; i < 50; ++i) {
                audio.packet(silence, 0, cancel);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            rendered = audio.stats();
            check(rendered.frames == 24000, "WASAPI actual PCM buffer submissions");
            check(rendered.queued_frames < 48000 / 10, "bounded output buffer");
            auto encoder = open_opus_encoder(48000);
            if (encoder && WindowsAudio::supports_opus()) {
                WindowsAudio opus(48000, 1, encoder->lookahead_frames());
                for (int i = 0; i < 50; ++i) {
                    auto packet = encoder->encode(silence);
                    opus.packet(packet, 0, cancel);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                check(opus.stats().frames == 24000 - encoder->lookahead_frames(),
                      "real Opus decode and WASAPI playback");
                expect(Code::invalid, [&] { opus.packet(Bytes(5000), 0, cancel); }, "oversize Opus rejected");
            }
            if (!hardware)
                throw Error(Code::unsupported, "Hardware validation failed: " + video_error);
        }
        std::ofstream report(root / L"windows-client.json");
        report << "{\"success\":true,\"kind\":\"measured\",\"checks\":" << checks
               << ",\"transport\":\"Schannel TLS 127.0.0.1 same PC\",\"hardware_decode\":"
               << (hardware ? "true" : "false") << ",\"decoded_frames\":" << decoded.decoded
               << ",\"presented_frames\":" << decoded.presented
               << ",\"cpu_decoded_surfaces\":" << decoded.cpu_surfaces << ",\"seconds\":" << seconds
               << ",\"gpu_present_calls_per_second\":" << (seconds ? decoded.presented / seconds : 0)
               << ",\"software_clock_samples\":" << clock_samples
               << ",\"software_clock_abs_skew_ms_p95\":" << clock_p95
               << ",\"audio_clock_test_source\":\"generated local silent PCM; not remote acoustic sync\""
               << ",\"encoded_payload_bytes\":" << bytes_
               << ",\"wasapi_pcm_frames_submitted\":" << rendered.frames
               << ",\"optical_latency_measured\":false,\"av_sync_measured\":false,\"two_pc_lan_measured\":"
                  "false}";
        std::cout << "Windows client checks passed: " << checks
                  << "\nReport: " << utf8((root / L"windows-client.json").wstring()) << "\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        CoUninitialize();
        return 1;
    }
}
