#include "lantern/av_timeline.hpp"
#include "lantern/media_receiver.hpp"
#include <iostream>
using namespace lantern;
static void verify_timeline_ring() {
    AvTimeline timeline;
    if (!timeline.samples().empty())
        throw std::runtime_error("timeline samples should start empty");
    auto owner1 = timeline.begin_audio();
    constexpr uint64_t base_host = 1'000'000;
    constexpr uint64_t base_local = 10'000'000'000ULL;
    timeline.anchor(owner1, base_host, base_local);
    // Old owner must not replace or end a newer audio owner.
    auto owner2 = timeline.begin_audio();
    timeline.anchor(owner1, base_host + 999, base_local + 1000);
    timeline.observe(base_host + 5, base_local);
    if (!timeline.samples().empty())
        throw std::runtime_error("stale audio owner must not anchor timeline");
    timeline.anchor(owner2, base_host, base_local);
    timeline.end_audio(owner1);
    auto check_sequence = [&](size_t total) {
        auto s = timeline.samples();
        size_t expected_size = std::min<size_t>(512, total);
        if (s.size() != expected_size)
            throw std::runtime_error("timeline ring sample count mismatch");
        int64_t first_expected = int64_t(total - expected_size);
        for (size_t i = 0; i < expected_size; ++i) {
            if (s[i] != first_expected + int64_t(i))
                throw std::runtime_error("timeline ring oldest-to-newest order mismatch");
        }
    };
    for (int64_t i = 0; i < 1200; ++i) {
        // Choose host = base_host + i and now = base_local so skew == i.
        timeline.observe(base_host + uint64_t(i), base_local);
        if (i == 0 || i == 15 || i == 510 || i == 511 || i == 512 || i == 700 || i == 1199)
            check_sequence(size_t(i + 1));
    }
    timeline.end_audio(owner2);
}
struct ScriptedConnection {
    std::mutex mutex;std::condition_variable changed;bool stopped=false;
    void ready(){}
    int next(){std::unique_lock l(mutex);if(!stopped){changed.wait_for(l,std::chrono::milliseconds(25),[&]{return stopped;});if(!stopped)return 1;}throw TransportError("interrupted",0);}
    void interrupt(){std::lock_guard l(mutex);stopped=true;changed.notify_all();}
};
int main(){try{
    verify_timeline_ring();
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
