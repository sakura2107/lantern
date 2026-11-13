#include "lantern/desktop_connection.hpp"
#include "lantern/windows_input_channel.hpp"
#include "lantern/host_admin.hpp"
#include "lantern/audio_connection.hpp"
#include "lantern/media_receiver.hpp"
#include <fstream>
#include <iostream>
#include <thread>
using namespace lantern;
static int checks=0;
static void check(bool b,const char* message){++checks;if(!b)throw std::runtime_error(message);}
static void policy(){
    std::vector<DecoderMode> modes{{3840,2160,60},{2560,1440,120}};
    auto fast=decoder_limit(modes,120,3840,2160),large=decoder_limit(modes,60,3840,2160);
    check(fast.width==2560&&fast.fps==120&&large.width==3840&&large.fps==60,"nonrectangular 4K60 / 2K120 decoder limits");
    AdaptiveQuality quality;quality.configure({3840,120,true},3840,300'000'000);
    check(quality.state().selected_width==3840,"initial capacity chooses 4K");
    for(uint64_t t=1;t<=4001;t+=1000)quality.sample(t,true,false);
    check(quality.state().selected_width==3840&&quality.state().bitrate<90'000'000,"short jitter only changes bitrate");
    quality.sample(5001,true,false);check(quality.state().selected_width==2560&&quality.state().fps==120,"5 seconds drops one step, keeps fps");
    quality.sample(10001,true,false);check(quality.state().selected_width==1920,"second downstep");
    quality.sample(15001,true,false);check(quality.state().selected_width==1280,"third downstep");
    for(uint64_t t=16001;t<=31001;t+=1000)quality.sample(t,false,true);
    check(quality.state().selected_width==1920,"15 seconds raises one step");
    for(uint64_t t=32001;t<=46001;t+=1000)quality.sample(t,false,true);
    check(quality.state().selected_width==2560,"second upstep");
    for(uint64_t t=47001;t<=61001;t+=1000)quality.sample(t,false,true);
    check(quality.state().selected_width==3840,"third upstep");
    quality.configure({3840,120,false},3840);for(int i=1;i<70;i++)quality.sample(i*1000,true,false);
    check(quality.state().selected_width==3840&&quality.state().fps==120&&quality.state().dropping,"fixed mode reduces bitrate to floor and marks frame dropping");
}
struct Running {Host host;std::thread thread;Running(const fs::path& p):host(p,"127.0.0.1",0),thread([this]{host.run();}){}~Running(){host.stop();thread.join();}};
int wmain(int argc,wchar_t** argv){
    try{
        if(argc<2)return 2;policy();
        auto root=fs::absolute(argv[1])/wide(random_token().substr(0,12));fs::create_directories(root/L"share");
        HostAdministration::initialize(root/L"state",root/L"share",31);HostAdministration admin(root/L"state");auto paired=admin.add("v03 generated fixture",31);
        Running running(root/L"state");ClientCredentials c{"127.0.0.1",paired.pin,paired.secret,running.host.listening_port()};
        auto limits=DesktopConnection::limits(c);check(!limits.empty()&&limits[0].refresh>0,"monitor dimensions/refresh and encoder capabilities queried");
        auto input=c.connect(control,0);auto key=random_token();
        auto invoke=[&](uint64_t seq){Writer w;w.u32(1);w.str(key);w.u64(seq);w.u16(uint16_t(Op::input_focus));w.blob({});auto bytes=input->call(Op::input_reliable,std::move(w));Reader r{bytes};check(r.u64()==seq,"input sequence acknowledged");return bytes;};
        auto ack=invoke(1);input.reset();input=c.connect(control,0);check(invoke(1)==ack,"host dedup survives TLS reconnect");invoke(2);
        auto channel=open_input_channel(c);check(channel->submit(Op::input_focus,{}),"native input channel submit");
        auto until=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(channel->acknowledged()!=1&&std::chrono::steady_clock::now()<until)std::this_thread::sleep_for(std::chrono::milliseconds(5));
        check(channel->acknowledged()==1,"native input worker wire acknowledgement");channel->close();
        for(bool hevc:{false,true}){
            auto stream=open_desktop_stream(0,1280,60,5'000'000,hevc,true);
            for(auto width:{1280u,1920u,2560u,3840u})for(auto fps:{30u,60u,120u}){
                stream->reconfigure(width,fps,AdaptiveQuality::target(width,fps));auto frame=stream->next();
                check(stream->config().width==width&&stream->config().fps==fps&&!frame.annex_b.empty()&&(frame.flags&1),"GPU encode preset and online IDR at each size/rate");
            }
            check(stream->config().codec==(hevc?"hevc":"h264"),"HEVC preferred / H264 selected");
        }
        DesktopConnection session(c,0,{1920,60,false},{3840,2160,120,false});session.ready();auto first=session.next();
        check(first.config.codec=="h264","client requests H264 fallback");
        session.set_quality({2560,120,false},true);
        bool changed=false;for(int i=0;i<100&&!changed;i++){auto p=session.next();changed=p.config.width==std::min(2560u,limits[0].width)&&p.config.fps==std::min(120u,limits[0].refresh);}
        check(changed,"same TLS stream online quality and native refresh clamp");session.interrupt();
        {
            DecoderLimits decoder;decoder.modes={{1280,720,120},{3840,2160,30}};
            DesktopConnection paired(c,0,{3840,30,false},decoder);paired.ready();
            auto initial=paired.next();check(initial.config.width==std::min(3840u,limits[0].width)&&initial.config.fps==30,"v5 preserves large low-rate mode");
            paired.set_quality({3840,120,false},true);bool limited=false;
            for(int i=0;i<100&&!limited;i++){auto packet=paired.next();limited=packet.config.width==1280&&packet.config.fps==std::min(120u,limits[0].refresh);}
            check(limited,"v5 online change applies paired decoder capability");paired.interrupt();
        }
        {
            AudioConnection audio(c,true);audio.ready();auto packet=audio.next();
            check(audio.path()=="udp"&&packet.pts_us>0,"native authenticated UDP audio");audio.interrupt();
        }
        {
            std::atomic_int packets=0;AudioReceiver receiver([&](bool automatic){return std::make_shared<AudioConnection>(c,true,automatic);},[&](const AudioPacket&){++packets;});
            auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);while(!packets&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(10));
            check(packets>0&&receiver.state().phase==ConnectionPhase::normal,"typed media receiver reports normal on real packets");receiver.close();
        }
        {
            auto client=c.connect(view,1);Writer open;open.u32(4);open.u32(0);open.u32(1280);open.u32(30);open.u32(5'000'000);
            open.u32(0);open.u32(3840);open.u32(2160);open.u32(120);open.u32(0);open.u64(0);
            auto response=client->call(Op::desktop_stream,std::move(open));Reader config{response};
            check(config.u32()==4,"UDP-capable desktop version");for(int i=0;i<4;i++)config.u32();config.str();config.str();config.blob(65536);config.str();config.str();config.str();
            auto offer=read_udp_offer(config);config.end();
            // A wrong session identifier simulates a completely filtered UDP path: host silently drops it.
            offer.session[0]^=1;UdpMediaClient blocked({"127.0.0.1",offer.port},offer);auto began=std::chrono::steady_clock::now();
            check(!blocked.handshake(),"unrecognized/blocked UDP probe receives no amplification");
            auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();check(elapsed>=1.4&&elapsed<2,"blocked UDP falls back inside 1-2 seconds");
            Writer ready;ready.u32(1);ready.u32(0);client->send(Op::desktop_ready,std::move(ready));
            auto event=client->receive(Op::media_path);Reader path{event};check(path.str()=="tcp","fallback event reports TCP");path.end();
            check(!client->receive(Op::desktop_packet).empty(),"same authenticated TLS connection carries fallback video");client->interrupt();
        }
        std::ofstream report(root/L"v03-backend.json");report<<"{\"kind\":\"measured\",\"checks\":"<<checks<<",\"gpu_encode_configs\":24,\"auto_policy_simulated\":true,\"udp_tested\":false,\"optical_latency_measured\":false}";
        std::cout<<"V03 backend checks passed: "<<checks<<"\n";return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
