#include "lantern/lan.hpp"
#include <iphlpapi.h>
#include <ws2tcpip.h>
namespace lantern {
bool temporary_ipv6(bool random_suffix, uint32_t valid, uint32_t preferred) {
    return random_suffix && valid != UINT32_MAX && preferred != UINT32_MAX && valid <= 7 * 86400 &&
           preferred <= 86400;
}
std::vector<HostAddress> host_addresses() {
    init_sockets();
    ULONG bytes = 16384;
    Bytes buffer(bytes);
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (unsigned attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(bytes);
        status = GetAdaptersAddresses(
            AF_UNSPEC, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
    }
    if (status == ERROR_NO_DATA)
        return {};
    if (status != NO_ERROR)
        throw Error(Code::io, "无法枚举本机地址");
    std::vector<HostAddress> result;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        auto name = a->FriendlyName ? utf8(a->FriendlyName) : std::string();
        auto description = name + " " + (a->Description ? utf8(a->Description) : std::string());
        std::transform(description.begin(), description.end(), description.begin(),
                       [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
        bool virt = a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP;
        for (auto m : {"hyper-v", "vethernet", "vmware", "virtualbox", "wsl", "vpn", "virtual", "tap-", "tun",
                       "wireguard", "tailscale", "zerotier"})
            if (description.find(m) != description.npos)
                virt = true;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr ||
                (u->DadState != IpDadStatePreferred && u->DadState != IpDadStateDeprecated))
                continue;
            char ip[NI_MAXHOST]{};
            if (getnameinfo(u->Address.lpSockaddr, u->Address.iSockaddrLength, ip, sizeof(ip), nullptr, 0,
                            NI_NUMERICHOST))
                continue;
            auto k = classify_address(ip);
            if (k == AddressKind::invalid || k == AddressKind::loopback || k == AddressKind::link_local ||
                k == AddressKind::multicast || !u->ValidLifetime)
                continue;
            HostAddress h;
            h.address = normalize_address(ip);
            h.adapter_name = name;
            h.type = a->IfType == IF_TYPE_IEEE80211         ? LanType::wifi
                     : a->IfType == IF_TYPE_ETHERNET_CSMACD ? LanType::ethernet
                                                            : LanType::other;
            h.virtual_adapter = virt;
            h.category = k;
            h.deprecated = u->DadState == IpDadStateDeprecated || !u->PreferredLifetime;
            h.valid_lifetime = u->ValidLifetime;
            h.preferred_lifetime = u->PreferredLifetime;
            h.temporary = u->Address.lpSockaddr->sa_family == AF_INET6 &&
                          temporary_ipv6(u->SuffixOrigin == IpSuffixOriginRandom, u->ValidLifetime,
                                         u->PreferredLifetime);
            // Transient also includes clustered addresses; none are suitable stable inbound endpoints.
            if (u->Flags & IP_ADAPTER_ADDRESS_TRANSIENT)
                h.temporary = true;
            result.push_back(std::move(h));
        }
    }
    std::stable_sort(result.begin(), result.end(), [](auto& a, auto& b) {
        return std::tuple(a.virtual_adapter, a.temporary, a.deprecated, a.category, a.adapter_name,
                          a.address) < std::tuple(b.virtual_adapter, b.temporary, b.deprecated, b.category,
                                                  b.adapter_name, b.address);
    });
    return result;
}
uint64_t host_address_fingerprint() {
    uint64_t hash = 14695981039346656037ull;
    for (auto& a : host_addresses()) {
        auto text = a.address + a.adapter_name + std::to_string(int(a.category)) +
                    std::to_string(a.temporary) + std::to_string(a.deprecated);
        for (auto c : text) {
            hash ^= uint8_t(c);
            hash *= 1099511628211ull;
        }
    }
    return hash;
}
std::vector<ConnectionEndpoint> host_endpoints(const Config& c, uint16_t port,
                                               const std::vector<HostAddress>& addresses) {
    std::vector<ConnectionEndpoint> out;
    auto now = pairing_unix_time();
    for (auto& a : addresses) {
        if (a.deprecated || a.temporary)
            continue;
        bool local = a.category == AddressKind::private_v4 || a.category == AddressKind::ula_v6;
        if (!local && (!c.internet_access || a.category == AddressKind::cgnat_v4))
            continue;
        auto e = connection_endpoint(a.address, port);
        e.priority = uint32_t(e.kind) * 100 + (a.virtual_adapter ? 10 : 0);
        if (a.valid_lifetime != UINT32_MAX)
            e.expires_at = now + a.valid_lifetime;
        if (e.kind == EndpointKind::public_v4 && c.external_port)
            e.port = c.external_port;
        out.push_back(e);
    }
    if (c.internet_access && !c.public_hostname.empty())
        out.push_back(connection_endpoint(c.public_hostname, c.external_port ? c.external_port : port));
    return merge_endpoints({}, out, now);
}
std::vector<ConnectionEndpoint> host_endpoints(const Config& c, uint16_t port) {
    return host_endpoints(c, port, host_addresses());
}
ReachabilityReport reachability_report(const Config& c, const fs::path& exe, const std::string& bind,
                                       uint16_t port, bool listening, const UpnpReport& upnp) {
    ReachabilityReport r;
    r.bind = bind;
    r.port = port;
    r.listening = listening;
    r.dual_stack = listening && bind == "::";
    r.internet_access = c.internet_access;
    r.legacy_enabled = c.legacy_enabled;
    r.upnp = upnp;
    r.addresses = host_addresses();
    for (auto& a : r.addresses) {
        if (a.category == AddressKind::global_v6 && !a.temporary && !a.deprecated)
            r.stable_global_ipv6 = true;
        if (a.category == AddressKind::cgnat_v4)
            r.cgnat = true;
    }
    r.cgnat = r.cgnat || upnp.ipv4.status == UpnpStatus::cgnat;
    r.public_firewall = public_firewall_state(exe, port);
    r.current_profiles = firewall_current_profiles();
    return r;
}
std::vector<LanAddress> lan_addresses() {
    init_sockets();
    ULONG bytes = 16384;
    Bytes buffer(bytes);
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (unsigned attempt = 0; attempt < 3 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(bytes);
        status = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &bytes);
    }
    if (status == ERROR_NO_DATA)
        return {};
    if (status != NO_ERROR)
        throw Error(Code::io, "无法枚举局域网网卡");
    std::vector<LanAddress> result;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        auto name = a->FriendlyName ? utf8(a->FriendlyName) : std::string();
        auto description = name + " " + (a->Description ? utf8(a->Description) : std::string());
        std::transform(description.begin(), description.end(), description.begin(),
                       [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
        bool virtual_adapter = a->IfType == IF_TYPE_TUNNEL || a->IfType == IF_TYPE_PPP;
        for (auto marker : {"hyper-v", "vethernet", "vmware", "virtualbox", "wsl", "vpn", "virtual", "tap-",
                            "tun", "wireguard", "tailscale", "zerotier"})
            if (description.find(marker) != description.npos)
                virtual_adapter = true;
        auto type = a->IfType == IF_TYPE_IEEE80211         ? LanType::wifi
                    : a->IfType == IF_TYPE_ETHERNET_CSMACD ? LanType::ethernet
                                                           : LanType::other;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET ||
                u->DadState != IpDadStatePreferred)
                continue;
            auto* address = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
            auto ip = ntohl(address->sin_addr.s_addr);
            if ((ip >> 24) == 127 || (ip >> 16) == 0xa9fe || !ip || (ip >> 24) >= 224)
                continue;
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)))
                result.push_back({text, name, type, virtual_adapter});
        }
    }
    std::stable_sort(result.begin(), result.end(), [](auto& a, auto& b) {
        if (a.virtual_adapter != b.virtual_adapter)
            return !a.virtual_adapter;
        if (a.type != b.type)
            return int(a.type) < int(b.type);
        if (a.adapter_name != b.adapter_name)
            return a.adapter_name < b.adapter_name;
        return a.address < b.address;
    });
    return result;
}
} // namespace lantern
