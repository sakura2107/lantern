#pragma once
#include "input_channel.hpp"
#include "windows_client.hpp"
namespace lantern {
// Keeps channel identity across TLS reconnects. A new instance starts a new deliberate control session.
std::unique_ptr<InputChannel> open_input_channel(ClientCredentials, InputChannel::Events = {},InputChannel::Results = {});
} // namespace lantern
