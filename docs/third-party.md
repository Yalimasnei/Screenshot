# 第三方资料与运行时依赖

## NVIDIA 编码接口头文件

- 文件：`third_party/nv-codec-headers/nvEncodeAPI.h`。
- 来源：FFmpeg 的 `nv-codec-headers` 项目，路径 `include/ffnvcodec/nvEncodeAPI.h`；文件内版权为 NVIDIA Corporation，2010–2026。
- 当前接口版本宏：13.1。该值是头文件接口版本，不是程序版本或已验证的最低驱动版本。引入时的精确上游提交尚未固定，不能据此声明与某个 SDK 发布包逐字节一致。
- 许可：MIT，完整原文保留在该文件开头。项目的 GPL-3.0-only 声明不替换此文件的许可和版权。
- 本仓库不包含 NVIDIA 驱动、编码运行时 DLL 或 SDK 工具。运行时从系统驱动加载 NVENC。

## Windows 接口

程序调用 Windows、Direct3D / DXGI、WIC、GDI+、Task Scheduler 和 Windows Runtime OCR。构建使用 llvm-mingw 提供的接口头文件与导入库；这些系统和工具链组件不随源码包分发。

`src/encoding/wic_heif.h` 与 `src/platform/text_recognition.cpp` 中的最小接口声明用于调用 Windows API，按 Windows SDK 接口定义与 Windows Runtime 元数据的 UUID 和方法顺序声明。本仓库不包含从系统应用导出的 XAML、图片或程序二进制。

FFmpeg、libheif 等独立研究或解码工具不是本程序的运行依赖，未纳入源码仓库。系统编码器、驱动和其他外部组件仍受其各自条款约束。
