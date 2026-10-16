> 2026-10-15 更新：首次配置、桌面工具、目录恢复、SAF直传、发现、调度与音画共同时间轴已新增；当前结果、限制与版本兼容以 [本批报告](CONTINUATION-DELIVERY.md) 为准。下文较早的验证数字是历史记录。

# Lantern wire protocol v1

此文档描述当前代码，未实现的音视频协议不占用伪造能力位。传输是 TLS 1.2（Windows Schannel / Android SSLSocket），证书通过完整 DER SHA-256 固定。证书不匹配或过期，在发送配对密钥前失败。

## 线格式

大端序；UTF-8；无压缩。每个 TLS application frame：

```
u32 payload_length                 // 4..4,194,304
u16 protocol_version = 1
u16 operation
... body
```

blob/string：`u32 byte_length; byte[byte_length]`。普通字符串最大 65,536 字节，密钥最大 128，文件分块最大 262,144。解析必须消费全部输入；多余字段失败，版本 1 不支持忽略任意扩展字段。版本不兼容明确拒绝，下一大版本通过新 HELLO 协商后采用新 schema。

响应头为 `u16 version,u16 operation,u32 error`，成功后接该操作的结果；失败接 UTF-8 错误字符串。握手/HELLO 错误可能直接关闭连接；客户端必须处理这一失败，不显示已配对。普通 RPC 连接一次最多一个请求，因此当前版本无需请求 ID。硬件桌面使用专用连接及以下 stream-v1 扩展，旧客户端可继续使用 op21。异步多路复用将在下一版本加入 stream/request ID，不改变 v1 的顺序语义。

错误：0 ok、1 invalid、2 unauthorized、3 not_found、4 conflict、5 io、6 unsupported、7 cancelled、8 busy、9 integrity、10 source_changed。客户端不得对写入失败无限自动重试；恢复从服务确认的 offset 开始。

## HELLO 和权限

普通会话第一帧为 op=1：`string pairing_secret,u32 requested_scopes,u32 channel`；一次性邀请兑换允许以下 op=55，完成后关闭连接。
响应 `u32 granted_scopes`。请求权限必须为配置允许集合的子集；没有自动授予所有权限。

权限位：read=1/write=2/view=4/control=8/audio=16。channel 0=input/control、1=desktop、2=original media、3=bulk、4=system audio。不同 channel 使用不同 TLS socket，文件/媒体共享主机级 Budget。channel不能赋予额外权限；desktop/audio持续流强制独立通道。requested_scopes=0用于仅验证身份与查询能力，没有任何文件/观看/输入/音频权限。

普通后续操作重新读取加密配置，验证密钥未被撤销，并再次取权限交集；持续硬件桌面/音频每 250 ms 复查授权和密钥。主机级密钥是首版限制，尚无逐设备公钥、刷新令牌、过期凭据或互相独立的撤销列表。

## 实际操作表

