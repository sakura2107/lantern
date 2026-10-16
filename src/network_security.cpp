#include "lantern/network_security.hpp"
#include "lantern/pairing.hpp"
namespace lantern {
void AuthenticationGuard::event(const std::string& address, Code code, bool throttle, uint64_t now) {
    ++stats_.rejected;
    if (throttle)
        ++stats_.throttled;
    stats_.recent.push_back({source_key(address), now, code});
    if (stats_.recent.size() > 32)
        stats_.recent.erase(stats_.recent.begin());
}
bool AuthenticationGuard::acquire(const std::string& address, uint64_t now) {
    std::erase_if(entries_, [&](auto& e) {
        return !e.second.active && now > e.second.last && now - e.second.last > 300000;
    });
    auto key = source_key(address);
    if (!entries_.contains(key) && entries_.size() >= 1024) {
        event(address, Code::busy, true, pairing_unix_time());
        return false;
    }
    auto& e = entries_[key];
    e.last = now;
    if (e.active >= 4) {
        event(address, Code::busy, true, pairing_unix_time());
        return false;
    }
    ++e.active;
    return true;
}
void AuthenticationGuard::release(const std::string& address) {
    auto at = entries_.find(source_key(address));
    if (at != entries_.end() && at->second.active)
        --at->second.active;
}
bool AuthenticationGuard::allowed(const std::string& address, uint64_t now) {
    auto at = entries_.find(source_key(address));
    if (at == entries_.end() || now >= at->second.retry_at)
        return true;
    event(address, Code::busy, true, pairing_unix_time());
    return false;
}
void AuthenticationGuard::failed(const std::string& address, uint64_t now) {
    auto& e = entries_[source_key(address)];
    e.last = now;
    e.failures = std::min(10u, e.failures + 1);
    e.retry_at = now + std::min(60000ull, 250ull << (e.failures - 1));
    event(address, Code::unauthorized, false, pairing_unix_time());
}
void AuthenticationGuard::success(const std::string& address) {
    auto at = entries_.find(source_key(address));
    if (at != entries_.end()) {
        at->second.failures = 0;
        at->second.retry_at = 0;
    }
}
void AuthenticationGuard::timeout(const std::string& address) {
    ++stats_.timeouts;
    event(address, Code::busy, false, pairing_unix_time());
}
} // namespace lantern
