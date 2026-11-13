# 依赖与许可证清单

## 实际运行依赖

| 项目 | 使用 | 许可证/分发 |
|---|---|---|
| 本仓库 Lantern | 核心与客户端 | MIT，见 LICENSE |
| Windows 系统 DLL | Winsock、Schannel/SSPI、Crypt32/DPAPI、CNG、WIC、GDI、D3D11、DXGI、Shell、COM、WASAPI/CoreAudio、Media Foundation系统H.264解码/DXVA、原生Common Controls；测试音使用WinMM | Windows 平台组件，由用户系统提供，不复制 DLL |
| MSVC C++ runtime | 静态 C++ 标准库 | 由 Visual Studio 工具链提供，/MT 静态链接，按 Microsoft 分发条款；不要求额外安装 VC++ Redistributable |
| Android 平台 | SSLSocket、Keystore、ContentProvider、Proxy FD、MediaCodec/Surface、AudioTrack、InputConnection、系统 UI | 用户 Android 系统提供，不捆绑系统镜像 |
| LLVM libc++/libc++abi（Android native） | 静态 C++ 标准库 | Apache-2.0 WITH LLVM-exception；NDK NOTICE/许可证需随 APK 对应分发材料提供 |
| nv-codec-headers n12.2.72.0 / nvEncodeAPI.h | Windows NVENC C ABI 声明 | NVIDIA 2010–2024，MIT；固定头文件 SHA256 与来源见 third_party/nvenc/README.md，随包提供许可证 |
| NVIDIA 编码驱动 | 可选硬件 H.264 提供者，从 System32 动态加载 | 用户驱动提供，不随本软件分发；没有捆绑 CUDA/NVIDIA SDK 运行库 |
| 示例插件 | 字节预览 | MIT，无外部依赖 |

启用媒体前缀时，主机静态链接 FFmpeg/libass 及下列依赖；没有 mpv、Qt、Chromium、OpenSSL 或 QUIC 运行库。可选 libopus 静态编码/解码见下节。外部播放器/办公应用由设备用户安装，它们的能力和许可证由应用本身负责。

## 构建工具

- Visual Studio 2022 / Windows SDK：Microsoft 条款，用户工具安装。
- CMake：BSD-3-Clause。
- Android SDK Platform 35 / Build Tools 35.0.1：Android SDK License；仅构建工具，不打入 APK。官方包的 checksum 经 repository XML 验证。
- Android NDK r27c (27.2.12479018)：NDK 随包 NOTICE 与各组件许可证；LLVM Apache-2.0 WITH LLVM-exception，具体 runtime 分发材料见 `licenses/android`。
- Microsoft Build of OpenJDK 17：GPLv2 with Classpath Exception；工作目录仅用于构建，JDK 不随产品发布。下载通过官方 SHA-256 校验。
- Gradle/Android Gradle Plugin：Gradle Apache-2.0，Android Gradle Plugin Apache-2.0；提供固定版本配置。此次 Android 构建也支持独立 SDK 工具路径，具体实测见报告。

## v0.3 optional static media

| Component | Version | License selected for distribution |
|---|---|---|
| FFmpeg | 7.1.2 | LGPL-2.1-or-later; GPL/nonfree/version3 disabled |
| libass | 0.17.5 | ISC |
| FriBidi | 1.0.16 | LGPL-2.1-or-later |
| HarfBuzz | 14.2.1 | MIT-style notices in the supplied copyright file |
| FreeType | 2.14.3 | FreeType Project License option (not its alternative GPL option) |
| libpng | 1.6.58 | libpng-2.0 |
| zlib | 1.3.2 | Zlib |
| bzip2 | 1.0.8 | bzip2 license |
| Brotli | 1.2.0 | MIT |
| nv-codec-headers | 13.0.19.0 | MIT; build headers, no NVIDIA runtime bundled |

