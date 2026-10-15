#include "lantern/windows_media.hpp"
#include <array>
#include <cctype>
#include <ws2tcpip.h>
namespace lantern {
namespace {
void send_bytes(SOCKET socket, std::span<const uint8_t> b) {
    size_t at = 0;
    while (at < b.size()) {
        int sent = ::send(socket, (const char*)b.data() + at, int(b.size() - at), 0);
        if (sent <= 0)
            throw TransportError("Local player closed connection");
        at += sent;
    }
}
void send_text(SOCKET s, const std::string& text) {
    send_bytes(s, {(const uint8_t*)text.data(), text.size()});
}
} // namespace
struct WindowsMediaBridge::Impl {
    ClientCredentials credentials;
    std::string remote, path, address;
    FileInfo info;
    SOCKET listener = INVALID_SOCKET;
    std::array<SOCKET, 4> clients{INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET, INVALID_SOCKET};
    std::array<ActiveConnection, 4> connections;
    std::array<std::thread, 4> workers;
    std::mutex mutex;
    std::atomic_bool stopped = false;
    ~Impl() {
        stop();
    }
    void stop() {
        if (stopped.exchange(true))
            return;
        {
            std::lock_guard lock(mutex);
            for (auto socket : clients)
                if (socket != INVALID_SOCKET)
                    shutdown(socket, SD_BOTH);
        }
        for (auto& c : connections)
            c.interrupt();
        for (auto& w : workers)
            if (w.joinable())
                w.join();
        if (listener != INVALID_SOCKET) {
            closesocket(listener);
            listener = INVALID_SOCKET;
        }
    }
    void response(SOCKET socket, const char* status) {
        send_text(socket,
                  std::string("HTTP/1.1 ") + status +
                      "\r\nContent-Length: 0\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n");
    }
    void serve(SOCKET socket, unsigned worker) {
        bool responded = false;
        try {
            std::string header;
            char buffer[1024];
            while (header.find("\r\n\r\n") == std::string::npos) {
                int n = recv(socket, buffer, sizeof(buffer), 0);
                if (n <= 0 || header.size() + n > 8192)
                    throw Error(Code::invalid, "HTTP header bound");
                header.append(buffer, n);
            }
            auto line_end = header.find("\r\n"), first = header.find(' '), last = header.find(' ', first + 1);
            bool head = header.starts_with("HEAD ");
            if (!head && !header.starts_with("GET ")) {
                response(socket, "405 Method Not Allowed");
                return;
            }
            if (first == std::string::npos || last == std::string::npos ||
                header.substr(first + 1, last - first - 1) != path ||
                header.substr(last + 1, line_end - last - 1) != "HTTP/1.1") {
                response(socket, "404 Not Found");
                return;
            }
            auto lower = header;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](unsigned char c) { return char(std::tolower(c)); });
            if (lower.find("\r\norigin:") != std::string::npos) {
                response(socket, "403 Forbidden");
                return;
            }
            uint64_t start = 0, length = info.size, end = length ? length - 1 : 0;
            bool ranged = false;
            auto where = lower.find("\r\nrange:");
            if (where != std::string::npos) {
                ranged = true;
                auto value = header.substr(where + 8);
                value = value.substr(0, value.find("\r\n"));
                while (!value.empty() && value[0] == ' ')
                    value.erase(value.begin());
                try {
                    if (!value.starts_with("bytes="))
                        throw Error(Code::invalid, "Range unit");
                    auto parsed = parse_range(value.substr(6), info.size);
                    start = parsed.begin;
                    end = parsed.end;
                    length = end - start + 1;
                } catch (const Error&) {
                    send_text(
                        socket,
                        "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nContent-Range: bytes */" +
                            std::to_string(info.size) + "\r\nConnection: close\r\n\r\n");
                    return;
                }
            }
            auto ext = fs::path(wide(remote)).extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), towlower);
            std::string mime = ext == L".mp4"   ? "video/mp4"
                               : ext == L".mkv" ? "video/x-matroska"
                                                : "application/octet-stream";
            std::string out = ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
            out += "Content-Type: " + mime +
                   "\r\nAccept-Ranges: bytes\r\nCache-Control: no-store\r\nConnection: "
                   "close\r\nContent-Length: " +
                   std::to_string(length) + "\r\n";
            if (ranged)
                out += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                       std::to_string(info.size) + "\r\n";
            send_text(socket, out + "\r\n");
            responded = true;
            if (head)
                return;
            std::shared_ptr<Client> client;
            for (uint64_t at = start; at < start + length && !stopped;) {
                Bytes bytes;
                for (unsigned retry = 0;; ++retry) {
                    try {
                        if (!client) {
                            client = credentials.connect(files_read, 2);
                            connections[worker].set(client);
                            if (stopped)
                                client->interrupt();
                        }
                        bytes = client->read(remote, info.version, at,
                                             uint32_t(std::min<uint64_t>(chunk_size, start + length - at)));
                        break;
                    } catch (const TransportError&) {
                        client.reset();
                        connections[worker].set({});
                        if (stopped || retry >= 2)
                            throw;
                    }
                }
                if (bytes.empty())
                    throw Error(Code::source_changed, "Original file changed/EOF");
                send_bytes(socket, bytes);
                at += bytes.size();
            }
        } catch (...) {
            if (!responded)
                try {
                    response(socket, "400 Bad Request");
                } catch (...) {
                }
        }
        connections[worker].set({});
    }
    void run(unsigned worker) {
        while (!stopped) {
            SOCKET socket = accept(listener, nullptr, nullptr);
            if (socket == INVALID_SOCKET) {
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
                continue;
            }
            {
                std::lock_guard lock(mutex);
                clients[worker] = socket;
                if (stopped)
                    shutdown(socket, SD_BOTH);
            }
            DWORD timeout = 10000;
            // Accepted WinSock sockets inherit the nonblocking listener mode.
            u_long blocking = 0;
            ioctlsocket(socket, FIONBIO, &blocking);
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
            serve(socket, worker);
            connections[worker].set({});
            {
                std::lock_guard lock(mutex);
                clients[worker] = INVALID_SOCKET;
                closesocket(socket);
            }
        }
    }
};
WindowsMediaBridge::WindowsMediaBridge(ClientCredentials c, std::string remote)
    : p_(std::make_unique<Impl>()) {
    if (!safe_path(remote) || remote.empty())
        throw Error(Code::invalid, "Media path");
    auto& p = *p_;
    p.credentials = std::move(c);
    p.remote = std::move(remote);
    auto metadata = p.credentials.connect(files_read, 2);
    p.info = metadata->stat(p.remote, false);
    p.listener = listen_socket("127.0.0.1", 0);
    sockaddr_in address{};
    int length = sizeof(address);
    if (getsockname(p.listener, (sockaddr*)&address, &length))
        throw TransportError("Local media bind");
    u_long nonblocking = 1;
    ioctlsocket(p.listener, FIONBIO, &nonblocking);
    p.path = "/" + random_token() + "/media" + utf8(fs::path(wide(p.remote)).extension().wstring());
    p.address = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + p.path;
    for (unsigned i = 0; i < 4; ++i)
        p.workers[i] = std::thread([&p, i] { p.run(i); });
}
WindowsMediaBridge::~WindowsMediaBridge() = default;
std::string WindowsMediaBridge::url() const {
    return p_->address;
}
void WindowsMediaBridge::stop() {
    p_->stop();
}
} // namespace lantern
