#include "lantern/media_receiver.hpp"
#include <iostream>
using namespace lantern;
struct ScriptedConnection {
    std::mutex mutex;std::condition_variable changed;bool stopped=false;
    void ready(){}
    int next(){std::unique_lock l(mutex);if(!stopped){changed.wait_for(l,std::chrono::milliseconds(25),[&]{return stopped;});if(!stopped)return 1;}throw TransportError("interrupted",0);}
    void interrupt(){std::lock_guard l(mutex);stopped=true;changed.notify_all();}
};
int main(){try{
    std::mutex mutex;std::condition_variable changed;bool recovering=false,disconnected=false,normal=false;std::atomic_bool available=false;
    std::atomic_int frames=0,attempts=0;
    MediaReceiver<ScriptedConnection,int> receiver([&](bool){++attempts;if(!available)throw TransportError("injected network outage",0);return std::make_shared<ScriptedConnection>();},
        [&](int){++frames;changed.notify_all();},[&](ConnectionState state){std::lock_guard l(mutex);recovering|=state.phase==ConnectionPhase::recovering;disconnected|=state.phase==ConnectionPhase::disconnected;normal|=state.phase==ConnectionPhase::normal;changed.notify_all();});
    auto start=std::chrono::steady_clock::now();
    {std::unique_lock l(mutex);if(!changed.wait_for(l,std::chrono::seconds(35),[&]{return disconnected;}))throw std::runtime_error("30 second disconnected event missing");}
    if(!recovering||std::chrono::steady_clock::now()-start<std::chrono::seconds(30))throw std::runtime_error("recovery duration policy");
    available=true;
    {std::unique_lock l(mutex);if(!changed.wait_for(l,std::chrono::seconds(4),[&]{return normal&&frames>0;}))throw std::runtime_error("background recovery stops after 30 seconds");}
    receiver.close();
    std::atomic_bool unauthorized=false;
    MediaReceiver<ScriptedConnection,int> denied([](bool)->std::shared_ptr<ScriptedConnection>{throw Error(Code::unauthorized,"revoked");},[](int){},{},
        [&](Code code,const std::string&){unauthorized=code==Code::unauthorized;changed.notify_all();});
    {std::unique_lock l(mutex);if(!changed.wait_for(l,std::chrono::seconds(2),[&]{return unauthorized.load();}))throw std::runtime_error("fatal authorization callback missing");}
    if(denied.state().phase!=ConnectionPhase::disconnected)throw std::runtime_error("revocation must disconnect immediately");denied.close();
    std::cout<<"Real 30 second recovery transition, continued background retry, first-packet normal and immediate revocation passed; attempts "<<attempts<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
