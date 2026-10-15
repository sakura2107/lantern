#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
namespace lantern {
using Bytes = std::vector<uint8_t>;
constexpr uint16_t protocol_version = 1;
constexpr uint32_t max_frame = 4 * 1024 * 1024, chunk_size = 256 * 1024;
enum class Code : uint32_t {
    ok,
    invalid,
    unauthorized,
    not_found,
    conflict,
    io,
    unsupported,
    cancelled,
    busy,
    integrity,
    source_changed
};
struct Error : std::runtime_error {
    Code code;
    Error(Code c, std::string s) : runtime_error(std::move(s)), code(c) {}
};
enum Scope : uint32_t { files_read = 1, files_write = 2, view = 4, control = 8, audio = 16 };
enum class Op : uint16_t {
    hello = 1,
    list = 2,
    stat = 3,
    read = 4,
    upload_begin = 5,
    upload_chunk = 6,
    upload_commit = 7,
    mkdir = 8,
    rename = 9,
    copy = 10,
    remove = 11,
    monitors = 20,
    frame = 21,
    input = 22,
    capabilities = 30,
    media_stat = 31,
    desktop_stream = 32,
    desktop_packet = 33,
    desktop_ready = 34,
    click = 35,
    pointer_gesture = 36,
    double_click = 37,
    text_input = 38,
    key_press = 39,
    input_focus = 40,
    audio_stream = 41,
    audio_packet = 42,
    audio_ready = 43,
    files_page = 44,
    files_search = 45,
    thumbnail = 46,
    file_job = 47,
    file_job_poll = 48,
    file_job_cancel = 49,
    ping = 50,
    shortcut = 51,
    right_click = 52,
    clipboard_read = 53,
    clipboard_write = 54
};
struct Writer {
    Bytes b;
    void u16(uint16_t n);
    void u32(uint32_t n);
    void u64(uint64_t n);
    void blob(std::span<const uint8_t> s);
    void str(const std::string& s);
};
struct Reader {
    std::span<const uint8_t> b;
    size_t at = 0;
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    Bytes blob(uint32_t limit = max_frame);
    std::string str(uint32_t limit = 65536);
    void end();
};
struct Endpoint {std::string host;uint16_t port=47443;};
Endpoint parse_endpoint(const std::string&,uint16_t default_port=47443);
std::string format_endpoint(const Endpoint&);
bool safe_path(const std::string& p);
bool valid_sha(const std::string& p);
struct ByteRange {
    uint64_t begin, end;
};
ByteRange parse_range(const std::string& value, uint64_t size);
enum class Lane { input, desktop, audio, media, bulk };
// One shared aggregate budget. Admission reserves foreground capacity even across TCP connections.
class Budget {
    std::mutex m_;
    double bulk_ = 0, media_ = 0, rate_;
    std::chrono::steady_clock::time_point last_;

  public:
    explicit Budget(double bytes_per_sec) : rate_(bytes_per_sec), last_(std::chrono::steady_clock::now()) {}
    bool admit(Lane lane, size_t bytes, std::chrono::steady_clock::time_point now);
};
struct Cancel {
    std::atomic_bool requested = false;
    void check() const {
        if (requested)
            throw Error(Code::cancelled, "Cancelled");
    }
};
enum class TaskState { queued, running, paused, verifying, completed, failed, cancelled };
bool transition_allowed(TaskState a, TaskState b);
} // namespace lantern
