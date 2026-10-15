#pragma once
#include "core.hpp"
#include <filesystem>
#include <functional>
#include <memory>
#include <map>
#include <optional>
#include <windows.h>
#include <winsock2.h>
namespace lantern {
namespace fs = std::filesystem;
struct TransportError : Error { uint32_t native_code;explicit TransportError(std::string s,uint32_t n=0):Error(Code::io,std::move(s)),native_code(n){} };
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() = default;
    explicit Handle(HANDLE v) : h(v) {}
    ~Handle() {
        if (h != INVALID_HANDLE_VALUE && h)
            CloseHandle(h);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h(o.h) {
        o.h = INVALID_HANDLE_VALUE;
    }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            if (h != INVALID_HANDLE_VALUE && h)
                CloseHandle(h);
            h = o.h;
            o.h = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    operator HANDLE() const {
        return h;
    }
};
std::wstring wide(const std::string&);
std::string utf8(const std::wstring&);
std::string hex(std::span<const uint8_t>);
std::string random_token();
uint64_t monotonic_us();
std::string sha256(std::span<const uint8_t>);
std::string hash_file(HANDLE);
std::string hash_file(HANDLE,Cancel&,std::function<void(uint64_t,uint64_t)>);
std::string hash_file(const fs::path&);
Bytes read_file(const fs::path&, size_t max = 1024 * 1024);
void write_file(const fs::path&, std::span<const uint8_t>);
Bytes protect(std::span<const uint8_t>);
Bytes unprotect(std::span<const uint8_t>);
[[noreturn]] void win_error(const std::string&);
struct Identity {
    void* store = nullptr;
    const void* cert = nullptr;
    ~Identity();
    Identity(const fs::path&, bool create);
    std::string fingerprint() const;
};
class Tls {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    Tls(SOCKET, Identity* server, const std::string& expected_pin = "");
    ~Tls();
    Tls(const Tls&) = delete;
    void send(std::span<const uint8_t>);
    Bytes receive();
    void set_send_gate(std::function<void(size_t)>); // Owner thread, installed after authentication.
    void interrupt() noexcept; // May be called by another thread; owner must keep Tls alive.
};
SOCKET connect_socket(const std::string&, uint16_t);
SOCKET listen_socket(const std::string&, uint16_t);
void init_sockets();
struct FileInfo {
    uint64_t size = 0;
    std::string version, hash;
};
struct Entry {
    std::string name;
    bool directory;
    uint64_t size, mtime;
};
struct Guard {
    fs::path path;
    std::vector<Handle> locks;
};
/** Hidden + system (e.g. "System Volume Information", "$RECYCLE.BIN"): Explorer hides these by default
 *  and ordinary accounts usually cannot open them, so listings and searches leave them out. */
inline bool protected_os_entry(DWORD attributes) {
    constexpr DWORD both = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM;
    return (attributes & both) == both;
}
struct DirectoryPage { std::string generation; uint32_t total = 0; std::vector<Entry> entries; };
struct SearchPage { bool done = false; uint64_t scanned = 0; std::vector<Entry> entries; };
class DirectorySearch {
  public:
    virtual ~DirectorySearch() = default;
    virtual SearchPage next() = 0; // Bounded incremental scan; caller stops requesting to cancel.
};
class FileService {
    friend class IncrementalSearch;
    fs::path root_;
    Handle root_lock_;
    std::mutex mutations_;
    std::mutex hash_mutex_;
    std::map<std::string,std::string> hash_cache_;
    FileInfo inspect(const std::string&,bool,Cancel*,std::function<void(uint64_t,uint64_t)>);
    Guard guard(const std::string&, bool allow_missing = false);
    Handle open_read(Guard&);
    fs::path stage(const std::string& id);

