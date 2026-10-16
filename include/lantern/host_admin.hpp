#pragma once
#include "win.hpp"
namespace lantern {
constexpr uint32_t host_admin_api_version = 1;
struct DeviceSummary {
    std::string id, label;
    uint32_t scopes = 0, effective_scopes = 0;
};
struct HostAdminSnapshot {
    fs::path root;
    std::string pin;
    uint32_t ceiling = 0;
    bool legacy = false;
    std::vector<DeviceSummary> devices;
};
struct DevicePairing {
    DeviceSummary device;
    std::string pin, secret;
};
struct InternetSettings {
    bool internet_access = false;
    std::string public_hostname;
    uint16_t external_port = 0;
};
// Local Windows-user access to a selected DPAPI state directory. Not a network admin API.
class HostAdministration {
    fs::path state_;

  public:
    static void initialize(const fs::path& state,const fs::path& shared_root,uint32_t scopes);
    explicit HostAdministration(fs::path);
    HostAdminSnapshot snapshot() const;
    DevicePairing add(const std::string& label, uint32_t scopes);
    void grant(const std::string& id, uint32_t scopes);
    void remove(const std::string& id);
    void end_device(const std::string& id);
    void ceiling(uint32_t scopes);
    /** Changes the authorized root of an existing host; identity, devices and pairings are kept. */
    void root(const fs::path& shared_root);
    void disable_legacy();
    DevicePairing pairing(const std::string& id) const;
    PairingInvite invite(const std::string& label, uint32_t scopes,
                         std::chrono::seconds ttl = std::chrono::minutes(10), bool allow_remote = false);
    std::vector<PendingInvite> invites() const;
    void cancel_invite(const std::string& invite_id);
    // Protocol backend: atomically consumes the invitation and saves its device.
    TrustedDevice redeem_invite(const std::string& code, const std::string& device_label,
                                bool local_source = true);
    InternetSettings network_settings() const;
    void internet(bool enabled, const std::string& hostname = {}, uint16_t external_port = 0);
};
enum class InternetError { legacy_enabled };
struct InternetAccessError : Error {
    InternetError reason = InternetError::legacy_enabled;
    InternetAccessError() : Error(Code::conflict, "请先禁用旧共享凭据，再开启公网访问") {}
};
} // namespace lantern
