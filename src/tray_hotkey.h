#pragma once

#include <windows.h>
#include <shellapi.h>

#include <functional>

class TrayHotkeyManager {
public:
    static constexpr UINT kTrayCallbackMsg = WM_APP + 1;

    static constexpr int kHotkeyOpacityUp = 1;
    static constexpr int kHotkeyOpacityDown = 2;
    static constexpr int kHotkeyOpacityReset = 3;

    struct Callbacks {
        std::function<void()> onShowWindow;
        std::function<void()> onRestoreAll;
        std::function<void()> onExit;
        std::function<void(int deltaPercent)> onForegroundOpacityDelta;
        std::function<void()> onForegroundOpacityReset;
    };

    TrayHotkeyManager() = default;
    ~TrayHotkeyManager();

    TrayHotkeyManager(const TrayHotkeyManager&) = delete;
    TrayHotkeyManager& operator=(const TrayHotkeyManager&) = delete;

    // 初始化托盘图标与全局快捷键
    bool Initialize(HWND hwnd);
    void Shutdown();

    // 处理托盘与全局快捷键消息，已处理返回 true
    bool HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam, const Callbacks& callbacks);

    bool AreHotkeysRegistered() const { return hotkeysRegistered_; }

private:
    void ShowContextMenu(const Callbacks& callbacks);

    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW nid_{};
    bool trayAdded_ = false;
    bool hotkeysRegistered_ = false;
};
