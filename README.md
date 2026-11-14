# Screenshot

Screenshot 是一个 Windows 截图工具，使用 C++17、Win32、Direct3D 11 和 GDI+ 开发，实在受不了 Windows 截图工具才vibe了这玩意出来。

## 功能

- 矩形、窗口、全屏和任意形状截图
- 文字识别和取色器
- HDR 桌面捕获与 SDR 输出映射
- 编辑器：画笔、荧光笔、橡皮擦、裁剪、撤销和重做
- 自动保存、剪贴板复制和截图通知
- PNG、Windows HEIC、NVENC HEIC 和 JPEG 输出
- 中文、English、浅色、深色和跟随系统设置
- 可选屏蔽 Windows 截图快捷键
- 管理员启动和当前用户的高权限开机启动任务

## 运行

要求管理员权限运行，省的某些应用截不了图。

启动后程序常驻系统托盘：

- `PrtScn` 或 `Win+Shift+S`：打开截图界面
- `Esc` 或右键：取消当前截图
- 单击截图通知：打开编辑器
- 编辑器快捷键：`Ctrl+C` 复制，`Ctrl+S` 保存，`Ctrl+Z` 撤销，`Ctrl+Y` 重做

任意形状截图在 PNG 中保留轮廓外透明区域；剪贴板同时提供 PNG 和 `CF_DIB` 位图，位图中的轮廓外区域使用白色填充。

设置会即时保存到程序目录下的 `Screenshot.ini`。日志写入 `%TEMP%\Screenshot.log`。默认保存位置为系统的“屏幕截图”文件夹，也可以在设置中更改。

## 构建

构建环境：

- Windows
- PowerShell 7
- x86-64 llvm-mingw 工具链

`clang++.exe` 和同一工具链目录下的 `llvm-windres.exe` 必须可用。工具链在 `PATH` 中时可以直接运行：

```powershell
pwsh -File .\scripts\build.ps1
```

也可以显式指定编译器：

```powershell
pwsh -File .\scripts\build.ps1 -Compiler 'C:\path\to\clang++.exe'
```

构建输出为 `bin\Screenshot.exe`，中间文件位于 `build\`。这些目录是本地产物，不属于源码目录，也不会被源码导出脚本包含。

程序通过 Windows 系统组件使用 HEIF 编解码器和 OCR，通过 NVIDIA 驱动使用 NVENC；这些运行时组件不会随源码发布。

## 保存格式与精度


| 格式         | 实现                    | 位深与采样                                    | 透明度                         |
| ------------ | ----------------------- | --------------------------------------------- | ------------------------------ |
| PNG          | Windows WIC             | 每通道 8 位，无损压缩                         | 支持                           |
| Windows HEIC | Windows WIC HEIF 编码器 | 8 位，4:2:0                                   | 使用系统辅助帧接口             |
| NVENC HEIC   | NVIDIA HEVC 编码器      | 按捕获数据使用 8 或 10 位，可选 4:2:0 / 4:4:4 | 透明内容使用 8 位 4:2:0 辅助图 |
| JPEG         | Windows WIC             | 每通道 8 位，有损压缩                         | 透明区域铺白                   |

PNG 是唯一保持原始 RGB 像素的无损输出。NVENC 使用 HEVC 无损编码设置，但仍会经过 RGB 到 Y'CbCr 的转换、量化和色度抽样，因此不能视为 PNG 的逐像素替代。Windows HEIC 和 JPEG 都是有损格式。

NVENC 只在设备通过 HEVC、无损编码、输入格式、位深和尺寸能力检查后开放。请求的位深或色度采样不可用时，编码器会回退到 8 位 4:2:0；编码失败或解码校验失败时，自动保存会回退到 PNG。

捕获过程会保留可用的高精度像素。PNG、Windows HEIC、JPEG 和剪贴板使用 8 位输出路径；NVENC 根据捕获数据使用 8 或 10 位。HDR 桌面会先转换为 SDR，文件中不保存原始 HDR 帧。

## 快捷键拦截

启用“运行时屏蔽 Windows 截图快捷键”后，程序会在运行期间接管相关截图组合键，包括 `Win+Shift+S`、`Win+Shift+R`、`PrtScn`、`Win+PrtScn` 和 `Alt+PrtScn`。程序退出后 Windows 原有行为恢复。Xbox Game Bar 等其他快捷键不保证能够拦截。

## 源码目录

```text
Screenshot/
├── src/
│   ├── app/         入口、应用状态、设置和快捷键
│   ├── capture/     桌面捕获、像素转换和 HDR 映射
│   ├── core/        公共类型和图像操作
│   ├── encoding/    PNG、JPEG、HEIC 和 NVENC 输出
│   ├── platform/    Windows 路径和文字识别
│   └── ui/          选框、编辑器、设置和通知
├── resources/       管理员清单和版本资源
├── scripts/         构建和源码导出脚本
├── third_party/     NVIDIA NVENC API 头文件
├── docs/            第三方许可说明
├── LICENSE
└── README.md
```

## 源码导出

使用以下命令生成经过清单校验的源码包：

```powershell
pwsh -File .\scripts\export-source.ps1 -OutputZip 'C:\path\Screenshot-source.zip'
```

目标 ZIP 必须不存在。脚本会重新读取压缩包并核对每个文件的大小和 SHA-256。

## 限制

- 不捕获鼠标指针，不提供录屏和延时截图。
- 受保护内容可能无法捕获或显示为黑屏。
- OCR 依赖 Windows 已安装的语言包。
- HDR 画面经过 SDR 映射后保存，屏幕观感会受显示器、系统亮度设置和查看器影响。
- HEIC 的实际显示效果和透明度支持取决于显示器。

## 许可证

本项目基于 [GNU General Public License v3.0](LICENSE)。

`third_party/nv-codec-headers/nvEncodeAPI.h` 保留其原有 MIT 许可证，具体来源和许可边界见 [docs/third-party.md](docs/third-party.md)。
