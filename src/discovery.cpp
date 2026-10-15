#include "lantern/discovery.hpp"
#include <iphlpapi.h>
#include <set>
#include <thread>
#include <ws2tcpip.h>
namespace lantern {
namespace {
constexpr std::string_view query_magic = "LANTERN?", reply_magic = "LANTERN!";
bool local_address(uint32_t network) {
    auto ip = ntohl(network);
    return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 16) == 0xc0a8 || (ip >> 20) == 0xac1 ||
           (ip >> 16) == 0xa9fe;
}
void prefix(std::span<const uint8_t> b, std::string_view magic) {
    if (b.size() < 8 || !std::equal(magic.begin(), magic.end(), b.begin()))
        throw Error(Code::invalid, "Discovery signature");
}
struct Socket {
    SOCKET s = INVALID_SOCKET;
    ~Socket() {
        if (s != INVALID_SOCKET)
            closesocket(s);
    }
};
} // namespace
Bytes discovery_query(const std::string& nonce) {
    if (!valid_sha(nonce))
        throw Error(Code::invalid, "Discovery nonce");
    Writer w;
    w.b.assign(query_magic.begin(), query_magic.end());
    w.u32(1);
    w.str(nonce);
    w.b.resize(128);
    return w.b;
}
Bytes discovery_reply(std::span<const uint8_t> request, const std::string& label, const std::string& pin,
                      uint16_t port) {
    if (request.size() != 128 || label.empty() || label.size() > 64 || !valid_sha(pin) || !port)
        throw Error(Code::invalid, "Discovery limits");
    prefix(request, query_magic);
    Reader r{request.subspan(8)};
    if (r.u32() != 1)
        throw Error(Code::unsupported, "Discovery version");
    auto nonce = r.str(64);
    if (!valid_sha(nonce) || std::any_of(r.b.begin() + r.at, r.b.end(), [](auto b) { return b != 0; }))
        throw Error(Code::invalid, "Discovery query");
    Writer w;
    w.b.assign(reply_magic.begin(), reply_magic.end());
    w.u32(1);
    w.str(nonce);
    w.str(pin);
    w.str(label);
    w.u32(port);
    return w.b;
}
DiscoveredDevice parse_discovery(std::span<const uint8_t> data, const std::string& nonce,
                                 const std::string& source) {
    if (data.size() > 256 || !valid_sha(nonce))
        throw Error(Code::invalid, "Discovery reply bound");
    prefix(data, reply_magic);
    Reader r{data.subspan(8)};
    if (r.u32() != 1 || r.str(64) != nonce)
        throw Error(Code::invalid, "Discovery nonce mismatch");
    auto pin = r.str(64), label = r.str(64);
    auto port = r.u32();
    r.end();
    if (!valid_sha(pin) || label.empty() || !port || port > 65535 ||
        std::any_of(label.begin(), label.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        throw Error(Code::invalid, "Discovery metadata");
    wide(label);
    return {source, label, pin, uint16_t(port)};
}
std::vector<DiscoveredDevice> discover_devices(Cancel& cancel, const std::string& target) {
    init_sockets();
    Socket socket;
    socket.s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket.s == INVALID_SOCKET)
        throw Error(Code::io, "Discovery socket");
    BOOL broadcast = TRUE;
    setsockopt(socket.s, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast));
    DWORD timeout = 100;
    setsockopt(socket.s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    std::set<uint32_t> destinations;
    if (!target.empty()) {
        IN_ADDR a{};
        if (inet_pton(AF_INET, target.c_str(), &a) != 1)
            throw Error(Code::invalid, "Discovery test target");
        destinations.insert(a.S_un.S_addr);
    } else {
        destinations.insert(htonl(INADDR_LOOPBACK));
        ULONG size = 16384;
        Bytes adapters(size);
        auto result = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
            (IP_ADAPTER_ADDRESSES*)adapters.data(), &size);
        if (result == ERROR_BUFFER_OVERFLOW && size <= 1024 * 1024) {
            adapters.resize(size);
            result = GetAdaptersAddresses(
                AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                (IP_ADAPTER_ADDRESSES*)adapters.data(), &size);
        }
        if (result == NO_ERROR)
            for (auto adapter = (IP_ADAPTER_ADDRESSES*)adapters.data(); adapter && destinations.size() < 16;
                 adapter = adapter->Next)
                if (adapter->OperStatus == IfOperStatusUp)
                    for (auto address = adapter->FirstUnicastAddress; address; address = address->Next)
                        if (address->Address.lpSockaddr->sa_family == AF_INET &&
                            address->OnLinkPrefixLength > 0 && address->OnLinkPrefixLength < 31) {
                            auto ip = ((sockaddr_in*)address->Address.lpSockaddr)->sin_addr.S_un.S_addr;
                            if (local_address(ip)) {
                                uint32_t mask = 0xffffffffu << (32 - address->OnLinkPrefixLength);
                                destinations.insert(htonl(ntohl(ip) | ~mask));
                            }
                        }
    }
    auto nonce = random_token();
    auto request = discovery_query(nonce);
    std::vector<DiscoveredDevice> result;
    auto started = std::chrono::steady_clock::now(), next = started;
    while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(1500)) {
        cancel.check();
        auto now = std::chrono::steady_clock::now();
        if (now >= next) {
            for (auto ip : destinations) {
                sockaddr_in to{};
                to.sin_family = AF_INET;
                to.sin_addr.S_un.S_addr = ip;
                to.sin_port = htons(discovery_port);
                sendto(socket.s, (const char*)request.data(), int(request.size()), 0, (sockaddr*)&to,
                       sizeof(to));
            }
            next = now + std::chrono::milliseconds(500);
        }
        uint8_t buffer[257];
        sockaddr_in source{};
        int n = sizeof(source);
        auto count = recvfrom(socket.s, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&source, &n);
        if (count <= 0 || count > 256 || ntohs(source.sin_port) != discovery_port ||
            !local_address(source.sin_addr.S_un.S_addr))
            continue;
        char address[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &source.sin_addr, address, sizeof(address));
        try {
            auto device = parse_discovery({buffer, size_t(count)}, nonce, address);
            if (result.size() < 32 && std::none_of(result.begin(), result.end(), [&](auto& d) {
                    return d.host == device.host && d.port == device.port;
                }))
                result.push_back(std::move(device));
        } catch (const Error&) {
        }
    }
    return result;
}
struct DiscoveryResponder::Impl {
    Socket socket;
    std::thread worker;
    std::atomic_bool stop = false;
    std::string message = "当前绑定地址不支持IPv4发现，可手工连接";
    ~Impl() {
        stop = true;
        if (worker.joinable())
            worker.join();
    }
};
DiscoveryResponder::DiscoveryResponder(const std::string& bind, uint16_t port, const std::string& pin)
    : p_(std::make_unique<Impl>()) {
    IN_ADDR numeric{};
    if (bind != "::" && inet_pton(AF_INET, bind.c_str(), &numeric) != 1)
        return;
    p_->socket.s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (p_->socket.s == INVALID_SOCKET) {
        p_->message = "发现服务未启动：UDP套接字不可用";
        return;
    }
    BOOL exclusive = TRUE;
    setsockopt(p_->socket.s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (char*)&exclusive, sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(discovery_port);
    address.sin_addr = bind == "::" ? IN_ADDR{} : numeric;
    if (::bind(p_->socket.s, (sockaddr*)&address, sizeof(address))) {
        p_->message = "发现端口已占用；可继续手工连接";
        return;
    }
    DWORD timeout = 100;
    setsockopt(p_->socket.s, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
    DWORD ttl = 1;
    setsockopt(p_->socket.s, IPPROTO_IP, IP_TTL, (char*)&ttl, sizeof(ttl));
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD length = MAX_COMPUTERNAME_LENGTH + 1;
    std::string label =
        GetComputerNameW(computer, &length) ? utf8(std::wstring(computer, length)) : "Windows host";
    p_->message = "局域网发现已开启，设备身份仍需核对";
    p_->worker = std::thread([p = p_.get(), port, pin, label] {
        auto window = std::chrono::steady_clock::now();
        unsigned replies = 0;
        while (!p->stop) {
            uint8_t buffer[129];
            sockaddr_in from{};
            int n = sizeof(from);
            auto count = recvfrom(p->socket.s, (char*)buffer, sizeof(buffer), 0, (sockaddr*)&from, &n);
            if (count != 128 || !from.sin_port || !local_address(from.sin_addr.S_un.S_addr))
                continue;
            auto now = std::chrono::steady_clock::now();
            if (now - window >= std::chrono::seconds(1)) {
                window = now;
                replies = 0;
            }
            if (replies >= 4)
                continue;
            try {
                auto reply = discovery_reply({buffer, size_t(count)}, label, pin, port);
                ++replies;
                sendto(p->socket.s, (const char*)reply.data(), int(reply.size()), 0, (sockaddr*)&from,
                       sizeof(from));
            } catch (const Error&) {
            }
        }
    });
}
DiscoveryResponder::~DiscoveryResponder() = default;
void DiscoveryResponder::stop() noexcept {
    p_->stop = true;
}
std::string DiscoveryResponder::status() const {
    return p_->message;
}
} // namespace lantern
