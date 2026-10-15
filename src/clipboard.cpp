#include "lantern/clipboard.hpp"
#include <cstring>
namespace lantern {
namespace {
std::recursive_mutex clipboard_mutex;
struct Lock {
    std::unique_lock<std::recursive_mutex> serialize{clipboard_mutex};
    Lock(HWND owner) {
        if (!OpenClipboard(owner))
            throw Error(Code::busy, "剪贴板正被其他应用占用");
    }
    ~Lock() {
        CloseClipboard();
    }
};
void validate(const std::string& s) {
    if (s.size() > 65536 || s.find('\0') != std::string::npos)
        throw Error(Code::invalid, "剪贴板仅支持最多64KiB UTF-8文本");
    wide(s);
}
} // namespace
ClipboardText read_clipboard_text() {
    Lock lock(nullptr);
    ClipboardText result;
    result.present = IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE;
    if (result.present) {
        auto memory = GetClipboardData(CF_UNICODETEXT);
        if (!memory)
            win_error("Read text clipboard");
        auto size = GlobalSize(memory);
        if (size > 131074 || size < 2)
            throw Error(Code::unsupported, "剪贴板文本超过支持大小");
        auto text = (const wchar_t*)GlobalLock(memory);
        if (!text)
            win_error("Lock text clipboard");
        try {
            size_t count = 0;
            while (count < size / 2 && text[count])
                ++count;
            if (count == size / 2)
                throw Error(Code::invalid, "剪贴板文本缺少结束符");
            result.text = utf8(std::wstring(text, count));
            validate(result.text);
        } catch (...) {
            GlobalUnlock(memory);
            throw;
        }
        GlobalUnlock(memory);
    }
    result.sequence = GetClipboardSequenceNumber();
    return result;
}
uint32_t write_clipboard_text(HWND owner, uint32_t expected, const std::string& text) {
    std::lock_guard serialize(clipboard_mutex);
    validate(text);
    auto units = wide(text);
    auto bytes = (units.size() + 1) * sizeof(wchar_t);
    auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
        win_error("Allocate clipboard text");
    auto data = GlobalLock(memory);
    if (!data) {
        GlobalFree(memory);
        win_error("Clipboard allocation");
    }
    std::memcpy(data, units.c_str(), bytes);
    GlobalUnlock(memory);
    HWND temporary = nullptr;
    if (!owner) {
        temporary = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                    GetModuleHandleW(nullptr), nullptr);
        owner = temporary;
        if (!owner) {
            GlobalFree(memory);
            win_error("Clipboard owner");
        }
    }

    try {
        Lock lock(owner);
        if (GetClipboardSequenceNumber() != expected)
            throw Error(Code::conflict, "剪贴板在操作期间变化，保留新内容");
        if (!EmptyClipboard())
            win_error("Empty clipboard");
        if (!SetClipboardData(CF_UNICODETEXT, memory))
            win_error("Set clipboard text");
        memory = nullptr;

    } catch (...) {
        if (memory)
            GlobalFree(memory);
        if (temporary)
            DestroyWindow(temporary);
        throw;
    }
    if (temporary)
        DestroyWindow(temporary);
    // Windows may update the sequence again when closing the clipboard or destroying its owner.
    // Observe the published data after those operations; a concurrent replacement is a conflict.
    auto published = read_clipboard_text();
    if (!published.present || published.text != text)
        throw Error(Code::conflict, "剪贴板在提交后变化，保留新内容");
    return published.sequence;
}
} // namespace lantern
