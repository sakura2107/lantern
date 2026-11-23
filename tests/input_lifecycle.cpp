#include "lantern/input_channel.hpp"
#include "lantern/input_session.hpp"
#include <iostream>
#include <optional>
using namespace lantern;
static void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
namespace {
PointerLease left_button;
unsigned button_ups = 0;

void release_left_button(uint64_t generation) noexcept {
    if (left_button.release(generation))
        ++button_ups;
}

void input_session_lifecycle() {
    {
        InputSession disconnected(release_left_button);
        check(left_button.acquire(disconnected.generation()), "disconnected session holds left button");
    }
    check(!left_button.held() && button_ups == 1, "scope exit immediately releases left button once");

    std::optional<InputSession> current;
    {
        InputSession previous(release_left_button);
        check(left_button.acquire(previous.generation()), "previous session holds left button");
        previous.cancel();
        check(!left_button.held() && button_ups == 2, "explicit cancellation releases left button once");
        current.emplace(release_left_button);
        check(current->generation() != previous.generation() && left_button.acquire(current->generation()),
              "new generation acquires left button after cancellation");
    }
    check(left_button.owns(current->generation()) && button_ups == 2,
          "previous session's late destructor leaves the new generation holding left button");
    current.reset();
    check(!left_button.held() && button_ups == 3, "new generation releases its own button on scope exit");
}

void input_replay_deduplicates_press() {
    InputReplay replay;
    InputSession session(release_left_button);
    Writer request;
    request.u32(1); // Pointer down.
    request.u32(120);
    request.u32(240);
    request.u32(0);
    const Bytes expected_ack{0, 1, 42};
    unsigned presses = 0;
    auto press = [&] {
        ++presses;
        check(left_button.acquire(session.generation()), "pointer press takes the left-button lease");
        return expected_ack;
    };
    auto ack = replay.execute(1, Op::pointer_gesture, request.b, press);
    auto repeated_ack = replay.execute(1, Op::pointer_gesture, request.b, press);
    check(ack == expected_ack && repeated_ack == ack && presses == 1 && left_button.owns(session.generation()),
          "replaying the same sequence and payload returns the identical ACK and presses once");
}
} // namespace
struct IdleTransport final:InputTransport {
    std::mutex mutex;std::condition_variable changed;
    int connected=0,focus=0,actions=0,callbacks=0;bool stopped=false;
    bool replay_safe()const override{return true;}
    InputResult exchange(uint64_t,Op op,const Bytes&) override {
        std::lock_guard l(mutex);
        if(op==Op::input_focus)++focus;else ++actions;
        changed.notify_all();
        return {Code::invalid,"application rejection",{9}};
    }
    void interrupt()noexcept override{std::lock_guard l(mutex);stopped=true;changed.notify_all();}
    template<class Predicate> bool await(Predicate p,std::chrono::seconds timeout){
        std::unique_lock l(mutex);return changed.wait_for(l,timeout,p);
    }
};
int main(){try{
    input_session_lifecycle();
    input_replay_deduplicates_press();
    std::cout << "WP-SEC-05 InputSession release and InputReplay deduplication passed" << std::endl;
    auto wire=std::make_shared<IdleTransport>();
    InputChannel channel([&]{std::lock_guard l(wire->mutex);++wire->connected;wire->changed.notify_all();return wire;},{},
        [&](uint64_t,const InputResult& result){
            check(result.code==Code::invalid&&result.payload==Bytes{9},"application error preserved");
            std::lock_guard l(wire->mutex);++wire->callbacks;wire->changed.notify_all();
            throw std::runtime_error("consumer exception must not kill worker");
        });
    check(wire->await([&]{return wire->connected==1;},std::chrono::seconds(2)),"idle channel connects immediately");
    check(wire->await([&]{return wire->focus==1;},std::chrono::seconds(12)),"10 second idle focus heartbeat");
    check(channel.submit(Op::key_press,{1}),"submit after heartbeat");
    check(wire->await([&]{return wire->callbacks==1;},std::chrono::seconds(2)),"internal heartbeat hidden; application result delivered");
    check(channel.submit(Op::key_press,{2}),"submit after callback exception");
    check(wire->await([&]{return wire->callbacks==2;},std::chrono::seconds(2)),"callback exception isolated");
    check(channel.acknowledged()==3&&channel.pending()==0,"heartbeat and commands share ordered acknowledgements");
    auto start=std::chrono::steady_clock::now();channel.close();
    check(std::chrono::steady_clock::now()-start<std::chrono::seconds(1),"close interrupts idle wait");
    std::cout<<"Immediate connect, real idle heartbeat, application errors, callback isolation and close passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
