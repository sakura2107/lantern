#include "lantern/win.hpp"
#include <cwctype>
namespace lantern {
static std::wstring folded(const std::wstring& input) {
    auto result = input;
    std::transform(result.begin(), result.end(), result.begin(), towlower);
    return result;
}
static bool hidden_entry(const WIN32_FIND_DATAW& data) {
    auto name = folded(data.cFileName);
    return name == L"." || name == L".." || name.starts_with(L".rdx-private") ||
           (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || protected_os_entry(data.dwFileAttributes);
}
struct FindHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FindHandle() {
        if (value != INVALID_HANDLE_VALUE)
            FindClose(value);
    }
    FindHandle() = default;
    FindHandle(const FindHandle&) = delete;
    FindHandle(FindHandle&& other) noexcept : value(other.value) {
        other.value = INVALID_HANDLE_VALUE;
    }
};
static std::string dir_version(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION i{};
    if (!GetFileInformationByHandle(handle, &i))
        win_error("Directory generation");
    return std::to_string(i.dwVolumeSerialNumber) + ":" + std::to_string(i.nFileIndexHigh) + ":" +
           std::to_string(i.nFileIndexLow) + ":" + std::to_string(i.ftLastWriteTime.dwHighDateTime) + ":" +
           std::to_string(i.ftLastWriteTime.dwLowDateTime);
}
static Entry entry(const WIN32_FIND_DATAW& data, std::string prefix = {}) {
    return {prefix + utf8(data.cFileName), (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
            (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow,
            (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime};
}
DirectoryPage FileService::page(const std::string& path, uint32_t off, uint32_t limit,
                                const std::string& filter, uint32_t sort, bool descending,
                                const std::string& expected) {
    if (limit < 1 || limit > 256 || off > 200000 || filter.size() > 256 || sort > 2)
        throw Error(Code::invalid, "Sorted page bounds");
    auto folder = guard(path);
    Handle directory(CreateFileW(folder.path.c_str(), FILE_READ_ATTRIBUTES,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (directory.h == INVALID_HANDLE_VALUE)
        win_error("Open listing directory");
    if (!(GetFileAttributesW(folder.path.c_str()) & FILE_ATTRIBUTE_DIRECTORY))
        throw Error(Code::invalid, "Expected directory");
    DirectoryPage out;
    out.generation = dir_version(directory);

    WIN32_FIND_DATAW data{};
    FindHandle find;
    find.value = FindFirstFileW((folder.path / L"*").c_str(), &data);
    if (find.value == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND)
            return out;
        win_error("Enumerate directory");
    }
    std::vector<Entry> all;
    auto needle = folded(wide(filter));
    size_t memory = 0;
    uint32_t scanned = 0;
    do {
        if (++scanned > 200000)
            throw Error(Code::busy, "Directory scan exceeds 200000 entries");
        if (hidden_entry(data) ||
            (!needle.empty() && folded(data.cFileName).find(needle) == std::wstring::npos))
            continue;
        auto item = entry(data);
        if (item.directory) {
            // NTFS FindFirstFile directory timestamps may lag until a child is opened. Use the
            // authoritative handle metadata so walking a fresh tree does not invalidate its own page.
            auto relative = path.empty() ? item.name : path + "/" + item.name;
            Guard authoritative;
            try {
                authoritative = guard(relative);
            } catch (const Error& e) {
                // A folder this account may not open (e.g. "System Volume Information" in a drive
                // root) can never be browsed either; leave it out instead of failing the whole page.
                if (e.code == Code::unauthorized)
                    continue;
                throw;
            }
            BY_HANDLE_FILE_INFORMATION information{};
            if (authoritative.locks.empty() ||
                !GetFileInformationByHandle(authoritative.locks.back(), &information))
                win_error("Directory entry metadata");
            if (!(information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                throw Error(Code::conflict, "Directory entry changed type");
            item.size = 0;
            item.mtime = (uint64_t(information.ftLastWriteTime.dwHighDateTime) << 32) |
                         information.ftLastWriteTime.dwLowDateTime;
        }
        memory += sizeof(Entry) + item.name.size() + 32;
        if (memory > 32 * 1024 * 1024)
            throw Error(Code::busy, "Directory index exceeds 32MiB; narrow filter");
        all.push_back(std::move(item));
    } while (FindNextFileW(find.value, &data));
    if (GetLastError() != ERROR_NO_MORE_FILES)
        win_error("Directory enumeration changed or denied");
    if (dir_version(directory) != out.generation)
        throw Error(Code::conflict, "Directory changed while listing");
    std::sort(all.begin(), all.end(), [&](const Entry& a, const Entry& b) {
        if (a.directory != b.directory)
            return a.directory;
        if (sort == 1 && a.size != b.size)
            return descending ? a.size > b.size : a.size < b.size;
        if (sort == 2 && a.mtime != b.mtime)
            return descending ? a.mtime > b.mtime : a.mtime < b.mtime;
        auto an = folded(wide(a.name)), bn = folded(wide(b.name));
        if (an == bn)
            return descending ? a.name > b.name : a.name < b.name;
        return descending ? an > bn : an < bn;
    });
    Writer fingerprint;
    fingerprint.str(out.generation);
    fingerprint.str(filter);
    fingerprint.u32(sort);
    fingerprint.u32(descending);
    for (const auto& item : all) {
        fingerprint.str(item.name);
        fingerprint.u32(item.directory);
        fingerprint.u64(item.size);
        fingerprint.u64(item.mtime);
    }
    out.generation = sha256(fingerprint.b);
    if (!expected.empty() && expected != out.generation)
        throw Error(Code::conflict, "Directory changed; restart at first page");
    out.total = uint32_t(all.size());
    size_t end = std::min(all.size(), size_t(off) + limit);
    for (size_t i = off; i < end; i++)
        out.entries.push_back(std::move(all[i]));
    return out;
}
class IncrementalSearch final : public DirectorySearch {
    struct Node {
        std::string path, prefix;
        uint32_t depth;
    };
    FileService& files;
    std::wstring needle;
    std::vector<Node> pending;
    std::unique_ptr<FindHandle> find;
    std::unique_ptr<Guard> active;
    WIN32_FIND_DATAW data{};
    std::string prefix, current_generation, current_path;
    uint32_t depth = 0;
    uint64_t scanned = 0;
    HANDLE directory_handle() const {
        return active->locks.empty() ? files.root_lock_.h : active->locks.back().h;
    }
    void check_generation() {
        if (dir_version(directory_handle()) != current_generation)
            throw Error(Code::conflict, "Directory changed during search; restart");
    }

  public:
    IncrementalSearch(FileService& service, std::string path, std::string filter)
        : files(service), needle(folded(wide(filter))) {
        if (filter.empty() || filter.size() > 256)
            throw Error(Code::invalid, "Search query must be 1..256 bytes");
        auto g = files.guard(path);
        if (!(GetFileAttributesW(g.path.c_str()) & FILE_ATTRIBUTE_DIRECTORY))
            throw Error(Code::invalid, "Expected search directory");
        pending.push_back({path, "", 0});
    }
    SearchPage next() override {
        SearchPage out;
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
        uint32_t work = 0;
        while (work < 500 && out.entries.size() < 128 && std::chrono::steady_clock::now() < until) {
            if (!find) {
                if (pending.empty()) {
                    out.done = true;
                    break;
                }
                auto node = std::move(pending.back());
                pending.pop_back();
                current_path = node.path;
                prefix = node.prefix;
                depth = node.depth;
                active = std::make_unique<Guard>(files.guard(current_path));
                current_generation = dir_version(directory_handle());
                find = std::make_unique<FindHandle>();
                find->value = FindFirstFileW((active->path / L"*").c_str(), &data);
                if (find->value == INVALID_HANDLE_VALUE) {
                    DWORD error = GetLastError();
                    find.reset();
                    active.reset();
                    if (error == ERROR_FILE_NOT_FOUND)
                        continue;
                    SetLastError(error);
                    win_error("Search directory");
                }
            }
            auto current = data;
            ++work;
            if (++scanned > 200000)
                throw Error(Code::busy, "Recursive search scan limit exceeded");
            if (!FindNextFileW(find->value, &data)) {
                DWORD error = GetLastError();
                if (error != ERROR_NO_MORE_FILES) {
                    SetLastError(error);
                    win_error("Search enumeration");
                }
                check_generation();
                find.reset();
                active.reset();
            }
            if (hidden_entry(current))
                continue;
            auto item = entry(current, prefix);
            if (item.directory) {
                if (depth >= 32 || pending.size() >= 4096)
                    throw Error(Code::busy, "Search depth or directory queue limit exceeded");
                std::string name = utf8(current.cFileName);
                pending.push_back(
                    {current_path.empty() ? name : current_path + "/" + name, item.name + "/", depth + 1});
            }
            if (folded(current.cFileName).find(needle) != std::wstring::npos)
                out.entries.push_back(std::move(item));
        }
        if (active)
            check_generation();
        out.scanned = scanned;
        if (!find && pending.empty())
            out.done = true;
        return out;
    }
};
std::unique_ptr<DirectorySearch> FileService::search(const std::string& path, const std::string& query) {
    return std::make_unique<IncrementalSearch>(*this, path, query);
}
Bytes FileService::preview_source(const std::string& path) {
    auto guarded = guard(path);
    auto source = open_read(guarded);
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(source, &size))
        win_error("Preview size");
    if (size.QuadPart <= 0 || size.QuadPart > 16 * 1024 * 1024)
        throw Error(Code::unsupported, "Image preview supports files up to 16MiB");
    Bytes bytes(size_t(size.QuadPart));
    DWORD got = 0;
    if (!ReadFile(source, bytes.data(), DWORD(bytes.size()), &got, nullptr) || got != bytes.size())
        win_error("Preview read");
    return bytes;
}
} // namespace lantern
