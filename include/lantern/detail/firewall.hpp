#pragma once
#include "../lan.hpp"
#include <netfw.h>
namespace lantern::detail {
// Configures an unattached rule without adding it to OS policy; shared by the elevated writer and tests.
void configure_firewall_rule(INetFwRule*, const std::wstring& exe, uint16_t port, long protocol);
} // namespace lantern::detail
