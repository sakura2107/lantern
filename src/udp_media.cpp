#include "lantern/udp_media.hpp"
#include <deque>
#include <bcrypt.h>
#include <thread>
#include <ws2tcpip.h>
namespace lantern {
namespace {
using Clock=std::chrono::steady_clock;
uint64_t milliseconds(){return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count());}
Bytes random_bytes(size_t size){Bytes bytes(size);if(BCryptGenRandom(nullptr,bytes.data(),ULONG(size),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw Error(Code::io,"UDP key generation");return bytes;}
std::string key(std::span<const uint8_t> bytes){return std::string(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
bool same(const sockaddr_storage& a,int an,const sockaddr_storage& b,int bn){return an==bn&&!memcmp(&a,&b,size_t(an));}
void timeout(SOCKET s,DWORD ms){setsockopt(s,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&ms),sizeof(ms));}
SOCKET udp_socket(const std::string& host,uint16_t port,bool bind_socket,sockaddr_storage& address,int& address_size){
    init_sockets();ADDRINFOA hints{};hints.ai_family=AF_UNSPEC;hints.ai_socktype=SOCK_DGRAM;hints.ai_protocol=IPPROTO_UDP;
    ADDRINFOA* list=nullptr;auto service=std::to_string(port);
    if(getaddrinfo(host.c_str(),service.c_str(),&hints,&list))throw Error(Code::io,"UDP address unavailable");
    SOCKET selected=INVALID_SOCKET;
    for(auto* a=list;a;a=a->ai_next){auto s=socket(a->ai_family,SOCK_DGRAM,IPPROTO_UDP);if(s==INVALID_SOCKET)continue;
        if(bind_socket){if(a->ai_family==AF_INET6){DWORD no=0;setsockopt(s,IPPROTO_IPV6,IPV6_V6ONLY,reinterpret_cast<const char*>(&no),sizeof(no));}
            if(bind(s,a->ai_addr,int(a->ai_addrlen))){closesocket(s);continue;}}
        else if(connect(s,a->ai_addr,int(a->ai_addrlen))){closesocket(s);continue;}
        selected=s;address_size=int(a->ai_addrlen);memcpy(&address,a->ai_addr,a->ai_addrlen);break;
    }
    freeaddrinfo(list);if(selected==INVALID_SOCKET)throw Error(Code::io,"UDP socket unavailable");
    timeout(selected,20);return selected;
}
}
struct UdpMediaSession::Impl {
    mutable std::mutex mutex;
    UdpOffer offer;
    DatagramCipher outgoing,incoming;
    std::function<void(std::span<const uint8_t>,const sockaddr_storage&,int)> transmit;
    std::function<bool()> cancelled;
    sockaddr_storage peer{},candidate{};int peer_size=0,candidate_size=0;
    Bytes challenge;
    bool verified=false;
    uint64_t last_feedback=0,last_challenge=0;
    uint64_t sent_frame=0,ack_frame=0,last_loss=0;
    std::map<uint64_t,Bytes> frames;
    std::deque<std::pair<uint64_t,uint32_t>> retransmit;
    Impl(UdpOffer o,decltype(transmit) send,decltype(cancelled) stop):offer(std::move(o)),
        outgoing(offer.send_key,offer.session,1),incoming(offer.receive_key,offer.session,2),transmit(std::move(send)),cancelled(std::move(stop)){}
    ~Impl(){SecureZeroMemory(offer.send_key.data(),offer.send_key.size());SecureZeroMemory(offer.receive_key.data(),offer.receive_key.size());}
    void received(std::span<const uint8_t> bytes,const sockaddr_storage& source,int size){
        std::lock_guard l(mutex);if(cancelled())return;
        auto decoded=incoming.open(bytes);if(!decoded)return;auto& packet=*decoded;
        auto now=milliseconds();
        if(packet.type==DatagramType::hello){
            if(packet.payload.size()!=32||now-last_challenge<250)return;
            // One small challenge only; no media until proof of return reachability (anti-amplification).
            candidate=source;candidate_size=size;challenge=random_bytes(32);last_challenge=now;
            auto reply=outgoing.seal({DatagramType::challenge,0,0,0,0,0,challenge});transmit(reply,source,size);return;
        }
        if(packet.type==DatagramType::proof&&packet.payload==challenge&&!challenge.empty()&&
            now-last_challenge<1500&&same(source,size,candidate,candidate_size)){
            peer=source;peer_size=size;verified=true;last_feedback=now;challenge.clear();
            auto reply=outgoing.seal({DatagramType::feedback,0,0,0,0,0,Bytes{1}});transmit(reply,peer,peer_size);return;
        }
        if(!verified||!same(source,size,peer,peer_size))return;
        if(packet.type==DatagramType::feedback){last_feedback=now;if(packet.frame<=sent_frame)ack_frame=std::max(ack_frame,packet.frame);return;}
        if(packet.type==DatagramType::nack){
            last_feedback=now;
            last_loss=now;
            try{Reader r{packet.payload};auto n=r.u32();if(n>128)return;
                auto frame=frames.find(packet.frame);if(frame==frames.end())return;
                auto count=(frame->second.size()+datagram_payload-1)/datagram_payload;
                for(uint32_t i=0;i<n;i++){auto index=r.u32();if(index<count&&retransmit.size()<128)retransmit.emplace_back(packet.frame,index);}r.end();
            }catch(const Error&){}
        }
    }
};
UdpMediaSession::UdpMediaSession(UdpOffer offer,decltype(Impl::transmit) send,std::function<bool()> stop):p_(std::make_unique<Impl>(std::move(offer),std::move(send),std::move(stop))){}
UdpMediaSession::~UdpMediaSession()=default;
UdpOffer UdpMediaSession::offer()const {return p_->offer;}
bool UdpMediaSession::ready()const {std::lock_guard l(p_->mutex);return p_->verified&&!p_->cancelled();}
bool UdpMediaSession::live()const {std::lock_guard l(p_->mutex);return p_->verified&&!p_->cancelled()&&milliseconds()-p_->last_feedback<1500;}
bool UdpMediaSession::congested(uint32_t fps)const {std::lock_guard l(p_->mutex);return p_->sent_frame>p_->ack_frame+std::max(3u,fps/5)||milliseconds()-p_->last_loss<500;}
void UdpMediaSession::send_frame(uint64_t id,std::span<const uint8_t> bytes){
    auto& p=*p_;
    auto transmit=[&](Datagram d){
        Bytes packet;sockaddr_storage address{};int size;
        {std::lock_guard l(p.mutex);if(p.cancelled())throw Error(Code::cancelled,"UDP session ended");
            packet=p.outgoing.seal(std::move(d));address=p.peer;size=p.peer_size;}
        p.transmit(packet,address,size);
    };
    std::vector<Datagram> repeats;
    {
        std::lock_guard l(p.mutex);
        p.sent_frame=id;
        for(size_t n=0;n<16&&!p.retransmit.empty();n++){
            auto [frame,index]=p.retransmit.front();p.retransmit.pop_front();auto it=p.frames.find(frame);if(it==p.frames.end())continue;
            auto offset=size_t(index)*datagram_payload;if(offset>=it->second.size())continue;
            auto count=uint32_t((it->second.size()+datagram_payload-1)/datagram_payload);
            auto end=std::min(it->second.size(),offset+datagram_payload);
            repeats.push_back({DatagramType::data,0,frame,index,count,uint32_t(it->second.size()),Bytes(it->second.begin()+offset,it->second.begin()+end)});
        }
        p.frames[id]=Bytes(bytes.begin(),bytes.end());size_t total=0;for(auto& [_,frame]:p.frames)total+=frame.size();
        while(p.frames.size()>2||total>max_video_frame){auto it=p.frames.begin();total-=it->second.size();p.frames.erase(it);}
    }
    for(auto& packet:repeats)transmit(std::move(packet));
    fragment_datagram(id,bytes,transmit);
}
struct UdpMediaServer::Impl {
    SOCKET socket=INVALID_SOCKET;uint16_t port;std::atomic_bool stopped=false;
    std::thread worker;std::mutex mutex;std::map<std::string,std::weak_ptr<UdpMediaSession>> sessions;
    Impl(const std::string& bind,uint16_t p):port(p){sockaddr_storage address{};int size=0;socket=udp_socket(bind,p,true,address,size);
        size=sizeof(address);getsockname(socket,reinterpret_cast<sockaddr*>(&address),&size);port=ntohs(address.ss_family==AF_INET6?reinterpret_cast<sockaddr_in6*>(&address)->sin6_port:reinterpret_cast<sockaddr_in*>(&address)->sin_port);
        worker=std::thread([this]{run();});}
    ~Impl(){stopped=true;closesocket(socket);if(worker.joinable())worker.join();}
    void send(std::span<const uint8_t> packet,const sockaddr_storage& address,int size){
        if(stopped)throw Error(Code::cancelled,"UDP listener stopped");
        if(sendto(socket,reinterpret_cast<const char*>(packet.data()),int(packet.size()),0,reinterpret_cast<const sockaddr*>(&address),size)<0){
            auto error=WSAGetLastError();if(error!=WSAEWOULDBLOCK&&error!=WSAENOBUFS)throw TransportError("UDP send failed",error);
        }
    }
    void run(){uint64_t window=milliseconds();uint32_t packets=0;
        while(!stopped){
            std::array<uint8_t,1200> bytes{};sockaddr_storage source{};int size=sizeof(source);
            int n=recvfrom(socket,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0,reinterpret_cast<sockaddr*>(&source),&size);
            if(n<72)continue;auto now=milliseconds();if(now-window>=1000){window=now;packets=0;}if(++packets>4096)continue;
            std::shared_ptr<UdpMediaSession> session;
            {std::lock_guard l(mutex);auto it=sessions.find(key(std::span(bytes).subspan(8,16)));if(it!=sessions.end())session=it->second.lock();}
            if(session)try{session->p_->received(std::span(bytes).first(size_t(n)),source,size);}catch(const Error&){}
        }
    }
};
UdpMediaServer::UdpMediaServer(const std::string& bind,uint16_t port):p_(std::make_unique<Impl>(bind,port)){}
UdpMediaServer::~UdpMediaServer()=default;
std::shared_ptr<UdpMediaSession> UdpMediaServer::create(std::function<bool()> stop,std::function<void(size_t)> gate){
    UdpOffer offer{random_bytes(16),random_bytes(32),random_bytes(32),p_->port};
    auto session=std::make_shared<UdpMediaSession>(offer,[this,gate=std::move(gate)](auto packet,const auto& address,int size){gate(packet.size());p_->send(packet,address,size);},std::move(stop));
    std::lock_guard l(p_->mutex);std::erase_if(p_->sessions,[](auto& e){return e.second.expired();});
    if(p_->sessions.size()>=4)throw Error(Code::busy,"UDP session capacity");p_->sessions[key(offer.session)]=session;return session;
}
struct UdpMediaClient::Impl {
    SOCKET socket=INVALID_SOCKET;DatagramCipher outgoing,incoming;DatagramFrames frames;std::atomic_bool stopped=false;
    uint64_t last_feedback=0,last_delivered=0;
    Impl(const Endpoint& endpoint,const UdpOffer& offer):outgoing(offer.receive_key,offer.session,2),incoming(offer.send_key,offer.session,1){
        sockaddr_storage address{};int size=0;socket=udp_socket(endpoint.host,offer.port,false,address,size);
    }
    ~Impl(){if(socket!=INVALID_SOCKET)closesocket(socket);}
    void send(Datagram d){auto packet=outgoing.seal(std::move(d));if(::send(socket,reinterpret_cast<const char*>(packet.data()),int(packet.size()),0)<0)throw TransportError("UDP feedback failed",WSAGetLastError());}
    std::optional<Datagram> read(){std::array<uint8_t,1200> bytes{};auto n=recv(socket,reinterpret_cast<char*>(bytes.data()),int(bytes.size()),0);return n>0?incoming.open(std::span(bytes).first(size_t(n))):std::optional<Datagram>{};}
};
UdpMediaClient::UdpMediaClient(const Endpoint& endpoint,const UdpOffer& offer):p_(std::make_unique<Impl>(endpoint,offer)){}
UdpMediaClient::~UdpMediaClient()=default;
bool UdpMediaClient::handshake(std::chrono::milliseconds timeout){
    auto end=Clock::now()+timeout;uint64_t hello=0;
    while(!p_->stopped&&Clock::now()<end){
        if(milliseconds()-hello>=300){p_->send({DatagramType::hello,0,0,0,0,0,random_bytes(32)});hello=milliseconds();}
        auto packet=p_->read();if(!packet)continue;
        if(packet->type==DatagramType::challenge&&packet->payload.size()==32)p_->send({DatagramType::proof,0,0,0,0,0,packet->payload});
        else if(packet->type==DatagramType::feedback&&packet->payload==Bytes{1})return true;
    }return false;
}
Bytes UdpMediaClient::receive(){
    auto end=Clock::now()+std::chrono::milliseconds(1500);uint64_t current=0,last_nack=0;bool have=false;
    while(!p_->stopped&&Clock::now()<end){
        auto packet=p_->read();auto now=milliseconds();
        if(now-p_->last_feedback>=100){p_->send({DatagramType::feedback,0,p_->last_delivered,0,0,0,Bytes{1}});p_->last_feedback=now;}
        if(packet&&(packet->type==DatagramType::data||packet->type==DatagramType::parity)){
            current=packet->frame;have=true;auto frame=p_->frames.accept(*packet,now);if(frame){p_->last_delivered=current;p_->send({DatagramType::feedback,0,current,0,0,0,Bytes{1}});p_->last_feedback=now;return *frame;}
        }
        if(have&&now-last_nack>=20){auto missing=p_->frames.missing(current,now);if(!missing.empty()){Writer w;w.u32(uint32_t(missing.size()));for(auto index:missing)w.u32(index);p_->send({DatagramType::nack,0,current,0,0,0,std::move(w.b)});}last_nack=now;}
    }
    throw TransportError("UDP media timed out");
}
void UdpMediaClient::interrupt()noexcept {p_->stopped=true;shutdown(p_->socket,SD_BOTH);}
void write_udp_offer(Writer& w,const UdpOffer& o){w.u32(o.port);w.blob(o.session);w.blob(o.send_key);w.blob(o.receive_key);}
UdpOffer read_udp_offer(Reader& r){UdpOffer o;auto port=r.u32();o.session=r.blob(16);o.send_key=r.blob(32);o.receive_key=r.blob(32);
    if(!port||port>65535||o.session.size()!=16||o.send_key.size()!=32||o.receive_key.size()!=32)throw Error(Code::invalid,"UDP offer bound");o.port=uint16_t(port);return o;}
} // namespace lantern
