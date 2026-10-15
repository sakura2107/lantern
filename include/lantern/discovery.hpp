#pragma once
#include "win.hpp"
namespace lantern {
constexpr uint16_t discovery_port = 47442;
struct DiscoveredDevice {
    std::string host, label, pin;
    uint16_t port;
};
Bytes discovery_query(const std::string& nonce);
Bytes discovery_reply(std::span<const uint8_t> request, const std::string& label, const std::string& pin,
                      uint16_t port);
DiscoveredDevice parse_discovery(std::span<const uint8_t>, const std::string& nonce,
                                 const std::string& source);
std::vector<DiscoveredDevice> discover_devices(Cancel&, const std::string& target = "");
class DiscoveryResponder {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    DiscoveryResponder(const std::string& bind, uint16_t port, const std::string& pin);
    ~DiscoveryResponder();
    void stop() noexcept;
    std::string status() const;
};
} // namespace lantern
