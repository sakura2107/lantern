#include "lantern/win.hpp"
#include <cwctype>
#include <fstream>
namespace lantern {
static BY_HANDLE_FILE_INFORMATION info(HANDLE h) {
    BY_HANDLE_FILE_INFORMATION i{};
    if (!GetFileInformationByHandle(h, &i))
        win_error("File information");
    if (i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw Error(Code::unauthorized, "Reparse points are denied");
    if (!(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && i.nNumberOfLinks > 1)
        throw Error(Code::unauthorized, "Hard-linked files are denied");
    return i;
}
static uint64_t file_size(const BY_HANDLE_FILE_INFORMATION& i) {
    return (uint64_t(i.nFileSizeHigh) << 32) | i.nFileSizeLow;
}
static std::string version(const BY_HANDLE_FILE_INFORMATION& i) {
    return std::to_string(i.dwVolumeSerialNumber) + ":" + std::to_string(i.nFileIndexHigh) + ":" +
           std::to_string(i.nFileIndexLow) + ":" + std::to_string(file_size(i)) + ":" +
           std::to_string(i.ftLastWriteTime.dwHighDateTime) + ":" +
           std::to_string(i.ftLastWriteTime.dwLowDateTime);
}
static Handle lock_path(const fs::path& p) {
    Handle h(CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (h.h == INVALID_HANDLE_VALUE)
        win_error("Open authorized path");
    info(h);
    return h;
}
FileService::FileService(fs::path root)
    : root_(fs::absolute(root).lexically_normal()), root_lock_(lock_path(root_)) {
    if (!(info(root_lock_).dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        throw Error(Code::invalid, "Authorized root must be a directory");
    auto private_dir = root_ / L".rdx-private";
    if (fs::exists(private_dir)) {
        auto lock = lock_path(private_dir);
    }
}
Guard FileService::guard(const std::string& p, bool missing) {
    if (!safe_path(p))
        throw Error(Code::unauthorized, "Unsafe relative path");
    auto w = wide(p);
    Guard g;
    g.path = root_;
    size_t at = 0;
    while (at < w.size()) {
        auto end = w.find(L'/', at);
        if (end == std::wstring::npos)
            end = w.size();
        g.path /= w.substr(at, end - at);
        bool last = end == w.size();
        DWORD attrs = GetFileAttributesW(g.path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            DWORD e = GetLastError();
            if (missing && last && (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND))
                return g;
            win_error("Authorized path missing");
        }
        auto h = lock_path(g.path);
        auto i = info(h);
        if (!last && !(i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            throw Error(Code::invalid, "Non-directory path component");
        g.locks.push_back(std::move(h));
        at = end + 1;
    }
    return g;
}
Handle FileService::open_read(Guard& g) {
    Handle h(CreateFileW(g.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (h.h == INVALID_HANDLE_VALUE)
        win_error("Open source");
    if (info(h).dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        throw Error(Code::invalid, "Expected regular file");
    return h;
}
std::vector<Entry> FileService::list(const std::string& p, uint32_t offset, uint32_t limit,
                                     const std::string& filter, std::string& gen) {
    if (limit == 0 || limit > 256 || offset > 200000 || filter.size() > 256)
        throw Error(Code::invalid, "Pagination bound");
    auto g = guard(p);
    Handle d = lock_path(g.path);
    auto before = info(d);
    if (!(before.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        throw Error(Code::invalid, "Expected directory");
    gen = version(before);
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW((g.path / L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            return {};
        win_error("List directory");
    }
    std::vector<Entry> result;
    uint32_t matched = 0, scanned = 0;
    auto needle = wide(filter);
    std::transform(needle.begin(), needle.end(), needle.begin(), std::towlower);
    do {
        if (++scanned > 200000) {
            FindClose(find);
            throw Error(Code::busy, "Directory exceeds scan bound; narrow the search");
        }
        std::wstring name = data.cFileName, lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), std::towlower);
        if (name == L"." || name == L".." || lower.starts_with(L".rdx-private") ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || protected_os_entry(data.dwFileAttributes) ||
            (!needle.empty() && lower.find(needle) == std::wstring::npos))
            continue;
        if (matched++ < offset)
            continue;
        result.push_back(
            {utf8(name), (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
             (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
             (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime});
        if (result.size() == limit)
            break;
    } while (FindNextFileW(find, &data));
    FindClose(find);
    if (version(info(d)) != gen)
        throw Error(Code::conflict, "Directory changed; refresh listing");
    return result;
}
FileInfo FileService::stat(const std::string& p, bool hash) {
    return inspect(p,hash,nullptr,{});
}
FileInfo FileService::hash_snapshot(const std::string& p,Cancel& cancel,std::function<void(uint64_t,uint64_t)> progress) {
    return inspect(p,true,&cancel,std::move(progress));
}
FileInfo FileService::inspect(const std::string& p,bool hash,Cancel* cancellation,std::function<void(uint64_t,uint64_t)> progress) {
    if(cancellation)cancellation->check();
    auto g = guard(p);
    auto h = open_read(g);
    auto before = info(h);
    FileInfo f{file_size(before), version(before), ""};
    if(hash) {
        {std::lock_guard lock(hash_mutex_);auto it=hash_cache_.find(f.version);if(it!=hash_cache_.end())f.hash=it->second;}
        if(f.hash.empty()) {Cancel local;f.hash=hash_file(h,cancellation?*cancellation:local,progress);}
        else if(progress)progress(f.size,f.size);
    }
    if (version(info(h)) != f.version)
        throw Error(Code::source_changed, "Source changed during hashing");
    if(hash){std::lock_guard lock(hash_mutex_);if(hash_cache_.size()>=256)hash_cache_.erase(hash_cache_.begin());hash_cache_[f.version]=f.hash;}
    if(cancellation)cancellation->check();
    return f;
}
Bytes FileService::read(const std::string& p, const std::string& v, uint64_t off, uint32_t n) {
    if (n > chunk_size)
        throw Error(Code::invalid, "Read chunk bound");
    auto g = guard(p);
    auto h = open_read(g);
    auto i = info(h);
    if (version(i) != v)
        throw Error(Code::source_changed, "Source changed; restart task");
    if (off > file_size(i))
        throw Error(Code::invalid, "Read offset");
    LARGE_INTEGER pos;
    pos.QuadPart = LONGLONG(off);
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
        win_error("Read seek");
    Bytes b(size_t(std::min<uint64_t>(n, file_size(i) - off)));
    DWORD got = 0;
    if (!b.empty() && !ReadFile(h, b.data(), DWORD(b.size()), &got, nullptr))
        win_error("Read source");
    b.resize(got);
    return b;
}
fs::path FileService::stage(const std::string& id) {
    if (!valid_sha(id))
        throw Error(Code::invalid, "Invalid transfer id");
    return root_ / L".rdx-private" / wide(id);
}
std::pair<std::string, uint64_t> FileService::begin(const std::string& p, uint64_t size,
                                                    const std::string& hash) {
    std::lock_guard lock(mutations_);
    auto private_dir = root_ / L".rdx-private";
    fs::create_directories(private_dir);
    auto private_lock = lock_path(private_dir);
    if (p.empty() || !valid_sha(hash) || size > uint64_t(INT64_MAX))
        throw Error(Code::invalid, "Invalid upload metadata");
    auto g = guard(p, true);
    if (fs::exists(g.path))
        throw Error(Code::conflict, "Destination exists");
    Writer metadata;
    metadata.u16(1);
    metadata.str(p);
    metadata.u64(size);
    metadata.str(hash);
    auto id = sha256(metadata.b);
    auto temp = stage(id);
    auto meta = temp;
    meta += L".meta";
    if (fs::exists(meta)) {
        if (read_file(meta) != metadata.b)
            throw Error(Code::conflict, "Resume manifest mismatch");
    } else
        write_file(meta, metadata.b);
    Handle h(CreateFileW(temp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (h.h == INVALID_HANDLE_VALUE)
        win_error("Create partial upload");
    auto i = info(h);
    if (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        throw Error(Code::invalid, "Partial is a directory");
    auto off = file_size(i);
    if (off > size)
        throw Error(Code::integrity, "Partial exceeds source size");
    return {id, off};
}
uint64_t FileService::chunk(const std::string& id, uint64_t off, std::span<const uint8_t> b) {
    std::lock_guard lock(mutations_);
    if (b.empty() || b.size() > chunk_size)
        throw Error(Code::invalid, "Upload chunk bound");
    auto temp = stage(id), meta = temp;
    meta += L".meta";
    auto data = read_file(meta);
    Reader r{data};
    if (r.u16() != 1)
        throw Error(Code::invalid, "Manifest version");
    auto path = r.str();
    auto total = r.u64();
    auto hash = r.str();
    r.end();
    auto g = guard(path, true);
    Handle h(CreateFileW(temp.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (h.h == INVALID_HANDLE_VALUE)
        win_error("Open partial upload");
    if (file_size(info(h)) != off || off > total || b.size() > total - off)
        throw Error(Code::conflict, "Resume offset differs");
    LARGE_INTEGER pos;
    pos.QuadPart = LONGLONG(off);
    if (!SetFilePointerEx(h, pos, nullptr, FILE_BEGIN))
        win_error("Upload seek");
    DWORD wrote = 0;
    if (!WriteFile(h, b.data(), DWORD(b.size()), &wrote, nullptr) || wrote != b.size() ||
        !FlushFileBuffers(h))
        win_error("Write or flush upload");
    return off + wrote;
}
void FileService::commit(const std::string& id) {
    std::lock_guard lock(mutations_);
    auto temp = stage(id), meta = temp;
    meta += L".meta";
    auto data = read_file(meta);
    Reader r{data};
    if (r.u16() != 1)
        throw Error(Code::invalid, "Manifest version");
    auto p = r.str();
    auto total = r.u64();
    auto hash = r.str();
    r.end();
    auto g = guard(p, true);
    auto partial = lock_path(temp);
    Handle h(CreateFileW(temp.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (h.h == INVALID_HANDLE_VALUE)
        win_error("Open partial for verification");
    if (file_size(info(h)) != total)
        throw Error(Code::conflict, "Upload incomplete");
    if (hash_file(h) != hash)
        throw Error(Code::integrity, "Upload SHA256 mismatch; partial retained");
    h = Handle();
    partial = Handle();
    if (fs::exists(g.path))
        throw Error(Code::conflict, "Destination appeared during upload");
    if (!MoveFileExW(temp.c_str(), g.path.c_str(), MOVEFILE_WRITE_THROUGH))
        win_error("Commit upload");
    DeleteFileW(meta.c_str());
}
void FileService::mutate(Op op, const std::string& p, const std::string& to) {
    std::lock_guard lock(mutations_);
    if (p.empty())
        throw Error(Code::unauthorized, "Cannot modify authorized root");
    auto from = guard(p, op == Op::mkdir);
    if (op == Op::mkdir) {
        if (!CreateDirectoryW(from.path.c_str(), nullptr))
            win_error("Create directory");
        return;
    }
    auto mutable_handle = [&]() {
        auto before = info(from.locks.back());
        from.locks.pop_back();
        Handle h(CreateFileW(from.path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (h.h == INVALID_HANDLE_VALUE)
            win_error("Open mutation handle");
        auto after = info(h);
        if (before.dwVolumeSerialNumber != after.dwVolumeSerialNumber ||
            before.nFileIndexHigh != after.nFileIndexHigh || before.nFileIndexLow != after.nFileIndexLow)
            throw Error(Code::conflict, "Source path replaced during mutation");
        return h;
    };
    if (op == Op::remove) {
        auto h = mutable_handle();
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(h, FileDispositionInfo, &disposition, sizeof(disposition)))
            win_error("Delete (directories must be empty)");
        return;
    }
    if (to.empty())
        throw Error(Code::invalid, "Destination required");
    auto dest = guard(to, true);
    if (fs::exists(dest.path))
        throw Error(Code::conflict, "Destination exists");
    if (op == Op::rename) {
        auto h = mutable_handle();
        auto name = dest.path.wstring();
        Bytes bytes(offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t));
        auto rename = (FILE_RENAME_INFO*)bytes.data();
        rename->ReplaceIfExists = FALSE;
        rename->RootDirectory = nullptr;
        rename->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
        std::copy(name.begin(), name.end(), rename->FileName);
        if (!SetFileInformationByHandle(h, FileRenameInfo, rename, DWORD(bytes.size())))
            win_error("Move within host by handle");
    } else if (op == Op::copy) {
        auto src = open_read(from);
        auto dir = dest.path.parent_path();
        auto temporary = dir / (L".rdx-private-copy-" + wide(random_token()));
        Handle out(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                               FILE_ATTRIBUTE_NORMAL, nullptr));
        if (out.h == INVALID_HANDLE_VALUE)
            win_error("Copy temporary");
        try {
            Bytes b(chunk_size);
            DWORD n, w;
            while (true) {
                if (!ReadFile(src, b.data(), DWORD(b.size()), &n, nullptr))
                    win_error("Copy read");
                if (!n)
                    break;
                if (!WriteFile(out, b.data(), n, &w, nullptr) || w != n)
                    win_error("Copy write");
            }
            if (!FlushFileBuffers(out))
                win_error("Copy flush");
            out = Handle();
            if (!MoveFileExW(temporary.c_str(), dest.path.c_str(), MOVEFILE_WRITE_THROUGH))
                win_error("Copy commit");
        } catch (...) {
            out = Handle();
            DeleteFileW(temporary.c_str());
            throw;
        }
    } else
        throw Error(Code::unsupported, "File operation unsupported");
}
} // namespace lantern
