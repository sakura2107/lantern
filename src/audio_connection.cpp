#include "lantern/audio_connection.hpp"
namespace lantern {
AudioConnection::AudioConnection(const ClientCredentials& credentials,bool opus,bool automatic){
    stream_=connect_subscription(credentials,audio,4,automatic);auto bytes=stream_->call(Op::capabilities);Reader caps{bytes};auto text=caps.str();caps.end();
    version_=text.find(";datagram=av-v1")!=text.npos?3:2;
    Writer w;w.u32(version_);w.u32(opus?1:0);bytes=stream_->call(Op::audio_stream,std::move(w));Reader r{bytes};
    if(r.u32()!=version_)throw Error(Code::unsupported,"Audio version");
    config.sample_rate=r.u32();config.channels=r.u32();config.bits=r.u32();config.provider=r.str();codec=r.u32();lookahead=r.u32();
    if(version_==3){auto offer=read_udp_offer(r);try{auto& endpoint=*stream_->selected_endpoint;udp_=std::make_unique<UdpMediaClient>(Endpoint{endpoint.host,endpoint.port},offer);}catch(const Error&) {}}
    r.end();if(config.sample_rate<8000||config.sample_rate>96000||config.channels!=2||config.bits!=16||codec>1||lookahead>5760)
        throw Error(Code::invalid,"Audio format bound");
}
void AudioConnection::ready(){
    bool want=false;if(udp_)try{want=udp_->handshake();}catch(const Error&){}
    Writer w;w.u32(1);if(version_==3)w.u32(want);stream_->send(Op::audio_ready,std::move(w));
    if(version_==3){auto bytes=stream_->receive(Op::media_path);Reader r{bytes};auto path=r.str();r.end();
        if(path!="udp"&&path!="tcp")throw Error(Code::invalid,"Audio path");using_udp_=path=="udp";}
}
AudioPacket AudioConnection::next(){
    Bytes bytes;
    if(using_udp_)try{auto packet=udp_->receive();Reader r{packet};
        if(r.u16()!=protocol_version||r.u16()!=uint16_t(Op::audio_packet)||r.u32()!=0)throw Error(Code::invalid,"UDP audio envelope");
        bytes=Bytes(packet.begin()+r.at,packet.end());
    }catch(const TransportError&){udp_->interrupt();using_udp_=false;auto event=stream_->receive(Op::media_path);Reader r{event};if(r.str()!="tcp")throw Error(Code::invalid,"Audio fallback");r.end();}
    if(!using_udp_)bytes=stream_->receive(Op::audio_packet);Reader r{bytes};AudioPacket packet;
    packet.sequence=r.u64();packet.pts_us=r.u64();packet.flags=r.u32();packet.payload=r.blob(codec?1275:config.sample_rate*4/100);r.end();
    bool lost=packet.sequence!=sequence_;
    if((version_<3&&lost)||packet.sequence<sequence_||packet.pts_us<=last_pts_||(packet.flags&~3u)||
        ((packet.flags&2)!=0)!=packet.payload.empty())throw Error(Code::invalid,"Audio packet order");
    if(lost)packet.flags|=1;sequence_=packet.sequence+1;last_pts_=packet.pts_us;return packet;
}
} // namespace lantern