| ID | 请求体 | 成功结果 |
|---|---|---|
| 1 HELLO | secret,scopes,channel | granted scopes |
| 55 PAIR | string invite_code,string device_label | string device_id,string device_token,u32 effective_scopes；随后关闭 |
| 2 LIST | string path,u32 offset,u32 limit,string filter | string generation,u32 count; 每项 string name,u32 directory,u64 size,u64 mtime |
| 3 STAT | string path | u64 size,string version,string sha256 |
| 4 READ | string path,string version,u64 offset,u32 length | blob bytes |
| 5 UPLOAD_BEGIN | string path,u64 size,string sha256 | string task_id,u64 durable_offset |
| 6 UPLOAD_CHUNK | string task_id,u64 offset,blob bytes | u64 durable_offset |
| 7 UPLOAD_COMMIT | string task_id | 空 |
| 8 MKDIR | string path,string empty | 空 |
| 9 RENAME/MOVE | string path,string destination | 空 |
| 10 COPY | string path,string destination | 空 |
| 11 REMOVE | string path,string empty | 空 |
| 20 MONITORS | 空 | u32 count; 每项 i32 x,i32 y,u32 width,u32 height,string name |
| 21 FRAME | u32 display,u32 target_width,u32 quality | blob JPEG |
| 22 INPUT | u32 kind,i32 x,i32 y,u32 value,u32 display | 空 |
| 30 CAPS | 空 | string UTF-8 capabilities |
| 35 CLICK | u32 normalized_x,u32 normalized_y,u32 display | 空；control权限；一次SendInput批次移动/按下/松开 |
| 36 POINTER_GESTURE | u32 phase,x,y,display | 空；phase1开始、2移动/心跳、3移动并结束、4取消；独立input通道 |
| 37 DOUBLE_CLICK | u32 normalized_x,normalized_y,display | 空；一次批次移动和两对左键按放 |
| 38 TEXT_INPUT | string UTF-8 text | 空；4096字节/1024 UTF-16单位上限；禁止控制字符；Unicode按放批次 |
| 39 KEY_PRESS | u32 Windows_VK,count | 空；允许退格/回车/Tab/Esc/Delete/方向/Home/End，count1..32，成对按放 |
| 40 INPUT_FOCUS | 空 | u32 flags：1已知可编辑标准窗口、2密码；不包含任何输入内容 |
| 31 MEDIA_STAT | string path | u64 size,string version,string empty_hash |

CAPS 当前是描述性 capability 字符串，尚非完整编码/字幕/HDR 格式协商。MEDIA_STAT 不扫描整个媒体文件的哈希，保证原始媒体开始读取不被大文件全盘扫描阻塞。原始媒体随机读取用 READ，要求稳定版本。端到端 TLS 负责传输完整性；文件任务额外做完整 SHA-256。

Android新控制先查询CAPS的 `gesture-v1,unicode-ime-v1`，没有能力的旧主机会提示更新，而不默默调用未知操作。op35..40保留版本1封装并以能力字符串协商；严格验证长度和坐标0..65535；旧Windows客户端仍可用op22。36阶段4忽略坐标但仍严格解析消息长度。

旧式op22 kind：0=移动（x/y 0..65535 映射目标显示器）、1=按钮（value 0左下/1左上/2右下/3右上）、2=键（value=Windows VK 0..255，x 0按/1释放）、3=滚轮（x 范围 -1200..1200）。安全桌面、UIPI 和会话隔离拒绝时返回 unsupported。新式鼠标租约只属于建立它的连接，1800ms过期，取消/断线/协议错误释放；持有期间其他输入被拒绝。完整旧式按键/修饰键租约尚待实现。

## H.264 桌面 stream-v1 扩展（已实现）

专用 channel=1，必须独立授予 view；控制仍走另一个 channel。最多两路硬件流，不占用所有工作线程。

1. op32 请求：`u32 stream_version=1,monitor,target_width,fps,bitrate`。支持宽 640–1920、30/60 fps、2–30 Mbps；拒绝截断、尾随字段、错误版本、越权和错误通道。
2. op32 成功响应：`u32 stream_version,width,height,fps,bitrate; string capture_provider; string encoder_provider; blob AnnexB_SPS_PPS`。当前实现为 dxgi-d3d11 / nvidia-nvenc-h264，SDR H264 High 8-bit 4:2:0，无 B 帧。硬件/系统不支持明确返回 unsupported；能力字符串的 optional 不保证初始化成功。
3. 客户端创建 MediaCodec 和 Surface 后发送 op34：`u32 stream_version=1`，无单独响应。主机最多等待 5 秒就绪。
4. 主机持续推 op33，使用标准响应头；成功体：`u64 sequence; u64 pts_us; u32 flags; blob AnnexB_access_unit`。序列从 0 开始，PTS 会话相对、单调；flags bit0=IDR，bit1=复用最近桌面纹理。参数集最多 4096 字节，编码帧小于 4 MiB-256。客户端校验序列、字段、时间戳溢出与尾随内容。
5. 错误 op33 使用标准错误码/字符串并关闭连接。停止订阅关闭连接；服务器释放编码器和纹理。重新开始需重新请求配置，从 IDR 恢复，不拼接旧会话码流。

