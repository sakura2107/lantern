#include "lantern/windows_tree.hpp"
#include <set>
namespace lantern {
namespace {
struct Node {
    std::string path, version;
    bool directory = false;
    uint64_t size = 0;
    std::string hash;
};
std::string joined(const std::string& a, const std::string& b) {
    return b.empty() ? a : a + "/" + b;
}
void local_guard(const fs::path& path) {
    auto p = fs::absolute(path).lexically_normal();
    for (auto it = p.begin(), end = p.end(); it != end; ++it) {
        fs::path prefix;
        for (auto k = p.begin(); k != std::next(it); ++k)
            prefix /= *k;
        auto attr = GetFileAttributesW(prefix.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
            throw Error(Code::unauthorized, "Local directory contains a reparse point");
    }
}
std::string local_version(const fs::path& path) {
    local_guard(path);
    Handle f(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    BY_HANDLE_FILE_INFORMATION i{};
    if (f.h == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(f, &i))
        win_error("Directory snapshot");
    if (i.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        throw Error(Code::unauthorized, "Local link denied");
    return std::to_string(i.dwVolumeSerialNumber) + ":" + std::to_string(i.nFileIndexHigh) + ":" +
           std::to_string(i.nFileIndexLow) + ":" + std::to_string(i.nFileSizeHigh) + ":" +
           std::to_string(i.nFileSizeLow) + ":" + std::to_string(i.ftLastWriteTime.dwHighDateTime) + ":" +
           std::to_string(i.ftLastWriteTime.dwLowDateTime);
}
void snapshot_remote(Client& c, const std::string& root, std::string relative, std::vector<Node>& nodes,
                     Cancel& cancel, unsigned depth) {
    if (depth > 32 || nodes.size() > 9999)
        throw Error(Code::busy, "Directory limit: 32 levels / 10000 entries");
    auto first = client_page(c, joined(root, relative), 0, "", 0, false, "");
    nodes.push_back({relative, first.generation, true, 0});
    auto page = first;
    uint32_t offset = 0;
    for (;;) {
        cancel.check();
        for (auto& entry : page.entries) {
            auto path = relative.empty() ? entry.name : relative + "/" + entry.name;
            if (nodes.size() > 9999)
                throw Error(Code::busy, "Directory entry bound");
            if (entry.directory)
                snapshot_remote(c, root, path, nodes, cancel, depth + 1);
            else {
                auto info = c.stat(joined(root, path), false);
                nodes.push_back({path, info.version, false, info.size});
            }
        }
        offset += uint32_t(page.entries.size());
        if (offset >= page.total)
            break;
        page = client_page(c, joined(root, relative), offset, "", 0, false, first.generation);
    }
    client_page(c, joined(root, relative), 0, "", 0, false, first.generation);
}
void snapshot_local(const fs::path& root, std::string relative, std::vector<Node>& nodes, Cancel& cancel,
                    unsigned depth) {
    cancel.check();
    if (depth > 32 || nodes.size() > 9999)
        throw Error(Code::busy, "Directory limit: 32 levels / 10000 entries");
    auto folder = root / fs::path(wide(relative));
    auto version = local_version(folder);
    nodes.push_back({relative, version, true, 0});
    for (auto& item : fs::directory_iterator(folder)) {
        cancel.check();
        local_guard(item.path());
        auto name = utf8(item.path().filename().wstring());
        auto path = relative.empty() ? name : relative + "/" + name;
        if (!safe_path(path))
            throw Error(Code::invalid, "Directory entry cannot be shared safely");
        if (item.is_directory())
            snapshot_local(root, path, nodes, cancel, depth + 1);
        else {
            if (nodes.size() > 9999)
                throw Error(Code::busy, "Directory entry bound");
            nodes.push_back({path, local_version(item.path()), false, item.file_size()});
        }
    }
    if (local_version(folder) != version)
        throw Error(Code::source_changed, "Local directory changed during snapshot");
}
void save_manifest(const fs::path& path, const TransferTask& task, const std::vector<Node>& nodes) {
    Writer w;
    w.u32(1);
    w.str(task.id);
    w.str(task.remote);
    w.str(utf8(task.local.wstring()));
    w.str(task.credentials.pin);
    w.u32(task.upload ? 1 : 0);
    w.u32(uint32_t(nodes.size()));
    for (auto& n : nodes) {
        w.str(n.path);
        w.str(n.version);
        w.u32(n.directory ? 1 : 0);
        w.u64(n.size);
    }
    if (w.b.size() > 1024 * 1024)
        throw Error(Code::busy, "Directory manifest exceeds 1 MiB");
    fs::create_directories(path.parent_path());
    auto temp = path;
    temp += L".tmp";
    write_file(temp, protect(w.b));
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        win_error("Commit tree manifest");
}
std::vector<Node> load_manifest(const fs::path& path, const TransferTask& task) {
    auto b = unprotect(read_file(path, 2 * 1024 * 1024));
    Reader r{b};
    if (r.u32() != 1 || r.str() != task.id || r.str() != task.remote ||
        r.str() != utf8(task.local.wstring()) || r.str() != task.credentials.pin ||
        r.u32() != (task.upload ? 1u : 0u))
        throw Error(Code::conflict, "Directory manifest does not match task");
    auto count = r.u32();
    if (!count || count > 10000)
        throw Error(Code::invalid, "Directory manifest count");
    std::vector<Node> result;
    std::set<std::string> paths;
    for (uint32_t i = 0; i < count; ++i) {
        Node n;
        n.path = r.str();
        n.version = r.str();
        auto kind = r.u32();
        n.directory = kind != 0;
        n.size = r.u64();
        if (kind > 1 || !safe_path(n.path) || !paths.insert(n.path).second ||
            (i == 0 && (!n.path.empty() || !n.directory)) || (i > 0 && n.path.empty()))
            throw Error(Code::invalid, "Directory manifest corrupt");
        result.push_back(n);
    }
    r.end();
    return result;
}
fs::path receipt_path(const fs::path& manifest) {
    return fs::path(manifest.wstring() + L".commit");
}
std::string local_identity(const fs::path& path) {
    auto version = local_version(path);
    size_t at = 0;
    for (int i = 0; i < 3; ++i) {
        at = version.find(':', at);
        if (at == std::string::npos)
            throw Error(Code::invalid, "Local identity");
        ++at;
    }
    return version.substr(0, at - 1);
}
void save_receipt(const fs::path& manifest, const TransferTask& task, const fs::path& stage,
                  const std::vector<Node>& nodes) {
    Writer w;
    w.u32(1);
    w.str(task.id);
    w.str(task.remote);
    w.str(utf8(task.local.wstring()));
    w.str(task.credentials.pin);
    w.str(local_identity(stage));
    w.u32(uint32_t(nodes.size()));
    for (auto& n : nodes) {
        w.str(n.path);
        w.u32(n.directory ? 1 : 0);
        w.u64(n.size);
        w.str(n.hash);
    }
    if (w.b.size() > 2 * 1024 * 1024)
        throw Error(Code::busy, "Directory commit receipt bound");
    auto path = receipt_path(manifest), temp = fs::path(path.wstring() + L".tmp");
    write_file(temp, protect(w.b));
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        win_error("Commit directory receipt");
}
} // namespace
std::optional<uint64_t> recover_committed_tree(const TransferTask& task, const fs::path& manifest,
                                               Cancel& cancel) {
    if (task.upload || !task.directory || !fs::exists(task.local) || !fs::exists(receipt_path(manifest)))
        return std::nullopt;
    auto data = unprotect(read_file(receipt_path(manifest), 3 * 1024 * 1024));
    Reader r{data};
    if (r.u32() != 1 || r.str() != task.id || r.str() != task.remote ||
        r.str() != utf8(task.local.wstring()) || r.str() != task.credentials.pin)
        throw Error(Code::conflict, "Directory receipt belongs to another task");
    local_guard(task.local);
    if (r.str() != local_identity(task.local))
        throw Error(Code::conflict, "Committed directory was replaced; preserve destination");
    auto count = r.u32();
    if (!count || count > 10000)
        throw Error(Code::invalid, "Directory receipt count");
    std::map<std::string, Node> expected;
    uint64_t total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        Node n;
        n.path = r.str();
        auto kind = r.u32();
        n.directory = kind != 0;
        n.size = r.u64();
        n.hash = r.str(64);
        if (kind > 1 || !safe_path(n.path) || (i == 0 && (!n.path.empty() || !n.directory)) ||
            (i > 0 && n.path.empty()) || (!n.directory && !valid_sha(n.hash)) ||
            (n.directory && (!n.hash.empty() || n.size)) || n.size > UINT64_MAX - total ||
            !expected.emplace(n.path, n).second)
            throw Error(Code::invalid, "Directory receipt entry");
        total += n.size;
    }
    r.end();
    std::vector<Node> actual;
    snapshot_local(task.local, "", actual, cancel, 0);
    if (actual.size() != expected.size())
        throw Error(Code::conflict, "Committed directory entry set changed");
    FileService files(task.local);
    for (auto& n : actual) {
        cancel.check();
        auto it = expected.find(n.path);
        if (it == expected.end() || n.directory != it->second.directory || n.size != it->second.size)
            throw Error(Code::conflict, "Committed directory contents changed");
        if (!n.directory) {
            auto verified = files.hash_snapshot(n.path, cancel, {});
            if (verified.size != it->second.size || verified.hash != it->second.hash)
                throw Error(Code::integrity, "Committed directory hash changed");
        }
    }
    std::vector<Node> after;
    snapshot_local(task.local, "", after, cancel, 0);
    if (after.size() != actual.size())
        throw Error(Code::source_changed, "Committed directory changed during verification");
    std::map<std::string, std::string> versions;
    for (auto& n : actual)
        versions[n.path] = n.version;
    for (auto& n : after)
        if (!versions.contains(n.path) || versions[n.path] != n.version)
            throw Error(Code::source_changed, "Committed file changed during verification");
    return total;
}
void finalize_tree_journal(const fs::path& manifest) {
    DeleteFileW(manifest.c_str());
    DeleteFileW(receipt_path(manifest).c_str());
}
void transfer_tree(Client& c, const TransferTask& task, const fs::path& manifest, Cancel& cancel,
                   std::function<void(uint64_t, uint64_t)> progress) {
    if (auto complete = recover_committed_tree(task, manifest, cancel)) {
        progress(*complete, *complete);
        return;
    }
    local_guard(task.local);
    std::vector<Node> nodes;
    bool fresh = !fs::exists(manifest);
    auto stage = fs::path(task.local.wstring() + L".lantern-tree-" + wide(task.id.substr(0, 16)));
    if (fresh) {
        if (task.upload) {
            auto slash = task.remote.rfind('/');
            auto parent = slash == std::string::npos ? "" : task.remote.substr(0, slash),
                 leaf = slash == std::string::npos ? task.remote : task.remote.substr(slash + 1);
            auto page = client_page(c, parent, 0, "", 0, false, "");
            uint32_t offset = 0;
            for (;;) {
                for (auto& e : page.entries)
                    if (e.name == leaf)
                        throw Error(Code::conflict, "Remote directory exists; choose a new name");
                offset += uint32_t(page.entries.size());
                if (offset >= page.total)
                    break;
                page = client_page(c, parent, offset, "", 0, false, page.generation);
            }
            snapshot_local(task.local, "", nodes, cancel, 0);
        } else {
            if (fs::exists(task.local) || fs::exists(stage))
                throw Error(Code::conflict, "Local directory or staging area exists");
            snapshot_remote(c, task.remote, "", nodes, cancel, 0);
        }
        save_manifest(manifest, task, nodes);
    } else
        nodes = load_manifest(manifest, task);
    uint64_t total = 0, done = 0;
    for (auto& n : nodes) {
        if (n.size > UINT64_MAX - total)
            throw Error(Code::invalid, "Directory total overflow");
        total += n.size;
    }
    progress(0, total);
    if (!task.upload) {
        if (fs::exists(task.local))
            throw Error(Code::conflict, "Local directory exists; original remains protected");
        local_guard(stage);
        fs::create_directories(stage);
    }
    for (auto& n : nodes) {
        cancel.check();
        auto remote = joined(task.remote, n.path);
        auto local = (task.upload ? task.local : stage) / fs::path(wide(n.path));
        local_guard(local);
        if (n.directory) {
            if (task.upload) {
                if (local_version(local) != n.version)
                    throw Error(Code::source_changed, "Local directory changed since snapshot");
                if (fresh)
                    c.mutate(Op::mkdir, remote);
                else { // Existing directories belong to this persisted task; validate type without accepting
                       // links.
                    try {
                        client_page(c, remote, 0, "", 0, false, "");
                    } catch (const Error& e) {
                        if (e.code != Code::not_found)
                            throw;
                        c.mutate(Op::mkdir, remote);
                    }
                }
            } else {
                client_page(c, remote, 0, "", 0, false, n.version);
                fs::create_directories(local);
            }
            continue;
        }
        auto report = [&](uint64_t current, uint64_t) { progress(done + current, total); };
        if (task.upload) {
            if (local_version(local) != n.version)
                throw Error(Code::source_changed, "Local file changed since snapshot");
            c.upload(local, remote, cancel, report);
            if (local_version(local) != n.version)
                throw Error(Code::source_changed, "Local file changed during upload");
        } else {
            auto before = c.stat(remote, false);
            if (before.version != n.version || before.size != n.size)
                throw Error(Code::source_changed, "Remote file changed since snapshot");
            if (fs::exists(local)) {
                auto info = c.stat(remote, true, &cancel);
                if (hash_file(local) != info.hash)
                    throw Error(Code::integrity, "Completed staged file hash differs");
                n.hash = info.hash;
            } else {
                FileInfo verified;
                c.download(remote, local, cancel, report, &verified);
                n.hash = verified.hash;
            }
            if (c.stat(remote, false).version != n.version)
                throw Error(Code::source_changed, "Remote file changed during download");
        }
        done += n.size;
        progress(done, total);
    }
    for (auto& n : nodes) {
        cancel.check();
        if (task.upload) {
            if (local_version(task.local / fs::path(wide(n.path))) != n.version)
                throw Error(Code::source_changed, "Upload source changed before completion");
        } else if (n.directory)
            client_page(c, joined(task.remote, n.path), 0, "", 0, false, n.version);
        else if (c.stat(joined(task.remote, n.path), false).version != n.version)
            throw Error(Code::source_changed, "Download source changed before directory commit");
    }
    if (!task.upload)
        save_receipt(manifest, task, stage, nodes);
    if (!task.upload && !MoveFileExW(stage.c_str(), task.local.c_str(), MOVEFILE_WRITE_THROUGH))
        win_error("Commit downloaded directory");
    // Receipt and manifest survive until the queue completion record is durable.
    // Do not cancel after committing the directory: a concurrent pause cannot undo its final name.
}
} // namespace lantern
