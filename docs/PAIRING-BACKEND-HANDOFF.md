# Pairing / LAN backend handoff for the frontend engineer

## Scope and integration status

This branch implements backend APIs only. `src/gui.cpp`, `src/host_gui.cpp`, `src/ui_kit.hpp`, Android page/activity/string/style files, and `AndroidManifest.xml` are untouched. QR drawing, camera scanning, deep links, confirmation, countdown, saving UI preferences and firewall buttons remain frontend work.

Host's default constructor arguments and CLI now bind `0.0.0.0:47443`. The existing host UI explicitly supplies its own saved/default address, so it still needs to adopt `default_host_bind` for **new** settings. Preserve existing user choices, including explicit loopback. No automatic listening, UAC or firewall changes occur at startup.

## Public C++ interfaces

### Invitation administration — `include/lantern/host_admin.hpp`

```cpp
PairingInvite HostAdministration::invite(
    const std::string& label, uint32_t scopes,
    std::chrono::seconds ttl = std::chrono::minutes(10));
std::vector<PendingInvite> HostAdministration::invites() const;
void HostAdministration::cancel_invite(const std::string& invite_id);
TrustedDevice HostAdministration::redeem_invite(
    const std::string& code, const std::string& device_label);
```

`redeem_invite` is for the TLS router/tests, not for normal UI calls. It does not itself apply the network source-IP gate; that gate is in Host's PAIR first-frame path.

Models in `include/lantern/pairing.hpp`:

```cpp
struct PendingInvite {
    std::string id, label;
    uint32_t scopes;
    uint64_t expires_at;       // UTC Unix seconds
    uint32_t failed_attempts;
};
struct PairingInvite : PendingInvite {
    std::string host_name, pin, code;
};
struct PairingLink {
    std::string host_name;
    std::vector<std::string> addresses;
    uint16_t port;
    std::string pin, code;
    uint64_t expires_at;
};
```

Invitation label and enrolled device label are distinct: the first labels the local pending invitation; the second is provided by the scanning client and becomes the device's name. Both are 1..64 UTF-8 bytes with no ASCII controls. TTL accepts 1..86400 seconds. Scopes must be nonzero and within the current host ceiling; enrollment also intersects with the ceiling at redemption time. Limits: 32 pending invitations, 64 trusted devices. Errors use existing `Error.code` (`invalid`, `unauthorized`, `busy`, `io`).

Only `invite()` returns the plaintext code. Pending-list entries never contain it, and it cannot be recovered after closing/restarting the dialog. When reopening, cancel the prior invitation and generate a fresh one. Cancel is idempotent for a valid invitation ID. Failed attempts, expiry cleanup and consumption are persisted using `edit_config`. Expiry cleanup is lazy on invitation operations; expiration is always checked before enrollment. Maintain the countdown locally from `expires_at`; refresh the pending list on relevant state changes rather than doing DPAPI writes every animation tick.

All APIs doing config, network, COM or elevation work are blocking: call them on a background worker and marshal UI results back. Clear QR/code memory when closing the pairing UI; never write URI/code/token to logs, preferences, telemetry, exception dialogs or clipboard automatically.

### Link and QR — `include/lantern/pairing.hpp`

```cpp
std::string pairing_uri(const PairingInvite&,
                        const std::vector<std::string>& addresses, uint16_t port);
std::optional<PairingLink> parse_pairing_uri(std::string_view) noexcept;
struct QrMatrix { int size; std::vector<bool> modules; };
QrMatrix qr_encode(std::string_view text);
```

`parse_pairing_uri` returns `nullopt` for all malformed/expired inputs. `pairing_uri` and QR encoding throw sanitized `Error(Code::invalid, ...)` on invalid data or capacity overflow. QR is exact M correction, automatically selected version 1..40. `modules[y * size + x]` is true for a dark module. Add a **four-module light quiet zone** on every side, render square pixels/modules without smoothing, and use integer scaling where possible. No quiet zone is included in the returned matrix.

