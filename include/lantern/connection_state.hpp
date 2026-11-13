#pragma once
#include "core.hpp"
#include <functional>
namespace lantern {
enum class ConnectionPhase { normal, recovering, disconnected };
struct ConnectionState {ConnectionPhase phase;uint64_t recovering_ms;};
class ConnectionStatus {
    mutable std::mutex mutex_;
    std::chrono::steady_clock::time_point since_{};bool terminal_=false;
  public:
    void normal(){std::lock_guard l(mutex_);since_={};terminal_=false;}
    void failure(bool terminal=false){std::lock_guard l(mutex_);terminal_=terminal;if(since_==std::chrono::steady_clock::time_point{})since_=std::chrono::steady_clock::now();}
    ConnectionState snapshot()const{std::lock_guard l(mutex_);uint64_t elapsed=since_==std::chrono::steady_clock::time_point{}?0:
        std::max<uint64_t>(1,uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-since_).count()));
        return {terminal_||elapsed>=30000?ConnectionPhase::disconnected:elapsed?ConnectionPhase::recovering:ConnectionPhase::normal,elapsed};}
};
} // namespace lantern
