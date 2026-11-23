#pragma once
#include "core.hpp"
#include "connection_state.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
namespace lantern {
// A single command is in flight. A replay-capable transport must deduplicate (channel,sequence).
struct InputResult {Code code=Code::ok;std::string message;Bytes payload;};
class InputTransport {
  public:
    virtual ~InputTransport() = default;
    virtual bool replay_safe() const = 0;
    virtual InputResult exchange(uint64_t sequence, Op, const Bytes&) = 0;
    virtual void interrupt() noexcept = 0;
};
class InputChannel {
  public:
    using Connector = std::function<std::shared_ptr<InputTransport>()>;
    using Events = std::function<void(ConnectionState)>;
    using Results = std::function<void(uint64_t,const InputResult&)>;
    explicit InputChannel(Connector, Events = {}, Results = {});
    ~InputChannel();
    // False means bounded backpressure: the producer retains this critical event and retries.
    // Moves coalesce only at the tail, never across a click/key/scroll or an in-flight event.
    bool submit(Op, Bytes);
    ConnectionState state() const;
    uint64_t acknowledged() const;
    size_t pending() const;
    void close();
  private:
    struct Command { Op op; Bytes payload; bool move; bool internal=false; };
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Command> queue_;
    Connector connector_; Events events_;Results results_;
    std::shared_ptr<InputTransport> transport_;
    std::thread worker_;
    bool stopped_ = false, sending_ = false, terminal_ = false;
    uint64_t ack_ = 0;
    std::chrono::steady_clock::time_point recovery_{};
    void run();
    void publish();
};
// Caller holds the input mutex. Zero is reserved for an unowned button.
class PointerLease {
    uint64_t owner_ = 0;
  public:
    bool held() const noexcept { return owner_ != 0; }
    bool owns(uint64_t generation) const noexcept { return held() && owner_ == generation; }
    bool acquire(uint64_t generation) noexcept {
        if (!generation || held()) return false;
        owner_ = generation;
        return true;
    }
    bool release(uint64_t generation) noexcept {
        if (!owns(generation)) return false;
        owner_ = 0;
        return true;
    }
    uint64_t owner() const noexcept { return owner_; }
};
// Retained by one authenticated host process. Never persist raw input or identity in a journal.
class InputReplay {
    uint64_t last_ = 0;
    Op op_{}; Bytes request_, response_;
  public:
    Bytes execute(uint64_t sequence, Op op, const Bytes& request,
                  const std::function<Bytes()>& action) {
        if (!sequence || request.size() > 8192)
            throw Error(Code::invalid, "Input envelope bound");
        if (sequence == last_) {
            if (op != op_ || request != request_)
                throw Error(Code::conflict, "Input replay payload changed");
            return response_;
        }
        if (sequence != last_ + 1)
            throw Error(Code::conflict, "Input sequence expired or out of order");
        auto result = action();
        last_ = sequence; op_ = op; request_ = request; response_ = result;
        return result;
    }
};
} // namespace lantern
