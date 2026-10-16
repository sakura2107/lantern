#include "lantern/upnp.hpp"
#include "lantern/lan.hpp"
#include <cctype>
#include <charconv>
#include <condition_variable>
#include <objbase.h>
#include <set>
#include <thread>
#include <winhttp.h>
#include <wrl/client.h>
#include <ws2tcpip.h>
#include <xmllite.h>
namespace lantern {
namespace {
using Microsoft::WRL::ComPtr;
struct HttpHandle {
    HINTERNET h = nullptr;
    ~HttpHandle() {
        if (h)
            WinHttpCloseHandle(h);
    }
    operator HINTERNET() const {
        return h;
    }
};
struct RouterError {
    UpnpStatus status;
};
void checked(bool b) {
    if (!b)
        throw RouterError{UpnpStatus::network_error};
}
struct Url {
    std::wstring host, path;
    INTERNET_PORT port = 80;
    std::string address;
};
bool router_address(const std::string& ip) {
    auto k = classify_address(ip);
    return k == AddressKind::private_v4 || k == AddressKind::ula_v6 || k == AddressKind::link_local ||
           k == AddressKind::loopback;
}
Url url(const std::string& value, const std::string& responder) {
    if (value.size() > 2048 || value.find_first_of("\r\n#") != value.npos)
        throw RouterError{UpnpStatus::unsupported};
    auto text = wide(value);
    URL_COMPONENTS c{};
    c.dwStructSize = sizeof(c);
    c.dwHostNameLength = c.dwUrlPathLength = c.dwExtraInfoLength = c.dwUserNameLength = c.dwPasswordLength =
        DWORD(-1);
    checked(WinHttpCrackUrl(text.c_str(), DWORD(text.size()), 0, &c));
    if (c.nScheme != INTERNET_SCHEME_HTTP || c.dwUserNameLength || c.dwPasswordLength)
        throw RouterError{UpnpStatus::unsupported};
    Url out;
    out.host.assign(c.lpszHostName, c.dwHostNameLength);
    out.address = utf8(out.host);
    if (!numeric_address(out.address) || !router_address(out.address) ||
        normalize_address(out.address) != normalize_address(responder))
        throw RouterError{UpnpStatus::denied};
    out.port = c.nPort;
    out.path.assign(c.lpszUrlPath, c.dwUrlPathLength);
    if (out.path.empty())
        out.path = L"/";
    if (c.dwExtraInfoLength)
        out.path.append(c.lpszExtraInfo, c.dwExtraInfoLength);
    return out;
}
std::string http(const Url& u, const std::string& action = {}, const std::string& body = {}) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    HttpHandle session{WinHttpOpen(L"Lantern/1 UPnP", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0)};
    checked(session.h != nullptr);
    checked(WinHttpSetTimeouts(session, 2000, 2000, 2000, 2000));
    DWORD retries = 0;
    checked(WinHttpSetOption(session, WINHTTP_OPTION_CONNECT_RETRIES, &retries, sizeof(retries)));
    HttpHandle connection{WinHttpConnect(session, u.host.c_str(), u.port, 0)};
    checked(connection.h != nullptr);
    HttpHandle request{WinHttpOpenRequest(connection, action.empty() ? L"GET" : L"POST", u.path.c_str(),
                                          nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0)};
    checked(request.h != nullptr);
    DWORD disable = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
    checked(WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable)));
    auto headers = action.empty() ? std::wstring()
                                  : L"Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" +
                                        wide(action) + L"\"\r\n";
    checked(WinHttpSendRequest(request, headers.c_str(), DWORD(headers.size()),
                               body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()),
                               DWORD(body.size()), DWORD(body.size()), 0));
    checked(WinHttpReceiveResponse(request, nullptr));
    DWORD code = 0, size = sizeof(code);
    checked(WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX));
    std::string result;
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline)
            throw RouterError{UpnpStatus::network_error};
        char b[8192];
        DWORD n = 0;
        checked(WinHttpReadData(request, b, sizeof(b), &n));
        if (!n)
            break;
        if (result.size() + n > 256 * 1024)
            throw RouterError{UpnpStatus::unsupported};
        result.append(b, n);
    }
    if (code != 200) {
        if (result.find("<errorCode>401</errorCode>") != result.npos ||
            result.find("<errorCode>602</errorCode>") != result.npos)
            throw RouterError{UpnpStatus::unsupported};
        throw RouterError{UpnpStatus::denied};
    }
    return result;
}
struct Node {
    std::string name, text;
    std::vector<std::unique_ptr<Node>> children;
};
Node xml(const std::string& text) {
    ComPtr<IStream> stream;
    checked(SUCCEEDED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)));
    ULONG written = 0;
    checked(SUCCEEDED(stream->Write(text.data(), ULONG(text.size()), &written)) && written == text.size());
    LARGE_INTEGER zero{};
    checked(SUCCEEDED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)));
    ComPtr<IXmlReader> reader;
    checked(SUCCEEDED(
        CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(reader.GetAddressOf()), nullptr)));
    checked(SUCCEEDED(reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit)));
    checked(SUCCEEDED(reader->SetProperty(XmlReaderProperty_MaxElementDepth, 32)));
    checked(SUCCEEDED(reader->SetInput(stream.Get())));
    Node root;
    std::vector<Node*> stack{&root};
    XmlNodeType type;
    HRESULT hr;
    while ((hr = reader->Read(&type)) == S_OK) {
        if (type == XmlNodeType_Element) {
            const wchar_t* name = nullptr;
            UINT n = 0;
            checked(SUCCEEDED(reader->GetLocalName(&name, &n)));
            auto node = std::make_unique<Node>();
            node->name = utf8(std::wstring(name, n));
            auto* p = node.get();
            stack.back()->children.push_back(std::move(node));
            if (!reader->IsEmptyElement())
                stack.push_back(p);
        } else if (type == XmlNodeType_EndElement) {
            if (stack.size() < 2)
                throw RouterError{UpnpStatus::unsupported};
            stack.pop_back();
        } else if (type == XmlNodeType_Text || type == XmlNodeType_CDATA) {
            const wchar_t* value = nullptr;
            UINT n = 0;
            checked(SUCCEEDED(reader->GetValue(&value, &n)));
            stack.back()->text += utf8(std::wstring(value, n));
        }
    }
    if (hr != S_FALSE || stack.size() != 1)
        throw RouterError{UpnpStatus::unsupported};
    return root;
}
std::string child(const Node& node, const std::string& name) {
    for (auto& c : node.children)
        if (c->name == name)
            return c->text;
    return {};
}
std::string find(const Node& node, const std::string& name) {
    if (node.name == name)
        return node.text;
    for (auto& c : node.children) {
        auto value = find(*c, name);
        if (!value.empty())
            return value;
    }
    return {};
}
struct Service {
    std::string type;
    Url control;
};
void services(const Node& node, const std::string& location, const std::string& responder,
              std::vector<Service>& out) {
    if (node.name == "service") {
        auto type = child(node, "serviceType"), control = child(node, "controlURL");
        if (type == "urn:schemas-upnp-org:service:WANIPConnection:1" ||
            type == "urn:schemas-upnp-org:service:WANIPConnection:2" ||
            type == "urn:schemas-upnp-org:service:WANIPv6FirewallControl:1") {
            if (control.empty())
                return;
            if (control.find("://") == control.npos) {
                auto slash = location.find('/', 7);
                auto origin = slash == location.npos ? location : location.substr(0, slash);
                if (control.front() == '/')
                    control = origin + control;
                else {
                    auto end = location.rfind('/');
                    control = (end < 7 ? origin + "/" : location.substr(0, end + 1)) + control;
                }
            }
            out.push_back({type, url(control, responder)});
        }
    }
    for (auto& c : node.children)
        services(*c, location, responder, out);
}
std::string escape(const std::string& value) {
    std::string out;
    for (char c : value) {
        if (c == '&')
            out += "&amp;";
        else if (c == '<')
            out += "&lt;";
        else if (c == '>')
            out += "&gt;";
        else
            out += c;
    }
    return out;
}
std::string soap(const Service& s, const std::string& action,
                 const std::vector<std::pair<std::string, std::string>>& args) {
    std::string body =
        "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
        action + " xmlns:u=\"" + s.type + "\">";
    for (auto& [key, value] : args)
        body += "<" + key + ">" + escape(value) + "</" + key + ">";
    body += "</u:" + action + "></s:Body></s:Envelope>";
    return http(s.control, s.type + "#" + action, body);
}
uint32_t number(const std::string& text, uint32_t max) {
    uint32_t n = 0;
    auto r = std::from_chars(text.data(), text.data() + text.size(), n);
    if (text.empty() || r.ec != std::errc() || r.ptr != text.data() + text.size() || n > max)
        throw RouterError{UpnpStatus::unsupported};
    return n;
}
std::string header(const std::string& packet, const std::string& key) {
    size_t start = packet.find("\r\n");
    while (start != packet.npos) {
        start += 2;
        auto end = packet.find("\r\n", start);
        if (end == packet.npos)
            break;
        auto line = packet.substr(start, end - start);
        auto colon = line.find(':');
        if (colon != line.npos) {
            auto name = line.substr(0, colon);
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (name == key) {
                auto value = line.substr(colon + 1);
                auto first = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t");
                return first == value.npos ? "" : value.substr(first, last - first + 1);
            }
        }
        start = end;
    }
    return {};
}
} // namespace
struct UpnpIgd::Impl {
    SsdpTarget target;
    std::vector<Service> found;
    UpnpReport report;
    UpnpRequest request;
    std::optional<uint32_t> pinhole;
    explicit Impl(SsdpTarget t) : target(std::move(t)) {}
    Service* service(bool v6) {
        for (auto& s : found)
            if ((s.type.find("WANIPv6FirewallControl") != s.type.npos) == v6)
                return &s;
        return nullptr;
    }
    UpnpStatus discover() {
        if (report.ipv4.external_port || pinhole)
            return UpnpStatus::success;
        found.clear();
        init_sockets();
        if (target.timeout_ms < 1 || target.timeout_ms > 5000 || !target.port)
            throw Error(Code::invalid, "SSDP parameters invalid");
        auto address = numeric_address(target.address);
        if (!address || address->v6 ||
            (!router_address(target.address) && target.address != "239.255.255.250"))
            throw Error(Code::invalid, "SSDP target must be LAN");
        SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET)
            return UpnpStatus::network_error;
        struct Close {
            SOCKET s;
            ~Close() {
                closesocket(s);
            }
        } close{s};
        DWORD timeout = target.timeout_ms;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&timeout), sizeof(timeout));
        char ttl = 2;
        setsockopt(s, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(target.port);
        inet_pton(AF_INET, target.address.c_str(), &to.sin_addr);
        std::vector<std::string> interfaces;
        if (target.address == "239.255.255.250")
            for (auto& a : lan_addresses())
                if (!a.virtual_adapter && classify_address(a.address) == AddressKind::private_v4 &&
                    interfaces.size() < 8)
                    interfaces.push_back(a.address);
        if (interfaces.empty())
            interfaces.push_back("0.0.0.0");
        for (auto& adapter_address : interfaces) {
            in_addr local{};
            inet_pton(AF_INET, adapter_address.c_str(), &local);
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<char*>(&local), sizeof(local));
            for (auto version : {"1", "2"}) {
                std::string query =
                    "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: "
                    "1\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:" +
                    std::string(version) + "\r\n\r\n";
                sendto(s, query.data(), int(query.size()), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
            }
        }
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(target.timeout_ms);
        std::set<std::string> seen;
        bool replied = false;
        while (std::chrono::steady_clock::now() < end && seen.size() < 8) {
            char packet[8192];
            sockaddr_in from{};
            int size = sizeof(from);
            auto n = recvfrom(s, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&from), &size);
            if (n <= 0)
                break;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            if (!router_address(ip))
                continue;
            if (target.address != "239.255.255.250" &&
                normalize_address(ip) != normalize_address(target.address))
                continue;
            std::string response(packet, n);
            if (!response.starts_with("HTTP/1.1 200 "))
                continue;
            auto location = header(response, "location");
            if (location.empty() || !seen.insert(location).second)
                continue;
            replied = true;
            try {
                auto description = xml(http(url(location, ip)));
                auto base = find(description, "URLBase");
                if (base.empty())
                    base = location;
                else
                    url(base, ip);
                std::vector<Service> candidate;
                services(description, base, ip, candidate);
                if (!candidate.empty()) {
                    found = std::move(candidate);
                    return UpnpStatus::success;
                }
            } catch (const RouterError&) {
            }
        }
        return replied ? UpnpStatus::unsupported : UpnpStatus::no_igd;
    }
    UpnpReport map(const UpnpRequest& r) {
        if (!r.internal_port || !r.external_port || r.lease_seconds < 2 || r.lease_seconds > 86400 ||
            (!r.ipv4.empty() && (classify_address(r.ipv4) != AddressKind::private_v4 &&
                                 classify_address(r.ipv4) != AddressKind::loopback)) ||
            (!r.ipv6.empty() && classify_address(r.ipv6) != AddressKind::global_v6))
            throw Error(Code::invalid, "UPnP mapping parameters invalid");
        if (report.ipv4.external_port || pinhole) {
            auto old = unmap();
            if (old.ipv4.external_port || pinhole)
                return old;
        }
        request = r;
        auto status = found.empty() ? discover() : UpnpStatus::success;
        if (status != UpnpStatus::success) {
            if (!r.ipv4.empty())
                report.ipv4.status = status;
            if (!r.ipv6.empty())
                report.ipv6.status = status;
            return report;
        }
        auto perform = [&](bool v6) {
            auto& mapping = v6 ? report.ipv6 : report.ipv4;
            auto* s = service(v6);
            mapping = {};
            if (!s) {
                mapping.status = UpnpStatus::unsupported;
                return;
            }
            try {
                if (v6) {
                    auto reply = xml(soap(*s, "AddPinhole",
                                          {{"RemoteHost", ""},
                                           {"RemotePort", "0"},
                                           {"InternalClient", r.ipv6},
                                           {"InternalPort", std::to_string(r.internal_port)},
                                           {"Protocol", "6"},
                                           {"LeaseTime", std::to_string(r.lease_seconds)}}));
                    pinhole = number(find(reply, "UniqueID"), 65535);
                    mapping.pinhole_id = pinhole;
                    mapping.external_address = r.ipv6;
                    mapping.external_port = r.internal_port;
                } else {
                    auto reply = xml(soap(*s, "GetExternalIPAddress", {}));
                    auto ip = find(reply, "NewExternalIPAddress");
                    auto kind = classify_address(ip);
                    if (kind == AddressKind::private_v4 || kind == AddressKind::cgnat_v4) {
                        mapping.status = UpnpStatus::cgnat;
                        return;
                    }
                    if (kind != AddressKind::public_v4) {
                        mapping.status = UpnpStatus::unsupported;
                        return;
                    }
                    soap(*s, "AddPortMapping",
                         {{"NewRemoteHost", ""},
                          {"NewExternalPort", std::to_string(r.external_port)},
                          {"NewProtocol", "TCP"},
                          {"NewInternalPort", std::to_string(r.internal_port)},
                          {"NewInternalClient", r.ipv4},
                          {"NewEnabled", "1"},
                          {"NewPortMappingDescription", "Lantern"},
                          {"NewLeaseDuration", std::to_string(r.lease_seconds)}});
                    mapping.external_address = ip;
                    mapping.external_port = r.external_port;
                }
                mapping.status = UpnpStatus::success;
                mapping.lease_seconds = r.lease_seconds;
                mapping.expires_at = pairing_unix_time() + r.lease_seconds;
            } catch (const RouterError& e) {
                mapping.status = e.status;
            }
        };
        if (!r.ipv4.empty())
            perform(false);
        if (!r.ipv6.empty())
            perform(true);
        return report;
    }
    UpnpReport renew() {
        if (report.ipv4.status == UpnpStatus::success) {
            auto* s = service(false);
            try {
                soap(*s, "AddPortMapping",
                     {{"NewRemoteHost", ""},
                      {"NewExternalPort", std::to_string(request.external_port)},
                      {"NewProtocol", "TCP"},
                      {"NewInternalPort", std::to_string(request.internal_port)},
                      {"NewInternalClient", request.ipv4},
                      {"NewEnabled", "1"},
                      {"NewPortMappingDescription", "Lantern"},
                      {"NewLeaseDuration", std::to_string(request.lease_seconds)}});
                report.ipv4.expires_at = pairing_unix_time() + request.lease_seconds;
            } catch (const RouterError& e) {
                report.ipv4.status = e.status;
            }
        }
        if (report.ipv6.status == UpnpStatus::success && pinhole) {
            auto* s = service(true);
            try {
                soap(*s, "UpdatePinhole",
                     {{"UniqueID", std::to_string(*pinhole)},
                      {"NewLeaseTime", std::to_string(request.lease_seconds)}});
                report.ipv6.expires_at = pairing_unix_time() + request.lease_seconds;
            } catch (const RouterError& e) {
                report.ipv6.status = e.status;
            }
        }
        return report;
    }
    UpnpReport unmap() {
        // Ownership is tracked even after renewal failure; never delete somebody else's mapping.
        if (report.ipv4.external_port) {
            try {
                soap(*service(false), "DeletePortMapping",
                     {{"NewRemoteHost", ""},
                      {"NewExternalPort", std::to_string(report.ipv4.external_port)},
                      {"NewProtocol", "TCP"}});
                report.ipv4 = {};
            } catch (const RouterError& e) {
                report.ipv4.status = e.status;
            }
        }
        if (pinhole) {
            try {
                soap(*service(true), "DeletePinhole", {{"UniqueID", std::to_string(*pinhole)}});
                pinhole.reset();
                report.ipv6 = {};
            } catch (const RouterError& e) {
                report.ipv6.status = e.status;
            }
        }
        return report;
    }
};
UpnpIgd::UpnpIgd(SsdpTarget t) : p_(std::make_unique<Impl>(std::move(t))) {}
UpnpIgd::~UpnpIgd() = default;
UpnpStatus UpnpIgd::discover() {
    return p_->discover();
}
UpnpReport UpnpIgd::map(const UpnpRequest& r) {
    return p_->map(r);
}
UpnpReport UpnpIgd::renew() {
    return p_->renew();
}
UpnpReport UpnpIgd::unmap() {
    return p_->unmap();
}
UpnpReport UpnpIgd::status() const {
    return p_->report;
}
UpnpReport UpnpIgd::remove(uint16_t port, std::optional<uint32_t> pinhole) {
    auto result = p_->found.empty() ? p_->discover() : UpnpStatus::success;
    if (result != UpnpStatus::success) {
        UpnpReport r;
        r.ipv4.status = r.ipv6.status = result;
        return r;
    }
    if (port && p_->service(false))
        p_->report.ipv4.external_port = port;
    if (pinhole && *pinhole <= 65535 && p_->service(true))
        p_->pinhole = pinhole;
    return p_->unmap();
}
struct UpnpLease::Impl {
    mutable std::mutex mutex;
    std::once_flag stop_once;
    std::condition_variable cv;
    UpnpIgd igd;
    bool stopped = false, mapped = false;
    uint32_t interval = 900;
    std::thread worker;
    explicit Impl(SsdpTarget t) : igd(std::move(t)), worker([this] { run(); }) {}
    void run() {
        std::unique_lock lock(mutex);
        while (!stopped) {
            if (!mapped) {
                cv.wait(lock, [&] { return stopped || mapped; });
                continue;
            }
            if (cv.wait_for(lock, std::chrono::seconds(interval)) == std::cv_status::timeout && !stopped &&
                mapped)
                igd.renew();
        }
    }
};
UpnpLease::UpnpLease(SsdpTarget t) : p_(std::make_unique<Impl>(std::move(t))) {}
UpnpLease::~UpnpLease() {
    stop();
}
UpnpReport UpnpLease::map(const UpnpRequest& r) {
    std::lock_guard lock(p_->mutex);
    if (p_->stopped)
        throw Error(Code::cancelled, "Host stopped");
    if (p_->mapped)
        p_->igd.unmap();
    auto report = p_->igd.map(r);
    p_->mapped = report.ipv4.status == UpnpStatus::success || report.ipv6.status == UpnpStatus::success;
    p_->interval = std::max(1u, r.lease_seconds / 2);
    p_->cv.notify_all();
    return report;
}
UpnpReport UpnpLease::unmap() {
    std::lock_guard lock(p_->mutex);
    p_->mapped = false;
    p_->cv.notify_all();
    return p_->igd.unmap();
}
UpnpReport UpnpLease::status() const {
    std::lock_guard lock(p_->mutex);
    auto r = p_->igd.status();
    auto now = pairing_unix_time();
    for (auto* m : {&r.ipv4, &r.ipv6})
        if (m->status == UpnpStatus::success && m->expires_at <= now)
            m->status = UpnpStatus::expired;
    return r;
}
void UpnpLease::stop() {
    if (!p_)
        return;
    std::call_once(p_->stop_once, [&] {
        {
            std::lock_guard lock(p_->mutex);
            p_->stopped = true;
            p_->cv.notify_all();
        }
        if (p_->worker.joinable())
            p_->worker.join();
        std::lock_guard lock(p_->mutex);
        p_->mapped = false;
        p_->igd.unmap();
    });
}
} // namespace lantern
