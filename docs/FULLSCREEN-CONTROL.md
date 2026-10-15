# 安卓全屏点击映射与工具栏修复

日期：2026-10-13，Asia/Tokyo。功能已经构建并在一加 Ace 5 Pro / Android 16 真机验证。

## 原因与实现

H264 使用 TextureView 输出，没有 ImageView drawable。原触控代码读取 ImageView.getImageMatrix()，在没有 drawable 的情况下不能可靠得到视频渲染的缩放/居中矩阵，手机触点因此映射到错误的源像素。

现在 DesktopViewport 管理源像素、显示区域、缩放、拖动偏移和黑边。JPEG 绘制、TextureView 变换和触控都读取这份几何数据；触控不再依赖 ImageView 的 drawable 矩阵。JPEG Bitmap 显式使用 DENSITY_NONE，避免显示密度把原始像素改变为 drawable 单位。越界、黑边和未准备画面不发送输入，最后像素的归一化坐标夹到65535。

全屏“工具”中的两行操作栏新增“控制：关/授权中/开”，不必退出全屏开启或关闭控制。点击“工具”始终展开操作栏，再次点击也不把按钮隐藏；用“收起”关闭操作栏。它和文件页的按钮同步。开启时单独申请控制权限，服务器拒绝时不会显示已开启。切换设备、离开前台或关闭控制会取消旧输入。

点击使用独立的单工作线程、有界32项输入队列和专用TLS连接；不等待文件任务队列。空闲连接超过5秒时重建，失败不会无限重试点击。新增op35把移动/左键按下/松开发成一条完整请求；服务器以一个SendInput批次注入，减少分消息断线留下按键的风险，部分失败尽力补松开。完整键盘/拖拽租约仍未实现。

## 实测

使用专门生成的全屏 Windows 测试画布，不点击用户的其他应用。主机测试模式只允许目标进程保持前台时注入输入；目标关闭或失焦就拒绝。Android 向真实控制连接发请求，Windows 画布从实际 WM_LBUTTONUP 记录落点。

本次主机画布2560×1440、H264输出1920×1080，包含主机端缩小后的坐标映射。五次手机触控经手势识别、网络、权限和真实 SendInput 后均与预期匹配：

| 场景 | 预期Windows像素 | 实际Windows像素 | 最大误差/像素 |
|---|---:|---:|---:|
| h264_landscape_center | 1280, 720 | 1280, 720 | 0.0 |
| h264_landscape_quarter | 640, 360 | 640, 360 | 0.0 |
| h264_zoom_pan_center | 960, 540 | 960, 540 | 0.0 |
| h264_portrait_center | 1280, 720 | 1280, 720 | 0.0 |
| jpeg_portrait_center | 1280, 720 | 1280, 720 | 0.0 |

真机13项断言通过；黑边点击和关闭控制后点击都没有产生额外Windows事件。控制开关在全屏工具栏的真实可见按钮上执行，测试查找会排除被隐藏的文件页按钮，避免只调用隐藏按钮而误报UI完成。

最后安装包补充UI核验通过：进入全屏并切横屏，收起工具栏后点击“工具”可见“控制：关”，再次点击“工具”后按钮仍可见；控制保持关闭。该检查只操作客户端工具栏，没有向用户桌面注入点击。

纯Java/JVM几何回归17项通过，包括横/竖屏中心、两种黑边、2倍裁剪、缩放拖动、边界夹取、NaN和未准备画面。C++ integration 新增控制权限拒绝、截断和超范围指针3项；共47项，启用Java互操作后48项加Java11项。core88项不变。

进一步回归：`desktop-h264-control-regression.json` 19项、`device-lan-control-regression.json` 49项均通过。此次H264 TextureView更新约50/s、解码回调约60.1/s；JPEG约10.4/s。画面内容/源分辨率/采集条件未固定，不据此断言性能回退或提高，JPEG低帧率仍是已知降级限制。

## 复测

```powershell
.\build.ps1 -Package
python tests/device_debug.py --adb C:/tools/platform-tools/adb.exe --serial DEVICE_SERIAL --host-exe package/lantern.exe --apk ../Lantern-0.1-Android-debug.apk --address HOST_LAN_IP --work C:/LanternTests --report fullscreen-control.json --control-only --input-target BUILD/Release/lantern-input-target.exe
```

不要让其他窗口覆盖测试画布；可按Esc中止，测试会报告取消。诊断PID保护仅限带显式 --input-guard-pid 参数的测试主机，不改变正常客户端权限。测试运行只访问生成数据，不将桌面截图、身份资料或临时码流打包。

尚未验证所有DPI配置、多显示器跨屏、旋转主机显示器或输入光学延迟；上述是本设备有限回归。独立系统音频仍在下一阶段，本次没有把音频空壳算成实现。
