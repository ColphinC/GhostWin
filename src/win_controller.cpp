#include "win_controller.h"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace {

BYTE PercentToAlpha(int percent) {
    const int clamped = std::clamp(percent, WinController::kMinOpacityPercent, WinController::kMaxOpacityPercent);
    return static_cast<BYTE>(std::lround(clamped * 255.0 / 100.0));
}

int AlphaToPercent(BYTE alpha) {
    const int pct = static_cast<int>(std::lround(alpha * 100.0 / 255.0));
    return std::clamp(pct, WinController::kMinOpacityPercent, WinController::kMaxOpacityPercent);
}

bool IsErrorAccessDenied(DWORD err) {
    return err == ERROR_ACCESS_DENIED;
}

}  // namespace

WindowState WinController::GetWindowState(HWND hwnd) const {
    auto it = states_.find(hwnd);
    if (it != states_.end()) {
        return it->second;
    }

    WindowState live;
    if (!IsWindow(hwnd)) {
        return live;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    live.initialLayered = (exStyle & WS_EX_LAYERED) != 0;
    live.initialTopmost = (exStyle & WS_EX_TOPMOST) != 0;
    live.initialTransparent = (exStyle & WS_EX_TRANSPARENT) != 0;
    live.topmost = live.initialTopmost;
    live.clickThrough = live.initialTransparent;

    if (live.initialLayered) {
        BYTE alpha = 255;
        DWORD flags = 0;
        if (GetLayeredWindowAttributes(hwnd, nullptr, &alpha, &flags) && (flags & LWA_ALPHA) != 0) {
            live.opacityPercent = AlphaToPercent(alpha);
        }
    }
    return live;
}

WindowState& WinController::EnsureTracked(HWND hwnd) {
    auto it = states_.find(hwnd);
    if (it != states_.end()) {
        return it->second;
    }

    WindowState initial = GetWindowState(hwnd);
    auto [insertedIt, _] = states_.emplace(hwnd, initial);
    return insertedIt->second;
}

bool WinController::ApplyExtendedStyle(HWND hwnd, LONG_PTR newExStyle, WindowState& state) {
    SetLastError(0);
    const LONG_PTR prev = SetWindowLongPtrW(hwnd, GWL_EXSTYLE, newExStyle);
    const DWORD err = GetLastError();
    if (prev == 0 && err != 0) {
        if (IsErrorAccessDenied(err)) {
            state.accessDenied = true;
        }
        return false;
    }
    state.accessDenied = false;
    return true;
}

void WinController::MaybeUntrackIfClean(HWND hwnd) {
    auto it = states_.find(hwnd);
    if (it == states_.end()) {
        return;
    }
    const WindowState& s = it->second;
    if (s.opacityPercent == kMaxOpacityPercent &&
        s.topmost == s.initialTopmost &&
        s.clickThrough == s.initialTransparent &&
        !s.accessDenied) {
        states_.erase(it);
    }
}

bool WinController::SetOpacity(HWND hwnd, int percent) {
    if (!IsWindow(hwnd)) {
        states_.erase(hwnd);
        return false;
    }

    const int targetPercent = std::clamp(percent, kMinOpacityPercent, kMaxOpacityPercent);
    WindowState& state = EnsureTracked(hwnd);

    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const bool needLayered = (targetPercent < kMaxOpacityPercent) || state.clickThrough || state.initialLayered;

    if (needLayered) {
        if ((exStyle & WS_EX_LAYERED) == 0) {
            if (!ApplyExtendedStyle(hwnd, exStyle | WS_EX_LAYERED, state)) {
                return false;
            }
        }
        const BYTE alpha = PercentToAlpha(targetPercent);
        if (!SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA)) {
            if (IsErrorAccessDenied(GetLastError())) {
                state.accessDenied = true;
            }
            return false;
        }
    } else {
        if ((exStyle & WS_EX_LAYERED) != 0) {
            if (!ApplyExtendedStyle(hwnd, exStyle & ~WS_EX_LAYERED, state)) {
                return false;
            }
        }
    }

    state.opacityPercent = targetPercent;
    state.accessDenied = false;
    MaybeUntrackIfClean(hwnd);
    return true;
}

