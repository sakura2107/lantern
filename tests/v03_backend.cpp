#include "lantern/desktop_connection.hpp"
#include "lantern/windows_input_channel.hpp"
#include "lantern/host_admin.hpp"
#include "lantern/audio_connection.hpp"
#include "lantern/media_receiver.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <thread>
#include <ws2tcpip.h>
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
struct UdpTestPeer {
    SOCKET s = INVALID_SOCKET;
    explicit UdpTestPeer(uint16_t server_port) {
        init_sockets();
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        check(s != INVALID_SOCKET, "udp test peer socket");
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0;
        check(bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0, "udp test peer bind");
        sockaddr_in remote{};
        remote.sin_family = AF_INET;
        remote.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        remote.sin_port = htons(server_port);
        check(connect(s, reinterpret_cast<sockaddr*>(&remote), sizeof(remote)) == 0, "udp test peer connect");
    }
    ~UdpTestPeer() {
        if (s != INVALID_SOCKET)
            closesocket(s);
    }
    void send_raw(std::span<const uint8_t> packet) const {
        int n = ::send(s, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0);
        check(n == int(packet.size()), "udp test peer send");
    }
    std::optional<Bytes> recv_raw(DWORD timeout_ms) const {
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        std::array<uint8_t, 1200> buf{};
        int n = ::recv(s, reinterpret_cast<char*>(buf.data()), int(buf.size()), 0);
        if (n <= 0)
            return {};
        return Bytes(buf.begin(), buf.begin() + n);
    }
    std::optional<Datagram> recv_open(DatagramCipher& rx, DWORD timeout_ms) const {
        auto raw = recv_raw(timeout_ms);
        if (!raw)
            return {};
        return rx.open(*raw);
    }
    void drain() const {
        while (recv_raw(20)) {}
    }
};
static void udp_endpoint_lock() {
    using Clock = std::chrono::steady_clock;
    UdpMediaServer server("127.0.0.1", 0);
    auto session = server.create([] { return false; }, [](size_t) {});
    auto offer = session->offer();
    DatagramCipher tx(offer.receive_key, offer.session, 2);
    DatagramCipher rx(offer.send_key, offer.session, 1);
    UdpTestPeer peer_a(offer.port), peer_b(offer.port);

    check(!session->ready() && !session->live(), "udp session starts unbound");

    // 1. Unbound: HELLO must be 32 bytes; valid 32-byte HELLO sets candidate with >= 250 ms challenge interval.
    peer_a.send_raw(tx.seal({DatagramType::hello, 0, 0, 0, 0, 0, Bytes(31, 0x11)}));
    check(!peer_a.recv_raw(100) && !session->ready(), "non-32-byte HELLO ignored");

    peer_a.send_raw(tx.seal({DatagramType::hello, 0, 0, 0, 0, 0, Bytes(32, 0x22)}));
    auto ch1 = peer_a.recv_open(rx, 500);
    auto ch1_at = Clock::now();
    check(ch1 && ch1->type == DatagramType::challenge && ch1->payload.size() == 32 && !session->ready(),
          "valid 32-byte HELLO establishes candidate and returns challenge");

    // Challenge rate limit (< 250 ms).
    peer_a.send_raw(tx.seal({DatagramType::hello, 0, 0, 0, 0, 0, Bytes(32, 0x23)}));
    check(!peer_a.recv_raw(100), "HELLO within 250ms rate limit receives no challenge");

    // 5 (pre-lock). Auth failure gives zero response and preserves candidate and replay window.
    auto good_hello_reuse = tx.seal({DatagramType::hello, 0, 0, 0, 0, 0, Bytes(32, 0x24)});
    auto bad_hello = good_hello_reuse;
    bad_hello.back() ^= 0xff;
    peer_b.send_raw(bad_hello);
    check(!peer_b.recv_raw(100) && !session->ready(), "forged HELLO receives zero response");

    // 2. PROOF must match current candidate, challenge, and < 1500 ms window.
    peer_b.send_raw(tx.seal({DatagramType::proof, 0, 0, 0, 0, 0, ch1->payload}));
    check(!peer_b.recv_raw(100) && !session->ready(), "PROOF from non-candidate rejected");

    auto wrong_ch = ch1->payload;
    wrong_ch[0] ^= 0x01;
    peer_a.send_raw(tx.seal({DatagramType::proof, 0, 0, 0, 0, 0, wrong_ch}));
    check(!peer_a.recv_raw(100) && !session->ready(), "PROOF with mismatched challenge rejected");

    std::this_thread::sleep_until(ch1_at + std::chrono::milliseconds(1580));
    peer_a.send_raw(tx.seal({DatagramType::proof, 0, 0, 0, 0, 0, ch1->payload}));
    check(!peer_a.recv_raw(100) && !session->ready(), "expired PROOF (>= 1500ms) rejected");

    // Re-send original uncorrupted good_hello_reuse: proves forged packet did not consume its replay sequence.
    peer_a.send_raw(good_hello_reuse);
    auto ch2 = peer_a.recv_open(rx, 500);
    auto ch2_at = Clock::now();
    check(ch2 && ch2->type == DatagramType::challenge && ch2->payload.size() == 32,
          "auth failure did not consume replay window and HELLO succeeds after >= 250ms");

    // While still unbound, another peer can take over candidate after >= 250 ms; first valid PROOF locks it.
    std::this_thread::sleep_until(ch2_at + std::chrono::milliseconds(280));
    peer_b.send_raw(tx.seal({DatagramType::hello, 0, 0, 0, 0, 0, Bytes(32, 0x25)}));
    auto ch3 = peer_b.recv_open(rx, 500);
    auto ch3_at = Clock::now();
    check(ch3 && ch3->type == DatagramType::challenge && ch3->payload.size() == 32,
          "unbound session accepts new candidate after 250ms");

    peer_a.send_raw(tx.seal({DatagramType::proof, 0, 0, 0, 0, 0, ch3->payload}));
    check(!peer_a.recv_raw(100) && !session->ready(), "stale candidate PROOF rejected");

    peer_b.send_raw(tx.seal({DatagramType::proof, 0, 0, 0, 0, 0, ch3->payload}));
    auto fb1 = peer_b.recv_open(rx, 500);
    check(fb1 && fb1->type == DatagramType::feedback && fb1->payload == Bytes{1},
          "first valid PROOF from candidate locks peer with FEEDBACK {1}");
    check(session->ready() && session->live(), "locked session reports ready and live");

    session->send_frame(1, Bytes{0xaa, 0xbb, 0xcc});
    auto d1 = peer_b.recv_open(rx, 500);
    check(d1 && d1->type == DatagramType::data && d1->frame == 1, "locked peer receives media frame");
    check(!peer_a.recv_raw(80), "non-locked peer receives no media");
    peer_b.drain();

    // 3. Simulate lost final confirmation: locked peer re-runs HELLO -> PROOF with fresh sequences idempotently.
    std::this_thread::sleep_until(ch3_at + std::chrono::milliseconds(280));
    peer_b.send_raw(tx.seal({DatagramType::hello, 0, 0, 0, 0, 0, Bytes(32, 0x26)}));
    auto ch4 = peer_b.recv_open(rx, 500);
    auto ch4_at = Clock::now();
    check(ch4 && ch4->type == DatagramType::challenge && ch4->payload.size() == 32,
          "locked peer can restart HELLO when final confirmation is lost");
    peer_b.send_raw(tx.seal({DatagramType::proof, 0, 0, 0, 0, 0, ch4->payload}));
    auto fb2 = peer_b.recv_open(rx, 500);
    check(fb2 && fb2->type == DatagramType::feedback && fb2->payload == Bytes{1},
          "locked peer idempotently regains FEEDBACK {1}");

    // 4. After locking, another endpoint is dropped before decryption: valid packet cannot rebind or consume replay window.
    std::this_thread::sleep_until(ch4_at + std::chrono::milliseconds(280));
    auto valid_hello_from_other = tx.seal({DatagramType::hello, 0, 0, 0, 0, 0, Bytes(32, 0x27)});
    peer_a.send_raw(valid_hello_from_other);
    check(!peer_a.recv_raw(120) && !peer_b.recv_raw(50),
          "other endpoint valid HELLO silently dropped after lock");

    // Sending the exact same sealed packet from locked peer_b must succeed (replay sequence unconsumed).
    peer_b.send_raw(valid_hello_from_other);
    auto ch5 = peer_b.recv_open(rx, 500);
    check(ch5 && ch5->type == DatagramType::challenge && ch5->payload.size() == 32,
          "dropped packet from other endpoint did not consume replay window or change binding");
    peer_b.send_raw(tx.seal({DatagramType::proof, 0, 0, 0, 0, 0, ch5->payload}));
    auto fb3 = peer_b.recv_open(rx, 500);
    auto last_live_at = Clock::now();
    check(fb3 && fb3->type == DatagramType::feedback && fb3->payload == Bytes{1},
          "locked peer still owns session after other endpoint probe");

    session->send_frame(2, Bytes{1, 2, 3, 4});
    auto d2 = peer_b.recv_open(rx, 500);
    check(d2 && d2->type == DatagramType::data && d2->frame == 2 && !peer_a.recv_raw(80),
          "media remains locked to original peer");
    peer_b.drain();

    // 5. Activity timeout does not unlock peer; auth failure leaves active time and replay window intact.
    std::this_thread::sleep_until(last_live_at + std::chrono::milliseconds(1600));
    check(session->ready() && !session->live(), "activity timeout marks not-live without unlocking peer");

    auto valid_fb = tx.seal({DatagramType::feedback, 0, 2, 0, 0, 0, Bytes{1}});
    auto bad_fb = valid_fb;
    bad_fb.back() ^= 0x80;
    peer_b.send_raw(bad_fb);
    check(!peer_b.recv_raw(100) && !session->live() && session->ready(),
          "forged feedback gives zero response and does not refresh active time");

    peer_a.send_raw(tx.seal({DatagramType::hello, 0, 0, 0, 0, 0, Bytes(32, 0x28)}));
    check(!peer_a.recv_raw(100) && session->ready() && !session->live(),
          "activity timeout does not allow another endpoint to rebind");

    peer_b.send_raw(valid_fb);
    auto until = Clock::now() + std::chrono::milliseconds(500);
    while (!session->live() && Clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(session->ready() && session->live(),
          "valid feedback with same sequence restores liveness (auth failure did not consume replay window)");
    // Twenty distinct indices and 108 duplicates in a single authenticated NACK.
    // HELLO on the same socket acts as a barrier for the server's receive worker.
    using namespace std::chrono_literals;
    auto baseline=session->stats();
    session->send_frame(30,Bytes(20*1024,0x5a),80ms,true);
    auto big=session->stats();
    check(big.data_bytes-baseline.data_bytes==20*(1024+72) && big.parity_bytes-baseline.parity_bytes==3*(1024+72),
          "accepted data/parity UDP bytes");
    check(big.last_frame_span_us>=2000 && big.last_frame_span_us<80000,"large frame paced within period");
    Writer nack;nack.u32(128);
    for(unsigned i=0;i<109;++i)nack.u32(0);
    for(unsigned i=1;i<20;++i)nack.u32(i);
    peer_b.send_raw(tx.seal({DatagramType::nack,0,30,0,0,0,std::move(nack.b)}));
    peer_b.send_raw(tx.seal({DatagramType::hello,0,0,0,0,0,Bytes(32,0x29)}));
    auto read_challenge=[&]{
        for(int i=0;i<64;++i){
            auto packet=peer_b.recv_open(rx,100);
            if(!packet)break;
            if(packet->type==DatagramType::challenge)return true;
        }
        return false;
    };
    check(read_challenge(),"authenticated NACK barrier");
    check(session->stats().duplicate_repairs-baseline.duplicate_repairs==108,"duplicate repair indices collapse");
    session->send_frame(31,Bytes(120),10ms,false);
    auto after=session->stats();
    check(after.repair_bytes-baseline.repair_bytes==16*(1024+72),"16 distinct repair slots used");
    for(unsigned i=0;i<16;++i){
        auto part=peer_b.recv_open(rx,100);
        check(part && part->type==DatagramType::data && part->frame==30 && part->index==i,"repair emitted exactly once");
    }
    session->send_frame(32,Bytes(120),10ms,false);
    check(session->stats().expired_repairs-baseline.expired_repairs==4,"eviction prunes unsent repairs before draining");
    std::this_thread::sleep_for(260ms);
    Writer stale;stale.u32(1);stale.u32(0);
    peer_b.send_raw(tx.seal({DatagramType::nack,0,32,0,0,0,std::move(stale.b)}));
    peer_b.send_raw(tx.seal({DatagramType::hello,0,0,0,0,0,Bytes(32,0x2a)}));
    check(read_challenge(),"expired NACK barrier");
    auto stats=session->stats();
    check(stats.expired_repairs-baseline.expired_repairs==5 && stats.repair_bytes==after.repair_bytes,
          "cached but overdue audio request dropped");
    std::cout<<"UDP stats delta: data_bytes="<<stats.data_bytes-baseline.data_bytes
             <<" parity_bytes="<<stats.parity_bytes-baseline.parity_bytes<<" repair_bytes="<<stats.repair_bytes-baseline.repair_bytes
             <<" duplicate_repairs="<<stats.duplicate_repairs-baseline.duplicate_repairs
             <<" expired_repairs="<<stats.expired_repairs-baseline.expired_repairs
             <<" send_rejections="<<stats.send_rejections-baseline.send_rejections
             <<" large_frame_span_us="<<big.last_frame_span_us<<"\n";
    session->send_frame(33,Bytes(32*1024),8333us,true);
    auto fast=session->stats();
    for(unsigned i=0;i<36;++i)check(bool(peer_b.recv_open(rx,200)),"120 fps paced frame delivered");
    std::cout<<"120 fps, 32 KiB video: large_frame_span_us="<<fast.last_frame_span_us<<" pacing_budget_us=4000\n";
    // Measure only host-to-peer encrypted media, after the preceding challenge barrier.
    for(uint32_t payload:{120u,152u}){
        uint64_t data_bytes=0,parity_bytes=0;
        for(unsigned i=0;i<100;++i){
            uint64_t frame=100+payload*100+i;
            session->send_frame(frame,Bytes(payload,0x3c),10ms,false);
            for(auto type:{DatagramType::data,DatagramType::parity}){
                auto wire=peer_b.recv_raw(200);
                check(bool(wire),"loopback audio datagram arrived");
                auto part=rx.open(*wire);
                check(part && part->frame==frame && part->type==type && part->payload.size()==payload,
                      "loopback audio wire packet authenticates");
                (type==DatagramType::data ? data_bytes : parity_bytes)+=wire->size();
            }
        }
        check(data_bytes==100*(payload+72) && parity_bytes==data_bytes,"100-frame UDP receive byte count");
        std::cout<<"Loopback 100 x "<<payload<<" B: data_bytes="<<data_bytes<<" parity_bytes="<<parity_bytes<<"\n";
    }
    std::cout << "UDP endpoint lock checks passed (" << checks << " checks)\n";
}
int wmain(int argc,wchar_t** argv){
    try{
        if(argc<2)return 2;
        if(std::wstring(argv[1])==L"--udp-only"){udp_endpoint_lock();return 0;}
        policy();udp_endpoint_lock();
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
