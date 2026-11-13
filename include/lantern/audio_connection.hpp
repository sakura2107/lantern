#pragma once
#include "windows_client.hpp"
#include "udp_media.hpp"
namespace lantern {
struct AudioPacket {uint64_t sequence,pts_us;uint32_t flags;Bytes payload;};
class AudioConnection {
    std::shared_ptr<Client> stream_;std::unique_ptr<UdpMediaClient> udp_;
    bool using_udp_=false;uint32_t version_=2;uint64_t sequence_=0,last_pts_=0;
  public:
    AudioConfig config{};uint32_t codec=0,lookahead=0;
    AudioConnection(const ClientCredentials&,bool prefer_opus=true,bool automatic_reconnect=false);
    void ready();
    AudioPacket next();
    std::string path()const{return using_udp_?"udp":"tcp";}
    void interrupt()noexcept{if(udp_)udp_->interrupt();stream_->interrupt();}
};
} // namespace lantern
