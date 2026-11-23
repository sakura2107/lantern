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
    std::map<std::string, unsigned> port_actions;
    std::map<std::string, unsigned> pinhole_actions;
    uint16_t http_port = 0;
    static std::string xml_value(const std::string& text, const std::string& tag) {
        auto open = "<" + tag + ">", close = "</" + tag + ">";
        auto a = text.find(open);
        if (a == text.npos)
            return {};
        a += open.size();
        auto b = text.find(close, a);
        return b == text.npos ? std::string() : text.substr(a, b - a);
    }
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
                auto ext_port = xml_value(request, "NewExternalPort");
                auto internal_port = xml_value(request, "InternalPort");
                auto proto_name = xml_value(request, "NewProtocol");
                auto proto_num = xml_value(request, "Protocol");
                auto uid = xml_value(request, "UniqueID");
                std::string proto;
                if (proto_name == "UDP" || proto_num == "17")
                    proto = "UDP";
                else if (proto_name == "TCP" || proto_num == "6")
                    proto = "TCP";
                {
                    std::lock_guard lock(mutex);
                    ++actions[action];
                    if (proto == "UDP")
                        ++actions[action + ":udp"];
                    if (!proto.empty() && !ext_port.empty())
                        ++port_actions[action + ":" + proto + ":" + ext_port];
                    if (!proto.empty() && !internal_port.empty())
                        ++port_actions[action + ":" + proto + ":" + internal_port];
                    if (!uid.empty())
                        ++pinhole_actions[action + ":" + uid];
                }
                bool is_v4 = action == "AddPortMapping" || action == "DeletePortMapping" ||
                             action == "GetExternalIPAddress";
                bool is_v6 = action == "AddPinhole" || action == "UpdatePinhole" || action == "DeletePinhole";
                bool reject = deny || (deny_v4 && is_v4) || (deny_v6 && is_v6) ||
                              (deny_tcp && proto == "TCP") || (deny_udp && proto == "UDP") ||
                              (deny_update_pinhole && action == "UpdatePinhole");
                if (reject) {
                    code = 500;
                    body = "<error><errorCode>606</errorCode></error>";
                } else if (action == "GetExternalIPAddress")
                    body = "<NewExternalIPAddress>" +
                           (cgnat ? std::string("100.64.1.1") : std::string("192.0.2.10")) +
                           "</NewExternalIPAddress>";
                else if (action == "AddPinhole")
                    body = "<UniqueID>" + std::to_string(pinhole_id.load()) + "</UniqueID>";
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
    std::atomic_bool deny_v4 = false, deny_v6 = false, deny_tcp = false, deny_udp = false,
                     deny_update_pinhole = false;
    std::atomic<uint32_t> pinhole_id = 42;
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
    unsigned port_count(const std::string& action, const std::string& proto, uint16_t port) {
        std::lock_guard lock(mutex);
        return port_actions[action + ":" + proto + ":" + std::to_string(port)];
    }
    unsigned pinhole_count(const std::string& action, uint32_t id) {
        std::lock_guard lock(mutex);
        return pinhole_actions[action + ":" + std::to_string(id)];
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
        {
            UpnpIgd media(mock.target());request.protocol=17;request.lease_seconds=4;
            auto udp=media.map(request);check(udp.ipv4.status==UpnpStatus::success&&udp.ipv6.status==UpnpStatus::success,"same-port UDP mapping and IPv6 pinhole");
            media.renew();media.unmap();
            check(mock.count("AddPortMapping:udp")==2&&mock.count("DeletePortMapping:udp")==1&&mock.count("AddPinhole:udp")==1,
                "UDP add/renew/delete protocol fields remain independent of TCP");
        }
        // WP-SEC-08 (SEC-04): Lease ownership, expiry, protocol independence, IPv6 pinhole independence, and explicit remove.
        {
            // 1. Confirmed lease (mapped + renewed within deadline): auto unmap deletes by port/protocol and pinhole ID.
            mock.pinhole_id = 51;
            UpnpIgd owned(mock.target());
            UpnpRequest r;
            r.ipv4 = "127.0.0.1";
            r.ipv6 = "2001:db8::1";
            r.internal_port = 47501;
            r.external_port = 47501;
            r.protocol = 6;
            r.lease_seconds = 4;
            auto mapped = owned.map(r);
            check(mapped.ipv4.status == UpnpStatus::success && mapped.ipv6.pinhole_id == 51u,
                  "confirmed lease mapped");
            auto renewed = owned.renew();
            check(renewed.ipv4.status == UpnpStatus::success && renewed.ipv6.status == UpnpStatus::success,
                  "confirmed lease renewed");
            auto unmapped = owned.unmap();
            check(unmapped.ipv4.external_port == 0 && !unmapped.ipv6.pinhole_id &&
                      mock.port_count("AddPortMapping", "TCP", 47501) == 2 &&
                      mock.port_count("DeletePortMapping", "TCP", 47501) == 1 &&
                      mock.pinhole_count("UpdatePinhole", 51) == 1 &&
                      mock.pinhole_count("DeletePinhole", 51) == 1,
                  "confirmed lease auto unmap deletes gateway mapping and pinhole");
        }
        {
            // 2. Failed renewal enters unknown ownership: auto unmap clears local state without sending gateway deletes.
            mock.pinhole_id = 52;
            UpnpIgd unknown(mock.target());
            UpnpRequest r;
            r.ipv4 = "127.0.0.1";
            r.ipv6 = "2001:db8::1";
            r.internal_port = 47502;
            r.external_port = 47502;
            r.protocol = 6;
            r.lease_seconds = 4;
            auto mapped = unknown.map(r);
            check(mapped.ipv4.status == UpnpStatus::success && mapped.ipv6.pinhole_id == 52u,
                  "unknown-state test lease mapped");
            mock.deny_v4 = true;
            mock.deny_update_pinhole = true;
            auto failed = unknown.renew();
            mock.deny_v4 = false;
            mock.deny_update_pinhole = false;
            check(failed.ipv4.status == UpnpStatus::denied && failed.ipv6.status == UpnpStatus::denied,
                  "failed renew transitions IPv4 and IPv6 leases to unknown state");
            auto cleared = unknown.unmap();
            check(cleared.ipv4.external_port == 0 && !cleared.ipv6.pinhole_id &&
                      unknown.status().ipv4.external_port == 0 && !unknown.status().ipv6.pinhole_id,
                  "auto unmap clears local records on unknown ownership");
            check(mock.port_count("DeletePortMapping", "TCP", 47502) == 0 &&
                      mock.pinhole_count("DeletePinhole", 52) == 0,
                  "auto unmap sends no gateway delete when ownership is unknown");
            // 6a. Explicit admin remove still issues delete for specified port and pinhole ID.
            auto removed = unknown.remove(47502, 52u);
            check(removed.ipv4.status == UpnpStatus::idle && removed.ipv6.status == UpnpStatus::idle &&
                      mock.port_count("DeletePortMapping", "TCP", 47502) == 1 &&
                      mock.pinhole_count("DeletePinhole", 52) == 1,
                  "explicit admin remove deletes specified TCP port and pinhole ID");
        }
        {
            // 3. Expired lease past deadline: status reports expired, renew and auto unmap send no gateway delete.
            mock.pinhole_id = 53;
            UpnpIgd expiring(mock.target());
            UpnpRequest r;
            r.ipv4 = "127.0.0.1";
            r.ipv6 = "2001:db8::1";
            r.internal_port = 47503;
            r.external_port = 47503;
            r.protocol = 6;
            r.lease_seconds = 2;
            auto mapped = expiring.map(r);
            check(mapped.ipv4.status == UpnpStatus::success && mapped.ipv6.pinhole_id == 53u,
                  "expiring lease mapped");
            std::this_thread::sleep_for(std::chrono::milliseconds(2200));
            auto st = expiring.status();
            check(st.ipv4.status == UpnpStatus::expired && st.ipv6.status == UpnpStatus::expired,
                  "expired lease reports expired status after deadline");
            auto after_renew = expiring.renew();
            check(after_renew.ipv4.status == UpnpStatus::expired && after_renew.ipv6.status == UpnpStatus::expired &&
                      mock.port_count("AddPortMapping", "TCP", 47503) == 1 &&
                      mock.pinhole_count("UpdatePinhole", 53) == 0,
                  "expired lease is not renewed");
            auto cleared = expiring.unmap();
            check(cleared.ipv4.external_port == 0 && !cleared.ipv6.pinhole_id &&
                      mock.port_count("DeletePortMapping", "TCP", 47503) == 0 &&
                      mock.pinhole_count("DeletePinhole", 53) == 0,
                  "expired lease auto unmap clears locally without gateway delete");
        }
        {
            // 4. TCP and UDP leases on the same external port are independent: TCP failure does not affect UDP.
            UpnpIgd tcp_igd(mock.target()), udp_igd(mock.target());
            UpnpRequest tcp_req;
            tcp_req.ipv4 = "127.0.0.1";
            tcp_req.ipv6 = "2001:db8::1";
            tcp_req.internal_port = 47504;
            tcp_req.external_port = 47504;
            tcp_req.protocol = 6;
            tcp_req.lease_seconds = 4;
            UpnpRequest udp_req = tcp_req;
            udp_req.protocol = 17;

            mock.pinhole_id = 54;
            auto m_tcp = tcp_igd.map(tcp_req);
            mock.pinhole_id = 55;
            auto m_udp = udp_igd.map(udp_req);
            check(m_tcp.ipv4.status == UpnpStatus::success && m_udp.ipv4.status == UpnpStatus::success &&
                      m_tcp.ipv6.pinhole_id == 54u && m_udp.ipv6.pinhole_id == 55u,
                  "TCP and UDP leases mapped on same external port");

            mock.deny_tcp = true;
            auto r_tcp = tcp_igd.renew();
            auto r_udp = udp_igd.renew();
            mock.deny_tcp = false;
            check(r_tcp.ipv4.status == UpnpStatus::denied && r_udp.ipv4.status == UpnpStatus::success &&
                      r_udp.ipv6.status == UpnpStatus::success,
                  "TCP renewal failure does not affect UDP renewal");

            tcp_igd.unmap();
            udp_igd.unmap();
            check(mock.port_count("DeletePortMapping", "TCP", 47504) == 0 &&
                      mock.port_count("DeletePortMapping", "UDP", 47504) == 1 &&
                      mock.pinhole_count("DeletePinhole", 54) == 1 &&
                      mock.pinhole_count("DeletePinhole", 55) == 1,
                  "TCP unknown lease skipped while UDP lease and confirmed pinholes still deleted");
        }
        {
            // 5. IPv6 pinhole lease deadline and ownership are judged independently of IPv4 mapping.
            mock.pinhole_id = 56;
            UpnpIgd v4_fail(mock.target());
            UpnpRequest r;
            r.ipv4 = "127.0.0.1";
            r.ipv6 = "2001:db8::1";
            r.internal_port = 47505;
            r.external_port = 47505;
            r.protocol = 6;
            r.lease_seconds = 4;
            v4_fail.map(r);
            mock.deny_v4 = true;
            auto r1 = v4_fail.renew();
            mock.deny_v4 = false;
            check(r1.ipv4.status == UpnpStatus::denied && r1.ipv6.status == UpnpStatus::success,
                  "IPv4 renewal failure leaves IPv6 pinhole confirmed");
            v4_fail.unmap();
            check(mock.port_count("DeletePortMapping", "TCP", 47505) == 0 &&
                      mock.pinhole_count("DeletePinhole", 56) == 1,
                  "IPv6 pinhole still deleted when IPv4 ownership is unknown");

            // Reverse: IPv6 pinhole fails renewal and expires while IPv4 is renewed and stays confirmed.
            mock.pinhole_id = 57;
            UpnpIgd v6_fail(mock.target());
            r.internal_port = 47506;
            r.external_port = 47506;
            r.lease_seconds = 2;
            v6_fail.map(r);
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            mock.deny_update_pinhole = true;
            auto r2 = v6_fail.renew();
            mock.deny_update_pinhole = false;
            check(r2.ipv4.status == UpnpStatus::success && r2.ipv6.status == UpnpStatus::denied,
                  "IPv6 pinhole renewal failure leaves IPv4 mapping confirmed and extended");
            std::this_thread::sleep_for(std::chrono::milliseconds(1100));
            auto mid = v6_fail.status();
            check(mid.ipv4.status == UpnpStatus::success && mid.ipv6.status == UpnpStatus::expired,
                  "IPv6 pinhole expires on its own lease while renewed IPv4 remains valid");
            v6_fail.unmap();
            check(mock.port_count("DeletePortMapping", "TCP", 47506) == 1 &&
                      mock.pinhole_count("DeletePinhole", 57) == 0,
                  "auto unmap deletes confirmed IPv4 mapping without deleting expired IPv6 pinhole");
        }
        {
            // 6b. Explicit admin remove on UDP lease after auto unmap skipped unknown mapping.
            mock.pinhole_id = 58;
            UpnpIgd udp_admin(mock.target());
            UpnpRequest r;
            r.ipv4 = "127.0.0.1";
            r.ipv6 = "2001:db8::1";
            r.internal_port = 47507;
            r.external_port = 47507;
            r.protocol = 17;
            r.lease_seconds = 4;
            udp_admin.map(r);
            mock.deny_udp = true;
            mock.deny_update_pinhole = true;
            udp_admin.renew();
            mock.deny_udp = false;
            mock.deny_update_pinhole = false;
            udp_admin.unmap();
            check(mock.port_count("DeletePortMapping", "UDP", 47507) == 0 &&
                      mock.pinhole_count("DeletePinhole", 58) == 0,
                  "UDP unknown lease skipped by auto unmap");
            udp_admin.remove(47507, 58u);
            check(mock.port_count("DeletePortMapping", "UDP", 47507) == 1 &&
                      mock.pinhole_count("DeletePinhole", 58) == 1,
                  "explicit admin remove deletes specified UDP port and pinhole ID");
        }
        std::cout << checks << " local mock UPnP checks passed; no real router changed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
