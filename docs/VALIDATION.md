> 2026-10-15 更新：首次配置、桌面工具、目录恢复、SAF直传、发现、调度与音画共同时间轴已新增；当前结果、限制与版本兼容以 [本批报告](CONTINUATION-DELIVERY.md) 为准。下文较早的验证数字是历史记录。

# 验证与真实性能报告

验证日期：2026-10-13（Asia/Tokyo）。结果只适用于所述环境。现已完成用户提供的一加 Ace 5 Pro / Android 16 真机验证，见 `DEVICE-VALIDATION.md`。仍没有第二台 Windows 设备、4K 动漫测试片或可控弱网设备，对未测项目不编造数据。

## 构建与功能实测

| 项目 | 状态与证据 |
|---|---|
| Windows x64 Release | MSVC 19.44.35229.0、Windows SDK 10.0.26100.0、CMake；/MT 静态运行库，构建成功 |
| 协议、安全、调度、插件 | core_tests：88 个断言通过；包括截断字段、超长 blob、Windows 路径、Range、共享预算、状态终结规则、真实动态插件 ABI/缓冲/生命周期 |
| Windows 文件服务/输入/音频协议 | integration_tests：70 个断言通过，真实 TLS/文件/网络；取消与断点、服务器重建恢复、错误哈希、源变化、主机内 copy/move/delete、权限、撤销/重新配对、新式输入解析边界、音频通道与纯音频身份 |
| junction/硬链接逃逸 | 实际创建并验证读取/写入拒绝；symbolic link 创建因 Windows 权限 1314 跳过，不能把这一条写成已测试通过 |
| Java/JNI → C++ 主机 | JVM 中使用 Android 同源 Remote/Native 类，13 个互操作断言通过；C++ integration 包含该检查后 71 个断言通过，viewport几何19项通过。另有 Android 真机测试覆盖 Keystore、UI、代理 FD |
| 媒体桥 | 真实独立主机和桥进程；9 项通过：HEAD、4 路并发不同范围、后缀区间、大小写头部、非法范围 416 和错误路径 404；逐字节比较 2,097,225 原始字节。没有播放/解码测试 |
| Windows GUI | 隐藏方式启动自己的原生窗口，创建、3 s 空闲、正常关闭通过；尚无连接后的 GUI 操作自动化 |
| Android APK | JDK 17、Platform 35、Build Tools 35.0.1、NDK r27c；Java 与 ARM64/x86_64 C++ 原生库实际构建；APK v2/v3 签名及 zipalign 16 KiB 检查通过。测试签名；已安装到 ARM64 真机；完整复测 49 项通过，三档桌面复测另 41 项通过 |

回归曾发现 Windows 句柄移动的偶发 ERROR_INVALID_NAME。修正可变长度 UTF-16 文件名缓冲的终止字符后，完整测试通过，连续 5 次 integration 复测均通过。这是针对已发现问题的有限回归，不能证明长时间运行稳定性。

## 性能实测条件

- CPU：Intel Core i5-14600KF，14 核/20 线程。
- GPU：NVIDIA GeForce RTX 4070 SUPER，驱动 32.0.15.9186；**此次软件路径没有利用该 GPU 的视频硬件编解码**。
- 内存：34,138,935,296 bytes（约 31.79 GiB）。
- 系统：Windows 11 Pro 10.0.26300。
- 网络：同一机器 loopback，主机与客户端在同一 benchmark 进程内；不是物理 LAN，不能推出 Wi-Fi 性能。
- 数据：64 MiB 确定性二进制文件；100 个 4 KiB 小文件。未测 TB 级文件。
- 调度：全局名义 40 MiB/s，bulk 25%、media 50%，bulk 可积累最多 10 MiB burst。恢复与并行测试可能使用桶中已有令牌。
- 桌面：实际当前显示器画面、目标宽 1920、保持源纵横比、JPEG quality 70；连续 60 帧，无硬编，无客户端解码/呈现计时。内容不是标准运动测试序列。

以下均为**实测**，完整原始值见 `../benchmark.json`：

| 指标 | 本次结果 | 解释 |
|---|---:|---|
| 连接 TLS + 授权 | 5.195 ms | 回环单次 |
| 64 MiB 下载 | 6.443 s，9.933 MiB/s | 包含主机 SHA、逐块刷新、本地 SHA 与提交 |
| 64 MiB 上传 | 6.412 s，9.981 MiB/s | 包含源 SHA、逐块刷新、主机 SHA 与提交 |
| 64 MiB 独立 SHA-256 | 0.034 s | CNG；文件缓存状态未受控，不代表冷盘性能 |
| 100×4 KiB 下载 | 0.777 s | 顺序传输，非批量打包优化 |
| 断点恢复 offset | 8,388,608 bytes | 取消后重连，实际 offset 匹配 |
| 重连并传剩余 56 MiB | 5.268 s | 含全文件源 SHA 与最终校验；不能视为单独恢复握手开销 |
| 桌面请求吞吐 | 23.829 fps | 采集+软件编码+TLS 接收；未包含客户端渲染 |
| 桌面请求 p50 / p95 | 41.722 / 44.553 ms | 不是输入到显示端到端延迟 |
| 60 帧 JPEG 总字节 | 11,287,701 bytes | 静态/实际工作桌面，不代表运动画面最坏码率 |
| 并行下载 64 MiB | 5.524 s | 有初始 burst；独立可靠连接 |
| 并行桌面 | 132 帧，23.897 fps，p95 43.532 ms | 与文件下载共存；无音频/原始电影并行 |
| benchmark 峰值工作集 | 48,967,680 bytes | 主机+客户端+测试数据+编码都在一个进程，不能当作主机独立内存 |