Typical host flow:

```cpp
auto all = lantern::lan_addresses();
std::vector<std::string> addresses;
for (const auto& a : all)
    if (!a.virtual_adapter && addresses.size() < 8)
        addresses.push_back(a.address);
// If empty, let the user explicitly select an available adapter; do not encode 0.0.0.0.
auto invite = admin.invite("Android phone", lantern::files_read);
auto uri = lantern::pairing_uri(invite, addresses, actual_listening_port);
auto matrix = lantern::qr_encode(uri);
// Render, display expiry, and retain invite.id for cancel/refresh.
```

Use the host's actual bound adapter/port. If bound to a specific IPv4, include that address only. Use the adapter list for wildcard binding. If the host is stopped or bound only to loopback, guide the user to start it on a reachable address before displaying a production QR. Adapter virtuality is a name/type heuristic and not a guarantee of physical hardware or reachability.

### Windows client — `include/lantern/pairing_client.hpp`

```cpp
struct PairResult {
    std::string host;
    uint16_t port;
    std::string pin, device_id, token;
    uint32_t scopes;
};
PairResult pair_with_link(const PairingLink&, const std::string& device_label);
struct PairingError : Error { PairError reason; };
enum class PairError {
    invalid_link, host_too_old, invalid_invite, unreachable,
    pin_mismatch, busy, protocol_error, outcome_unknown
};
```

Addresses are tried in order; connect/TLS failures may move to the next address, and the code is never sent before pin verification. A definitive PAIR response stops failover. After the request is sent, a lost response is `outcome_unknown`; automatic retries could consume/create state twice and are intentionally avoided. `unreachable` means none connected; if any candidate failed pin validation and no candidate succeeded, `pin_mismatch` wins.

On success create `ClientCredentials{r.host, r.pin, r.token, r.port}` and call existing `save_credentials()` from `windows_client.hpp` if the user chooses to remember it. Normal subsequent connection uses unchanged HELLO. The helper itself does not change GUI state or save credentials.

### Windows LAN/firewall — `include/lantern/lan.hpp`

```cpp
inline constexpr const char* default_host_bind = "0.0.0.0";
enum class LanType { wifi, ethernet, other };
struct LanAddress {
    std::string address, adapter_name;
    LanType type;
    bool virtual_adapter;
};
std::vector<LanAddress> lan_addresses();
enum class FirewallState { allowed, not_allowed, unknown };
FirewallState firewall_state(const fs::path& exe, uint16_t port) noexcept;
void firewall_allow(const fs::path& exe, uint16_t port);
bool request_firewall_allow(const fs::path& exe, uint16_t port) noexcept;
```

`lan_addresses` uses connected/preferred IPv4 unicast addresses, excludes loopback/APIPA/unspecified/multicast, and places likely physical adapters first. The type is separate from virtuality. It may throw `Error(Code::io)` and may return an empty list.

`firewall_state` queries **Private-profile TCP** rules by effective program/port match, including disabled/outbound/profile/protocol checks, block precedence and conservative `unknown` for constraints or unavailable policy. It is not an end-to-end connectivity test, current-network-category check, UDP discovery check or third-party-firewall audit. Private firewall disabled means `allowed` under that profile.

`request_firewall_allow` locates sibling `lantern.exe` next to the calling program and starts `firewall-allow` with `runas`, hidden console, waiting up to 60 seconds. Pass the **host process executable** (`lantern-host.exe` for GUI host, `lantern.exe` for CLI host), not the client executable. A launcher-installed host must have the CLI helper alongside it. UAC refusal, missing helper, timeout or nonzero exit returns false. Do not call automatically; attach to a user action and run off the UI thread.

