#include "win_enumerator.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <array>
#include <cwchar>

namespace {

bool IsShellWindowClass(const wchar_t* className) {
    return wcscmp(className, L"Progman") == 0 ||
           wcscmp(className, L"WorkerW") == 0 ||
           wcscmp(className, L"Shell_TrayWnd") == 0;
}

std::wstring GetWindowClassNameW(HWND hwnd) {
    std::array<wchar_t, 256> buf{};
    const int len = GetClassNameW(hwnd, buf.data(), static_cast<int>(buf.size()));
    if (len <= 0) {
        return {};
    }
    return std::wstring(buf.data(), static_cast<size_t>(len));
}

std::wstring GetWindowTitleW(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) {
        return {};
    }
    std::wstring title(static_cast<size_t>(len) + 1, L'\0');
    const int copied = GetWindowTextW(hwnd, title.data(), len + 1);
    if (copied <= 0) {
        return {};
    }
    title.resize(static_cast<size_t>(copied));
    return title;
}

std::wstring GetProcessExePathW(DWORD pid) {
    if (pid == 0) {
        return {};
    }
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hProcess) {
        return {};
    }

    std::wstring path(1024, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(hProcess, 0, path.data(), &size)) {
        CloseHandle(hProcess);
        return {};
    }
    CloseHandle(hProcess);
    path.resize(static_cast<size_t>(size));
    return path;
}

std::wstring ExtractFileName(const std::wstring& fullPath) {
    if (fullPath.empty()) {
        return {};
    }
    const size_t pos = fullPath.find_last_of(L"\\/");
    if (pos == std::wstring::npos) {
        return fullPath;
    }
    return fullPath.substr(pos + 1);
}

struct EnumContext {
    HWND selfHwnd = nullptr;
    std::vector<WindowInfo>* results = nullptr;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<EnumContext*>(lParam);
    if (!ctx || !ctx->results) {
        return FALSE;
    }

    if (hwnd == ctx->selfHwnd) {
        return TRUE;
    }

    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }

    if (GetWindowTextLengthW(hwnd) <= 0) {
        return TRUE;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0 && (exStyle & WS_EX_APPWINDOW) == 0) {
        return TRUE;
    }

    DWORD cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
        return TRUE;
    }

    const std::wstring className = GetWindowClassNameW(hwnd);
    if (className.empty() || IsShellWindowClass(className.c_str())) {
        return TRUE;
    }

    const std::wstring title = GetWindowTitleW(hwnd);
    if (title.empty()) {
        return TRUE;
    }

    WindowInfo info;
    info.hwnd = hwnd;
    GetWindowThreadProcessId(hwnd, &info.pid);
    info.exePath = GetProcessExePathW(info.pid);
    const std::wstring procName = ExtractFileName(info.exePath);
    info.processNameUtf8 = procName.empty() ? "Unknown" : WideToUtf8(procName);
    info.titleUtf8 = WideToUtf8(title);
    info.classNameUtf8 = WideToUtf8(className);

    ctx->results->push_back(std::move(info));
    return TRUE;
}

}  // namespace

std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8(static_cast<size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), utf8.data(), required, nullptr, nullptr);
    return utf8;
}

std::vector<WindowInfo> EnumerateAppWindows(HWND selfHwnd) {
    std::vector<WindowInfo> results;
    EnumContext ctx{selfHwnd, &results};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return results;
}

HICON ExtractWindowIcon(HWND hwnd, const std::wstring& exePath, bool& outOwnsIcon) {
    outOwnsIcon = false;

    auto queryMsgIcon = [hwnd](WPARAM iconType) -> HICON {
        DWORD_PTR result = 0;
        if (SendMessageTimeoutW(hwnd, WM_GETICON, iconType, 0,
                                SMTO_ABORTIFHUNG, 60, &result) != 0) {
            return reinterpret_cast<HICON>(result);
        }
        return nullptr;
    };

    if (HICON icon = queryMsgIcon(ICON_BIG)) {
        return icon;
    }
    if (HICON icon = queryMsgIcon(ICON_SMALL2)) {
        return icon;
    }
    if (HICON icon = queryMsgIcon(ICON_SMALL)) {
        return icon;
    }

    if (HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON))) {
        return icon;
    }
    if (HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM))) {
        return icon;
    }

    if (!exePath.empty()) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(exePath.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_LARGEICON) != 0 &&
            sfi.hIcon != nullptr) {
            outOwnsIcon = true;
            return sfi.hIcon;
        }
    }

    return nullptr;
}
