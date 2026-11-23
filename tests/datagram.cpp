#include "lantern/udp_media.hpp"
#include <fstream>
#include <iostream>
#include <thread>
using namespace lantern;
static int checks=0;static void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
static constexpr uint32_t sizes[]={120,8192+17,8192,9216+37,152};
static Bytes fixture(uint32_t size){
    Bytes data(size);
    for(uint32_t i=0;i<size;++i)data[i]=uint8_t(i*31+(i>>7)+size);
    return data;
}
static void verify_java(const fs::path& path,const Bytes& key,const Bytes& session){
    DatagramCipher receiver(key,session,2);
    std::ifstream input(path,std::ios::binary);
    uint64_t id=10;
    DatagramFrames frames;
    size_t packets=0;
    for(auto size:sizes){
        auto data=fixture(size);
        std::optional<Bytes> recovered;
        fragment_datagram(id,data,[&](Datagram expected){
            uint8_t prefix[2];
            check(bool(input.read(reinterpret_cast<char*>(prefix),2)),"Java vector length");
            Bytes wire((uint32_t(prefix[0])<<8)|prefix[1]);
            check(wire.size()==72+expected.payload.size() && bool(input.read(reinterpret_cast<char*>(wire.data()),wire.size())),"Java packet size");
            auto d=receiver.open(wire);
            check(d && d->type==expected.type && d->frame==id && d->count==expected.count && d->index==expected.index &&
                  d->total==size && d->payload==expected.payload,"CNG opens Java directional vector");
            ++packets;
            if(d->type==DatagramType::data && (d->index%8==7 || d->index+1==d->count))return;
            if(auto out=frames.accept(*d,1))recovered=std::move(out);
        });
        check(recovered && *recovered==data,"CNG restores Java frame with last data shard per group lost");
        ++id;
    }
    check(input.peek()==EOF,"unexpected trailing Java vectors");
    std::cout<<"Java -> CNG: "<<packets<<" authenticated packets, 5 recovered frames\n";
}
static void audio_wire(const Bytes& key,const Bytes& session,uint32_t size){
    DatagramCipher cipher(key,session,1);
    size_t data=0,old_parity=0,compact=0;
    for(unsigned i=0;i<100;++i)fragment_datagram(i,fixture(size),[&](Datagram d){
        if(d.type==DatagramType::data)data+=cipher.seal(std::move(d)).size();
        else{
            compact+=cipher.seal(d).size();
            d.payload.resize(datagram_payload);old_parity+=cipher.seal(std::move(d)).size();
        }
    });
    check(data==(size+72)*100 && compact==data && old_parity==109600,"100-frame sealed wire accounting");
    std::cout<<"100 frames x "<<size<<" B: data="<<data<<", parity="<<old_parity<<" -> "<<compact
             <<", UDP payload bytes="<<data+old_parity<<" -> "<<data+compact<<"\n";
}
int wmain(int argc,wchar_t** argv){try{
    Bytes key(32),session(16);for(int i=0;i<32;i++)key[i]=uint8_t(i);for(int i=0;i<16;i++)session[i]=uint8_t(32+i);
    if(argc==3 && std::wstring(argv[1])==L"--verify-java"){verify_java(argv[2],key,session);return 0;}
    DatagramCipher sender(key,session,1),receiver(key,session,1);
    auto packet=sender.seal({DatagramType::data,0,7,0,1,3,Bytes{1,2,3}});
    auto empty_pkt=sender.seal({DatagramType::feedback,0,7,0,0,0,Bytes{}});
    Bytes full_payload(datagram_payload);for(size_t i=0;i<full_payload.size();i++)full_payload[i]=uint8_t((i*37+11)&255);
    auto full_pkt=sender.seal({DatagramType::data,0,8,0,2,datagram_payload+17,full_payload});
    Bytes short_payload(17);for(size_t i=0;i<short_payload.size();i++)short_payload[i]=uint8_t(200+i);
    auto short_pkt=sender.seal({DatagramType::data,0,8,1,2,datagram_payload+17,short_payload});
    if(argc>1){
        std::ofstream out{fs::path(argv[1])};
        out<<hex(packet)<<'\n'<<hex(empty_pkt)<<'\n'<<hex(full_pkt)<<'\n'<<hex(short_pkt)<<'\n';
        uint64_t id=10;
        for(auto size:sizes){
            fragment_datagram(id++,fixture(size),[&](Datagram d){out<<hex(sender.seal(std::move(d)))<<'\n';});
        }
        check(bool(out),"writing CNG vectors");
    }
    auto valid=receiver.open(packet);check(valid&&valid->payload==Bytes{1,2,3},"AES-GCM roundtrip");
    check(!receiver.open(packet),"duplicate replay discarded");
    auto valid_empty=receiver.open(empty_pkt);check(valid_empty&&valid_empty->payload.empty()&&valid_empty->sequence==2,"empty payload roundtrip");
    // Forged/corrupted packets (including high sequence and mutated tag/body) must leave replay state unchanged.
    auto forged_full=full_pkt;forged_full[24]=255;
    check(!receiver.open(forged_full),"forged high sequence cannot advance replay window");
    auto mutated_tag=full_pkt;mutated_tag.back()^=0x01;
    check(!receiver.open(mutated_tag),"mutated tag leaves state unchanged");
    // Deliver short packet (seq 4) before full packet (seq 3) to verify alternating lengths and out-of-order window.
    auto valid_short=receiver.open(short_pkt);check(valid_short&&valid_short->payload==short_payload&&valid_short->sequence==4,"short packet after failed auth");
    auto valid_full=receiver.open(full_pkt);check(valid_full&&valid_full->payload==full_payload&&valid_full->sequence==3,"full packet accepted out of order after auth failures");
    check(!receiver.open(full_pkt),"full packet replay discarded");
    check(!receiver.open(short_pkt),"short packet replay discarded");
    auto fresh=sender.seal({DatagramType::data,0,9,0,1,3,Bytes{4,5,6}});auto forged=fresh;forged[24]=255;
    check(!receiver.open(forged),"forged high sequence cannot advance replay window (post-alternating)");
    check(receiver.open(fresh).has_value(),"valid packet accepted after forged high sequence");
    DatagramCipher wrong_direction(key,session,2);check(!wrong_direction.open(packet),"direction binds nonce/tag");
    for(size_t i=0;i<packet.size();i++){auto bad=packet;bad[i]^=0x80;DatagramCipher r(key,session,1);check(!r.open(bad),"header/body/tag mutation discarded");}
    check(!receiver.open(Bytes(1201)),"oversized packet discarded before reassembly");
    Bytes content(25000);for(size_t i=0;i<content.size();i++)content[i]=uint8_t(i*79);
    std::vector<Datagram> parts;fragment_datagram(1,content,[&](auto d){parts.push_back(std::move(d));});
    std::reverse(parts.begin(),parts.end());DatagramFrames frames;std::optional<Bytes> restored;
    for(auto& part:parts){if(part.type==DatagramType::data&&part.index%8==2)continue;auto out=frames.accept(part,1);if(out)restored=std::move(out);}
    check(restored&&*restored==content,"reordering + one lost fragment per group repaired by XOR FEC");
    // Single-group FEC: parity before data in group 0, parity after data in group 1, tail partial group repaired by parity alone, duplicates harmless.
    DatagramFrames mixed;std::optional<Bytes> mixed_out;
    for(auto& part:parts){if(part.type==DatagramType::parity&&part.index==0){mixed.accept(part,2);mixed.accept(part,2);}}
    for(auto& part:parts){
        if(part.type==DatagramType::data&&(part.index==5||part.index==11||part.index==24))continue;
        if(part.type==DatagramType::parity&&part.index==0)continue;
        if(auto out=mixed.accept(part,2))mixed_out=std::move(out);
        check(!mixed.accept(part,2).has_value(),"duplicate fragment does not re-emit frame");
    }
    check(mixed_out&&*mixed_out==content,"single-group FEC handles parity-first, parity-last, tail-group and duplicates");
    DatagramFrames loss;for(auto& part:parts){if(part.type==DatagramType::data&&(part.index==0||part.index==1))continue;loss.accept(part,10);}
    auto missing=loss.missing(1,30);check(missing==std::vector<uint32_t>{1},"bounded NACK lists remaining losses");
    for(auto& part:parts)if(part.type==DatagramType::data&&part.index==1){auto out=loss.accept(part,31);check(out&&*out==content,"one retransmission plus parity repairs two losses");}
    check(loss.missing(1,300).empty(),"stale NACK does not prolong frame lifetime");
    Datagram tail{DatagramType::parity,0,99,0,1,120,fixture(120)};
    DatagramFrames legacy;
    tail.payload.resize(121);
    check(!legacy.accept(tail,0),"invalid intermediate parity width discarded");
    tail.payload.resize(1024);
    check(legacy.accept(tail,1)==fixture(120),"legacy padded parity restores exactly 120 bytes");
    audio_wire(key,session,120);
    audio_wire(key,session,152); // 32-byte service envelope + 120-byte Opus payload.
    DatagramFrames retained;
    Datagram partial{DatagramType::data,0,20,0,2,1041,full_payload};
    retained.accept(partial,0);partial.frame=21;retained.accept(partial,1);
    partial.frame=19;retained.accept(partial,2);
    check(retained.missing(19,18).empty() && !retained.missing(20,14,16).empty() &&
          retained.missing(20,16,16).empty(),"two-frame retention and 120 fps NACK deadline");
    partial.frame=21;retained.accept(partial,205);
    check(retained.missing(21,225).empty(),"late data cannot restart expired frame lifetime");
    partial.frame=80;retained.accept(partial,230);
    partial.frame=30;retained.accept(partial,231);
    check(retained.missing(30,245).empty(),"sequence gap cannot reopen an old NACK window");
    UdpMediaServer server("127.0.0.1",0);std::atomic_bool cancelled=false;
    auto live=server.create([&]{return cancelled.load();},[](size_t){});auto offer=live->offer();
    UdpMediaClient client({"127.0.0.1",offer.port},offer);
    check(client.handshake(),"real UDP encrypted return-path challenge/proof");check(live->ready(),"verified UDP endpoint");
    Bytes tiny_frame{9,8,7,6,5,4,3};
    Bytes med_frame(3500);for(size_t i=0;i<med_frame.size();i++)med_frame[i]=uint8_t((i*13+5)&255);
    std::jthread send([&]{
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        live->send_frame(9,tiny_frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        live->send_frame(10,content);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        live->send_frame(11,med_frame);
    });
    check(client.receive()==tiny_frame,"real UDP short frame");
    check(client.receive()==content,"real fragmented UDP large frame after short frame");
    check(client.receive()==med_frame,"real UDP medium frame after large frame");
    send.join();cancelled=true;check(!live->ready()&&!live->live(),"revocation flag ends UDP authorization");
    std::cout<<"Datagram checks passed: "<<checks<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