The elevated command upserts two deterministic rule names: `Lantern 主机` for the executable + TCP port, and `Lantern 主机（发现）` for the executable + UDP47442. TCP and UDP require separate rules. Both are inbound/Private only, LocalSubnet source, edge traversal off; Public/Domain profiles are never enabled. Updating the helper does not change the network's category. If one rule update fails after another succeeds, the command reports failure; repeating is idempotent and completes the pair. Rule names represent one selected Lantern host path/port per machine; moving installs requires calling again.

## Java APIs

`android/app/src/main/java/org/lantern/client/PairingLink.java` is a public immutable model with:

```java
public final String hostName, pin, code;
public final List<String> addresses; // unmodifiable
public final int port;
public final long expiresAt;
public static PairingLink parse(String uri); // null for malformed/expired input
```

In the existing `org.lantern.client` package, `Remote.java` adds:

```java
static Credentials pair(PairingLink link, String deviceLabel)
    throws Remote.PairingException;
// error.reason is Remote.PairFailure:
// INVALID_LINK, HOST_TOO_OLD, INVALID_INVITE, UNREACHABLE,
// PIN_MISMATCH, BUSY, PROTOCOL_ERROR, OUTCOME_UNKNOWN
```

This uses SSLSocket with the pinned certificate, does not need a camera/UI library, and does not alter existing `Credentials` or HELLO constructor behavior. `Credentials` retains existing host/port/pin/token fields; ID/scopes from the wire are validated but not added to that existing type. Call `Secrets.save(context, credentials.token)` through the existing successful-connection path and persist address/fingerprint as before. No credential is automatically written by `Remote.pair`.

## URI contract and example

```text
lantern://pair?v=1&n=Desktop%20PC&a=192.168.1.20,10.0.0.20&p=47443&f=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&c=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb&x=4102444800
```

The repeated hex values above are **nonfunctional examples**, not credentials. Exactly seven fields, one occurrence each, at most eight unique canonical IPv4 addresses and 2048 total bytes; no DNS/IPv6/fragment/unknown fields. Host name is percent-encoded UTF-8, 1..64 bytes. Decimal port is 1..65535; expiry is UTC Unix seconds and must be in the future (upper bound 2100-01-01). The 64-hex pin is normalized to lowercase; the 64-hex code is lowercase. `+` is not decoded as a space. C++ and Java reject malformed percent/UTF-8. Device clock correctness matters.

## Suggested UI errors

| Reason | Suggested message/action |
| --- | --- |
| invalid_link / INVALID_LINK | 二维码无效或已过期，请在电脑刷新；时钟错误时检查系统时间 |
| host_too_old / HOST_TOO_OLD | 电脑端版本过旧，请更新后重新生成二维码 |
| invalid_invite / INVALID_INVITE | 配对码无效、已使用或已过期，请刷新二维码 |
| unreachable / UNREACHABLE | 所有地址均无法连接；检查同一 LAN、主机是否启动、绑定地址和防火墙 |
| pin_mismatch / PIN_MISMATCH | 电脑身份与二维码不一致；停止配对并重新核对电脑上的二维码 |
| busy / BUSY | 尝试过于频繁，请一分钟后重试；若持续出现，检查电脑设备容量 |
| protocol_error / PROTOCOL_ERROR | 配对响应无效，请检查两端版本，不保存凭据 |
| outcome_unknown / OUTCOME_UNKNOWN | 响应中断，结果无法确认；查看电脑设备列表，撤销多余条目后重新生成邀请 |

Historical hosts in this repository can silently close a non-HELLO first frame instead of sending unsupported. No client can reliably distinguish that from a response lost after consumption. Structured unsupported maps to `host_too_old`; silent closure maps to `outcome_unknown`. Do not claim that a timeout definitively means an old host.

## CLI

```text
lantern.exe invite STATE LABEL SCOPES [MINUTES]
lantern.exe pair LINK DEVICE-LABEL SECRET-FILE
lantern.exe firewall-allow EXE PORT
```

