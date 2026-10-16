#pragma once
#include "network.hpp"
#include <map>
namespace lantern {
struct RejectEvent {
    std::string source;
    uint64_t unix_time = 0;
    Code reason = Code::unauthorized;
};
struct NetworkSecurityStats {
    uint64_t rejected = 0, throttled = 0, timeouts = 0;
    std::vector<RejectEvent> recent;
};
// Caller serializes access. Time injection is for deterministic policy tests.
class AuthenticationGuard {
    struct Entry {
        uint32_t active = 0, failures = 0;
        uint64_t retry_at = 0, last = 0;
    };
    std::map<std::string, Entry> entries_;
    NetworkSecurityStats stats_;
    void event(const std::string&, Code, bool, uint64_t);

  public:
    bool acquire(const std::string& address, uint64_t now_ms);
    void release(const std::string& address);
    bool allowed(const std::string& address, uint64_t now_ms);
    void failed(const std::string& address, uint64_t now_ms);
    void success(const std::string& address);
    void timeout(const std::string& address);
    NetworkSecurityStats stats() const {
        return stats_;
    }
};
} // namespace lantern
