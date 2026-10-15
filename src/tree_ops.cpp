#include "lantern/win.hpp"
#include <cwctype>
namespace lantern {
static BY_HANDLE_FILE_INFORMATION checked(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info))
        win_error("Tree file information");
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && info.nNumberOfLinks > 1))
        throw Error(Code::unauthorized, "Tree operation refuses links");
    return info;
}
static Handle tree_handle(const fs::path& path, DWORD access = FILE_READ_ATTRIBUTES) {
    Handle handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                              nullptr));
    if (handle.h == INVALID_HANDLE_VALUE)
        win_error("Open tree path");
    checked(handle);
    return handle;
}
static std::string directory_identity(const BY_HANDLE_FILE_INFORMATION& i) {
    return std::to_string(i.dwVolumeSerialNumber) + ":" + std::to_string(i.nFileIndexHigh) + ":" +
           std::to_string(i.nFileIndexLow) + ":" + std::to_string(i.ftLastWriteTime.dwHighDateTime) + ":" +
           std::to_string(i.ftLastWriteTime.dwLowDateTime);
}
static void rename_tree(HANDLE handle, const fs::path& destination) {
    auto name = destination.wstring();
    Bytes buffer(offsetof(FILE_RENAME_INFO, FileName) + (name.size() + 1) * sizeof(wchar_t));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->FileNameLength = DWORD(name.size() * sizeof(wchar_t));
    std::copy(name.begin(), name.end(), info->FileName);
    if (!SetFileInformationByHandle(handle, FileRenameInfo, info, DWORD(buffer.size())))
        win_error("Commit tree without overwrite");
}
static void erase_stage(const fs::path& path, uint32_t depth = 0) {
    if (depth > 32)
        throw Error(Code::busy, "Staging cleanup depth");
    auto handle = tree_handle(path, DELETE | FILE_READ_ATTRIBUTES);
    auto info = checked(handle);
    if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((path / L"*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE) {
            try {
                do {
                    std::wstring name = data.cFileName;
                    if (name != L"." && name != L"..")
                        erase_stage(path / name, depth + 1);
                } while (FindNextFileW(find, &data));
            } catch (...) {
                FindClose(find);
                throw;
            }
            FindClose(find);
        }
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition)))
        win_error("Staging cleanup");
}
void FileService::copy_tree(const std::string& from, const std::string& to, Cancel& cancel,
                            std::function<void(uint64_t, uint64_t)> progress) {
    if (from.empty() || to.empty())
        throw Error(Code::invalid, "Tree source and destination required");
    auto lower = [](std::string value) {
        auto w = wide(value);
        std::transform(w.begin(), w.end(), w.begin(), towlower);
        return w;
    };
    auto src_name = lower(from), dst_name = lower(to);
    if (src_name == dst_name || dst_name.starts_with(src_name + L"/"))
        throw Error(Code::invalid, "Cannot copy a directory into itself");
    struct Node {
        std::string path, relative, version;
        bool directory;
        uint64_t size;
    };
    std::vector<Node> nodes;
    uint64_t total = 0;
    size_t name_bytes = 0;
    std::function<void(std::string, std::string, uint32_t)> scan = [&](std::string path, std::string relative,
                                                                       uint32_t depth) {
        cancel.check();
        if (depth > 32 || nodes.size() >= 20000)
            throw Error(Code::busy, "Directory transfer depth/item limit");
        auto g = guard(path);
        auto attrs = GetFileAttributesW(g.path.c_str());
        bool directory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
        Node node{path, relative, "", directory, 0};
        if (directory)
            node.version = directory_identity(checked(g.locks.empty() ? root_lock_.h : g.locks.back().h));
        if (!directory) {
            auto metadata = stat(path, false);
            node.size = metadata.size;
            node.version = metadata.version;
            total += metadata.size;
        }
        name_bytes += path.size() + relative.size() + node.version.size();
        if (name_bytes > 8 * 1024 * 1024 || total > uint64_t(INT64_MAX))
            throw Error(Code::busy, "Tree manifest bound");
        nodes.push_back(node);
        if (!directory)
            return;
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((g.path / L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
            if (GetLastError() == ERROR_FILE_NOT_FOUND)
                return;
            win_error("Tree scan");
        }
        try {
            do {
                cancel.check();
                std::string name = utf8(data.cFileName);
                if (name == "." || name == ".." || name.starts_with(".rdx-private"))
                    continue;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    throw Error(Code::unauthorized, "Directory contains a link; copy refused");
                scan(path + "/" + name, relative.empty() ? name : relative + "/" + name, depth + 1);
            } while (FindNextFileW(find, &data));
            if (GetLastError() != ERROR_NO_MORE_FILES)
                win_error("Tree scan interrupted");
        } catch (...) {
            FindClose(find);
            throw;
        }
        FindClose(find);
        if (directory_identity(checked(g.locks.empty() ? root_lock_.h : g.locks.back().h)) != node.version)
            throw Error(Code::source_changed, "Source directory changed while scanning");
    };
    scan(from, "", 0);
    if (nodes.empty() || !nodes.front().directory)
        throw Error(Code::invalid, "Use regular copy for a single file");
    std::lock_guard mutation(mutations_);
    auto destination = guard(to, true);
    if (fs::exists(destination.path))
        throw Error(Code::conflict, "Destination exists; original preserved");
    auto source_root = guard(from);
    auto source_identity = checked(source_root.locks.back());
    for (auto& ancestor : destination.locks) {
        auto target_identity = checked(ancestor);
        if (source_identity.dwVolumeSerialNumber == target_identity.dwVolumeSerialNumber &&
            source_identity.nFileIndexHigh == target_identity.nFileIndexHigh &&
            source_identity.nFileIndexLow == target_identity.nFileIndexLow)
            throw Error(Code::invalid, "Destination parent aliases source directory");
    }
    auto private_path = root_ / L".rdx-private";
    fs::create_directories(private_path);
    auto private_lock = tree_handle(private_path);
    auto temp = private_path / (L"tree-" + wide(random_token()));
    if (!CreateDirectoryW(temp.c_str(), nullptr))
        win_error("Create staging tree");
    Handle stage;
    try {
        stage = tree_handle(temp, DELETE | FILE_READ_ATTRIBUTES);
        std::vector<Handle> directory_locks;
        uint64_t completed = 0;
        if (progress)
            progress(0, total);
        for (auto& node : nodes) {
            cancel.check();
            if (node.relative.empty())
                continue;
            if (!safe_path(node.relative))
                throw Error(Code::unauthorized, "Unsafe tree child");
            auto target = temp / wide(node.relative);
            if (node.directory) {
                if (directory_locks.size() >= 4096)
                    throw Error(Code::busy, "Staging directory handle bound");
                if (!CreateDirectoryW(target.c_str(), nullptr))
                    win_error("Create staging directory");
                directory_locks.push_back(tree_handle(target));
                continue;
            }
            auto guarded = guard(node.path);
            auto source = open_read(guarded);
            auto before = stat(node.path, false);
            if (before.version != node.version)
                throw Error(Code::source_changed, "Tree source changed before copying");
            auto parent = tree_handle(target.parent_path());
            Handle output(CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr));
            if (output.h == INVALID_HANDLE_VALUE)
                win_error("Tree copy destination");
            Bytes buffer(chunk_size);
            uint64_t file_bytes = 0;
            for (;;) {
                cancel.check();
                DWORD read = 0, written = 0;
                if (!ReadFile(source, buffer.data(), DWORD(buffer.size()), &read, nullptr))
                    win_error("Tree copy read");
                if (!read)
                    break;
                if (!WriteFile(output, buffer.data(), read, &written, nullptr) || written != read)
                    win_error("Tree copy write");
                file_bytes += read;
                completed += read;
                if (progress)
                    progress(completed, total);
            }
            if (file_bytes != node.size)
                throw Error(Code::source_changed, "Tree source size changed");
            if (!FlushFileBuffers(output))
                win_error("Tree copy flush");
            checked(output);
        }
        for (auto& node : nodes) {
            cancel.check();
            if (node.directory) {
                auto g = guard(node.path);
                if (directory_identity(checked(g.locks.back())) != node.version)
                    throw Error(Code::source_changed, "Tree directory changed; staging not committed");
            } else if (stat(node.path, false).version != node.version)
                throw Error(Code::source_changed, "Tree source changed; staging not committed");
        }
        cancel.check();
        directory_locks.clear();
        rename_tree(stage, destination.path);
        if (progress)
            progress(total, total);
    } catch (...) {
        stage = Handle();
        try {
            erase_stage(temp);
        } catch (...) {
        }
        throw;
    }
}
void FileService::remove_tree(const std::string& path, Cancel& cancel,
                              std::function<void(uint64_t, uint64_t)> progress) {
    if (path.empty())
        throw Error(Code::unauthorized, "Cannot delete authorized root");
    struct Node {
        std::string path;
        bool directory;
    };
    std::vector<Node> nodes;
    std::function<void(std::string, uint32_t)> scan = [&](std::string current, uint32_t depth) {
        cancel.check();
        if (depth > 32 || nodes.size() >= 20000)
            throw Error(Code::busy, "Delete item/depth limit");
        auto guarded = guard(current);
        bool directory = (GetFileAttributesW(guarded.path.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (directory) {
            WIN32_FIND_DATAW data{};
            HANDLE find = FindFirstFileW((guarded.path / L"*").c_str(), &data);
            if (find != INVALID_HANDLE_VALUE) {
                try {
                    do {
                        std::string name = utf8(data.cFileName);
                        if (name == "." || name == "..")
                            continue;
                        if (name.starts_with(".rdx-private") ||
                            (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                            throw Error(Code::unauthorized, "Delete refuses reserved children and links");
                        scan(current + "/" + name, depth + 1);
                    } while (FindNextFileW(find, &data));
                    if (GetLastError() != ERROR_NO_MORE_FILES)
                        win_error("Delete scan interrupted");
                } catch (...) {
                    FindClose(find);
                    throw;
                }
                FindClose(find);
            } else if (GetLastError() != ERROR_FILE_NOT_FOUND)
                win_error("Delete scan denied");
        }
        nodes.push_back({current, directory});
    };
    scan(path, 0);
    uint64_t completed = 0;
    if (progress)
        progress(0, nodes.size());
    for (auto& node : nodes) {
        cancel.check();
        mutate(Op::remove, node.path);
        ++completed;
        if (progress)
            progress(completed, nodes.size());
    }
}
} // namespace lantern
