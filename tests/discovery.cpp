#include "lantern/discovery.hpp"
#include <iostream>
using namespace lantern;
int main() {
    try {
        init_sockets();
        auto nonce = random_token(), pin = random_token();
        auto request = discovery_query(nonce);
        auto reply = discovery_reply(request, "Generated fixture", pin, 47443);
        auto parsed = parse_discovery(reply, nonce, "127.0.0.1");
        int checks = 0;
        auto check = [&](bool ok) {
            if (!ok)
                throw std::runtime_error("discovery assertion");
            ++checks;
        };
        check(request.size() == 128 && reply.size() <= 256 && parsed.pin == pin && parsed.port == 47443);
        bool rejected = false;
        try {
            parse_discovery(reply, random_token(), "127.0.0.1");
        } catch (const Error&) {
            rejected = true;
        }
        check(rejected);
        for (size_t n = 0; n < reply.size(); n++) {
            rejected = false;
            try {
                parse_discovery({reply.data(), n}, nonce, "127.0.0.1");
            } catch (const Error&) {
                rejected = true;
            }
            check(rejected);
        }
        request.back() = 1;
        rejected = false;
        try {
            discovery_reply(request, "fixture", pin, 47443);
        } catch (const Error&) {
            rejected = true;
        }
        check(rejected);
        DiscoveryResponder responder("127.0.0.1", 47443, pin);
        Cancel cancel;
        auto devices = discover_devices(cancel, "127.0.0.1");
        check(!devices.empty() && devices[0].pin == pin && devices[0].host == "127.0.0.1");
        cancel.requested = true;
        rejected = false;
        try {
            discover_devices(cancel, "127.0.0.1");
        } catch (const Error& e) {
            rejected = e.code == Code::cancelled;
        }
        check(rejected);
        responder.stop();
        std::cout << "{\"success\":true,\"checks\":" << checks
                  << ",\"actual_udp_loopback\":true,\"advertisement_grants_trust\":false}" << std::endl;
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