采集/缩放/编码/发送在拥有提供者的单工作线程中执行；一帧处理、一组固定 GPU 表面和编码输出缓冲，无增长的帧队列。持续发送使用绝对周期预算，落后超过一周期时跳过过期调度。socket 发送缓冲 256 KiB、单次发送超时 300 ms、整轮发送超过 300 ms 则结束；它是 TCP 背压限制，不保证每个丢包场景下都有 300 ms 的严格端到端上界。客户端相对首包检测积压超过 300 ms 后结束并提示 JPEG 降级，不能将此时钟差当作输入光学延迟。

仍为可靠 TLS/TCP，没有过期 datagram 丢弃或跨连接自适应拥塞控制。系统音频使用下述独立流；原始电影/文档依然使用独立文件范围读取，不经过 H264 桌面管线。

## 系统音频 pcm-v1 扩展（已实现）

独立channel=4、audio=16授权，最多2路。op41请求 `u32 audio_version=1`，成功响应 `u32 audio_version,sample_rate,channels=2,bits=16;string provider=wasapi-loopback`。客户端配置成功后op43无响应确认 `u32 audio_version=1`。主机随后op42推送 `u64 sequence,pts_us;u32 flags;blob PCM16LE`。序号从0递增，pts为主机QPC微秒（取采集时间并保持单调），flags1不连续/2无采样idle心跳。每帧最多10ms、PCM长度是4的倍数，idle心跳为空且不能作为播放数据。协议字段大端，PCM采样独立采用小端。

主机捕获积压最多200ms、socket发送超时200ms；客户端约60ms预缓冲、80ms请求缓冲，非阻塞输出并检查取消；相对源时间积压超过250ms结束。它们不是声学延迟测量或共享视频同步时钟。PCM约1.536Mbps@48kHz，不提供音频压缩；内容不再通用压缩。信任/权限撤销、设备失效、格式不兼容返回明确错误，详见SYSTEM-AUDIO。

## 恢复与提交

上传 task_id 是 SHA256(v1,path,size,sha256)，不是认证凭据；每次操作仍需 write 权限。暂存和 metadata 在根目录 `.rdx-private` 内；远程路径、枚举和读取禁止访问该命名空间。chunk offset 必须等于实际暂存长度；每块写完 FlushFileBuffers 后确认。网络确认丢失时重新 BEGIN 查询已落盘 offset。BEGIN 目标存在时 conflict；Windows 客户端可验证已有目标与源完全一致，处理提交确认丢失。

COMMIT 检查大小和完整 SHA，目标不存在才提交；不会覆盖现有原文件。同名竞争返回 conflict，校验失败保留暂存并返回 integrity。暂存、最终文件与元数据都保持同卷，避免跨卷移动被伪装为原子提交。

下载恢复记录绑定 remote path,size,version,sha256；不同版本拒绝继续。最终提交前检验整个 `.part`，发现部分文件被篡改不会发布错误文件。源 version 包含卷、file ID、长度与修改时间。当前路径锁保证单次读取的源禁止写共享；各次请求间变化导致 source_changed。

LIST generation 包含目录 file ID/修改时间；客户端跨页应比较 generation，变化时从第一页刷新。当前 GUI 跨页一致性还没有完整实现，故大目录变化可能要求用户手动刷新。扫描/过滤仅针对当前目录的名称子串，不是递归搜索。

## 插件 ABI v1

