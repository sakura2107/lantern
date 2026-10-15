#pragma once
#include "win.hpp"
#include "av_timeline.hpp"
namespace lantern {
struct Viewport {
    int left = 0, top = 0, width = 0, height = 0;
    static Viewport fit(int cw, int ch, int sw, int sh, double zoom = 1);
    bool map(int x, int y, int32_t& nx, int32_t& ny, bool clamp = false) const;
};
struct DecodeStats {
    uint64_t decoded = 0, presented = 0, gpu_surfaces = 0, cpu_surfaces = 0;
    uint64_t scheduled_wait_us=0;
};
// One owning MTA thread. HWND is retained by the UI until shutdown/join.
class WindowsVideo {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    WindowsVideo(HWND, const VideoConfig&, std::shared_ptr<AvTimeline> = {}, std::function<bool()> = {});
    ~WindowsVideo();
    void frame(std::span<const uint8_t>, uint64_t pts_us, bool visible, double zoom = 1);
    DecodeStats stats() const;
    std::string provider() const;
};
struct RenderStats {
    uint64_t frames = 0, overruns = 0, resets = 0;
    uint32_t queued_frames = 0;
};
class WindowsAudio {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    WindowsAudio(uint32_t rate, uint32_t codec, uint32_t lookahead, std::shared_ptr<AvTimeline> = {});
    ~WindowsAudio();
    void packet(std::span<const uint8_t>, uint32_t flags, Cancel&, uint64_t host_pts=0);
    RenderStats stats() const;
    static bool supports_opus();
};
} // namespace lantern
