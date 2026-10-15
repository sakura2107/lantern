#pragma once
#include "clipboard.hpp"
#include "windows_client.hpp"
namespace lantern {
ClipboardText client_clipboard_read(Client&);
uint32_t client_clipboard_write(Client&, uint32_t expected, const std::string&);
struct ClipboardAccess {
    std::function<ClipboardText()> read;
    std::function<uint32_t(uint32_t, const std::string&)> write;
};
class ClipboardSync {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    ClipboardSync(HWND, std::function<void(std::wstring)> status, ClipboardAccess access = {});
    ~ClipboardSync();
    void start(ClientCredentials); // Opt-in; baseline preserves both original clipboards.
    void stop();
    void copy_once(ClientCredentials, bool to_remote);
    bool enabled() const;
};
} // namespace lantern