`lp_api_v1.size` 和 `abi=0x00010000` 都必须满足；当前 query 严格拒绝不同版本或短结构。提供者字符串生命周期为模块静态生命周期。所有实例为不透明 `lp_handle`，由同一提供者 create/destroy；调用者拥有输入/输出缓冲。小缓冲返回 2 并写需要长度。每个实例调用串行；不同实例由提供者声明是否可并发。C ABI 不跨边界传播 STL 或异常。示例 provider 不执行媒体解码、捕获或网络。


## 2026-10-14 增量协议（消息头版本仍为1）

- Op44 sorted page：u32 API版本1,str path,u32 offset,u32 limit,str filter,u32 sort(0名称/1大小/2时间),u32 descending,str expected_generation。回复u32版本1,str generation,u32 total,u32 count，后接Entry(name,type,size,mtime)。limit<=256，generation空串表示新快照；内容摘要变化返回conflict。
- Op45 search：u32版本1,u32 phase(0开始/1继续/2取消)。开始额外str path,str query；回复u32版本1,u32 done,u64 scanned,u32 count及Entry，最多128项。一个连接一个搜索游标，断开自动销毁。
- Op46 thumbnail：u32版本1,str path,u32 width(32..256)；回复u32版本1,blob JPEG<=256KiB。仅Microsoft WIC JPEG/PNG/BMP；源<=16MiB、<=6400万像素，同时最多2项。
- Op47 host job：u32版本1,u32 kind(0目录/文件复制，1递归删除，2源SHA256),str source,str destination；回复str 64hex ID。复制read+write、删除write、hash read。哈希任务在来源连接结束时取消。
- Op48 poll：u32版本1,str ID；回复u32 TaskState,u64 current,u64 total,u32 error,str message,u32 kind,u64 file_size,str file_version,str SHA256。后3字段只有成功hash任务有意义。
- Op49 cancel：u32版本1,str ID；空回复表示取消请求已登记，不保证提交前一定获胜；以终态为准。ID不代替授权，轮询/取消检查当前作用域与信任代际。
- Op50 ping：u64 nonce原样回复，独立控制通道可用于RTT诊断。
- Op41音频配置新增API版本2：u32 2,u32 preferred_codec(0 PCM/1 Opus)。回复u32 2,u32 sample_rate,u32 channels,u32 bits,str capture_provider,u32 chosen_codec,u32 lookahead_frames。Opus仅48000Hz/stereo/16bit输入、96000bps CBR、10ms。客户端不支持或主机未启用/格式不符时选0。v1请求/回复不变。Op43 readiness仍u32 1；Op42的blob按配置解释为PCM或单Opus包，空闲心跳仍为空blob。

这些新功能需要同版本主机；旧客户端与v1操作继续可用。没有声称新Android客户端可自动使用缺少这些操作的旧主机。


2026-10-14 最新：主机Config-v2支持最多64个独立设备凭据/权限与会话generation、单设备撤销和结束现有连接，TLS协议v1保持兼容，旧Config-v1读取迁移；设备管理为CLI，详见DEVICE-TRUST.md。剩余实现及未验证项以REMAINING.md为准。


## Windows原子输入增量（2026-10-14）

消息头保持v1；主机capabilities新增input=atomic-shortcut-v1,right-click-v1。Op51 shortcut需要control权限，payload为u32 modifiers（Ctrl1/Alt2/Shift4/Win8组合）,u32 virtual_key（1..255，拒绝修饰键本身），空成功回复。主机在一个SendInput批次内按下修饰键和主键，再倒序释放；Ctrl+Alt+Del返回unsupported，不模拟安全桌面。Op52 right_click同样需要control，payload为u32 normalized_x,u32 normalized_y,u32 monitor，归一化0..65535；在一个批次内移动/右键按下/释放，部分发送失败补偿释放。两者与拖拽租约互斥。旧客户端不受影响；Windows新入口要求对应能力的新版主机。

