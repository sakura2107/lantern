#include "lantern/win.hpp"
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>
namespace lantern {
struct FileJobs::Impl {
    struct Job {
        FileJobInfo info;
        uint32_t kind;
        std::string from, to, owner, session;
        Cancel cancel;
    };
    FileService& files;
    std::mutex mutex;
    std::condition_variable ready;
    std::map<std::string, std::shared_ptr<Job>> jobs;
    std::deque<std::shared_ptr<Job>> queue;
    std::vector<std::thread> workers;
    bool stopping = false;
    explicit Impl(FileService& service) : files(service) {
        for (int i = 0; i < 2; i++)
            workers.emplace_back([this] { run(); });
    }
    void stop() {
        std::lock_guard lock(mutex);
        stopping = true;
        for (auto& [id, job] : jobs)
            job->cancel.requested = true;
        ready.notify_all();
    }
    ~Impl() {
        stop();
        for (auto& worker : workers)
            worker.join();
    }
    void run() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock(mutex);
                ready.wait(lock, [&] { return stopping || !queue.empty(); });
                if (stopping)
                    return;
                job = queue.front();
                queue.pop_front();
                job->info.state = TaskState::running;
            }
            try {
                auto progress = [&](uint64_t current, uint64_t total) {
                    std::lock_guard lock(mutex);
                    job->info.current = current;
                    job->info.total = total;
                };
                if (job->kind == 0)
                    files.copy_tree(job->from, job->to, job->cancel, progress);
                else if (job->kind == 1)
                    files.remove_tree(job->from, job->cancel, progress);
                else {
                    auto metadata = files.hash_snapshot(job->from, job->cancel, progress);
                    std::lock_guard lock(mutex);
                    job->info.file = std::move(metadata);
                }
                std::lock_guard lock(mutex);
                job->info.state = TaskState::verifying;
                job->info.state = TaskState::completed;
            } catch (const Error& error) {
                std::lock_guard lock(mutex);
                job->info.error = error.code;
                job->info.message = error.what();
                job->info.state = error.code == Code::cancelled ? TaskState::cancelled : TaskState::failed;
            } catch (...) {
                std::lock_guard lock(mutex);
                job->info.error = Code::io;
                job->info.message = "Host file operation failed";
                job->info.state = TaskState::failed;
            }
        }
    }
};
FileJobs::FileJobs(FileService& files) : p_(std::make_unique<Impl>(files)) {}
FileJobs::~FileJobs() = default;
void FileJobs::stop() {
    p_->stop();
}
void FileJobs::disconnect(const std::string& session) {
    if (session.empty())
        return;
    std::lock_guard lock(p_->mutex);
    for (auto& [id, job] : p_->jobs)
        if (job->kind == 2 && job->session == session)
            job->cancel.requested = true;
}
void FileJobs::authorize(const std::string& owner, uint32_t scopes) {
    authorize_all([&](const std::string& token){return token==owner?scopes:0u;});
}
void FileJobs::cancel_session(const std::string& session){std::lock_guard lock(p_->mutex);for(auto& [id,job]:p_->jobs)if(job->session==session)job->cancel.requested=true;}
void FileJobs::authorize_all(const std::function<uint32_t(const std::string&)>& allowed) {
    std::lock_guard lock(p_->mutex);
    for (auto& [id, job] : p_->jobs) {
        auto scopes=allowed(job->owner);
        if ((job->kind != 2 && !(scopes & files_write)) ||
            (job->kind != 1 && !(scopes & files_read)))
            job->cancel.requested = true;
    }
}
std::string FileJobs::start(uint32_t kind, const std::string& from, const std::string& to,
                            const std::string& owner, const std::string& session) {
    if (kind > 2 || from.empty() || !safe_path(from) || (kind == 0 && (to.empty() || !safe_path(to))))
        throw Error(Code::invalid, "Host task parameters");
    std::lock_guard lock(p_->mutex);
    if (p_->stopping)
        throw Error(Code::cancelled, "Host stopping");
    for (auto it = p_->jobs.begin(); it != p_->jobs.end();) {
        auto state = it->second->info.state;
        if (p_->jobs.size() >= 16 &&
            (state == TaskState::completed || state == TaskState::failed || state == TaskState::cancelled))
            it = p_->jobs.erase(it);
        else
            ++it;
    }
    if (p_->jobs.size() >= 16 || p_->queue.size() >= 8)
        throw Error(Code::busy, "Host file task queue full");
    auto job = std::make_shared<Impl::Job>();
    job->info.id = random_token();
    job->info.state = TaskState::queued;
    job->info.kind = kind;
    job->kind = kind;
    job->from = from;
    job->to = to;
    job->owner = owner;
    job->session = session;
    p_->jobs.emplace(job->info.id, job);
    p_->queue.push_back(job);
    p_->ready.notify_one();
    return job->info.id;
}
FileJobInfo FileJobs::poll(const std::string& id, const std::string& owner) {
    std::lock_guard lock(p_->mutex);
    auto it = p_->jobs.find(id);
    if (it == p_->jobs.end())
        throw Error(Code::not_found, "Host file task expired or host restarted");
    if (it->second->owner != owner)
        throw Error(Code::unauthorized, "Task belongs to another trust generation");
    return it->second->info;
}
void FileJobs::cancel(const std::string& id, const std::string& owner) {
    std::lock_guard lock(p_->mutex);
    auto it = p_->jobs.find(id);
    if (it == p_->jobs.end())
        throw Error(Code::not_found, "Host file task not found");
    if (it->second->owner != owner)
        throw Error(Code::unauthorized, "Task belongs to another trust generation");
    it->second->cancel.requested = true;
}
} // namespace lantern
