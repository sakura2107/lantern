#include "lantern/network.hpp"
#include <charconv>
namespace lantern {
namespace {
std::string_view unscoped(std::string_view s) {
    auto at = s.find('%');
    if (at == s.npos || s.find(':') == s.npos)
        return s;
    auto zone = s.substr(at + 1);
    if (zone.empty() || !std::all_of(zone.begin(), zone.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return s;
    return s.substr(0, at);
}
bool v4(std::string_view s, std::array<uint8_t, 16>& b, size_t off = 0) {
    for (unsigned i = 0; i < 4; ++i) {
        auto dot = s.find('.');
        auto p = s.substr(0, dot);
        unsigned n = 0;
        auto r = std::from_chars(p.data(), p.data() + p.size(), n);
        if (p.empty() || p.size() > 3 || (p.size() > 1 && p[0] == '0') || r.ec != std::errc() ||
            r.ptr != p.data() + p.size() || n > 255)
            return false;
        b[off + i] = uint8_t(n);
        if (i == 3)
            return dot == s.npos;
        if (dot == s.npos)
            return false;
        s.remove_prefix(dot + 1);
    }
    return false;
}
} // namespace
std::optional<NumericAddress> numeric_address(std::string_view s) {
    NumericAddress a;
    if (s.find(':') == s.npos)
        return v4(s, a.bytes) ? std::optional(a) : std::nullopt;
    a.v6 = true;
    if (s.empty() || s.size() > 45 || s.find_first_of("%[]") != s.npos)
        return {};
    auto gap = s.find("::");
    if (gap != s.npos && s.find("::", gap + 2) != s.npos)
        return {};
    std::vector<uint16_t> left, right;
    auto parts = [&](std::string_view t, std::vector<uint16_t>& out, bool tail) {
        if (t.empty())
            return true;
        for (;;) {
            auto c = t.find(':');
            auto p = t.substr(0, c);
            if (p.find('.') != p.npos) {
                std::array<uint8_t, 16> b{};
                if (!tail || c != t.npos || !v4(p, b))
                    return false;
                out.push_back(uint16_t(b[0] * 256 + b[1]));
                out.push_back(uint16_t(b[2] * 256 + b[3]));
                return true;
            }
            unsigned n = 0;
            auto r = std::from_chars(p.data(), p.data() + p.size(), n, 16);
            if (p.empty() || p.size() > 4 || r.ec != std::errc() || r.ptr != p.data() + p.size())
                return false;
            out.push_back(uint16_t(n));
            if (out.size() > 8)
                return false;
            if (c == t.npos)
                break;
            t.remove_prefix(c + 1);
        }
        return true;
    };
    if (gap == s.npos) {
        if (!parts(s, left, true) || left.size() != 8)
            return {};
    } else if (!parts(s.substr(0, gap), left, false) || !parts(s.substr(gap + 2), right, true) ||
               left.size() + right.size() >= 8)
        return {};
    size_t at = 0;
    for (auto n : left) {
        a.bytes[at++] = uint8_t(n >> 8);
        a.bytes[at++] = uint8_t(n);
    }
    at = 16 - right.size() * 2;
    for (auto n : right) {
        a.bytes[at++] = uint8_t(n >> 8);
        a.bytes[at++] = uint8_t(n);
    }
    if (std::all_of(a.bytes.begin(), a.bytes.begin() + 10, [](auto b) { return b == 0; }) &&
        a.bytes[10] == 255 && a.bytes[11] == 255) {
        std::copy_n(a.bytes.begin() + 12, 4, a.bytes.begin());
        std::fill(a.bytes.begin() + 4, a.bytes.end(), 0);
        a.v6 = false;
    }
    return a;
}
AddressKind classify_address(std::string_view s) {
    auto a = numeric_address(unscoped(s));
    if (!a)
        return AddressKind::invalid;
    auto& b = a->bytes;
    if (!a->v6) {
        if (b[0] == 127)
            return AddressKind::loopback;
        if (b[0] == 169 && b[1] == 254)
            return AddressKind::link_local;
        if (b[0] >= 224)
            return AddressKind::multicast;
        if (!b[0])
            return AddressKind::invalid;
        if (b[0] == 10 || (b[0] == 172 && (b[1] & 240) == 16) || (b[0] == 192 && b[1] == 168))
            return AddressKind::private_v4;
        if (b[0] == 100 && (b[1] & 192) == 64)
            return AddressKind::cgnat_v4;
        return AddressKind::public_v4;
    }
    bool zero = std::all_of(b.begin(), b.end() - 1, [](auto v) { return v == 0; });
    if (zero && b[15] <= 1)
        return b[15] ? AddressKind::loopback : AddressKind::invalid;
    if (b[0] == 255)
        return AddressKind::multicast;
    if (b[0] == 254 && (b[1] & 192) == 128)
        return AddressKind::link_local;
    if ((b[0] & 254) == 252)
        return AddressKind::ula_v6;
    if ((b[0] & 224) == 32)
        return AddressKind::global_v6;
    return AddressKind::invalid;
}
std::string normalize_address(std::string_view s) {
    auto a = numeric_address(unscoped(s));
    if (!a)
        return std::string(s);
    auto& b = a->bytes;
    std::string out;
    if (!a->v6)
        return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
               std::to_string(b[3]);
    // Fixed eight-group form makes equality and source aggregation independent of spelling.
    for (unsigned i = 0; i < 16; i += 2) {
        if (i)
            out += ':';
        char buf[5];
        auto r = std::to_chars(buf, buf + 5, unsigned(b[i]) * 256 + b[i + 1], 16);
        out.append(buf, r.ptr);
    }
    return out;
}
std::string source_key(std::string_view s) {
    auto a = numeric_address(unscoped(s));
    if (!a)
        return "unknown";
    if (!a->v6)
        return normalize_address(s);
    auto out = normalize_address(s);
    size_t at = 0;
    for (int i = 0; i < 4; ++i)
        at = out.find(':', at) + 1;
    return out.substr(0, at) + "/64";
}
bool local_pairing_source(std::string_view s) {
    auto k = classify_address(s);
    return k == AddressKind::private_v4 || k == AddressKind::ula_v6 || k == AddressKind::link_local ||
           k == AddressKind::loopback;
}
bool valid_hostname(std::string_view s) {
    if (s.size() > 253 || s.find('.') == s.npos || numeric_address(s))
        return false;
    if (std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }))
        return false;
    for (;;) {
        auto dot = s.find('.');
        auto label = s.substr(0, dot);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-')
            return false;
        for (char c : label)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
                return false;
        if (dot == s.npos)
            return true;
        s.remove_prefix(dot + 1);
    }
}
bool valid_connection_host(std::string_view s) {
    if (s.find('%') != s.npos)
        return false;
    auto k = classify_address(s);
    return (k != AddressKind::invalid && k != AddressKind::multicast && k != AddressKind::link_local) ||
           valid_hostname(s);
}
bool is_public(EndpointKind k) {
    return k == EndpointKind::public_v4 || k == EndpointKind::public_v6 || k == EndpointKind::hostname;
}
void record_endpoint_results(std::vector<ConnectionEndpoint>& list,
                             const std::vector<EndpointProbe>& reports) {
    for (auto& p : reports)
        for (auto& e : list)
            if (normalize_address(e.host) == normalize_address(p.endpoint.host) &&
                e.port == p.endpoint.port) {
                if (p.failure == EndpointFailure::none)
                    e.failures = 0;
                else if (p.failure != EndpointFailure::cancelled &&
                         p.failure != EndpointFailure::pin_mismatch)
                    e.failures = std::min(100u, e.failures + 1);
            }
}
ConnectionEndpoint connection_endpoint(const std::string& host, uint16_t port) {
    auto k = classify_address(host);
    EndpointKind kind = k == AddressKind::global_v6                                 ? EndpointKind::public_v6
                        : k == AddressKind::ula_v6                                  ? EndpointKind::lan_v6
                        : k == AddressKind::public_v4 || k == AddressKind::cgnat_v4 ? EndpointKind::public_v4
                        : valid_hostname(host)                                      ? EndpointKind::hostname
                                                                                    : EndpointKind::lan_v4;
    return {kind, normalize_address(host), port, uint32_t(kind) * 100};
}
std::vector<ConnectionEndpoint> merge_endpoints(const std::vector<ConnectionEndpoint>& saved,
                                                const std::vector<ConnectionEndpoint>& fresh, uint64_t now) {
    std::vector<ConnectionEndpoint> out;
    auto add = [&](ConnectionEndpoint e) {
        if (!e.port || !valid_connection_host(e.host) || uint32_t(e.kind) < 1 || uint32_t(e.kind) > 5)
            return;
        if (connection_endpoint(e.host, e.port).kind != e.kind)
            return;
        if (e.kind != EndpointKind::hostname && ((e.expires_at && e.expires_at <= now) || e.failures >= 3))
            return;
        e.host = normalize_address(e.host);
        std::transform(e.host.begin(), e.host.end(), e.host.begin(),
                       [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; });
        if (std::none_of(out.begin(), out.end(),
                         [&](auto& v) { return v.host == e.host && v.port == e.port; }))
            out.push_back(std::move(e));
    };
    for (auto& e : fresh)
        add(e);
    for (auto& e : saved)
        add(e);
    std::stable_sort(out.begin(), out.end(), [](auto& a, auto& b) {
        return std::pair(is_public(a.kind), a.priority) < std::pair(is_public(b.kind), b.priority);
    });
    // Domain survives truncation, including a learned DDNS name behind eight numeric addresses.
    while (out.size() > 8) {
        auto at =
            std::find_if(out.rbegin(), out.rend(), [](auto& e) { return e.kind != EndpointKind::hostname; });
        if (at == out.rend())
            out.pop_back();
        else
            out.erase(std::next(at).base());
    }
    return out;
}
void write_endpoints(Writer& w, const std::vector<ConnectionEndpoint>& list) {
    if (list.size() > 8)
        throw Error(Code::invalid, "Endpoint count bound");
    w.u32(uint32_t(list.size()));
    for (auto& e : list) {
        w.u32(uint32_t(e.kind));
        w.str(e.host);
        w.u32(e.port);
        w.u32(e.priority);
        w.u64(e.expires_at);
        w.u32(e.failures);
    }
}
std::vector<ConnectionEndpoint> read_endpoints(Reader& r) {
    auto n = r.u32();
    if (n > 8)
        throw Error(Code::invalid, "Endpoint count bound");
    std::vector<ConnectionEndpoint> out;
    while (n--) {
        ConnectionEndpoint e;
        e.kind = EndpointKind(r.u32());
        e.host = r.str(253);
        auto p = r.u32();
        e.priority = r.u32();
        e.expires_at = r.u64();
        e.failures = r.u32();
        if (!p || p > 65535 || uint32_t(e.kind) < 1 || uint32_t(e.kind) > 5 ||
            !valid_connection_host(e.host) || connection_endpoint(e.host, uint16_t(p)).kind != e.kind)
            throw Error(Code::invalid, "Invalid endpoint");
        e.port = uint16_t(p);
        out.push_back(std::move(e));
    }
    return out;
}
} // namespace lantern
