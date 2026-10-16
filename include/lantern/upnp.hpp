#pragma once
#include "win.hpp"
namespace lantern {
enum class UpnpStatus { idle, no_igd, unsupported, denied, success, cgnat, network_error, expired };
struct UpnpMapping {
    UpnpStatus status = UpnpStatus::idle;
    std::string external_address;
    uint16_t external_port = 0;
    uint32_t lease_seconds = 0;
    uint64_t expires_at = 0;
    std::optional<uint32_t> pinhole_id;
};
struct UpnpReport {
    UpnpMapping ipv4, ipv6;
};
struct UpnpRequest {
    std::string ipv4, ipv6; // Local connected IPv4 / stable global IPv6; empty skips that family.
    uint16_t internal_port = 47443, external_port = 47443;
    uint32_t lease_seconds = 1800;
};
struct SsdpTarget {
    std::string address = "239.255.255.250";
    uint16_t port = 1900;
    uint32_t timeout_ms = 1500;
};
// No mapping at construction or discovery. All HTTP targets must be the LAN SSDP responder.
class UpnpIgd {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    explicit UpnpIgd(SsdpTarget = {});
    ~UpnpIgd();
    UpnpIgd(const UpnpIgd&) = delete;
    UpnpStatus discover();
    UpnpReport map(const UpnpRequest&);
    UpnpReport renew();
    UpnpReport unmap();
    UpnpReport status() const;
    // Explicit CLI cleanup after a crash; caller supplies previously displayed ownership identifiers.
    UpnpReport remove(uint16_t external_port, std::optional<uint32_t> pinhole_id = {});
};
// Host-owned lease worker. Explicit map only; renews at half lease and removes on stop.
class UpnpLease {
    struct Impl;
    std::unique_ptr<Impl> p_;

  public:
    explicit UpnpLease(SsdpTarget = {});
    ~UpnpLease();
    UpnpReport map(const UpnpRequest&);
    UpnpReport status() const;
    UpnpReport unmap();
    void stop();
};
} // namespace lantern