bool WinController::AdjustOpacity(HWND hwnd, int deltaPercent) {
    const WindowState current = GetWindowState(hwnd);
    return SetOpacity(hwnd, current.opacityPercent + deltaPercent);
}

bool WinController::SetTopmost(HWND hwnd, bool topmost) {
    if (!IsWindow(hwnd)) {
        states_.erase(hwnd);
        return false;
    }

    WindowState& state = EnsureTracked(hwnd);
    const HWND insertAfter = topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
    if (!SetWindowPos(hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)) {
        if (IsErrorAccessDenied(GetLastError())) {
            state.accessDenied = true;
        }
        return false;
    }

    state.topmost = topmost;
    state.accessDenied = false;
    MaybeUntrackIfClean(hwnd);
    return true;
}

bool WinController::SetClickThrough(HWND hwnd, bool enabled) {
    if (!IsWindow(hwnd)) {
        states_.erase(hwnd);
        return false;
    }

    WindowState& state = EnsureTracked(hwnd);
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);

    if (enabled) {
        LONG_PTR targetStyle = exStyle | WS_EX_TRANSPARENT;
        const bool addedLayered = (exStyle & WS_EX_LAYERED) == 0;
        if (addedLayered) {
            targetStyle |= WS_EX_LAYERED;
        }
        if (!ApplyExtendedStyle(hwnd, targetStyle, state)) {
            return false;
        }
        if (addedLayered) {
            SetLayeredWindowAttributes(hwnd, 0, PercentToAlpha(state.opacityPercent), LWA_ALPHA);
        }
    } else {
        LONG_PTR targetStyle = exStyle & ~WS_EX_TRANSPARENT;
        if (state.opacityPercent == kMaxOpacityPercent && !state.initialLayered) {
            targetStyle &= ~WS_EX_LAYERED;
        }
        if (!ApplyExtendedStyle(hwnd, targetStyle, state)) {
            return false;
        }
    }

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

    state.clickThrough = enabled;
    state.accessDenied = false;
    MaybeUntrackIfClean(hwnd);
    return true;
}

bool WinController::ResetWindow(HWND hwnd) {
    auto it = states_.find(hwnd);
    if (it == states_.end()) {
        return true;
    }

    if (!IsWindow(hwnd)) {
        states_.erase(it);
        return true;
    }

    WindowState saved = it->second;
    bool ok = true;

    if (saved.clickThrough != saved.initialTransparent) {
        ok = SetClickThrough(hwnd, saved.initialTransparent) && ok;
    }
    if (saved.topmost != saved.initialTopmost) {
        ok = SetTopmost(hwnd, saved.initialTopmost) && ok;
    }
    if (saved.opacityPercent != kMaxOpacityPercent) {
        ok = SetOpacity(hwnd, kMaxOpacityPercent) && ok;
    }

    if (ok) {
        states_.erase(hwnd);
    }
    return ok;
}

void WinController::RestoreAll() {
    std::vector<HWND> handles;
    handles.reserve(states_.size());
    for (const auto& [hwnd, _] : states_) {
        handles.push_back(hwnd);
    }
    for (HWND hwnd : handles) {
        ResetWindow(hwnd);
    }
}

void WinController::PruneInvalidWindows() {
    for (auto it = states_.begin(); it != states_.end();) {
        if (!IsWindow(it->first)) {
            it = states_.erase(it);
        } else {
            ++it;
        }
    }
}

bool WinController::HasModifiedWindows() const {
    return !states_.empty();
}

bool WinController::IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

bool WinController::RestartAsAdmin() {
    std::array<wchar_t, 1024> exePath{};
    const DWORD len = GetModuleFileNameW(nullptr, exePath.data(), static_cast<DWORD>(exePath.size()));
    if (len == 0 || len >= exePath.size()) {
        return false;
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"runas";
    sei.lpFile = exePath.data();
    sei.nShow = SW_SHOWNORMAL;

    return ShellExecuteExW(&sei) == TRUE;
}
