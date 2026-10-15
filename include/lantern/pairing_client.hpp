#pragma once
#include "win.hpp"
namespace lantern {
enum class PairError {
    invalid_link,
    host_too_old,
    invalid_invite,
    unreachable,
    pin_mismatch,
    busy,
    protocol_error,
    outcome_unknown
};
struct PairingError : Error {
    PairError reason;
    PairingError(PairError r, Code c, std::string message) : Error(c, std::move(message)), reason(r) {}
};
struct PairResult {
    std::string host;
    uint16_t port;
    std::string pin, device_id, token;
    uint32_t scopes;
};
PairResult pair_with_link(const PairingLink&, const std::string& device_label);
} // namespace lantern
