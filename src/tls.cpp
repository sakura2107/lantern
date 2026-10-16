#include "lantern/win.hpp"
#include <schannel.h>
#include <security.h>
#include <wincrypt.h>
#include <ws2tcpip.h>
namespace lantern {
static void security_check(SECURITY_STATUS s, const char* where) {
    if (s != SEC_E_OK)
        throw Error(Code::io, std::string(where) + " status=" + std::to_string(uint32_t(s)));
}
Identity::Identity(const fs::path& p, bool create) {
    if (fs::exists(p)) {
        auto blob = unprotect(read_file(p));
        CRYPT_DATA_BLOB b{DWORD(blob.size()), blob.data()};
        store = PFXImportCertStore(&b, L"", CRYPT_USER_KEYSET | PKCS12_ALLOW_OVERWRITE_KEY);
        if (!store)
            win_error("Load identity");
        cert = CertEnumCertificatesInStore((HCERTSTORE)store, nullptr);
        if (!cert)
            win_error("Identity certificate missing");
    } else {
        if (!create)
            throw Error(Code::not_found, "Identity not initialized");
        HCRYPTPROV prov = 0;
        auto container = L"Lantern-" + wide(random_token());
        if (!CryptAcquireContextW(&prov, container.c_str(), MS_ENH_RSA_AES_PROV_W, PROV_RSA_AES,
                                  CRYPT_NEWKEYSET))
            win_error("Identity key store");
        HCRYPTKEY key = 0;
        if (!CryptGenKey(prov, AT_KEYEXCHANGE, (2048u << 16) | CRYPT_EXPORTABLE, &key)) {
            CryptReleaseContext(prov, 0);
            win_error("RSA key generation");
        }
        CryptDestroyKey(key);
        DWORD len = 0;
        CertStrToNameW(X509_ASN_ENCODING, L"CN=Lantern Local Device", CERT_X500_NAME_STR, nullptr, nullptr,
                       &len, nullptr);
        Bytes name(len);
        CertStrToNameW(X509_ASN_ENCODING, L"CN=Lantern Local Device", CERT_X500_NAME_STR, nullptr,
                       name.data(), &len, nullptr);
        CERT_NAME_BLOB nb{len, name.data()};
        CRYPT_KEY_PROV_INFO info{};
        info.pwszContainerName = container.data();
        info.pwszProvName = const_cast<wchar_t*>(MS_ENH_RSA_AES_PROV_W);
        info.dwProvType = PROV_RSA_AES;
        info.dwKeySpec = AT_KEYEXCHANGE;
        CRYPT_ALGORITHM_IDENTIFIER alg{};
        alg.pszObjId = const_cast<char*>(szOID_RSA_SHA256RSA);
        SYSTEMTIME end{};
        GetSystemTime(&end);
        end.wYear += 5;
        cert = CertCreateSelfSignCertificate(prov, &nb, 0, &info, &alg, nullptr, &end, nullptr);
        CryptReleaseContext(prov, 0);
        if (!cert)
            win_error("Self-signed certificate");
        store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
        if (!store)
            win_error("Certificate store");
        if (!CertAddCertificateContextToStore((HCERTSTORE)store, (PCCERT_CONTEXT)cert, CERT_STORE_ADD_ALWAYS,
                                              nullptr))
            win_error("Store certificate");
        CRYPT_DATA_BLOB b{};
        if (!PFXExportCertStoreEx((HCERTSTORE)store, &b, L"", nullptr,
                                  EXPORT_PRIVATE_KEYS | REPORT_NO_PRIVATE_KEY |
                                      REPORT_NOT_ABLE_TO_EXPORT_PRIVATE_KEY))
            win_error("Export identity size");
        Bytes raw(b.cbData);
        b.pbData = raw.data();
        if (!PFXExportCertStoreEx((HCERTSTORE)store, &b, L"", nullptr, EXPORT_PRIVATE_KEYS))
            win_error("Export identity");
        write_file(p, protect(raw));
    }
}
Identity::~Identity() {
    if (cert)
        CertFreeCertificateContext((PCCERT_CONTEXT)cert);
    if (store)
        CertCloseStore((HCERTSTORE)store, 0);
}
std::string Identity::fingerprint() const {
    auto c = (PCCERT_CONTEXT)cert;
    return sha256({c->pbCertEncoded, c->cbCertEncoded});
}
void init_sockets() {
    static const bool once = []() {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w))
            throw Error(Code::io, "WSAStartup");
        return true;
    }();
    (void)once;
}
static void configure_socket(SOCKET s) {
    DWORD timeout = 15000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof(timeout));
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
}
SOCKET connect_socket(const std::string& host, uint16_t port) {
    init_sockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addresses))
        throw TransportError("DNS lookup failed");
    SOCKET s = INVALID_SOCKET;
    int last_error = 0;
    for (auto a = addresses; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET)
            continue;
        configure_socket(s);
        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);
        int connected = ::connect(s, a->ai_addr, int(a->ai_addrlen));
        if (connected != 0)
            last_error = WSAGetLastError();
        if (connected != 0 && last_error == WSAEWOULDBLOCK) {
            fd_set write_set, error_set;
            FD_ZERO(&write_set);
            FD_ZERO(&error_set);
            FD_SET(s, &write_set);
            FD_SET(s, &error_set);
            timeval wait{5, 0};
            if (select(0, nullptr, &write_set, &error_set, &wait) > 0 &&
                (FD_ISSET(s, &write_set) || FD_ISSET(s, &error_set))) {
                int error = 0, size = sizeof(error);
                if (!getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&error, &size)) {
                    last_error = error;
                    if (!error)
                        connected = 0;
                }
            }
        }
        nonblocking = 0;
        ioctlsocket(s, FIONBIO, &nonblocking);
        if (connected == 0)
            break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(addresses);
    if (s == INVALID_SOCKET)
        throw TransportError("Connection failed (Windows " + std::to_string(last_error) + ")", last_error);
    return s;
}
SOCKET listen_socket(const std::string& bind, uint16_t port) {
    init_sockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICHOST | AI_PASSIVE;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(bind.c_str(), std::to_string(port).c_str(), &hints, &addresses))
        throw Error(Code::invalid, "Bind requires an IPv4 or IPv6 numeric address");
    SOCKET selected = INVALID_SOCKET;
    for (auto item = addresses; item; item = item->ai_next) {
        SOCKET candidate = socket(item->ai_family, SOCK_STREAM, IPPROTO_TCP);
        if (candidate == INVALID_SOCKET)
            continue;
        int exclusive = 1;
        setsockopt(candidate, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&exclusive, sizeof(exclusive));
        if (item->ai_family == AF_INET6) {
            DWORD only = bind == "::" ? 0 : 1;
            if (setsockopt(candidate, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&only, sizeof(only))) {
                closesocket(candidate);
                continue;
            }
        }
        if (!::bind(candidate, item->ai_addr, int(item->ai_addrlen)) && !listen(candidate, 12)) {
            selected = candidate;
            break;
        }
        closesocket(candidate);
    }
    freeaddrinfo(addresses);
    if (selected == INVALID_SOCKET)
        throw Error(Code::io, "Port unavailable on selected address");
    return selected;
}

