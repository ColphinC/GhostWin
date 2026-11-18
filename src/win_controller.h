#pragma once

#include <windows.h>
#include <unordered_map>

struct WindowState {
    bool initialLayered = false;
    bool initialTopmost = false;
    bool initialTransparent = false;

    int opacityPercent = 100;  // 5 ~ 100
    bool topmost = false;
    bool clickThrough = false;
    bool accessDenied = false;
};

class WinController {
public:
    static constexpr int kMinOpacityPercent = 5;
    static constexpr int kMaxOpacityPercent = 100;

    // 查询当前窗口在控制层中的状态（若未修改过则读取实时样式）
    WindowState GetWindowState(HWND hwnd) const;

    // 设置窗口透明度（自动限制在 5% ~ 100%）
    bool SetOpacity(HWND hwnd, int percent);

    // 在当前透明度基础上增减步长（用于全局快捷键）
    bool AdjustOpacity(HWND hwnd, int deltaPercent);

    // 设置窗口置顶状态
    bool SetTopmost(HWND hwnd, bool topmost);

    // 设置鼠标穿透状态（WS_EX_TRANSPARENT）
    bool SetClickThrough(HWND hwnd, bool enabled);

    // 重置单个窗口到初始状态（100% 不透明、取消置顶与穿透）
    bool ResetWindow(HWND hwnd);

    // 恢复所有已修改窗口的原始状态
    void RestoreAll();

    // 清理已失效的窗口句柄记录
    void PruneInvalidWindows();

    // 是否有窗口被修改过
    bool HasModifiedWindows() const;

    // 检查当前进程是否以管理员权限运行
    static bool IsProcessElevated();

    // 以管理员身份重启当前程序（返回 true 表示已拉起新进程）
    static bool RestartAsAdmin();

private:
    WindowState& EnsureTracked(HWND hwnd);
    bool ApplyExtendedStyle(HWND hwnd, LONG_PTR newExStyle, WindowState& state);
    void MaybeUntrackIfClean(HWND hwnd);

    std::unordered_map<HWND, WindowState> states_;
};
