# 硬件桌面与安卓全屏：截止验收报告

此文件保留初次截止阶段的历史实现与测量。后续已取消双击缩放，加入电脑双击、拖拽、输入法及系统音频；当前状态见DESKTOP-INTERACTION和SYSTEM-AUDIO，旧数值不冒充新版本复测。

用户截止时间：2026-10-13 19:45，Asia/Tokyo。**整个产品未在此截止时间完成。** 19:45 时全屏已有 50 项真机检查通过，主机硬件编码有短测，安卓硬件持续播放尚未验收；本文件记录截止验收后完成的故障修复和收尾验证。它不是整套软件的完成声明。

## 已交付实现

- Android 单父容器全屏布局；全屏退出/文件/返回键、横竖屏、双击缩放、双指缩放和拖动。真实横竖屏、双击缩放、返回文件页保持连接已测试；双指缩放/拖动实现尚未独立自动手势压力验证。
- 当前 Windows NVIDIA GPU 显示器：Desktop Duplication → D3D11 GPU 缓存/缩放 → NVENC H264 High 8-bit → 独立 TLS/TCP 持续推送。压缩像素不经 CPU 回读；编码码流仍存在有界字节复制。
- Android MediaCodec 查询真实硬件标志、SPS/PPS 配置、Surface/TextureView 输出；默认 1080p60/12 Mbps 目标，可切 720p60/8 Mbps、720p30/6 Mbps。JPEG 三档作为明确降级。
- 专用流版本 1、顺序和时间戳校验、两路上限、单帧处理、256 KiB socket 发送缓冲、发送超时/积压终止；独立查看权限和每 250 ms 撤销检查。不是完整弱网拥塞/过期 datagram 算法。
- 隐藏仍接收/解码而不渲染；恢复显示重建 Surface 时重新开会话/IDR。停止订阅 abort 原始 socket，后台线程完成 TLS 与解码器释放。

## 实测设备与结果

主机 i5-14600KF / RTX 4070 SUPER / Windows 11；一加 Ace 5 Pro PKR110 / Android 16 API36 / ARM64。PC 10.217.43.110、手机 10.217.43.250，热点局域网。USB 只用于安装与调试，**性能数据使用真实 Wi-Fi TCP 47443**。画面是实际工作桌面，非固定标准运动片。

| 项目（实测） | 结果 | 定义/范围 |
|---|---:|---|
| 主机探测编码吞吐 | 59.84 fps | 1920×1080，120 帧 / 2.00523 s；独立解码器成功解码120帧 |
| 主机采集+GPU缩放均值 | 0.176 ms | 短测调用耗时，GPU异步阶段不等同完整执行时延 |
| 主机编码+码流取出均值 | 1.966 ms | 同一短测 |
| Android 硬件解码器 | c2.qti.avc.decoder | isHardwareAccelerated=true |
| Android 首个呈现回调 | 237.34 ms | 流配置/解码阶段至回调；不包括此前 TLS 连接、View 准备或输入延迟 |
| 10秒收包/解码呈现回调率 | 60.00 /s | 正确序列码流，回调计数不等于面板发光 |
| TextureView 新图像更新 | 51.20 /s | 真实 onSurfaceTextureUpdated，System.nanoTime 计时 |
| TextureView 更新间隔 p95 | 33.39 ms | 同一10秒窗口 |
| 本次实际编码有效码率 | 7.59 Mbps | 12 Mbps 为配置目标；桌面内容下实测字节量 |
| 最近纹理复用帧 | 56 | 同一10秒窗口，不能把所有编码帧视为独立源画面变化 |
| 横屏全屏叠加层更新 | 51.11 /s | 5秒 onDraw 计数，独立于码流和Surface计数 |
| 并行32MiB下载 | 2.399 s | 包含元信息/校验；使用已积累的burst，不是链路持续上限 |
| 并行时解码呈现回调 | 60.02 /s | 与下载窗口一致；SHA256确认 |

**尚未证明实际面板持续60 fps、1080p60完整验收、光学输入端到端延迟、系统音频或长稳。** 客户端图像更新约51/s，与解码呈现回调60/s必须分别报告，不将后者替换前者。此设备驱动回报的 FrameRendered nanoTime 在会话相对域，与 System.nanoTime 不同，因此只用同域相邻差，不据此计算跨设备延迟。TextureView计时使用本地单调时钟。

## 验证与发现的错误

- C++ core 88项、integration 44项，启用JVM互操作后integration 45项+Java11项，均通过。新增5项：流观看授权、截断、版本、尺寸、通道隔离。
- 最新 Android 文件/Keystore/只读FD/JPEG完整回归 49 项通过；32MiB完整下载 9.97 MiB/s，上传/下载取消后实际从1MiB恢复。
- 硬件桌面独立 19 项通过：硬解、持续呈现与TextureView更新、全屏旋转/缩放保持同一连接、并行文件hash、隐藏继续收包、停止释放流。JPEG专用全屏首轮50项通过，记录仍保留。
- 前两次统计错误地把驱动相对时间和本地时钟直接比较，导致测试误判；记录 desktop-h264-attempt1/2.json，不删除失败证据。
- 停止订阅曾触发 NetworkOnMainThreadException：SSLSocket.close 在 UI 上发送 close_notify。已改为原始Socket abort，TLS完成关闭留在后台；实际停止/隐藏/恢复回归通过，记录 attempt3.json。
- 把主机逐轮相对 sleep 改为绝对周期调度，并扩大固定发送缓冲到256KiB；两项同时调整，所以不能把提速只归因其中一项。

## 重复执行

```powershell
.\build.ps1 -Package
python tests/device_debug.py --adb C:/tools/platform-tools/adb.exe --serial DEVICE_SERIAL --host-exe package/lantern.exe --apk ../Lantern-0.1-Android-debug.apk --address HOST_LAN_IP --work C:/LanternTests --report desktop-h264.json --video-only
```

源代码诊断程序由 CMake 构建：`lantern-video-probe OUTPUT.h264 REPORT.json`。码流是桌面截图信息，只放私有测试目录，不随软件发布。独立验证可用合法安装的 ffmpeg：`ffmpeg -f h264 -i OUTPUT.h264 -f null NUL`，此工具不随发布包捆绑。

## 剩余交付

- Windows客户端硬件解码；Intel/AMD编码、HEVC、HDR/色彩验证、主机显示器旋转、硬件光标叠加、完整键鼠与触控模式、系统音频/A-V、隐私屏/虚拟显示器。
- 目录/批量传输与任务管理、搜索/排序/缩略图、源版本冲突交互、设备级信任撤销、缓存回收、剪贴板/快捷键、网络切换自动恢复和自适应跨通道公平预算。
- 原始4K影片真实播放/拖动/ASS/HDR/多轨兼容、外部办公/播放器跨应用验证；当前原始文件管线有字节随机读取验证，不能称完整播放器。
- 面板帧率/240fps光学延迟、10分钟运动片/丢帧、30分钟温度和降频、8/24小时稳定性、可控弱网，以及第二台Windows的完整验收。

这是可运行的实验交付，有源码、构建、协议、插件示例、依赖许可和真实有限测量；不是全部产品范围完成。
