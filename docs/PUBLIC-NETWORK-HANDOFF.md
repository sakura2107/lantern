# 公网连接后端交接（给 Claude）

基于 v0.2 / `a412853`，分支 `feature/public-network-backend`。本轮不修改 Windows 或 Android 界面、Manifest。P0 和 P1 后端已提供；P2 HTTPS DDNS 更新、PCP/NAT-PMP、中继未实现。用户填写的 DDNS 名称已支持连接和持久保留，更新该名称的 DNS 记录由用户自己的服务负责。

## 前端接入顺序

1. 三端一起升级：主机配置写 v4、客户端任务 journal 写 v4，旧程序不能读取新版本。已有证书/设备信任沿用，读取 v1..v3 主机配置默认关闭公网。先停止旧程序，备份私有状态，再升级；不要用旧主机读升级后的状态。
2. 新配置默认 bind=`::`，已有保存的监听选择不覆盖。用运行中 `Host::listening_address()/listening_port()` 显示真实监听；系统 IPv6/双栈绑定失败才退回 IPv4。如果旧配置明确是 IPv4，提示用户手动选择双栈，不能替用户覆盖。
3. 公网开关开启前，展示影响并调用 `disable_legacy()`；`internet(true, hostname, external_port)` 遇到仍启用的旧共享凭据会抛 `InternetAccessError`。设备独立凭据和权限不受影响。
4. Windows 防火墙按钮单独调用 `request_public_firewall`；UPnP 按钮单独调用运行中 `Host::upnp_map`。加载配置、诊断、启动主机都不自动添加策略。所有调用放后台工作线程。
5. 关闭时先明确请求删除公网防火墙规则，成功后保存 `internet(false, ...)` 并调用 `upnp_unmap()`；Host 也会在检测到关闭后撤销自己的租约。如果 UAC 被拒绝，不应假装规则已删，应保留防护状态并给出停止主机/手动删除的选项。该开关控制公网入口广告、硬化策略和租约，不替代 Windows/路由器的包过滤；用户另行手动配置的端口转发不由它管理。
6. 扫码邀请默认 `allow_remote=false`。需要在手机外网兑换时必须单独明确选择允许，二维码等同短期访问能力。优先在家里配对，再在外网使用已配对凭据。
7. 认证后调用入口查询并合并保存；重连使用保存的全部入口。显示实际选中的 `public_path`，可建议降低画质；不要因域名/地址变化更换 pin。

## C++ 地址与主机接口

头文件 `include/lantern/lan.hpp`、`network.hpp`、`host_admin.hpp`、`win.hpp`：

```cpp
std::vector<HostAddress> host_addresses();
uint64_t host_address_fingerprint();
AddressKind classify_address(std::string_view);
std::string normalize_address(std::string_view);
std::string source_key(std::string_view);
bool local_pairing_source(std::string_view);
std::vector<ConnectionEndpoint> host_endpoints(const Config&, uint16_t port=47443);
std::vector<ConnectionEndpoint> host_endpoints(const Config&, uint16_t,
                                             const std::vector<HostAddress>&); // pure/testable
InternetSettings HostAdministration::network_settings() const;
void HostAdministration::internet(bool enabled, const std::string& hostname={},
                                  uint16_t external_port=0);
std::string Host::listening_address() const;
uint16_t Host::listening_port() const;
NetworkSecurityStats Host::security_stats() const;
ReachabilityReport Host::reachability_report(const fs::path& exe) const;
ReachabilityReport reachability_report(const Config&, const fs::path& exe,
    const std::string& actual_bind, uint16_t port, bool listening,
    const UpnpReport& upnp={});
```

`InternetSettings` 仅包含 `internet_access/public_hostname/external_port`，无令牌。空域名表示清除，外部端口 0 表示使用监听端口；如果关闭后还想保留这两个字段，调用时传原值。保存使用 `edit_config` 的互斥/原子 DPAPI 写法。`InternetAccessError::reason == InternetError::legacy_enabled` 是专门的可分辨错误，继承 `Error`、code=conflict。

