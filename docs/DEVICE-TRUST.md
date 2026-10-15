# 逐设备授权、撤销与会话终止（2026-10-14）

主机现可为每个客户端签发独立随机凭据、分别调整权限和撤销，不再要求它们共用一个密钥。设备名称/ID/凭据/权限保存在Windows用户DPAPI保护的配置中；设备列表不输出凭据。Android与Windows客户端仍使用原来的指纹+密钥配对入口，协议版本1兼容。这里的设备身份是独立配对凭据，不是硬件指纹或不可复制的设备证明。

## 使用

先升级并重新启动本包Windows主机，继续使用原状态目录；不需要重新初始化或更换授权目录。新版能读取旧v1配置，第一次修改授权后写为v2；旧主机不能读取v2，因此不要在旧主机仍运行时用新CLI修改授权。

为手机签发仅允许文件读取的凭据：

```powershell
.\lantern.exe device-add C:\LanternState MyPhone 1
```

命令仅在本机显示设备ID、主机指纹及该设备配对密钥。手机设备页填写这组指纹和密钥，正常连接即可浏览根目录/用手机播放器打开动漫。权限值仍为读取1、写入2、观看4、控制8、音频16，可明确相加；设备权限不能超过主机总体权限上限。需要全部能力时，主机管理员先明确配置上限31，再给相应设备31。

```powershell
.\lantern.exe device-list C:\LanternState
.\lantern.exe device-grant C:\LanternState DEVICE_ID 17
.\lantern.exe device-remove C:\LanternState DEVICE_ID
.\lantern.exe device-end C:\LanternState DEVICE_ID
```

device-grant替换该设备权限，17是只读文件+系统音频；device-remove永久删除该凭据并结束其连接，其他设备保持可用。device-end结束该设备当前的全部逻辑连接与关联主机任务，保留配对；开启自动恢复的客户端可能重新连接。要持续禁止访问，请撤销设备或取消其权限，不能用断开连接代替撤销信任。主机每个请求复核授权，流/空闲连接有约250ms的授权巡检周期；不是光学中断延迟承诺。

撤销阻止后续远程访问，不抹除客户端之前已接收的文件或缓存。

所有客户端换成独立凭据后可禁用旧的共享密钥：

```powershell
.\lantern.exe legacy-off C:\LanternState
```

已有命名设备不受影响。旧配对默认保留是迁移兼容策略；共享密钥尚未禁用时，持有它的旧客户端仍共享原权限。`revoke STATE`是全局撤销：清空设备凭据、轮换旧密钥并重新开启新的迁移密钥。Windows CLI的设备凭据文件可用 `device-secret STATE DEVICE_ID LOCAL-DPAPI-FILE`生成，只能由当前Windows用户解密；不把密钥放在命令行参数。

## 实现与实际验证

Config-v2最多64条设备记录，校验ID/凭据/权限/名称、拒绝重复记录和尾随数据，配置密文上限1MiB。原子替换写入，读取使用允许删除共享的稳定文件句柄快照；本地编辑由命名互斥锁串行化，避免同时签发导致记录丢失。设备会话有持久递增generation；结束会话后旧连接在下一次操作或巡检时拒绝继续使用。连接、输入租约和主机任务各自释放。FileJobs按每个所有者授权，不把设备A的撤销误应用到B。

| 检查 | 实测结果 |
|---|---|
| Windows构建与回归 | 8个ctest套件全部通过 |
| Native安全闭环 | device-trust-native.json，22项：DPAPI密文、旧配置迁移、重复记录不覆盖、两个线程并发16次编辑不丢记录、两条真实TLS连接分别读/写、禁用共享凭据、单设备撤销/会话终止、另一条既有连接继续工作、权限降级与文件哈希 |
| Android真机授权 | device-trust-android.json，12项：手机使用独立凭据实际创建目录；主机调整到只读后写权限被拒；旧TLS会话实际中断、新连接仍可配对；撤销后即使请求0文件权限也拒绝身份；另一个Windows凭据仍可读取生成文件 |

Native条件为本机Windows真实Schannel TLS回环；Android为OnePlus Ace5Pro PKR110/Android16，与主机WLAN连接，USB只用于调试。没有把两个凭据测试说成两台安卓真机测试。用的全是生成文件，用户原配对与任务日志在测试结束恢复；没有修改个人Anime授权状态。

新增tests/device_trust.cpp与device_debug.py的`--device-trust-only`可复现。测试中发现配置热替换与读取需要稳定快照，已调整为Windows共享删除句柄；连续回归通过。一次Android测试失败来自调试命令的远端shell引号，修复后12项通过，失败报告保留。设备管理目前是主机CLI，尚无主机图形化授权管理或二维码/短码向导。

后续缺口：限时配对凭据、设备公钥绑定/二次验证、完整连接列表管理、无人值守图形配置与登录/锁屏/安全桌面支持。公网穿透、中继、短期只读分享尚未接入。完整剩余模块见REMAINING.md；不要把本次独立凭据当作所有安全功能完成。

2026-10-14 后续交付：本机Windows主机/授权/连接管理已接通，HostAdministration API-v1及本地无凭据HostSession接口，不增加网络管理员协议。安卓真Wi-Fi补测授权12项、文件/外部打开37项通过；本轮只重跑受影响的3套原生测试。当前使用与限制以HOST-MANAGER.md为准；旧性能数字不视为本轮复测。
