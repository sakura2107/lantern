#pragma once
#include "core.hpp"

namespace lantern {
// Connection-owned pointer lease. Never transfers a held button to a new connection.
class InputSession {
    inline static std::atomic_uint64_t next_generation_{1};
    const uint64_t generation_;
    void (*release_)(uint64_t) noexcept;
  public:
    ~InputSession() { cancel(); }
    InputSession();
    explicit InputSession(void (*release)(uint64_t) noexcept)
        : generation_(next_generation_.fetch_add(1)), release_(release) {}
    uint64_t generation() const noexcept { return generation_; }
    InputSession(const InputSession&) = delete;
    void pointer(uint32_t phase, int32_t x, int32_t y, uint32_t monitor);
    void click(int32_t x, int32_t y, uint32_t monitor, bool twice = false);
    void text(const std::string&);
    void key(uint32_t vk, uint32_t count);
    void shortcut(uint32_t modifiers,uint32_t vk);
    void right_click(int32_t x,int32_t y,uint32_t monitor);
    uint32_t focus(); // flags: 1 known editable native text field, 2 password; no contents.
    void cancel() noexcept { release_(generation_); }
};
} // namespace lantern
