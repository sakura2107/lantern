#pragma once
#include "datagram.hpp"
#include "win.hpp"
namespace lantern {
struct UdpOffer { Bytes session,send_key,receive_key;uint16_t port=0; };
// Accepted UDP payload bytes include the authenticated header and tag. Controls are excluded.
struct UdpMediaStats {
    uint64_t data_bytes=0,parity_bytes=0,repair_bytes=0,expired_repairs=0,duplicate_repairs=0;
    uint64_t send_rejections=0,last_frame_span_us=0;
};
class UdpMediaSession {
    struct Impl;std::unique_ptr<Impl> p_;
    friend class UdpMediaServer;
  public:
    UdpMediaSession(UdpOffer,std::function<bool(std::span<const uint8_t>,const sockaddr_storage&,int)>,std::function<bool()>,std::function<void(size_t)>);
    ~UdpMediaSession();
    UdpOffer offer()const;
    bool ready()const;
    bool live()const;
    bool congested(uint32_t fps)const;
    UdpMediaStats stats()const;
    void send_frame(uint64_t,std::span<const uint8_t>,std::chrono::microseconds period=std::chrono::milliseconds(16),
                    bool video=true);
};
class UdpMediaServer {
    struct Impl;std::unique_ptr<Impl> p_;
  public:
    UdpMediaServer(const std::string& bind,uint16_t port);
    ~UdpMediaServer();
    std::shared_ptr<UdpMediaSession> create(std::function<bool()> cancelled,
        std::function<void(size_t)> gate);
};
class UdpMediaClient {
    struct Impl;std::unique_ptr<Impl> p_;
  public:
    UdpMediaClient(const Endpoint&,const UdpOffer&); // Offer directions are from host's viewpoint.
    ~UdpMediaClient();
    bool handshake(std::chrono::milliseconds timeout=std::chrono::milliseconds(1500));
    Bytes receive(uint32_t fps=1); // 1.5s without authenticated data => caller waits for TLS fallback path event.
    void interrupt()noexcept;
};
void write_udp_offer(Writer&,const UdpOffer&);
UdpOffer read_udp_offer(Reader&);
} // namespace lantern
