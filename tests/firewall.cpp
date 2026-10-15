#include "lantern/detail/firewall.hpp"
#include <iostream>
#include <wrl/client.h>
using namespace lantern;
int wmain(int, wchar_t** argv) {
    try {
        auto hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr))
            throw std::runtime_error("COM initialization failed");
        struct Cleanup {
            ~Cleanup() {
                CoUninitialize();
            }
        } cleanup;
        unsigned checks = 0;
        auto check = [&](bool ok) {
            if (!ok)
                throw std::runtime_error("Firewall check failed");
            ++checks;
        };
        Microsoft::WRL::ComPtr<INetFwRule> rule;
        check(SUCCEEDED(
            CoCreateInstance(__uuidof(NetFwRule), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&rule))));
        auto exe = fs::absolute(argv[0]).wstring();
        for (auto port : {uint16_t(47443), uint16_t(47444)}) {
            // This rule is never passed to INetFwRules::Add: no elevation or OS policy modification.
            detail::configure_firewall_rule(rule.Get(), exe, port, NET_FW_IP_PROTOCOL_TCP);
            long profile = 0, protocol = 0;
            VARIANT_BOOL enabled = VARIANT_FALSE, edge = VARIANT_TRUE;
            NET_FW_RULE_DIRECTION direction;
            NET_FW_ACTION action;
            check(SUCCEEDED(rule->get_Profiles(&profile)) && profile == NET_FW_PROFILE2_PRIVATE);
            check(SUCCEEDED(rule->get_Protocol(&protocol)) && protocol == NET_FW_IP_PROTOCOL_TCP);
            check(SUCCEEDED(rule->get_Enabled(&enabled)) && enabled);
            check(SUCCEEDED(rule->get_EdgeTraversal(&edge)) && !edge);
            check(SUCCEEDED(rule->get_Direction(&direction)) && direction == NET_FW_RULE_DIR_IN);
            check(SUCCEEDED(rule->get_Action(&action)) && action == NET_FW_ACTION_ALLOW);
            BSTR ports = nullptr;
            check(SUCCEEDED(rule->get_LocalPorts(&ports)));
            check(std::wstring(ports) == std::to_wstring(port));
            SysFreeString(ports);
        }
        detail::configure_firewall_rule(rule.Get(), exe, 47442, NET_FW_IP_PROTOCOL_UDP);
        long protocol = 0;
        check(SUCCEEDED(rule->get_Protocol(&protocol)) && protocol == NET_FW_IP_PROTOCOL_UDP);
        BSTR remote = nullptr;
        check(SUCCEEDED(rule->get_RemoteAddresses(&remote)));
        check(_wcsicmp(remote, L"LocalSubnet") == 0);
        SysFreeString(remote);
        auto state = firewall_state(fs::path(argv[0]), 47443);
        check(state == FirewallState::allowed || state == FirewallState::not_allowed ||
              state == FirewallState::unknown);
        check(firewall_state(fs::path(argv[0]), 0) == FirewallState::unknown);
        std::cout << checks
                  << " firewall checks passed; unattached rule configuration + read-only OS policy query; no "
                     "rules added\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