  public:
    explicit FileService(fs::path);
    std::vector<Entry> list(const std::string&, uint32_t offset, uint32_t limit, const std::string& filter,
                            std::string& generation);
    DirectoryPage page(const std::string&, uint32_t offset, uint32_t limit, const std::string& filter,
                       uint32_t sort, bool descending, const std::string& expected_generation);
    std::unique_ptr<DirectorySearch> search(const std::string&, const std::string&);
    Bytes preview_source(const std::string&);
    void copy_tree(const std::string& from, const std::string& to, Cancel&, std::function<void(uint64_t,uint64_t)>);
    void remove_tree(const std::string&, Cancel&, std::function<void(uint64_t,uint64_t)>);
    FileInfo stat(const std::string&, bool hash = true);
    FileInfo hash_snapshot(const std::string&,Cancel&,std::function<void(uint64_t,uint64_t)>);
    Bytes read(const std::string&, const std::string& version, uint64_t off, uint32_t n);
    std::pair<std::string, uint64_t> begin(const std::string&, uint64_t, const std::string& hash);
    uint64_t chunk(const std::string& id, uint64_t off, std::span<const uint8_t>);
    void commit(const std::string& id);
    void mutate(Op, const std::string&, const std::string& to = "");
};
Bytes image_thumbnail(std::span<const uint8_t>, uint32_t width);
struct FileJobInfo { std::string id; TaskState state; uint32_t kind=0; uint64_t current=0,total=0; Code error=Code::ok; std::string message; FileInfo file; };
class FileJobs {
    struct Impl; std::unique_ptr<Impl> p_;
  public:
    explicit FileJobs(FileService&);
    ~FileJobs();
    std::string start(uint32_t kind,const std::string& from,const std::string& to,const std::string& owner,const std::string& session={});
    FileJobInfo poll(const std::string&,const std::string& owner);
    void cancel(const std::string&,const std::string& owner);
    void authorize(const std::string& current_owner,uint32_t scopes);
    void authorize_all(const std::function<uint32_t(const std::string&)>&);
    void disconnect(const std::string& session);
    void cancel_session(const std::string& session);
    void stop();
};
struct Monitor {
    int x, y, width, height;
    std::string name;
};
std::vector<Monitor> monitors();
Bytes capture(uint32_t monitor, uint32_t width, uint32_t quality);
struct VideoConfig {
    uint32_t width, height, fps, bitrate;
    std::string capture_provider, encoder_provider;
    Bytes headers;
};
struct VideoFrame {
    uint64_t sequence, pts_us;
    uint32_t flags; // 1 IDR, 2 repeated desktop surface
    Bytes annex_b;
    double capture_ms = 0, encode_ms = 0;
    uint64_t clock_us = 0;
};
// Built-in provider; single owning worker thread. Dynamic deployment is independent of this interface.
class DesktopStream {
  public:
    virtual ~DesktopStream() = default;
    virtual const VideoConfig& config() const = 0;
    virtual VideoFrame next() = 0;
};
std::unique_ptr<DesktopStream> open_desktop_stream(uint32_t monitor, uint32_t width, uint32_t fps,
                                                   uint32_t bitrate);
void remote_input(uint32_t kind, int32_t x, int32_t y, uint32_t value, uint32_t monitor);
void set_input_validation_pid(uint32_t pid);
void remote_click(int32_t x, int32_t y, uint32_t monitor);
// Connection-owned pointer lease. Never transfers a held button to a new connection.
class InputSession {
  public:
    ~InputSession();
    InputSession() = default;
    InputSession(const InputSession&) = delete;
    void pointer(uint32_t phase, int32_t x, int32_t y, uint32_t monitor);
    void click(int32_t x, int32_t y, uint32_t monitor, bool twice = false);
    void text(const std::string&);
    void key(uint32_t vk, uint32_t count);
    void shortcut(uint32_t modifiers,uint32_t vk);
    void right_click(int32_t x,int32_t y,uint32_t monitor);
    uint32_t focus(); // flags: 1 known editable native text field, 2 password; no contents.
    void cancel() noexcept;
};
void expire_pointer_lease() noexcept;
struct AudioConfig { uint32_t sample_rate, channels = 2, bits = 16; std::string provider = "wasapi-loopback"; };
struct AudioFrame { uint64_t pts_us; uint32_t flags; Bytes pcm; }; // flags1 discontinuity,2 idle heartbeat
class AudioSource {
  public:
    virtual ~AudioSource() = default;
    virtual const AudioConfig& config() const = 0;
    virtual AudioFrame next() = 0;
};
std::unique_ptr<AudioSource> open_system_audio();
class AudioEncoder {
public:
    virtual ~AudioEncoder()=default;
    virtual uint32_t lookahead_frames()const=0;
    virtual Bytes encode(std::span<const uint8_t> pcm)=0;
};
std::unique_ptr<AudioEncoder> open_opus_encoder(uint32_t sample_rate);
struct TrustedDevice { std::string id,label,token; uint32_t scopes=files_read;uint64_t epoch=0; };
struct Config {
    fs::path root;
    std::string token;
    uint32_t scopes = files_read;
    bool legacy_enabled=true;
    uint64_t legacy_epoch=0;
    std::vector<TrustedDevice> devices;
};
void save_config(const fs::path&, const Config&);
Config load_config(const fs::path&);
void edit_config(const fs::path&,const std::function<void(Config&)>&);
std::optional<uint32_t> authorized_scopes(const Config&,const std::string& token);
std::optional<uint64_t> authorization_epoch(const Config&,const std::string& token);
struct HostSession {
    std::string id,device_id,label,address;
    uint32_t scopes=0,channel=0;
};
class Host {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    Host(const fs::path& state, const std::string& bind, uint16_t port);
    ~Host();
    void run();
    void stop();
    void set_link_rate(uint64_t bytes_per_second);
    std::string discovery_status() const;
    std::vector<HostSession> sessions(); // Local management only; no credentials.
    bool end_session(const std::string&); // Ends this connection, retains device trust.
};
class Client {
    std::unique_ptr<Tls> tls_;
    uint32_t scopes_;
    uint64_t transfer_bytes_=0;

  public:
    /** hello_flags: 1 = automatic reconnect (session=end-v1 hosts only; 0 sends the original HELLO). */
    Client(const std::string& address, uint16_t port, const std::string& pin, const std::string& token,
           uint32_t scopes = files_read, uint32_t channel = UINT32_MAX, uint32_t hello_flags = 0);
    Bytes call(Op, Writer payload = {});
    void send(Op, Writer payload = {}); // Streaming readiness; no response is expected.
    Bytes receive(Op);
    void interrupt() noexcept;
    uint64_t transfer_bytes()const noexcept {return transfer_bytes_;} // This owning worker's payload count.
    std::vector<Entry> list(const std::string&, uint32_t offset = 0, const std::string& filter = "",
                            std::string* generation = nullptr);
    FileInfo stat(const std::string&, bool hash = true, Cancel* cancellation = nullptr);
    Bytes read(const std::string&, const std::string&, uint64_t, uint32_t);
    void download(const std::string& remote, const fs::path& local, Cancel&,
                  std::function<void(uint64_t, uint64_t)> progress = {},FileInfo* verified = nullptr);
    void upload(const fs::path& local, const std::string& remote, Cancel&,
                std::function<void(uint64_t, uint64_t)> progress = {});
    void mutate(Op, const std::string&, const std::string& to = "");
};
void media_bridge(const std::string& host, uint16_t port, const std::string& pin, const std::string& token,
                  const std::string& remote, uint16_t local_port, bool launch);
void open_external(const fs::path&, bool chooser, HWND owner = nullptr);
} // namespace lantern
