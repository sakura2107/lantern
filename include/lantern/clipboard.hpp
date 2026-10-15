#pragma once
#include "win.hpp"
namespace lantern {
struct ClipboardText {
    uint32_t sequence = 0;
    bool present = false;
    std::string text;
};
ClipboardText read_clipboard_text();
uint32_t write_clipboard_text(HWND owner, uint32_t expected, const std::string&);
} // namespace lantern
