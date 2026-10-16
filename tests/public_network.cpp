#include "lantern/host_admin.hpp"
#include "lantern/lan.hpp"
#include "lantern/windows_client.hpp"
#include <iostream>
#include <thread>
#include <ws2tcpip.h>
using namespace lantern;
namespace {
unsigned checks = 0;
void check(bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
    ++checks;
    std::cout << message << std::endl;
}
template <class F> void until(F fn, const char* message, int seconds = 3) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!fn()) {
        if (std::chrono::steady_clock::now() > end)
            throw std::runtime_error(message);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
template <class F> void error(Code code, F fn, const char* message) {
    bool caught = false;
    try {
        fn();
    } catch (const Error& e) {
        caught = e.code == code;
    }
    check(caught, message);
}
struct Server {
    Host host;
    std::thread thread;
    explicit Server(const fs::path& state) : host(state, "::", 0), thread([&] { host.run(); }) {}
    ~Server() {
        host.stop();
        thread.join();
    }
};
struct StallServer {
    SOCKET listener = listen_socket("127.0.0.1", 0);
    std::atomic<SOCKET> accepted = INVALID_SOCKET;
    std::atomic_bool stopped = false;
    uint16_t port = 0;
    std::thread thread;
    StallServer() {
        sockaddr_in a{};
        int size = sizeof(a);
        getsockname(listener, reinterpret_cast<sockaddr*>(&a), &size);
        port = ntohs(a.sin_port);
        thread = std::thread([&] {
            accepted = accept(listener, nullptr, nullptr);
            while (!stopped)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            auto s = accepted.exchange(INVALID_SOCKET);
            if (s != INVALID_SOCKET)
                closesocket(s);
        });
    }
    ~StallServer() {
        stopped = true;
        shutdown(listener, SD_BOTH);
        closesocket(listener);
        thread.join();
    }
};
void models() {
    for (auto [ip, kind] :
         std::vector<std::pair<std::string, AddressKind>>{{"192.168.1.2", AddressKind::private_v4},
                                                          {"172.31.0.1", AddressKind::private_v4},
                                                          {"10.1.1.1", AddressKind::private_v4},
                                                          {"100.64.0.1", AddressKind::cgnat_v4},
                                                          {"100.127.255.254", AddressKind::cgnat_v4},
                                                          {"100.128.0.1", AddressKind::public_v4},
                                                          {"192.0.2.1", AddressKind::public_v4},
                                                          {"2001:db8::1", AddressKind::global_v6},
                                                          {"fd00::1", AddressKind::ula_v6},
                                                          {"fc00::1", AddressKind::ula_v6},
                                                          {"fe80::1", AddressKind::link_local},
                                                          {"169.254.1.1", AddressKind::link_local},
                                                          {"::1", AddressKind::loopback},
                                                          {"::", AddressKind::invalid},
                                                          {"ff02::1", AddressKind::multicast},
                                                          {"::ffff:192.168.1.2", AddressKind::private_v4}})
        check(classify_address(ip) == kind, "address category");
    check(normalize_address("::FFFF:c0a8:102") == "192.168.1.2", "mapped IPv4 normalization");
    check(source_key("2001:db8:1:2::1") == source_key("2001:db8:1:2:ffff::1"), "IPv6 /64 grouping");
    check(source_key("2001:db8:1:2::1") != source_key("2001:db8:1:3::1"), "different IPv6 /64 isolated");
    check(source_key("::ffff:192.0.2.1") == source_key("192.0.2.1"), "mapped source rate key");
    check(local_pairing_source("::1") && local_pairing_source("fd00::1") && local_pairing_source("fe80::1") &&
              !local_pairing_source("2001:db8::1") && !local_pairing_source("100.64.1.1"),
          "local invitation source policy");
    check(temporary_ipv6(true, 604800, 86400) && !temporary_ipv6(false, 604800, 86400) &&
              !temporary_ipv6(true, UINT32_MAX, UINT32_MAX),
          "conservative random suffix lifetime heuristic");
    AuthenticationGuard guard;
    for (int i = 0; i < 4; i++)
        check(guard.acquire("2001:db8::1", 0), "four unauthenticated admissions");
    check(!guard.acquire("2001:db8::2", 0), "rotating IPv6 IID cannot evade source cap");
    guard.release("2001:db8::1");
    check(guard.acquire("2001:db8::3", 0), "admission slot released");
    guard.failed("192.0.2.1", 1000);
    check(!guard.allowed("192.0.2.1", 1249) && guard.allowed("192.0.2.1", 1250), "first auth backoff 250ms");
    guard.failed("192.0.2.1", 1250);
    check(!guard.allowed("192.0.2.1", 1749) && guard.allowed("192.0.2.1", 1750), "incremental auth backoff");
    guard.success("192.0.2.1");
    check(guard.allowed("192.0.2.1", 1251), "auth success resets failures");
    guard.timeout("192.0.2.1");
    check(guard.stats().timeouts == 1 && guard.stats().throttled >= 3, "sanitized security counters");
    std::vector<HostAddress> addresses;
    for (auto ip : {"10.1.1.1", "fd00::1", "2001:db8::1", "192.0.2.1", "100.64.1.1"}) {
        HostAddress a;
        a.address = ip;
        a.category = classify_address(ip);
        a.valid_lifetime = 3600;
        addresses.push_back(a);
    }
    Config config;
    auto endpoints = host_endpoints(config, 47443, addresses);
    check(endpoints.size() == 2 && endpoints[0].kind == EndpointKind::lan_v4 &&
              endpoints[1].kind == EndpointKind::lan_v6,
          "Internet off advertises LAN only");
    config.internet_access = true;
    config.public_hostname = "example.com";
    config.external_port = 443;
    endpoints = host_endpoints(config, 47443, addresses);
    check(endpoints.size() == 5 && endpoints[2].kind == EndpointKind::public_v6 &&
              endpoints[3].kind == EndpointKind::hostname && endpoints[4].kind == EndpointKind::public_v4 &&
              endpoints[3].port == 443,
          "ordered Internet endpoints and translated port");
    addresses[2].temporary = true;
    check(host_endpoints(config, 47443, addresses).size() == 4, "temporary IPv6 never advertised");
    std::vector<ConnectionEndpoint> saved;
    for (int i = 1; i <= 12; i++)
        saved.push_back(connection_endpoint("192.0.2." + std::to_string(i), 47443));
    saved.push_back(connection_endpoint("example.com", 443));
    saved[0].failures = 3;
    saved[1].expires_at = 1;
    auto merged = merge_endpoints(saved, {saved[2]}, pairing_unix_time());
    check(merged.size() == 8 && std::any_of(merged.begin(), merged.end(),
                                            [](auto& e) { return e.kind == EndpointKind::hostname; }),
          "merge deduplicates trims expiry failures and retains DDNS");
    Writer w;
    write_endpoints(w, merged);
    Reader r{w.b};
    check(read_endpoints(r).size() == 8, "endpoint wire roundtrip");
    r.end();
}
void legacy_configs(const fs::path& state, const fs::path& share) {
    for (unsigned version = 1; version <= 3; ++version) {
        Writer w;
        w.u16(uint16_t(version));
        w.str(utf8(share.wstring()));
        w.str(std::string(64, 'a'));
        w.u32(1);
        if (version >= 2) {
            w.u32(1);
            w.u64(0);
            w.u32(0);
        }
        if (version >= 3) {
            w.u32(1);
            w.str(std::string(32, 'b'));
            w.str("v3 invitation");
            w.str(std::string(64, 'c'));
            w.u32(1);
            w.u64(pairing_unix_time() + 600);
            w.u32(0);
        }
        auto file = state / L"old.dpapi";
        write_file(file, protect(w.b));
        auto c = load_config(file);
        check(!c.internet_access && c.public_hostname.empty() && c.external_port == 0,
              "old config defaults to Internet off");
        if (version == 3)
            check(c.invites.size() == 1 && !c.invites[0].allow_remote,
                  "v3 invitation migrates to local-only redemption");
    }
}
void old_host_fallback(const fs::path& state, const DevicePairing& pairing) {
    Identity identity(state / L"identity.dpapi", false);
    auto listener = listen_socket("127.0.0.1", 0);
    sockaddr_in bound{};
    int size = sizeof(bound);
    getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &size);
    std::atomic_bool requested_endpoints = false, failed = false;
    std::jthread worker([&] {
        try {
            auto s = accept(listener, nullptr, nullptr);
            Tls tls(s, &identity, "", std::chrono::steady_clock::now() + std::chrono::seconds(3));
            auto bytes = tls.receive();
            Reader hello{bytes};
            if (hello.u16() != 1 || hello.u16() != 1)
                throw std::runtime_error("mock HELLO");
            Writer reply;
            reply.u16(1);
            reply.u16(1);
            reply.u32(0);
            reply.u32(0);
            tls.send(reply.b);
            bytes = tls.receive();
            Reader caps{bytes};
            if (caps.u16() != 1 || caps.u16() != 30)
                throw std::runtime_error("mock CAPS");
            Writer answer;
            answer.u16(1);
            answer.u16(30);
            answer.u32(0);
            answer.str("Lantern/1;tls=1.2");
            tls.send(answer.b);
            try {
                bytes = tls.receive();
                Reader extra{bytes};
                extra.u16();
                requested_endpoints = extra.u16() == 56;
            } catch (const Error&) {
            }
        } catch (...) {
            failed = true;
        }
    });
    try {
        {
            Client client("127.0.0.1", ntohs(bound.sin_port), pairing.pin, pairing.secret, 0, 0);
            check(client_endpoints(client).empty(),
                  "old host without endpoint capability returns single-address fallback");
        }
    } catch (...) {
        closesocket(listener);
        throw;
    }
    worker.join();
    closesocket(listener);
    check(!failed && !requested_endpoints, "old host never receives unsupported Op56");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    try {
        models();
        fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / L"lantern-public-tests";
        root /= wide(random_token().substr(0, 12));
        auto share = root / L"share", state = root / L"state";
        fs::create_directories(share);
        write_file(share / L"test.txt", Bytes{'o', 'k'});
        HostAdministration::initialize(state, share, 31);
        HostAdministration admin(state);
        legacy_configs(state, share);
        edit_config(state / L"config.dpapi", [](Config& c) { c.legacy_enabled = true; });
        bool gate = false;
        try {
            admin.internet(true);
        } catch (const InternetAccessError& e) {
            gate = e.reason == InternetError::legacy_enabled;
        }
        check(gate && !admin.network_settings().internet_access,
              "legacy shared credential prevents Internet enable");
        auto pairing = admin.add("network fixture", 1);
        old_host_fallback(state, pairing);
        admin.disable_legacy();
        admin.internet(true, "example.com", 47443);
        auto invite = admin.invite("local", 1);
        for (int i = 0; i < 5; i++)
            error(
                Code::unauthorized, [&] { admin.redeem_invite(invite.code, "remote", false); },
                "public invitation failure counted");
        check(admin.invites().empty(), "five remote attempts invalidate local invitation");
        auto remote = admin.invite("remote", 1, std::chrono::seconds(600), true);
        check(admin.redeem_invite(remote.code, "remote", false).scopes == 1,
              "explicit remote invitation can be redeemed");
        Server server(state);
        auto port = server.host.listening_port();
        check(server.host.listening_address() == "::", "actual dual stack listener reported");
        auto ipv6_only = listen_socket("::1", 0);
        sockaddr_in6 reserved{};
        int reserved_size = sizeof(reserved);
        getsockname(ipv6_only, reinterpret_cast<sockaddr*>(&reserved), &reserved_size);
        try {
            Host fallback(state, "::", ntohs(reserved.sin6_port));
            check(fallback.listening_address() == "0.0.0.0",
                  "IPv6 bind failure falls back to IPv4 and reports actual bind");
        } catch (...) {
            closesocket(ipv6_only);
            throw;
        }
        closesocket(ipv6_only);
        {
            Client ipv6("::1", port, pairing.pin, pairing.secret, 1);
            check(ipv6.list("").size() == 1, "mandatory real IPv6 pinned TLS");
        }
        Client ipv4("127.0.0.1", port, pairing.pin, pairing.secret, 1);
        check(ipv4.list("").size() == 1, "dual stack accepts IPv4");
        until(
            [&] {
                auto sessions = server.host.sessions();
                return std::any_of(sessions.begin(), sessions.end(),
                                   [](auto& s) { return s.address == "127.0.0.1"; });
            },
            "normalized IPv4 session source missing");
        check(true, "mapped IPv4 session source normalized");
        {
            Tls tls(connect_socket("::1", port), nullptr, pairing.pin);
            Writer w;
            w.u16(1);
            w.u16(56);
            tls.send(w.b);
            auto b = tls.receive();
            Reader r{b};
            r.u16();
            r.u16();
            check(Code(r.u32()) == Code::unauthorized, "endpoint enumeration denied before HELLO");
        }
        auto advertised = client_endpoints(ipv4);
        check(std::any_of(advertised.begin(), advertised.end(),
                          [](auto& e) { return e.kind == EndpointKind::hostname; }),
              "authenticated endpoint enumeration");
        admin.internet(false);
        auto local = client_endpoints(ipv4);
        check(std::none_of(local.begin(), local.end(), [](auto& e) { return is_public(e.kind); }),
              "live Internet disable hides all public endpoints");
        admin.internet(true);
        std::vector<ConnectionEndpoint> endpoints{connection_endpoint("192.0.2.1", port),
                                                  connection_endpoint("2001:db8::1", port),
                                                  connection_endpoint("::1", port)};
        endpoints[0].priority = 0;
        endpoints[1].priority = 1;
        endpoints[2].priority = 2;
        auto begin = std::chrono::steady_clock::now();
        auto connected = connect_endpoints(endpoints, pairing.pin, std::chrono::seconds(3));
        auto elapsed = std::chrono::steady_clock::now() - begin;
        check(!connected.public_path && connected.selected.host == normalize_address("::1") &&
                  elapsed < std::chrono::seconds(3),
              "LAN route preferred to unreachable documentation public addresses");
        connected.tls.reset();
        {
            StallServer slow1, slow2;
            auto one = connection_endpoint("127.0.0.1", slow1.port),
                 two = connection_endpoint("127.0.0.1", slow2.port), three = connection_endpoint("::1", port);
            one.priority = 0;
            two.priority = 1;
            three.priority = 2;
            auto race = connect_endpoints({one, two, three}, pairing.pin, std::chrono::seconds(3));
            auto winner = std::find_if(race.attempts.begin(), race.attempts.end(),
                                       [](auto& p) { return p.failure == EndpointFailure::none; });
            check(winner != race.attempts.end() && winner->start_ms >= 450 && winner->start_ms < 1500,
                  "third endpoint starts approximately 500ms later and cancels slow TLS losers");
        }
        bool mismatch = false;
        try {
            connect_endpoints({connection_endpoint("::1", port)}, std::string(64, '0'),
                              std::chrono::seconds(2));
        } catch (const EndpointConnectError& e) {
            mismatch = e.reason == EndpointFailure::pin_mismatch;
        }
        check(mismatch, "pin mismatch fails directly without HELLO");
        begin = std::chrono::steady_clock::now();
        try {
            connect_endpoints({connection_endpoint("192.0.2.1", port)}, pairing.pin,
                              std::chrono::milliseconds(350));
        } catch (const EndpointConnectError&) {
        }
        check(std::chrono::steady_clock::now() - begin < std::chrono::seconds(1),
              "unreachable endpoint respects global deadline");
        bool dns_reported = false;
        try {
            connect_endpoints({connection_endpoint("missing.invalid", port)}, pairing.pin,
                              std::chrono::milliseconds(500));
        } catch (const EndpointConnectError& e) {
            dns_reported = std::any_of(e.attempts.begin(), e.attempts.end(), [](auto& p) {
                return p.failure == EndpointFailure::dns || p.failure == EndpointFailure::timeout;
            });
        }
        check(dns_reported, "DNS failure or resolver timeout is reported per endpoint");
        ClientCredentials credentials{"127.0.0.1", pairing.pin, pairing.secret, port, endpoints};
        save_credentials(root / L"client.dpapi", credentials);
        auto restored = load_credentials(root / L"client.dpapi");
        check(restored.endpoints.size() == 3 && restored.token == credentials.token,
              "DPAPI persists endpoint list");
        Writer old;
        old.u32(1);
        old.str(credentials.host);
        old.u32(port);
        old.str(credentials.pin);
        old.str(credentials.token);
        write_file(root / L"old-client.dpapi", protect(old.b));
        check(load_credentials(root / L"old-client.dpapi").endpoints.empty(),
              "old DPAPI single address file accepted");
        ClientCredentials probe_credentials = credentials;
        probe_credentials.endpoints = {connection_endpoint("127.0.0.1", port)};
        auto probes = probe_endpoints(probe_credentials, std::chrono::seconds(2));
        check(probes.size() == 1 && probes[0].tls_ok && probes[0].rtt_ms.has_value(),
              "authenticated probe returns ping RTT");
        Tls first_message_stall(connect_socket("127.0.0.1", port), nullptr, pairing.pin);
        std::vector<SOCKET> slow;
        for (int i = 0; i < 12; i++)
            slow.push_back(connect_socket("::1", port));
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        {
            Client legitimate("127.0.0.1", port, pairing.pin, pairing.secret, 1);
            check(legitimate.list("").size() == 1,
                  "slow unauthenticated source cannot starve legitimate client");
        }
        check(ipv4.list("").size() == 1, "authenticated session remains available under handshake flood");
        until([&] { return server.host.security_stats().timeouts >= 5; },
              "10 second TLS and first message deadline not enforced", 12);
        for (auto s : slow)
            closesocket(s);
        check(server.host.security_stats().throttled >= 8, "per source handshake cap and timeout counters");
        bool closed = false;
        try {
            first_message_stall.receive();
        } catch (const Error&) {
            closed = true;
        }
        check(closed, "TLS success without HELLO still expires at shared deadline");
        error(
            Code::unauthorized, [&] { Client bad("127.0.0.1", port, pairing.pin, std::string(64, '0'), 1); },
            "invalid HELLO rejected");
        error(
            Code::busy, [&] { Client retry("127.0.0.1", port, pairing.pin, pairing.secret, 1); },
            "HELLO source backoff enforced");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        {
            Client good("127.0.0.1", port, pairing.pin, pairing.secret, 1);
        }
        error(
            Code::unauthorized, [&] { Client bad("127.0.0.1", port, pairing.pin, std::string(64, '0'), 1); },
            "successful HELLO cleared prior failure backoff");
        auto report = server.host.reachability_report(argv[0]);
        check(report.dual_stack && report.internet_access && !report.legacy_enabled,
              "local readiness diagnostic snapshot");
        unsigned stable = 0, temporary = 0;
        for (auto& a : report.addresses) {
            check(a.category != AddressKind::loopback && a.category != AddressKind::link_local &&
                      a.category != AddressKind::multicast,
                  "real adapter model excludes ineligible addresses");
            if (a.category == AddressKind::global_v6 && !a.deprecated)
                (a.temporary ? temporary : stable)++;
        }
        std::cout << "Read-only local IPv6 classification: stable=" << stable << " temporary=" << temporary
                  << " (addresses withheld)\n";
        std::cout << checks << " public network checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << std::endl;
        return 1;
    }
}