static void send_all(SOCKET s, std::span<const uint8_t> b) {
    size_t off = 0;
    while (off < b.size()) {
        int n = ::send(s, (const char*)b.data() + off, int(b.size() - off), 0);
        if (n <= 0)
            throw TransportError("Socket send failed");
        off += n;
    }
}
struct Tls::Impl {
    std::function<void(bool)> before_close;
    bool timed_out = false;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max();
    void time_left() {
        if (deadline == std::chrono::steady_clock::time_point::max())
            return;
        auto n =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
                .count();
        if (n <= 0) {
            timed_out = true;
            throw TransportError("Connection deadline exceeded", WSAETIMEDOUT);
        }
        DWORD ms = DWORD(std::min<int64_t>(15000, n));
        if (!ms)
            ms = 1;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&ms), sizeof(ms));
        setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<char*>(&ms), sizeof(ms));
    }
    std::function<void(size_t)> send_gate;
    SOCKET socket = INVALID_SOCKET;
    CredHandle cred{};
    CtxtHandle context{};
    bool has_cred = false, has_context = false;
    SecPkgContext_StreamSizes sizes{};
    Bytes encrypted, plain;
    size_t plain_at = 0;
    ~Impl() {
        if (has_context)
            DeleteSecurityContext(&context);
        if (has_cred)
            FreeCredentialsHandle(&cred);
        if (socket != INVALID_SOCKET) {
            if (before_close)
                before_close(timed_out);
            shutdown(socket, SD_BOTH);
            closesocket(socket);
        }
    }
    void pull() {
        time_left();
        uint8_t buf[16384];
        int n = recv(socket, (char*)buf, sizeof(buf), 0);
        if (n <= 0) {
            auto error = n < 0 ? WSAGetLastError() : 0;
            timed_out = error == WSAETIMEDOUT;
            throw TransportError("Connection closed or timed out", error);
        }
        if (encrypted.size() + n > 128 * 1024)
            throw Error(Code::invalid, "TLS input bound exceeded");
        encrypted.insert(encrypted.end(), buf, buf + n);
    }
    void handshake(Identity* identity, const std::string& pin) {
        const bool server = identity != nullptr;
        SCHANNEL_CRED sc{};
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        sc.grbitEnabledProtocols = server ? SP_PROT_TLS1_2_SERVER : SP_PROT_TLS1_2_CLIENT;
        PCCERT_CONTEXT c = server ? (PCCERT_CONTEXT)identity->cert : nullptr;
        if (server) {
            sc.cCreds = 1;
            sc.paCred = &c;
        } else
            sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS;
        TimeStamp expiry{};
        security_check(AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
                                                 server ? SECPKG_CRED_INBOUND : SECPKG_CRED_OUTBOUND, nullptr,
                                                 &sc, nullptr, nullptr, &cred, &expiry),
                       "TLS credentials");
        has_cred = true;
        if (!server && !valid_sha(pin))
            throw Error(Code::invalid, "Required SHA256 certificate pin missing");
        bool first = true;
        for (;;) {
            if (server && encrypted.empty())
                pull();
            SecBuffer ib[2] = {{ULONG(encrypted.size()), SECBUFFER_TOKEN, encrypted.data()},
                               {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc in{SECBUFFER_VERSION, 2, ib};
            SecBuffer ob{0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc out{SECBUFFER_VERSION, 1, &ob};
            ULONG flags = 0;
            SECURITY_STATUS status;
            if (server)
                status =
                    AcceptSecurityContext(&cred, first ? nullptr : &context, &in,
                                          ASC_REQ_STREAM | ASC_REQ_CONFIDENTIALITY | ASC_REQ_REPLAY_DETECT |
                                              ASC_REQ_SEQUENCE_DETECT | ASC_REQ_ALLOCATE_MEMORY,
                                          SECURITY_NATIVE_DREP, &context, &out, &flags, &expiry);
            else
                status = InitializeSecurityContextW(
                    &cred, first ? nullptr : &context, const_cast<wchar_t*>(L"Lantern"),
                    ISC_REQ_STREAM | ISC_REQ_CONFIDENTIALITY | ISC_REQ_REPLAY_DETECT |
                        ISC_REQ_SEQUENCE_DETECT | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_MANUAL_CRED_VALIDATION,
                    0, SECURITY_NATIVE_DREP, first ? nullptr : &in, 0, &context, &out, &flags, &expiry);
            if (ob.pvBuffer) {
                if (ob.cbBuffer) {
                    try {
                        send_all(socket, {(uint8_t*)ob.pvBuffer, ob.cbBuffer});
                    } catch (...) {
                        FreeContextBuffer(ob.pvBuffer);
                        throw;
                    }
                }
                FreeContextBuffer(ob.pvBuffer);
            }
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                pull();
                continue;
            }
            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
                security_check(status, "TLS handshake");
            has_context = true;
            if (ib[1].BufferType == SECBUFFER_EXTRA) {
                auto n = ib[1].cbBuffer;
                encrypted = Bytes(encrypted.end() - n, encrypted.end());
            } else
                encrypted.clear();
            first = false;
            if (status == SEC_E_OK)
                break;
            if (encrypted.empty())
                pull();
        }
        security_check(QueryContextAttributesW(&context, SECPKG_ATTR_STREAM_SIZES, &sizes),
                       "TLS stream sizes");
        if (!server) {
            PCCERT_CONTEXT remote = nullptr;
            security_check(QueryContextAttributesW(&context, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote),
                           "Peer certificate");
            auto actual = sha256({remote->pbCertEncoded, remote->cbCertEncoded});
            bool valid = CertVerifyTimeValidity(nullptr, remote->pCertInfo) == 0;
            CertFreeCertificateContext(remote);
            if (actual != pin || !valid)
                throw Error(Code::unauthorized, "Host certificate pin or validity mismatch");
        }
    }
    void write(std::span<const uint8_t> data) {
        for (size_t at = 0; at < data.size();) {
            time_left();
            size_t n = std::min<size_t>(sizes.cbMaximumMessage, data.size() - at);
            Bytes record(sizes.cbHeader + n + sizes.cbTrailer);
            std::copy_n(data.data() + at, n, record.data() + sizes.cbHeader);
            SecBuffer b[4] = {{sizes.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
                              {ULONG(n), SECBUFFER_DATA, record.data() + sizes.cbHeader},
                              {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes.cbHeader + n},
                              {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            security_check(EncryptMessage(&context, 0, &d, 0), "TLS encrypt");
            size_t wire_size = size_t(b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer);
            if (send_gate)
                send_gate(wire_size);
            send_all(socket, {record.data(), wire_size});
            at += n;
        }
    }
    uint8_t byte() {
        while (plain_at >= plain.size()) {
            plain.clear();
            plain_at = 0;
            if (encrypted.empty())
                pull();
            SecBuffer b[4] = {{ULONG(encrypted.size()), SECBUFFER_DATA, encrypted.data()},
                              {0, SECBUFFER_EMPTY, nullptr},
                              {0, SECBUFFER_EMPTY, nullptr},
                              {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d{SECBUFFER_VERSION, 4, b};
            auto s = DecryptMessage(&context, &d, 0, nullptr);
            if (s == SEC_E_INCOMPLETE_MESSAGE) {
                pull();
                continue;
            }
            security_check(s, "TLS decrypt");
            ULONG extra = 0;
            for (auto& v : b) {
                if (v.BufferType == SECBUFFER_DATA)
                    plain.assign((uint8_t*)v.pvBuffer, (uint8_t*)v.pvBuffer + v.cbBuffer);
                if (v.BufferType == SECBUFFER_EXTRA)
                    extra = v.cbBuffer;
            }
            if (extra)
                encrypted = Bytes(encrypted.end() - extra, encrypted.end());
            else
                encrypted.clear();
        }
        return plain[plain_at++];
    }
};
Tls::Tls(SOCKET s, Identity* i, const std::string& pin, std::chrono::steady_clock::time_point deadline,
         std::function<void(bool)> before_close)
    : p_(std::make_unique<Impl>()) {
    p_->socket = s;
    p_->deadline = deadline;
    p_->before_close = std::move(before_close);
    configure_socket(s);
    p_->handshake(i, pin);
}
void Tls::clear_deadline() noexcept {
    p_->deadline = std::chrono::steady_clock::time_point::max();
    configure_socket(p_->socket);
}
Tls::~Tls() = default;
void Tls::set_send_gate(std::function<void(size_t)> gate) {
    p_->send_gate = std::move(gate);
}
void Tls::interrupt() noexcept {
    shutdown(p_->socket, SD_BOTH);
}
void Tls::send(std::span<const uint8_t> b) {
    if (b.size() > max_frame)
        throw Error(Code::invalid, "Frame too big");
    Writer w;
    w.b.reserve(p_->sizes.cbMaximumMessage);
    w.u32(uint32_t(b.size()));
    // Put the length and initial payload in one record; a four-byte-only record adds avoidable packets.
    auto initial = std::min<size_t>(b.size(), p_->sizes.cbMaximumMessage - 4);
    w.b.insert(w.b.end(), b.begin(), b.begin() + initial);
    p_->write(w.b);
    if (initial < b.size())
        p_->write(b.subspan(initial));
}
Bytes Tls::receive(uint32_t limit) {
    uint32_t n = 0;
    for (int i = 0; i < 4; ++i)
        n = (n << 8) | p_->byte();
    if (n > std::min(max_frame, limit) || n < 4)
        throw Error(Code::invalid, "Invalid frame length");
    Bytes b(n);
    for (auto& c : b)
        c = p_->byte();
    return b;
}
} // namespace lantern
