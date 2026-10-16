#include "lantern/endpoint_client.hpp"
#include <condition_variable>
#include <set>
#include <thread>
#include <ws2tcpip.h>
namespace lantern {
namespace {
using Clock = std::chrono::steady_clock;
struct SocketTracker {
    std::mutex mutex;
    std::set<SOCKET> sockets;
};
struct Race {
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic_bool cancelled = false;
    std::shared_ptr<SocketTracker> tracked = std::make_shared<SocketTracker>();
    EndpointConnection result;
    bool pin_failed = false;
    size_t finished = 0;
    void cancel() {
        {
            std::lock_guard lock(mutex);
            cancelled = true;
        }
        std::lock_guard lock(tracked->mutex);
        for (auto s : tracked->sockets)
            shutdown(s, SD_BOTH);
        cv.notify_all();
    }
};
struct Lookup : OVERLAPPED {
    std::atomic_uint references = 1;
    std::atomic_int error = WSA_IO_PENDING;
    ADDRINFOEXW* result = nullptr;
    ADDRINFOEXW hints{};
    std::wstring name, service;
    HANDLE complete = nullptr;
    HANDLE cancel = nullptr;
    Lookup() {
        static_cast<OVERLAPPED&>(*this) = {};
        complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!complete)
            win_error("DNS event");
    }
    ~Lookup() {
        if (result)
            FreeAddrInfoExW(result);
        CloseHandle(complete);
    }
    void release() {
        if (references.fetch_sub(1) == 1)
            delete this;
    }
    static void CALLBACK completed(DWORD status, DWORD, OVERLAPPED* overlapped) {
        auto* lookup = static_cast<Lookup*>(overlapped);
        lookup->error = int(status);
        SetEvent(lookup->complete);
        lookup->release();
    }
};
EndpointFailure resolve(Lookup& lookup, const std::string& host, uint16_t port, Clock::time_point deadline,
                        Race& race) {
    auto& hints = lookup.hints;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    lookup.name = wide(host);
    lookup.service = std::to_wstring(port);
    lookup.references.fetch_add(1);
    int status = GetAddrInfoExW(lookup.name.c_str(), lookup.service.c_str(), NS_DNS, nullptr, &hints,
                                &lookup.result, nullptr, &lookup, Lookup::completed, &lookup.cancel);
    if (status == WSA_IO_PENDING) {
        while (WaitForSingleObject(lookup.complete, 10) == WAIT_TIMEOUT) {
            if (race.cancelled || Clock::now() >= deadline) {
                GetAddrInfoExCancel(&lookup.cancel);
                // Completion owns a reference. A slow DNS provider cannot extend the caller deadline.
                return race.cancelled ? EndpointFailure::cancelled : EndpointFailure::timeout;
            }
        }
        status = lookup.error.load();
    } else
        lookup.release(); // No callback is made for synchronous completion.
    return status ? EndpointFailure::dns : EndpointFailure::none;
}
struct SocketSet {
    Race& race;
    std::vector<SOCKET> owned;
    void add(SOCKET s) {
        std::lock_guard lock(race.tracked->mutex);
        owned.push_back(s);
        race.tracked->sockets.insert(s);
    }
    void remove(SOCKET s, bool close) {
        std::lock_guard lock(race.tracked->mutex);
        race.tracked->sockets.erase(s);
        std::erase(owned, s);
        if (close)
            closesocket(s);
    }
    ~SocketSet() {
        for (auto s : owned) {
            std::lock_guard lock(race.tracked->mutex);
            race.tracked->sockets.erase(s);
            closesocket(s);
        }
    }
};
std::unique_ptr<Tls> dial(const ConnectionEndpoint& e, const std::string& pin, Clock::time_point deadline,
                          Race& race, EndpointFailure& failure, bool& tcp_ok) {
    init_sockets();
    std::unique_ptr<Lookup, void (*)(Lookup*)> owner(new Lookup, [](Lookup* lookup) { lookup->release(); });
    auto& lookup = *owner;
    failure = resolve(lookup, e.host, e.port, deadline, race);
    if (failure != EndpointFailure::none)
        return {};
    SocketSet sockets{race};
    std::vector<ADDRINFOEXW*> v4, v6, addresses;
    for (auto* a = lookup.result; a; a = a->ai_next) {
        auto& family = a->ai_family == AF_INET6 ? v6 : v4;
        if (family.size() < 8)
            family.push_back(a);
    }
    for (size_t i = 0; addresses.size() < 8 && (i < v4.size() || i < v6.size()); ++i) {
        if (i < v6.size())
            addresses.push_back(v6[i]);
        if (i < v4.size() && addresses.size() < 8)
            addresses.push_back(v4[i]);
    }
    size_t next = 0;
    auto launch = Clock::now();
    SOCKET selected = INVALID_SOCKET;
    while (!race.cancelled && Clock::now() < deadline) {
        if (next < addresses.size() && Clock::now() >= launch) {
            auto a = addresses[next++];
            launch = Clock::now() + std::chrono::milliseconds(250);
            auto s = socket(a->ai_family, SOCK_STREAM, IPPROTO_TCP);
            if (s != INVALID_SOCKET) {
                sockets.add(s);
                u_long mode = 1;
                ioctlsocket(s, FIONBIO, &mode);
                int n = ::connect(s, a->ai_addr, int(a->ai_addrlen));
                auto error = n ? WSAGetLastError() : 0;
                if (!n) {
                    selected = s;
                    break;
                }
                if (error != WSAEWOULDBLOCK) {
                    failure =
                        error == WSAECONNREFUSED ? EndpointFailure::refused : EndpointFailure::transport;
                    sockets.remove(s, true);
                }
            }
        }
        if (sockets.owned.empty()) {
            if (next >= addresses.size())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        fd_set writes, errors;
        FD_ZERO(&writes);
        FD_ZERO(&errors);
        for (auto s : sockets.owned) {
            FD_SET(s, &writes);
            FD_SET(s, &errors);
        }
        timeval wait{0, 20000};
        if (select(0, nullptr, &writes, &errors, &wait) > 0) {
            auto current = sockets.owned;
            for (auto s : current)
                if (FD_ISSET(s, &writes) || FD_ISSET(s, &errors)) {
                    int error = 0, size = sizeof(error);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size);
                    if (!error) {
                        selected = s;
                        break;
                    }
                    failure =
                        error == WSAECONNREFUSED ? EndpointFailure::refused : EndpointFailure::transport;
                    sockets.remove(s, true);
                }
        }
        if (selected != INVALID_SOCKET)
            break;
    }
    if (selected == INVALID_SOCKET) {
        if (race.cancelled)
            failure = EndpointFailure::cancelled;
        else if (Clock::now() >= deadline)
            failure = EndpointFailure::timeout;
        return {};
    }
    u_long mode = 0;
    tcp_ok = true;
    ioctlsocket(selected, FIONBIO, &mode);
    // Ownership passes to Tls, but the race retains a shutdown-only reference until construction ends.
    std::erase(sockets.owned, selected);
    try {
        auto tracked = race.tracked;
        auto tls = std::make_unique<Tls>(selected, nullptr, pin, deadline, [tracked, selected](bool) {
            std::lock_guard lock(tracked->mutex);
            tracked->sockets.erase(selected);
        });
        {
            std::lock_guard lock(tracked->mutex);
            tracked->sockets.erase(selected);
        }
        failure = EndpointFailure::none;
        return tls;
    } catch (const Error& e) {
        {
            std::lock_guard lock(race.tracked->mutex);
            race.tracked->sockets.erase(selected);
        }
        if (e.code == Code::unauthorized)
            failure = EndpointFailure::pin_mismatch;
        else {
            auto* transport = dynamic_cast<const TransportError*>(&e);
            failure = (Clock::now() >= deadline || (transport && transport->native_code == WSAETIMEDOUT))
                          ? EndpointFailure::timeout
                      : race.cancelled ? EndpointFailure::cancelled
                                       : EndpointFailure::transport;
        }
        return {};
    }
}
} // namespace
EndpointConnection connect_endpoints(const std::vector<ConnectionEndpoint>& list, const std::string& pin,
                                     std::chrono::milliseconds timeout) {
    if (!valid_sha(pin) || list.empty() || list.size() > 8 || timeout.count() < 1 ||
        timeout > std::chrono::seconds(60))
        throw Error(Code::invalid, "Invalid connection parameters");
    auto ordered = merge_endpoints({}, list, pairing_unix_time());
    if (ordered.empty())
        throw EndpointConnectError(EndpointFailure::timeout, {});
    Race race;
    auto begin = Clock::now(), deadline = begin + timeout;
    std::vector<std::jthread> threads;
    for (size_t i = 0; i < ordered.size(); ++i) {
        auto e = ordered[i];
        threads.emplace_back([&, i, e] {
            {
                std::unique_lock lock(race.mutex);
                race.cv.wait_until(lock, begin + std::chrono::milliseconds(250 * i),
                                   [&] { return race.cancelled.load(); });
            }
            if (race.cancelled) {
                std::lock_guard lock(race.mutex);
                EndpointProbe skipped{e};
                skipped.failure = EndpointFailure::cancelled;
                race.result.attempts.push_back(skipped);
                ++race.finished;
                return;
            }
            EndpointProbe probe{e};
            auto start = Clock::now();
            probe.start_ms =
                uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(start - begin).count());
            std::unique_ptr<Tls> tls;
            try {
                tls = dial(e, pin, deadline, race, probe.failure, probe.tcp_ok);
            } catch (...) {
                probe.failure = EndpointFailure::transport;
            }
            probe.elapsed_ms =
                uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
            probe.tls_ok = probe.failure == EndpointFailure::none;
            {
                std::lock_guard lock(race.mutex);
                race.result.attempts.push_back(probe);
                ++race.finished;
                if (probe.failure == EndpointFailure::pin_mismatch)
                    race.pin_failed = true;
                if (tls && !race.result.tls && !race.cancelled) {
                    race.result.tls = std::move(tls);
                    race.result.selected = e;
                    race.result.public_path = is_public(e.kind);
                }
                race.cv.notify_all();
            }
        });
    }
    {
        std::unique_lock lock(race.mutex);
        race.cv.wait_until(lock, deadline, [&] {
            return race.pin_failed || race.result.tls || race.finished == ordered.size();
        });
    }
    race.cancel();
    for (auto& t : threads)
        t.join();
    if (race.pin_failed)
        throw EndpointConnectError(EndpointFailure::pin_mismatch, std::move(race.result.attempts));
    if (!race.result.tls)
        throw EndpointConnectError(EndpointFailure::timeout, std::move(race.result.attempts));
    return std::move(race.result);
}
std::vector<EndpointProbe> probe_endpoints(const std::vector<ConnectionEndpoint>& list,
                                           const std::string& pin, std::chrono::milliseconds timeout) {
    std::vector<EndpointProbe> out;
    for (auto& e : merge_endpoints({}, list, pairing_unix_time())) {
        try {
            auto result = connect_endpoints({e}, pin, timeout);
            out.insert(out.end(), result.attempts.begin(), result.attempts.end());
        } catch (const EndpointConnectError& error) {
            out.insert(out.end(), error.attempts.begin(), error.attempts.end());
        }
    }
    return out;
}
std::vector<ConnectionEndpoint> client_endpoints(Client& c) {
    auto bytes = c.call(Op::capabilities);
    Reader caps{bytes};
    auto text = caps.str();
    caps.end();
    if (text.find(";endpoints=v1") == text.npos)
        return {};
    bytes = c.call(Op::endpoints);
    Reader r{bytes};
    if (r.u32() != 1)
        throw Error(Code::unsupported, "Endpoint version");
    auto out = read_endpoints(r);
    r.end();
    return out;
}
} // namespace lantern
