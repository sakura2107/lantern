#include "lantern/host_admin.hpp"
namespace lantern {
namespace {
void scopes_valid(uint32_t scopes) {
    if (scopes > 31)
        throw Error(Code::invalid, "权限范围必须为0..31");
}
TrustedDevice& device(Config& c, const std::string& id) {
    if (!valid_sha(id))
        throw Error(Code::invalid, "设备ID无效");
    for (auto& d : c.devices)
        if (d.id == id)
            return d;
    throw Error(Code::not_found, "设备不存在，请刷新");
}
void subset(const Config& c, uint32_t scopes) {
    scopes_valid(scopes);
    if (scopes & ~c.scopes)
        throw Error(Code::unauthorized, "设备权限超过主机上限，请先明确调整上限");
}
/** The identity/config directory must never be reachable through the shared root. */
void outside_share(const fs::path& target, const fs::path& shared) {
    auto a = target.wstring(), b = shared.wstring();
    std::transform(a.begin(), a.end(), a.begin(), towlower);
    std::transform(b.begin(), b.end(), b.begin(), towlower);
    auto relative = fs::path(a).lexically_relative(fs::path(b));
    if (!relative.empty() && *relative.begin() != L"..")
        throw Error(Code::unauthorized, "身份状态不能放在授权共享目录内");
}
} // namespace
void HostAdministration::initialize(const fs::path& state, const fs::path& root, uint32_t scopes) {
    scopes_valid(scopes);
    if (!scopes)
        throw Error(Code::invalid, "至少选择一项权限上限");
    auto target = fs::weakly_canonical(fs::absolute(state)),
         shared = fs::weakly_canonical(fs::absolute(root));
    FileService validate(shared);
    if (fs::exists(target))
        throw Error(Code::conflict, "新状态目录必须尚不存在；已有主机请直接加载");
    outside_share(target, shared);
    fs::create_directories(target.parent_path());
    auto stage = target.parent_path() / (L".lantern-init-" + wide(random_token()));
    if (!fs::create_directory(stage))
        throw Error(Code::conflict, "初始化暂存目录冲突");
    try {
        {
            Identity identity(stage / L"identity.dpapi", true);
            Config cfg{shared, random_token(), scopes};
            cfg.legacy_enabled = false;
            save_config(stage / L"config.dpapi", cfg);
        }
        if (!MoveFileExW(stage.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH))
            win_error("Commit new host identity");
    } catch (...) {
        DeleteFileW((stage / L"config.dpapi").c_str());
        DeleteFileW((stage / L"identity.dpapi").c_str());
        RemoveDirectoryW(stage.c_str());
        throw;
    }
}
HostAdministration::HostAdministration(fs::path state) : state_(fs::absolute(state).lexically_normal()) {
    snapshot();
}
HostAdminSnapshot HostAdministration::snapshot() const {
    auto c = load_config(state_ / L"config.dpapi");
    Identity identity(state_ / L"identity.dpapi", false);
    HostAdminSnapshot s{c.root, identity.fingerprint(), c.scopes, c.legacy_enabled, {}};
    for (auto& d : c.devices)
        s.devices.push_back({d.id, d.label, d.scopes, d.scopes & c.scopes});
    return s;
}
DevicePairing HostAdministration::add(const std::string& label, uint32_t scopes) {
    if (label.empty() || label.size() > 128 ||
        std::any_of(label.begin(), label.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }) ||
        !scopes)
        throw Error(Code::invalid, "填写1..128字节设备名称并选择至少一项权限");
    wide(label);
    TrustedDevice d{random_token(), label, random_token(), scopes};
    edit_config(state_ / L"config.dpapi", [&](Config& c) {
        subset(c, scopes);
        c.devices.push_back(d);
    });
    Identity identity(state_ / L"identity.dpapi", false);
    return {{d.id, d.label, d.scopes, d.scopes}, identity.fingerprint(), d.token};
}
void HostAdministration::grant(const std::string& id, uint32_t scopes) {
    edit_config(state_ / L"config.dpapi", [&](Config& c) {
        subset(c, scopes);
        device(c, id).scopes = scopes;
    });
}
void HostAdministration::remove(const std::string& id) {
    edit_config(state_ / L"config.dpapi", [&](Config& c) {
        device(c, id);
        std::erase_if(c.devices, [&](auto& d) { return d.id == id; });
    });
}
void HostAdministration::end_device(const std::string& id) {
    edit_config(state_ / L"config.dpapi", [&](Config& c) {
        auto& d = device(c, id);
        if (d.epoch == UINT64_MAX)
            throw Error(Code::conflict, "设备会话代数耗尽");
        ++d.epoch;
    });
}
void HostAdministration::ceiling(uint32_t scopes) {
    scopes_valid(scopes);
    edit_config(state_ / L"config.dpapi", [&](Config& c) { c.scopes = scopes; });
}
void HostAdministration::root(const fs::path& shared_root) {
    auto shared = fs::weakly_canonical(fs::absolute(shared_root));
    FileService validate(shared); // existing directory, no reparse point
    outside_share(fs::weakly_canonical(state_), shared);
    edit_config(state_ / L"config.dpapi", [&](Config& c) { c.root = shared; });
}
void HostAdministration::disable_legacy() {
    edit_config(state_ / L"config.dpapi", [](Config& c) { c.legacy_enabled = false; });
}
DevicePairing HostAdministration::pairing(const std::string& id) const {
    auto c = load_config(state_ / L"config.dpapi");
    auto d = device(c, id);
    Identity identity(state_ / L"identity.dpapi", false);
    return {{d.id, d.label, d.scopes, d.scopes & c.scopes}, identity.fingerprint(), d.token};
}
} // namespace lantern
