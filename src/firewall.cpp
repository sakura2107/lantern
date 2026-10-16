#include "lantern/detail/firewall.hpp"
#include "lantern/lan.hpp"
#include <charconv>
#include <netfw.h>
#include <oleauto.h>
#include <shellapi.h>
#include <wrl/client.h>
namespace lantern {
namespace {
using Microsoft::WRL::ComPtr;
void checked(HRESULT hr) {
    if (FAILED(hr))
        throw Error(Code::io, "无法读取或修改 Windows 防火墙策略");
}
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComScope() {
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            checked(hr);
    }
    ~ComScope() {
        if (SUCCEEDED(hr))
            CoUninitialize();
    }
};
struct Bstr {
    BSTR value;
    explicit Bstr(const std::wstring& s) : value(SysAllocStringLen(s.data(), UINT(s.size()))) {
        if (!value)
            throw std::bad_alloc();
    }
    ~Bstr() {
        SysFreeString(value);
    }
    operator BSTR() const {
        return value;
    }
};
template <class F> std::wstring property(F get) {
    BSTR s = nullptr;
    checked(get(&s));
    std::wstring value(s ? s : L"", s ? SysStringLen(s) : 0);
    SysFreeString(s);
    return value;
}
ComPtr<INetFwPolicy2> policy() {
    ComPtr<INetFwPolicy2> p;
    checked(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&p)));
    return p;
}
std::wstring exe_path(const fs::path& exe) {
    auto input = exe.wstring();
    if (input.empty() || input.find(L'\0') != input.npos || input.find(L'"') != input.npos)
        throw Error(Code::invalid, "程序路径无效");
    DWORD size = ExpandEnvironmentStringsW(input.c_str(), nullptr, 0);
    if (!size || size > 32768)
        throw Error(Code::invalid, "程序路径无效");
    std::wstring expanded(size, L'\0');
    if (!ExpandEnvironmentStringsW(input.c_str(), expanded.data(), size))
        throw Error(Code::invalid, "程序路径无效");
    expanded.resize(size - 1);
    return fs::absolute(expanded).lexically_normal().wstring();
}
bool same_exe(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(exe_path(fs::path(a)).c_str(), b.c_str()) == 0;
}
bool ports_match(std::wstring ports, uint16_t port) {
    if (ports.empty() || ports == L"*")
        return true;
    for (size_t start = 0; start < ports.size();) {
        auto end = ports.find(L',', start);
        auto part = ports.substr(start, end == ports.npos ? ports.npos : end - start);
        part.erase(std::remove_if(part.begin(), part.end(), [](wchar_t c) { return c == L' '; }), part.end());
        auto dash = part.find(L'-');
        auto number = [](const std::wstring& s) -> unsigned {
            if (s.empty() || s.size() > 5 ||
                std::any_of(s.begin(), s.end(), [](wchar_t c) { return c < L'0' || c > L'9'; }))
                return 0;
            auto n = std::stoul(s);
            return n <= 65535 ? unsigned(n) : 0;
        };
        unsigned low = number(part.substr(0, dash)),
                 high = dash == part.npos ? low : number(part.substr(dash + 1));
        if (low && high >= low && port >= low && port <= high)
            return true;
        if (end == ports.npos)
            break;
        start = end + 1;
    }
    return false;
}
bool constrained(INetFwRule* r) {
    auto local = property([&](BSTR* s) { return r->get_LocalAddresses(s); });
    auto remote = property([&](BSTR* s) { return r->get_RemoteAddresses(s); });
    auto service = property([&](BSTR* s) { return r->get_ServiceName(s); });
    auto types = property([&](BSTR* s) { return r->get_InterfaceTypes(s); });
    auto ports = property([&](BSTR* s) { return r->get_RemotePorts(s); });
    VARIANT interfaces;
    VariantInit(&interfaces);
    checked(r->get_Interfaces(&interfaces));
    bool has_interfaces = interfaces.vt != VT_EMPTY && interfaces.vt != VT_NULL;
    VariantClear(&interfaces);
    return (!local.empty() && local != L"*") ||
           (!remote.empty() && remote != L"*" && remote != L"LocalSubnet") || !service.empty() ||
           (!types.empty() && types != L"All") || (!ports.empty() && ports != L"*") || has_interfaces;
}
} // namespace
namespace detail {
void configure_public_firewall_rule(INetFwRule* rule, const std::wstring& exe, uint16_t port) {
    configure_firewall_rule(rule, exe, port, NET_FW_IP_PROTOCOL_TCP);
    checked(rule->put_RemoteAddresses(Bstr(L"*")));
    checked(rule->put_Description(Bstr(L"Lantern 公网 TCP 主机；仅受信任的专用网络，用户显式启用")));
}
void configure_firewall_rule(INetFwRule* rule, const std::wstring& exe, uint16_t port, long protocol) {
    checked(rule->put_Enabled(VARIANT_FALSE));
    checked(rule->put_Description(Bstr(L"Lantern 私有局域网主机；仅专用网络和本地子网")));
    checked(rule->put_ApplicationName(Bstr(exe)));
    checked(rule->put_ServiceName(Bstr(L"")));
    checked(rule->put_Direction(NET_FW_RULE_DIR_IN));
    checked(rule->put_Protocol(protocol));
    checked(rule->put_LocalPorts(Bstr(std::to_wstring(port))));
    checked(rule->put_RemotePorts(Bstr(L"*")));
    checked(rule->put_LocalAddresses(Bstr(L"*")));
    checked(rule->put_RemoteAddresses(Bstr(L"LocalSubnet")));
    checked(rule->put_InterfaceTypes(Bstr(L"All")));
    VARIANT interfaces;
    VariantInit(&interfaces);
    checked(rule->put_Interfaces(interfaces));
    checked(rule->put_Profiles(NET_FW_PROFILE2_PRIVATE));
    checked(rule->put_EdgeTraversal(VARIANT_FALSE));
    checked(rule->put_Action(NET_FW_ACTION_ALLOW));
    checked(rule->put_Enabled(VARIANT_TRUE));
}
} // namespace detail
namespace {
void configure_rule(INetFwRules* rules, const wchar_t* name, const std::wstring& exe, uint16_t port,
                    long protocol, bool internet = false) {
    ComPtr<INetFwRule> rule;
    Bstr key(name);
    HRESULT hr = rules->Item(key, &rule);
    bool existing = SUCCEEDED(hr);
    if (!existing)
        checked(CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rule)));
    checked(rule->put_Name(key));
    if (internet)
        detail::configure_public_firewall_rule(rule.Get(), exe, port);
    else
        detail::configure_firewall_rule(rule.Get(), exe, port, protocol);
    if (!existing)
        checked(rules->Add(rule.Get()));
}
} // namespace
static FirewallState firewall_state_impl(const fs::path& exe, uint16_t port, bool internet) noexcept {
    try {
        if (!port)
            return FirewallState::unknown;
        ComScope com;
        auto p = policy();
        auto target = exe_path(exe);
        VARIANT_BOOL enabled = VARIANT_FALSE, blocked = VARIANT_FALSE;
        checked(p->get_FirewallEnabled(NET_FW_PROFILE2_PRIVATE, &enabled));
        checked(p->get_BlockAllInboundTraffic(NET_FW_PROFILE2_PRIVATE, &blocked));
        if (!enabled)
            return FirewallState::allowed;
        if (blocked)
            return FirewallState::not_allowed;
        NET_FW_MODIFY_STATE modify;
        checked(p->get_LocalPolicyModifyState(&modify));
        if (modify == NET_FW_MODIFY_STATE_GP_OVERRIDE)
            return FirewallState::unknown;
        ComPtr<INetFwRules> rules;
        checked(p->get_Rules(&rules));
        ComPtr<IUnknown> iterator;
        checked(rules->get__NewEnum(&iterator));
        ComPtr<IEnumVARIANT> enumeration;
        checked(iterator.As(&enumeration));
        bool allowed = false, uncertain = false;
        for (;;) {
            VARIANT v;
            VariantInit(&v);
            ULONG fetched = 0;
            HRESULT next = enumeration->Next(1, &v, &fetched);
            if (next == S_FALSE) {
                VariantClear(&v);
                break;
            }
            checked(next);
            ComPtr<INetFwRule> rule;
            if (v.vt == VT_DISPATCH && v.pdispVal)
                v.pdispVal->QueryInterface(IID_PPV_ARGS(&rule));
            VariantClear(&v);
            if (!rule)
                continue;
            VARIANT_BOOL active;
            NET_FW_RULE_DIRECTION direction;
            long profiles, protocol;
            NET_FW_ACTION action;
            checked(rule->get_Enabled(&active));
            checked(rule->get_Direction(&direction));
            checked(rule->get_Profiles(&profiles));
            if (!active || direction != NET_FW_RULE_DIR_IN || !(profiles & NET_FW_PROFILE2_PRIVATE))
                continue;
            checked(rule->get_Protocol(&protocol));
            if (protocol != NET_FW_IP_PROTOCOL_TCP && protocol != NET_FW_IP_PROTOCOL_ANY)
                continue;
            auto app = property([&](BSTR* s) { return rule->get_ApplicationName(s); });
            if (!app.empty() && !same_exe(app, target))
                continue;
            if (protocol == NET_FW_IP_PROTOCOL_TCP &&
                !ports_match(property([&](BSTR* s) { return rule->get_LocalPorts(s); }), port))
                continue;
            checked(rule->get_Action(&action));
            if (internet) {
                auto remote = property([&](BSTR* s) { return rule->get_RemoteAddresses(s); });
                if (!remote.empty() && remote != L"*") {
                    if (action == NET_FW_ACTION_BLOCK)
                        uncertain = true;
                    continue;
                }
            }
            if (constrained(rule.Get())) {
                uncertain = true;
                continue;
            }
            if (action == NET_FW_ACTION_BLOCK)
                return FirewallState::not_allowed;
            if (action == NET_FW_ACTION_ALLOW)
                allowed = true;
        }
        if (uncertain)
            return FirewallState::unknown;
        if (allowed)
            return FirewallState::allowed;
        NET_FW_ACTION action;
        checked(p->get_DefaultInboundAction(NET_FW_PROFILE2_PRIVATE, &action));
        return action == NET_FW_ACTION_ALLOW ? FirewallState::allowed : FirewallState::not_allowed;
    } catch (...) {
        return FirewallState::unknown;
    }
}
FirewallState firewall_state(const fs::path& exe, uint16_t port) noexcept {
    return firewall_state_impl(exe, port, false);
}
FirewallState public_firewall_state(const fs::path& exe, uint16_t port) noexcept {
    return firewall_state_impl(exe, port, true);
}
uint32_t firewall_current_profiles() noexcept {
    try {
        ComScope com;
        long profiles = 0;
        checked(policy()->get_CurrentProfileTypes(&profiles));
        return uint32_t(profiles);
    } catch (...) {
        return 0;
    }
}
static void require_elevation() {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw))
        win_error("Elevation token");
    Handle token(raw);
    TOKEN_ELEVATION info{};
    DWORD bytes = 0;
    if (!GetTokenInformation(token, TokenElevation, &info, sizeof(info), &bytes) || !info.TokenIsElevated)
        throw Error(Code::unauthorized, "此命令需要管理员权限");
}
void firewall_public_allow(const fs::path& exe, uint16_t port) {
    if (!port || !fs::is_regular_file(exe))
        throw Error(Code::invalid, "请选择有效程序和端口");
    require_elevation();
    ComScope com;
    ComPtr<INetFwRules> rules;
    checked(policy()->get_Rules(&rules));
    configure_rule(rules.Get(), L"Lantern 主机（公网）", exe_path(exe), port, NET_FW_IP_PROTOCOL_TCP, true);
}
void firewall_public_remove(const fs::path& exe) {
    require_elevation();
    ComScope com;
    ComPtr<INetFwRules> rules;
    checked(policy()->get_Rules(&rules));
    ComPtr<INetFwRule> rule;
    Bstr name(L"Lantern 主机（公网）");
    auto hr = rules->Item(name, &rule);
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return;
    checked(hr);
    if (!same_exe(property([&](BSTR* s) { return rule->get_ApplicationName(s); }), exe_path(exe)))
        throw Error(Code::conflict, "公网规则属于另一程序");
    checked(rules->Remove(name));
}
void firewall_allow(const fs::path& exe, uint16_t port) {
    if (!port || !fs::is_regular_file(exe))
        throw Error(Code::invalid, "请选择有效程序和1..65535端口");
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        win_error("Open elevation token");
    Handle owner(token);
    TOKEN_ELEVATION elevated{};
    DWORD bytes = 0;
    if (!GetTokenInformation(token, TokenElevation, &elevated, sizeof(elevated), &bytes) ||
        !elevated.TokenIsElevated)
        throw Error(Code::unauthorized, "此命令需要管理员权限");
    ComScope com;
    auto p = policy();
    ComPtr<INetFwRules> rules;
    checked(p->get_Rules(&rules));
    auto target = exe_path(exe);
    configure_rule(rules.Get(), L"Lantern 主机", target, port, NET_FW_IP_PROTOCOL_TCP);
    configure_rule(rules.Get(), L"Lantern 主机（发现）", target, 47442, NET_FW_IP_PROTOCOL_UDP);
}
static bool request_firewall_change(const fs::path& exe, uint16_t port, const wchar_t* command) noexcept {
    try {
        if (!port || !fs::is_regular_file(exe))
            return false;
        wchar_t module[32768]{};
        auto length = GetModuleFileNameW(nullptr, module, 32768);
        if (!length || length >= 32768)
            return false;
        auto cli = fs::path(module).parent_path() / L"lantern.exe";
        if (!fs::is_regular_file(cli))
            return false;
        auto parameters = std::wstring(command) + L" \"" + exe_path(exe) + L"\" " + std::to_wstring(port);
        SHELLEXECUTEINFOW call{sizeof(call)};
        call.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        call.lpVerb = L"runas";
        call.lpFile = cli.c_str();
        call.lpParameters = parameters.c_str();
        call.nShow = SW_HIDE;
        if (!ShellExecuteExW(&call))
            return false;
        Handle process(call.hProcess);
        if (!process.h || WaitForSingleObject(process, 60000) != WAIT_OBJECT_0)
            return false;
        DWORD code = 1;
        return GetExitCodeProcess(process, &code) && code == 0;
    } catch (...) {
        return false;
    }
}
bool request_firewall_allow(const fs::path& exe, uint16_t port) noexcept {
    return request_firewall_change(exe, port, L"firewall-allow");
}
bool request_public_firewall(const fs::path& exe, uint16_t port, bool allow) noexcept {
    return request_firewall_change(exe, port, allow ? L"firewall-public-allow" : L"firewall-public-remove");
}
} // namespace lantern
