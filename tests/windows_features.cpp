#include "lantern/windows_clipboard.hpp"
#include "lantern/windows_thumbnail.hpp"
#include <fstream>
#include <iostream>
#include <objbase.h>
using namespace lantern;
static int checks = 0;
static void check(bool b, const char* name) {
    if (!b)
        throw std::runtime_error(name);
    ++checks;
    std::cout << name << std::endl;
}
template <class F> void expect(Code code, F f, const char* name) {
    bool caught = false;
    try {
        f();
    } catch (const Error& e) {
        caught = e.code == code;
    }
    check(caught, name);
}
static void pump() {
    MSG m;
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
template <class F> void until(F f, const char* name, int ms = 10000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!f()) {
        pump();
        if (std::chrono::steady_clock::now() > end)
            throw std::runtime_error(name);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
// No native clipboard call occurs before selecting a noninteractive window station.
// A deterministic in-memory local provider exercises sync against the real isolated host clipboard.
struct PrivateDesktop {
    HWINSTA previous = nullptr, station = nullptr;
    HDESK desktop = nullptr, old_desktop = nullptr;
    PrivateDesktop() {
        previous = GetProcessWindowStation();
        old_desktop = GetThreadDesktop(GetCurrentThreadId());

        station = CreateWindowStationW(nullptr, 0,
                                       WINSTA_ACCESSCLIPBOARD | WINSTA_ACCESSGLOBALATOMS |
                                           WINSTA_CREATEDESKTOP | WINSTA_READATTRIBUTES,
                                       nullptr);
        if (!station)
            win_error("Create private station");
        USEROBJECTFLAGS flags{};
        DWORD needed = 0;
        if (!GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &needed) ||
            (flags.dwFlags & WSF_VISIBLE))
            throw std::runtime_error("Refuse interactive clipboard station");
        if (!SetProcessWindowStation(station))
            win_error("Select private station");
        desktop = CreateDesktopW(L"Fixture", nullptr, nullptr, 0,
                                 DESKTOP_CREATEWINDOW | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS |
                                     DESKTOP_SWITCHDESKTOP,
                                 nullptr);
        if (!desktop || !SetThreadDesktop(desktop))
            win_error("Private clipboard desktop");
    }
    ~PrivateDesktop() {
        SetThreadDesktop(old_desktop);
        SetProcessWindowStation(previous);
        if (desktop)
            CloseDesktop(desktop);
        if (station)
            CloseWindowStation(station);
    }
};
struct Child {
    PROCESS_INFORMATION process{};
    fs::path root;
    Child(const fs::path& r) : root(r) {
        wchar_t exe[32768];
        GetModuleFileNameW(nullptr, exe, 32768);
        auto cmd = L"\"" + std::wstring(exe) + L"\" --host \"" + r.wstring() + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        if (!CreateProcessW(exe, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                            &startup, &process))
            win_error("Spawn isolated host");
    }
    ~Child() {
        write_file(root / L"stop", Bytes{1});
        if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT)
            TerminateProcess(process.hProcess, 2);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
};
static TransferTask task(WindowsTransfers& q, const std::string& id) {
    for (auto& t : q.snapshot())
        if (t.id == id)
            return t;
    throw std::runtime_error("Task missing");
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 2)
        return 2;
    try {
        PrivateDesktop isolation;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (std::wstring(argv[1]) == L"--host"||std::wstring(argv[1])==L"--host-lan") {
            fs::path root = argv[2];
            write_clipboard_text(nullptr, GetClipboardSequenceNumber(), "remote initial");
            bool lan=std::wstring(argv[1])==L"--host-lan";
            Host host(root / (lan?L"state":L"host"),lan?"0.0.0.0":"127.0.0.1",lan?uint16_t(std::stoul(argv[3])):47446);
            std::thread worker([&] { host.run(); });
            write_file(root / L"ready", Bytes{1});
            while (!fs::exists(root / L"stop"))
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            host.stop();
            worker.join();
            CoUninitialize();
            return 0;
        }
        auto root = fs::absolute(argv[1]) / wide(random_token().substr(0, 16)), share = root / L"share",
             state = root / L"host";
        fs::create_directories(share);
        fs::create_directories(state);
        Identity identity(state / L"identity.dpapi", true);
        Config config;
        config.root = share;
        config.token = random_token();
        config.scopes = 31;
        save_config(state / L"config.dpapi", config);
        ClientCredentials c{"127.0.0.1", identity.fingerprint(), config.token, 47446};
        save_credentials(root / L"device.dpapi", c);
        Bytes payload(2 * 1024 * 1024, 43);
        write_file(share / L"slow.bin", payload);
        write_file(share / L"small.txt", Bytes{'o', 'k'});
        Bytes bmp(70);
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[2] = 70;
        bmp[10] = 54;
        bmp[14] = 40;
        bmp[18] = 2;
        bmp[22] = 2;
        bmp[26] = 1;
        bmp[28] = 24;
        for (size_t i = 54; i < bmp.size(); ++i)
            bmp[i] = uint8_t(i * 5);
        write_file(share / L"sample.bmp", bmp);
        write_file(root / L"one.txt", Bytes{'1'});
        write_file(root / L"two.txt", Bytes{'2'});
        Child child(root);
        until([&] { return fs::exists(root / L"ready"); }, "private host ready");
        auto peer = c.connect(control, 0);
        std::mutex memory_mutex;
        ClipboardText memory{1, false, {}};
        auto local_read = [&] {
            std::lock_guard lock(memory_mutex);
            return memory;
        };
        auto local_write = [&](uint32_t sequence, const std::string& value) {
            std::lock_guard lock(memory_mutex);
            if (memory.sequence != sequence)
                throw Error(Code::conflict, "Test provider CAS");
            memory.text = value;
            memory.present = true;
            return ++memory.sequence;
        };
        auto local = local_read();
        check(!local.present, "independent test client starts empty");
        auto remote = client_clipboard_read(*peer);
        check(remote.text == "remote initial", "remote station independent clipboard");
        auto unicode = std::string("剪贴板测试 😀\\n第二行");
        auto seq = client_clipboard_write(*peer, remote.sequence, unicode);
        check(client_clipboard_read(*peer).text == unicode, "TLS Unicode clipboard round trip");
        expect(
            Code::conflict, [&] { client_clipboard_write(*peer, remote.sequence, "stale"); },
            "stale clipboard CAS preserves content");
        check(client_clipboard_read(*peer).text == unicode, "CAS rejection preserved remote");
        client_clipboard_write(*peer, seq, "");
        check(client_clipboard_read(*peer).present && client_clipboard_read(*peer).text.empty(),
              "empty text remains text");
        auto maxtext = std::string(65536, 'a');
        remote = client_clipboard_read(*peer);
        client_clipboard_write(*peer, remote.sequence, maxtext);
        check(client_clipboard_read(*peer).text == maxtext, "64KiB text boundary");
        expect(
            Code::invalid, [&] { client_clipboard_write(*peer, 0, maxtext + "a"); },
            "oversize clipboard rejected");
        expect(
            Code::invalid, [&] { client_clipboard_write(*peer, 0, std::string("a\0b", 3)); },
            "embedded NUL rejected");
        {
            auto readonly = c.connect(files_read, 0);
            expect(
                Code::unauthorized, [&] { client_clipboard_read(*readonly); },
                "clipboard requires control scope");
            Writer w;
            w.u32(2);
            expect(
                Code::unsupported, [&] { peer->call(Op::clipboard_read, w); },
                "unknown clipboard API rejected");
        }
        remote = client_clipboard_read(*peer);
        client_clipboard_write(*peer, remote.sequence, "remote baseline");
        local_write(local_read().sequence, "local baseline");
        std::mutex messages_mutex;
        std::vector<std::wstring> messages;
        ClipboardSync sync(
            nullptr,
            [&](std::wstring m) {
                std::lock_guard lock(messages_mutex);
                messages.push_back(std::move(m));
            },
            ClipboardAccess{local_read, local_write});
        auto started = [&] {
            std::lock_guard lock(messages_mutex);
            return !messages.empty() && messages.back().find(L"已开启") != std::wstring::npos;
        };
        sync.start(c);
        until(started, "auto sync baseline ready");
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        check(local_read().text == "local baseline" && client_clipboard_read(*peer).text == "remote baseline",
              "enable sync does not overwrite initial content");
        local_write(local_read().sequence, unicode);
        until([&] { return client_clipboard_read(*peer).text == unicode; }, "local to remote auto sync");
        check(true, "sync worker local to real host clipboard");
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        auto stable = client_clipboard_read(*peer).sequence;
        std::this_thread::sleep_for(std::chrono::milliseconds(550));
        check(stable == client_clipboard_read(*peer).sequence, "synchronization does not echo");
        remote = client_clipboard_read(*peer);
        client_clipboard_write(*peer, remote.sequence, "remote update");
        until([&] { return local_read().text == "remote update"; }, "remote to local auto sync");
        check(true, "sync worker real host clipboard to local");
        sync.stop();
        local_write(local_read().sequence, "stopped local");
        std::this_thread::sleep_for(std::chrono::milliseconds(650));
        check(client_clipboard_read(*peer).text == "remote update", "stopped sync preserves remote");
        sync.copy_once(c, true);
        until([&] { return client_clipboard_read(*peer).text == "stopped local"; }, "manual push");
        check(true, "manual push without desktop subscription");
        remote = client_clipboard_read(*peer);
        client_clipboard_write(*peer, remote.sequence, "manual remote");
        sync.copy_once(c, false);
        until([&] { return local_read().text == "manual remote"; }, "manual pull");
        check(true, "manual pull without desktop subscription");
        {
            std::lock_guard lock(messages_mutex);
            messages.clear();
        }
        sync.start(c);
        until(started, "conflict baseline");
        local_write(local_read().sequence, "local conflict");
        remote = client_clipboard_read(*peer);
        client_clipboard_write(*peer, remote.sequence, "remote conflict");
        until([&] { return !sync.enabled(); }, "simultaneous edits pause sync");
        check(local_read().text == "local conflict" && client_clipboard_read(*peer).text == "remote conflict",
              "simultaneous edits preserve both clipboards");
        sync.stop();
        auto filepeer = c.connect(files_read, 0);
        auto image = client_thumbnail(*filepeer, "sample.bmp");
        check(image.w == 2 && image.h == 2 && image.pixels.size() == 16, "real remote BMP JPEG WIC preview");
        expect(Code::invalid, [&] { decode_jpeg(Bytes{1, 2, 3}); }, "malformed preview rejected");
        expect(
            Code::invalid, [&] { decode_jpeg(image_thumbnail(bmp, 128), 1); },
            "decoded image allocation bound");
        {
            std::atomic_uint64_t ticket = 0;
            ThumbnailLoader loader([&](ThumbnailResult r) {
                if (!r.image.pixels.empty())
                    ticket = r.ticket;
            });
            for (int i = 1; i <= 40; ++i)
                loader.request(c, "sample.bmp", i);
            until([&] { return ticket == 40; }, "coalesced thumbnail latest result");
            check(true, "bounded preview replacement yields final selection");
            loader.cancel();
        }
        check(effective_transfer_rate(0, 0) == 32ull * 1024 * 1024 &&
                  effective_transfer_rate(1, 0) == 4ull * 1024 * 1024 &&
                  effective_transfer_rate(2, 0) == 2ull * 1024 * 1024 &&
                  effective_transfer_rate(3, 0) == 2ull * 1024 * 1024,
              "foreground activity reduces transfer caps");
        check(effective_transfer_rate(3, 65536) == 65536, "user cap cannot override media budget");
        double limited_seconds = 0, policy_wake_ms = 0;
        auto journal = root / L"tasks.dpapi";
        {
            WindowsTransfers q(journal, [] {}, [](auto&, bool) {}, [] { return 0u; });
            auto gate = q.add(c, "slow.bin", root / L"gate.bin", false);
            q.set_policy(gate, 0, 65536);
            until([&] { return task(q, gate).state == TaskState::running; }, "rate limited task running");
            auto n = q.snapshot().size();
            expect(
                Code::conflict,
                [&] {
                    q.add_upload_batch(
                        c, {{root / L"one.txt", "duplicate.txt"}, {root / L"two.txt", "DUPLICATE.txt"}});
                },
                "batch duplicate destinations reject atomically");
            check(q.snapshot().size() == n, "invalid batch added no partial task");
            expect(
                Code::invalid,
                [&] { q.add_upload_batch(c, {{root / L"one.txt", "valid.txt"}, {share, "bad.txt"}}); },
                "invalid batch file rejects whole selection");
            check(q.snapshot().size() == n, "invalid source leaves queue unchanged");
            auto ids = q.add_upload_batch(
                c, {{root / L"one.txt", "one-upload.txt"}, {root / L"two.txt", "two-upload.txt"}});
            check(ids.size() == 2, "batch admitted two tasks");
            expect(
                Code::conflict,
                [&] {
                    q.add_upload_batch(
                        c, {{root / L"one.txt", "one-upload.txt"}, {root / L"two.txt", "unused.txt"}});
                },
                "existing pending target rejects whole batch");
            auto low = q.add(c, "small.txt", root / L"low.txt", false);
            q.set_policy(low, 0, 0);
            auto high = q.add(c, "slow.bin", root / L"high.bin", false);
            q.set_policy(high, 2, 65536);
            check(task(q, gate).state == TaskState::running && task(q, high).state == TaskState::queued,
                  "priority does not preempt current task");
            q.pause(gate);
            until([&] { return task(q, high).state == TaskState::running; }, "high priority dispatch");
            check(task(q, low).state == TaskState::queued && task(q, ids[0]).state == TaskState::queued,
                  "highest priority selected before FIFO lower tasks");
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            auto woke = std::chrono::steady_clock::now();
            q.set_policy(high, 2, 0);
            until([&] { return task(q, high).state == TaskState::completed; },
                  "active rate update wakes pacing");
            policy_wake_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - woke).count();
            check(policy_wake_ms < 2500, "active rate change interrupts old four-second delay");
            check(sha256(read_file(root / L"high.bin", 3 * 1024 * 1024)) == sha256(payload),
                  "policy update preserves downloaded SHA");
            until([&] { return task(q, ids[1]).state == TaskState::completed; },
                  "both batch uploads complete");
            check(read_file(share / L"one-upload.txt") == Bytes{'1'} &&
                      read_file(share / L"two-upload.txt") == Bytes{'2'},
                  "both uploaded payloads verified");
            expect(Code::invalid, [&] { q.set_policy(gate, 3, 0); }, "invalid priority rejected");
            expect(Code::invalid, [&] { q.set_policy(gate, 1, 1); }, "unsafe rate rejected");
            auto paced = q.add(c, "slow.bin", root / L"paced.bin", false);
            q.set_policy(paced, 1, 1024 * 1024);
            auto began = std::chrono::steady_clock::now();
            until([&] { return task(q, paced).state == TaskState::completed; }, "real user paced transfer");
            limited_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
            check(limited_seconds >= 1.8 && limited_seconds < 8,
                  "2MiB transfer obeys actual 1MiB per second cap");
            q.set_policy(gate, 2, 65536);
            q.stop();
        }
        {
            WindowsTransfers restored(journal, [] {}, [](auto&, bool) {}, [] { return 0u; });
            auto snapshot = restored.snapshot();
            auto it = std::find_if(snapshot.begin(), snapshot.end(), [](auto& t) {
                return t.remote == "slow.bin" && t.local.filename() == L"gate.bin";
            });
            check(it != snapshot.end() && it->state == TaskState::paused && it->priority == 2 &&
                      it->rate_limit == 65536,
                  "journal v4 restores policy and paused state");
            restored.stop();
        }
        auto plain = unprotect(read_file(journal));
        Reader r{plain};
        check(r.u32() == 4, "new journal version explicit");
        r.u32();
        r.str();
        r.str();
        r.u32();
        r.str();
        r.str();
        (void)read_endpoints(r);
        r.str();
        r.str();
        r.u32();
        r.u32();
        r.u32();
        plain[r.at] = 3;
        write_file(root / L"invalid-policy.dpapi", protect(plain));
        expect(
            Code::invalid,
            [&] {
                WindowsTransfers invalid(
                    root / L"invalid-policy.dpapi", [] {}, [](auto&, bool) {}, [] { return 0u; });
            },
            "corrupt journal policy refused");
        // Capacity failure and journal failure must leave an entire batch unadmitted.
        {
            WindowsTransfers q(root / L"capacity.dpapi", [] {}, [](auto&, bool) {}, [] { return 0u; });
            for (int i = 0; i < 31; ++i) {
                auto id = q.add(c, "small.txt", root / wide("capacity-" + std::to_string(i) + ".txt"), false);
                q.pause(id);
            }
            expect(
                Code::busy,
                [&] {
                    q.add_upload_batch(c, {{root / L"one.txt", "capacity-one.txt"},
                                           {root / L"two.txt", "capacity-two.txt"}});
                },
                "whole batch refused at queue capacity");
            check(q.snapshot().size() == 31, "capacity failure keeps queue size");
            q.stop();
        }
        {
            auto path = root / L"rollback" / L"tasks.dpapi";
            WindowsTransfers q(path, [] {}, [](auto&, bool) {}, [] { return 0u; });
            fs::create_directories(path.wstring() + L".tmp");
            expect(
                Code::io,
                [&] {
                    q.add_upload_batch(c, {{root / L"one.txt", "rollback-one.txt"},
                                           {root / L"two.txt", "rollback-two.txt"}});
                },
                "journal failure rejects batch");
            check(q.snapshot().empty(), "journal failure rolls back all tasks");
            q.stop();
        }
        for (uint32_t version : {1u, 2u}) {
            Writer legacy;
            legacy.u32(version);
            legacy.u32(1);
            legacy.str(random_token());
            legacy.str(c.host);
            legacy.u32(c.port);
            legacy.str(c.pin);
            legacy.str(c.token);
            legacy.str("small.txt");
            legacy.str(utf8((root / L"legacy.txt").wstring()));
            legacy.u32(0);
            legacy.u32(0);
            if (version == 2)
                legacy.u32(0);
            legacy.u64(0);
            legacy.u64(2);
            legacy.str("");
            auto path = root / wide("legacy-" + std::to_string(version) + ".dpapi");
            write_file(path, protect(legacy.b));
            WindowsTransfers q(path, [] {}, [](auto&, bool) {}, [] { return 0u; });
            auto t = q.snapshot()[0];
            check(t.state == TaskState::paused && t.priority == 1 && t.rate_limit == 0,
                  "old journal migrates to safe paused default policy");
            q.stop();
        }
        std::ofstream report(root / L"windows-features.json");
        report << "{\"success\":true,\"kind\":\"measured\",\"checks\":" << checks
               << ",\"clipboard_peers\":\"native noninteractive host clipboard + deterministic local provider, TLS "
                  "loopback\",\"user_clipboard_accessed\":false,\"rate_2mib_at_1mib_seconds\":"
               << limited_seconds << ",\"active_policy_wake_ms\":" << policy_wake_ms
               << ",\"android_physical_tested_this_stage\":false}";
        std::cout << "Report: " << utf8((root / L"windows-features.json").wstring()) << std::endl;
        peer.reset();
        filepeer.reset();
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
