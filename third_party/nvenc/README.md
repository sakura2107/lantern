# NVENC 接口头文件

来源：FFmpeg 项目维护的 nv-codec-headers，固定标签 n12.2.72.0。
原始地址：https://raw.githubusercontent.com/FFmpeg/nv-codec-headers/n12.2.72.0/include/ffnvcodec/nvEncodeAPI.h
SHA256：4677a397e3ec5300a6b38bf49cba42bb63a922ab26f24bc63a05ed08857cba16
头文件版权 NVIDIA 2010–2024，MIT，原始通知完整保留，分发副本见 licenses/NVENC-HEADER-MIT.txt。

只编译接口声明，不捆绑 NVIDIA SDK 样例库、CUDA 运行库或编码驱动 DLL。
Windows 提供者从 System32 加载用户已安装的 nvEncodeAPI64.dll，检查驱动 API 兼容性。
需要 NVIDIA GPU 驱动当前显示器；不满足时返回 unsupported，Android 明确提示并降级 JPEG。
