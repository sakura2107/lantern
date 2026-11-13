#include "lantern/input_channel.hpp"
#include <iostream>
using namespace lantern;
static void check(bool b,const char* s){if(!b)throw std::runtime_error(s);}
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
