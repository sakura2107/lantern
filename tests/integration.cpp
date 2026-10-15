#include "lantern/win.hpp"
#include <iostream>
#include <thread>
#include <sddl.h>
#include <winioctl.h>
using namespace lantern;
static int checks = 0;
void check(bool b, const char* s) {
    ++checks;
    if (!b)
        throw std::runtime_error(s);
}
template <class F> void error(Code code, F f, const char* s) {
    bool hit = false;
    try {
        f();
    } catch (const Error& e) {
        hit = e.code == code;
    }
    check(hit, s);
}
/** Replaces a folder's DACL (SDDL); "D:P" = protected and empty, so nobody may open it. */
static bool set_dacl(const fs::path& p, const wchar_t* sddl) {
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, nullptr))
        return false;
    bool ok = SetFileSecurityW(p.c_str(), DACL_SECURITY_INFORMATION, sd) != 0;
    LocalFree(sd);
    return ok;
}
/** A drive root holds folders the account cannot open; listing it must still work (lantern E:\ report). */
static void unopenable_children(const fs::path& root) {
    auto base = root / L"drive-like";
    fs::create_directories(base / L"normal");
    fs::create_directories(base / L"locked");
    fs::create_directories(base / L"system");
    write_file(base / L"file.txt", Bytes{'x'});
    SetFileAttributesW((base / L"system").c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    check(set_dacl(base / L"locked", L"D:P"), "deny-all folder prepared");
    try {
        FileService files(base);
        auto page = files.page("", 0, 256, "", 0, false, "");
        std::vector<std::string> names;
        for (auto& e : page.entries)
            names.push_back(e.name);
        check(page.total == 2 && names == std::vector<std::string>{"normal", "file.txt"},
              "sorted page skips unopenable and protected OS folders");
        std::string gen;
        auto plain = files.list("", 0, 256, "", gen);
        check(std::none_of(plain.begin(), plain.end(), [](auto& e) { return e.name == "system"; }),
              "plain listing hides protected OS folders");
    } catch (...) {
        set_dacl(base / L"locked", L"D:(A;OICI;FA;;;WD)");
        throw;
    }
    set_dacl(base / L"locked", L"D:(A;OICI;FA;;;WD)");
    SetFileAttributesW((base / L"system").c_str(), FILE_ATTRIBUTE_NORMAL);
}
static bool junction(const fs::path& link, const fs::path& target) {
    if (!CreateDirectoryW(link.c_str(), nullptr))
        return false;
    Handle h(CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (h.h == INVALID_HANDLE_VALUE)
        return false;
    auto print = fs::absolute(target).wstring(), sub = L"\\??\\" + print;
    size_t bytes = 16 + (sub.size() + print.size() + 2) * sizeof(wchar_t);
    Bytes b(bytes);
    auto tag = (DWORD*)b.data();
    *tag = IO_REPARSE_TAG_MOUNT_POINT;
    auto words = (WORD*)(b.data() + 4);
    words[0] = WORD(bytes - 8);
    words[1] = 0;
    words[2] = 0;
    words[3] = WORD(sub.size() * 2);
    words[4] = WORD((sub.size() + 1) * 2);
    words[5] = WORD(print.size() * 2);
    auto path = (wchar_t*)(b.data() + 16);
    std::copy(sub.begin(), sub.end(), path);
    std::copy(print.begin(), print.end(), path + sub.size() + 1);
    DWORD returned = 0;
    return DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, b.data(), DWORD(b.size()), nullptr, 0, &returned,
                           nullptr) != 0;
}
static bool run_java(const fs::path& jdk, const fs::path& classes, const fs::path& library,
                     const std::string& pin, const std::string& token, const fs::path& directory) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read_pipe, write_pipe;
    if (!CreatePipe(&read_pipe, &write_pipe, &sa, 0))
        win_error("JVM input pipe");
    Handle in(read_pipe), out(write_pipe);
    SetHandleInformation(out, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW start{};
    start.cb = sizeof(start);
    start.dwFlags = STARTF_USESTDHANDLES;
    start.hStdInput = in;
    start.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    start.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    auto command = L"\"" + (jdk / L"bin/java.exe").wstring() + L"\" -Djava.library.path=\"" +
                   library.wstring() + L"\" -cp \"" + classes.wstring() +
                   L"\" org.lantern.client.InteropTest";
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                        &start, &process))
        win_error("Launch JVM interoperability");
    Handle proc(process.hProcess), thread(process.hThread);
    in = Handle();
    auto data = pin + "\n" + token + "\n" + utf8(directory.wstring()) + "\n";
    DWORD n = 0;
    if (!WriteFile(out, data.data(), DWORD(data.size()), &n, nullptr))
        win_error("JVM credentials pipe");
    out = Handle();
    auto wait = WaitForSingleObject(proc, 60000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(proc, 1);
        throw Error(Code::io, "JVM timed out");
    }
    DWORD code = 1;
    GetExitCodeProcess(proc, &code);
    return code == 0;
}
int wmain(int argc, wchar_t** argv) {
    auto testroot = fs::temp_directory_path() / wide("Lantern-test-" + random_token().substr(0, 12));
    try {
        auto state = testroot / L"state", root = testroot / L"root", client = testroot / L"client";
        fs::create_directories(state);
        fs::create_directories(root);
        fs::create_directories(client);
        Bytes data(4 * 1024 * 1024 + 731);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = uint8_t((i * 97 + i / 731) % 256);
        write_file(root / L"source.bin", data);
        auto hash = sha256(data);
        std::string pin;
        {
            Identity id(state / L"identity.dpapi", true);
            pin = id.fingerprint();
        }
        auto token = random_token();
        save_config(state / L"config.dpapi", {root, token, 15});
        unopenable_children(testroot);
        FileService local(root);
        error(Code::unauthorized, [&] { local.stat("../outside"); }, "traversal");
        error(Code::unauthorized, [&] { local.stat("source.bin:ads"); }, "ADS");
        auto outside = testroot / L"outside";
        fs::create_directories(outside);
        write_file(outside / L"secret.bin", data);
        bool link = CreateSymbolicLinkW((root / L"escape").c_str(), outside.c_str(),
                                        SYMBOLIC_LINK_FLAG_DIRECTORY |
                                            SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
        if (link)
            error(Code::unauthorized, [&] { local.stat("escape/secret.bin"); }, "symlink escape");
        else
            std::cout << "SKIP symlink creation Windows=" << GetLastError()
                      << "; unprivileged junction and hardlink tests still execute\n";
        check(junction(root / L"junction", outside), "create unprivileged junction");
        error(Code::unauthorized, [&] { local.stat("junction/secret.bin"); }, "junction read escape");
        error(
            Code::unauthorized, [&] { local.begin("junction/write.bin", 3, std::string(64, '0')); },
            "junction write escape");
        check(CreateHardLinkW((root / L"hardlink.bin").c_str(), (outside / L"secret.bin").c_str(), nullptr) !=
                  0,
              "create hardlink");
        error(Code::unauthorized, [&] { local.stat("hardlink.bin"); }, "hardlink read denied");
        auto [id, off] = local.begin("incomplete.bin", data.size(), hash);
        check(off == 0, "begin");
        local.chunk(id, 0, {data.data(), chunk_size});
        error(Code::conflict, [&] { local.chunk(id, 0, {data.data(), chunk_size}); }, "stale offset");
        error(Code::conflict, [&] { local.commit(id); }, "partial commit");
        check(!fs::exists(root / L"incomplete.bin"), "partial exposed");
        {
            FileService recovered(root);
            auto resumed = recovered.begin("incomplete.bin", data.size(), hash);
            check(resumed.first == id && resumed.second == chunk_size, "disk recovery");
        }
        auto bad = local.begin("bad.bin", 3, std::string(64, '0'));
        uint8_t abc[] = {1, 2, 3};
        local.chunk(bad.first, 0, abc);
        error(Code::integrity, [&] { local.commit(bad.first); }, "bad checksum");
        check(!fs::exists(root / L"bad.bin"), "bad file committed");
        Host host(state, "127.0.0.1", 47449);
        std::thread thread([&] { host.run(); });
        struct Stop {
            Host& h;
            std::thread& t;
            ~Stop() {
                h.stop();
                if (t.joinable())
                    t.join();
            }
        } stop{host, thread};
        {
            error(
                Code::unauthorized, [&] { Client c("127.0.0.1", 47449, std::string(64, '0'), token); },
                "host pin mismatch");
            bool denied = false;
            try {
                Client c("127.0.0.1", 47449, pin, random_token());
            } catch (const Error&) {
                denied = true;
            }
            check(denied, "bad token denied");
            Client c("127.0.0.1", 47449, pin, token, files_read | files_write);
            check(!c.list("").empty(), "file-only listing");
            auto f = c.stat("source.bin");
            check(f.size == data.size() && f.hash == hash, "stat hash");
            check(c.read("source.bin", f.version, 513, 16384) ==
                      Bytes(data.begin() + 513, data.begin() + 513 + 16384),
                  "media random byte range");
            error(
                Code::source_changed, [&] { c.read("source.bin", "wrong-version", 0, 5); },
                "source version guard");
            write_file(root / L"mutable.bin", data);
            auto old = c.stat("mutable.bin");
            Sleep(2);
            data[0] ^= 1;
            write_file(root / L"mutable.bin", data);
            error(
                Code::source_changed, [&] { c.read("mutable.bin", old.version, 0, 5); },
                "actual changed source rejected");
            data[0] ^= 1;
            Writer trailing;
            trailing.str("not-created");
            trailing.str("");
            trailing.u16(7);
            error(
                Code::invalid, [&] { c.call(Op::mkdir, std::move(trailing)); },
                "trailing request rejected before mutation");
            check(!fs::exists(root / L"not-created"), "malformed request mutated disk");
            error(Code::unsupported, [&] { c.call(Op(65535)); }, "unknown protocol operation");
            Cancel cancelled;
            cancelled.requested = true;
            error(
                Code::cancelled, [&] { c.download("source.bin", client / L"cancelled.bin", cancelled); },
                "cancel queued download");
            Cancel download;
            error(
                Code::cancelled,
                [&] {
                    c.download("source.bin", client / L"download.bin", download, [&](uint64_t n, uint64_t) {
                        if (n >= chunk_size)
                            download.requested = true;
                    });
                },
                "cancel active download");
            check(!fs::exists(client / L"download.bin"), "unverified local exposed");
            download.requested = false;
            uint64_t resumed = 0;
            c.download("source.bin", client / L"download.bin", download, [&](uint64_t n, uint64_t) {
                if (!resumed)
                    resumed = n;
            });
            check(resumed == chunk_size, "download resumed offset");
            check(hash_file(client / L"download.bin") == hash, "download verified");
            Cancel upload;
            error(
                Code::cancelled,
                [&] {
                    c.upload(client / L"download.bin", "uploaded.bin", upload, [&](uint64_t n, uint64_t) {
                        if (n >= chunk_size)
                            upload.requested = true;
                    });
                },
                "cancel active upload");
            check(!fs::exists(root / L"uploaded.bin"), "unverified remote exposed");
            upload.requested = false;
            uint64_t uploaded = 0;
            c.upload(client / L"download.bin", "uploaded.bin", upload, [&](uint64_t n, uint64_t) {
                if (!uploaded)
                    uploaded = n;
            });
            check(uploaded == chunk_size, "upload resumed offset");
            check(hash_file(root / L"uploaded.bin") == hash, "upload verified");
            c.mutate(Op::mkdir, "folder");
            c.mutate(Op::copy, "source.bin", "folder/copy.bin");
            check(hash_file(root / L"folder/copy.bin") == hash, "host-side copy");
            c.mutate(Op::rename, "folder/copy.bin", "folder/moved.bin");
            c.mutate(Op::remove, "folder/moved.bin");
            check(!fs::exists(root / L"folder/moved.bin"), "host-side remove");
            Client readonly("127.0.0.1", 47449, pin, token);
            error(Code::unauthorized, [&] { readonly.mutate(Op::mkdir, "denied"); }, "write scope denied");
            error(Code::unauthorized, [&] { readonly.call(Op::monitors); }, "view scope denied");
            error(Code::unauthorized, [&] { readonly.call(Op::desktop_stream); }, "stream view scope denied");
            Client viewing("127.0.0.1", 47449, pin, token, view, 1);
            error(Code::invalid, [&] { viewing.call(Op::desktop_stream); }, "stream truncated request");
            Writer incompatible;
            incompatible.u32(3);
            error(
                Code::unsupported, [&] { viewing.call(Op::desktop_stream, incompatible); },
                "stream version incompatible");
            Writer invalid;
            invalid.u32(1);
            invalid.u32(0);
            invalid.u32(0);
            invalid.u32(60);
            invalid.u32(12000000);
            error(
                Code::invalid, [&] { viewing.call(Op::desktop_stream, invalid); }, "stream dimension bounds");
            Client wrong_lane("127.0.0.1", 47449, pin, token, view, 0);
            error(
                Code::invalid, [&] { wrong_lane.call(Op::desktop_stream, invalid); },
                "stream lane separation");
            error(Code::unauthorized, [&] { readonly.call(Op::click); }, "atomic click requires control");
            Client controller("127.0.0.1", 47449, pin, token, control, 0);
            error(Code::invalid, [&] { controller.call(Op::click); }, "atomic click truncated packet");
            Writer bad_pointer;
            bad_pointer.u32(65536);
            bad_pointer.u32(10);
            bad_pointer.u32(0);
            error(
                Code::invalid, [&] { controller.call(Op::click, bad_pointer); },
                "atomic click coordinate bounds");
            for (auto operation :
                 {Op::pointer_gesture, Op::double_click, Op::text_input, Op::key_press, Op::input_focus})
                error(
                    Code::unauthorized, [&] { readonly.call(operation); },
                    "new input operations require control");
            error(Code::invalid, [&] { controller.call(Op::pointer_gesture); }, "gesture truncated packet");
            Writer invalid_phase;
            invalid_phase.u32(0);
            invalid_phase.u32(0);
            invalid_phase.u32(0);
            invalid_phase.u32(0);
            error(
                Code::invalid, [&] { controller.call(Op::pointer_gesture, invalid_phase); },
                "gesture invalid phase");
            error(
                Code::invalid, [&] { controller.call(Op::double_click, bad_pointer); },
                "double click coordinate bounds");
            Writer bad_text;
            bad_text.str(std::string("\xc0\xaf", 2));
            error(Code::invalid, [&] { controller.call(Op::text_input, bad_text); }, "text invalid UTF-8");
            Writer large_text;
            large_text.str(std::string(1025, 'x'));
            error(Code::invalid, [&] { controller.call(Op::text_input, large_text); }, "text UTF-16 bound");
            Writer control_text;
            control_text.str("hello\n");
            error(
                Code::invalid, [&] { controller.call(Op::text_input, control_text); },
                "text controls use atomic key operation");
            Writer bad_key;
            bad_key.u32(0x5b);
            bad_key.u32(1);
            error(Code::invalid, [&] { controller.call(Op::key_press, bad_key); }, "key whitelist");
            Writer too_many;
            too_many.u32(8);
            too_many.u32(33);
            error(Code::invalid, [&] { controller.call(Op::key_press, too_many); }, "key repeat bound");
            {
                Client wrong_input_lane("127.0.0.1", 47449, pin, token, control, 3);
                error(
                    Code::invalid, [&] { wrong_input_lane.call(Op::pointer_gesture, invalid_phase); },
                    "gesture lane separation");
            }
            error(
                Code::unauthorized, [&] { viewing.call(Op::audio_stream); },
                "audio permission independent of viewing");
            error(
                Code::unauthorized, [&] { Client denied_audio("127.0.0.1", 47449, pin, token, audio, 4); },
                "audio HELLO denied without configured scope");
            {
                save_config(state / L"config.dpapi", {root, token, 31});
                Client audio_client("127.0.0.1", 47449, pin, token, audio, 4);
                error(Code::invalid, [&] { audio_client.call(Op::audio_stream); }, "audio truncated request");
                Writer audio_version;
                audio_version.u32(99);
                error(
                    Code::unsupported, [&] { audio_client.call(Op::audio_stream, audio_version); },
                    "audio version mismatch");
                Writer audio_extra;
                audio_extra.u32(1);
                audio_extra.u32(0);
                error(
                    Code::invalid, [&] { audio_client.call(Op::audio_stream, audio_extra); },
                    "audio trailing bytes");
                Client wrong_audio_lane("127.0.0.1", 47449, pin, token, audio, 0);
                error(
                    Code::invalid, [&] { wrong_audio_lane.call(Op::audio_stream, audio_version); },
                    "audio independent channel required");
                save_config(state / L"config.dpapi", {root, token, audio});
                {
                    Client identity_only("127.0.0.1", 47449, pin, token, 0, 0);
                    check(!identity_only.call(Op::capabilities).empty(),
                          "audio-only identity pairs without file scope");
                    error(
                        Code::unauthorized, [&] { identity_only.list(""); },
                        "audio-only configuration denies file access");
                }
                save_config(state / L"config.dpapi", {root, token, 15});
                error(
                    Code::unauthorized, [&] { audio_client.call(Op::audio_stream, audio_version); },
                    "audio permission revocation on live control socket");
            }
            if (argc == 4)
                check(run_java(argv[1], argv[2], argv[3], pin, token, client),
                      "Java/JNI cross-client interoperability");
            auto new_token = random_token();
            save_config(state / L"config.dpapi", {root, new_token, 15});
            error(Code::unauthorized, [&] { c.list(""); }, "revoke active trust");
            Client freshly_paired("127.0.0.1", 47449, pin, new_token);
            check(!freshly_paired.list("").empty(), "new pairing accepted without restart");
        }
        std::cout << checks << " integration assertions passed; real TLS, disk, network and resume\n";
        // Cleanup only this freshly created test directory, after host/files release at process exit.
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Integration failure: " << e.what() << "\nArtifacts: " << utf8(testroot.wstring())
                  << "\n";
        return 1;
    }
}
