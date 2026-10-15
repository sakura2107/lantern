#include "lantern/win.hpp"
#include <fstream>
#include <shellapi.h>
#include <sstream>
#include <thread>
namespace lantern {
Client::Client(const std::string& a, uint16_t port, const std::string& pin, const std::string& token,
               uint32_t scopes, uint32_t channel, uint32_t hello_flags)
    : scopes_(scopes) {
    tls_ = std::make_unique<Tls>(connect_socket(a, port), nullptr, pin);
    Writer w;
    w.str(token);
    w.u32(scopes);
    w.u32(channel == UINT32_MAX ? ((scopes & control) ? 0 : (scopes & view) ? 1 : (scopes & audio) ? 4 : 3) : channel);
    if (hello_flags) // only for hosts advertising session=end-v1; older hosts reject trailing fields
        w.u32(hello_flags);
    auto response = call(Op::hello, std::move(w));
    Reader r{response};
    if (r.u32() != scopes)
        throw Error(Code::unauthorized, "Granted scope differs");
    r.end();
}
Bytes Client::call(Op op, Writer payload) {
    send(op, std::move(payload));
    return receive(op);
}
void Client::interrupt() noexcept { tls_->interrupt(); }
void Client::send(Op op, Writer payload) {
    Writer w;
    w.u16(protocol_version);
    w.u16(uint16_t(op));
    w.b.insert(w.b.end(), payload.b.begin(), payload.b.end());
    tls_->send(w.b);
}
Bytes Client::receive(Op op) {
    auto b = tls_->receive();
    Reader r{b};
    if (r.u16() != protocol_version || r.u16() != uint16_t(op))
        throw Error(Code::invalid, "Response version or operation mismatch");
    auto code = Code(r.u32());
    if (code != Code::ok) {
        auto msg = r.str();
        r.end();
        throw Error(code, msg);
    }
    return Bytes(b.begin() + r.at, b.end());
}
std::vector<Entry> Client::list(const std::string& p, uint32_t offset, const std::string& filter,
                                std::string* generation) {
    Writer w;
    w.str(p);
    w.u32(offset);
    w.u32(256);
    w.str(filter);
    auto b = call(Op::list, std::move(w));
    Reader r{b};
    auto gen = r.str();
    if (generation)
        *generation = gen;
    auto n = r.u32();
    if (n > 256)
        throw Error(Code::invalid, "List bound");
    std::vector<Entry> entries;
    while (n--) {
        auto name = r.str();
        bool dir = r.u32() != 0;
        auto size = r.u64(), mtime = r.u64();
        entries.push_back({name, dir, size, mtime});
    }
    r.end();
    return entries;
}
FileInfo Client::stat(const std::string& p, bool hash, Cancel* cancellation) {
    if(hash) {
        Writer start;start.u32(1);start.u32(2);start.str(p);start.str("");
        auto answer=call(Op::file_job,std::move(start));Reader began{answer};auto id=began.str(64);began.end();
        for(;;) {
            if(cancellation && cancellation->requested) {Writer stop;stop.u32(1);stop.str(id);try{call(Op::file_job_cancel,std::move(stop));}catch(...){}cancellation->check();}
            Writer query;query.u32(1);query.str(id);auto result=call(Op::file_job_poll,std::move(query));Reader r{result};
            auto state=TaskState(r.u32());r.u64();r.u64();auto code=Code(r.u32());auto message=r.str();auto kind=r.u32();
            FileInfo info{r.u64(),r.str(),r.str()};r.end();
            if(kind!=2)throw Error(Code::invalid,"Hash task kind mismatch");
            if(state==TaskState::completed){if(!valid_sha(info.hash))throw Error(Code::integrity,"Hash task result invalid");return info;}
            if(state==TaskState::failed || state==TaskState::cancelled)throw Error(code,message);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    Writer w;
    w.str(p);
    auto b = call(Op::media_stat, std::move(w));
    Reader r{b};
    FileInfo f;
    f.size = r.u64();
    f.version = r.str();
    f.hash = r.str();
    r.end();
    if (hash && !valid_sha(f.hash))
        throw Error(Code::invalid, "Invalid source hash");
    return f;
}
Bytes Client::read(const std::string& p, const std::string& v, uint64_t off, uint32_t n) {
    Writer w;
    w.str(p);
    w.str(v);
    w.u64(off);
    w.u32(n);
    auto b = call(Op::read, std::move(w));
    Reader r{b};
    auto out = r.blob(chunk_size);
    r.end();
    transfer_bytes_+=out.size();
    return out;
}
void Client::download(const std::string& remote, const fs::path& local, Cancel& cancel,
                      std::function<void(uint64_t, uint64_t)> progress,FileInfo* verified) {
    cancel.check();
    auto f = stat(remote,true,&cancel);
    auto part = local;
    part += L".lantern-part";
    auto journal = local;
    journal += L".lantern-resume";
    Writer expected;
    expected.u16(1);
    expected.str(remote);
    expected.u64(f.size);
    expected.str(f.version);
    expected.str(f.hash);
    if (fs::exists(local))
        throw Error(Code::conflict, "Local destination exists");
    if (fs::exists(part)) {
        if (!fs::exists(journal) || read_file(journal) != expected.b)
            throw Error(Code::conflict, "Local resume belongs to a different source version");
    } else
        write_file(journal, expected.b);
    Handle output(CreateFileW(part.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (output.h == INVALID_HANDLE_VALUE)
        win_error("Open local partial");
    BY_HANDLE_FILE_INFORMATION inf{};
    if (!GetFileInformationByHandle(output, &inf))
        win_error("Partial information");
    if (inf.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw Error(Code::unauthorized, "Local partial reparse point denied");
    uint64_t offset = (uint64_t(inf.nFileSizeHigh) << 32) | inf.nFileSizeLow;
    if (offset > f.size)
        throw Error(Code::integrity, "Partial exceeds source");
    LARGE_INTEGER at;
    at.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(output, at, nullptr, FILE_BEGIN))
        win_error("Resume seek");
    if (progress)
        progress(offset, f.size);
    while (offset < f.size) {
        cancel.check();
        auto b = read(remote, f.version, offset, uint32_t(std::min<uint64_t>(chunk_size, f.size - offset)));
        if (b.empty())
            throw Error(Code::source_changed, "Unexpected remote EOF");
        DWORD wrote = 0;
        if (!WriteFile(output, b.data(), DWORD(b.size()), &wrote, nullptr) || wrote != b.size() ||
            !FlushFileBuffers(output))
            win_error("Write local partial");
        offset += wrote;
        if (progress)
            progress(offset, f.size);
    }
    cancel.check();
    if (hash_file(output) != f.hash)
        throw Error(Code::integrity, "Download SHA256 mismatch; partial retained");
    output = Handle();
    if (!MoveFileExW(part.c_str(), local.c_str(), MOVEFILE_WRITE_THROUGH))
        win_error("Commit local download");
    DeleteFileW(journal.c_str());
    if(verified)*verified=f;
}
void Client::upload(const fs::path& local, const std::string& remote, Cancel& cancel,
                    std::function<void(uint64_t, uint64_t)> progress) {
    cancel.check();
    Handle source(CreateFileW(local.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (source.h == INVALID_HANDLE_VALUE)
        win_error("Open local source");
    BY_HANDLE_FILE_INFORMATION i{};
    if (!GetFileInformationByHandle(source, &i))
        win_error("Source information");
    if (i.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))
        throw Error(Code::invalid, "Regular local file required");
    uint64_t size = (uint64_t(i.nFileSizeHigh) << 32) | i.nFileSizeLow;
    auto hash = hash_file(source,cancel,{});
    Writer begin;
    begin.str(remote);
    begin.u64(size);
    begin.str(hash);
    Bytes response;
    try {
        response = call(Op::upload_begin, std::move(begin));
    } catch (const Error& e) {
        if (e.code == Code::conflict && (scopes_ & files_read)) {
            auto existing = stat(remote);
            if (existing.size == size && existing.hash == hash) {
                if (progress)
                    progress(size, size);
                return;
            }
        }
        throw;
    }
    Reader r{response};
    auto id = r.str();
    auto offset = r.u64();
    r.end();
    if (offset > size || !valid_sha(id))
        throw Error(Code::invalid, "Invalid upload resume");
    LARGE_INTEGER at;
    at.QuadPart = LONGLONG(offset);
    if (!SetFilePointerEx(source, at, nullptr, FILE_BEGIN))
        win_error("Upload seek");
    Bytes buffer(chunk_size);
    if (progress)
        progress(offset, size);
    while (offset < size) {
        cancel.check();
        DWORD got = 0;
        if (!ReadFile(source, buffer.data(), DWORD(std::min<uint64_t>(buffer.size(), size - offset)), &got,
                      nullptr) ||
            !got)
            win_error("Upload source read");
        Writer w;
        w.str(id);
        w.u64(offset);
        w.blob({buffer.data(), got});
        auto answer = call(Op::upload_chunk, std::move(w));
        Reader ack{answer};
        auto next = ack.u64();
        ack.end();
        if (next != offset + got)
            throw Error(Code::invalid, "Upload offset acknowledgement");
        offset = next;
        transfer_bytes_+=got;
        if (progress)
            progress(offset, size);
    }
    cancel.check();
    Writer commit;
    commit.str(id);
    call(Op::upload_commit, std::move(commit));
}
void Client::mutate(Op op, const std::string& p, const std::string& to) {
    Writer w;
    w.str(p);
    w.str(to);
    call(op, std::move(w));
}
static void http_send(SOCKET s, const std::string& str) {
    size_t at = 0;
    while (at < str.size()) {
        int n = send(s, str.data() + at, int(str.size() - at), 0);
        if (n <= 0)
            throw Error(Code::io, "HTTP consumer closed");
        at += n;
    }
}
void media_bridge(const std::string& address, uint16_t port, const std::string& pin, const std::string& token,
                  const std::string& remote, uint16_t local_port, bool launch) {
    // Original bytes only; receiving application owns container, codec, subtitles and A/V clock.
    Client metadata(address, port, pin, token, files_read, 2);
    auto f = metadata.stat(remote, false);
    SOCKET listener = listen_socket("127.0.0.1", local_port);
    auto secret = random_token();
    std::string path = "/" + secret + "/media" + utf8(fs::path(wide(remote)).extension().wstring());
    auto url = "http://127.0.0.1:" + std::to_string(local_port) + path;
    if (launch) {
        auto result = ShellExecuteW(nullptr, L"open", wide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)result <= 32) {
            closesocket(listener);
            throw Error(Code::unsupported, "No URL application available");
        }
    }
    printf("Original-media URL (local, temporary): %s\nKeep this process running; Ctrl+C stops it.\n",
           url.c_str());
    fflush(stdout);
    auto worker = [&] {
        for (;;) {
            SOCKET socket = accept(listener, nullptr, nullptr);
            if (socket == INVALID_SOCKET)
                break;
            DWORD timeout = 10000;
            setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
            setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
            bool responded = false;
            try {
                std::string header;
                char buf[1024];
                while (header.find("\r\n\r\n") == std::string::npos) {
                    int n = recv(socket, buf, sizeof(buf), 0);
                    if (n <= 0 || header.size() + n > 8192)
                        throw Error(Code::invalid, "HTTP header bound");
                    header.append(buf, n);
                }
                bool head = header.starts_with("HEAD ");
                if (!head && !header.starts_with("GET "))
                    throw Error(Code::invalid, "HTTP method");
                auto first = header.find(' '), last = header.find(' ', first + 1);
                if (header.substr(first + 1, last - first - 1) != path) {
                    http_send(socket,
                              "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    closesocket(socket);
                    continue;
                }
                uint64_t start = 0, end = f.size ? f.size - 1 : 0;
                bool range = false;
                auto lower = header;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return char(std::tolower(c)); });
                auto where = lower.find("\r\nrange: bytes=");
                if (where != std::string::npos) {
                    range = true;
                    auto line = header.substr(where + 15);
                    line = line.substr(0, line.find("\r\n"));
                    try {
                        auto parsed = parse_range(line, f.size);
                        start = parsed.begin;
                        end = parsed.end;
                    } catch (const Error&) {
                        http_send(socket, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                                              std::to_string(f.size) +
                                              "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        closesocket(socket);
                        continue;
                    }
                }
                auto length = f.size ? end - start + 1 : 0;
                std::string response = range ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
                response += "Accept-Ranges: bytes\r\nContent-Type: application/octet-stream\r\nConnection: "
                            "close\r\nContent-Length: " +
                            std::to_string(length) + "\r\n";
                if (range)
                    response += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) +
                                "/" + std::to_string(f.size) + "\r\n";
                http_send(socket, response + "\r\n");
                responded = true;
                if (!head) {
                    Client stream(address, port, pin, token, files_read, 2);
                    for (uint64_t at = start; at < start + length;) {
                        auto data =
                            stream.read(remote, f.version, at,
                                        uint32_t(std::min<uint64_t>(chunk_size, start + length - at)));
                        if (data.empty())
                            throw Error(Code::source_changed, "Media EOF");
                        http_send(socket, std::string((char*)data.data(), data.size()));
                        at += data.size();
                    }
                }
            } catch (...) {
                if (!responded) {
                    try {
                        http_send(
                            socket,
                            "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    } catch (...) {
                    }
                }
            }
            closesocket(socket);
        }
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < 4; ++i)
        workers.emplace_back(worker);
    for (auto& t : workers)
        t.join();
    closesocket(listener);
}
} // namespace lantern
