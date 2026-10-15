#include "lantern/core.hpp"
#include <cctype>
#include <charconv>
namespace lantern {
void Writer::u16(uint16_t n) {
    b.push_back(uint8_t(n >> 8));
    b.push_back(uint8_t(n));
}
void Writer::u32(uint32_t n) {
    for (int i = 3; i >= 0; --i)
        b.push_back(uint8_t(n >> (8 * i)));
}
void Writer::u64(uint64_t n) {
    for (int i = 7; i >= 0; --i)
        b.push_back(uint8_t(n >> (8 * i)));
}
void Writer::blob(std::span<const uint8_t> s) {
    if (s.size() > max_frame)
        throw Error(Code::invalid, "Oversized blob");
    u32(uint32_t(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
void Writer::str(const std::string& s) {
    blob({reinterpret_cast<const uint8_t*>(s.data()), s.size()});
}
uint16_t Reader::u16() {
    if (b.size() - at < 2)
        throw Error(Code::invalid, "Truncated u16");
    auto n = uint16_t((b[at] << 8) | b[at + 1]);
    at += 2;
    return n;
}
uint32_t Reader::u32() {
    if (b.size() - at < 4)
        throw Error(Code::invalid, "Truncated u32");
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i)
        n = (n << 8) | b[at++];
    return n;
}
uint64_t Reader::u64() {
    if (b.size() - at < 8)
        throw Error(Code::invalid, "Truncated u64");
    uint64_t n = 0;
    for (int i = 0; i < 8; ++i)
        n = (n << 8) | b[at++];
    return n;
}
Bytes Reader::blob(uint32_t limit) {
    auto n = u32();
    if (n > limit || n > b.size() - at)
        throw Error(Code::invalid, "Invalid blob length");
    Bytes r(b.begin() + at, b.begin() + at + n);
    at += n;
    return r;
}
std::string Reader::str(uint32_t l) {
    auto v = blob(l);
    return std::string(v.begin(), v.end());
}
void Reader::end() {
    if (at != b.size())
        throw Error(Code::invalid, "Trailing protocol data");
}
bool valid_sha(const std::string& s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}
ByteRange parse_range(const std::string& s, uint64_t size) {
    auto parse = [](std::string_view v) {
        uint64_t n = 0;
        auto [p, e] = std::from_chars(v.data(), v.data() + v.size(), n);
        if (v.empty() || e != std::errc{} || p != v.data() + v.size())
            throw Error(Code::invalid, "Invalid byte range number");
        return n;
    };
    if (!size)
        throw Error(Code::invalid, "Empty source has no ranges");
    auto dash = s.find('-');
    if (dash == std::string::npos || s.find(',', 0) != std::string::npos)
        throw Error(Code::invalid, "Single byte range required");
    uint64_t begin = 0, end = size - 1;
    if (dash == 0) {
        auto suffix = parse(std::string_view(s).substr(1));
        if (!suffix)
            throw Error(Code::invalid, "Zero suffix range");
        begin = size - std::min(size, suffix);
    } else {
        begin = parse(std::string_view(s).substr(0, dash));
        if (dash + 1 < s.size())
            end = std::min(end, parse(std::string_view(s).substr(dash + 1)));
    }
    if (begin >= size || end < begin)
        throw Error(Code::invalid, "Range outside source");
    return {begin, end};
}
Endpoint parse_endpoint(const std::string& value, uint16_t default_port) {
    if (!default_port || value.empty() || value.size() > 260)
        throw Error(Code::invalid, "Invalid device address");
    Endpoint e;
    e.port = default_port;
    std::string port;
    if (value.front() == '[') {
        auto end = value.find(']');
        if (end == std::string::npos || end < 2)
            throw Error(Code::invalid, "IPv6 brackets");
        e.host = value.substr(1, end - 1);
        if (end + 1 < value.size()) {
            if (value[end + 1] != ':')
                throw Error(Code::invalid, "IPv6 port separator");
            port = value.substr(end + 2);
            if (port.empty())
                throw Error(Code::invalid, "Port missing");
        }
    } else {
        auto colon = value.find(':');
        if (colon != std::string::npos && colon == value.rfind(':')) {
            e.host = value.substr(0, colon);
            port = value.substr(colon + 1);
            if (port.empty())
                throw Error(Code::invalid, "Port missing");
        } else
            e.host = value;
    }
    if (e.host.empty() || e.host.size() > 253 ||
        std::any_of(e.host.begin(), e.host.end(), [](unsigned char c) {
            return c <= 32 || c == 127 || c == '/' || c == '\\' || c == '[' || c == ']' || c == '@';
        }))
        throw Error(Code::invalid, "Host address characters");
    if (!port.empty()) {
        uint32_t n = 0;
        for (char c : port) {
            if (c < '0' || c > '9' || n > 65535 / 10)
                throw Error(Code::invalid, "Port must be 1..65535");
            n = n * 10 + uint32_t(c - '0');
            if (n > 65535)
                throw Error(Code::invalid, "Port must be 1..65535");
        }
        if (!n)
            throw Error(Code::invalid, "Port must be 1..65535");
        e.port = uint16_t(n);
    }
    return e;
}
std::string format_endpoint(const Endpoint& e) {
    return (e.host.find(':') == std::string::npos ? e.host : "[" + e.host + "]") + ":" +
           std::to_string(e.port);
}
bool safe_path(const std::string& p) {
    if (p.empty())
        return true;
    if (p.size() > 2048 || p.front() == '/' || p.back() == '/')
        return false;
    size_t start = 0;
    while (start < p.size()) {
        auto end = p.find('/', start);
        if (end == std::string::npos)
            end = p.size();
        auto s = p.substr(start, end - start);
        if (s.empty() || s == "." || s == ".." || s.back() == '.' || s.back() == ' ')
            return false;
        for (unsigned char c : s)
            if (c < 32 || c == 127 || c == ':' || c == '\\' || c == '*' || c == '?' || c == '"' || c == '<' ||
                c == '>' || c == '|')
                return false;
        auto base = s.substr(0, s.find('.'));
        std::transform(base.begin(), base.end(), base.begin(),
                       [](unsigned char c) { return char(std::toupper(c)); });
        if (base == ".RDX-PRIVATE" || base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            base == "CONIN$" || base == "CONOUT$" || base == "CLOCK$" ||
            (base.size() == 4 && (base.starts_with("COM") || base.starts_with("LPT")) && base[3] >= '0' &&
             base[3] <= '9'))
            return false;
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return char(std::tolower(c)); });
        if (lower.starts_with(".rdx-private"))
            return false;
        start = end + 1;
    }
    return true;
}
bool Budget::admit(Lane lane, size_t bytes, std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(m_);
    double dt = std::max(0.0, std::chrono::duration<double>(now - last_).count());
    last_ = now;
    bulk_ = std::min(rate_ * .25, bulk_ + dt * rate_ * .25);
    media_ = std::min(rate_ * .5, media_ + dt * rate_ * .5);
    if (lane == Lane::bulk || lane == Lane::media) {
        double& tokens = lane == Lane::bulk ? bulk_ : media_;
        if (tokens < bytes)
            return false;
        tokens -= bytes;
    }
    return true;
}
bool transition_allowed(TaskState a, TaskState b) {
    if (a == TaskState::completed || a == TaskState::failed || a == TaskState::cancelled)
        return false;
    if (b == TaskState::cancelled || b == TaskState::failed)
        return true;
    if (a == TaskState::queued)
        return b == TaskState::running;
    if (a == TaskState::running)
        return b == TaskState::paused || b == TaskState::verifying;
    if (a == TaskState::paused)
        return b == TaskState::running;
    return a == TaskState::verifying && b == TaskState::completed;
}
} // namespace lantern
