#include "lantern/lan.hpp"
#include <iphlpapi.h>
#include <ws2tcpip.h>
namespace lantern {
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
