#pragma once
#include "lantern/win.hpp"

namespace lantern {
// Internal probe for the Schannel handshake test, with no network peer required.
struct TlsFirstSendProbe {
    Bytes output;
    unsigned buffer_frees = 0, context_deletes = 0;

    void handshake(Identity* identity, std::span<const uint8_t> input = {});
};
} // namespace lantern
