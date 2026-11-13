#include "lantern/desktop_connection.hpp"
namespace lantern {
DesktopConnection::DesktopConnection(ClientCredentials c,uint32_t monitor,QualitySettings settings,DecoderLimits limits,bool automatic)
    :credentials_(std::move(c)),settings_(settings) {
    AdaptiveQuality::validate(settings);
    stream_=connect_subscription(credentials_,view,1,automatic);
    auto caps_bytes=stream_->call(Op::capabilities);Reader caps_reader{caps_bytes};auto caps=caps_reader.str();caps_reader.end();
    version_=caps.find(";datagram=av-v1")!=caps.npos?4:caps.find(";desktop-quality=v3")!=caps.npos?3:caps.find("av-clock=host-qpc-v1")!=caps.npos?2:1;
    if(!limits.modes.empty()&&caps.find(";decoder-table=v1")!=caps.npos)version_=5;
    uint64_t capacity=0;
    if(version_>=3&&settings.automatic) {
        Writer probe;probe.u32(1);auto started=std::chrono::steady_clock::now();
        auto result=stream_->call(Op::desktop_probe,std::move(probe));Reader r{result};auto bytes=r.blob(256*1024);r.end();
        auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        capacity=uint64_t(bytes.size()*8/std::max(.001,elapsed));
    }
    Writer open;open.u32(version_);open.u32(monitor);
    open.u32(version_>=3?settings.width:std::min(1920u,settings.width));
    open.u32(version_>=3?settings.fps:std::min(60u,settings.fps));
    open.u32(std::clamp(AdaptiveQuality::target(settings.width,settings.fps),2'000'000u,30'000'000u));
    if(version_>=3){open.u32(limits.hevc);open.u32(limits.width);open.u32(limits.height);open.u32(limits.fps);open.u32(settings.automatic);open.u64(capacity);}
    if(version_==5){if(limits.modes.empty()||limits.modes.size()>64)throw Error(Code::invalid,"Decoder mode count");
        decoder_limit(limits.modes,settings.fps,1920,1080);open.u32(uint32_t(limits.modes.size()));
        for(auto mode:limits.modes){open.u32(mode.width);open.u32(mode.height);open.u32(mode.fps);}}
    auto response=stream_->call(Op::desktop_stream,std::move(open));Reader r{response};
    if(r.u32()!=version_)throw Error(Code::invalid,"Desktop version");
    config_.width=r.u32();config_.height=r.u32();config_.fps=r.u32();config_.bitrate=r.u32();
    config_.capture_provider=r.str();config_.encoder_provider=r.str();config_.headers=r.blob(65536);
    if(version_>=2&&r.str()!="host-qpc-us")throw Error(Code::invalid,"Desktop clock domain");
    if(version_>=3){id_=r.str(64);config_.codec=r.str(16);if(!valid_sha(id_))throw Error(Code::invalid,"Desktop identifier");}
    if(version_>=4){auto offer=read_udp_offer(r);try{udp_=std::make_unique<UdpMediaClient>(Endpoint{stream_->selected_endpoint->host,stream_->selected_endpoint->port},offer);}catch(const Error&) {}}
    r.end();
    if(!config_.width||config_.width>3840||!config_.height||config_.height>2160||!config_.fps||config_.fps>120||
        (config_.codec!="h264"&&config_.codec!="hevc"))throw Error(Code::invalid,"Desktop configuration");
    status_={config_.width,config_.fps,config_.bitrate,config_.width,false,false,config_.codec,"tcp"};
}
void DesktopConnection::ready(){
    bool requested=false;if(udp_)try{requested=udp_->handshake();}catch(const Error&){}
    Writer w;w.u32(1);if(version_>=4)w.u32(requested);stream_->send(Op::desktop_ready,std::move(w));
    if(version_>=4){auto bytes=stream_->receive(Op::media_path);Reader r{bytes};auto path=r.str();r.end();
        if(path!="udp"&&path!="tcp")throw Error(Code::invalid,"Media path");using_udp_=path=="udp";}
}
DesktopPacket DesktopConnection::next(){
    for(;;){
    Bytes bytes;
    if(using_udp_){
        try{auto packet=udp_->receive();Reader header{packet};
            if(header.u16()!=protocol_version||header.u16()!=uint16_t(Op::desktop_packet)||header.u32()!=0)throw Error(Code::invalid,"UDP desktop envelope");
            bytes=Bytes(packet.begin()+header.at,packet.end());
        }catch(const TransportError&){udp_->interrupt();using_udp_=false;
            auto event=stream_->receive(Op::media_path);Reader path{event};if(path.str()!="tcp")throw Error(Code::invalid,"TCP fallback path");path.end();}
    }
    if(!using_udp_)bytes=stream_->receive(Op::desktop_packet);
    Reader r{bytes};DesktopPacket p;
    p.frame.sequence=r.u64();p.frame.clock_us=p.frame.pts_us=r.u64();p.frame.flags=r.u32();
    bool gap=p.frame.sequence!=sequence_;
    if((version_<4&&gap)||p.frame.sequence<sequence_||!p.frame.pts_us||p.frame.pts_us<=last_pts_||(p.frame.flags&~3u))throw Error(Code::invalid,"Desktop frame order");
    sequence_=p.frame.sequence+1;
    if(gap&&!waiting_keyframe_){QualitySettings desired;{std::lock_guard l(control_mutex_);desired=settings_;}set_quality(desired,true);waiting_keyframe_=true;}
    last_pts_=p.frame.pts_us;p.config=config_;
    if(version_>=3){
        p.config.width=r.u32();p.config.height=r.u32();p.config.fps=r.u32();p.config.bitrate=r.u32();p.config.codec=r.str(16);
        auto headers=r.blob(65536);if(!headers.empty())p.config.headers=std::move(headers);
        p.status.selected_width=r.u32();auto constrained=r.u32(),dropping=r.u32();
        if(constrained>1||dropping>1||p.config.width<160||p.config.width>3840||!p.config.height||p.config.height>2160||
           !p.config.fps||p.config.fps>120||p.config.codec!=config_.codec)throw Error(Code::invalid,"Desktop update configuration");
        p.status.constrained=constrained!=0;p.status.dropping=dropping!=0;
    }else p.status.selected_width=config_.width;
    p.frame.annex_b=r.blob(version_>=3?max_video_frame-512:max_frame-256);r.end();
    if(p.frame.annex_b.empty())throw Error(Code::invalid,"Empty desktop frame");
    p.status.width=p.config.width;p.status.fps=p.config.fps;p.status.bitrate=p.config.bitrate;p.status.codec=p.config.codec;
    p.status.path=using_udp_?"udp":"tcp";
    config_=p.config;{std::lock_guard l(status_mutex_);status_=p.status;}
    if(waiting_keyframe_){if(!(p.frame.flags&1))continue;waiting_keyframe_=false;}
    return p;
    }
}
QualityStatus DesktopConnection::quality(){std::lock_guard l(status_mutex_);return status_;}
void DesktopConnection::set_quality(QualitySettings q,bool keyframe){
    AdaptiveQuality::validate(q);std::lock_guard l(control_mutex_);
    if(version_<3)throw Error(Code::unsupported,"Host does not support online quality");
    auto c=connect_subscription(credentials_,view,0,true);Writer w;w.u32(1);w.str(id_);w.u32(q.width);w.u32(q.fps);w.u32(q.automatic);w.u32(keyframe);
    uint64_t measured=0;
    if(q.automatic&&!settings_.automatic){Writer probe;probe.u32(1);auto began=std::chrono::steady_clock::now();auto bytes=c->call(Op::desktop_probe,std::move(probe));Reader r{bytes};auto sample=r.blob(256*1024);r.end();
        measured=uint64_t(sample.size()*8/std::max(.001,std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count()));}
    w.u64(measured);c->call(Op::desktop_update,std::move(w));settings_=q;
}
std::vector<DisplayLimits> DesktopConnection::limits(const ClientCredentials& c){
    auto client=c.connect(view,0);Writer w;w.u32(1);auto bytes=client->call(Op::desktop_limits,std::move(w));Reader r{bytes};
    if(r.u32()!=1)throw Error(Code::unsupported,"Desktop limits version");auto n=r.u32();
    if(n>64)throw Error(Code::invalid,"Display count");std::vector<DisplayLimits> out;
    for(uint32_t i=0;i<n;i++){DisplayLimits d{};d.index=r.u32();d.width=r.u32();d.height=r.u32();d.refresh=r.u32();d.name=r.str();auto avc=r.u32(),hevc=r.u32();
        if(avc>1||hevc>1)throw Error(Code::invalid,"Encoder capabilities");d.avc=avc!=0;d.hevc=hevc!=0;out.push_back(std::move(d));}r.end();return out;
}
} // namespace lantern
