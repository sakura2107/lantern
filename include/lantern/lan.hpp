#pragma once
#include "upnp.hpp"
#include "win.hpp"
namespace lantern {
inline constexpr const char* default_host_bind = "::";
enum class LanType { wifi, ethernet, other };
struct LanAddress {
    std::string address, adapter_name;
    LanType type = LanType::other;
    bool virtual_adapter = false;
};
std::vector<LanAddress> lan_addresses();
struct HostAddress : LanAddress {
    AddressKind category = AddressKind::invalid;
    bool temporary = false, deprecated = false;
    uint32_t valid_lifetime = 0, preferred_lifetime = 0;
};
// Random suffix + finite short lifetime is conservative: potentially temporary addresses
// are excluded from public advertisements, even if Windows uses a stable random IID.
bool temporary_ipv6(bool random_suffix, uint32_t valid_lifetime, uint32_t preferred_lifetime);
std::vector<HostAddress> host_addresses();
uint64_t host_address_fingerprint(); // Ignores ticking lifetime; changes on expiry/deprecation.
std::vector<ConnectionEndpoint> host_endpoints(const Config&, uint16_t port, const std::vector<HostAddress>&);
std::vector<ConnectionEndpoint> host_endpoints(const Config&, uint16_t port = 47443);
enum class FirewallState { allowed, not_allowed, unknown };
// Describes the Private-profile TCP rule, not the current network category or UDP discovery.
FirewallState firewall_state(const fs::path& exe, uint16_t port) noexcept;
void firewall_allow(const fs::path& exe, uint16_t port); // Explicit elevated CLI operation only.
bool request_firewall_allow(const fs::path& exe, uint16_t port) noexcept;
FirewallState public_firewall_state(const fs::path& exe, uint16_t port) noexcept;
uint32_t firewall_current_profiles() noexcept; // NET_FW_PROFILE2 bitmask; 0 unknown.
void firewall_public_allow(const fs::path& exe, uint16_t port);
void firewall_public_remove(const fs::path& exe);
bool request_public_firewall(const fs::path& exe, uint16_t port, bool allow) noexcept;
struct ReachabilityReport {
    std::string bind;
    uint16_t port = 0;
    bool listening = false, dual_stack = false, stable_global_ipv6 = false, cgnat = false;
    bool internet_access = false, legacy_enabled = true;
    FirewallState public_firewall = FirewallState::unknown;
    uint32_t current_profiles = 0;
    UpnpReport upnp;
    std::vector<HostAddress> addresses;
};
ReachabilityReport reachability_report(const Config&, const fs::path& exe, const std::string& actual_bind,
                                       uint16_t port, bool listening, const UpnpReport& upnp = {});
} // namespace lantern
