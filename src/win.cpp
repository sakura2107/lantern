#include "lantern/win.hpp"
#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <shellapi.h>
#include <shlobj.h>
#include <sstream>
#include <cwctype>
#include <wincrypt.h>
namespace lantern {
uint64_t monotonic_us(){static const uint64_t frequency=[] {LARGE_INTEGER f;QueryPerformanceFrequency(&f);return uint64_t(f.QuadPart);}();LARGE_INTEGER now;QueryPerformanceCounter(&now);auto q=uint64_t(now.QuadPart);return q/frequency*1'000'000+(q%frequency)*1'000'000/frequency;}
[[noreturn]] void win_error(const std::string& s) {
    DWORD e = GetLastError();
    Code c = e == ERROR_ACCESS_DENIED                                 ? Code::unauthorized
             : e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? Code::not_found
             : e == ERROR_SHARING_VIOLATION || e == ERROR_FILE_EXISTS || e == ERROR_ALREADY_EXISTS
                 ? Code::conflict
                 : Code::io;
    throw Error(c, s + " (Windows " + std::to_string(e) + ")");
}
std::wstring wide(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0);
    if (!n)
        throw Error(Code::invalid, "Invalid UTF-8");
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), int(s.size()), r.data(), n);
    return r;
}
std::string utf8(const std::wstring& s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), nullptr, 0, nullptr,
                                nullptr);
    if (!n)
        throw Error(Code::invalid, "Invalid Unicode");
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), int(s.size()), r.data(), n, nullptr,
                        nullptr);
    return r;
}
std::string hex(std::span<const uint8_t> b) {
    const char* d = "0123456789abcdef";
    std::string r;
    for (auto c : b) {
        r += d[c >> 4];
        r += d[c & 15];
    }
    return r;
}
std::string random_token() {
    Bytes b(32);
    if (BCryptGenRandom(nullptr, b.data(), ULONG(b.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG))
        throw Error(Code::io, "Random generator failure");
    return hex(b);
}
struct Hash {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    Bytes object;
    Hash() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
            throw Error(Code::io, "SHA provider");
        DWORD n = 0, got = 0;
        BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&n, sizeof(n), &got, 0);
        object.resize(n);
        if (BCryptCreateHash(alg, &h, object.data(), n, nullptr, 0, 0))
            throw Error(Code::io, "SHA create");
    }
    ~Hash() {
        if (h)
            BCryptDestroyHash(h);
        if (alg)
            BCryptCloseAlgorithmProvider(alg, 0);
    }
    void add(std::span<const uint8_t> b) {
        if (BCryptHashData(h, const_cast<PUCHAR>(b.data()), ULONG(b.size()), 0))
            throw Error(Code::io, "SHA update");
    }
    std::string finish() {
        Bytes b(32);
        if (BCryptFinishHash(h, b.data(), 32, 0))
            throw Error(Code::io, "SHA finish");
        return hex(b);
    }
};
std::string sha256(std::span<const uint8_t> b) {
    Hash h;
    h.add(b);
    return h.finish();
}
std::string hash_file(HANDLE f) {
    Cancel cancel;return hash_file(f,cancel,{});
}
std::string hash_file(HANDLE f,Cancel& cancel,std::function<void(uint64_t,uint64_t)> progress) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(f, zero, nullptr, FILE_BEGIN))
        win_error("Seek hash");
    Hash h;
    Bytes b(1024 * 1024);
    DWORD n;
    LARGE_INTEGER size{};if(!GetFileSizeEx(f,&size))win_error("Hash size");uint64_t done=0;
    if(progress)progress(0,uint64_t(size.QuadPart));
    while (true) {
        cancel.check();
        if (!ReadFile(f, b.data(), DWORD(b.size()), &n, nullptr))
            win_error("Read hash");
        if (!n)
            break;
        h.add({b.data(), n});
        done+=n;if(progress)progress(done,uint64_t(size.QuadPart));
    }
    return h.finish();
}
std::string hash_file(const fs::path& p) {
    Handle h(CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (h.h == INVALID_HANDLE_VALUE)
        win_error("Open hash");
    return hash_file(h);
}
Bytes read_file(const fs::path& p, size_t max) {
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw Error(Code::not_found, "Read file failed");
    f.seekg(0, std::ios::end);
    auto n = f.tellg();
    if (n < 0 || uint64_t(n) > max)
        throw Error(Code::invalid, "File exceeds bound");
    Bytes b(static_cast<size_t>(n));
    f.seekg(0);
    if (!b.empty() && !f.read((char*)b.data(), b.size()))
        throw Error(Code::io, "Short read");
    return b;
}
void write_file(const fs::path& p, std::span<const uint8_t> b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f || !f.write((const char*)b.data(), b.size()) || !f.flush())
        throw Error(Code::io, "Write file failed");
}
Bytes protect(std::span<const uint8_t> b) {
    DATA_BLOB in{DWORD(b.size()), const_cast<BYTE*>(b.data())}, out{};
    if (!CryptProtectData(&in, L"Lantern", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        win_error("DPAPI protect");
    Bytes r(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return r;
}
Bytes unprotect(std::span<const uint8_t> b) {
    DATA_BLOB in{DWORD(b.size()), const_cast<BYTE*>(b.data())}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
        win_error("DPAPI decrypt");
    Bytes r(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return r;
}
void save_config(const fs::path& p, const Config& c) {
    if(!valid_sha(c.token)||(c.scopes&~31u)||c.devices.size()>64)throw Error(Code::invalid,"Invalid trust configuration");
    std::vector<std::string> ids,tokens{c.token};
    for(auto& d:c.devices){
        if(!valid_sha(d.id)||!valid_sha(d.token)||(d.scopes&~31u)||d.label.empty()||d.label.size()>128||
           std::any_of(d.label.begin(),d.label.end(),[](unsigned char ch){return ch<32||ch==127;})||
           std::find(ids.begin(),ids.end(),d.id)!=ids.end()||std::find(tokens.begin(),tokens.end(),d.token)!=tokens.end())throw Error(Code::invalid,"Invalid or duplicated device trust");
        (void)wide(d.label);ids.push_back(d.id);tokens.push_back(d.token);
    }
    if (c.invites.size() > 32)
        throw Error(Code::invalid, "Invitation count bound");
    std::vector<std::string> invite_ids;
    for (auto& i : c.invites) {
        if (i.id.size() != 32 || !valid_sha(i.id + i.id) || !valid_sha(i.code_hash) ||
            !valid_pairing_label(i.label) || !i.scopes || (i.scopes & ~31u) || !i.expires_at ||
            i.expires_at > 4102444800ULL || i.failed_attempts >= 5 ||
            std::find(invite_ids.begin(), invite_ids.end(), i.id) != invite_ids.end())
            throw Error(Code::invalid, "Invalid invitation configuration");
        invite_ids.push_back(i.id);
    }
    Writer w;
    w.u16(3);
    w.str(utf8(fs::absolute(c.root).wstring()));
    w.str(c.token);
    w.u32(c.scopes);
    w.u32(c.legacy_enabled?1:0);w.u64(c.legacy_epoch);w.u32(uint32_t(c.devices.size()));
    for(auto& d:c.devices){w.str(d.id);w.str(d.label);w.str(d.token);w.u32(d.scopes);w.u64(d.epoch);}
    w.u32(uint32_t(c.invites.size()));
    for (auto& i : c.invites) {
        w.str(i.id);
        w.str(i.label);
        w.str(i.code_hash);
        w.u32(i.scopes);
        w.u64(i.expires_at);
        w.u32(i.failed_attempts);
    }
    auto b = protect(w.b);
    auto temp = p;
    temp += L".new-" + wide(random_token().substr(0, 12));
    write_file(temp, b);
    if (!MoveFileExW(temp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        win_error("Commit configuration");
    }
}
Config load_config(const fs::path& p) {
    // Keep a stable snapshot while another local process atomically replaces the configuration.
    Handle file(CreateFileW(p.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(file.h==INVALID_HANDLE_VALUE)win_error("Open trust configuration");
    LARGE_INTEGER length{};if(!GetFileSizeEx(file,&length))win_error("Trust configuration size");
    if(length.QuadPart<=0||length.QuadPart>1024*1024)throw Error(Code::invalid,"Trust configuration size bound");
    Bytes cipher(size_t(length.QuadPart));size_t done=0;
    while(done<cipher.size()){DWORD got=0;if(!ReadFile(file,cipher.data()+done,DWORD(cipher.size()-done),&got,nullptr))win_error("Read trust configuration");if(!got)throw Error(Code::io,"Trust configuration truncated");done+=got;}
    auto b = unprotect(cipher);
    Reader r{b};
    auto version=r.u16();
    if (version != 1 && version != 2 && version != 3)
        throw Error(Code::unsupported, "Configuration version");
    Config c;
    c.root = wide(r.str());
    c.token = r.str(128);
    c.scopes = r.u32();
    if (version >= 2) {
        auto enabled=r.u32();c.legacy_epoch=r.u64();auto count=r.u32();if(enabled>1||count>64)throw Error(Code::invalid,"Trust configuration bound");c.legacy_enabled=enabled!=0;
        for(uint32_t i=0;i<count;i++){TrustedDevice d;d.id=r.str(64);d.label=r.str(128);d.token=r.str(64);d.scopes=r.u32();d.epoch=r.u64();
            if(!valid_sha(d.id)||!valid_sha(d.token)||(d.scopes&~31u)||d.label.empty()||std::any_of(d.label.begin(),d.label.end(),[](unsigned char ch){return ch<32||ch==127;})||
               d.token==c.token||std::any_of(c.devices.begin(),c.devices.end(),[&](auto& old){return old.id==d.id||old.token==d.token;}))throw Error(Code::invalid,"Device configuration corrupt");
            (void)wide(d.label);c.devices.push_back(std::move(d));
        }
    }
    if (version >= 3) {
        auto count = r.u32();
        if (count > 32)
            throw Error(Code::invalid, "Invitation count bound");
        for (uint32_t at = 0; at < count; ++at) {
            InviteRecord i;
            i.id = r.str(32);
            i.label = r.str(64);
            i.code_hash = r.str(64);
            i.scopes = r.u32();
            i.expires_at = r.u64();
            i.failed_attempts = r.u32();
            if (i.id.size() != 32 || !valid_sha(i.id + i.id) || !valid_sha(i.code_hash) ||
                !valid_pairing_label(i.label) || !i.scopes || (i.scopes & ~31u) || !i.expires_at ||
                i.expires_at > 4102444800ULL || i.failed_attempts >= 5 ||
                std::any_of(c.invites.begin(), c.invites.end(), [&](auto& old) { return old.id == i.id; }))
                throw Error(Code::invalid, "Invitation configuration corrupt");
            c.invites.push_back(std::move(i));
        }
    }
    r.end();
    if (!valid_sha(c.token) || (c.scopes & ~31u))
        throw Error(Code::invalid, "Configuration corrupt");
    return c;
}
std::optional<uint32_t> authorized_scopes(const Config& c,const std::string& token){
    auto equal=[&](const std::string& expected){if(expected.size()!=token.size())return false;uint8_t difference=0;for(size_t i=0;i<token.size();i++)difference|=uint8_t(token[i]^expected[i]);return difference==0;};
    std::optional<uint32_t> result;if(c.legacy_enabled&&equal(c.token))result=c.scopes;
    for(auto& d:c.devices)if(equal(d.token))result=d.scopes&c.scopes;return result;
}
std::optional<uint64_t> authorization_epoch(const Config& c,const std::string& token){
    auto equal=[&](const std::string& expected){if(expected.size()!=token.size())return false;uint8_t difference=0;for(size_t i=0;i<token.size();i++)difference|=uint8_t(token[i]^expected[i]);return difference==0;};
    std::optional<uint64_t> result;if(c.legacy_enabled&&equal(c.token))result=c.legacy_epoch;
    for(auto& d:c.devices)if(equal(d.token))result=d.epoch;return result;
}
void edit_config(const fs::path& path,const std::function<void(Config&)>& edit){
    auto name=fs::absolute(path).lexically_normal().wstring();std::transform(name.begin(),name.end(),name.begin(),[](wchar_t c){return wchar_t(towlower(c));});
    auto key=utf8(name);auto digest=sha256({reinterpret_cast<const uint8_t*>(key.data()),key.size()});
    Handle mutex(CreateMutexW(nullptr,FALSE,(L"Local\\LanternConfig-"+wide(digest)).c_str()));if(!mutex.h)win_error("Create configuration lock");
    auto wait=WaitForSingleObject(mutex,10000);if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)throw Error(Code::busy,"Configuration edit busy");
    struct Unlock{HANDLE handle;~Unlock(){ReleaseMutex(handle);}}unlock{mutex.h};auto current=load_config(path);edit(current);save_config(path,current);
}
void open_external(const fs::path& p, bool chooser, HWND owner) {
    if (!fs::is_regular_file(p))
        throw Error(Code::not_found, "Local file required");
    auto path = fs::absolute(p).wstring();
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    struct Cleanup {
        HRESULT h;
        ~Cleanup() {
            if (SUCCEEDED(h))
                CoUninitialize();
        }
    } cleanup{co};
    if (chooser) {
        OPENASINFO info{};
        info.pcszFile = path.c_str();
        info.oaifInFlags = OAIF_EXEC;
        if (FAILED(SHOpenWithDialog(owner, &info)))
            throw Error(Code::unsupported, "Windows open-with dialog unavailable or cancelled");
    } else if ((INT_PTR)ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL) <= 32)
        throw Error(Code::unsupported, "Default application launch failed");
}
} // namespace lantern
