#include "lantern/input_channel.hpp"
namespace lantern {
static bool movable(Op op, const Bytes& bytes) {
    Reader r{bytes};
    return (op == Op::input && r.u32() == 0) || (op == Op::pointer_gesture && r.u32() == 2);
}
InputChannel::InputChannel(Connector c, Events e,Results results) : connector_(std::move(c)), events_(std::move(e)),results_(std::move(results)) {worker_=std::thread([this]{run();});}
InputChannel::~InputChannel() { close(); }
bool InputChannel::submit(Op op, Bytes bytes) {
    if (bytes.size()>8192) throw Error(Code::invalid,"Input queue payload bound");
    bool move = movable(op, bytes);
    std::lock_guard lock(mutex_);
    if (stopped_ || terminal_) return false;
    if (move && !queue_.empty() && queue_.back().move && queue_.back().op == op &&
        (!sending_ || queue_.size()>1)) queue_.back()={op,std::move(bytes),true};
    else {
        if (queue_.size()>=512) return false;
        queue_.push_back({op,std::move(bytes),move});
    }
    changed_.notify_all(); return true;
}
ConnectionState InputChannel::state() const {
    std::lock_guard lock(mutex_);
    auto elapsed = recovery_ == std::chrono::steady_clock::time_point{} ? 0ull :
        std::max<uint64_t>(1,uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-recovery_).count()));
    return {stopped_||terminal_||elapsed>=30000 ? ConnectionPhase::disconnected :
        elapsed ? ConnectionPhase::recovering : ConnectionPhase::normal,elapsed};
}
uint64_t InputChannel::acknowledged() const { std::lock_guard l(mutex_); return ack_; }
size_t InputChannel::pending() const { std::lock_guard l(mutex_); return queue_.size(); }
void InputChannel::publish() { if(events_) try {events_(state());} catch(...) {} }
void InputChannel::close() {
    std::shared_ptr<InputTransport> active;
    { std::lock_guard l(mutex_); stopped_=true; active=transport_; changed_.notify_all(); }
    if(active) active->interrupt();
    if(worker_.joinable()) worker_.join();
}
void InputChannel::run() {
    while(true) {
        Command command; uint64_t seq; std::shared_ptr<InputTransport> current;
        {
            std::unique_lock l(mutex_);
            changed_.wait(l,[&]{return stopped_ || !terminal_;});
            if(stopped_) break;
            current=transport_;
        }
        bool sent=false;
        try {
            if(!current) {
                current=connector_();
                {std::lock_guard l(mutex_);transport_=current;recovery_={};if(stopped_)current->interrupt();}
                publish();
            }
            {
                std::unique_lock l(mutex_);
                changed_.wait_for(l,std::chrono::seconds(10),[&]{return stopped_||!queue_.empty();});
                if(stopped_)break;
                // A harmless, acknowledged focus query keeps the replay lease and detects idle loss.
                if(queue_.empty())queue_.push_back({Op::input_focus,{},false,true});
                command=queue_.front();seq=ack_+1;sending_=true;
            }
            sent=true; auto result=current->exchange(seq,command.op,command.payload);
            {std::lock_guard l(mutex_);queue_.pop_front();ack_=seq; sending_=false;recovery_={};}
            if(results_&&!command.internal)try{results_(seq,result);}catch(...){}
            publish();
        } catch(const Error& error) {
            {std::lock_guard l(mutex_); transport_.reset();sending_=false;
                // Never replay an ambiguous action on an old host that cannot deduplicate it.
                terminal_=error.code==Code::unauthorized || error.code==Code::conflict || error.code==Code::cancelled ||
                    (sent && current && !current->replay_safe());
                if(recovery_==std::chrono::steady_clock::time_point{}) recovery_=std::chrono::steady_clock::now();}
            publish();
            std::unique_lock l(mutex_);changed_.wait_for(l,std::chrono::milliseconds(250),[&]{return stopped_;});
        } catch(...) {
            {std::lock_guard l(mutex_); terminal_=true; sending_=false;transport_.reset();}
            publish();
        }
    }
}
} // namespace lantern
