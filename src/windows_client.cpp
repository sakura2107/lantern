#include "lantern/windows_client.hpp"
#include "lantern/windows_tree.hpp"
#include <objbase.h>
#include <set>
namespace lantern {
namespace {
void credential_write(Writer& w, const ClientCredentials& c) {
    w.str(c.host);
    w.u32(c.port);
    w.str(c.pin);
    w.str(c.token);
}
ClientCredentials credential_read(Reader& r) {
    ClientCredentials c;
    c.host = r.str(253);
    auto port = r.u32();
    c.pin = r.str(64);
    c.token = r.str(64);
    if (c.host.empty() || !port || port > 65535 || !valid_sha(c.pin) || !valid_sha(c.token))
        throw Error(Code::invalid, "Invalid saved device");
    c.port = uint16_t(port);
    return c;
}
void atomic_private(const fs::path& path, const Bytes& bytes) {
    fs::create_directories(path.parent_path());
    auto tmp = path;
    tmp += L".tmp";
    write_file(tmp, protect(bytes));
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        win_error("Save client state");
}
std::vector<Entry> parse_entries(Reader& r, uint32_t bound) {
    uint32_t n = r.u32();
    if (n > bound)
        throw Error(Code::invalid, "File result bound");
    std::vector<Entry> entries;
    for (uint32_t i = 0; i < n; ++i) {
        auto name = r.str();
        auto kind = r.u32();
        auto size = r.u64(), mtime = r.u64();
        if (kind > 1 || !safe_path(name))
            throw Error(Code::invalid, "Invalid file entry");
        entries.push_back({name, kind != 0, size, mtime});
    }
    return entries;
}
} // namespace
std::shared_ptr<Client> ClientCredentials::connect(uint32_t scopes, uint32_t channel) const {
    return std::make_shared<Client>(host, port, pin, token, scopes, channel);
}
void ActiveConnection::set(std::shared_ptr<Client> c) {
    std::lock_guard lock(mutex_);
    client_ = std::move(c);
}
void ActiveConnection::interrupt() {
    std::lock_guard lock(mutex_);
    if (client_)
        client_->interrupt();
}
void save_credentials(const fs::path& p, const ClientCredentials& c) {
    Writer w;
    w.u32(1);
    credential_write(w, c);
    atomic_private(p, w.b);
}
ClientCredentials load_credentials(const fs::path& p) {
    auto b = unprotect(read_file(p));
    Reader r{b};
    if (r.u32() != 1)
        throw Error(Code::unsupported, "Client state version");
    auto c = credential_read(r);
    r.end();
    return c;
}
DirectoryPage client_page(Client& client, const std::string& path, uint32_t offset, const std::string& filter,
                          uint32_t sort, bool descending, const std::string& gen) {
    Writer w;
    w.u32(1);
    w.str(path);
    w.u32(offset);
    w.u32(256);
    w.str(filter);
    w.u32(sort);
    w.u32(descending ? 1 : 0);
    w.str(gen);
    auto b = client.call(Op::files_page, std::move(w));
    Reader r{b};
    if (r.u32() != 1)
        throw Error(Code::invalid, "File page version");
    DirectoryPage p;
    p.generation = r.str();
    p.total = r.u32();
    p.entries = parse_entries(r, 256);
    if (p.total < p.entries.size() || p.total > 200000)
        throw Error(Code::invalid, "File page total bound");
    r.end();
    return p;
}
SearchPage client_search(Client& client, uint32_t phase, const std::string& path, const std::string& query) {
    Writer w;
    w.u32(1);
    w.u32(phase);
    if (!phase) {
        w.str(path);
        w.str(query);
    }
    auto b = client.call(Op::files_search, std::move(w));
    Reader r{b};
    if (r.u32() != 1)
        throw Error(Code::invalid, "Search version");
    SearchPage p;
    auto done = r.u32();
    p.done = done != 0;
    p.scanned = r.u64();
    p.entries = parse_entries(r, 128);
    if (done > 1 || p.scanned > 200000)
        throw Error(Code::invalid, "Search bound");
    r.end();
    return p;
}
std::string client_job(Client& c, uint32_t kind, const std::string& path, const std::string& dest) {
    Writer w;
    w.u32(1);
    w.u32(kind);
    w.str(path);
    w.str(dest);
    auto b = c.call(Op::file_job, std::move(w));
    Reader r{b};
    auto id = r.str();
    r.end();
    if (!valid_sha(id))
        throw Error(Code::invalid, "Host task id");
    return id;
}
FileJobInfo client_job_poll(Client& c, const std::string& id) {
    Writer w;
    w.u32(1);
    w.str(id);
    auto b = c.call(Op::file_job_poll, std::move(w));
    Reader r{b};
    FileJobInfo j;
    j.id = id;
    auto state = r.u32();
    j.current = r.u64();
    j.total = r.u64();
    auto error = r.u32();
    j.message = r.str();
    j.kind = r.u32();
    j.file = {r.u64(), r.str(), r.str()};
    if (state > 6 || error > 10 || j.kind > 2)
        throw Error(Code::invalid, "Host task state");
    j.state = TaskState(state);
    j.error = Code(error);
    r.end();
    return j;
}
void client_job_cancel(Client& c, const std::string& id) {
    Writer w;
    w.u32(1);
    w.str(id);
    c.call(Op::file_job_cancel, std::move(w));
}
WindowsTransfers::WindowsTransfers(fs::path p, std::function<void()> changed,
                                   std::function<void(const fs::path&, bool)> open,
                                   std::function<uint32_t()> realtime)
    : journal_(std::move(p)), changed_(std::move(changed)), open_(std::move(open)),
      realtime_(std::move(realtime)) {
    if (fs::exists(journal_)) {
        auto b = unprotect(read_file(journal_, 1024 * 1024));
        Reader r{b};
        auto version = r.u32();
        if (version < 1 || version > 3)
            throw Error(Code::unsupported, "Transfer journal version");
        auto n = r.u32();
        if (n > 32)
            throw Error(Code::invalid, "Transfer journal bound");
        std::set<std::string> ids;
        for (uint32_t i = 0; i < n; ++i) {
            TransferTask t;
            t.id = r.str(64);
            t.credentials = credential_read(r);
            t.remote = r.str();
            t.local = fs::path(wide(r.str()));
            auto direction = r.u32(), state = r.u32();
            if (version >= 2) {
                auto directory = r.u32();
                if (directory > 1)
                    throw Error(Code::invalid, "Transfer type bound");
                t.directory = directory != 0;
            }
            if (version >= 3) {
                t.priority = r.u32();
                t.rate_limit = r.u64();
                if (t.priority > 2 ||
                    (t.rate_limit && (t.rate_limit < 65536 || t.rate_limit > 32ull * 1024 * 1024)))
                    throw Error(Code::invalid, "Task policy bound");
            }
            t.current = r.u64();
            t.total = r.u64();
            t.message = r.str();
            if (!valid_sha(t.id) || !ids.insert(t.id).second || !safe_path(t.remote) ||
                !t.local.is_absolute() || direction > 1 || state > 6)
                throw Error(Code::invalid, "Transfer journal corrupt");
            t.upload = direction != 0;
            t.state = state == 4 ? TaskState::completed : TaskState::paused;
            tasks_.push_back(std::move(t));
        }
        r.end();
    }
    worker_ = std::thread([this] {
        try {
            run();
        } catch (const std::exception& e) {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            cancel_.requested = true;
            for (auto& t : tasks_)
                if (t.state != TaskState::completed) {
                    t.state = TaskState::failed;
                    t.message = std::string("本机任务日志或队列失败，请重启客户端：") + e.what();
                }
            changed_();
        }
    });
}
WindowsTransfers::~WindowsTransfers() {
    stop();
}
void WindowsTransfers::persist() {
    Writer w;
    w.u32(3);
    w.u32(uint32_t(tasks_.size()));
    for (auto& t : tasks_) {
        w.str(t.id);
        credential_write(w, t.credentials);
        w.str(t.remote);
        w.str(utf8(t.local.wstring()));
        w.u32(t.upload ? 1 : 0);
        w.u32(uint32_t(t.state));
        w.u32(t.directory ? 1 : 0);
        w.u32(t.priority);
        w.u64(t.rate_limit);
        w.u64(t.current);
        w.u64(t.total);
        w.str(t.message);
    }
    atomic_private(journal_, w.b);
}
std::string WindowsTransfers::add(ClientCredentials c, const std::string& remote, const fs::path& local,
                                  bool upload, bool open, bool chooser, bool directory) {
    if (!safe_path(remote) || remote.empty() || !local.is_absolute())
        throw Error(Code::invalid, "Invalid transfer destination");
    std::lock_guard lock(mutex_);
    if (stopped_)
        throw Error(Code::io, "传输队列已停止，请重启客户端");
    if (tasks_.size() >= 32)
        throw Error(Code::busy, "传输列表最多32项，请清除完成任务");
    for (auto& t : tasks_)
        if (t.state != TaskState::completed && t.state != TaskState::cancelled &&
            ((!upload && !t.upload && t.local == local) ||
             (upload && t.upload && t.credentials.pin == c.pin && t.remote == remote)))
            throw Error(Code::conflict, "目的地已有任务，请继续原任务");
    TransferTask t;
    t.id = random_token();
    t.credentials = std::move(c);
    t.remote = remote;
    t.local = local;
    t.upload = upload;
    t.directory = directory;
    t.open = open;
    t.chooser = chooser;
    t.state = TaskState::queued;
    auto id = t.id;
    tasks_.push_back(std::move(t));
    persist();
    cv_.notify_all();
    changed_();
    return id;
}
uint64_t effective_transfer_rate(uint32_t foreground, uint64_t user_limit) {
    uint64_t cap = (foreground & 2)   ? 2ull * 1024 * 1024
                   : (foreground & 1) ? 4ull * 1024 * 1024
                                      : 32ull * 1024 * 1024;
    return user_limit ? std::min(cap, user_limit) : cap;
}
void WindowsTransfers::set_policy(const std::string& id, uint32_t priority, uint64_t rate) {
    if (priority > 2 || (rate && (rate < 65536 || rate > 32ull * 1024 * 1024)))
        throw Error(Code::invalid, "任务优先级或限速无效");
    std::lock_guard lock(mutex_);
    if (stopped_)
        throw Error(Code::io, "传输队列已停止");
    auto it = std::find_if(tasks_.begin(), tasks_.end(), [&](auto& t) { return t.id == id; });
    if (it == tasks_.end())
        throw Error(Code::invalid, "任务不存在");
    auto old_priority = it->priority;
    auto old_rate = it->rate_limit;
    it->priority = priority;
    it->rate_limit = rate;
    try {
        persist();
    } catch (...) {
        it->priority = old_priority;
        it->rate_limit = old_rate;
        throw;
    }
    ++policy_epoch_;
    cv_.notify_all();
    changed_();
}
std::vector<std::string> WindowsTransfers::add_upload_batch(ClientCredentials c,
                                                            const std::vector<UploadSelection>& selection) {
    if (selection.empty() || selection.size() > 32)
        throw Error(Code::invalid, "请选择1到32个文件");
    std::vector<TransferTask> pending;
    std::set<std::wstring> destinations;
    for (auto& item : selection) {
        if (item.remote.empty() || !safe_path(item.remote) || !item.local.is_absolute())
            throw Error(Code::invalid, "上传路径无效");
        // Fail the whole selection before admitting any task. The upload worker revalidates and locks
        // sources.
        for (auto path = item.local; !path.empty(); path = path.parent_path()) {
            auto attr = GetFileAttributesW(path.c_str());
            if (attr == INVALID_FILE_ATTRIBUTES)
                win_error("Upload source");
            if (attr & FILE_ATTRIBUTE_REPARSE_POINT)
                throw Error(Code::unauthorized, "上传来源不能包含重解析点");
            if (path == path.parent_path())
                break;
        }
        if (!fs::is_regular_file(item.local))
            throw Error(Code::invalid, "批量上传仅支持普通文件");
        auto key = wide(item.remote);
        std::transform(key.begin(), key.end(), key.begin(), towlower);
        if (!destinations.insert(key).second)
            throw Error(Code::conflict, "所选文件的目标名称重复");
        TransferTask t;
        t.id = random_token();
        t.credentials = c;
        t.remote = item.remote;
        t.local = item.local;
        t.upload = true;
        t.state = TaskState::queued;
        pending.push_back(std::move(t));
    }
    std::lock_guard lock(mutex_);
    if (stopped_)
        throw Error(Code::io, "传输队列已停止");
    if (tasks_.size() + pending.size() > 32)
        throw Error(Code::busy, "传输列表最多32项，请清除完成任务");
    for (auto& t : tasks_)
        if (t.upload && t.state != TaskState::completed && t.state != TaskState::cancelled &&
            t.credentials.pin == c.pin) {
            auto key = wide(t.remote);
            std::transform(key.begin(), key.end(), key.begin(), towlower);
            if (destinations.contains(key))
                throw Error(Code::conflict, "目的地已有任务，请继续原任务");
        }
    auto original = tasks_.size();
    std::vector<std::string> ids;
    for (auto& t : pending) {
        ids.push_back(t.id);
        tasks_.push_back(std::move(t));
    }
    try {
        persist();
    } catch (...) {
        tasks_.resize(original);
        throw;
    }
    cv_.notify_all();
    changed_();
    return ids;
}
std::vector<TransferTask> WindowsTransfers::snapshot() {
    std::lock_guard lock(mutex_);
    return tasks_;
}
void WindowsTransfers::resume(const std::string& id) {
    std::lock_guard lock(mutex_);
    if (stopped_)
        throw Error(Code::io, "传输队列已停止，请重启客户端");
    for (auto& t : tasks_)
        if (t.id == id && t.state != TaskState::completed && t.state != TaskState::running) {
            t.state = TaskState::queued;
            t.message.clear();
            persist();
            cv_.notify_all();
            changed_();
            return;
        }
}
void WindowsTransfers::pause(const std::string& id) {
    std::lock_guard lock(mutex_);
    for (auto& t : tasks_)
        if (t.id == id && t.state != TaskState::completed) {
            t.state = TaskState::paused;
            if (active_ == id) {
                cancel_.requested = true;
                connection_.interrupt();
            }
            persist();
            cv_.notify_all();
            changed_();
            return;
        }
}
void WindowsTransfers::remove(const std::string& id) {
    std::lock_guard lock(mutex_);
    if (active_ == id)
        throw Error(Code::busy, "请先暂停并等待当前分块结束");
    std::erase_if(tasks_, [&](auto& t) { return t.id == id; });
    persist();
    changed_();
}
void WindowsTransfers::rename_destination(const std::string& id, const std::string& remote,
                                          const fs::path& local) {
    std::lock_guard lock(mutex_);
    if (active_ == id)
        throw Error(Code::busy, "任务仍在运行");
    for (auto& t : tasks_)
        if (t.id == id) {
            if (t.state == TaskState::completed)
                throw Error(Code::invalid, "任务已完成");
            if (!safe_path(remote) || remote.empty() || !local.is_absolute())
                throw Error(Code::invalid, "新目的地无效");
            t.remote = remote;
            t.local = local;
            if (t.directory)
                t.id = random_token(); // Directory manifests are bound to the original destination.
            t.current = 0;
            t.state = TaskState::paused;
            t.message = "已另存，点击继续";
            persist();
            changed_();
            return;
        }
}
void WindowsTransfers::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        TransferTask task;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                return stopped_ || std::any_of(tasks_.begin(), tasks_.end(),
                                               [](auto& t) { return t.state == TaskState::queued; });
            });
            if (stopped_)
                break;
            auto it = tasks_.end();
            for (auto candidate = tasks_.begin(); candidate != tasks_.end(); ++candidate)
                if (candidate->state == TaskState::queued &&
                    (it == tasks_.end() || candidate->priority > it->priority))
                    it = candidate;
            it->state = TaskState::running;
            task = *it;
            active_ = task.id;
            cancel_.requested = false;
            persist();
        }
        changed_();
        auto update = [&](TaskState state, const std::string& msg) {
            std::lock_guard lock(mutex_);
            for (auto& t : tasks_)
                if (t.id == task.id) {
                    if (t.state != TaskState::paused || state == TaskState::completed)
                        t.state = state;
                    t.message = msg;
                    persist();
                }
            changed_();
        };
        for (unsigned attempt = 0;; ++attempt) {
            try {
                cancel_.check();
                auto tree_manifest = journal_.parent_path() / L"trees" / (wide(task.id) + L".dpapi");
                if (task.directory && !task.upload)
                    if (auto complete = recover_committed_tree(task, tree_manifest, cancel_)) {
                        {
                            std::lock_guard lock(mutex_);
                            for (auto& t : tasks_)
                                if (t.id == task.id)
                                    t.current = t.total = *complete;
                        }
                        update(TaskState::completed, "已恢复目录提交回执，逐文件SHA-256校验完成");
                        finalize_tree_journal(tree_manifest);
                        break;
                    }
                auto client = task.credentials.connect(task.upload ? files_read | files_write : files_read);
                connection_.set(client);
                cancel_.check();
                auto cache = journal_.parent_path() / L"cache";
                auto relative = task.local.lexically_relative(cache);
                if (!task.upload && !task.directory && !relative.empty() &&
                    *relative.begin() !=
                        L"..") { // Single worker enforces the hard cache bound before each queued download.
                    auto source = client->stat(task.remote, false);
                    if (source.size > 512ull * 1024 * 1024)
                        throw Error(Code::unsupported, "打开缓存单文件上限512MiB");
                    uint64_t used = 0;
                    fs::create_directories(cache);
                    for (auto& entry : fs::recursive_directory_iterator(cache)) {
                        cancel_.check();
                        if (GetFileAttributesW(entry.path().c_str()) & FILE_ATTRIBUTE_REPARSE_POINT)
                            throw Error(Code::unauthorized, "打开缓存不能包含重解析点");
                        if (entry.is_regular_file()) {
                            auto n = entry.file_size();
                            if (n > 2ull * 1024 * 1024 * 1024 - used)
                                throw Error(Code::busy, "打开缓存已超过2GiB，请清理缓存");
                            used += n;
                        }
                    }
                    uint64_t present = 0;
                    auto part = fs::path(task.local.wstring() + L".lantern-part");
                    if (fs::exists(task.local))
                        present = fs::file_size(task.local);
                    else if (fs::exists(part))
                        present = fs::file_size(part);
                    auto extra = source.size - std::min(source.size, present);
                    if (extra > 2ull * 1024 * 1024 * 1024 - used)
                        throw Error(Code::busy, "打开缓存达到2GiB上限，请在外部应用退出后清理");
                }
                uint64_t last = 0;
                auto deadline = std::chrono::steady_clock::now(), notified = deadline;
                auto progress = [&](uint64_t n, uint64_t total) {
                    cancel_.check();
                    auto now = std::chrono::steady_clock::now();
                    auto transferred = client->transfer_bytes();
                    uint64_t delta = transferred - last;
                    last = transferred; // Resume offsets/verified existing files consume no new bandwidth.
                    uint64_t user_limit = 0, policy = 0;
                    {
                        std::lock_guard lock(mutex_);
                        policy = policy_epoch_;
                        for (auto& t : tasks_)
                            if (t.id == task.id)
                                user_limit = t.rate_limit;
                    }
                    uint64_t speed = effective_transfer_rate(realtime_(), user_limit);
                    deadline = std::max(deadline, now) + std::chrono::microseconds(delta * 1000000 / speed);
                    {
                        std::unique_lock lock(mutex_);
                        while (!stopped_ && !cancel_.requested &&
                               std::chrono::steady_clock::now() < deadline) {
                            cv_.wait_until(lock, deadline, [&] {
                                return stopped_ || cancel_.requested.load() || policy != policy_epoch_;
                            });
                            if (policy != policy_epoch_) {
                                policy = policy_epoch_;
                                for (auto& t : tasks_)
                                    if (t.id == task.id)
                                        user_limit = t.rate_limit;
                                deadline = now + std::chrono::microseconds(
                                                     delta * 1000000 /
                                                     effective_transfer_rate(realtime_(), user_limit));
                            }
                        }
                        for (auto& t : tasks_)
                            if (t.id == task.id) {
                                t.current = n;
                                t.total = total;
                            }
                    }
                    cancel_.check();
                    if (now - notified >= std::chrono::milliseconds(150) || n == total) {
                        changed_();
                        notified = now;
                    }
                };
                if (task.directory)
                    transfer_tree(*client, task,
                                  journal_.parent_path() / L"trees" / (wide(task.id) + L".dpapi"), cancel_,
                                  progress);
                else if (task.upload)
                    client->upload(task.local, task.remote, cancel_, progress);
                else {
                    fs::create_directories(task.local.parent_path());
                    if (fs::exists(task.local)) {
                        auto info = client->stat(task.remote, true, &cancel_);
                        if (hash_file(task.local) != info.hash)
                            throw Error(Code::conflict, "本地同名文件不同，选择另存为");
                        progress(info.size, info.size);
                    } else
                        client->download(task.remote, task.local, cancel_, progress);
                }
                update(TaskState::completed, "SHA-256校验与提交完成");
                if (task.directory)
                    finalize_tree_journal(tree_manifest);
                if (task.open)
                    open_(task.local, task.chooser);
                break;
            } catch (const TransportError& e) {
                connection_.set({});
                if (cancel_.requested) {
                    update(TaskState::paused, "已暂停，保留可恢复分块");
                    break;
                }
                if (attempt >= 9) {
                    update(TaskState::failed, e.what());
                    break;
                }
                update(TaskState::running, "网络中断，等待重连 " + std::to_string(attempt + 1) + "/10");
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(std::min(30000u, 500u << std::min(attempt, 6u))),
                             [&] { return stopped_ || cancel_.requested.load(); });
            } catch (const Error& e) {
                update(cancel_.requested || e.code == Code::cancelled ? TaskState::paused : TaskState::failed,
                       e.what());
                break;
            } catch (const std::exception& e) {
                update(TaskState::failed, e.what());
                break;
            }
        }
        connection_.set({});
        {
            std::lock_guard lock(mutex_);
            active_.clear();
        }
        changed_();
    }
    CoUninitialize();
}
void WindowsTransfers::stop() {
    {
        std::lock_guard lock(mutex_);
        if (!stopped_) {
            stopped_ = true;
            cancel_.requested = true;
            connection_.interrupt();
            for (auto& t : tasks_)
                if (t.state == TaskState::running || t.state == TaskState::queued)
                    t.state = TaskState::paused;
            try {
                persist();
            } catch (const std::exception& e) {
                for (auto& t : tasks_)
                    if (t.state != TaskState::completed)
                        t.message = std::string("任务日志保存失败，临时文件保留：") + e.what();
                changed_();
            }
        }
    }
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
}
} // namespace lantern
