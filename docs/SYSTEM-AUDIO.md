# 独立系统音频首版

Windows主机WASAPI渲染端点loopback → 独立TLS音频通道 → Android AudioTrack，已实现真实采集与手机播放，文件页可以只听音频，不必订阅桌面。默认关闭，需要单独audio=16权限；全屏“工具”同样有音频开关。当前仅Android播放器，Windows客户端的音频播放界面待实现。

## 使用与格式

新建主机配置时显式选择权限集合：`lantern init STATE AUTHORIZED_ROOT 31` 包含文件读/写、画面、控制与音频；16只有音频。Android先独立验证身份，无文件读取权限时显示已配对但无文件授权，音频仍可独立使用。已有15配置继续拒绝音频，不会自动扩大授权；用 `lantern grant STATE 31` 显式替换允许的权限集合，0拒绝全部。活动流复查权限。配置采用DPAPI保存，凭据不能写入脚本/日志。

Android配对后点击“音频：关”，显示连接中，实际WASAPI与AudioTrack配置成功后才显示“音频：开”。可以不订阅画面，独立停止音频，也可与桌面、文件同时使用。应用离开前台或丢失音频焦点会停止，尚无后台音频前台服务/锁屏持续播放。

首版传送PCM16LE双声道，不是AAC/Opus压缩；48kHz有效PCM码率1.536Mbps，不包括TLS/协议开销。保留Windows默认输出的采样率：支持8–96kHz、100的整数倍，以及单/双声道PCM16/24/32或float32；单声道复制为双声道。多声道、特殊格式、端点变化或系统拒绝时明确失败，没有伪造静音成功。默认输出设备之外的应用路由、受保护媒体、独占音频可能不可捕获。后续可替换提供者加入Opus/AAC和端点选择。

## 资源与失败恢复

内置AudioSource接口在同一主机工作线程拥有COM/WASAPI对象，10ms帧、捕获积压上限200ms。无系统采样可取时，每100ms发送空的idle心跳，区别于实际采样的全零帧。发送缓冲32KiB、单次发送超时200ms；最多2路音频会话，与最多2路桌面共享固定8个连接工作线程。每250ms复核音频授权；捕获和设备失败结束会话。

Android一条独立网络/输出线程、一个在途PCM包；AudioTrack请求80ms缓冲（设备最小要求若大于250ms则拒绝），约60ms预缓冲后播放。写入非阻塞并检查取消/250ms超时；相对源时间戳积压超过250ms时结束，不排队播放过期音频。开始/停止以新请求对象区分，快速关开不会复用旧会话。停止先abort原始socket解除I/O，后台释放AudioTrack和音频焦点；不在UI上写TLS close_notify。

该实现仍是可靠TLS/TCP，尚无音频datagram/丢包隐藏、压缩编码、自动网络切换重连或与视频的共同呈现时钟。当前音画可同时播放，但不能宣称同步目标已达成。系统采集不访问麦克风；产品不保存录音。用于验证的PCM能量/频率统计只在显式debug测试中开启，不保存样本。

## 真实验证

条件：Windows11 / i5-14600KF / RTX4070SUPER，用户一加Ace5Pro Android16，同物理Wi-Fi LAN。测试程序在Windows系统输出播放自己生成的997Hz、4%幅度正弦，客户端从真实loopback检测频率能量变化；AudioTrack驱动时间戳的播放帧数同时前进。未操作个人播放器或录制个人音频。测试结果见 `../system-audio.json`；构建时的协议测试包括音频独立授权、截断、版本、尾随字节、错误通道及活动连接撤销。

最终音频复测 `system-audio.json`：13项通过，含快速关闭/开启会话；48000Hz双声道，有效PCM约1.536Mbps；4秒稳态AudioTrack实际驱动帧数增加192000，underrun增加0。音频/H264/32MiB下载同时工作，下载约2.37秒，短测underrun增加0。停止约13.29ms。

补充真机UI检查 `audio-only-ui.json`：在生成测试主机上仅授予16，重新配对成功且文件页明确提示未授予读取；独立音频正常开启/停止。恢复31后文件页恢复。全屏内控制、键盘与音频按钮均可见，检查结束保持控制/音频关闭。

这是有限短测，不是声学端到端延迟、音画同步、丢包/移动网络、蓝牙延迟、温度或长稳证明。没有第二台Windows播放端实测。

## 复现

```powershell
python tests/device_debug.py --adb C:/tools/platform-tools/adb.exe --serial DEVICE_SERIAL --host-exe package/lantern.exe --apk ../Lantern-0.1-Android-debug.apk --address HOST_LAN_IP --work C:/LanternTests --report system-audio.json --audio-only --audio-tone BUILD/Release/lantern-audio-tone.exe
```

仅测试构建包含tone工具；不会放入主机安装包。参考：[WASAPI loopback](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)、[AudioTrack](https://developer.android.com/reference/android/media/AudioTrack)。


2026-10-14 后续实现与验证已更新：[文件、媒体、音频交付报告](FILES-MEDIA-AUDIO.md)。早期“未实现”条目请与该报告对照；完整产品仍未完成。