Full notices are in [licenses/media](licenses/media). Exact source archive hashes,
the vcpkg baseline, configure flags and build/relink instructions are in
[MEDIA-BUILD.md](docs/MEDIA-BUILD.md) and tools/media/sources.lock.json. Do not
confuse libass (ISC) with FriBidi/FFmpeg (LGPL). The main NVENC desktop header
version listed above remains separately vendored.

The host uses real in-process static libraries; it ships no ffmpeg.exe and does
not download a conversion component at runtime. Android uses system decoders
and contains no FFmpeg. Windows adapted movie playback is not exposed this round.

Static LGPL delivery uses the section 6(a) source/object route: distribute the
corresponding library sources and applied patches, application link objects,
static archives and the relink recipe alongside every binary. Recipients can
modify the LGPL libraries and relink with the supplied MIT application objects.
Reverse engineering to debug those modifications is permitted. This does not
relicense the MIT Lantern source. The repository recipe alone is insufficient:
the matching generated source/object delivery must accompany the binary.
See [FFmpeg's guidance](https://ffmpeg.org/legal.html) and the full included LGPL
text. Release UI attribution and links to notices remain Claude's integration
work; no backend result implies that public distribution requirements are all
satisfied before that integration and source/object delivery are present.

## libopus 1.5.2（可选内置 Windows 编码/解码器）

官方源码：https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz 。官方发行页：https://opus-codec.org/release/stable/2024/04/12/libopus-1_5_2.html 。下载SHA256已核对：65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1。
源码保存在 third_party/opus-1.5.2；许可证见该目录COPYING以及 licenses/OPUS.txt。BSD类许可证及相关专利声明按原文保留。Windows主机编码与客户端解码静态链接，不携带额外运行时DLL；CMake LANTERN_WITH_OPUS=OFF可去除。Android通过系统MediaCodec解码Opus，桌面视频通过系统MediaCodec解码；文件影音交给外部手机应用，APK没有打包FFmpeg/libopus。生成验证视频用的FFmpeg工具仅保存在工作区，不随产品分发。

Windows剪贴板/WIC预览/多文件选择器增量只调用Windows系统API，没有增加第三方库或捆绑播放器；许可证清单保持适用。

## Nayuki QR Code generator 1.8.0（C++ 编码）

- 来源：[官方仓库 v1.8.0](https://github.com/nayuki/QR-Code-generator/tree/v1.8.0/cpp)，tag 解引用的 commit 为 `720f62bddb7226106071d4728c292cb1df519ceb`。
- 位置：`third_party/qrcodegen/qrcodegen.cpp`、`qrcodegen.hpp`。保留原始头部 MIT 声明，并提供 `third_party/qrcodegen/LICENSE`，安装时复制为 `licenses/QRCODEGEN-MIT.txt`。
- 原始文件 SHA-256：cpp `1f3b3fcdac6954c32cf583ccd02ec9b5901f756a38c461acedc70be4a77d3757`；hpp `b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793`。
- 只静态链接编码器；`qr_encode` 固定 M 纠错、自动选择 1..40 版本，不自动提升纠错等级。没有相机/扫码实现和动态运行库。

## ZXing core 3.5.3（Android 扫码解码）

- 来源：Maven Central `com.google.zxing:core:3.5.3`（https://repo1.maven.org/maven2/com/google/zxing/core/3.5.3/ ）。项目主页 https://github.com/zxing/zxing 。
- 位置：`android/libs/zxing-core-3.5.3.jar`，未作修改。SHA-1 `ca1349214a356cd7958651b2d5a0e1f3811a9c4b`（与 Maven Central 发布的校验值一致），SHA-256 `8d8064c1636fdaef7189dd9055c7d59950a8940a12f2293956446ec3c109fd82`。
- 许可：Apache License 2.0，全文见 `licenses/ZXING-APACHE-2.0.txt`；APK 内为 `assets/licenses/ZXing-Apache-2.0.txt`，可在应用“开源许可”页查看。
- 用途：只在 Android 端 `ScanActivity` 中用 `QRCodeReader` 解码相机画面里的配对二维码。Windows 端不使用。