Windows传输journal API-v3可读v1/v2；v2新增directory位，v3在directory之后增加u32 priority(0..2)、u64 rate_limit(0自动或65536..33554432 bytes/s)；凭据及任务以DPAPI加密保存，不属于网络协议。目录manifest-v1绑定任务ID、设备指纹、目的地、方向以及每项路径/版本/类型/大小，最多10000条、32层、1MiB。未完成任务重启恢复为paused，不隐含恢复输入按键或自动启动外部应用。

## 文本剪贴板增量（2026-10-14）

HELLO/frame仍v1；CAPS广告clipboard=text-cas-v1。Op53读取需要control，payload u32 API_version=1，reply u32 version=1,u32 Windows_sequence,u32 text_present(0/1),str UTF8_text(max65536)。非文本返回present=0、空str，不转换图片/文件。Op54写入同样需要control，payload u32 version=1,u32 expected_sequence,str UTF8_text(max65536)，reply u32 version=1,u32 published_sequence。严格消费消息、不接受嵌入NUL/无效UTF-8；OpenClipboard锁内匹配sequence再提交，变化返回conflict保留新内容。关闭剪贴板/销毁临时owner后复核发布文本，再返回序列号，防止Windows额外序列更新造成假冲突。序列号是当前Windows窗口站的32位版本，不是跨重启或密码学身份。

Windows自动同步默认关闭，500ms轮询、代际取消、双端不同内容同时修改会暂停并保留两边；初次启用不复制历史内容。此轮未给Android增加剪贴板UI。

2026-10-14 后续交付：本机Windows主机/授权/连接管理已接通，HostAdministration API-v1及本地无凭据HostSession接口，不增加网络管理员协议。安卓真Wi-Fi补测授权12项、文件/外部打开37项通过；本轮只重跑受影响的3套原生测试。当前使用与限制以HOST-MANAGER.md为准；旧性能数字不视为本轮复测。


## 2026-10-15 增量接口（外层协议仍为 1）

- `capabilities` 增加 `av-clock=host-qpc-v1` 与 `desktop-stream=h264-v1,h264-clock-v2`。桌面 Op32 请求 v1 字段与相对微秒时间戳保持兼容；v2 请求其余字段不变，配置响应在 H.264 headers 后附加字符串 `host-qpc-us`，Op33 的 PTS 使用主机 QPC 微秒。只在能力声明后请求 v2。音频 Op42 继续使用主机 QPC 微秒。
- UDP 47442 发现 v1：请求固定 128 字节，ASCII `LANTERN?`、u32 1、str 64 位小写十六进制随机 nonce、余部全 0；响应最多 256 字节，ASCII `LANTERN!`、u32 1、同 nonce、str SHA-256 证书指纹、str 主机名称（最多 64 UTF-8 字节）、u32 TCP 端口（1..65535）。整数及字符串长度采用现有网络序。响应来源决定候选连接地址；发现结果不能替换已保存信任。
- 共享调度不增加网络管理权限接口；`Host::set_link_rate` 是本地运行时 API。调度仍受原会话授权、取消和撤销约束。
- Windows 下载目录提交凭据 v1 是本机 DPAPI 文件；完成任务日志持久化前保留，离线认领恢复须同时核对主机身份、目录 ID、文件集合和内容摘要。
- Android 任务日志新增 kind=3、content URI 与 sourceHash；旧 kind 0..2 保留。摘要用于拒绝跨源版本续传。仅具备持久读取授权、已知长度和可寻址能力的来源使用该类型，其余使用原快照方案。
- Android 已增加前台手动剪贴板 UI，复用 Op53/54；多显示器和快捷键复用既有接口。原生 UTF-8 字符串解析拒绝损坏序列。

## 公网后端增量（2026-10-16，外层协议仍为 1）

以下新增约定优先于下方历史 v0.2 说明。完整调用方式见 [PUBLIC-NETWORK-HANDOFF.md](PUBLIC-NETWORK-HANDOFF.md)。HELLO 成功回复、请求权限和通道格式保持兼容。

### 认证后的入口枚举

