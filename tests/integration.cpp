#include "lantern/host_admin.hpp"
#include "lantern/lan.hpp"
#include "lantern/pairing_client.hpp"
#include "lantern/win.hpp"
#include <iostream>
#include <sddl.h>
#include <thread>
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
                     const std::string& pin, const std::string& token, const fs::path& directory,
                     const std::string& pairing_link) {
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
    auto data = pin + "\n" + token + "\n" + utf8(directory.wstring()) + "\n" + pairing_link + "\n";
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
static void old_pairing_response(const fs::path& state, const std::string& pin) {
    Identity identity(state / L"identity.dpapi", false);
    SOCKET listener = listen_socket("127.0.0.1", 0);
    sockaddr_in address{};
    int address_length = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &address_length)) {
        closesocket(listener);
        throw std::runtime_error("legacy fixture address");
    }
    std::exception_ptr failure;
    std::thread server([&] {
        try {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(listener, &readable);
            timeval timeout{5, 0};
            if (select(0, &readable, nullptr, nullptr, &timeout) <= 0)
                throw std::runtime_error("legacy fixture timeout");
            Tls tls(accept(listener, nullptr, nullptr), &identity);
            auto request = tls.receive();
            Reader r{request};
            if (r.u16() != 1 || r.u16() != uint16_t(Op::pair))
                throw std::runtime_error("legacy fixture operation");
            Writer response;
            response.u16(1);
            response.u16(uint16_t(Op::hello));
            response.u32(uint32_t(Code::unsupported));
            response.str("Protocol version or HELLO required");
            tls.send(response.b);
        } catch (...) {
            failure = std::current_exception();
        }
    });
    bool old = false;
    try {
        PairingLink link;
        link.host_name = "Legacy fixture";
        link.addresses = {"127.0.0.1"};
        link.port = ntohs(address.sin_port);
        link.pin = pin;
        link.code = std::string(64, 'a');
        link.expires_at = pairing_unix_time() + 600;
        pair_with_link(link, "Legacy phone");
    } catch (const PairingError& error) {
        old = error.reason == PairError::host_too_old;
    }
    server.join();
    closesocket(listener);
    if (failure)
        std::rethrow_exception(failure);
    check(old, "legacy HELLO unsupported response maps to host-too-old over real TLS");
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
        old_pairing_response(state, pin);
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
            HostAdministration admin(state);
            auto invite = admin.invite("TLS invite", files_read);
            auto uri = pairing_uri(invite, {"127.0.0.2", "127.0.0.1"}, 47449);
            auto invite_link = parse_pairing_uri(uri);
            check(bool(invite_link), "TLS invitation link parsed");
            auto paired = pair_with_link(*invite_link, "TLS phone");
            check(paired.host == "127.0.0.1" && paired.scopes == files_read && valid_sha(paired.token),
                  "real pinned TLS PAIR with address failover");
            {
                Tls tls(connect_socket("127.0.0.1", 47449), nullptr, pin);
                Writer request;
                request.u16(2);
                request.u16(uint16_t(Op::pair));
                request.str("unused");
                request.str("Version");
                tls.send(request.b);
                auto bytes = tls.receive();
                Reader response{bytes};
                check(response.u16() == 1 && response.u16() == uint16_t(Op::pair) &&
                          response.u32() == uint32_t(Code::unsupported),
                      "PAIR unsupported version gets structured response");
            }
            Client accepted(paired.host, paired.port, paired.pin, paired.token, files_read);
            check(!accepted.list("").empty(), "paired token accepted by unchanged HELLO");
            error(
                Code::unauthorized, [&] { pair_with_link(*invite_link, "Replay"); },
                "TLS invitation replay rejected");
            auto bad_invite = admin.invite("Bad TLS invite", 1);
            auto wrong = *parse_pairing_uri(pairing_uri(bad_invite, {"127.0.0.1"}, 47449));
            wrong.code.back() = wrong.code.back() == '0' ? '1' : '0';
            error(
                Code::unauthorized, [&] { pair_with_link(wrong, "Bad"); },
                "wrong invitation denied over TLS");
            auto pin_link = wrong;
            pin_link.pin = std::string(64, '0');
            try {
                pair_with_link(pin_link, "Pin");
                check(false, "pair pin mismatch missing");
            } catch (const PairingError& e) {
                check(e.reason == PairError::pin_mismatch,
                      "pair pin mismatch distinguished before sending code");
            }
            auto address_link = wrong;
            address_link.port = 1;
            try {
                pair_with_link(address_link, "Offline");
                check(false, "pair unreachable missing");
            } catch (const PairingError& e) {
                check(e.reason == PairError::unreachable, "all pair addresses unreachable distinguished");
            }
            auto before = load_config(state / L"config.dpapi").devices.size();
            // Ten PAIR frames per rolling minute; successful and unsuccessful attempts both count.
            bool busy = false;
            for (unsigned i = 0; i < 12; ++i) {
                try {
                    pair_with_link(wrong, "Limited");
                } catch (const PairingError& e) {
                    if (e.reason == PairError::busy) {
                        busy = true;
                        break;
                    }
                }
            }
            check(busy && load_config(state / L"config.dpapi").devices.size() == before,
                  "IP pairing rate limit returns busy without creating devices");
            Client legacy("127.0.0.1", 47449, pin, token, 0);
            auto caps_bytes = legacy.call(Op::capabilities);
            Reader caps_reader{caps_bytes};
            check(caps_reader.str().find("pair=qr-v1") != std::string::npos,
                  "old HELLO unaffected by pair throttling and advertises QR capability");
            auto firewall = firewall_state(fs::path(argv[0]), 47449);
            check(firewall == FirewallState::allowed || firewall == FirewallState::not_allowed ||
                      firewall == FirewallState::unknown,
                  "nonadmin firewall query returns a tri-state without mutation");
            check(firewall_state(fs::path(argv[0]), 0) == FirewallState::unknown,
                  "invalid firewall query cannot report allowed");
            check(!request_firewall_allow(testroot / L"missing.exe", 47449),
                  "invalid firewall request does not launch elevation");
            for (auto& a : lan_addresses())
                check(valid_pairing_address(a.address) && !a.address.starts_with("127.") &&
                          !a.address.starts_with("169.254."),
                      "LAN enumeration excludes unusable addresses");
        }
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
            if (argc == 4) {
                Host pairing_host(state, "127.0.0.1", 47448);
                std::thread pairing_thread([&] { pairing_host.run(); });
                Stop pairing_stop{pairing_host, pairing_thread};
                auto invitation = HostAdministration(state).invite("Java phone", 1);
                auto pairing_link = pairing_uri(invitation, {"127.0.0.2", "127.0.0.1"}, 47448);
                check(run_java(argv[1], argv[2], argv[3], pin, token, client, pairing_link),
                      "Java/JNI cross-client interoperability and QR pairing");
            }
            auto new_token = random_token();
            save_config(state / L"config.dpapi", {root, new_token, 15});
            // Revocation may be observed by the request handler or by the asynchronous socket auditor.
            bool revoked = false;
            try {
                c.list("");
            } catch (const TransportError&) {
                revoked = true;
            } catch (const Error& e) {
                revoked = e.code == Code::unauthorized;
            }
            check(revoked, "revoke active trust rejects request or closes the old connection");
            error(
                Code::unauthorized, [&] { Client old("127.0.0.1", 47449, pin, token); },
                "revoked token cannot establish a fresh HELLO");
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
