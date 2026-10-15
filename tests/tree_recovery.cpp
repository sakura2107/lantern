#include "lantern/windows_tree.hpp"
#include <fstream>
#include <iostream>
#include <thread>
using namespace lantern;
static int checks = 0;
static void check(bool v, const char* s) {
    if (!v)
        throw std::runtime_error(s);
    ++checks;
    std::cout << s << std::endl;
}
template <class F> static void expect(Code code, F f, const char* name) {
    bool ok = false;
    try {
        f();
    } catch (const Error& e) {
        ok = e.code == code;
    }
    check(ok, name);
}
static TransferTask task(const fs::path& root, const std::string& id) {
    TransferTask t;
    t.id = id;
    t.credentials = load_credentials(root / L"credentials.dpapi");
    t.remote = "tree";
    t.local = root / wide(id);
    t.directory = true;
    return t;
}
static void journal(const fs::path& path, const TransferTask& t) {
    Writer w;
    w.u32(3);
    w.u32(1);
    w.str(t.id);
    w.str(t.credentials.host);
    w.u32(t.credentials.port);
    w.str(t.credentials.pin);
    w.str(t.credentials.token);
    w.str(t.remote);
    w.str(utf8(t.local.wstring()));
    w.u32(0);
    w.u32(1);
    w.u32(1);
    w.u32(1);
    w.u64(0);
    w.u64(0);
    w.u64(0);
    w.str("");
    write_file(path, protect(w.b));
}
static void crash_commit(const fs::path& root, const std::string& id) {
    wchar_t exe[32768];
    GetModuleFileNameW(nullptr, exe, 32768);
    auto line = L"\"" + std::wstring(exe) + L"\" --commit \"" + root.wstring() + L"\" " + wide(id);
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION p{};
    if (!CreateProcessW(exe, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup, &p))
        win_error("Start crash fixture");
    Handle process(p.hProcess), thread(p.hThread);
    if (WaitForSingleObject(process, 10000) != WAIT_OBJECT_0) {
        TerminateProcess(process, 2);
        throw Error(Code::io, "Commit fixture timeout");
    }
    DWORD exit = 0;
    GetExitCodeProcess(process, &exit);
    if(exit!=99){auto error=root/L"child-error.txt";if(fs::exists(error)){auto b=read_file(error);throw std::runtime_error(std::string(b.begin(),b.end()));}throw std::runtime_error("Commit child exit "+std::to_string(exit));}
    check(exit == 99, "actual process exits after directory rename before queue completion journal");
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2)
            return 2;
        if (std::wstring(argv[1]) == L"--commit") {
            fs::path root = argv[2];
            auto t = task(root, utf8(argv[3]));
            auto c = t.credentials.connect();
            Cancel cancel;
            transfer_tree(*c, t, root / L"trees" / (wide(t.id) + L".dpapi"), cancel, [](auto, auto) {});
            ExitProcess(99);
        }
        auto root = fs::absolute(argv[1]) / wide(random_token().substr(0, 12));
        fs::create_directories(root / L"share/tree/sub");
        fs::create_directories(root / L"state");
        fs::create_directories(root / L"trees");
        write_file(root / L"share/tree/sub/test.bin", Bytes(8192, 17));
        write_file(root / L"share/tree/note.txt", Bytes{'x'});
        Identity identity(root / L"state/identity.dpapi", true);
        Config cfg{root / L"share", random_token(), 3};
        save_config(root / L"state/config.dpapi", cfg);
        ClientCredentials c{"127.0.0.1", identity.fingerprint(), cfg.token, 47448};
        save_credentials(root / L"credentials.dpapi", c);
        Host host(root / L"state", "127.0.0.1", 47448);
        std::thread server([&] { host.run(); });
        struct Stop {
            Host& h;
            std::thread& t;
            ~Stop() {
                h.stop();
                if (t.joinable())
                    t.join();
            }
        } stop{host, server};
        auto id = random_token();
        auto t = task(root, id);
        auto manifest = root / L"trees" / (wide(id) + L".dpapi");
        journal(root / L"tasks.dpapi", t);
        crash_commit(root, id);
        check(fs::exists(t.local / L"sub/test.bin") && fs::exists(manifest) &&
                  fs::exists(manifest.wstring() + L".commit"),
              "committed data and durable receipt survive process death");
        host.stop();
        server.join();
        {
            WindowsTransfers q(root / L"tasks.dpapi", [] {}, [](auto&, bool) {}, [] { return 0u; });
            q.resume(id);
            auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            for (;;) {
                auto a = q.snapshot()[0];
                if (a.state == TaskState::completed)
                    break;
                if (a.state == TaskState::failed || std::chrono::steady_clock::now() > end)
                    throw Error(Code::io, a.message);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            q.stop();
            check(q.snapshot()[0].current == 8193,
                  "offline recovery verifies files and records full completed byte count");
        }
        check(!fs::exists(manifest) && !fs::exists(manifest.wstring() + L".commit"),
              "receipt removed only after completion journal is durable");
        check(read_file(t.local / L"sub/test.bin") == Bytes(8192, 17),
              "original committed payload remains intact");
        // Preserve receipt copies to validate corruption/replacement without running unrelated media tests.
        Host source(root / L"state", "127.0.0.1", 47448);
        std::thread source_thread([&] { source.run(); });
        Stop stop_source{source, source_thread};
        auto other_id = random_token();
        auto other = task(root, other_id);
        auto other_manifest = root / L"trees" / (wide(other_id) + L".dpapi");
        crash_commit(root, other_id);
        Cancel cancel;
        auto valid = recover_committed_tree(other, other_manifest, cancel);
        check(valid == 8193, "direct receipt proves exact original directory");
        write_file(other.local / L"extra.txt", Bytes{'n'});
        expect(
            Code::conflict, [&] { recover_committed_tree(other, other_manifest, cancel); },
            "extra destination entry prevents false completion");
        fs::remove(other.local / L"extra.txt");
        write_file(other.local / L"note.txt", Bytes{'y'});
        expect(
            Code::integrity, [&] { recover_committed_tree(other, other_manifest, cancel); },
            "modified same-size destination fails SHA and is preserved");
        check(read_file(other.local / L"note.txt") == Bytes{'y'},
              "recovery never overwrites modified destination");
        write_file(other.local / L"note.txt", Bytes{'x'});
        Cancel stopped;
        stopped.requested = true;
        expect(
            Code::cancelled, [&] { recover_committed_tree(other, other_manifest, stopped); },
            "recovery hashing respects cancellation");
        auto forged = other;
        forged.credentials.pin = random_token();
        expect(
            Code::conflict, [&] { recover_committed_tree(forged, other_manifest, cancel); },
            "receipt bound to original device identity");
        fs::rename(other.local, root / L"preserved-original");
        fs::create_directories(other.local);
        expect(
            Code::conflict, [&] { recover_committed_tree(other, other_manifest, cancel); },
            "replacement directory identity rejected even at same path");
        std::ofstream report(root / L"tree-recovery.json");
        report << "{\"success\":true,\"checks\":" << checks
               << ",\"real_process_death_after_rename\":true,\"offline_recovery\":true,\"original_file_"
                  "protection\":true}";
        std::cout << "Report: " << utf8((root / L"tree-recovery.json").wstring()) << std::endl;
        return 0;
    } catch (const std::exception& e) {
        if(argc>3&&std::wstring(argv[1])==L"--commit"){std::string error=e.what();write_file(fs::path(argv[2])/L"child-error.txt",{reinterpret_cast<const uint8_t*>(error.data()),error.size()});}
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
