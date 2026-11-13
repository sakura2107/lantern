#pragma once
#include "connection_state.hpp"
#include "desktop_connection.hpp"
#include "audio_connection.hpp"
#include <condition_variable>
#include <thread>
namespace lantern {
// Typed stream lifecycle shared by Windows audio/video. Events execute on this owning media worker.
// Transport loss retries in the background; authorization/protocol/codec errors require explicit action.
template<class Connection,class Packet> class MediaReceiver {
  public:
    using Factory=std::function<std::shared_ptr<Connection>(bool automatic)>;
    using Frames=std::function<void(const Packet&)>;
    using Events=std::function<void(ConnectionState)>;
    using Errors=std::function<void(Code,const std::string&)>;
    MediaReceiver(Factory factory,Frames frames,Events events={},Errors errors={}):factory_(std::move(factory)),frames_(std::move(frames)),events_(std::move(events)),errors_(std::move(errors)) {status_.failure();worker_=std::thread([this]{run();});}
    ~MediaReceiver(){close();}
    ConnectionState state()const{return status_.snapshot();}
    std::shared_ptr<Connection> connection()const{std::lock_guard l(mutex_);return active_;}
    void close(){std::shared_ptr<Connection> active;{std::lock_guard l(mutex_);stopped_=true;active=active_;changed_.notify_all();}if(active)active->interrupt();if(worker_.joinable())worker_.join();}
  private:
    Factory factory_;Frames frames_;Events events_;Errors errors_;ConnectionStatus status_;
    mutable std::mutex mutex_;std::condition_variable changed_;std::shared_ptr<Connection> active_;
    std::thread worker_;bool stopped_=false;
    bool stopped(){std::lock_guard l(mutex_);return stopped_;}
    void publish(){if(events_)try{events_(state());}catch(...) {}}
    void run(){bool automatic=false;
        while(!stopped())try{
            auto stream=factory_(automatic);automatic=true;
            {std::lock_guard l(mutex_);active_=stream;if(stopped_)stream->interrupt();}
            stream->ready();
            while(!stopped()){auto packet=stream->next();bool recovering=state().phase!=ConnectionPhase::normal;
                status_.normal();if(recovering)publish();frames_(packet);}
        }catch(const Error& error){
            {std::lock_guard l(mutex_);active_.reset();}
            if(stopped())return;
            bool transient=dynamic_cast<const TransportError*>(&error)||error.code==Code::io||error.code==Code::busy;
            status_.failure(!transient);publish();
            if(!transient){if(errors_)try{errors_(error.code,error.what());}catch(...){}return;}
            std::unique_lock l(mutex_);changed_.wait_for(l,std::chrono::milliseconds(state().recovering_ms<30000?500:2000),[&]{return stopped_;});
        }catch(const std::exception& error){status_.failure(true);publish();if(errors_)try{errors_(Code::io,error.what());}catch(...){}return;}
    }
};
using DesktopReceiver=MediaReceiver<DesktopConnection,DesktopPacket>;
using AudioReceiver=MediaReceiver<AudioConnection,AudioPacket>;
} // namespace lantern
