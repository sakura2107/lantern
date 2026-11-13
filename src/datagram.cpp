#include "lantern/datagram.hpp"
#include "lantern/win.hpp"
#include <bcrypt.h>
namespace lantern {
namespace {
constexpr uint32_t magic=0x4c554433;
constexpr size_t header_size=56,tag_size=16;
void crypto(NTSTATUS s){if(s<0)throw Error(Code::io,"System AES-GCM operation failed");}
Bytes nonce(uint32_t direction,uint64_t sequence){Writer w;w.u32(direction);w.u64(sequence);return w.b;}
}
struct DatagramCipher::Impl {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_KEY_HANDLE key=nullptr;
    Bytes object,session;uint32_t direction;uint64_t sent=0,highest=0;
    std::bitset<2048> received;
    ~Impl(){if(key)BCryptDestroyKey(key);if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);SecureZeroMemory(object.data(),object.size());}
};
DatagramCipher::DatagramCipher(std::span<const uint8_t> key,std::span<const uint8_t> session,uint32_t direction):impl_(std::make_unique<Impl>()){
    if(key.size()!=32||session.size()!=16||!direction)throw Error(Code::invalid,"Datagram key/session bound");
    auto& p=*impl_;p.session.assign(session.begin(),session.end());p.direction=direction;
    crypto(BCryptOpenAlgorithmProvider(&p.algorithm,BCRYPT_AES_ALGORITHM,nullptr,0));
    crypto(BCryptSetProperty(p.algorithm,BCRYPT_CHAINING_MODE,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),sizeof(BCRYPT_CHAIN_MODE_GCM),0));
    DWORD size=0,written=0;crypto(BCryptGetProperty(p.algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&size),sizeof(size),&written,0));
    p.object.resize(size);crypto(BCryptGenerateSymmetricKey(p.algorithm,&p.key,p.object.data(),size,const_cast<PUCHAR>(key.data()),ULONG(key.size()),0));
}
DatagramCipher::~DatagramCipher()=default;
Bytes DatagramCipher::seal(Datagram d){
    auto& p=*impl_;
    if(d.payload.size()>datagram_payload||p.sent==UINT64_MAX)throw Error(Code::invalid,"Datagram sender bound");
    d.sequence=++p.sent;Writer w;w.u32(magic);w.u16(1);w.u16(uint16_t(d.type));
    w.b.insert(w.b.end(),p.session.begin(),p.session.end());w.u64(d.sequence);w.u64(d.frame);
    w.u32(d.index);w.u32(d.count);w.u32(d.total);w.u32(uint32_t(d.payload.size()));
    auto iv=nonce(p.direction,d.sequence);Bytes cipher(d.payload.size()),tag(tag_size);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce=iv.data();info.cbNonce=ULONG(iv.size());info.pbAuthData=w.b.data();info.cbAuthData=ULONG(w.b.size());
    info.pbTag=tag.data();info.cbTag=ULONG(tag.size());ULONG written=0;
    crypto(BCryptEncrypt(p.key,d.payload.data(),ULONG(d.payload.size()),&info,nullptr,0,cipher.data(),ULONG(cipher.size()),&written,0));
    w.b.insert(w.b.end(),cipher.begin(),cipher.end());w.b.insert(w.b.end(),tag.begin(),tag.end());return w.b;
}
std::optional<Datagram> DatagramCipher::open(std::span<const uint8_t> bytes){
    auto& p=*impl_;
    if(bytes.size()<header_size+tag_size||bytes.size()>header_size+tag_size+datagram_payload)return {};
    try{
        Reader r{bytes};if(r.u32()!=magic||r.u16()!=1)return {};auto type=r.u16();
        if(type<1||type>7||!std::equal(p.session.begin(),p.session.end(),bytes.begin()+8))return {};
        r.at+=16;Datagram d;d.type=DatagramType(type);d.sequence=r.u64();d.frame=r.u64();d.index=r.u32();d.count=r.u32();d.total=r.u32();auto size=r.u32();
        if(!d.sequence||size>datagram_payload||bytes.size()!=header_size+size+tag_size)return {};
        if(d.sequence<=p.highest&&(p.highest-d.sequence>=p.received.size()||p.received[size_t(p.highest-d.sequence)]))return {};
        auto iv=nonce(p.direction,d.sequence);d.payload.resize(size);Bytes tag(bytes.end()-tag_size,bytes.end());
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce=iv.data();info.cbNonce=ULONG(iv.size());info.pbAuthData=const_cast<PUCHAR>(bytes.data());info.cbAuthData=header_size;
        info.pbTag=tag.data();info.cbTag=tag_size;ULONG written=0;
        if(BCryptDecrypt(p.key,const_cast<PUCHAR>(bytes.data()+header_size),size,&info,nullptr,0,d.payload.data(),size,&written,0)<0)return {};
        // Authenticated packets alone advance the window. Forged high sequence numbers cannot evict it.
        if(d.sequence>p.highest){auto delta=d.sequence-p.highest;if(delta>=p.received.size())p.received.reset();else p.received<<=size_t(delta);p.highest=d.sequence;}
        p.received.set(size_t(p.highest-d.sequence));return d;
    }catch(const Error&){return {};}
}
void fragment_datagram(uint64_t frame,std::span<const uint8_t> bytes,const std::function<void(Datagram)>& send){
    if(bytes.empty()||bytes.size()>max_video_frame)throw Error(Code::invalid,"Datagram frame bound");
    uint32_t count=uint32_t((bytes.size()+datagram_payload-1)/datagram_payload);
    Bytes parity(datagram_payload);
    for(uint32_t i=0;i<count;i++){
        if(i%8==0)std::fill(parity.begin(),parity.end(),0);
        auto part=bytes.subspan(size_t(i)*datagram_payload,std::min(size_t(datagram_payload),bytes.size()-size_t(i)*datagram_payload));
        for(size_t j=0;j<part.size();j++)parity[j]^=part[j];
        send({DatagramType::data,0,frame,i,count,uint32_t(bytes.size()),Bytes(part.begin(),part.end())});
        if(i%8==7||i+1==count)send({DatagramType::parity,0,frame,i/8,count,uint32_t(bytes.size()),parity});
    }
}
std::optional<Bytes> DatagramFrames::accept(const Datagram& d,uint64_t now){
    if(d.type!=DatagramType::data&&d.type!=DatagramType::parity)return {};
    if(!d.total||d.total>max_video_frame||d.count!=(d.total+datagram_payload-1)/datagram_payload||
        (have_delivered_&&d.frame<=delivered_)||d.payload.size()>datagram_payload)return {};
    if((d.type==DatagramType::data&&(d.index>=d.count||d.payload.size()!=std::min(datagram_payload,d.total-d.index*datagram_payload)))||
       (d.type==DatagramType::parity&&(d.index>=(d.count+7)/8||d.payload.size()!=datagram_payload)))return {};
    std::erase_if(frames_,[&](auto& e){return now-e.second.began>200;});
    if(!frames_.contains(d.frame)){
        if(frames_.size()>=2)frames_.erase(frames_.begin());
        frames_.emplace(d.frame,Frame{d.total,d.count,now,{},{}});
    }
    auto& frame=frames_.at(d.frame);if(frame.total!=d.total||frame.count!=d.count)return {};
    (d.type==DatagramType::data?frame.data:frame.parity).try_emplace(d.index,d.payload);
    for(auto& [group,parity]:frame.parity){
        uint32_t absent=0,index=0;
        for(uint32_t i=group*8;i<std::min(frame.count,group*8+8);i++)if(!frame.data.contains(i)){++absent;index=i;}
        if(absent==1){auto recovered=parity;for(uint32_t i=group*8;i<std::min(frame.count,group*8+8);i++)if(i!=index){auto& present=frame.data.at(i);for(size_t j=0;j<present.size();j++)recovered[j]^=present[j];}
            recovered.resize(std::min(datagram_payload,frame.total-index*datagram_payload));frame.data.emplace(index,std::move(recovered));}
    }
    if(frame.data.size()!=frame.count)return {};
    Bytes result;result.reserve(frame.total);for(auto& [index,bytes]:frame.data)result.insert(result.end(),bytes.begin(),bytes.end());
    delivered_=d.frame;have_delivered_=true;std::erase_if(frames_,[&](auto& e){return e.first<=delivered_;});return result;
}
std::vector<uint32_t> DatagramFrames::missing(uint64_t frame,uint64_t now)const {
    std::vector<uint32_t> out;auto it=frames_.find(frame);if(it==frames_.end()||now-it->second.began<15||now-it->second.began>160)return out;
    for(uint32_t i=0;i<it->second.count&&out.size()<128;i++)if(!it->second.data.contains(i))out.push_back(i);return out;
}
} // namespace lantern
