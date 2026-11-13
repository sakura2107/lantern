#pragma once
#include "windows_client.hpp"
#include "quality.hpp"
#include "udp_media.hpp"
#include "decoder_limits.hpp"
namespace lantern {
struct DecoderLimits { uint32_t width=3840,height=2160,fps=120;bool hevc=false;std::vector<DecoderMode> modes; };
struct DisplayLimits { uint32_t index,width,height,refresh;std::string name;bool avc,hevc; };
struct DesktopPacket { VideoConfig config;VideoFrame frame;QualityStatus status; };
class DesktopConnection {
    ClientCredentials credentials_;
    std::shared_ptr<Client> stream_;
    std::mutex control_mutex_,status_mutex_;
    QualitySettings settings_;
    VideoConfig config_{};
    QualityStatus status_;
    std::string id_;
    uint32_t version_=1;
    uint64_t sequence_=0,last_pts_=0;
    std::unique_ptr<UdpMediaClient> udp_;
    bool using_udp_=false,waiting_keyframe_=false;
  public:
    DesktopConnection(ClientCredentials,uint32_t monitor,QualitySettings,DecoderLimits = {},bool automatic_reconnect=false);
    const VideoConfig& config() const {return config_;} // Stream owner thread only.
    void ready();
    DesktopPacket next(); // One stream owner. Changed dimensions/headers are delivered before the next IDR.
    QualityStatus quality() ;
    void set_quality(QualitySettings,bool request_keyframe=false); // Control worker, independent of next().
    void interrupt() noexcept {if(udp_)udp_->interrupt();stream_->interrupt();}
    bool online_quality() const {return version_>=3;}
    static std::vector<DisplayLimits> limits(const ClientCredentials&);
};
} // namespace lantern
