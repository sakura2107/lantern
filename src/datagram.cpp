#include "lantern/datagram.hpp"
#include "lantern/win.hpp"
#include <bcrypt.h>
namespace lantern {
namespace {
constexpr uint32_t magic=0x4c554433;
constexpr size_t header_size=56,tag_size=16;
void crypto(NTSTATUS s){if(s<0)throw Error(Code::io,"System AES-GCM operation failed");}
void put_u16(uint8_t* dst,uint16_t n){dst[0]=uint8_t(n>>8);dst[1]=uint8_t(n);}
void put_u32(uint8_t* dst,uint32_t n){dst[0]=uint8_t(n>>24);dst[1]=uint8_t(n>>16);dst[2]=uint8_t(n>>8);dst[3]=uint8_t(n);}
void put_u64(uint8_t* dst,uint64_t n){for(int i=0;i<8;i++)dst[i]=uint8_t(n>>(8*(7-i)));}
std::array<uint8_t,12> nonce(uint32_t direction,uint64_t sequence){
    std::array<uint8_t,12> iv{};put_u32(iv.data(),direction);put_u64(iv.data()+4,sequence);return iv;
}
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
    d.sequence=++p.sent;
    const size_t payload_size=d.payload.size();
    Bytes out(header_size+payload_size+tag_size);
    put_u32(out.data(),magic);put_u16(out.data()+4,1);put_u16(out.data()+6,uint16_t(d.type));
    std::copy_n(p.session.data(),16,out.data()+8);
    put_u64(out.data()+24,d.sequence);put_u64(out.data()+32,d.frame);
    put_u32(out.data()+40,d.index);put_u32(out.data()+44,d.count);put_u32(out.data()+48,d.total);put_u32(out.data()+52,uint32_t(payload_size));
    auto iv=nonce(p.direction,d.sequence);
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce=iv.data();info.cbNonce=ULONG(iv.size());info.pbAuthData=out.data();info.cbAuthData=ULONG(header_size);
    info.pbTag=out.data()+header_size+payload_size;info.cbTag=ULONG(tag_size);ULONG written=0;
    crypto(BCryptEncrypt(p.key,payload_size?d.payload.data():nullptr,ULONG(payload_size),&info,nullptr,0,out.data()+header_size,ULONG(payload_size),&written,0));
    if(written!=payload_size)throw Error(Code::io,"System AES-GCM output length mismatch");
    return out;
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
        auto iv=nonce(p.direction,d.sequence);d.payload.resize(size);
        std::array<uint8_t,tag_size> tag{};std::copy_n(bytes.data()+header_size+size,tag_size,tag.data());
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;BCRYPT_INIT_AUTH_MODE_INFO(info);
        info.pbNonce=iv.data();info.cbNonce=ULONG(iv.size());info.pbAuthData=const_cast<PUCHAR>(bytes.data());info.cbAuthData=header_size;
        info.pbTag=tag.data();info.cbTag=tag_size;ULONG written=0;
        if(BCryptDecrypt(p.key,size?const_cast<PUCHAR>(bytes.data()+header_size):nullptr,size,&info,nullptr,0,size?d.payload.data():nullptr,size,&written,0)<0||written!=size)return {};
        // Authenticated packets alone advance the window. Forged high sequence numbers cannot evict it.
        if(d.sequence>p.highest){auto delta=d.sequence-p.highest;if(delta>=p.received.size())p.received.reset();else p.received<<=size_t(delta);p.highest=d.sequence;}
        p.received.set(size_t(p.highest-d.sequence));return d;
    }catch(const Error&){return {};}
}
void fragment_datagram(uint64_t frame,std::span<const uint8_t> bytes,const std::function<void(Datagram)>& send){
    if(bytes.empty()||bytes.size()>max_video_frame)throw Error(Code::invalid,"Datagram frame bound");
    uint32_t count=uint32_t((bytes.size()+datagram_payload-1)/datagram_payload);
    Bytes parity;
    for(uint32_t i=0;i<count;i++){
        auto part=bytes.subspan(size_t(i)*datagram_payload,std::min(size_t(datagram_payload),bytes.size()-size_t(i)*datagram_payload));
        if(i%8==0)parity.assign(part.size(),0); // The first shard is the longest in its group.
        for(size_t j=0;j<part.size();j++)parity[j]^=part[j];
        send({DatagramType::data,0,frame,i,count,uint32_t(bytes.size()),Bytes(part.begin(),part.end())});
        if(i%8==7||i+1==count)send({DatagramType::parity,0,frame,i/8,count,uint32_t(bytes.size()),parity});
    }
}
std::optional<Bytes> DatagramFrames::accept(const Datagram& d,uint64_t now){
    if(d.type!=DatagramType::data&&d.type!=DatagramType::parity)return {};
    if(!d.total||d.total>max_video_frame||d.count!=(d.total+datagram_payload-1)/datagram_payload||
        (have_delivered_&&d.frame<=delivered_)||d.payload.size()>datagram_payload)return {};
    const bool parity=d.type==DatagramType::parity;
    if(d.index>=(parity?(d.count+7)/8:d.count))return {};
    const auto first=parity?d.index*8:d.index;
    const auto length=std::min(datagram_payload,d.total-first*datagram_payload);
    // Also accept a legacy full-width parity shard for a short final group.
    if(d.payload.size()!=length && !(parity && d.payload.size()==datagram_payload))return {};
    if(!frames_.empty()){
        const auto newest=std::max(d.frame,frames_.rbegin()->first);
        if(newest-d.frame>=2)return {};
        std::erase_if(frames_,[&](auto& entry){return newest-entry.first>=2;});
    }
    for(auto& [_,f]:frames_)if(now-f.began>=200){f.data.clear();f.parity.clear();}
    if(!frames_.contains(d.frame))frames_.emplace(d.frame,Frame{d.total,d.count,now,0,{},{}});
    auto& frame=frames_.at(d.frame);if(now-frame.began>=200||frame.total!=d.total||frame.count!=d.count)return {};
    auto [it,inserted]=(d.type==DatagramType::data?frame.data:frame.parity).try_emplace(d.index,d.payload);
    (void)it;
    if(inserted){
        const uint32_t group=d.type==DatagramType::data?d.index/8:d.index;
        if(auto pit=frame.parity.find(group);pit!=frame.parity.end()){
            const uint32_t start=group*8,end=std::min(frame.count,start+8);
            uint32_t absent=0,index=0;
            for(uint32_t i=start;i<end;i++)if(!frame.data.contains(i)){++absent;index=i;}
            if(absent==1){
                auto recovered=pit->second;
                for(uint32_t i=start;i<end;i++)if(i!=index){auto& present=frame.data.at(i);for(size_t j=0;j<present.size();j++)recovered[j]^=present[j];}
                recovered.resize(std::min(datagram_payload,frame.total-index*datagram_payload));
                frame.data.emplace(index,std::move(recovered));
            }
        }
    }
    if(frame.data.size()!=frame.count)return {};
    Bytes result;result.reserve(frame.total);for(auto& [index,bytes]:frame.data)result.insert(result.end(),bytes.begin(),bytes.end());
    delivered_=d.frame;have_delivered_=true;std::erase_if(frames_,[&](auto& e){return e.first<=delivered_;});return result;
}
std::vector<uint32_t> DatagramFrames::missing(uint64_t frame,uint64_t now,uint32_t retention_ms)const {
    std::vector<uint32_t> out;
    auto it=frames_.find(frame);
    if(it==frames_.end())return out;
    const auto& f=it->second;
    auto age=now-f.began;
    if(age<3||age>=retention_ms)return out;
    for(uint32_t start=0;start<f.count && out.size()<128;start+=8){
        bool parity=f.parity.contains(start/8);
        // A parity shard can supply one missing index after the others arrive.
        // Wait for reordering before speculating about a group still in flight.
        if(!parity && age<std::min(15u,retention_ms/3) && frame==frames_.rbegin()->first)continue;
        for(auto i=start;i<std::min(start+8,f.count) && out.size()<128;++i){
            if(f.data.contains(i))continue;
            if(parity){parity=false;continue;}
            out.push_back(i);
        }
    }
    return out;
}
std::optional<std::pair<uint64_t,std::vector<uint32_t>>> DatagramFrames::nack(uint64_t now,uint32_t retention_ms) {
    for(auto it=frames_.rbegin();it!=frames_.rend();++it){
        if(it->second.last_nack && now-it->second.last_nack<20)continue;
        auto indices=missing(it->first,now,retention_ms);
        if(!indices.empty()){it->second.last_nack=now;return std::pair{it->first,std::move(indices)};}
    }
    return {};
}
} // namespace lantern
