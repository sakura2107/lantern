#include "lantern/clipboard.hpp"
#include "lantern/discovery.hpp"
#include "lantern/link_scheduler.hpp"
#include "lantern/win.hpp"

#include <condition_variable>
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
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic_bool stopped = false;
    LinkScheduler link{40 * 1024 * 1024};
    std::atomic_uint64_t next_flow = 1;
    std::mutex m;
    std::condition_variable cv;
    std::deque<SOCKET> pending;
    std::vector<SOCKET> active;
    struct Peer {
        std::string token, session;
        uint32_t requested;
        uint64_t epoch;
        HostSession info;
        std::shared_ptr<std::atomic_bool> cancelled;
        bool ended = false; // closed by an administrator (end session / end device)
    };
    std::map<SOCKET, Peer> authenticated;
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
        ended_holds[peer.token] = std::chrono::steady_clock::now() + ended_hold;
    }
    std::vector<std::thread> workers;
    std::atomic_uint video_sessions = 0;
    std::atomic_uint audio_sessions = 0;
    std::atomic_uint preview_sessions = 0;
    Impl(const fs::path& s, const std::string& bind, uint16_t port)
        : state(s), identity(s / L"identity.dpapi", false), config(load_config(s / L"config.dpapi")),
          files(config.root), file_jobs(files) {
        listener = listen_socket(bind, port);
        discovery = std::make_unique<DiscoveryResponder>(bind, port, identity.fingerprint());
    }
    Bytes dispatch(Op op, Reader& r, uint32_t granted, Lane lane, InputSession& input,
                   std::unique_ptr<DirectorySearch>& search, const std::string& owner,
                   const std::string& session) {
        auto require = [&](uint32_t flag) {
            if (!(granted & flag))
                throw Error(Code::unauthorized, "Permission denied for this action");
        };
        Writer w;
        switch (op) {
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
            w.str("Lantern/"
                  "1;tls=1.2;files=paged,read,write,resume,sha256,sorted-v1,search-v1,thumbnail-v1,host-jobs-"
                  "v1;desktop=gdi-jpeg,pull,input,multi-monitor;"
                  "input=atomic-click-v1,gesture-v1,unicode-ime-v1,native-focus-v1,atomic-shortcut-v1,right-"
                  "click-v1;"
                  "clipboard=text-cas-v1;trust=per-device-v1;media=original-range-external;audio=wasapi-"
                  "pcm16-v1,codec-negotiation-v2;privacy-screen="
                  "unsupported;virtual-"
                  "display=unsupported;hardware-codec=optional-nvenc-h264;desktop-stream=h264-v1,h264-clock-"
                  "v2;av-clock=host-qpc-v1;session=end-v1");
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
        try {
            tls = std::make_unique<Tls>(socket, &identity);
            session = random_token();
            auto first = tls->receive();
            Reader auth{first};
            if (auth.u16() != protocol_version || auth.u16() != uint16_t(Op::hello))
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
            if (auth_config.root != config.root)
                reject(Code::conflict, "Root changed; restart host");
            auto allowed = authorized_scopes(auth_config, token);
            if (!allowed || (requested & ~*allowed))
                reject(Code::unauthorized, "Pairing credential or requested permission denied");
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
            Writer hello;
            hello.u16(protocol_version);
            hello.u16(uint16_t(Op::hello));
            hello.u32(0);
            hello.u32(requested);
            tls->send(hello.b);
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
                info.address = ip;
            {
                std::lock_guard lock(m);
                authenticated[socket] = {token,         session,         requested,
                                         session_epoch, std::move(info), cancelled};
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
                            : op == Op::read ? (lane == Lane::media ? Lane::media : Lane::bulk)
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
                        stream_desktop(*tls, socket, r, token, session_epoch);
                        break; // This connection belongs to the stream until it closes.
                    }
                    if (op == Op::audio_stream) {
                        if (!(requested & *current_allowed & audio))
                            throw Error(Code::unauthorized, "System audio permission required");
                        if (lane != Lane::audio)
                            throw Error(Code::invalid, "System audio requires its own channel");
                        stream_audio(*tls, socket, r, token, session_epoch);
                        break;
                    }
                    auto body =
                        dispatch(op, r, requested & *current_allowed, lane, input, search, token, session);
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
        } catch (...) { /* No secrets or remote paths in ambient logs. Client sees close on handshake/auth
                           failure. */
        }
        file_jobs.disconnect(session);
        {
            std::lock_guard lock(m);
            authenticated.erase(socket);
            active.erase(std::remove(active.begin(), active.end(), socket), active.end());
        }
        link.forget(flow);
        // Tls construction owns and closes socket, including failed handshakes.
    }
    void stream_desktop(Tls& tls, SOCKET socket, Reader& request, const std::string& token, uint64_t epoch) {
        auto version = request.u32();
        if (version != 1 && version != 2)
            throw Error(Code::unsupported, "Desktop stream version mismatch");
        auto monitor = request.u32(), width = request.u32(), fps = request.u32(), bitrate = request.u32();
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
        auto source = open_desktop_stream(monitor, width, fps, bitrate);
        const auto& cfg = source->config();
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
        if (version == 2)
            opened.str("host-qpc-us");
        tls.send(opened.b);
        try {
            DWORD ready_timeout = 15000, send_timeout = 300;
            int send_buffer = 256 * 1024;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ready_timeout, sizeof(ready_timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_timeout, sizeof(send_timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (const char*)&send_buffer, sizeof(send_buffer));
            auto ack = tls.receive();
            Reader ready{ack};
            if (ready.u16() != protocol_version || ready.u16() != uint16_t(Op::desktop_ready) ||
                ready.u32() != 1)
                throw Error(Code::invalid, "Decoder readiness packet required");
            ready.end();
            auto next_auth = std::chrono::steady_clock::now();
            auto next_frame = next_auth;
            auto frame_period = std::chrono::microseconds(1'000'000 / cfg.fps);
            while (!stopped) {
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
                auto frame = source->next();
                Writer packet;
                packet.u16(protocol_version);
                packet.u16(uint16_t(Op::desktop_packet));
                packet.u32(0);
                packet.u64(frame.sequence);
                packet.u64(version == 2 ? frame.clock_us : frame.pts_us);
                packet.u32(frame.flags);
                packet.blob(frame.annex_b);
                tls.send(packet.b); // One encoded frame, bounded socket buffer; no unbounded sender queue.
                if (std::chrono::steady_clock::now() - began > std::chrono::milliseconds(300))
                    throw Error(Code::busy, "Desktop send stalled; reconnect at lower bitrate");
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
    void stream_audio(Tls& tls, SOCKET socket, Reader& request, const std::string& token, uint64_t epoch) {
        auto version = request.u32();
        if (version != 1 && version != 2)
            throw Error(Code::unsupported, "Audio stream version mismatch");
        uint32_t preferred = version == 2 ? request.u32() : 0;
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
        if (version == 2) {
            opened.u32(codec);
            opened.u32(lookahead);
        }
        tls.send(opened.b);
        try {
            DWORD ready_timeout = 5000, send_timeout = 200;
            int buffer = 32 * 1024;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ready_timeout, sizeof(ready_timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&send_timeout, sizeof(send_timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDBUF, (const char*)&buffer, sizeof(buffer));
            auto ack = tls.receive();
            Reader ready{ack};
            if (ready.u16() != protocol_version || ready.u16() != uint16_t(Op::audio_ready) ||
                ready.u32() != 1)
                throw Error(Code::invalid, "Audio readiness required");
            ready.end();
            auto next_auth = std::chrono::steady_clock::now();
            uint64_t sequence = 0;
            while (!stopped) {
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
                auto sending = std::chrono::steady_clock::now();
                tls.send(packet.b);
                if (std::chrono::steady_clock::now() - sending > std::chrono::milliseconds(200))
                    throw Error(Code::busy, "Audio send stalled");
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
    void worker() {
        for (;;) {
            SOCKET s;
            {
                std::unique_lock lock(m);
                cv.wait(lock, [&] { return stopped || !pending.empty(); });
                if (stopped)
                    return;
                s = pending.front();
                pending.pop_front();
                active.push_back(s);
            }
            connection(s);
        }
    }
    void run() {
        workers.emplace_back([this] {
            uint32_t ticks = 0;
            while (!stopped) {
                expire_pointer_lease();
                if (++ticks % 5 == 0)
                    try {
                        auto current = load_config(state / L"config.dpapi");
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
        for (int i = 0; i < 16; ++i)
            workers.emplace_back([this] { worker(); });
        while (!stopped) {
            auto s = accept(listener.load(), nullptr, nullptr);
            if (s == INVALID_SOCKET) {
                if (stopped)
                    break;
                continue;
            }
            {
                std::lock_guard lock(m);
                if (pending.size() >= 8 || stopped)
                    closesocket(s);
                else {
                    pending.push_back(s);
                    cv.notify_one();
                }
            }
        }
        stop();
        for (auto& t : workers)
            if (t.joinable())
                t.join();
        workers.clear();
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
        std::lock_guard lock(m);
        for (auto sock : active)
            shutdown(sock, SD_BOTH);
        for (auto sock : pending)
            closesocket(sock);
        pending.clear();
        cv.notify_all();
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
