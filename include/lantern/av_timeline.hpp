#pragma once
#include <algorithm>
#include <cstdint>
#include <mutex>
#include <vector>
namespace lantern {
// Host timestamps are QPC microseconds; local timestamps are monotonic nanoseconds.
// Audio hardware is the master. No audio anchor means video stays low latency.
class AvTimeline {
    mutable std::mutex mutex_;
    uint64_t owner_ = 0, host_ = 0, local_ = 0;
    bool active_ = false, anchored_ = false;
    bool synchronize_ = false;
    std::vector<int64_t> skew_;
    uint64_t fallbacks_ = 0;

  public:
    void synchronize(bool enabled) {
        std::lock_guard lock(mutex_);
        synchronize_ = enabled;
    }
    bool synchronized() const {
        std::lock_guard lock(mutex_);
        return synchronize_;
    }
    uint64_t begin_audio() {
        std::lock_guard lock(mutex_);
        active_ = true;
        anchored_ = false;
        return ++owner_;
    }
    void end_audio(uint64_t owner) {
        std::lock_guard lock(mutex_);
        if (owner == owner_) {
            active_ = false;
            anchored_ = false;
        }
    }
    void anchor(uint64_t owner, uint64_t host, uint64_t local) {
        std::lock_guard lock(mutex_);
        if (owner == owner_ && active_ && host && local) {
            host_ = host;
            local_ = local;
            anchored_ = true;
        }
    }
    int64_t deadline(uint64_t host, uint64_t now) {
        std::lock_guard lock(mutex_);
        if (!synchronize_ || !active_ || !anchored_ || now < local_ || now - local_ > 500'000'000)
            return int64_t(now);
        int64_t delta = int64_t(host) - int64_t(host_);
        if (delta > 650'000 || delta < -650'000) {
            ++fallbacks_;
            return int64_t(now);
        }
        int64_t at = int64_t(local_) + delta * 1000;
        if (at > int64_t(now) + 350'000'000) {
            ++fallbacks_;
            return int64_t(now);
        }
        if (at < int64_t(now) - 80'000'000)
            return -1;
        return std::max(at, int64_t(now));
    }
    void observe(uint64_t host, uint64_t now) {
        std::lock_guard lock(mutex_);
        if (!active_ || !anchored_ || now < local_ || now - local_ > 500'000'000)
            return;
        auto d = int64_t(host) - int64_t(host_) - int64_t((now - local_) / 1000);
        if (skew_.size() == 512)
            skew_.erase(skew_.begin());
        skew_.push_back(d);
    }
    std::vector<int64_t> samples() const {
        std::lock_guard lock(mutex_);
        return skew_;
    }
    uint64_t fallbacks() const {
        std::lock_guard lock(mutex_);
        return fallbacks_;
    }
};
} // namespace lantern