`HostAddress` 继承旧 `LanAddress {address, adapter_name, type, virtual_adapter}`，新增 `category, temporary, deprecated, valid_lifetime, preferred_lifetime`。生命周期为剩余秒数，`UINT32_MAX` 表示无限。类别枚举还包括 invalid/loopback/link_local/multicast，后四种不会出现在枚举结果中。`lan_addresses()` 旧 IPv4 API 保持可用；`host_address_fingerprint()` 忽略每秒递减的生命周期，只在地址、网卡、类别、临时/弃用状态变化时改变，建议每 2..5 秒比较，变化后再刷新候选和二维码。

临时地址判断依据是 Windows [GetAdaptersAddresses 的 unicast 字段](https://learn.microsoft.com/en-us/windows/win32/api/iptypes/ns-iptypes-ip_adapter_unicast_address_lh) 和 [SuffixOrigin 定义](https://learn.microsoft.com/en-us/windows/win32/api/nldef/ne-nldef-nl_suffix_origin)。`Random` 只证明后缀随机，不单独证明会轮换。实现保守地把“随机后缀 + 有限有效期不超过 7 天 + 首选期不超过 1 天”标成可能临时；Transient 标志也排除出稳定入口（该标志还可能表示群集地址）。DAD=Deprecated 或 PreferredLifetime=0 标为弃用。`temporary_ipv6(bool random_suffix, uint32_t valid, uint32_t preferred)` 可单测。Windows 的随机稳定 IID/自定义隐私周期可能被保守漏选或误判，UI 应允许查看全部地址并显示“可能临时”，不能声称精确识别所有系统配置。稳定入站地址优先非临时、未弃用地址；公网前缀改变仍必须依靠 DDNS。

`ReachabilityReport` 返回 bind/port/listening/dual_stack、stable_global_ipv6、cgnat、internet_access、legacy_enabled、public_firewall、current_profiles（Windows profile 位图，0 未知）、upnp、完整地址模型。`cgnat=false` 仅表示未检测到；没有映射查询时不代表存在公网 IPv4。该报告**只证明本机侧条件，不能证明外网可达**。

安全统计包含总 rejected/throttled/timeouts 和最近最多 32 个 `RejectEvent {source, unix_time, reason}`。IPv6 source 已聚合 /64，IPv4 映射已还原。统计只存在于运行中 Host，不跨进程/重启共享，无凭据。公网模式接纳上限、10 秒 TLS+首帧预算、来源退避见 SECURITY。无需界面自行实现网络限速。

## C++ 入口与客户端

`ConnectionEndpoint` 在 `network.hpp`：`kind, host, port, priority, expires_at, failures`。kind 的线值 1..5 见 PROTOCOL；hostname 无 scheme/path，IPv6 host 不加括号。入口和 IP 是非秘密元数据，但 pin/令牌依旧按秘密凭据原流程保存。

```cpp
ConnectionEndpoint connection_endpoint(const std::string&, uint16_t);
bool is_public(EndpointKind);
std::vector<ConnectionEndpoint> merge_endpoints(
    const std::vector<ConnectionEndpoint>& saved,
    const std::vector<ConnectionEndpoint>& fresh, uint64_t now_unix);
void record_endpoint_results(std::vector<ConnectionEndpoint>&,
                             const std::vector<EndpointProbe>&);
EndpointConnection connect_endpoints(const std::vector<ConnectionEndpoint>&,
    const std::string& pin, std::chrono::milliseconds timeout=std::chrono::seconds(10));
std::vector<ConnectionEndpoint> client_endpoints(Client&); // endpoint_client.hpp
void merge_endpoints(ClientCredentials&, const std::vector<ConnectionEndpoint>&);
std::vector<EndpointProbe> probe_endpoints(const ClientCredentials&,
    std::chrono::milliseconds timeout=std::chrono::seconds(10));
```

客户端凭据在 `windows_client.hpp`，原 `host/pin/token/port` 保留，新增 `endpoints`；原构造聚合用法仍可编译。`connect()` 有列表时执行并发 TLS 竞争，否则保留旧单地址连接。返回的 `Client` 有 `selected_endpoint` 和 `connection_attempts`，用 `is_public(selected_endpoint->kind)` 提示画质。`Client(std::unique_ptr<Tls>, token, scopes, channel, hello_flags)` 可以接管已经固定指纹的胜出连接，HELLO 只发送一次。

`network.hpp` 还提供纯模型辅助函数，供测试或平台适配使用：

```cpp
std::optional<NumericAddress> numeric_address(std::string_view); // 16 bytes + bool v6
bool valid_hostname(std::string_view);
bool valid_connection_host(std::string_view);
void write_endpoints(Writer&, const std::vector<ConnectionEndpoint>&);
std::vector<ConnectionEndpoint> read_endpoints(Reader&);
```

低层 `Tls` 构造在旧三个参数之后增加可选的 steady_clock deadline 和 `std::function<void(bool timed_out)> before_socket_close`；后一回调给连接拥有者在关闭 socket 前清除取消登记，避免句柄重用竞争。`Tls::clear_deadline() noexcept` 在成功 HELLO 后恢复普通 IO 超时，`Tls::receive(uint32_t limit=max_frame)` 可限制首帧。界面通常应使用 Client/凭据 API，不直接管理该回调。

`connect_endpoints` 先 LAN，随后每约 250 ms 启动一个入口；同一域名的多个 DNS 地址也并发错开连接。总超时包含 DNS/TCP/TLS，默认 10 秒、允许 1 ms..60 s；先完成固定证书 TLS 的入口胜出，其他连接被取消并回收。设计参考 [RFC 8305](https://www.rfc-editor.org/rfc/rfc8305.html)，不是完整的 RFC 排序/网络历史数据库实现。

失败抛 `EndpointConnectError`，`attempts` 中每项是 `EndpointProbe {endpoint, failure, elapsed_ms, start_ms, tcp_ok, tls_ok, rtt_ms}`。failure 为 none/dns/timeout/refused/pin_mismatch/transport/cancelled；已知 pin mismatch 是直接终止错误。`EndpointConnection` 返回 `tls, selected, attempts, public_path`。裸入口版 `probe_endpoints(vector, pin, timeout)` 仅探测 TCP/TLS，不发送令牌；凭据版还做 scopes=0 的 HELLO 和 ping，`rtt_ms` 是实际 ping 往返，不用 TLS 总耗时冒充 RTT。不可用的 RTT 为空。

`client_endpoints` 先查询 CAPS，无 `endpoints=v1` 返回空、绝不发送 Op56。界面在成功连接后显式调用：记录这次失败结果 → 查询新入口 → 合并 → 保存 DPAPI。失败次数不会由 const `connect()` 偷改原凭据；失败异常也要把 attempts 交给 `record_endpoint_results`。合并按规范 host+port 去重，最多 8 项，地址连续失败 3 次或已过期即丢弃；取消/指纹错误不计地址故障，成功清零，域名不因故障/TTL 被丢弃，裁剪时优先淘汰数字地址。最多 8 个不同域名的容量边界仍适用。

## 邀请及链接 API

```cpp
PairingInvite HostAdministration::invite(const std::string& label, uint32_t scopes,
    std::chrono::seconds ttl=std::chrono::minutes(10), bool allow_remote=false);
TrustedDevice HostAdministration::redeem_invite(const std::string& code,
    const std::string& device_label, bool local_source=true); // backend, not a remote admin API
std::string pairing_uri(const PairingInvite&, const std::vector<std::string>&, uint16_t port);
std::string pairing_uri_endpoints(const PairingInvite&, const std::vector<ConnectionEndpoint>&, uint16_t default_port);
std::optional<PairingLink> parse_pairing_uri(std::string_view) noexcept;
PairResult pair_with_link(const PairingLink&, const std::string& device_label);
```

`PendingInvite/PairingInvite` 新增 `allow_remote`。`PairingLink` 保留 `addresses/port`，增加带各自端口的 `endpoints`；新代码优先用 endpoints。`PairResult` 保留旧字段，增加完整 endpoints；成功的 host/port 代表实际胜出入口。旧错误类型及不能自动重试 outcome_unknown 的约定保持不变。生产服务从 socket 判断来源，只有本地单测才应注入 redeem 的布尔来源参数。

v2 示意：`a=192.0.2.2,[2001:db8::1],example.com:443&p=47443`。同一链接能同时携带监听端口和路由器翻译端口。完整格式、七参数白名单、上限、v1 回退见 PROTOCOL。生成二维码仍使用原 `qr_encode`，级别 M，无 UI 绘制变化。

## Java API（同包 UI 可直接调用）

新增 `ConnectionEndpoint.java`、`NetworkDialer.java`，修改 `PairingLink.java`、`Remote.java`：

```java
PairingLink.parse(String); // v1/v2，失败 null；新增 endpoints 列表
Remote.pair(PairingLink, String deviceLabel); // Credentials，保留所有入口
new Remote.Credentials(host, port, pin, token); // 原行为
new Remote.Credentials(host, port, pin, token, List<ConnectionEndpoint> endpoints);
credentials.mergeEndpoints(List<ConnectionEndpoint> fresh); // 新不可变 Credentials
credentials.saveEndpointPreferences(); // String，存 SharedPreferences 的独立 key
credentials.loadEndpointPreferences(String); // 新 Credentials，令牌/pin/原 host 不变
ConnectionEndpoint.savePreferences(List<ConnectionEndpoint>);
ConnectionEndpoint.loadPreferences(String); // 畸形输入 -> 空列表
ConnectionEndpoint.recordResults(saved, reports); // 新列表，计故障并清理
remote.endpoints(); // CAPS 门控；旧主机 -> 空列表
remote.selectedEndpoint; remote.publicPath; remote.connectionAttempts;
Remote.probeEndpoints(credentials); // List<NetworkDialer.Probe>，含实际 ping RTT
```

`ConnectionEndpoint` 的 kind 常量、字段和规范化规则与 C++ 一致。原构造器和 `asDeliberate()` 保持语义，后者保留新入口。SharedPreferences key 请按 pin/设备隔离，建议 `endpoints_v1`；原 Keystore、主机/端口偏好和 UI 页不在本次后端中修改。入口更新后仍要保存新 Credentials，否则下次页面重建会丢列表。

`NetworkDialer.ConnectException.reason` 为 DNS/TIMEOUT/REFUSED/PIN_MISMATCH/TRANSPORT/CANCELLED，`attempts` 逐入口返回结果。线程池有上限；系统 DNS 在部分 Android 版本不能真正打断，但调用方到达总截止时间即返回并关闭其他 socket，滞留 DNS 不会创建无限线程。取消沿用 `Remote.SocketReady`，传回一个可关闭的连接组，关闭会取消所有未完成连接和胜出连接；不在 UI 线程做 TLS close_notify。

## 防火墙与 UPnP

```cpp
FirewallState public_firewall_state(const fs::path& exe,uint16_t port) noexcept;
uint32_t firewall_current_profiles() noexcept;
void firewall_public_allow(const fs::path& exe,uint16_t port); // elevated CLI
void firewall_public_remove(const fs::path& exe);             // elevated CLI
bool request_public_firewall(const fs::path& exe,uint16_t port,bool allow) noexcept;
UpnpReport Host::upnp_map(bool ipv4=true,bool ipv6=true);
UpnpReport Host::upnp_unmap();
UpnpReport Host::upnp_status() const;
```

FirewallState 仍是 allowed/not_allowed/unknown，判断的是 Private 配置，不等于当前网络能通过。公网规则绑定程序 + TCP 端口，远程任意地址；不放行 Public、不加 UDP，UAC 拒绝返回 false。禁用/删除规则不能假装已经成功。

`upnp.hpp` 另有可独立使用的 `UpnpIgd`（discover/map/renew/unmap/status/remove）和 `UpnpLease`（map/status/unmap/stop）。`UpnpRequest` 指定本机 IPv4、稳定公网 IPv6、内外端口、2..86400 秒租期（默认 1800）；Host 会从非虚拟、稳定的已连接地址选择。`SsdpTarget` 默认 IPv4 LAN SSDP 组播，仅用于显式发现；测试可注入 loopback 模拟 IGD。IPv6 pinhole 使用发现的 IGDv2 `WANIPv6FirewallControl` 服务，控制面目前走 IPv4 LAN；只提供 IPv6 SSDP/只允许 IPv6 控制源的路由器可能不支持或拒绝，需要用户手动放行。此限制不影响已手动放行的公网 IPv6 直连。

```cpp
explicit UpnpIgd(SsdpTarget={});
UpnpStatus UpnpIgd::discover();
UpnpReport UpnpIgd::map(const UpnpRequest&);
UpnpReport UpnpIgd::renew();
UpnpReport UpnpIgd::unmap();
UpnpReport UpnpIgd::status() const;
UpnpReport UpnpIgd::remove(uint16_t external_port,
                         std::optional<uint32_t> pinhole_id={});
explicit UpnpLease(SsdpTarget={});
UpnpReport UpnpLease::map(const UpnpRequest&);
UpnpReport UpnpLease::status() const;
UpnpReport UpnpLease::unmap();
void UpnpLease::stop();
```

`UpnpReport` 分 ipv4/ipv6，各含 status、external_address/port、lease_seconds、expires_at；IPv6 还返回可选 pinhole_id，CLI 会显示它，供崩溃后显式清理。UpnpStatus：idle/no_igd/unsupported/denied/success/cgnat/network_error/expired。IPv4 的 GetExternalIPAddress 是私有或 100.64/10 时返回 cgnat，不尝试 AddPortMapping。IPv6 完成 AddPinhole、UpdatePinhole、DeletePinhole；IPv4 使用 AddPortMapping 刷新租约和 DeletePortMapping。Host 运行中按半租期续期，正常停止/关闭公网时删除。异常退出依靠租期到期；不能承诺路由器一定遵守租期。

地址指纹变化后，如果用户此前启用了自动映射，前端后台可重新调用 `Host::upnp_map()`，它会先删除旧租约再选择新的稳定地址。低层续约对象保留创建时的 InternalClient，不能把旧地址续约视为新 IPv6 前缀已自动放行。

低层 `UpnpIgd` 由调用者负责 unmap；Host 使用的 `UpnpLease` 在 stop/析构时删除。CLI crash cleanup 的 `remove(external_port, optional_pinhole_id)` 是用户明确提供标识的删除操作；不扫描或批量删除别的程序规则。真实路由器之间的错误码、租期上限和权限要求不同，界面允许显示“请在路由器手动放行 TCP 端口”，不要把 unsupported 当作没有公网 IPv6。

## 错误提示建议

| 结果 | 建议提示 |
|---|---|
| legacy_enabled | 先禁用旧共享凭据；已配对设备独立凭据保留 |
| IPv4 fallback | 当前未能启用 IPv6，正在使用 IPv4；家庭宽带若仅有公网 IPv6则外网不可用 |
| 无稳定 GUA | 暂未发现稳定公网 IPv6，等待网络或检查路由器 IPv6 设置 |
| CGNAT | 没有可直接映射的公网 IPv4，可优先尝试 IPv6 |
| Public profile | 当前网络被 Windows 识别为公用，现有规则不在此网络放行；请先确认是否可信 |
| firewall unknown / UAC false | 无法确认/未获授权，不能显示“已放行” |
| no_igd / unsupported / denied | 路由器未发现/不支持/拒绝自动配置，可手动放行 TCP |
| DNS / timeout / refused | 域名解析失败 / 地址未响应 / 端口未监听，提供逐入口诊断 |
| pin mismatch | 电脑身份不匹配，停止连接并重新核对指纹，不能提供“忽略证书”按钮 |
| busy | 来源尝试过于频繁或会话名额已满，稍后重试 |
| invalid_invite | 邀请无效、过期或不允许当前公网来源，请在电脑刷新或选择远程兑换 |
| outcome_unknown | 兑换结果无法确认，检查电脑设备列表，再生成新邀请 |
| readiness 全部满足 | 本机侧准备完成，仍需在手机移动网络执行端到端诊断 |

## 命令行

```text
endpoints STATE [PORT=47443]
internet STATE on|off [HOSTNAME] [PUBLIC-PORT]
invite STATE LABEL SCOPES [MINUTES=10] [allow-remote]
client-probe HOST PORT PIN [OTHER-HOST...]   # 仅 TCP/TLS，无令牌
firewall-public-allow EXE PORT
firewall-public-remove EXE PORT
upnp status                                # 只读发现
upnp map STATE [PORT=47443]                 # 同时运行主机，Ctrl+C 停止并删租约
upnp unmap EXTERNAL-PORT [PINHOLE-ID]       # 明确的崩溃后清理
```

CLI 的数字结果对应上述枚举顺序，从 0 开始。`pair` 继续按原命令输出 DPAPI 包装令牌，与旧 `secret` 文件兼容；界面要保存完整 PairResult/Java Credentials 的入口列表，不能只沿用 CLI token 文件当作完整设备配置。

## 验证与边界

Windows Visual Studio 2022 x64 Release 实际运行 `build.ps1 -BuildDir build`：**20/20 CTest 通过，99.94 秒**。随后补充 Java 主动取消与超时的类型区分，相关两套 Java 定向回归再次通过（14.60 秒）。本机 core 测试本次也通过，没有绕过应用控制。最终 Android 47 个 main/debug Java 源文件使用 JDK 17 + Android SDK 35 + 项目已有 ZXing 类库全部编译通过；没有重打/安装 APK，也没有把桌面 JVM 互通说成手机实测。

| 检查 | 实测结果 |
|---|---|
| 公网后端综合 | 83 项，含强制 ::1 TLS、双栈映射源规范化、IPv6 绑定冲突后的 IPv4 回退、旧主机能力门控、v1..v3 迁移、多入口竞争/总截止时间、pin 硬失败、DPAPI、首帧/慢 TLS 截止时间、来源限额、HELLO 退避与成功清零、ping RTT |
| 配对链接/QR | C++ 52 项，Java 50 项，v1/v2、IPv6/域名、每项端口、异常输入、偏好保留 |
| UPnP | 本机模拟 SSDP + HTTP/SOAP 13 项，IPv4/IPv6 创建、续期、删除、CGNAT、拒绝、不支持、外部控制 URL 拒绝；没有写实际路由器 |
| 防火墙 | 27 项，仅未挂载 COM 规则配置和只读系统查询；没有调用系统 Add/Remove |
| Java/JNI 互通 | 桌面 JVM 的 23 项 Java 断言及 93 项 C++ 集成断言，包含真实 Schannel/JSSE TLS、多入口竞争/主动取消和 ping；普通集成另有 92 项 |
| IPv4 发现/旧客户端 | 原有 discovery、device_trust、Windows 文件/任务/剪贴板等回归套件通过 |
| 本机真实 IPv6 | 只读枚举得到 3 个未弃用稳定 GUA、2 个可能临时 GUA；未将实际地址写入仓库 |

未测：手机移动网络公网端到端、真实公网 IPv4（家庭网络为 CGNAT）、真实路由器写入/租约兼容性、管理员防火墙增删/UAC 交互、彻底没有 IPv6 栈的系统。IPv4 和路由器路径使用本机模拟；IPv6 绑定失败回退已通过受控端口冲突验证，无系统策略修改。原集成测试的符号链接创建因 Windows 1314 跳过，junction/hardlink 检查仍执行；没有新增跳过 IPv6 的路径。

未改变系统应用控制、防火墙策略、网卡或实际路由器配置。测试输出/本机路径/实际公网 IPv6 只留在本机工作目录，不进入提交。手机虽已可通过 USB 调试，跨网验证仍应等前端接入及用户明确放行系统/路由器策略后进行，不能通过 USB 隧道冒充公网可达。

历史 ::1 的 Windows 10013 本轮未复现：只读检查未发现 IPv6 TCP 端口排除，系统动态端口 IPv6 回环 TCP 及实际 Schannel TLS 均可用。没有证据可以把历史错误归因于某一策略或驱动；不凭空声称已修复系统故障。本次删除了测试中的跳过逻辑，IPv6 TLS 变为必过项，后续出现错误会直接失败。

## 前端接入状态（Claude，分支 `feature/public-network-frontend`）

- **已接入**：上面“前端接入顺序”的 1–7 条都已接入。界面细节见 [WINDOWS-UI.md](WINDOWS-UI.md) 的“公网访问页”和 [ANDROID-UI.md](ANDROID-UI.md) 的“公网连接”。
- **与上文不同的一处**：只限局域网的二维码只放局域网 IPv4，并使用 v1 链接，这样 v0.2 手机也能扫码配对。公网入口在设备连上后通过 Op56 获取。
- **公网端到端尚未成立**：真机用移动数据连公网 IPv6，两个入口都返回 REFUSED。电脑防火墙已允许该程序，所以最可能是家里路由器拒绝了入站。路由器放行之前，这一项不算通过。
- **本机测试环境的限制**：Claude 所在的进程环境里，出站 IPv6 TCP（包括 `::1`）全部返回 10013，所以 `public_network` 和 `host_admin` 在这里失败，GPT 的环境里两者都通过。`core` 失败是因为应用控制拦截了未签名的插件 DLL。
