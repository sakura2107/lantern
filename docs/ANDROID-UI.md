# Android 界面（2026-10 重写）

本次只重写表现层。协议、权限、TLS、任务 journal、编解码、SAF 和线程所有权都没有改动；业务调用仍走 `Remote`、`FileBrowser`、`TransferQueue`、`AvcDesktop`、`SystemAudio`、`DesktopTools`、`Discovery` 和 `Secrets`。

## 设计语言

- **底色与墨色**：冷灰底（`#EEF1F4`）、白色卡片、蓝黑墨色（`#1B2430`）。主要操作用墨色实心按钮，次要操作用浅灰底按钮。
- **一盏灯**：琥珀色 `#F3A223` 只表示"正在工作"：已连接、画面正在接收、控制已开启、声音正在播放、传输进行中。设备页顶部的 `Lamp` 是整套界面唯一的表现性元素：未连接时熄灭，连接中呼吸闪烁，已连接时点亮，失败时显示红圈。
- **形状**：卡片用 1dp 细线、不加阴影；按钮是胶囊形。圆角按层级区分：页面卡片 22–24dp，列表行 16dp，输入框 14dp。
- **深色模式**：跟随系统，颜色 token 见 `Theme.java`。
- **图标**：`Glyph.java` 在 24 单位网格上绘制约 60 个线性图标，不依赖资源文件。
- **应用图标**：墨色圆角方块里的白色金属提灯和琥珀火焰，母版是 `assets/lantern-icon.svg`。
  - **自适应图标**：`res/mipmap-anydpi-v26/ic_launcher{,_round}.xml`。背景是 `drawable/ic_launcher_background.xml`（墨色渐变），前景是 `drawable/ic_launcher_foreground.xml`（提灯和光晕，按 72/108 缩进安全区）。两者都是由 SVG 手工转写的矢量图。
  - **为什么要自适应图标**：ColorOS 会把只有 PNG 的旧式图标缩小后放在白底上。
  - **PNG 备用**：`res/mipmap-{m,h,xh,xxh,xxxh}dpi/ic_launcher{,_round}.png` 只作备用。
  - **构建**：两个构建脚本都会用 aapt2 编译 `res/` 并链接。
- **构建方式**：
  - **完整构建**：`android/build-standalone.ps1`，需要 NDK。
  - **免 NDK 的 debug 构建**：`android/build-repack.ps1 -Tools <android-tools> -BaseApk <旧 debug APK> -Keystore <debug.keystore> [-Install]`。
    - 编译 Java，用 aapt2 链接清单、`resources.arsc` 和 `res/`。
    - `android/tools/Repack.java` 保留旧 APK 的原生库和 assets，换入新的 classes.dex 与资源。
    - 最后 `zipalign -P 16` 并签名。
    - Java 工具打不开含中文的源码路径，所以所有文件先复制到 `%TEMP%\lantern-repack` 再处理。
- **文案**：集中在 `S.java`，统一使用句子式中文。错误提示写明原因和下一步。协议层、传输层的消息原样显示，仍保留在各自的类里。

## 结构

| 文件 | 作用 |
| --- | --- |
| `AppShell.java` | 外壳：标题栏、连接状态胶囊、四个页面、底部导航（含传输数量角标）、自动消失的提示条 |
| `DevicePage.java` | 连接灯和状态卡；可折叠的配对信息（地址+发现、指纹+粘贴+格式校验、密钥+显示/隐藏）；网络诊断、通知、开源许可 |
| `DesktopPage.java` | 预览区（视频面板对齐到这里）；四个独立开关：画面、控制、声音、隐藏本机画面；工具：全屏、键盘、画质、显示器、剪贴板、快捷键、时序，以及标为"不支持"的隐私屏。横屏时预览和控制左右排列 |
| `DesktopLayout.java` | 全屏：顶部一条可横向滚动的悬浮工具栏，可收起；收起后右侧中部留一个"工具"把手，提示条在底部居中、自动淡出 |
| `FilesPage.java` | 面包屑、搜索、排序、多选（长按进入）、每项的"⋯"操作面板、主机操作进度横幅（可取消）、上一页/下一页、上传悬浮按钮、几种空状态 |
| `TransfersPage.java` | 队列概览（速度、生效上限、往返延迟）、限速、全部暂停/继续、清理；全部/未完成/已结束筛选；任务卡片显示状态标签、进度、速度和预计剩余时间，并提供一个快捷操作 |
| `Sheet.java` | 底部面板，统一承载操作列表、单选、确认和文本输入，替代 `AlertDialog` |
| `Tile.java`、`Lamp.java`、`Ui.java`、`Theme.java`、`Glyph.java`、`S.java` | 组件、token、图标和文案 |

## 行为说明

- **三种"不显示"分开表述**：
  - "画面"开关：订阅或停止，停止后电脑不再采集；
  - "隐藏本机画面"：电脑继续发送，只是手机不显示；
  - "主机隐私屏"：明确标为不支持。
