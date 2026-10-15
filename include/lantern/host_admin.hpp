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
};
} // namespace lantern
