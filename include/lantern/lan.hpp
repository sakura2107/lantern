#pragma once
#include "win.hpp"
namespace lantern {
inline constexpr const char* default_host_bind = "0.0.0.0";
enum class LanType { wifi, ethernet, other };
struct LanAddress {
    std::string address, adapter_name;
    LanType type = LanType::other;
    bool virtual_adapter = false;
};
std::vector<LanAddress> lan_addresses();
enum class FirewallState { allowed, not_allowed, unknown };
// Describes the Private-profile TCP rule, not the current network category or UDP discovery.
FirewallState firewall_state(const fs::path& exe, uint16_t port) noexcept;
void firewall_allow(const fs::path& exe, uint16_t port); // Explicit elevated CLI operation only.
bool request_firewall_allow(const fs::path& exe, uint16_t port) noexcept;
} // namespace lantern
