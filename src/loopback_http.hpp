#pragma once
#include "lantern/win.hpp"
#include <ws2tcpip.h>

namespace lantern {
inline std::string loopback_authority(SOCKET listener) {
    sockaddr_in address{};
    int size = sizeof(address);
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &size))
        throw TransportError("Local media bind");
    return "127.0.0.1:" + std::to_string(ntohs(address.sin_port));
}

inline std::string read_loopback_header(SOCKET socket, std::string_view authority) {
    std::string header;
    char buffer[1024];
    size_t end;
    while ((end = header.find("\r\n\r\n")) == header.npos) {
        auto capacity = std::min(sizeof(buffer), 8192 - header.size());
        if (!capacity)
            throw Error(Code::invalid, "HTTP header bound");
        int n = recv(socket, buffer, int(capacity), 0);
        if (n <= 0)
            throw Error(Code::invalid, "Incomplete HTTP header");
        header.append(buffer, n);
    }
    header.resize(end + 4);
    std::string_view lines(header);
    bool request = true, seen_host = false;
    for (;;) {
        auto length = lines.find("\r\n");
        auto line = lines.substr(0, length);
        if (line.empty())
            break;
        for (unsigned char c : line)
            if ((c < 32 && c != '\t') || c == 127)
                throw Error(Code::invalid, "Invalid HTTP line");
        lines.remove_prefix(length + 2);
        if (request) {
            request = false;
            continue;
        }
        auto colon = line.find(':');
        auto name = line.substr(0, colon);
        constexpr std::string_view tokens = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'*+-.^_`|~";
        if (colon == line.npos || name.empty() || name.find_first_not_of(tokens) != name.npos)
            throw Error(Code::invalid, "Invalid HTTP field");
        if (name.size() == 4 && _strnicmp(name.data(), "Host", 4) == 0) {
            auto value = line.substr(colon + 1);
            auto first = value.find_first_not_of(" \t"), last = value.find_last_not_of(" \t");
            if (seen_host || first == value.npos || value.substr(first, last - first + 1) != authority)
                throw Error(Code::invalid, "Invalid HTTP Host");
            seen_host = true;
        }
    }
    if (!seen_host)
        throw Error(Code::invalid, "Missing HTTP Host");
    return header;
}
} // namespace lantern
