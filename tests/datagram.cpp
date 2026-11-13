#include "lantern/udp_media.hpp"
#include <fstream>
#include <iostream>
#include <thread>
using namespace lantern;
static int checks=0;static void check(bool b,const char* s){++checks;if(!b)throw std::runtime_error(s);}
int wmain(int argc,wchar_t** argv){try{
    Bytes key(32),session(16);for(int i=0;i<32;i++)key[i]=uint8_t(i);for(int i=0;i<16;i++)session[i]=uint8_t(32+i);
    DatagramCipher sender(key,session,1),receiver(key,session,1);
    auto packet=sender.seal({DatagramType::data,0,7,0,1,3,Bytes{1,2,3}});
    if(argc>1){std::ofstream out{fs::path(argv[1])};out<<hex(packet);}
    auto valid=receiver.open(packet);check(valid&&valid->payload==Bytes{1,2,3},"AES-GCM roundtrip");
    check(!receiver.open(packet),"duplicate replay discarded");
    auto fresh=sender.seal({DatagramType::data,0,8,0,1,3,Bytes{4,5,6}});auto forged=fresh;forged[24]=255;
    check(!receiver.open(forged),"forged high sequence cannot advance replay window");
    check(receiver.open(fresh).has_value(),"valid packet accepted after forged high sequence");
    DatagramCipher wrong_direction(key,session,2);check(!wrong_direction.open(packet),"direction binds nonce/tag");
    for(size_t i=0;i<packet.size();i++){auto bad=packet;bad[i]^=0x80;DatagramCipher r(key,session,1);check(!r.open(bad),"header/body/tag mutation discarded");}
    check(!receiver.open(Bytes(1201)),"oversized packet discarded before reassembly");
    Bytes content(25000);for(size_t i=0;i<content.size();i++)content[i]=uint8_t(i*79);
    std::vector<Datagram> parts;fragment_datagram(1,content,[&](auto d){parts.push_back(std::move(d));});
    std::reverse(parts.begin(),parts.end());DatagramFrames frames;std::optional<Bytes> restored;
    for(auto& part:parts){if(part.type==DatagramType::data&&part.index%8==2)continue;auto out=frames.accept(part,1);if(out)restored=std::move(out);}
    check(restored&&*restored==content,"reordering + one lost fragment per group repaired by XOR FEC");
    DatagramFrames loss;for(auto& part:parts){if(part.type==DatagramType::data&&(part.index==0||part.index==1))continue;loss.accept(part,10);}
    auto missing=loss.missing(1,30);check(missing==std::vector<uint32_t>{0,1},"bounded NACK lists remaining losses");
    for(auto& part:parts)if(part.type==DatagramType::data&&part.index==0){auto out=loss.accept(part,31);check(out&&*out==content,"one retransmission plus parity repairs two losses");}
    check(loss.missing(1,300).empty(),"stale NACK does not prolong frame lifetime");
    UdpMediaServer server("127.0.0.1",0);std::atomic_bool cancelled=false;
    auto live=server.create([&]{return cancelled.load();},[](size_t){});auto offer=live->offer();
    UdpMediaClient client({"127.0.0.1",offer.port},offer);
    check(client.handshake(),"real UDP encrypted return-path challenge/proof");check(live->ready(),"verified UDP endpoint");
    std::jthread send([&]{std::this_thread::sleep_for(std::chrono::milliseconds(20));live->send_frame(9,content);});
    check(client.receive()==content,"real fragmented UDP media and feedback");
    send.join();cancelled=true;check(!live->ready()&&!live->live(),"revocation flag ends UDP authorization");
    std::cout<<"Datagram checks passed: "<<checks<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
