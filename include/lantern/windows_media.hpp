#pragma once
#include "windows_client.hpp"
namespace lantern {
// Original byte ranges for an existing local player. Never decodes or transcodes video.
class WindowsMediaBridge {
    struct Impl;std::unique_ptr<Impl> p_;
public:
    WindowsMediaBridge(ClientCredentials,std::string remote);
    ~WindowsMediaBridge();
    std::string url()const;
    void stop();
};
}
