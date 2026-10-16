#pragma once
#include "endpoint_client.hpp"
#include "win.hpp"
#include <condition_variable>
#include <thread>
namespace lantern {
struct ClientCredentials {
    std::string host, pin, token;
    uint16_t port = 47443;
    std::vector<ConnectionEndpoint> endpoints;
    std::shared_ptr<Client> connect(uint32_t scopes = files_read, uint32_t channel = UINT32_MAX) const;
};
void merge_endpoints(ClientCredentials&, const std::vector<ConnectionEndpoint>&);
std::vector<EndpointProbe> probe_endpoints(const ClientCredentials&,
                                           std::chrono::milliseconds timeout = std::chrono::seconds(10));
// Lifetime-safe interruption of one connection. No TLS send/receive occurs under this lock.
class ActiveConnection {
    std::mutex mutex_;
    std::shared_ptr<Client> client_;

  public:
    void set(std::shared_ptr<Client>);
    void interrupt();
};
void save_credentials(const fs::path&, const ClientCredentials&);
ClientCredentials load_credentials(const fs::path&);
DirectoryPage client_page(Client&, const std::string&, uint32_t offset, const std::string& filter,
                          uint32_t sort, bool descending, const std::string& generation);
SearchPage client_search(Client&, uint32_t phase, const std::string& path = {},
                         const std::string& query = {});
std::string client_job(Client&, uint32_t kind, const std::string&, const std::string&);
FileJobInfo client_job_poll(Client&, const std::string&);
void client_job_cancel(Client&, const std::string&);
struct TransferTask {
    std::string id, remote, message;
    fs::path local;
    ClientCredentials credentials;
    bool upload = false, open = false, chooser = false, directory = false;
    TaskState state = TaskState::paused;
    uint64_t current = 0, total = 0;
    uint32_t priority = 1;
    uint64_t rate_limit = 0;
};
struct UploadSelection {
    fs::path local;
    std::string remote;
};
uint64_t effective_transfer_rate(uint32_t foreground, uint64_t user_limit);
class WindowsTransfers {
    fs::path journal_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<TransferTask> tasks_;
    std::thread worker_;
    bool stopped_ = false;
    Cancel cancel_;
    std::string active_;
    ActiveConnection connection_;
    std::function<void()> changed_;
    std::function<void(const fs::path&, bool)> open_;
    std::function<uint32_t()> realtime_;
    uint64_t policy_epoch_ = 0;
    void persist();
    void run();

  public:
    WindowsTransfers(fs::path, std::function<void()>, std::function<void(const fs::path&, bool)>,
                     std::function<uint32_t()>);
    ~WindowsTransfers();
    std::string add(ClientCredentials, const std::string&, const fs::path&, bool upload, bool open = false,
                    bool chooser = false, bool directory = false);
    std::vector<TransferTask> snapshot();
    void resume(const std::string&);
    void pause(const std::string&);
    void remove(const std::string&);
    void rename_destination(const std::string&, const std::string& remote, const fs::path& local);
    void stop();
    void set_policy(const std::string&, uint32_t priority, uint64_t bytes_per_second);
    std::vector<std::string> add_upload_batch(ClientCredentials, const std::vector<UploadSelection>&);
};
} // namespace lantern