能力串新增 `endpoints=v1`。客户端先执行 HELLO，再查询 CAPS；没有此能力时不发送 Op56，继续使用原有单地址连接。Op56 空请求，成功响应在原响应头后为：

```text
u32 API_version=1, u32 count (0..8)
repeated count times:
  u32 kind, str host (<=253 bytes), u32 port (1..65535), u32 priority,
  u64 expires_at (Unix seconds; 0 unspecified), u32 failures (server sends 0)
```

类型：1 LAN IPv4，2 ULA IPv6，3 稳定公网 IPv6，4 DNS 主机名，5 看起来像公网的 IPv4 / UPnP 映射。优先级数值越小越靠前；客户端始终先排 LAN，再比较优先级。主机默认优先级依次为 100、200、300、400、500，虚拟网卡另加 10。IPv6 的 `host` 无方括号，地址比较使用八组小写规范形式，IPv4 映射地址归一到 IPv4。DNS 使用 ASCII 名称（国际域名由界面先转换为 punycode）。不接受 URL、凭据、路径或 zone ID。

主机只广告实际监听地址覆盖的网卡；去掉临时、弃用、CGNAT 地址。公网开关关闭时不广告 3..5；开启时加入配置域名和有效的 IPv4 UPnP 映射。IPv6 始终使用监听端口；IPv4 映射、域名可以使用外部端口。入口只能在认证后请求；认证前 Op56 返回 unauthorized 并断开。发现协议完全不变，不携带公网入口。

### 配对链接 v2

七个参数和长度限制与 v1 相同，`v=2` 时 `a` 内每项语法是 `IPv4[:port]`、`[IPv6][:port]` 或 `hostname[:port]`。省略项内端口时使用 `p`。最多 8 项、最多 1 个 DNS 名称；按规范化后的 host+port 拒绝重复。拒绝 IPv6 zone、链路本地、多播、未指定地址、损坏/非规范端口、非法 DNS 标签。全字符串仍最多 2048 字节。IPv6 方括号属于 URI 内容，不属于最终连接主机名。

示意结构（占位指纹/配对码需换成实际 64 位十六进制，时间需未过期）：

```text
lantern://pair?v=2&n=Lantern&a=192.0.2.2,[2001:db8::1],example.com:443&p=47443&f=<64-hex>&c=<64-hex>&x=<Unix-seconds>
```

生成器仅在含 IPv6 或域名时生成 v2；纯 IPv4 仍生成 v1、使用一个共同端口。如果只含 IPv4 却要求不同端口，生成器返回 invalid，由界面选共同端口或加入已配置域名。新客户端同时支持两版，配对后保存所有入口；成功的端口可能不同于 `p`。只有一次兑换请求会发送出去，竞争失败的 TLS 连接不发送码，兑换响应丢失不会自动重试。

邀请新增持久化 `allow_remote`，默认 false。兑换仍是 Op55 原帧格式；默认仅接受私有 IPv4、ULA、链路本地和回环来源。公网来源返回 unauthorized，也计入指定邀请的 5 次失败。IPv4 映射来源先规范化；PAIR 来源限速的 IPv6 按 /64 聚合。

### 本地持久化版本

Host Config 写 v4，读 v1..v4。在 v3 的**每一条邀请**末尾加入 `u32 allow_remote (0/1)`；全部邀请后加入 `u32 internet_access (0/1), str public_hostname (<=253), u32 external_port (0 means unspecified)`。旧配置读取时新增字段为 false/空/0；开启公网且旧共享凭据仍启用的配置拒绝保存/载入。配置编辑继续用跨进程锁和原子 DPAPI 快照。

Windows 客户端凭据仍以 `u32 1` 和旧 host/port/pin/token 字段开头，可选尾部为上面的 count+entries；旧文件没有尾部也可读。传输 journal 写 v4、读 v1..v4，在每项旧凭据字段后插入入口列表，再接原有任务字段。Android 提供非秘密入口偏好文本 v1，令牌的 Keystore 行为不变。

