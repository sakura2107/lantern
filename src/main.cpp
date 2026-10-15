#include "lantern/host_admin.hpp"
#include "lantern/lan.hpp"
#include "lantern/pairing_client.hpp"
#include "lantern/win.hpp"
#include <iostream>
#include <shellapi.h>
using namespace lantern;
static Host* running = nullptr;
static BOOL WINAPI console_signal(DWORD) {
    if (running) {
        running->stop();
        return TRUE;
    }
    return FALSE;
}
static void usage() {
    std::cout << "Lantern 0.1 experimental LAN build\n"
                 "init STATE ROOT SCOPES  (1 read,2 write,4 view,8 control,16 audio; sum explicitly)\n"
                 "grant STATE SCOPES     (replace allowed scope set, including 0 to deny all)\n"
                 "identity STATE         (host certificate SHA256; no credential output)\n"
                 "device-add STATE LABEL SCOPES (unique credential, locally shown once)\n"
                 "device-list STATE; device-grant STATE ID SCOPES; device-remove STATE ID\n"
                 "device-end STATE ID (end current connections; trust remains, clients may reconnect)\n"
                 "device-secret STATE ID LOCAL-DPAPI-FILE; legacy-off STATE\n"
                 "host STATE [BIND=0.0.0.0] [PORT=47443]\n"
                 "invite STATE LABEL SCOPES [MINUTES=10] (prints a one-time pairing URI)\n"
                 "pair LINK DEVICE-LABEL SECRET-FILE (DPAPI credential; never prints the token)\n"
                 "firewall-allow EXE PORT (administrator; Private profile only)\n"
                 "revoke STATE           (rotates credential; interrupts existing trust)\n"
                 "secret STATE LOCAL-DPAPI-FILE  (protect pairing credential for this user)\n"
                 "client HOST PORT PIN SECRET-FILE caps|list|stat|get|put|mkdir|move|copy|delete|frame|media "
                 "ARGS...\n"
                 "get REMOTE LOCAL; put LOCAL REMOTE; list [PATH] [OFFSET] [FILTER]; frame JPEG [MONITOR]\n"
                 "media REMOTE [LOCAL-PORT=47880] [launch] (original byte range bridge for external player)\n"
                 "open LOCAL; open-with LOCAL\n";
}
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::string> a;
        for (int i = 1; i < argc; ++i)
            a.push_back(utf8(argv[i]));
        if (a.empty()) {
            usage();
            return 0;
        }
        auto need = [&](size_t n) {
            if (a.size() < n)
                throw Error(Code::invalid, "Missing argument; run without arguments for help");
        };
        auto bounded_number = [&](const std::string& value, uint64_t low, uint64_t high) {
            if (value.empty() ||
                std::any_of(value.begin(), value.end(), [](char c) { return c < '0' || c > '9'; }))
                throw Error(Code::invalid, "数字参数格式无效");
            uint64_t n = 0;
            try {
                n = std::stoull(value);
            } catch (...) {
                throw Error(Code::invalid, "数字参数超出范围");
            }
            if (n < low || n > high)
                throw Error(Code::invalid, "数字参数超出范围");
            return n;
        };
        if (a[0] == "invite") {
            need(4);
            if (a.size() > 5)
                throw Error(Code::invalid, "邀请命令参数过多");
            auto scopes = uint32_t(bounded_number(a[3], 1, 31));
            auto minutes = a.size() == 5 ? bounded_number(a[4], 1, 1440) : 10;
            std::vector<std::string> addresses;
            for (auto& address : lan_addresses()) {
                if (addresses.size() == 8)
                    break;
                if (std::find(addresses.begin(), addresses.end(), address.address) == addresses.end())
                    addresses.push_back(address.address);
            }
            if (addresses.empty())
                throw Error(Code::not_found, "没有已连接的局域网 IPv4 地址");
            auto invite =
                HostAdministration(wide(a[1])).invite(a[2], scopes, std::chrono::seconds(minutes * 60));
            std::cout << pairing_uri(invite, addresses, 47443) << "\n";
            return 0;
        }
        if (a[0] == "pair") {
            need(4);
            if (a.size() != 4)
                throw Error(Code::invalid, "配对命令参数无效");
            auto link = parse_pairing_uri(a[1]);
            if (!link)
                throw Error(Code::invalid, "配对链接无效或已过期");
            auto result = pair_with_link(*link, a[2]);
            write_file(wide(a[3]),
                       protect({reinterpret_cast<const uint8_t*>(result.token.data()), result.token.size()}));
            std::cout << "配对成功，凭据已用 DPAPI 保存。主机 " << result.host << ":" << result.port
                      << "；设备 " << result.device_id << "；生效权限 " << result.scopes << "\n";
            return 0;
        }
        if (a[0] == "firewall-allow") {
            need(3);
            if (a.size() != 3)
                throw Error(Code::invalid, "防火墙命令参数无效");
            firewall_allow(wide(a[1]), uint16_t(bounded_number(a[2], 1, 65535)));
            std::cout << "已更新 Lantern 专用网络 TCP 和局域网发现 UDP 规则。\n";
            return 0;
        }
        if (a[0] == "init") {
            need(4);
            fs::path state = wide(a[1]);
            fs::create_directories(state);
            if (fs::exists(state / L"config.dpapi"))
                throw Error(Code::conflict, "Host already initialized");
            size_t parsed = 0;
            auto scope_value = std::stoull(a[3], &parsed);
            if (parsed != a[3].size() || !scope_value || scope_value > 31)
                throw Error(Code::invalid, "Choose explicit supported scopes 1..31");
            uint32_t scopes = uint32_t(scope_value);
            FileService validate(wide(a[2]));
            Identity id(state / L"identity.dpapi", true);
            Config config{fs::absolute(wide(a[2])).lexically_normal(), random_token(), scopes};
            save_config(state / L"config.dpapi", config);
            std::cout << "PIN " << id.fingerprint()
                      << "\nPAIRING SECRET (show locally once; do not put on command line) " << config.token
                      << "\n";
            return 0;
        }
        if (a[0] == "identity") {
            need(2);
            Identity id(fs::path(wide(a[1])) / L"identity.dpapi", false);
            std::cout << id.fingerprint() << "\n";
            return 0;
        }
        if (a[0] == "grant") {
            need(3);
            size_t parsed = 0;
            auto scopes = std::stoull(a[2], &parsed);
            if (parsed != a[2].size() || scopes > 31)
                throw Error(Code::invalid, "Permission set must be 0..31");
            auto path = fs::path(wide(a[1])) / L"config.dpapi";
            edit_config(path,[&](Config& config){config.scopes=uint32_t(scopes);});
            std::cout << "Host permission ceiling updated: " << scopes << "\n";
            return 0;
        }
        if(a[0]=="device-add"){
            need(4);size_t parsed=0;auto scopes=std::stoull(a[3],&parsed);if(parsed!=a[3].size()||!scopes||scopes>31)throw Error(Code::invalid,"Device permissions must be 1..31");
            TrustedDevice device{random_token(),a[2],random_token(),uint32_t(scopes)};
            edit_config(fs::path(wide(a[1]))/L"config.dpapi",[&](Config& config){if(device.scopes&~config.scopes)throw Error(Code::unauthorized,"Device permissions exceed host ceiling; change it explicitly first");config.devices.push_back(device);});
            Identity identity(fs::path(wide(a[1]))/L"identity.dpapi",false);
            std::cout<<"DEVICE "<<device.id<<"\nPIN "<<identity.fingerprint()<<"\nPAIRING SECRET (local only) "<<device.token<<"\n";return 0;
        }
        if(a[0]=="device-list"){
            need(2);auto config=load_config(fs::path(wide(a[1]))/L"config.dpapi");
            std::cout<<"Legacy credential "<<(config.legacy_enabled?"enabled":"disabled")<<"; host ceiling "<<config.scopes<<"\n";
            for(auto& device:config.devices)std::cout<<device.id<<"  "<<device.label<<"  allowed="<<(device.scopes&config.scopes)<<"\n";return 0;
        }
        if(a[0]=="legacy-off"){
            need(2);edit_config(fs::path(wide(a[1]))/L"config.dpapi",[](Config& config){config.legacy_enabled=false;});std::cout<<"Legacy shared credential disabled; named devices unchanged.\n";return 0;
        }
        if(a[0]=="device-grant"||a[0]=="device-remove"||a[0]=="device-end"||a[0]=="device-secret"){
            need(a[0]=="device-grant"||a[0]=="device-secret"?4:3);if(!valid_sha(a[2]))throw Error(Code::invalid,"Device ID invalid");
            if(a[0]=="device-secret"){
                auto config=load_config(fs::path(wide(a[1]))/L"config.dpapi");auto it=std::find_if(config.devices.begin(),config.devices.end(),[&](auto& d){return d.id==a[2];});if(it==config.devices.end())throw Error(Code::not_found,"Device not found");
                write_file(wide(a[3]),protect({reinterpret_cast<const uint8_t*>(it->token.data()),it->token.size()}));std::cout<<"Device credential protected for this Windows user.\n";return 0;
            }
            uint32_t scopes=0;if(a[0]=="device-grant"){size_t parsed=0;auto value=std::stoull(a[3],&parsed);if(parsed!=a[3].size()||value>31)throw Error(Code::invalid,"Device permissions must be 0..31");scopes=uint32_t(value);}
            edit_config(fs::path(wide(a[1]))/L"config.dpapi",[&](Config& config){auto it=std::find_if(config.devices.begin(),config.devices.end(),[&](auto& d){return d.id==a[2];});if(it==config.devices.end())throw Error(Code::not_found,"Device not found");
                if(a[0]=="device-remove")config.devices.erase(it);
                else if(a[0]=="device-end"){if(it->epoch==UINT64_MAX)throw Error(Code::conflict,"Session generation exhausted");it->epoch++;}
                else{if(scopes&~config.scopes)throw Error(Code::unauthorized,"Device permissions exceed host ceiling");it->scopes=scopes;}
            });std::cout<<"Device trust updated. Existing affected connections terminate within the host authorization check interval.\n";return 0;
        }
        if (a[0] == "secret") {
            need(3);
            auto config = load_config(fs::path(wide(a[1])) / L"config.dpapi");
            write_file(wide(a[2]), protect({(uint8_t*)config.token.data(), config.token.size()}));
            std::cout << "Local credential protected; usable only as this Windows user. Pair other machines "
                         "via GUI.\n";
            return 0;
        }
        if (a[0] == "revoke") {
            need(2);
            auto path = fs::path(wide(a[1])) / L"config.dpapi";
            std::string token;
            edit_config(path,[&](Config& config){config.token=random_token();config.devices.clear();config.legacy_enabled=true;config.legacy_epoch=0;token=config.token;});
            std::cout << "All trust revoked. New pairing secret (local only): " << token << "\n";
            return 0;
        }
        if (a[0] == "host") {
            need(2);
            if (a.size() > 4) {
                if (a.size() != 6 || a[4] != "--input-guard-pid")
                    throw Error(Code::invalid, "Unknown host diagnostics option");
                auto pid = uint32_t(std::stoul(a[5]));
                if (!pid)
                    throw Error(Code::invalid, "Input diagnostics PID required");
                set_input_validation_pid(pid);
            }
            Host h(wide(a[1]), a.size() > 2 ? a[2] : default_host_bind,
                   a.size() > 3 ? uint16_t(std::stoul(a[3])) : 47443);
            running = &h;
            SetConsoleCtrlHandler(console_signal, TRUE);
            std::cout << "Host ready. No unattended service installed. Ctrl+C terminates sessions.\n";
            h.run();
            running = nullptr;
            return 0;
        }
        if (a[0] == "open" || a[0] == "open-with") {
            need(2);
            open_external(wide(a[1]), a[0] == "open-with");
            return 0;
        }
        if (a[0] != "client") {
            usage();
            return 2;
        }
        need(6);
        auto secret = unprotect(read_file(wide(a[4])));
        std::string token(secret.begin(), secret.end());
        auto command = a[5];
        uint16_t port = uint16_t(std::stoul(a[2]));
        uint32_t scopes = command == "put" || command == "mkdir" || command == "move" || command == "copy" ||
                                  command == "delete"
                              ? files_read | files_write
                          : command == "frame" ? view
                                               : files_read;
        Client c(a[1], port, a[3], token, scopes);
        Cancel cancel;
        if (command == "caps") {
            auto b = c.call(Op::capabilities);
            Reader r{b};
            std::cout << r.str() << "\n";
        } else if (command == "list") {
            auto entries = c.list(a.size() > 6 ? a[6] : "", a.size() > 7 ? uint32_t(std::stoul(a[7])) : 0,
                                  a.size() > 8 ? a[8] : "");
            for (auto& e : entries)
                std::cout << (e.directory ? "DIR " : "FILE ") << e.size << " " << e.name << "\n";
            std::cout << "Entries " << entries.size() << "; next offset +256 if page full\n";
        } else if (command == "stat") {
            need(7);
            auto f = c.stat(a[6]);
            std::cout << f.size << " " << f.hash << "\n";
        } else if (command == "get" || command == "put") {
            need(8);
            auto t = std::chrono::steady_clock::now();
            auto progress = [](uint64_t now, uint64_t total) {
                std::cout << "\r" << now << " / " << total << " bytes" << std::flush;
            };
            if (command == "get")
                c.download(a[6], wide(a[7]), cancel, progress);
            else
                c.upload(wide(a[6]), a[7], cancel, progress);
            std::cout << "\nVerified and committed in "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count()
                      << " s\n";
        } else if (command == "mkdir" || command == "move" || command == "copy" || command == "delete") {
            need(7);
            Op op = command == "mkdir"  ? Op::mkdir
                    : command == "move" ? Op::rename
                    : command == "copy" ? Op::copy
                                        : Op::remove;
            if (op == Op::rename || op == Op::copy)
                need(8);
            c.mutate(op, a[6], a.size() > 7 ? a[7] : "");
            std::cout << "Host operation completed\n";
        } else if (command == "frame") {
            need(7);
            Writer w;
            w.u32(a.size() > 7 ? uint32_t(std::stoul(a[7])) : 0);
            w.u32(1280);
            w.u32(70);
            auto b = c.call(Op::frame, std::move(w));
            Reader r{b};
            write_file(wide(a[6]), r.blob());
            std::cout << "Captured actual desktop JPEG\n";
        } else if (command == "media") {
            need(7);
            media_bridge(a[1], port, a[3], token, a[6], a.size() > 7 ? uint16_t(std::stoul(a[7])) : 47880,
                         a.size() > 8 && a[8] == "launch");
        } else
            throw Error(Code::unsupported, "Unknown client command");
        return 0;
    } catch (const Error& e) {
        std::cerr << "Error " << uint32_t(e.code) << ": " << e.what() << "\n";
        return int(e.code);
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
