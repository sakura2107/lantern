#include "lantern/clipboard.hpp"
#include "lantern/discovery.hpp"
#include "lantern/host_admin.hpp"
#include "lantern/lan.hpp"
#include "lantern/link_scheduler.hpp"
#include "lantern/input_channel.hpp"
#include "lantern/quality.hpp"
#include "lantern/decoder_limits.hpp"
#include "lantern/udp_media.hpp"
#ifdef LANTERN_WITH_ADAPT_MEDIA
#include "lantern/media_transcode.hpp"
#endif
#include "lantern/win.hpp"

#include <deque>
#include <thread>
#include <ws2tcpip.h>
namespace lantern {
struct Host::Impl {
    fs::path state;
    Identity identity;
    Config config;
    FileService files;
    FileJobs file_jobs;
    std::unique_ptr<DiscoveryResponder> discovery;
    std::unique_ptr<UdpMediaServer> udp;
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic_bool stopped = false;
    LinkScheduler link{40 * 1024 * 1024};
    std::atomic_uint64_t next_flow = 1;
    std::mutex m;
    std::vector<SOCKET> active;
    struct Handshake {
        std::string address;
        std::chrono::steady_clock::time_point deadline;
        bool guarded = false, timed_out = false;
    };
    std::map<SOCKET, Handshake> handshakes;
    AuthenticationGuard auth_guard;
    uint32_t public_sessions = 0;
    std::atomic_bool internet_enabled = false;
    UpnpLease upnp,upnp_udp;
    std::string actual_bind;
    uint16_t actual_port = 0;
    struct ConnectionThread {
        std::thread thread;
        std::shared_ptr<std::atomic_bool> done;
    };
    std::vector<ConnectionThread> connections;
    struct Peer {
        std::string token, session;
        uint32_t requested;
        uint64_t epoch;
        HostSession info;
        std::shared_ptr<std::atomic_bool> cancelled;
        bool ended = false; // closed by an administrator (end session / end device)
    };
    std::map<SOCKET, Peer> authenticated;
    std::mutex pairing_mutex;
    struct PairWindow {
        std::deque<std::chrono::steady_clock::time_point> attempts;
    };
    std::map<std::string, PairWindow> pairing_windows;
    /** session=end-v1: after an administrator ends a device's sessions, that device's *automatic*
     *  reconnects are refused until it connects deliberately (clears the hold) or the hold expires.
     *  Trust is unchanged; clients that never send the flag behave exactly as before. */
    std::map<std::string, std::chrono::steady_clock::time_point> ended_holds; // token -> expiry, guarded by m
    static constexpr auto ended_hold = std::chrono::minutes(10);
    static constexpr uint32_t hello_automatic = 1;
    static constexpr const char* ended_message = "电脑已结束此设备的连接";
    void hold_ended(Peer& peer) {
        if (peer.ended)
            return;
        peer.ended = true;
        {
            std::lock_guard lock(replay_mutex);
            for(auto& [key,replay]:replay_inputs) if(key.first==peer.token) replay->input.cancel();
        }
        ended_holds[peer.token] = std::chrono::steady_clock::now() + ended_hold;
    }
    std::vector<std::thread> workers;
    std::atomic_uint video_sessions = 0;
    std::atomic_uint audio_sessions = 0;
    std::atomic_uint preview_sessions = 0;
    std::atomic_bool media_busy=false;
    struct ReplayState {
        InputReplay replay;
        InputSession input;
        uint64_t epoch;
        std::chrono::steady_clock::time_point touched;
    };
    std::mutex replay_mutex;
    std::map<std::pair<std::string,std::string>,std::unique_ptr<ReplayState>> replay_inputs;
    struct VideoControl {
        std::string owner;
        QualitySettings settings;
        uint32_t max_width=3840,max_height=2160,max_fps=120;
        std::vector<DecoderMode> decoder_modes;
        uint64_t initial_bps=0,revision=0;
        bool keyframe=false;
        QualityStatus status;
    };
    std::mutex video_mutex;
    std::map<std::string,std::shared_ptr<VideoControl>> video_controls;
    Impl(const fs::path& s, const std::string& bind, uint16_t port)
        : state(s), identity(s / L"identity.dpapi", false), config(load_config(s / L"config.dpapi")),
          files(config.root), file_jobs(files) {
        actual_bind = bind;
        try {
            listener = listen_socket(bind, port);
        } catch (...) {
            if (bind != "::")
                throw;
            actual_bind = "0.0.0.0";
            listener = listen_socket(actual_bind, port);
        }
        sockaddr_storage bound{};
        int size = sizeof(bound);
        getsockname(listener.load(), reinterpret_cast<sockaddr*>(&bound), &size);
        actual_port = ntohs(bound.ss_family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                                                        : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
        try{udp=std::make_unique<UdpMediaServer>(actual_bind,actual_port);}catch(const Error&){}
        internet_enabled = config.internet_access;
        connections.reserve(128);
        discovery = std::make_unique<DiscoveryResponder>(actual_bind, actual_port, identity.fingerprint());
    }
    ~Impl(){stop();udp.reset();} // Join UDP callbacks before scheduler/authorization members are destroyed.
    std::vector<ConnectionEndpoint> recommended(const Config& c) {
        auto addresses = host_addresses();
        std::erase_if(addresses, [&](auto& a) {
            if (actual_bind == "::")
                return false;
            if (actual_bind == "0.0.0.0")
                return numeric_address(a.address)->v6;
            return normalize_address(a.address) != normalize_address(actual_bind);
        });
        auto list = host_endpoints(c, actual_port, addresses);
        auto mapping = upnp.status();
        if (c.internet_access && mapping.ipv4.status == UpnpStatus::success) {
            auto e = connection_endpoint(mapping.ipv4.external_address, mapping.ipv4.external_port);
            e.expires_at = mapping.ipv4.expires_at;
            list.push_back(e);
        }
        return merge_endpoints({}, list, pairing_unix_time());
    }
    Bytes dispatch(Op op, Reader& r, uint32_t granted, Lane lane, InputSession& input,
                   std::unique_ptr<DirectorySearch>& search, const std::string& owner,
                   const std::string& session, std::function<bool()> media_cancel = {}) {
        auto require = [&](uint32_t flag) {
            if (!(granted & flag))
                throw Error(Code::unauthorized, "Permission denied for this action");
        };
        Writer w;
        switch (op) {
#ifdef LANTERN_WITH_ADAPT_MEDIA
        case Op::media_tracks: {
            require(files_read);if(r.u32()!=1)throw Error(Code::unsupported,"Media tracks version");auto path=r.str();r.end();
            auto tracks=media_tracks(files.media_source(path),media_cancel?media_cancel:[this]{return stopped.load();});
            w.u32(1);w.u64(tracks.source.size);w.str(tracks.source.version);w.u64(tracks.duration_ms);w.u64(tracks.bitrate);
            w.u32(tracks.width);w.u32(tracks.height);w.u32(tracks.hdr);w.u32(uint32_t(tracks.tracks.size()));
            for(auto& track:tracks.tracks){w.u32(track.index);w.u32(track.kind);w.str(track.codec);w.str(track.language);w.str(track.title);}break;
        }
        case Op::media_segment: {
            require(files_read);if(r.u32()!=3)throw Error(Code::unsupported,"Media segment version (MPEG-TS requires v3)");
            auto path=r.str(),version=r.str(256);MediaAdaptOptions options;options.width=r.u32();options.height=r.u32();
            options.audio=int32_t(r.u32());options.subtitle=int32_t(r.u32());auto segment=r.u32();r.end();
            if(media_busy.exchange(true))throw Error(Code::busy,"One adapted media segment is already encoding");
            struct Release {std::atomic_bool& busy;~Release(){busy=false;}} release{media_busy};
            auto bytes=media_segment(files.media_source(path),version,options,segment,media_cancel?media_cancel:[this]{return stopped.load();});
            w.u32(3);w.blob(bytes);break;
        }
#endif
        case Op::desktop_probe: {
            require(view);if(r.u32()!=1)throw Error(Code::unsupported,"Desktop probe version");r.end();
            w.blob(Bytes(256*1024));break;
        }
        case Op::desktop_limits: {
            require(view);if(r.u32()!=1)throw Error(Code::unsupported,"Desktop limits version");r.end();
            auto displays=monitors();w.u32(1);w.u32(uint32_t(displays.size()));
            for(uint32_t i=0;i<displays.size();++i) {
                auto& display=displays[i];w.u32(i);w.u32(display.width);w.u32(display.height);w.u32(display.refresh_hz);w.str(display.name);
                bool avc=false,hevc=false;
                try{auto probe=open_desktop_stream(i,640,30,1'000'000,true);avc=true;hevc=probe->config().codec=="hevc";}catch(const Error&){}
                w.u32(avc?1:0);w.u32(hevc?1:0);
            }break;
        }
        case Op::desktop_update: {
            require(view);if(r.u32()!=1)throw Error(Code::unsupported,"Desktop update version");
            auto id=r.str(64);QualitySettings quality;quality.width=r.u32();quality.fps=r.u32();
            auto automatic=r.u32(),keyframe=r.u32();
            auto initial_bps=r.at<r.b.size()?r.u64():0;r.end();
            if(automatic>1||keyframe>1)throw Error(Code::invalid,"Desktop update flags");
            quality.automatic=automatic!=0;AdaptiveQuality::validate(quality);
            std::lock_guard lock(video_mutex);auto it=video_controls.find(id);
            if(it==video_controls.end()||it->second->owner!=owner)throw Error(Code::not_found,"Desktop stream unavailable");
            auto& c=*it->second;
            if(initial_bps)c.initial_bps=initial_bps;
            if(c.settings.width!=quality.width||c.settings.fps!=quality.fps||c.settings.automatic!=quality.automatic){c.settings=quality;++c.revision;}
            c.keyframe|=keyframe!=0;
            auto& status=c.status;w.u32(1);w.u32(status.width);w.u32(status.fps);w.u32(status.bitrate);
            w.str(status.codec);w.str(status.path);w.u32(status.selected_width);w.u32(status.constrained);w.u32(status.dropping);
            break;
        }
        case Op::input_reliable: {
            require(control);
            if (lane != Lane::input || r.u32()!=1)
                throw Error(Code::invalid,"Reliable input channel/version");
            auto id=r.str(64); auto seq=r.u64(); auto nested=Op(r.u16()); auto payload=r.blob(8192);r.end();
            if(!valid_sha(id)) throw Error(Code::invalid,"Input channel identifier");
            switch(nested) {
            case Op::input: case Op::click: case Op::pointer_gesture: case Op::double_click:
            case Op::text_input: case Op::key_press: case Op::shortcut: case Op::right_click:
            case Op::input_focus: break;
            default: throw Error(Code::invalid,"Reliable input operation");
            }
            auto c=load_config(state/L"config.dpapi"); auto e=authorization_epoch(c,owner);
            if(!e) throw Error(Code::unauthorized,"Input trust revoked");
            std::lock_guard lock(replay_mutex);
            auto now=std::chrono::steady_clock::now();
            std::erase_if(replay_inputs,[&](auto& item) {
                return now-item.second->touched>std::chrono::minutes(10) ||
                    authorization_epoch(c,item.first.first)!=item.second->epoch;
            });
            auto key=std::make_pair(owner,id); auto it=replay_inputs.find(key);
            if(it==replay_inputs.end()) {
                if(replay_inputs.size()>=64) throw Error(Code::busy,"Reliable input channel capacity");
                auto p=std::make_unique<ReplayState>();p->epoch=*e;p->touched=now;
                it=replay_inputs.emplace(key,std::move(p)).first;
            }
            it->second->touched=now;
            return it->second->replay.execute(seq,nested,payload,[&] {
                Writer answer;answer.u64(seq);
                try {
                    Reader body{payload};auto result=dispatch(nested,body,granted,lane,it->second->input,search,owner,session);
                    answer.u32(0);answer.str("");answer.blob(result);
                } catch(const Error& error) {
                    it->second->input.cancel();answer.u32(uint32_t(error.code));answer.str(error.what());answer.blob({});
                }
                return answer.b;
            });
        }
        case Op::endpoints: {
            r.end();
            auto current = load_config(state / L"config.dpapi");
            w.u32(1);
            write_endpoints(w, recommended(current));
            break;
        }
        case Op::ping: {
            auto nonce = r.u64();
            r.end();
            w.u64(nonce);
            break;
        }
        case Op::files_page: {
            require(files_read);
            if (r.u32() != 1)
                throw Error(Code::unsupported, "File page version");
            auto path = r.str();
            auto off = r.u32(), limit = r.u32();
            auto filter = r.str(256);
            auto sort = r.u32(), direction = r.u32();
            auto expected = r.str(256);
            r.end();
            if (direction > 1)
                throw Error(Code::invalid, "Sort direction");
            auto page = files.page(path, off, limit, filter, sort, direction != 0, expected);
            w.u32(1);
            w.str(page.generation);
            w.u32(page.total);
            w.u32(uint32_t(page.entries.size()));
            for (auto& item : page.entries) {
                w.str(item.name);
                w.u32(item.directory ? 1 : 0);
                w.u64(item.size);
                w.u64(item.mtime);
            }
            break;
        }
        case Op::files_search: {
            require(files_read);
            if (r.u32() != 1)
                throw Error(Code::unsupported, "Search version");
            auto phase = r.u32();
            if (phase == 0) {
                auto path = r.str(), query = r.str(256);
                r.end();
                search = files.search(path, query);
            } else {
                r.end();
                if (phase == 2)
                    search.reset();
                else if (phase != 1)
                    throw Error(Code::invalid, "Search phase");
            }
            SearchPage page;
            if (phase == 2)
                page.done = true;
            else {
                if (!search)
                    throw Error(Code::conflict, "No active search");
                page = search->next();
                if (page.done)
                    search.reset();
            }
            w.u32(1);
            w.u32(page.done ? 1 : 0);
            w.u64(page.scanned);
            w.u32(uint32_t(page.entries.size()));
            for (auto& item : page.entries) {
                w.str(item.name);
                w.u32(item.directory ? 1 : 0);
                w.u64(item.size);
                w.u64(item.mtime);
            }
            break;
        }
        case Op::thumbnail: {
            require(files_read);
            if (r.u32() != 1)
                throw Error(Code::unsupported, "Thumbnail version");
            auto path = r.str();
            auto width = r.u32();
            r.end();
            if (width < 32 || width > 256)
                throw Error(Code::invalid, "Thumbnail width");
            if (preview_sessions.fetch_add(1) >= 2) {
                preview_sessions.fetch_sub(1);
                throw Error(Code::busy, "Two previews already active");
            }
            struct Release {
                std::atomic_uint& count;
                ~Release() {
                    count.fetch_sub(1);
                }
            } release{preview_sessions};
            auto bytes = files.preview_source(path);
            w.u32(1);
            w.blob(image_thumbnail(bytes, width));
            break;
        }
        case Op::file_job: {
            if (r.u32() != 1)
                throw Error(Code::unsupported, "Host file task version");
            auto kind = r.u32();
            auto from = r.str(), to = r.str();
            r.end();
            if (kind != 2)
                require(files_write);
            if (kind != 1)
                require(files_read);
            w.str(file_jobs.start(kind, from, to, owner, session));
            break;
        }
        case Op::file_job_poll: {
            if (r.u32() != 1)
                throw Error(Code::unsupported, "Host task poll version");
            auto id = r.str(64);
            r.end();
            auto job = file_jobs.poll(id, owner);
            require(job.kind == 2 ? files_read : files_write);
            w.u32(uint32_t(job.state));
            w.u64(job.current);
            w.u64(job.total);
            w.u32(uint32_t(job.error));
            w.str(job.message);
            w.u32(job.kind);
            w.u64(job.file.size);
            w.str(job.file.version);
            w.str(job.file.hash);
            break;
        }
        case Op::file_job_cancel: {
            if (r.u32() != 1)
                throw Error(Code::unsupported, "Host task cancel version");
            auto id = r.str(64);
            r.end();
            auto job = file_jobs.poll(id, owner);
            require(job.kind == 2 ? files_read : files_write);
            file_jobs.cancel(id, owner);
            break;
        }
        case Op::capabilities:
            r.end();
            w.str(std::string("Lantern/"
                  "1;tls=1.2;files=paged,read,write,resume,sha256,sorted-v1,search-v1,thumbnail-v1,host-jobs-"
                  "v1;desktop=gdi-jpeg,pull,input,multi-monitor;"
                  "input=atomic-click-v1,gesture-v1,unicode-ime-v1,native-focus-v1,atomic-shortcut-v1,right-"
                  "click-v1;"
                  "clipboard=text-cas-v1;trust=per-device-v1;media=original-range-external;audio=wasapi-"
                  "pcm16-v1,codec-negotiation-v2;privacy-screen="
                  "unsupported;virtual-"
                  "display=unsupported;hardware-codec=optional-nvenc-h264;desktop-stream=h264-v1,h264-clock-"
                  "v2,adaptive-v3;av-clock=host-qpc-v1;session=end-v1;pair=qr-v1;endpoints=v1;input-replay=v1;desktop-quality=v3")+(udp?";datagram=av-v1;decoder-table=v1":";datagram=unavailable")
#ifdef LANTERN_WITH_ADAPT_MEDIA
                  +";media-adapt=h264-hls-ts-v3"
#endif
            );
            break;
        case Op::list: {
            require(files_read);
            auto p = r.str();
            auto off = r.u32(), limit = r.u32();
            auto filter = r.str();
            r.end();
            std::string generation;
            auto entries = files.list(p, off, limit, filter, generation);
            w.str(generation);
            w.u32(uint32_t(entries.size()));
            for (auto& e : entries) {
                w.str(e.name);
                w.u32(e.directory ? 1 : 0);
                w.u64(e.size);
                w.u64(e.mtime);
            }
            break;
        }
        case Op::stat:
        case Op::media_stat: {
            require(files_read);
            auto p = r.str();
            r.end();
            auto f = files.stat(p, op == Op::stat);
            w.u64(f.size);
            w.str(f.version);
            w.str(f.hash);
            break;
        }
        case Op::read: {
            require(files_read);
            auto p = r.str(), v = r.str();
            auto off = r.u64();
            auto n = r.u32();
            r.end();
            if (n > chunk_size)
                throw Error(Code::invalid, "Chunk bound");
            // Outbound payload is paced at each encrypted TLS record.
            w.blob(files.read(p, v, off, n));
            break;
        }
        case Op::upload_begin: {
            require(files_write);
            auto p = r.str();
            auto size = r.u64();
            auto hash = r.str();
            r.end();
            auto [id, offset] = files.begin(p, size, hash);
            w.str(id);
            w.u64(offset);
            break;
        }
        case Op::upload_chunk: {
            require(files_write);
            auto id = r.str();
            auto off = r.u64();
            auto b = r.blob(chunk_size);
            r.end();
            // Receive-side upload backpressure is applied before dispatch and ACK.
            w.u64(files.chunk(id, off, b));
            break;
        }
        case Op::upload_commit: {
            require(files_write);
            auto id = r.str();
            r.end();
            files.commit(id);
            break;
        }
        case Op::mkdir:
        case Op::rename:
        case Op::copy:
        case Op::remove: {
            require(files_write);
            auto p = r.str(), to = r.str();
            r.end();
            files.mutate(op, p, to);
            break;
        }
        case Op::monitors: {
            require(view);
            r.end();
            auto v = lantern::monitors();
            w.u32(uint32_t(v.size()));
            for (auto& monitor : v) {
                w.u32(uint32_t(monitor.x));
                w.u32(uint32_t(monitor.y));
                w.u32(monitor.width);
                w.u32(monitor.height);
                w.str(monitor.name);
            }
            break;
        }
        case Op::frame: {
            require(view);
            auto index = r.u32(), width = r.u32(), q = r.u32();
            r.end();
            w.blob(capture(index, width, q));
            break;
        }
        case Op::double_click:
        case Op::click: {
            require(control);
            int32_t x = int32_t(r.u32()), y = int32_t(r.u32());
            auto index = r.u32();
            r.end();
            input.click(x, y, index, op == Op::double_click);
            break;
        }
        case Op::pointer_gesture: {
            require(control);
            if (lane != Lane::input)
                throw Error(Code::invalid, "Gesture requires input channel");
            auto phase = r.u32();
            int32_t x = int32_t(r.u32()), y = int32_t(r.u32());
            auto index = r.u32();
            r.end();
            input.pointer(phase, x, y, index);
            break;
        }
        case Op::text_input: {
            require(control);
            auto text = r.str(4096);
            r.end();
            input.text(text);
            break;
        }
        case Op::key_press: {
            require(control);
            auto key = r.u32(), count = r.u32();
            r.end();
            input.key(key, count);
            break;
        }
        case Op::shortcut: {
            require(control);
            auto modifiers = r.u32(), key = r.u32();
            r.end();
            input.shortcut(modifiers, key);
            break;
        }
        case Op::right_click: {
            require(control);
            auto x = int32_t(r.u32()), y = int32_t(r.u32());
            auto monitor = r.u32();
            r.end();
            input.right_click(x, y, monitor);
            break;
        }
        case Op::clipboard_read: {
            require(control);
            if (r.u32() != 1)
                throw Error(Code::unsupported, "Clipboard version");
            r.end();
            auto data = read_clipboard_text();
            w.u32(1);
            w.u32(data.sequence);
            w.u32(data.present ? 1 : 0);
            w.str(data.text);
            break;
        }
        case Op::clipboard_write: {
            require(control);
            if (r.u32() != 1)
                throw Error(Code::unsupported, "Clipboard version");
            auto expected = r.u32();
            auto text = r.str(65536);
            r.end();
            w.u32(1);
            w.u32(write_clipboard_text(nullptr, expected, text));
            break;
        }
        case Op::input_focus: {
            require(control);
            r.end();
            w.u32(input.focus());
            break;
        }
        case Op::input: {
            require(control);
            auto kind = r.u32();
            int32_t x = int32_t(r.u32()), y = int32_t(r.u32());
            auto value = r.u32(), index = r.u32();
            r.end();
            remote_input(kind, x, y, value, index);
            break;
        }
        default:
            throw Error(Code::unsupported, "Unsupported protocol operation");
        }
        return w.b;
    }
    void connection(SOCKET socket) {
        std::unique_ptr<Tls> tls;
        InputSession input;
        std::unique_ptr<DirectorySearch> search;
        std::string session;
        auto flow = next_flow.fetch_add(1);
        auto cancelled = std::make_shared<std::atomic_bool>(false);
        Lane send_lane = Lane::input;
        std::string source;
        bool guarded = false;
        bool reserved = false;
        bool authentication_timeout = false;
        {
            std::lock_guard lock(m);
            auto& h = handshakes.at(socket);
            source = h.address;
            guarded = h.guarded;
        }
        try {
            auto deadline = guarded ? std::chrono::steady_clock::now() + std::chrono::seconds(10)
                                    : std::chrono::steady_clock::time_point::max();
            tls = std::make_unique<Tls>(
                socket, &identity, "", deadline, [this, socket, source, guarded](bool timed_out) {
                    std::lock_guard lock(m);
                    auto h = handshakes.find(socket);
                    if (h != handshakes.end() && guarded) {
                        if (!h->second.timed_out &&
                            (timed_out || std::chrono::steady_clock::now() >= h->second.deadline))
                            auth_guard.timeout(source);
                        auth_guard.release(source);
                    }
                    handshakes.erase(socket);
                    std::erase(active, socket);
                });
            session = random_token();
            auto first = tls->receive(512);
            Reader auth{first};
            auto first_version = auth.u16();
            auto first_op = auth.u16();
            if (first_op == uint16_t(Op::endpoints)) {
                Writer denied;
                denied.u16(protocol_version);
                denied.u16(first_op);
                denied.u32(uint32_t(Code::unauthorized));
                denied.str("请先完成 HELLO 认证");
                tls->send(denied.b);
                throw Error(Code::unauthorized, "HELLO required");
            }
            if (first_op == uint16_t(Op::pair)) {
                Writer response;
                response.u16(protocol_version);
                response.u16(uint16_t(Op::pair));
                try {
                    // Count all PAIR frames, including invalid versions/fields, before parsing secrets.
                    sockaddr_storage peer{};
                    int length = sizeof(peer);
                    char ip[NI_MAXHOST]{};
                    if (getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &length) ||
                        getnameinfo(reinterpret_cast<sockaddr*>(&peer), length, ip, sizeof(ip), nullptr, 0,
                                    NI_NUMERICHOST))
                        throw Error(Code::busy, "无法校验配对来源，请重试");
                    {
                        std::lock_guard lock(pairing_mutex);
                        auto now = std::chrono::steady_clock::now();
                        for (auto& [key, window] : pairing_windows) {
                            (void)key;
                            while (!window.attempts.empty() &&
                                   now - window.attempts.front() >= std::chrono::minutes(1))
                                window.attempts.pop_front();
                        }
                        std::erase_if(pairing_windows,
                                      [&](auto& item) { return item.second.attempts.empty(); });
                        auto key = source_key(ip);
                        if (!pairing_windows.contains(key) && pairing_windows.size() >= 1024)
                            throw Error(Code::busy, "配对尝试过于频繁，请稍后重试");
                        auto [at, inserted] = pairing_windows.try_emplace(key);
                        (void)inserted;
                        if (at->second.attempts.size() >= 10)
                            throw Error(Code::busy, "配对尝试过于频繁，请一分钟后重试");
                        at->second.attempts.push_back(now);
                    }
                    if (first_version != protocol_version)
                        throw Error(Code::unsupported, "不支持的配对协议版本，请更新客户端");
                    auto code = auth.str(128), label = auth.str(64);
                    auth.end();
                    auto device =
                        HostAdministration(state).redeem_invite(code, label, local_pairing_source(source));
                    response.u32(0);
                    response.str(device.id);
                    response.str(device.token);
                    response.u32(device.scopes);
                } catch (const Error& error) {
                    response.b.resize(4);
                    response.u32(uint32_t(error.code));
                    response.str(error.code == Code::unauthorized  ? "配对码无效或已过期，请在电脑刷新二维码"
                                 : error.code == Code::busy        ? "配对尝试过于频繁或设备已满，请稍后重试"
                                 : error.code == Code::unsupported ? "不支持的配对协议版本"
                                                                   : "配对请求无效或无法保存，请重试");
                } catch (...) {
                    response.b.resize(4);
                    response.u32(uint32_t(Code::io));
                    response.str("配对无法完成，请重试");
                }
                tls->send(response.b);
                // Use the normal cleanup path; do not leave this socket in active.
                throw Error(Code::cancelled, "Pair connection complete");
            }
            if (first_version != protocol_version || first_op != uint16_t(Op::hello))
                throw Error(Code::unsupported, "Protocol version or HELLO required");
            auto token = auth.str(128);
            auto requested = auth.u32(), channel = auth.u32();
            // Optional trailing flags (session=end-v1). Older clients send none.
            uint32_t hello_flags = auth.at < auth.b.size() ? auth.u32() : 0;
            auth.end();
            auto auth_config = load_config(state / L"config.dpapi");
            auto reject = [&](Code code, const std::string& message) {
                Writer denied;
                denied.u16(protocol_version);
                denied.u16(uint16_t(Op::hello));
                denied.u32(uint32_t(code));
                denied.str(message);
                tls->send(denied.b);
                throw Error(code, message);
            };
            bool auth_throttled = false;
            if (guarded) {
                std::lock_guard lock(m);
                auth_throttled = !auth_guard.allowed(source, GetTickCount64());
            }
            if (auth_throttled)
                reject(Code::busy, "认证尝试过于频繁，请稍后重试");
            if (auth_config.root != config.root)
                reject(Code::conflict, "Root changed; restart host");
            auto allowed = authorized_scopes(auth_config, token);
            if (!allowed || (requested & ~*allowed)) {
                if (guarded) {
                    std::lock_guard lock(m);
                    auth_guard.failed(source, GetTickCount64());
                }
                reject(Code::unauthorized, "Pairing credential or requested permission denied");
            }
            if (channel > 4)
                reject(Code::unsupported, "Channel unavailable");
            bool refused = false;
            {
                std::lock_guard lock(m);
                auto held = ended_holds.find(token);
                if (held != ended_holds.end()) {
                    if (std::chrono::steady_clock::now() >= held->second || !(hello_flags & hello_automatic))
                        ended_holds.erase(held); // expired, or the user reconnected on purpose
                    else
                        refused = true;
                }
            }
            if (refused)
                reject(Code::cancelled, ended_message);
            Lane lane = channel == 4   ? Lane::audio
                        : channel == 2 ? Lane::media
                        : channel == 3 ? Lane::bulk
                        : channel == 1 ? Lane::desktop
                                       : Lane::input;
            bool sessions_full = false;
            {
                std::lock_guard lock(m);
                sessions_full = guarded && public_sessions >= 16;
                if (guarded && !sessions_full) {
                    ++public_sessions;
                    reserved = true;
                }
            }
            if (sessions_full)
                reject(Code::busy, "已认证会话已满，请稍后重试");
            Writer hello;
            hello.u16(protocol_version);
            hello.u16(uint16_t(Op::hello));
            hello.u32(0);
            hello.u32(requested);
            tls->send(hello.b);
            tls->clear_deadline();
            auto session_epoch = authorization_epoch(auth_config, token).value();
            HostSession info;
            info.id = session;
            info.scopes = requested;
            info.channel = channel;
            info.device_id = "legacy";
            info.label = "旧共享凭据";
            for (auto& device : auth_config.devices)
                if (device.token == token) {
                    info.device_id = device.id;
                    info.label = device.label;
                    break;
                }
            sockaddr_storage address{};
            int address_size = sizeof(address);
            char ip[NI_MAXHOST]{};
            if (getpeername(socket, reinterpret_cast<sockaddr*>(&address), &address_size) == 0 &&
                !getnameinfo(reinterpret_cast<sockaddr*>(&address), address_size, ip, sizeof(ip), nullptr, 0,
                             NI_NUMERICHOST))
                info.address = normalize_address(ip);
            {
                std::lock_guard lock(m);
                authenticated[socket] = {token,         session,         requested,
                                         session_epoch, std::move(info), cancelled};
                if (guarded) {
                    auth_guard.success(source);
                    auth_guard.release(source);
                }
                handshakes.erase(socket);
            }
            tls->set_send_gate([&, flow](size_t bytes) {
                link.acquire(flow, send_lane, bytes, [&] { return stopped || cancelled->load(); });
            });
            while (!stopped) {
                auto packet = tls->receive();
                Reader r{packet};
                auto version = r.u16();
                auto op = Op(r.u16());
                send_lane = op == Op::audio_stream                          ? Lane::audio
                            : (op == Op::desktop_stream || op == Op::frame) ? Lane::desktop
                            : (op == Op::read || op == Op::media_segment) ? (lane == Lane::media ? Lane::media : Lane::bulk)
                            : (op == Op::thumbnail || op == Op::files_page || op == Op::list ||
                               op == Op::files_search || op == Op::upload_chunk)
                                ? Lane::bulk
                                : Lane::input;
                if (op == Op::upload_chunk)
                    for (size_t at = 0; at < packet.size(); at += 16384)
                        link.acquire(flow, Lane::bulk, std::min(size_t(16384), packet.size() - at),
                                     [&] { return stopped || cancelled->load(); });
                Writer response;
                response.u16(protocol_version);
                response.u16(uint16_t(op));
                try {
                    if (version != protocol_version)
                        throw Error(Code::unsupported, "Protocol version mismatch");
                    auto current = load_config(state / L"config.dpapi");
                    auto current_allowed = authorized_scopes(current, token);
                    if (!current_allowed || authorization_epoch(current, token) != session_epoch)
                        throw Error(Code::unauthorized, "Trust revoked; reconnect after pairing");
                    if (current.root != config.root)
                        throw Error(Code::conflict, "Root changed; restart host");
                    if (op == Op::desktop_stream) {
                        if (!(requested & *current_allowed & view))
                            throw Error(Code::unauthorized, "Desktop viewing permission required");
                        if (lane != Lane::desktop)
                            throw Error(Code::invalid, "Desktop streaming requires its own channel");
                        stream_desktop(*tls, socket, r, token, session_epoch,cancelled);
                        break; // This connection belongs to the stream until it closes.
                    }
                    if (op == Op::audio_stream) {
                        if (!(requested & *current_allowed & audio))
                            throw Error(Code::unauthorized, "System audio permission required");
                        if (lane != Lane::audio)
                            throw Error(Code::invalid, "System audio requires its own channel");
                        stream_audio(*tls, socket, r, token, session_epoch,cancelled);
                        break;
                    }
                    auto body =
                        dispatch(op, r, requested & *current_allowed, lane, input, search, token, session,
                            [&,cancelled]{return stopped||cancelled->load()||peer_closed(socket);});
                    response.u32(0);
                    response.b.insert(response.b.end(), body.begin(), body.end());
                } catch (const Error& e) {
                    input.cancel();
                    search.reset();
                    response.u32(uint32_t(e.code));
                    response.str(e.what());
                } catch (const std::exception&) {
                    input.cancel();
                    search.reset();
                    response.u32(uint32_t(Code::io));
                    response.str("Host operation failed");
                }
                tls->send(response.b);
            }
        } catch (const TransportError& error) {
            authentication_timeout = error.native_code == WSAETIMEDOUT;
        } catch (...) { /* No secrets or remote paths in ambient logs. Client sees close on handshake/auth
                           failure. */
        }
        file_jobs.disconnect(session);
        {
            std::lock_guard lock(m);
            authenticated.erase(socket);
            if (reserved)
                --public_sessions;
            auto h = handshakes.find(socket);
            if (h != handshakes.end() && guarded) {
                if (!h->second.timed_out &&
                    (authentication_timeout || std::chrono::steady_clock::now() >= h->second.deadline))
                    auth_guard.timeout(source);
                auth_guard.release(source);
            }
            handshakes.erase(socket);
            active.erase(std::remove(active.begin(), active.end(), socket), active.end());
        }
        link.forget(flow);
        // Tls construction owns and closes socket, including failed handshakes.
    }
    std::shared_ptr<UdpMediaSession> udp_stream(Lane lane,std::shared_ptr<std::atomic_bool> cancelled,uint64_t flow) {
        if(!udp)throw Error(Code::unsupported,"UDP listener unavailable");
        return udp->create([this,cancelled]{return stopped||cancelled->load();},[this,lane,cancelled,flow](size_t bytes){
            link.acquire(flow,lane,bytes,[this,cancelled]{return stopped||cancelled->load();});
        });
    }
    static void media_path(Tls& tls,bool udp) {
        Writer path;path.u16(protocol_version);path.u16(uint16_t(Op::media_path));path.u32(0);path.str(udp?"udp":"tcp");tls.send(path.b);
    }
    static bool peer_closed(SOCKET socket) {
        fd_set read;FD_ZERO(&read);FD_SET(socket,&read);timeval wait{};
        if(select(0,&read,nullptr,nullptr,&wait)<=0)return false;
        char byte;return recv(socket,&byte,1,MSG_PEEK)<=0;
    }
    void stream_desktop(Tls& tls, SOCKET socket, Reader& request, const std::string& token, uint64_t epoch,
                        std::shared_ptr<std::atomic_bool> cancelled) {
        auto version = request.u32();
        if (version < 1 || version > 5)
            throw Error(Code::unsupported, "Desktop stream version mismatch");
        auto monitor = request.u32(), width = request.u32(), fps = request.u32(), bitrate = request.u32();
        bool hevc=false;
        auto controller=std::make_shared<VideoControl>();controller->owner=token;
        controller->settings={width,fps,false};
        if(version>=3) {
            auto codec=request.u32();controller->max_width=request.u32();controller->max_height=request.u32();controller->max_fps=request.u32();
            auto automatic=request.u32();controller->initial_bps=request.u64();
            if(codec>1||automatic>1||controller->max_width<640||controller->max_width>3840||
                controller->max_height<360||controller->max_height>2160||!controller->max_fps||controller->max_fps>120)
                throw Error(Code::invalid,"Desktop decoder capability bound");
            hevc=codec!=0;controller->settings.automatic=automatic!=0;AdaptiveQuality::validate(controller->settings);
        }
        if(version==5){auto n=request.u32();if(!n||n>64)throw Error(Code::invalid,"Decoder mode count");
            for(uint32_t i=0;i<n;++i)controller->decoder_modes.push_back({request.u32(),request.u32(),request.u32()});
            decoder_limit(controller->decoder_modes,fps,1920,1080);
        }
        request.end();
        if (video_sessions.fetch_add(1) >= 2) {
            video_sessions.fetch_sub(1);
            throw Error(Code::busy, "Two hardware desktop streams are already active");
        }
        struct Release {
            std::atomic_uint& count;
            ~Release() {
                count.fetch_sub(1);
            }
        } release{video_sessions};
        AdaptiveQuality adaptive;
        uint32_t ceiling=3840;
        uint32_t display_width=3840,display_height=2160;
        auto decoder_ceiling=[&]{
            if(!controller->decoder_modes.empty()){
                auto mode=decoder_limit(controller->decoder_modes,controller->settings.fps,display_width,display_height);
                controller->max_width=mode.width;controller->max_height=mode.height;controller->max_fps=mode.fps;
            }
            return std::min({3840u,display_width,controller->max_width,
                uint32_t(uint64_t(controller->max_height)*display_width/display_height)});
        };
        if(version>=3) {
            auto displays=monitors();if(monitor>=displays.size())throw Error(Code::invalid,"Monitor index");
            auto& display=displays[monitor];
            display_width=display.width;display_height=display.height;ceiling=decoder_ceiling();
            adaptive.configure(controller->settings,ceiling,controller->initial_bps);
            width=std::min(adaptive.state().width,ceiling);fps=std::min(fps,controller->max_fps);bitrate=adaptive.state().bitrate;
        }
        auto source = open_desktop_stream(monitor, width, fps, bitrate,hevc);
        const auto& cfg = source->config();
        const auto stream_id=version>=3?random_token():std::string{};
        struct EraseControl {Impl& host;const std::string& id;~EraseControl(){std::lock_guard lock(host.video_mutex);host.video_controls.erase(id);}} erase{*this,stream_id};
        if(version>=3){std::lock_guard lock(video_mutex);video_controls[stream_id]=controller;}
        Writer opened;
        opened.u16(protocol_version);
        opened.u16(uint16_t(Op::desktop_stream));
        opened.u32(0);
        opened.u32(version);
        opened.u32(cfg.width);
        opened.u32(cfg.height);
        opened.u32(cfg.fps);
        opened.u32(cfg.bitrate);
        opened.str(cfg.capture_provider);
        opened.str(cfg.encoder_provider);
        opened.blob(cfg.headers);
        if (version >= 2)
            opened.str("host-qpc-us");
        if(version>=3){opened.str(stream_id);opened.str(cfg.codec);}
        auto udp_flow=next_flow.fetch_add(1);
        struct Forget {LinkScheduler& link;uint64_t id;~Forget(){link.forget(id);}} forget{link,udp_flow};
        auto datagrams=version>=4?udp_stream(Lane::desktop,cancelled,udp_flow):nullptr;
        if(datagrams)write_udp_offer(opened,datagrams->offer());
        tls.send(opened.b);
        try {
            // One frame in flight: while a send is slow no new frame is captured, so the next one is
            // the latest picture and delay does not pile up. Only ten seconds without any progress
            // counts as a lost connection; a slow network no longer ends the stream.
            DWORD ready_timeout = 15000, send_timeout = 10000;
            int send_buffer = 256 * 1024;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ready_timeout, sizeof(ready_timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_timeout, sizeof(send_timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (const char*)&send_buffer, sizeof(send_buffer));
            auto ack = tls.receive();
            Reader ready{ack};
            if (ready.u16() != protocol_version || ready.u16() != uint16_t(Op::desktop_ready) ||
                ready.u32() != 1)
                throw Error(Code::invalid, "Decoder readiness packet required");
            auto want_udp=version>=4?ready.u32():0;
            if(want_udp>1)throw Error(Code::invalid,"UDP readiness flag");
            ready.end();
            bool using_udp=datagrams&&want_udp&&datagrams->ready();
            if(version>=4)media_path(tls,using_udp);
            auto next_auth = std::chrono::steady_clock::now();
            auto next_frame = next_auth;
            auto frame_period = std::chrono::microseconds(1'000'000 / cfg.fps);
            uint64_t revision=0,last_sample=0;bool slow=false;bool first_metadata=true;
            while (!stopped) {
                if(cancelled->load()||peer_closed(socket))throw Error(Code::cancelled,"Desktop connection ended");
                if(using_udp&&!datagrams->live()){using_udp=false;media_path(tls,false);source->request_keyframe();}
                auto began = std::chrono::steady_clock::now();
                if (began >= next_auth) {
                    auto current = load_config(state / L"config.dpapi");
                    auto allowed = authorized_scopes(current, token);
                    if (!allowed || !(*allowed & view) || authorization_epoch(current, token) != epoch)
                        throw Error(Code::unauthorized, "Desktop trust revoked");
                    if (current.root != config.root)
                        throw Error(Code::conflict, "Root changed; restart host");
                    next_auth = began + std::chrono::milliseconds(250);
                }
                bool changed=false;
                if(version>=3) {
                    std::lock_guard lock(video_mutex);
                    auto now_ms=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(began.time_since_epoch()).count());
                    if(revision!=controller->revision) {
                        ceiling=decoder_ceiling();
                        adaptive.configure(controller->settings,ceiling,controller->initial_bps);revision=controller->revision;changed=true;
                    }
                    if(now_ms-last_sample>=1000) {
                        bool shortage=slow||(using_udp&&datagrams->congested(cfg.fps));
                        adaptive.sample(now_ms,shortage,!shortage&&controller->initial_bps>uint64_t(adaptive.state().bitrate)*14/10);
                        last_sample=now_ms;changed=true;
                    }
                    if(changed)source->reconfigure(std::min(adaptive.state().width,ceiling),std::min(controller->settings.fps,controller->max_fps),adaptive.state().bitrate);
                    if(controller->keyframe){source->request_keyframe();controller->keyframe=false;}
                    controller->status=adaptive.state();controller->status.width=cfg.width;controller->status.fps=cfg.fps;controller->status.codec=cfg.codec;controller->status.path=using_udp?"udp":"tcp";
                    frame_period=std::chrono::microseconds(1'000'000/cfg.fps);
                }
                auto frame = source->next();
                Writer packet;
                packet.u16(protocol_version);
                packet.u16(uint16_t(Op::desktop_packet));
                packet.u32(0);
                packet.u64(frame.sequence);
                packet.u64(version >= 2 ? frame.clock_us : frame.pts_us);
                packet.u32(frame.flags);
                if(version>=3) {
                    packet.u32(cfg.width);packet.u32(cfg.height);packet.u32(cfg.fps);packet.u32(cfg.bitrate);packet.str(cfg.codec);
                    packet.blob(first_metadata||changed?cfg.headers:Bytes{});first_metadata=false;
                    packet.u32(adaptive.state().selected_width);packet.u32(adaptive.state().constrained);packet.u32(adaptive.state().dropping);
                }
                packet.blob(frame.annex_b,version>=3?max_video_frame-512:max_frame-256);
                auto sending=std::chrono::steady_clock::now();
                if(using_udp)datagrams->send_frame(frame.sequence,packet.b);
                else tls.send(packet.b); // TCP remains one fresh frame in flight.
                slow=std::chrono::steady_clock::now()-sending>frame_period*2;
                next_frame += frame_period;
                auto now = std::chrono::steady_clock::now();
                if (next_frame + frame_period < now)
                    next_frame = now;
                std::this_thread::sleep_until(next_frame);
            }
        } catch (const Error& error) {
            try {
                Writer packet;
                packet.u16(protocol_version);
                packet.u16(uint16_t(Op::desktop_packet));
                packet.u32(uint32_t(error.code));
                packet.str(error.what());
                tls.send(packet.b);
            } catch (...) {
            }
        }
    }
    void stream_audio(Tls& tls, SOCKET socket, Reader& request, const std::string& token, uint64_t epoch,
                      std::shared_ptr<std::atomic_bool> cancelled) {
        auto version = request.u32();
        if (version != 1 && version != 2 && version != 3)
            throw Error(Code::unsupported, "Audio stream version mismatch");
        uint32_t preferred = version >= 2 ? request.u32() : 0;
        if (preferred > 1)
            throw Error(Code::unsupported, "Audio codec unsupported");
        request.end();
        if (audio_sessions.fetch_add(1) >= 2) {
            audio_sessions.fetch_sub(1);
            throw Error(Code::busy, "Two audio streams are already active");
        }
        struct Release {
            std::atomic_uint& count;
            ~Release() {
                count.fetch_sub(1);
            }
        } release{audio_sessions};
        auto source = open_system_audio();
        const auto& cfg = source->config();
        auto encoder = preferred == 1 ? open_opus_encoder(cfg.sample_rate) : nullptr;
        uint32_t codec = encoder ? 1 : 0, lookahead = encoder ? encoder->lookahead_frames() : 0;
        Writer opened;
        opened.u16(protocol_version);
        opened.u16(uint16_t(Op::audio_stream));
        opened.u32(0);
        opened.u32(version);
        opened.u32(cfg.sample_rate);
        opened.u32(cfg.channels);
        opened.u32(cfg.bits);
        opened.str(cfg.provider);
        if (version >= 2) {
            opened.u32(codec);
            opened.u32(lookahead);
        }
        auto udp_flow=next_flow.fetch_add(1);
        struct Forget {LinkScheduler& link;uint64_t id;~Forget(){link.forget(id);}} forget{link,udp_flow};
        auto datagrams=version==3?udp_stream(Lane::audio,cancelled,udp_flow):nullptr;
        if(datagrams)write_udp_offer(opened,datagrams->offer());
        tls.send(opened.b);
        try {
            // A slow network no longer ends the stream: capture drops stale audio and resumes live
            // (audio.cpp). Only ten seconds without any progress counts as a lost connection.
            DWORD ready_timeout = 5000, send_timeout = 10000;
            int buffer = 32 * 1024;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ready_timeout, sizeof(ready_timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_timeout, sizeof(send_timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (const char*)&buffer, sizeof(buffer));
            auto ack = tls.receive();
            Reader ready{ack};
            if (ready.u16() != protocol_version || ready.u16() != uint16_t(Op::audio_ready) ||
                ready.u32() != 1)
                throw Error(Code::invalid, "Audio readiness required");
            auto want_udp=version==3?ready.u32():0;
            if(want_udp>1)throw Error(Code::invalid,"UDP readiness flag");
            ready.end();
            bool using_udp=datagrams&&want_udp&&datagrams->ready();
            if(version==3)media_path(tls,using_udp);
            auto next_auth = std::chrono::steady_clock::now();
            uint64_t sequence = 0;
            while (!stopped) {
                if(cancelled->load()||peer_closed(socket))throw Error(Code::cancelled,"Audio connection ended");
                if(using_udp&&!datagrams->live()){using_udp=false;media_path(tls,false);}
                auto began = std::chrono::steady_clock::now();
                if (began >= next_auth) {
                    auto current = load_config(state / L"config.dpapi");
                    auto allowed = authorized_scopes(current, token);
                    if (!allowed || !(*allowed & audio) || authorization_epoch(current, token) != epoch)
                        throw Error(Code::unauthorized, "Audio trust revoked");
                    if (current.root != config.root)
                        throw Error(Code::conflict, "Root changed; restart host");
                    next_auth = began + std::chrono::milliseconds(250);
                }
                auto frame = source->next();
                Writer packet;
                packet.u16(protocol_version);
                packet.u16(uint16_t(Op::audio_packet));
                packet.u32(0);
                packet.u64(sequence++);
                packet.u64(frame.pts_us);
                packet.u32(frame.flags);
                if (encoder && !frame.pcm.empty())
                    packet.blob(encoder->encode(frame.pcm));
                else
                    packet.blob(frame.pcm);
                if(using_udp)datagrams->send_frame(sequence-1,packet.b);else tls.send(packet.b);
            }
        } catch (const Error& error) {
            try {
                Writer packet;
                packet.u16(protocol_version);
                packet.u16(uint16_t(Op::audio_packet));
                packet.u32(uint32_t(error.code));
                packet.str(error.what());
                tls.send(packet.b);
            } catch (...) {
            }
        }
    }
    void run() {
        workers.emplace_back([this] {
            while (!stopped) {
                if (!internet_enabled.load()) {
                    auto r = upnp.status();
                    if (r.ipv4.status != UpnpStatus::idle || r.ipv6.status != UpnpStatus::idle)
                        upnp.unmap();
                    auto u=upnp_udp.status();if(u.ipv4.status!=UpnpStatus::idle||u.ipv6.status!=UpnpStatus::idle)upnp_udp.unmap();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        });
        workers.emplace_back([this] {
            uint32_t ticks = 0;
            while (!stopped) {
                expire_pointer_lease();
                {
                    std::lock_guard lock(m);
                    auto now = std::chrono::steady_clock::now();
                    for (auto& [socket, h] : handshakes)
                        if (h.guarded && !h.timed_out && now >= h.deadline) {
                            h.timed_out = true;
                            auth_guard.timeout(h.address);
                            shutdown(socket, SD_BOTH);
                        }
                }
                if (++ticks % 5 == 0)
                    try {
                        auto current = load_config(state / L"config.dpapi");
                        internet_enabled = current.internet_access;
                        {
                            std::lock_guard lock(replay_mutex);
                            std::erase_if(replay_inputs,[&](auto& entry) {
                                auto scopes=authorized_scopes(current,entry.first.first);
                                return current.root!=config.root || !scopes || !(*scopes & control) ||
                                    authorization_epoch(current,entry.first.first)!=entry.second->epoch ||
                                    std::chrono::steady_clock::now()-entry.second->touched>std::chrono::minutes(10);
                            });
                        }
                        file_jobs.authorize_all([&](const std::string& token) {
                            return current.root == config.root ? authorized_scopes(current, token).value_or(0)
                                                               : 0u;
                        });
                        std::lock_guard lock(m);
                        for (auto& [socket, peer] : authenticated) {
                            auto allowed = authorized_scopes(current, peer.token);
                            auto epoch = authorization_epoch(current, peer.token);
                            if (current.root != config.root || !allowed || (peer.requested & ~*allowed) ||
                                !epoch || *epoch != peer.epoch) {
                                if (epoch && *epoch != peer.epoch)
                                    hold_ended(peer); // “结束设备全部会话”
                                file_jobs.cancel_session(peer.session);
                                peer.cancelled->store(true);
                                shutdown(socket, SD_BOTH);
                            }
                        }
                    } catch (...) {
                        file_jobs.authorize("", 0);
                        std::lock_guard lock(m);
                        for (auto& [socket, peer] : authenticated) {
                            peer.cancelled->store(true);
                            shutdown(socket, SD_BOTH);
                        }
                    }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        });
        while (!stopped) {
            auto s = accept(listener.load(), nullptr, nullptr);
            if (s == INVALID_SOCKET) {
                if (stopped)
                    break;
                continue;
            }
            for (auto it = connections.begin(); it != connections.end();) {
                if (it->done->load()) {
                    it->thread.join();
                    it = connections.erase(it);
                } else
                    ++it;
            }
            sockaddr_storage peer{};
            int size = sizeof(peer);
            char text[NI_MAXHOST]{};
            if (getpeername(s, reinterpret_cast<sockaddr*>(&peer), &size) ||
                getnameinfo(reinterpret_cast<sockaddr*>(&peer), size, text, sizeof(text), nullptr, 0,
                            NI_NUMERICHOST)) {
                closesocket(s);
                continue;
            }
            bool admitted = false;
            {
                std::lock_guard lock(m);
                bool guarded = internet_enabled.load();
                if (!stopped && active.size() < 64 && handshakes.size() < 32 &&
                    (!guarded || auth_guard.acquire(text, GetTickCount64()))) {
                    active.push_back(s);
                    handshakes[s] = {normalize_address(text),
                                     std::chrono::steady_clock::now() + std::chrono::seconds(10), guarded};
                    admitted = true;
                }
            }
            if (!admitted) {
                closesocket(s);
                continue;
            }
            auto done = std::make_shared<std::atomic_bool>(false);
            try {
                connections.emplace_back();
                connections.back().done = done;
                connections.back().thread = std::thread([this, s, done] {
                    connection(s);
                    done->store(true);
                });
            } catch (...) {
                if (!connections.empty() && connections.back().done == done &&
                    !connections.back().thread.joinable())
                    connections.pop_back();
                std::lock_guard lock(m);
                auto h = handshakes.find(s);
                if (h != handshakes.end() && h->second.guarded)
                    auth_guard.release(h->second.address);
                handshakes.erase(s);
                std::erase(active, s);
                closesocket(s);
                continue;
            }
        }
        stop();
        for (auto& t : workers)
            if (t.joinable())
                t.join();
        workers.clear();
        for (auto& c : connections)
            if (c.thread.joinable())
                c.thread.join();
        connections.clear();
    }
    void stop() {
        stopped = true;
        if (discovery)
            discovery->stop();
        file_jobs.stop();
        auto s = listener.exchange(INVALID_SOCKET);
        if (s != INVALID_SOCKET) {
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
        {
            std::lock_guard lock(m);
            for (auto sock : active)
                shutdown(sock, SD_BOTH);
        }
        upnp.stop();upnp_udp.stop();
    }
};
Host::Host(const fs::path& s, const std::string& b, uint16_t p) : p_(std::make_unique<Impl>(s, b, p)) {}
Host::~Host() {
    p_->stop();
}
void Host::run() {
    p_->run();
}
void Host::stop() {
    p_->stop();
}
std::string Host::listening_address() const {
    return p_->actual_bind;
}
uint16_t Host::listening_port() const {
    return p_->actual_port;
}
NetworkSecurityStats Host::security_stats() const {
    std::lock_guard lock(p_->m);
    return p_->auth_guard.stats();
}
UpnpReport Host::upnp_map(bool ipv4, bool ipv6) {
    auto c = load_config(p_->state / L"config.dpapi");
    if (!c.internet_access || c.legacy_enabled)
        throw Error(Code::unauthorized, "请先禁用旧凭据并开启公网访问");
    p_->internet_enabled = true;
    UpnpRequest request;
    request.internal_port = p_->actual_port;
    request.external_port = c.external_port ? c.external_port : p_->actual_port;
    for (auto& a : host_addresses()) {
        if (a.virtual_adapter || a.deprecated || a.temporary)
            continue;
        if (ipv4 && request.ipv4.empty() && a.category == AddressKind::private_v4)
            request.ipv4 = a.address;
        if (ipv6 && request.ipv6.empty() && a.category == AddressKind::global_v6)
            request.ipv6 = a.address;
    }
    if (request.ipv4.empty() && request.ipv6.empty())
        throw Error(Code::not_found, "没有可用于路由器映射的稳定地址");
    auto report=p_->upnp.map(request);request.protocol=17;auto udp=p_->upnp_udp.map(request);
    report.ipv4_udp=udp.ipv4;report.ipv6_udp=udp.ipv6;return report;
}
UpnpReport Host::upnp_unmap() {
    auto report=p_->upnp.unmap();auto udp=p_->upnp_udp.unmap();report.ipv4_udp=udp.ipv4;report.ipv6_udp=udp.ipv6;return report;
}
UpnpReport Host::upnp_status() const {
    auto report=p_->upnp.status();auto udp=p_->upnp_udp.status();report.ipv4_udp=udp.ipv4;report.ipv6_udp=udp.ipv6;return report;
}
ReachabilityReport Host::reachability_report(const fs::path& exe) const {
    auto report=lantern::reachability_report(load_config(p_->state / L"config.dpapi"), exe, p_->actual_bind,
                                        p_->actual_port, p_->listener.load() != INVALID_SOCKET,
                                        upnp_status());
    report.udp_listening=bool(p_->udp);report.public_udp_firewall=public_udp_firewall_state(exe,p_->actual_port);return report;
}
std::string Host::discovery_status() const {
    return p_->discovery->status();
}
void Host::set_link_rate(uint64_t bytes) {
    p_->link.rate(double(bytes));
}
std::vector<HostSession> Host::sessions() {
    std::lock_guard lock(p_->m);
    std::vector<HostSession> result;
    for (auto& [socket, peer] : p_->authenticated)
        result.push_back(peer.info);
    return result;
}
bool Host::end_session(const std::string& id) {
    std::lock_guard lock(p_->m);
    for (auto& [socket, peer] : p_->authenticated)
        if (peer.session == id) {
            p_->hold_ended(peer); // “终止选中的连接”
            p_->file_jobs.cancel_session(peer.session);
            peer.cancelled->store(true);
            shutdown(socket, SD_BOTH);
            return true;
        }
    return false;
}

} // namespace lantern