**旧版程序不能读 Host Config v4 和新的传输 journal；Windows 主机、Windows 客户端、Android 客户端应一起升级。**线上的旧 HELLO 客户端仍能连接新主机，单地址能力回退不要求旧主机支持 Op56。

## QR pairing v1（Op55，v0.2 历史约定，新增部分以上节为准）

客户端从受信任的电脑屏幕扫描二维码，先使用链接中的完整 SHA-256 固定 TLS 证书，再发送第一帧：

```text
u16 protocol_version=1, u16 operation=55, str invitation_code, str device_label
```

设备名为合法 UTF-8，1..64 字节，拒绝 ASCII 控制字符。配对码为 64 位小写十六进制：前 32 位是随机公开邀请 ID，后 32 位是独立随机的 128-bit 秘密。整个码仅在 TLS 建立且证书固定通过后发送。服务端仅持久保存完整码的 SHA-256；公开 ID 用于把错误秘密累计到具体邀请，未知 ID 不会处罚其他邀请。

成功响应：`u16 1,u16 55,u32 0,str device_id,str device_token,u32 effective_scopes`。设备 ID 和新令牌均为独立 256-bit 随机数的 64 位小写十六进制。邀请消耗与设备写入在同一个配置编辑事务中原子提交，权限取邀请权限与当前全局上限的交集；交集为空拒绝。连接结束，客户端保存凭据后重新走原有 HELLO，原 HELLO 字段和行为没有变化。

失败响应为原有响应头加错误字符串，operation=55。`unauthorized=2` 表示无效、过期、已取消或已消费；`busy=8` 表示来源尝试过于频繁或设备容量已满；`unsupported=6` 表示配对协议版本不支持；畸形字段为 `invalid=1`。失败消息不包含提交的码或任何令牌。

每个来源 IP 的滑动 60 秒窗口最多 10 个 PAIR 帧，包括成功、失败和畸形请求。限速表最多 1024 个来源；表满时新来源返回 busy。每个邀请累计 5 个错误秘密后删除；未知邀请 ID 只受 IP 限速。IP 限速属于运行中的 Host 实例，不跨独立进程或主机重启持久化。错误次数和一次性消费通过配置锁跨同一 Windows 会话的主机/管理进程协调。

能力串新增 `pair=qr-v1`，原有认证后 CAPS 查询保持兼容。客户端把带 HELLO 或 PAIR 操作头的 unsupported 响应识别为“主机版本过旧”。仓库历史主机也有在非 HELLO 首帧后直接关闭的实现；无响应断线无法可靠区分旧版本和消费后的网络故障，客户端返回 `outcome_unknown`，不自动重复兑换。

二维码内容严格为：

```text
lantern://pair?v=1&n=<percent-encoded UTF-8 hostname>&a=<IPv4,IPv4>&p=<port>&f=<SHA256>&c=<code>&x=<Unix expiry seconds>
```

所有 7 个字段必须且只能出现一次；拒绝未知字段、fragment、非预期 scheme/authority、损坏 percent/UTF-8、空/超长主机名、非规范 IPv4、重复地址、DNS 名称、IPv6、0.0.0.0、组播、非法端口、非十六进制指纹/码和过期链接。最多 8 个地址、总长 2048 字节。指纹允许输入大写并归一为小写，码使用小写；不把 `+` 当空格。客户端和主机都独立校验有效期，时钟错误需要用户校正。

主机配置写入版本升为 3，保留 v1/v2 读取。v2 字段后附加 `u32 invite_count`（<=32），每项为 `str id(32),str label(64),str code_hash(64),u32 scopes,u64 expires_at,u32 failed_attempts`；全部在 DPAPI 中。邀请的明文码不可从存储恢复。过期项在邀请操作时清理，已消费项立即删除。旧版主机/管理程序不能读取 v3，主机与管理程序应一起升级；旧客户端可继续连接新版主机。