`ui-smoke.json` 另记录隐藏 Windows GUI 的单次暖启动窗口创建约 1096.059 ms（含启动/轮询开销），空闲工作集 12,324,864 bytes，3 s 进程 CPU 时间增量为 0.0 s（测量精度内）。没有冷缓存控制、首个可交互帧或 8 h 空闲测量，不能宣称冷启动目标<500 ms已达成。

**完整 1080p60 验收、端到端<80 ms、高码率 4K 连续播放、音画同步、长期热稳定性和长期运行均尚未达成验证。**当前已用真实 NVENC/MediaCodec 跑通 1080p H264 短测，解码呈现回调约 60/s，TextureView 图像更新约 51/s，见 HARDWARE-DESKTOP.md；不能混用为完整显示 60 fps 的证据。回环软件桌面实测约 24 fps；真机新网络的 1280 宽 JPEG 请求/解码实测约 15 fps，详见 `DEVICE-VALIDATION.md`。调度已修正为计入处理耗时的最高 30 fps 周期预算；安卓独立画面绘制更新率实测从 5.70 提升至 15.10 fps，快速档另测约 18 fps。快速请求吞吐、onDraw 更新率、面板呈现和光学延迟是不同指标。

## 重复执行

```powershell
.\build.ps1
<BUILD>\Release\lantern-benchmark.exe <WORK>\benchmark <OUTPUT>\benchmark.json
python tests\media_bridge.py <BUILD>\Release\lantern.exe <WORK>\media-test
.\tests\ui_smoke.ps1 <BUILD>\Release\lantern-client.exe
```

benchmark 使用端口 47450；integration 47449；媒体测试 47452/47883。各用独立临时目录。测试工作文件与 identity 会保留供检查；系统创建的测试证书私钥由 Windows 用户密钥存储管理。请按明确的测试目录清理，不要删除任意用户目录。

可选 Java/JNI 验证：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 "-DLANTERN_JNI_TEST_JDK=C:\path\jdk-17"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

标准构建不要求 JDK；启用后自动编译同源 Java 类与本机 JNI 库。互操作测试把密钥通过匿名 stdin 管道送给子进程，不放到进程命令行或测试日志。

Android 可在 Android Studio/Gradle 环境构建，或者使用已实际执行的独立构建脚本：

```powershell
.\android\build-standalone.ps1 -Jdk C:\tools\jdk-17 -Platform C:\tools\android-35 -BuildTools C:\tools\build-tools\35.0.1 -Ndk C:\tools\android-ndk-r27c
adb install -r Lantern-0.1-Android-debug.apk
```

