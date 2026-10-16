#pragma once
#include "core.hpp"
#include <array>
#include <optional>
#include <string_view>
namespace lantern {
enum class AddressKind : uint32_t {
    invalid,
    private_v4,
    ula_v6,
    global_v6,
    public_v4,
    cgnat_v4,
    loopback,
    link_local,
    multicast
};
struct NumericAddress {
    std::array<uint8_t, 16> bytes{};
    bool v6 = false;
};
std::optional<NumericAddress> numeric_address(std::string_view);
AddressKind classify_address(std::string_view);
std::string normalize_address(std::string_view);
std::string source_key(std::string_view); // IPv6 /64, IPv4 address; mapped IPv4 is IPv4.
bool local_pairing_source(std::string_view);
bool valid_hostname(std::string_view); // ASCII DNS only, >=2 labels, no IP literals.
bool valid_connection_host(std::string_view);
enum class EndpointKind : uint32_t { lan_v4 = 1, lan_v6 = 2, public_v6 = 3, hostname = 4, public_v4 = 5 };
struct ConnectionEndpoint {
    EndpointKind kind = EndpointKind::lan_v4;
    std::string host;
    uint16_t port = 47443;
    uint32_t priority = 0;
    uint64_t expires_at = 0; // Unix seconds, 0 means unspecified.
    uint32_t failures = 0;
};
enum class EndpointFailure { none, dns, timeout, refused, pin_mismatch, transport, cancelled };
struct EndpointProbe {
    ConnectionEndpoint endpoint;
    EndpointFailure failure = EndpointFailure::none;
    uint64_t elapsed_ms = 0, start_ms = 0;
    bool tcp_ok = false, tls_ok = false;
    std::optional<uint64_t> rtt_ms;
};
void record_endpoint_results(std::vector<ConnectionEndpoint>&, const std::vector<EndpointProbe>&);
bool is_public(EndpointKind);
ConnectionEndpoint connection_endpoint(const std::string&, uint16_t);
std::vector<ConnectionEndpoint> merge_endpoints(const std::vector<ConnectionEndpoint>& saved,
                                                const std::vector<ConnectionEndpoint>& fresh, uint64_t now);
void write_endpoints(Writer&, const std::vector<ConnectionEndpoint>&);
std::vector<ConnectionEndpoint> read_endpoints(Reader&);
} // namespace lantern
