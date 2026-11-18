#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct WindowInfo {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring exePath;
    std::string processNameUtf8;
    std::string titleUtf8;
    std::string classNameUtf8;
};

// 枚举当前符合 Alt+Tab 规则的顶层可见窗口（排除自身及桌面壳窗口）
std::vector<WindowInfo> EnumerateAppWindows(HWND selfHwnd);

// 按优先级提取窗口图标：WM_GETICON -> GetClassLongPtrW -> SHGetFileInfoW
// 若返回图标且 outOwnsIcon 为 true，调用方使用完毕后需调用 DestroyIcon
HICON ExtractWindowIcon(HWND hwnd, const std::wstring& exePath, bool& outOwnsIcon);

// UTF-16 转 UTF-8 辅助函数
std::string WideToUtf8(const std::wstring& wstr);
