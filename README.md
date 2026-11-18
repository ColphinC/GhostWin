# GhostWin

一款轻量级 Windows 桌面窗口透明度管理工具（C++17 / Dear ImGui / DirectX 11），**专为 Electron 应用稳定适配**，可实时调节任意桌面软件的窗口透明度、置顶与鼠标穿透状态。

## 为什么做这个工具？

Electron / Chromium 应用（如 **VS Code、Discord、飞书、Notion、Obsidian、Slack、Telegram** 等）通常会启动多个子进程（Main、Renderer、GPU、Utility）并创建多个隐藏或无标题辅助窗口，很多同类工具直接按进程操作极易误改句柄或完全无效。

本工具严格对齐 Windows `Alt+Tab` 顶层可见窗口判定规则，精准命中 Electron 主宿主窗口，并在恢复 `100%` 不透明时自动清理附加的 `WS_EX_LAYERED` 样式，避免给 Chromium 渲染窗口残留合成开销或阴影异常。

## 核心特性

- **专为 Electron / Chromium 优化**：精准过滤 `Chrome_WidgetWin_0` 等后台辅助窗口与内部渲染子窗口，稳定控制主窗口透明度。
- **实时透明度调节（5% ~ 100%）**：支持滑块平滑调节；恢复 `100%` 时自动还原窗口原始 `WS_EX_LAYERED` 属性。
- **窗口置顶与鼠标穿透**：支持单窗口置顶（`HWND_TOPMOST`）与鼠标穿透（`WS_EX_TRANSPARENT`）一键开关，适合将半透明参考窗口悬浮在工作区上方。
- **全局快捷键**：无需切回工具界面，直接控制当前焦点窗口：
  - `Ctrl + Alt + Up`：透明度 `+10%`
  - `Ctrl + Alt + Down`：透明度 `-10%`
  - `Ctrl + Alt + R`：恢复 `100%` 不透明
- **系统托盘与低功耗运行**：关闭窗口自动最小化到系统托盘，隐藏或最小化时挂起界面渲染；支持单实例运行（重复打开自动唤醒已有窗口）与退出时自动恢复所有窗口。
- **权限隔离提示（UIPI）**：遇到以管理员身份运行的软件时自动提示“权限不足”，支持一键以管理员身份重启本工具。
- **单文件零依赖**：开启 `/MT` 静态链接 CRT，编译产物仅单个 `.exe`，无需安装任何外部运行库。

## 编译构建

### 环境要求

- Windows 10 / 11
- CMake 3.20+
- Visual Studio 2019 / 2022 或更高版本（需勾选“使用 C++ 的桌面开发”与 Windows SDK）

### 构建命令

```powershell
cmake -B build
cmake --build build --config Release
```

编译输出路径：`build/Release/GhostWin.exe`

## 开源协议

本项目采用宽松的 [MIT License](LICENSE) 开源协议。

### 第三方开源组件

- [Dear ImGui](https://github.com/ocornut/imgui)（MIT License）
