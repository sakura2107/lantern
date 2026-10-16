#pragma once
#include "win.hpp"
namespace lantern {
struct EndpointConnectError : Error {
    EndpointFailure reason;
    std::vector<EndpointProbe> attempts;
    EndpointConnectError(EndpointFailure why, std::vector<EndpointProbe> a)
        : Error(why == EndpointFailure::pin_mismatch ? Code::unauthorized : Code::io,
                why == EndpointFailure::pin_mismatch ? "主机证书指纹不匹配" : "所有连接入口均不可用"),
          reason(why), attempts(std::move(a)) {}
};
struct EndpointConnection {
    std::unique_ptr<Tls> tls;
    ConnectionEndpoint selected;
    std::vector<EndpointProbe> attempts;
    bool public_path = false;
};
EndpointConnection connect_endpoints(const std::vector<ConnectionEndpoint>&, const std::string& pin,
                                     std::chrono::milliseconds timeout = std::chrono::seconds(10));
std::vector<EndpointProbe> probe_endpoints(const std::vector<ConnectionEndpoint>&, const std::string& pin,
                                           std::chrono::milliseconds timeout = std::chrono::seconds(10));
std::vector<ConnectionEndpoint> client_endpoints(Client&); // Capability-gated; empty for older hosts.
} // namespace lantern
