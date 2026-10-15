# Lantern 0.1

Windows 主机、Windows 原生客户端与 Android 客户端的远程桌面、独立文件访问和文件传输软件。C++ 核心，Windows 原生界面，Android 原生 Java/JNI 界面。影片与文档使用设备已有应用，内置影片播放器已移除。

界面设计与控件标识：Windows 见 [docs/WINDOWS-UI.md](docs/WINDOWS-UI.md)，Android 见 [docs/ANDROID-UI.md](docs/ANDROID-UI.md)。Windows 界面冒烟测试：`powershell -ExecutionPolicy Bypass -File tests\windows_ui_smoke.ps1 -Bin <构建目录>\Release -Out <临时目录>`。

这是可构建、已实际运行的实验版本，**完整产品尚未完成**。本批源码和安装包对应 2026-10-15 版本，真实结果及边界见 [本批交付报告](docs/CONTINUATION-DELIVERY.md)，所有剩余模块见 [REMAINING.md](docs/REMAINING.md)。

## 使用

1. 解压 Windows ZIP，双击 `lantern-host.exe`。首次使用点“新建主机配置”，粘贴授权目录，例如 `E:\Anime`，确认后添加设备。仅看动漫授予文件读取即可；不要将身份状态目录放进共享目录。
2. 将绑定地址改为电脑 LAN 地址或 `0.0.0.0`，明确启动主机；`127.0.0.1` 只能本机连接。Windows 防火墙或应用控制须允许实际使用的程序与网络；本项目不会自动关闭系统安全策略。
3. 安装 Android APK，在设备页输入地址或发现局域网电脑，核对电脑的完整指纹，填写该设备密钥。发现不代表信任。
4. 手机文件页直接浏览授权目录。点动漫选择原始媒体用手机应用打开，不必投屏；播放器不兼容时下载后打开。共享根是 `E:\Anime` 时手机不需再填写 `E:\Anime`。
5. 需要远程操作时订阅桌面并开启控制，可全屏、横竖屏、双指缩放、拖拽和正常双击。全屏工具内可开控制和键盘；“更多”提供剪贴板、显示器、快捷键和声音同步选择。

Windows 客户端是 `lantern-client.exe`，设备、桌面、文件、传输任务分开。设备页可启动本机主机管理。文件写入需同时满足主机权限上限与设备权限，修改后重新连接。

## 当前能力

- TLS 1.2 固定证书指纹；DPAPI / Android Keystore；逐设备授权、撤销、单连接或整设备终止。
- 文件分页、排序、可取消搜索、图片缩略图；电脑内复制、移动、新建、删除不绕经手机。
- 双向文件与目录队列、断点续传、SHA-256、暂停取消、冲突保留原件；Windows 目录最终提交后的进程崩溃可离线认领恢复。
- Android 后台传输；可寻址并有持久授权的 SAF 源直接上传，其他来源使用有上限暂存。目录上传目前逐文件提交。
- DXGI / NVENC H.264 到 Windows GPU / Android MediaCodec；兼容时可回落 JPEG；独立 WASAPI / Opus 系统音频。
- 默认“响应优先”；可选“声音同步”使用音频硬件时钟安排画面，等待有上限并有超限提示。隐藏显示、停止订阅与主机隐私显示功能严格区分；隐私屏与虚拟显示器仍不支持。
- Windows 文本剪贴板手动及可选同步；Android 前台手动双向文本剪贴板。多显示器选择、拖拽、Unicode 输入法与常用快捷键。
- IPv4 LAN 发现、IPv4/IPv6 手工地址、网络诊断；跨连接共享带宽调度和运行时上限。没有公网穿透或中继服务。
- 原始媒体可寻址读取、外部播放器、临时缓存与系统打开方式；不宣称由 Lantern 实现外部播放器的字幕、HDR 或办公编辑功能。

## 构建与验证

Windows 使用 Visual Studio 2022 C++ 桌面工具与 Windows SDK，CMake 3.24+：

```powershell
.\build.ps1
.\build.ps1 -Package
# 可选：启用全部 15 套原生与 Java/JNI 检查
.\build.ps1 -JniTestJdk 'C:\Tools\jdk-17'
```

脚本要求测试通过后才打包；本机应用控制曾阻止两个最终测试程序运行，须在允许的开发环境运行，不能将未运行算通过。配置 JDK 17 的 `LANTERN_JNI_TEST_JDK` 可增加 Java/JNI 测试；当前共 15 套，未配置 JDK 时为 10 套原生测试。Android 构建说明见 [android/README.md](android/README.md) 及 [本批报告](docs/CONTINUATION-DELIVERY.md)。

本批 Android 真机工具/发现 9 项、SAF 上传 19 项、音画/文件/全屏 32 项通过。Windows 真实 GPU/音频时钟 313 项断言（含 300 帧序号）和首次配置 GUI 8 项通过；其余原生检查及系统阻止项按报告分别记录。Android 解码回调约 60 次/s，但实际 TextureView 更新在本批音画窗口约 43–47 次/s，**尚未验收稳定 1080p60 屏幕更新**。未测光学延迟、真实音画偏差、4K 长影片、发热或长稳。

## 文档与许可证

- [架构与模块边界](docs/ARCHITECTURE.md)、[协议](docs/PROTOCOL.md)、[插件 C ABI](include/lantern/plugin.h)
- [主机授权管理](docs/HOST-MANAGER.md)、[Windows 客户端](docs/WINDOWS-CLIENT.md)、[本批操作与验证](docs/CONTINUATION-DELIVERY.md)
- [未完成模块](docs/REMAINING.md)、[第三方依赖与许可证](THIRD_PARTY.md)

源码采用 MIT；Opus、NVENC 头文件及工具链许可分别随 `licenses` 与依赖目录提供。当前 Android 包为调试签名，Windows 为便携 ZIP；正式签名安装器与自动升级尚未实现。
