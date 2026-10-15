#include "lantern/host_admin.hpp"
#include <fstream>
#include <iostream>
#include <thread>
using namespace lantern;
static int checks = 0;
static void check(bool v, const char* name) {
    if (!v)
        throw std::runtime_error(name);
    ++checks;
    std::cout << name << std::endl;
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
template <class F> static void until(F f, const char* name) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!f()) {
        if (std::chrono::steady_clock::now() > end)
            throw std::runtime_error(name);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
struct Server {
    Host host;
    std::thread thread;
    Server(fs::path p) : host(p, "127.0.0.1", 47447), thread([&] { host.run(); }) {}
    ~Server() {
        host.stop();
        if (thread.joinable())
            thread.join();
    }
};
static bool disconnected(Client& c) {
    try {
        c.list("");
        return false;
    } catch (const std::exception&) {
        return true;
    }
}
static void invitation_checks(const fs::path& root, const fs::path& share) {
    auto state = root / L"invite-state";
    HostAdministration::initialize(state, share, 31);
    HostAdministration admin(state);
    auto previous = load_config(state / L"config.dpapi");
    Writer v2;
    v2.u16(2);
    v2.str(utf8(previous.root.wstring()));
    v2.str(previous.token);
    v2.u32(previous.scopes);
    v2.u32(previous.legacy_enabled ? 1 : 0);
    v2.u64(previous.legacy_epoch);
    v2.u32(0);
    write_file(state / L"config.dpapi", protect(v2.b));
    auto invite = admin.invite("生成邀请", 3);
    auto migrated = load_config(state / L"config.dpapi");
    check(migrated.token == previous.token && migrated.root == previous.root &&
              migrated.scopes == previous.scopes && migrated.legacy_enabled == previous.legacy_enabled,
          "v2 invitation migration preserves legacy configuration");
    check(invite.code.size() == 64 && invite.id == invite.code.substr(0, 32) &&
              invite.expires_at > pairing_unix_time(),
          "invitation generated with default TTL");
    auto pending = admin.invites();
    check(pending.size() == 1 && pending[0].scopes == 3 && pending[0].label == "生成邀请",
          "pending metadata contains no code");
    auto clear = unprotect(read_file(state / L"config.dpapi"));
    auto contains = [&](const std::string& s) {
        return std::search(clear.begin(), clear.end(), s.begin(), s.end()) != clear.end();
    };
    check(!contains(invite.code) && !contains(invite.code.substr(32)),
          "invitation plaintext absent even after DPAPI decryption");
    auto cfg = load_config(state / L"config.dpapi");
    check(contains(cfg.invites[0].code_hash), "config stores SHA256 digest only");
    auto device = admin.redeem_invite(invite.code, "Android fixture");
    check(device.scopes == 3 && valid_sha(device.id) && valid_sha(device.token) && admin.invites().empty(),
          "invitation consumed with distinct device credential");
    expect(Code::unauthorized, [&] { admin.redeem_invite(invite.code, "Again"); }, "invitation single use");
    auto expired = admin.invite("Expires", 1);
    edit_config(state / L"config.dpapi",
                [&](Config& c) { c.invites.back().expires_at = pairing_unix_time() - 1; });
    expect(
        Code::unauthorized, [&] { admin.redeem_invite(expired.code, "Expired"); },
        "expired invitation rejected");
    check(admin.invites().empty(), "expired invitation cleaned on access");
    auto wrong = admin.invite("Wrong attempts", 1);
    auto guessed = wrong.code;
    guessed.back() = guessed.back() == '0' ? '1' : '0';
    for (unsigned i = 0; i < 5; ++i)
        expect(
            Code::unauthorized, [&] { admin.redeem_invite(guessed, "Wrong"); },
            "incorrect invitation attempt rejected");
    expect(
        Code::unauthorized, [&] { admin.redeem_invite(wrong.code, "Now correct"); },
        "five wrong secrets invalidate selected invitation");
    check(admin.invites().empty(), "invalidated invitation cleaned");
    auto canceled = admin.invite("Canceled", 1);
    admin.cancel_invite(canceled.id);
    admin.cancel_invite(canceled.id);
    expect(
        Code::unauthorized, [&] { admin.redeem_invite(canceled.code, "Canceled"); },
        "cancel invitation is effective and idempotent");
    auto reduced = admin.invite("Reduced", 3);
    admin.ceiling(1);
    check(admin.redeem_invite(reduced.code, "Least privilege").scopes == 1,
          "redeem applies current ceiling intersection");
    expect(
        Code::unauthorized, [&] { admin.invite("Over ceiling", 2); },
        "invitation cannot exceed host ceiling");
    expect(
        Code::invalid, [&] { admin.invite("Bad TTL", 1, std::chrono::seconds(0)); },
        "nonpositive invitation TTL rejected");
    auto race = admin.invite("Concurrent", 1);
    std::atomic_uint succeeded = 0;
    std::thread first([&] {
        try {
            admin.redeem_invite(race.code, "First");
            ++succeeded;
        } catch (const Error&) {
        }
    });
    std::thread second([&] {
        try {
            HostAdministration(state).redeem_invite(race.code, "Second");
            ++succeeded;
        } catch (const Error&) {
        }
    });
    first.join();
    second.join();
    check(succeeded == 1, "atomic invitation redemption accepts exactly one concurrent client");
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc < 2)
            return 2;
        if (std::wstring(argv[1]) == L"--hold") {
            fs::path root = argv[2], out = argv[3];
            auto cfg = load_config(root / L"state/config.dpapi");
            Identity identity(root / L"state/identity.dpapi", false);
            Client a("127.0.0.1", 47447, identity.fingerprint(), cfg.devices[0].token, 1),
                b("127.0.0.1", 47447, identity.fingerprint(), cfg.devices[1].token, 1);
            write_file(out / L"ready", Bytes{1});
            bool ca = false, cb = false;
            while (!fs::exists(out / L"stop")) {
                if (!ca && disconnected(a)) {
                    ca = true;
                    write_file(out / L"closed-a", Bytes{1});
                }
                if (!cb && disconnected(b)) {
                    cb = true;
                    write_file(out / L"closed-b", Bytes{1});
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            return 0;
        }
        auto root = fs::absolute(argv[1]) / wide(random_token().substr(0, 12)), state = root / L"state",
             share = root / L"share";
        fs::create_directories(state);
        fs::create_directories(share);
        write_file(share / L"readme.txt", Bytes{'o', 'k'});
        invitation_checks(root, share);
        auto fresh = root / L"wizard";
        HostAdministration::initialize(fresh, share, 1);
        HostAdministration wizard(fresh);
        check(wizard.snapshot().root == fs::weakly_canonical(share) && !wizard.snapshot().legacy &&
                  wizard.snapshot().devices.empty(),
              "first configuration commits protected identity without legacy access");
        auto fingerprint = wizard.snapshot().pin;
        expect(
            Code::conflict, [&] { HostAdministration::initialize(fresh, share, 31); },
            "first setup never replaces existing identity");
        check(wizard.snapshot().pin == fingerprint, "existing identity preserved");
        expect(
            Code::unauthorized, [&] { HostAdministration::initialize(share / L"identity", share, 1); },
            "identity cannot be shared");
        expect(
            Code::invalid, [&] { HostAdministration::initialize(root / L"zero", share, 0); },
            "first setup empty scopes rejected");
        {
            auto other = root / L"other-share";
            fs::create_directories(other);
            wizard.root(other);
            check(wizard.snapshot().root == fs::weakly_canonical(other) && wizard.snapshot().pin == fingerprint,
                  "authorized root changes in place and keeps identity");
            expect(
                Code::unauthorized, [&] { wizard.root(root); }, "root containing identity rejected");
            expect(
                Code::not_found, [&] { wizard.root(root / L"missing"); }, "missing root rejected");
            check(wizard.snapshot().root == fs::weakly_canonical(other), "rejected root change leaves config");
            wizard.root(share);
            check(wizard.snapshot().root == fs::weakly_canonical(share), "root restored");
        }
        bool ipv6_tested = false, ipv6_policy_blocked = false;
        {
            Host ipv6(fresh, "::1", 47449);
            std::thread thread([&] { ipv6.run(); });
            try {
                auto pair = wizard.add("IPv6 fixture", 1);
                Client v6("::1", 47449, pair.pin, pair.secret, 1);
                check(v6.list("").size() == 1, "real IPv6 pinned TLS file access");
                ipv6_tested = true;
            } catch (const TransportError& error) {
                if (error.native_code == WSAEACCES) {
                    ipv6_policy_blocked = true;
                    std::cout << "SKIP IPv6 transport: OS denied ::1 connect with Windows 10013; parser "
                                 "tests still execute"
                              << std::endl;
                } else {
                    ipv6.stop();
                    thread.join();
                    throw;
                }
            } catch (...) {
                ipv6.stop();
                thread.join();
                throw;
            }
            ipv6.stop();
            thread.join();
        }
        Identity identity(state / L"identity.dpapi", true);
        Config cfg{share, random_token(), 31};
        save_config(state / L"config.dpapi", cfg);
        HostAdministration admin(state);
        check(admin.snapshot().pin == identity.fingerprint() && admin.snapshot().devices.empty(),
              "load local state without starting listener");
        auto a = admin.add("Device A", 3), b = admin.add("Device B", 1);
        check(a.secret != b.secret && a.device.id != b.device.id, "distinct per-device credentials");
        check(admin.pairing(a.device.id).secret == a.secret, "explicit pairing lookup");
        auto encrypted = read_file(state / L"config.dpapi");
        check(std::search(encrypted.begin(), encrypted.end(), a.secret.begin(), a.secret.end()) ==
                  encrypted.end(),
              "no plaintext credential in config");
        expect(Code::invalid, [&] { admin.add("", 1); }, "empty label rejected");
        expect(Code::invalid, [&] { admin.add("injected\nlabel", 1); }, "multiline label rejected");
        admin.ceiling(1);
        auto limited = admin.snapshot();
        check(limited.devices[0].scopes == 3 && limited.devices[0].effective_scopes == 1,
              "ceiling changes effective grant without erasing configured scopes");
        expect(Code::unauthorized, [&] { admin.grant(a.device.id, 3); }, "grant cannot exceed ceiling");
        expect(Code::unauthorized, [&] { admin.add("Invalid", 2); }, "create cannot exceed ceiling");
        check(admin.snapshot().devices.size() == 2, "failed create leaves devices unchanged");
        admin.ceiling(31);
        check(admin.snapshot().devices[0].effective_scopes == 3,
              "restoring ceiling restores configured grant");
        expect(Code::not_found, [&] { admin.remove(random_token()); }, "missing device remove rejected");
        auto original = load_config(state / L"config.dpapi");
        {
            Server server(state);
            Client first("127.0.0.1", 47447, identity.fingerprint(), a.secret, 1, 0),
                second("127.0.0.1", 47447, identity.fingerprint(), a.secret, 1, 2),
                other("127.0.0.1", 47447, identity.fingerprint(), b.secret, 1, 0);
            until([&] { return server.host.sessions().size() == 3; }, "session registration");
            auto all = server.host.sessions();
            check(all.size() == 3, "three actual logical connections listed");
            auto entry = std::find_if(all.begin(), all.end(), [](auto& s) { return s.channel == 2; });
            check(entry != all.end() && entry->device_id == a.device.id && entry->label == "Device A" &&
                      entry->address == "127.0.0.1" && entry->scopes == 1,
                  "session metadata identifies device address channel and scope");
            check(server.host.end_session(entry->id), "local exact-connection termination accepted");
            until([&] { return disconnected(second); }, "selected connection shutdown");
            check(first.list("").size() == 1 && other.list("").size() == 1,
                  "ending one connection preserves same-device sibling and other device");
            check(!server.host.end_session(random_token()), "unknown connection does not terminate another");
            admin.end_device(a.device.id);
            until([&] { return disconnected(first); }, "whole-device epoch shutdown");
            check(other.list("").size() == 1, "whole-device end preserves other device");
            expect(
                Code::cancelled,
                [&] { Client automatic("127.0.0.1", 47447, identity.fingerprint(), a.secret, 1, UINT32_MAX, 1); },
                "ended device: automatic reconnect refused (session=end-v1)");
            Client again("127.0.0.1", 47447, identity.fingerprint(), a.secret, 1);
            check(again.list("").size() == 1, "ended device retains deliberate reconnect");
            Client resumed("127.0.0.1", 47447, identity.fingerprint(), a.secret, 1, UINT32_MAX, 1);
            check(resumed.list("").size() == 1, "deliberate reconnect clears the hold for automatic ones");
            Client plain("127.0.0.1", 47447, identity.fingerprint(), b.secret, 1, UINT32_MAX, 1);
            check(plain.list("").size() == 1, "automatic flag alone never blocks a device that was not ended");
            admin.grant(a.device.id, 0);
            until([&] { return disconnected(again); }, "zero permission ends active requested scopes");
            expect(
                Code::unauthorized,
                [&] { Client denied("127.0.0.1", 47447, identity.fingerprint(), a.secret, 1); },
                "zero permission blocks file request");
            admin.remove(a.device.id);
            expect(
                Code::unauthorized,
                [&] { Client revoked("127.0.0.1", 47447, identity.fingerprint(), a.secret, 0); },
                "revoke blocks zero-scope authentication too");
            check(other.list("").size() == 1, "revocation isolated from second device");
            admin.disable_legacy();
            expect(
                Code::unauthorized,
                [&] { Client legacy("127.0.0.1", 47447, identity.fingerprint(), original.token, 0); },
                "legacy disabled independently");
            check(other.list("").size() == 1, "named trust survives disabling legacy");
        }
        save_config(state / L"config.dpapi", original);
        std::ofstream report(root / L"host-admin.json");
        report << "{\"success\":true,\"kind\":\"measured\",\"ipv6_tls_tested\":"
               << (ipv6_tested ? "true" : "false")
               << ",\"ipv6_os_access_denied_10013\":" << (ipv6_policy_blocked ? "true" : "false")
               << ",\"checks\":" << checks
               << ",\"transport\":\"same-PC Schannel TLS "
                  "loopback\",\"real_sessions_tested\":true,\"credentials_logged\":false}";
        std::cout << "Fixture: " << utf8(root.wstring()) << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
