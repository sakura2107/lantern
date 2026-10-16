#include "lantern/upnp.hpp"
#include <iostream>
#include <map>
#include <thread>
#include <ws2tcpip.h>
using namespace lantern;
namespace {
unsigned checks = 0;
void check(bool v, const char* s) {
    if (!v)
        throw std::runtime_error(s);
    ++checks;
    std::cout << s << std::endl;
}
class MockIgd {
    SOCKET tcp = INVALID_SOCKET, udp = INVALID_SOCKET;
    std::atomic_bool stopped = false;
    std::thread http_thread, ssdp_thread;
    std::mutex mutex;
    std::map<std::string, unsigned> actions;
    uint16_t http_port = 0;
    void http_loop() {
        while (!stopped) {
            SOCKET s = accept(tcp, nullptr, nullptr);
            if (s == INVALID_SOCKET)
                continue;
            DWORD ms = 1000;
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&ms), sizeof(ms));
            std::string request;
            char b[8192];
            for (;;) {
                int n = recv(s, b, sizeof(b), 0);
                if (n <= 0)
                    break;
                request.append(b, n);
                auto end = request.find("\r\n\r\n");
                if (end != request.npos) {
                    auto length = request.find("Content-Length:");
                    if (length == request.npos)
                        length = request.find("Content-length:");
                    size_t content = length == request.npos ? 0 : std::stoul(request.substr(length + 15));
                    if (request.size() >= end + 4 + content)
                        break;
                }
                if (request.size() > 65536)
                    break;
            }
            std::string body;
            int code = 200;
            if (request.starts_with("GET ")) {
                body = "<root><device><serviceList>";
                if (!unsupported)
                    for (auto type : {"WANIPConnection:2", "WANIPv6FirewallControl:1"})
                        body += "<service><serviceType>urn:schemas-upnp-org:service:" + std::string(type) +
                                "</serviceType><controlURL>" +
                                (external_url ? std::string("http://192.0.2.1/control")
                                              : std::string("/control")) +
                                "</controlURL></service>";
                body += "</serviceList></device></root>";
            } else {
                auto hash = request.find('#'), end = hash == request.npos ? hash : request.find('"', hash);
                auto action = hash == request.npos ? std::string() : request.substr(hash + 1, end - hash - 1);
                {
                    std::lock_guard lock(mutex);
                    ++actions[action];
                }
                if (deny) {
                    code = 500;
                    body = "<error><errorCode>606</errorCode></error>";
                } else if (action == "GetExternalIPAddress")
                    body = "<NewExternalIPAddress>" +
                           (cgnat ? std::string("100.64.1.1") : std::string("192.0.2.10")) +
                           "</NewExternalIPAddress>";
                else if (action == "AddPinhole")
                    body = "<UniqueID>42</UniqueID>";
                else
                    body = "<OK/>";
                body = "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>" + body +
                       "</s:Body></s:Envelope>";
            }
            auto response = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Error") +
                            "\r\nContent-Type: text/xml\r\nContent-Length: " + std::to_string(body.size()) +
                            "\r\nConnection: close\r\n\r\n" + body;
            size_t at = 0;
            while (at < response.size()) {
                int n = send(s, response.data() + at, int(response.size() - at), 0);
                if (n <= 0)
                    break;
                at += n;
            }
            shutdown(s, SD_BOTH);
            closesocket(s);
        }
    }
    void ssdp_loop() {
        while (!stopped) {
            char b[4096];
            sockaddr_in peer{};
            int size = sizeof(peer);
            int n = recvfrom(udp, b, sizeof(b), 0, reinterpret_cast<sockaddr*>(&peer), &size);
            if (n <= 0)
                continue;
            auto reply = "HTTP/1.1 200 OK\r\nLOCATION: http://127.0.0.1:" + std::to_string(http_port) +
                         "/root.xml\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:2\r\n\r\n";
            sendto(udp, reply.data(), int(reply.size()), 0, reinterpret_cast<sockaddr*>(&peer), size);
        }
    }

  public:
    std::atomic_bool cgnat = false, unsupported = false, deny = false, external_url = false;
    uint16_t ssdp_port = 0;
    MockIgd() {
        init_sockets();
        tcp = listen_socket("127.0.0.1", 0);
        sockaddr_in address{};
        int size = sizeof(address);
        getsockname(tcp, reinterpret_cast<sockaddr*>(&address), &size);
        http_port = ntohs(address.sin_port);
        udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(udp, reinterpret_cast<sockaddr*>(&address), sizeof(address)))
            throw std::runtime_error("Mock SSDP bind");
        getsockname(udp, reinterpret_cast<sockaddr*>(&address), &size);
        ssdp_port = ntohs(address.sin_port);
        DWORD ms = 100;
        setsockopt(udp, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&ms), sizeof(ms));
        http_thread = std::thread([&] { http_loop(); });
        ssdp_thread = std::thread([&] { ssdp_loop(); });
    }
    ~MockIgd() {
        stopped = true;
        shutdown(tcp, SD_BOTH);
        closesocket(tcp);
        closesocket(udp);
        http_thread.join();
        ssdp_thread.join();
    }
    SsdpTarget target() {
        return {"127.0.0.1", ssdp_port, 150};
    }
    unsigned count(const std::string& action) {
        std::lock_guard lock(mutex);
        return actions[action];
    }
};
} // namespace
int main() {
    try {
        MockIgd mock;
        UpnpRequest request;
        request.ipv4 = "127.0.0.1";
        request.ipv6 = "2001:db8::1";
        request.lease_seconds = 4;
        UpnpIgd igd(mock.target());
        check(igd.discover() == UpnpStatus::success, "SSDP and HTTP description discovery");
        auto report = igd.map(request);
        check(report.ipv4.status == UpnpStatus::success && report.ipv4.external_address == "192.0.2.10" &&
                  report.ipv4.external_port == 47443,
              "IPv4 SOAP mapping and external address");
        check(report.ipv6.status == UpnpStatus::success && report.ipv6.external_port == 47443 &&
                  report.ipv6.pinhole_id == 42u,
              "IGDv2 IPv6 firewall pinhole");
        igd.renew();
        check(mock.count("AddPortMapping") == 2 && mock.count("UpdatePinhole") == 1,
              "mapping and pinhole renewal");
        igd.unmap();
        check(mock.count("DeletePortMapping") == 1 && mock.count("DeletePinhole") == 1,
              "mapping and pinhole deletion");
        mock.cgnat = true;
        report = igd.map(request);
        check(report.ipv4.status == UpnpStatus::cgnat && mock.count("AddPortMapping") == 2,
              "CGNAT recognized without adding IPv4 mapping");
        igd.unmap();
        mock.cgnat = false;
        mock.deny = true;
        report = igd.map(request);
        check(report.ipv4.status == UpnpStatus::denied && report.ipv6.status == UpnpStatus::denied,
              "router policy denial structured");
        mock.deny = false;
        mock.unsupported = true;
        UpnpIgd unsupported(mock.target());
        check(unsupported.discover() == UpnpStatus::unsupported, "IGD without required services unsupported");
        mock.unsupported = false;
        mock.external_url = true;
        UpnpIgd external(mock.target());
        check(external.discover() == UpnpStatus::unsupported,
              "SSDP cannot redirect SOAP outside LAN responder");
        mock.external_url = false;
        auto maps = mock.count("AddPortMapping"), updates = mock.count("UpdatePinhole"),
             deletes = mock.count("DeletePinhole");
        {
            UpnpLease lease(mock.target());
            request.lease_seconds = 2;
            lease.map(request);
            std::this_thread::sleep_for(std::chrono::milliseconds(1300));
            check(mock.count("AddPortMapping") >= maps + 2 && mock.count("UpdatePinhole") >= updates + 1,
                  "host lease worker renews at half lease");
        }
        check(mock.count("DeletePinhole") == deletes + 1, "host lease destruction removes pinhole");
        UpnpIgd none({"127.0.0.1", uint16_t(mock.ssdp_port + 1), 100});
        check(none.discover() == UpnpStatus::no_igd, "missing IGD structured result");
        bool rejected = false;
        try {
            UpnpIgd outside({"192.0.2.1", 1900, 100});
            outside.discover();
        } catch (const Error&) {
            rejected = true;
        }
        check(rejected, "discovery targets restricted to LAN");
        std::cout << checks << " local mock UPnP checks passed; no real router changed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
