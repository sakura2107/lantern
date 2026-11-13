#include "lantern/input_channel.hpp"
#include <iostream>
using namespace lantern;
static void check(bool b,const char* what) {if(!b)throw std::runtime_error(what);}
struct Transport final:InputTransport {
    InputReplay ledger;std::atomic_int applied=0,calls=0;
    std::atomic_bool gate=false,lose=true;
    std::mutex mutex;
    std::vector<Op> order;
    bool replay_safe()const override{return true;}
    void interrupt()noexcept override{gate=true;}
    InputResult exchange(uint64_t seq,Op op,const Bytes& bytes)override{
        while(!gate)std::this_thread::sleep_for(std::chrono::milliseconds(1));
        std::lock_guard l(mutex);++calls;
        ledger.execute(seq,op,bytes,[&]{++applied;order.push_back(op);return Bytes{1};});
        if(lose.exchange(false))throw Error(Code::io,"lost ACK");
        return {};
    }
};
int main(){try{
    InputReplay log;int applied=0;
    auto action=[&]{++applied;return Bytes{9};};
    check(log.execute(1,Op::click,{2},action)==Bytes{9},"first execution");
    log.execute(1,Op::click,{2},action);check(applied==1,"ACK replay is not another click");
    bool altered=false;try{log.execute(1,Op::click,{3},action);}catch(const Error& e){altered=e.code==Code::conflict;}
    check(altered,"changed replay rejected");
    bool gap=false;try{log.execute(3,Op::click,{2},action);}catch(const Error& e){gap=e.code==Code::conflict;}
    check(gap,"gap rejected");
    auto wire=std::make_shared<Transport>();std::atomic_int connects=0;
    InputChannel channel([&]{++connects;return wire;});
    Writer move;move.u32(0);move.u32(1);move.u32(2);move.u32(0);move.u32(0);
    channel.submit(Op::input,move.b);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for(int i=0;i<1000;i++)check(channel.submit(Op::input,move.b),"tail coalescing");
    check(channel.submit(Op::click,{1}),"click accepted");
    check(channel.submit(Op::key_press,{2}),"key accepted");
    check(channel.pending()==4,"movement does not cross critical barriers or in-flight command");
    wire->gate=true;
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
    while(channel.pending()&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(2));
    check(channel.pending()==0&&channel.acknowledged()==4&&wire->applied==4&&connects==2,"lost ACK replay and reconnect");
    check(wire->order==std::vector<Op>{Op::input,Op::input,Op::click,Op::key_press},"critical order");
    channel.close();
    auto blocked=std::make_shared<Transport>();blocked->lose=false;
    InputChannel bounded([&]{return blocked;});
    for(int i=0;i<512;i++)check(bounded.submit(Op::click,{1}),"critical admission");
    check(!bounded.submit(Op::click,{1})&&bounded.pending()==512,"bounded queue gives producer backpressure");
    bounded.close();
    std::cout<<"Input channel replay/order/coalescing/backpressure checks passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