独立脚本不下载工具，不修改系统环境，生成测试签名且 debuggable 的 APK。生产包须使用用户自己的发布密钥和 non-debuggable 配置。Gradle 配置固定 AGP 8.9.2，推荐 Gradle 8.11.1 / JDK 17；具体要求见 [AGP 8.9 文档](https://developer.android.com/build/releases/agp-8-9-0-release-notes)。本次没有执行 Gradle 构建，执行的是直接 SDK/NDK 构建。

## 待执行的真实设备矩阵（全部是目标/步骤）

| 类别 | 条件与步骤 | 目标与记录 |
|---|---|---|
| 桌面端到端 | Win11 硬编主机 + Win/Android 真机，1080p60运动序列；客户端发带序号的输入，主机响应区域变化，用240fps相机同拍输入指示与显示屏，≥200次 | 目标 p95<80 ms；记录p50/p95/p99、丢帧、帧间隔、实际硬编路径。首版不适用于此目标验收 |
| 系统音频/A-V | 独立真实loopback/AudioTrack短测13项通过；后续flash/click序列连续10min，切换画面暂停/隐藏 | 目标 A/V≤40ms；音频首版已实现，音画同步和声学延迟尚未实测 |
| CPU/GPU/内存/发热 | WPR/PresentMon/GPUView及任务管理器；Android `dumpsys meminfo`, `dumpsys thermalservice`, Perfetto；30min与8h | 目标内存无持续增长、Android温度与频率稳定；报告设备/温度/采样周期，未实测 |
| 4K 媒体 | 3840×2160 HEVC Main10、23.976及60fps，40/80/100Mbps；MKV/MP4、双音轨、HDR10、ASS字体附件、外挂SRT/ASS；至少10min | 目标首帧<2s、20次随机seek p95<1.5s、无重缓冲；记录接收应用与硬解情况。桥的字节校验不证明播放达标 |
| 大文件 | 不可压缩10GiB及100GiB、NVMe与机械盘；双方hash验证；第1/30/90%处断网及主机重启 | 记录实际有效吞吐、完整校验时间、恢复元数据耗时、重复读取；目标接近剩余带宽，不承诺本版固定限速达线速 |
| 小文件/目录 | 10,000×4KiB、1,000×1MiB、多层目录；取消、空间不足、同名冲突 | 需先实现批量目录任务；记录总耗时、IOPS与峰值队列，当前只有100×4KiB顺序实测 |
| 并行体验 | 桌面1080p60 + 音频 + 4K观影 + 文件传输，分别限带宽200/100/30Mbps | 目标交互稳定，文件主动降速；本版只实测软件桌面与文件并行 |
| 弱网 | 测试路由器/netem网桥：RTT5/30/80ms，loss0/0.5/2/5%，抖动0/10/30ms；记录实际链路参数 | 验证期限丢帧、音频FEC、整体公平；这些未实现，不能仅凭分socket宣称通过 |
| 网络切换/休眠 | Android Wi-Fi→移动网络→Wi-Fi；主机休眠/恢复、锁屏、服务重启、断电恢复 | 本版手动重连/重试与落盘断点；目标自动会话恢复待实现；锁屏/安全桌面应明确失败 |
| 外部应用 | Android PDF/Word/PPT/Excel与至少2个播放器；URI权限、随机seek、进程被杀、缓存满；Windows默认/打开方式 | 只读查看，失败能选择完整下载；不测试不存在的编辑回传 |
| 包/启动/稳定性 | 干净Windows与Android安装、重启后冷启动、空闲8h、连接24h | 目标核心<15MiB、冷启动<500ms、空闲<40MiB；包文件大小另见manifest，安装/冷启动/长稳未测 |

测试材料需有合法来源；样本记录 SHA256、编码/profile/bit depth、帧率、色彩、轨道与平均/峰值码率。任何结果都应同时写设备、OS、应用版本、网络条件与“实测/目标/估算”。

新增 stream-v1 回归包含观看授权拒绝、截断请求、不兼容流版本、非法尺寸与通道隔离，5 项；硬件真机独立测试包含真实硬件解码标志、序列持续接收、Surface 更新、全屏旋转/缩放保持连接、并行文件哈希、隐藏接收与停止。

全屏控制修复：共享viewport当前JVM回归19项，真实Android→Windows点击13项通过；五点最大误差0像素。随后H26420项/文件49项回归通过，详见FULLSCREEN-CONTROL.md。

本轮追加实测：desktop-interaction.json 11项通过，真实拖动窗口、Windows双击、安卓输入法可见、中文/emoji/退格与断线/超时松键；system-audio.json 13项通过，真实WASAPI/AudioTrack、997Hz频率检测、音频独立于桌面、与H264/32MiB下载并行及快速开关；audio-only-ui.json验证仅音频权限配对与按钮。最终文件/Keystore/代理FD/JPEG 49项见device-lan-final.json。有限短测和未测项目见DESKTOP-INTERACTION/SYSTEM-AUDIO。


2026-10-14 后续实现与验证已更新：[文件、媒体、音频交付报告](FILES-MEDIA-AUDIO.md)。早期“未实现”条目请与该报告对照；完整产品仍未完成。


## 2026-10-14 Windows新客户端

本轮常规9个CTest套件通过（最后一次构建被客户端中断，在第4套件完成后恢复其余5套件，未把中断当作成功）。另有windows-client.json的真实DXVA/WASAPI与限速并行文件短测、windows-gui.json的实际全屏坐标/拖拽/双击/Unicode测试。环境为同一i5-14600KF / RTX4070 SUPER / Windows11专业版10.0.26300上的TLS回环，非两机LAN、非光学延迟/AVsync/4K外部播放器验收。具体可重复命令与限制见WINDOWS-CLIENT.md。

2026-10-14 Windows交互增量已构建/验证：文本剪贴板、图片预览、多文件上传、任务限速优先级；10套CTest、新增48项、GUI40项、媒体351项。USB关闭，安卓真机延期；同PC短测不能替代完整性能目标。权威结果见WINDOWS-INTERACTION-TASKS.md及windows-features.json。

2026-10-14 后续交付：本机Windows主机/授权/连接管理已接通，HostAdministration API-v1及本地无凭据HostSession接口，不增加网络管理员协议。安卓真Wi-Fi补测授权12项、文件/外部打开37项通过；本轮只重跑受影响的3套原生测试。当前使用与限制以HOST-MANAGER.md为准；旧性能数字不视为本轮复测。
