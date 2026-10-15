# Android 构建与运行

Android 原生 Java 界面共享 C++ 协议核心，通过 JNI 接入。最低 Android 8.0（API 26），目标及编译 SDK 35。已在 PKR110 / Android 16 / arm64 真机验证；x86_64 JNI 库只做了构建和包结构检查。

## 可复现的独立构建

安装 JDK 17、Android SDK Platform 35、Build Tools 35.0.1 和 NDK r27c（27.2.12479018）。在本目录运行，路径替换为本机工具目录：

```powershell
.\build-standalone.ps1 `
  -Jdk 'C:\Tools\jdk-17' `
  -Platform 'C:\Android\platforms\android-35' `
  -BuildTools 'C:\Android\build-tools\35.0.1' `
  -Ndk 'C:\Android\ndk\27.2.12479018' `
  -ScratchDir 'C:\Work\lantern-android' `
  -Apk 'C:\Work\Lantern-0.1-Android-debug.apk'
```

脚本编译全部 Java 和两个 ABI（arm64-v8a、x86_64），使用 16 KiB 原生页对齐，组装 APK、调试签名，再验证签名和对齐。输出目录需已存在。每次使用新的 Java 类输出目录，避免已删除类残留。调试密钥保存在 ScratchDir，勿把它放入源码或共享目录。

本次交付为 **debug APK**，包含仅用于验证的 instrumentation 和受 `MANAGE_DOCUMENTS` 权限保护的测试文档提供者，不是正式签名发行版。Gradle 工程也保留：使用支持 AGP 8.9.2 的 Gradle 及 JDK 17，在本目录执行 `gradle :app:assembleDebug`；需先配置 `ANDROID_HOME` 或未提交的 `local.properties`。仓库不捆绑 Gradle wrapper；本批实测构建走上面的独立脚本。

## 使用与验证边界

手机与 Windows 主机连同一局域网，在设备页输入主机地址，或发现 IPv4 电脑；核对电脑显示的完整证书指纹并填写独立设备密钥。发现结果不代替配对。文件浏览和原始媒体打开无需投屏，电影和文档交给手机已有应用。

桌面和全屏的“更多”提供剪贴板、多显示器、快捷键及“响应优先 / 声音同步”。SAF 来源具备持久读取授权、已知长度和寻址能力时可直接上传，其余来源回落到暂存。目录上传仍逐文件提交。

真机验证脚本为 [tests/device_debug.py](../tests/device_debug.py)，使用 `--help` 查看模式和参数。脚本只创建生成素材；需显式指定已授权 ADB 设备、测试主机程序、APK、实际 LAN 地址、独立测试目录与报告路径。USB 只作调试；本批业务链路为真实 Wi-Fi。

本批 9 项工具/发现、19 项 SAF 上传、32 项音画/文件/全屏检查通过。解码回调约 60 次/s，TextureView 实际更新约 43–47 次/s，尚未验收稳定 1080p60。完整结果见 [本批报告](../docs/CONTINUATION-DELIVERY.md)，剩余功能见 [REMAINING.md](../docs/REMAINING.md)。
