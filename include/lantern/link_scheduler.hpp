#pragma once
#include "core.hpp"
#include <condition_variable>
#include <functional>
#include <list>
#include <map>
namespace lantern {
// Schedules bounded TLS records from every connection against one aggregate bucket.
// The caller still uses TCP congestion control; this is application-level fairness.
class LinkScheduler {
    struct Pending {
        uint64_t flow;
        Lane lane;
        size_t bytes;
        double finish;
    };
    std::mutex mutex_;
    std::condition_variable changed_;
    std::list<Pending*> queue_;
    std::map<uint64_t, double> finish_;
    double rate_, tokens_ = 32768, virtual_ = 0;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
    static double weight(Lane l) {
        switch (l) {
        case Lane::input:
            return 32;
        case Lane::audio:
            return 16;
        case Lane::desktop:
            return 8;
        case Lane::media:
            return 4;
        default:
            return 1;
        }
    }

  public:
    explicit LinkScheduler(double rate) : rate_(rate) {}
    void rate(double bytes_per_second) {
        if (bytes_per_second < 65536 || bytes_per_second > 1024.0 * 1024 * 1024)
            throw Error(Code::invalid, "Link rate bound");
        std::lock_guard lock(mutex_);
        rate_ = bytes_per_second;
        changed_.notify_all();
    }
    void forget(uint64_t flow) {
        std::lock_guard lock(mutex_);
        finish_.erase(flow);
        changed_.notify_all();
    }
    void acquire(uint64_t flow, Lane lane, size_t bytes, const std::function<bool()>& cancelled) {
        if (!bytes || bytes > 32768)
            throw Error(Code::invalid, "TLS scheduler record bound");
        std::unique_lock lock(mutex_);
        if (queue_.size() >= 64)
            throw Error(Code::busy, "Link queue full");
        Pending p{flow, lane, bytes, std::max(virtual_, finish_[flow]) + double(bytes) / weight(lane)};
        queue_.push_back(&p);
        auto remove = [&] {
            queue_.remove(&p);
            changed_.notify_all();
        };
        try {
            for (;;) {
                if (cancelled())
                    throw Error(Code::cancelled, "Link send cancelled");
                auto now = std::chrono::steady_clock::now();
                tokens_ =
                    std::min(32768.0, tokens_ + std::chrono::duration<double>(now - last_).count() * rate_);
                last_ = now;
                auto best = std::min_element(queue_.begin(), queue_.end(),
                                             [](auto a, auto b) { return a->finish < b->finish; });
                if (best != queue_.end() && *best == &p && tokens_ >= double(bytes)) {
                    tokens_ -= double(bytes);
                    virtual_ = p.finish;
                    finish_[flow] = p.finish;
                    remove();
                    return;
                }
                changed_.wait_for(lock, std::chrono::milliseconds(2));
            }
        } catch (...) {
            remove();
            throw;
        }
    }
};
} // namespace lantern
