#pragma once
#include "core.hpp"
#include <optional>
#include <string_view>
namespace lantern {
struct PendingInvite {
    std::string id, label;
    uint32_t scopes = 0;
    uint64_t expires_at = 0;
    uint32_t failed_attempts = 0;
};
// Only this one-time return value contains the plaintext invitation code.
struct PairingInvite : PendingInvite {
    std::string host_name, pin, code;
};
struct PairingLink {
    std::string host_name;
    std::vector<std::string> addresses;
    uint16_t port = 47443;
    std::string pin, code;
    uint64_t expires_at = 0;
};
uint64_t pairing_unix_time();
bool valid_pairing_address(std::string_view);
bool valid_pairing_label(const std::string&);
std::string pairing_uri(const PairingInvite&, const std::vector<std::string>& addresses, uint16_t port);
std::optional<PairingLink> parse_pairing_uri(std::string_view) noexcept;
struct QrMatrix {
    int size = 0;
    std::vector<bool> modules;
};
QrMatrix qr_encode(std::string_view text); // Exact error-correction level M; quiet zone is a UI concern.
} // namespace lantern
