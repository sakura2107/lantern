#include "lantern/network.hpp"
#include "lantern/pairing.hpp"
#include <charconv>
#include <map>
namespace lantern {
uint64_t pairing_unix_time() {
    auto now =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    return now > 0 ? uint64_t(now) : 0;
}
bool valid_pairing_label(const std::string& text) {
    if (text.empty() || text.size() > 64 ||
        std::any_of(text.begin(), text.end(), [](unsigned char c) { return c < 32 || c == 127; }))
        return false;
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i++]);
        if (c < 128)
            continue;
        unsigned n = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : 0;
        if (!n || i + n > text.size())
            return false;
        uint32_t cp = c & ((1u << (6 - n)) - 1);
        for (unsigned k = 0; k < n; ++k) {
            auto b = static_cast<unsigned char>(text[i++]);
            if ((b & 0xC0) != 0x80)
                return false;
            cp = (cp << 6) | (b & 63);
        }
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
            (cp >= 0xD800 && cp <= 0xDFFF))
            return false;
    }
    return true;
}
bool valid_pairing_address(std::string_view text) {
    unsigned first = 0;
    for (int i = 0; i < 4; ++i) {
        auto dot = text.find('.');
        auto part = text.substr(0, dot);
        unsigned value = 0;
        auto parsed = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0') ||
            parsed.ec != std::errc() || parsed.ptr != part.data() + part.size() || value > 255)
            return false;
        if (i == 0)
            first = value;
        if (i == 3)
            return dot == std::string_view::npos && first > 0 && first < 224 && text != "255.255.255.255";
        if (dot == std::string_view::npos)
            return false;
        text.remove_prefix(dot + 1);
    }
    return false;
}
namespace {
int nibble(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
std::string decode(std::string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '%') {
            if (i + 2 >= s.size() || nibble(s[i + 1]) < 0 || nibble(s[i + 2]) < 0)
                throw Error(Code::invalid, "Invalid pairing link");
            c = uint8_t(nibble(s[i + 1]) * 16 + nibble(s[i + 2]));
            i += 2;
        } else if (c <= 32 || c >= 127 || c == '+' || c == '#')
            throw Error(Code::invalid, "Invalid pairing link");
        if (!c)
            throw Error(Code::invalid, "Invalid pairing link");
        out.push_back(char(c));
    }
    return out;
}
std::string encode(const std::string& s) {
    const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~')
            out += char(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}
uint64_t number(const std::string& s) {
    uint64_t n = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), n);
    if (s.empty() || (s.size() > 1 && s[0] == '0') || result.ec != std::errc() ||
        result.ptr != s.data() + s.size())
        throw Error(Code::invalid, "Invalid pairing link");
    return n;
}
} // namespace
std::optional<PairingLink> parse_pairing_uri(std::string_view uri) noexcept {
    try {
        constexpr std::string_view prefix = "lantern://pair?";
        if (uri.size() > 2048 || !uri.starts_with(prefix) || uri.find('#') != uri.npos)
            return {};
        uri.remove_prefix(prefix.size());
        std::map<std::string, std::string> fields;
        while (!uri.empty()) {
            auto amp = uri.find('&');
            auto item = uri.substr(0, amp);
            auto eq = item.find('=');
            if (eq == item.npos || item.find('=', eq + 1) != item.npos)
                return {};
            auto key = std::string(item.substr(0, eq));
            if (key != "v" && key != "n" && key != "a" && key != "p" && key != "f" && key != "c" &&
                key != "x")
                return {};
            if (!fields.emplace(key, decode(item.substr(eq + 1))).second)
                return {};
            if (amp == uri.npos)
                break;
            uri.remove_prefix(amp + 1);
            if (uri.empty())
                return {};
        }
        if (fields.size() != 7 || (fields.at("v") != "1" && fields.at("v") != "2"))
            return {};
        PairingLink link;
        link.host_name = fields.at("n");
        link.pin = fields.at("f");
        link.code = fields.at("c");
        std::transform(link.pin.begin(), link.pin.end(), link.pin.begin(),
                       [](char c) { return c >= 'A' && c <= 'F' ? char(c + 32) : c; });
        auto port = number(fields.at("p"));
        link.expires_at = number(fields.at("x"));
        if (!valid_pairing_label(link.host_name) || !valid_sha(link.pin) || !valid_sha(link.code) ||
            port < 1 || port > 65535 || link.expires_at <= pairing_unix_time() ||
            link.expires_at > 4102444800ULL)
            return {};
        link.port = uint16_t(port);
        std::string_view addresses = fields.at("a");
        unsigned domains = 0;
        for (;;) {
            auto comma = addresses.find(',');
            auto address = addresses.substr(0, comma);
            std::string host(address);
            uint16_t endpoint_port = link.port;
            bool valid = valid_pairing_address(address);
            if (fields.at("v") == "2") {
                std::string_view suffix;
                if (!address.empty() && address.front() == '[') {
                    auto end = address.find(']');
                    if (end == address.npos)
                        return {};
                    host = std::string(address.substr(1, end - 1));
                    suffix = address.substr(end + 1);
                    auto a = numeric_address(host);
                    valid = a && a->v6 && valid_connection_host(host);
                } else {
                    auto colon = address.find(':');
                    host = std::string(address.substr(0, colon));
                    suffix = colon == address.npos ? std::string_view() : address.substr(colon);
                    valid = valid_pairing_address(host);
                    if (valid_hostname(host))
                        valid = ++domains <= 1;
                }
                if (!suffix.empty()) {
                    if (suffix.front() != ':')
                        return {};
                    auto p = number(std::string(suffix.substr(1)));
                    if (!p || p > 65535)
                        return {};
                    endpoint_port = uint16_t(p);
                }
                host = normalize_address(host);
                std::transform(host.begin(), host.end(), host.begin(),
                               [](char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; });
            }
            if (!valid || link.addresses.size() >= 8 ||
                std::any_of(link.endpoints.begin(), link.endpoints.end(),
                            [&](auto& e) { return e.host == host && e.port == endpoint_port; }))
                return {};
            link.addresses.emplace_back(host);
            link.endpoints.push_back(connection_endpoint(host, endpoint_port));
            if (comma == addresses.npos)
                break;
            addresses.remove_prefix(comma + 1);
        }
        return link;
    } catch (...) {
        return {};
    }
}
std::string pairing_uri(const PairingInvite& invite, const std::vector<std::string>& addresses,
                        uint16_t port) {
    std::string joined;
    bool v2 = false;
    for (auto& a : addresses) {
        if (!joined.empty())
            joined += ',';
        if (a.find(':') != a.npos) {
            joined += '[' + a + ']';
            v2 = true;
        } else {
            joined += a;
            if (!valid_pairing_address(a))
                v2 = true;
        }
    }
    auto uri = std::string("lantern://pair?v=") + (v2 ? "2" : "1") + "&n=" + encode(invite.host_name) +
               "&a=" + joined + "&p=" + std::to_string(port) + "&f=" + invite.pin + "&c=" + invite.code +
               "&x=" + std::to_string(invite.expires_at);
    if (!parse_pairing_uri(uri))
        throw Error(Code::invalid, "配对链接参数无效或已过期");
    return uri;
}
std::string pairing_uri_endpoints(const PairingInvite& invite,
                                  const std::vector<ConnectionEndpoint>& endpoints, uint16_t port) {
    bool extended = std::any_of(endpoints.begin(), endpoints.end(),
                                [](auto& e) { return !valid_pairing_address(e.host); });
    if (!extended) {
        std::vector<std::string> addresses;
        for (auto& e : endpoints) {
            if (e.port != port)
                throw Error(Code::invalid, "IPv4-only pairing uses one common port");
            addresses.push_back(e.host);
        }
        return pairing_uri(invite, addresses, port);
    }
    std::string joined;
    for (auto& e : endpoints) {
        if (!joined.empty())
            joined += ',';
        joined += e.host.find(':') != e.host.npos ? '[' + e.host + ']' : e.host;
        if (e.port != port)
            joined += ':' + std::to_string(e.port);
    }
    auto uri = "lantern://pair?v=2&n=" + encode(invite.host_name) + "&a=" + joined +
               "&p=" + std::to_string(port) + "&f=" + invite.pin + "&c=" + invite.code +
               "&x=" + std::to_string(invite.expires_at);
    if (!parse_pairing_uri(uri))
        throw Error(Code::invalid, "配对链接参数无效或已过期");
    return uri;
}
} // namespace lantern
