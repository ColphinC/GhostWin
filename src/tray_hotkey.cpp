#include "tray_hotkey.h"

namespace {

constexpr UINT kMenuIdShow = 1001;
constexpr UINT kMenuIdRestoreAll = 1002;
constexpr UINT kMenuIdExit = 1003;

}  // namespace

TrayHotkeyManager::~TrayHotkeyManager() {
    Shutdown();
}

bool TrayHotkeyManager::Initialize(HWND hwnd) {
    hwnd_ = hwnd;
    if (!hwnd_) {
        return false;
    }

    nid_ = {};
    nid_.cbSize = sizeof(NOTIFYICONDATAW);
    nid_.hWnd = hwnd_;
    nid_.uID = 1;
    nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid_.uCallbackMessage = kTrayCallbackMsg;
    nid_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid_.szTip, L"GhostWin 窗口透明度工具");

    trayAdded_ = (Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE);

    const bool upOk = RegisterHotKey(hwnd_, kHotkeyOpacityUp, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_UP) == TRUE;
    const bool downOk = RegisterHotKey(hwnd_, kHotkeyOpacityDown, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_DOWN) == TRUE;
    const bool resetOk = RegisterHotKey(hwnd_, kHotkeyOpacityReset, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'R') == TRUE;
    hotkeysRegistered_ = upOk && downOk && resetOk;

    return trayAdded_;
}

void TrayHotkeyManager::Shutdown() {
    if (hwnd_) {
        UnregisterHotKey(hwnd_, kHotkeyOpacityUp);
        UnregisterHotKey(hwnd_, kHotkeyOpacityDown);
        UnregisterHotKey(hwnd_, kHotkeyOpacityReset);
        hotkeysRegistered_ = false;
    }

    if (trayAdded_) {
        Shell_NotifyIconW(NIM_DELETE, &nid_);
        trayAdded_ = false;
    }

    hwnd_ = nullptr;
}

bool TrayHotkeyManager::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam, const Callbacks& callbacks) {
    if (msg == kTrayCallbackMsg) {
        const UINT event = LOWORD(lParam);
        if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) {
            if (callbacks.onShowWindow) {
                callbacks.onShowWindow();
            }
            return true;
        }
        if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
            ShowContextMenu(callbacks);
            return true;
        }
        return false;
    }

    if (msg == WM_HOTKEY) {
        switch (static_cast<int>(wParam)) {
            case kHotkeyOpacityUp:
                if (callbacks.onForegroundOpacityDelta) {
                    callbacks.onForegroundOpacityDelta(+10);
                }
                return true;
            case kHotkeyOpacityDown:
                if (callbacks.onForegroundOpacityDelta) {
                    callbacks.onForegroundOpacityDelta(-10);
                }
                return true;
            case kHotkeyOpacityReset:
                if (callbacks.onForegroundOpacityReset) {
                    callbacks.onForegroundOpacityReset();
                }
                return true;
            default:
                return false;
        }
    }

    return false;
}

void TrayHotkeyManager::ShowContextMenu(const Callbacks& callbacks) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kMenuIdShow, L"显示主界面");
    AppendMenuW(menu, MF_STRING, kMenuIdRestoreAll, L"恢复所有窗口");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuIdExit, L"退出");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);

    const UINT cmd = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);

    switch (cmd) {
        case kMenuIdShow:
            if (callbacks.onShowWindow) {
                callbacks.onShowWindow();
            }
            break;
        case kMenuIdRestoreAll:
            if (callbacks.onRestoreAll) {
                callbacks.onRestoreAll();
            }
            break;
        case kMenuIdExit:
            if (callbacks.onExit) {
                callbacks.onExit();
            }
            break;
        default:
            break;
    }
}