- **发现电脑不建立信任**：选中发现结果只会填入地址。随后的面板把对方声明的指纹按 4 位一组展示，只有用户点"一致，使用此指纹"才会填入；如果与已保存的指纹不同，会用红色警示。
- **指纹输入**：去掉空格、冒号、连字符并转成小写后，如果正好是 64 位十六进制就采用规范形式，否则原样提交，交给原有校验。
- **传输**：
  - 新增 `TransferQueue.remove`：只能移除"完成"或"已取消"的任务，并清理该任务的私有缓存；
  - 新增 `moveToTop` 和 `openDone`，是从原来的弹窗代码中抽出来的；
  - `clearCompleted` 复用同一个缓存清理函数；
  - 用户设定的限速保存在 `device` 偏好中。
- **复制/移动到根目录**：目标路径填 `/` 表示根目录。原来的实现会拼出 `"/name"`，而且空输入不会执行，所以这个场景以前无法完成。
- **其他小改动**：
  - 画面循环出错时会把开关状态同步回界面，原来按钮文字不变，现在开关能正确熄灭；
  - 未连接时点"开启画面"会直接提示，不再进入失败循环。

## 自动化标识

控件通过 `Ui.id(view, "…")` 设置稳定标识（view tag key `0x7f4c0001`），用 `Ui.find(root, id)` 查找，不依赖显示文字。`DeviceTestRunner.click()` 把原来的步骤名映射到这些标识，并用 `isActivated()` 检查开关的预期状态（例如"控制：开"要求控制开关已激活）。主要标识：

`device.connect`、`device.host`、`device.fingerprint`、`device.secret`、`device.discover`、`nav.device|desktop|files|transfers`、`desktop.subscribe`、`desktop.control`、`desktop.audio`、`desktop.hide`、`desktop.fullscreen`、`full.rotate`、`full.collapse`、`full.tools`、`full.exit`、`files.upload`、`files.search`、`files.sort`、`files.select`、`files.cancel`、`transfers.limit`、`transfers.pauseall`、`transfers.clear`。

## 验证状态

- 已完成：用 `javac --release 17` 对照 Android 35 `android.jar` 编译全部 main 和 debug 源码，通过。
- 真机安装与截图：见交付报告。
- 2026-10-15 真机（PKR110，系统字体放大）修正：
  - 底部导航文字改为居中在图标下方；
  - 竖屏桌面预览固定为 16:9，且不低于 220dp，下方控件放进可滚动区域，空状态按钮不再被截断；预览区过矮时（如横屏）隐藏图标；
  - 桌面工具网格去掉了第二行状态小字（画质、显示器、时序、隐私屏），图标和名称上下对齐成两整排；
  - 传输页三个按钮按文字宽度分配空间，限速按钮只显示“限速”，当前上限仍在上方的“生效上限”里，无障碍描述保留具体数值。
  - 已装机截图，四个页面无崩溃。旋转和全屏手势仍需手动检查。
- 2026-10-15 第二轮：
  - 全屏工具条的文字改为居中在图标下方；
  - 新增右键：
    - 工具条“右键”按钮（标识 `full.right`）点亮后，下一次点按发送右键，发送后自动熄灭；关闭控制时也会熄灭；
    - 控制开启时，双指轻点（300 ms 内、位移不超过 12dp、未缩放）也会在两指中点发送右键；
    - 协议使用主机已有的 Op 52 `right_click`。

  - **连接与断开**：
    - **“断开连接”按钮**（标识 `device.disconnect`）：停止画面、声音、控制和文件浏览，暂停传输，配对资料保留，点“连接电脑”即可恢复。
    - **在线连接**：连接成功后保持一条空闲控制连接，每 10 秒 ping 一次，“已连接”因此反映真实状态。连接中断时显示“连接中”并自动重试。
    - **电脑结束连接**：收到“电脑已结束此设备的连接”（任何通道都算）时，直接切换到“未连接”，不再自动重连。
    - **主动连接**：“连接电脑”使用的是主动连接（不带自动重连标志）。
    - **真机实测**：结束会话后手机显示已断开；手动重连成功；点“断开连接”后主机上的连接数为 0。
  - 文件页列表加载失败时，显示“无法读取这个文件夹”、失败原因和“重试”，不再显示成“这个文件夹是空的”。

  已装机，并在真机上确认工具条文字居中、右键按钮显示。当时手机连不上主机，所以右键还没有对着电脑实测。
- 2026-10-15 应用图标：
  - 用 `build-repack.ps1` 构建，并以 `adb install -r` 覆盖安装到 PKR110（Android 16）。签名不变，数据保留。
  - `aapt2 dump badging` 显示 `application-icon` 指向 `ic_launcher`。
  - 桌面上显示为完整的墨色圆角提灯图标，与其他图标同尺寸。
  - 应用冷启动正常。
- 未重新运行：完整的 `tests/device_debug.py` 真机套件需要测试主机和夹具，本次没有重跑，不计为通过。