`invite` prints the URI once (explicit local output, never ambient logging), default 10 minutes/port47443, using current LAN candidates. For a nondefault listener port, the frontend calls `pairing_uri` with that actual port. `pair` writes only the returned token as DPAPI bytes, matching the existing `secret` / `device-secret` file format; use normal CLI `client HOST PORT PIN SECRET-FILE ...` afterward. `firewall-allow` requires elevation, and the UI helper performs that request. Quote the URI as one argument because it contains ampersands.

## Compatibility, validation and remaining checks

Config v3 reads v1/v2 and preserves identities/trust/legacy settings. Old clients continue to use the unchanged HELLO path against the new host. Upgrade the host and host-manager binaries together: older executables cannot read config v3. Invitations are shared through the atomic DPAPI config transaction across running local processes; the network IP limiter is per Host instance and in-memory.

Tests cover invitation generation/expiry/single use/cancellation/five wrong secrets, absence of plaintext even after DPAPI decryption, v2 migration and concurrent redemption; real Schannel TLS PAIR, follow-up HELLO, pin failure, address failover, version rejection, IP limit and existing HELLO; C++/Java malformed-link parity, QR dimensions/version/capacity; Java SSLSocket enrollment against the actual C++ host; read-only firewall query and in-memory unattached firewall-rule configuration. Final execution results are recorded below before delivery.

Not tested here: real QR camera scanning/painting/deep links (frontend scope), two physical devices on Wi-Fi with the new API, elevated addition to live OS firewall policy and UAC cancellation. Do not change security policy to make tests pass. Tests use generated fixtures and do not require personal share roots or stored credentials.

### Final execution on Windows (2026-10-15)

- Visual Studio 2022 / MSVC Release configuration and build succeeded for the host, CLI, client and all test targets.
- Final standard `build.ps1 -BuildDir build` run: **18/18 CTest suites passed, 0 failed, 87.16 seconds**. JDK17 interop tests were enabled in the CMake cache. The `core` plugin test and `file_features` both ran and passed on this run; no system security policies were disabled or bypassed.
- Existing subchecks still explicitly skipped IPv6 `::1` TLS because the OS returned Windows10013, and symbolic-link creation because the account lacked that privilege (Windows1314); junction/hardlink tests did run. These platform skips are not claimed as validated IPv6/symlink behavior.
- Android main + debug Java source set compiled against Android SDK35 using JDK17 (`javac --release 17`). No APK was rebuilt or installed; this is compilation and JVM/SSLSocket interop validation, not Android camera/device validation.
- The real TLS legacy-response fixture verified that `HELLO/unsupported` from an old-style host becomes `host_too_old`. Native and Java enrollment succeeded against the actual host; the existing HELLO/JNI/file-transfer checks also passed.
- Firewall tests configure/read an unattached COM rule (never `INetFwRules::Add`) and query live OS policy read-only. They verify repeat configuration, TCP/UDP settings, Private-only profile, local-subnet source and no edge traversal. Live elevated rule upsert and a user's UAC rejection remain manual checks.
- Initial complete runs exposed existing test-harness issues: a long checkout produced a 260-character resume filename, a transfer test raced active-worker cleanup, and an old connection could observe asynchronous revocation as a socket close. Added configurable `LANTERN_TEST_ROOT`, waited for cleanup in the test, and checked both valid revocation outcomes plus rejection of a fresh HELLO using the revoked token. No production transfer or HELLO behavior was modified for those fixes. Final tests used a shorter generated-fixture root.

For a deep Windows checkout, configure a short fixture directory once before running the normal script (example paths are placeholders, not machine state):

```powershell
cmake -S . -B build -DLANTERN_TEST_ROOT=C:/Temp/Lantern-tests -DLANTERN_JNI_TEST_JDK=C:/Tools/jdk-17
.\build.ps1 -BuildDir build
```

Do not commit that cache or generated fixtures. Tests still accept normal explicit fixture paths, and `LANTERN_TEST_ROOT` defaults to the build directory for existing workflows. Re-running only failed suites while diagnosing does not replace the final complete 18-suite run above.
