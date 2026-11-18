#include "icon_cache.h"
#include "tray_hotkey.h"
#include "win_controller.h"
#include "win_enumerator.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

struct D3DContext {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* deviceContext = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* mainRenderTargetView = nullptr;
    bool swapChainOccluded = false;
    UINT resizeWidth = 0;
    UINT resizeHeight = 0;
};

constexpr wchar_t kSingleInstanceMutexName[] = L"Local\\GhostWin_SingleInstance_Mutex";
constexpr UINT kMsgShowExistingInstance = WM_APP + 2;

struct AppState {
    D3DContext d3d;
    WinController controller;
    IconCache iconCache;
    TrayHotkeyManager trayHotkey;

    HANDLE singleInstanceMutex = nullptr;
    HWND mainHwnd = nullptr;
    std::vector<WindowInfo> windows;
    std::array<char, 256> searchFilter{};
    bool autoRefresh = true;
    bool restoreOnExit = true;
    bool isElevated = false;
    bool requestExit = false;
    bool needRefresh = true;
    std::chrono::steady_clock::time_point lastRefreshTime{};
};

AppState g_app;

void CreateRenderTarget(D3DContext& d3d) {
    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(d3d.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) && backBuffer) {
        d3d.device->CreateRenderTargetView(backBuffer, nullptr, &d3d.mainRenderTargetView);
        backBuffer->Release();
    }
}

void CleanupRenderTarget(D3DContext& d3d) {
    if (d3d.mainRenderTargetView) {
        d3d.mainRenderTargetView->Release();
        d3d.mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd, D3DContext& d3d) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel{};
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
        &d3d.swapChain, &d3d.device, &featureLevel, &d3d.deviceContext);

    if (res == DXGI_ERROR_UNSUPPORTED) {
        res = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
            &d3d.swapChain, &d3d.device, &featureLevel, &d3d.deviceContext);
    }

    if (res != S_OK) {
        return false;
    }

    CreateRenderTarget(d3d);
    return true;
}

void CleanupDeviceD3D(D3DContext& d3d) {
    CleanupRenderTarget(d3d);
    if (d3d.swapChain) {
        d3d.swapChain->Release();
        d3d.swapChain = nullptr;
    }
    if (d3d.deviceContext) {
        d3d.deviceContext->Release();
        d3d.deviceContext = nullptr;
    }
    if (d3d.device) {
        d3d.device->Release();
        d3d.device = nullptr;
    }
}

void ShowMainWindow() {
    if (!g_app.mainHwnd) {
        return;
    }
    if (IsIconic(g_app.mainHwnd)) {
        ShowWindow(g_app.mainHwnd, SW_RESTORE);
    } else {
        ShowWindow(g_app.mainHwnd, SW_SHOW);
    }
    // 先临时置顶再取消置顶，确保从托盘或后台窗口下强制弹出到最前
    SetWindowPos(g_app.mainHwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetWindowPos(g_app.mainHwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_app.mainHwnd);
    SetActiveWindow(g_app.mainHwnd);
    g_app.needRefresh = true;
}

bool IsTargetForegroundSafe(HWND fgHwnd) {
    if (!fgHwnd || fgHwnd == g_app.mainHwnd || !IsWindow(fgHwnd)) {
        return false;
    }
    std::array<wchar_t, 64> className{};
    if (GetClassNameW(fgHwnd, className.data(), static_cast<int>(className.size())) > 0) {
        if (wcscmp(className.data(), L"Progman") == 0 ||
            wcscmp(className.data(), L"WorkerW") == 0 ||
            wcscmp(className.data(), L"Shell_TrayWnd") == 0) {
            return false;
        }
    }
    return true;
}

bool ContainsCaseInsensitive(const std::string& haystack, const char* needle) {
    if (!needle || needle[0] == '\0') {
        return true;
    }
    auto toLowerAscii = [](unsigned char c) -> unsigned char {
        return static_cast<unsigned char>(std::tolower(c));
    };
    std::string needleStr(needle);
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needleStr.begin(), needleStr.end(),
        [&](char a, char b) {
            return toLowerAscii(static_cast<unsigned char>(a)) ==
                   toLowerAscii(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

void RefreshWindowList() {
    g_app.controller.PruneInvalidWindows();
    g_app.windows = EnumerateAppWindows(g_app.mainHwnd);
    g_app.lastRefreshTime = std::chrono::steady_clock::now();
    g_app.needRefresh = false;
}

void SetupFontsAndStyle(float dpiScale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    style.WindowRounding = 0.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.FramePadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ScaleAllSizes(dpiScale);

    ImGuiIO& io = ImGui::GetIO();
    const float fontSize = 17.0f * dpiScale;
    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
    };

    ImFont* loadedFont = nullptr;
    for (const char* path : fontPaths) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            loadedFont = io.Fonts->AddFontFromFileTTF(
                path, fontSize, nullptr, io.Fonts->GetGlyphRangesChineseFull());
            if (loadedFont) {
                break;
            }
        }
    }
    if (!loadedFont) {
        io.Fonts->AddFontDefault();
    }
}

void RenderMainUI(float dpiScale) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);

    constexpr ImGuiWindowFlags kRootFlags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings;

    ImGui::Begin("GhostWinMain", nullptr, kRootFlags);

    // 顶部工具栏
    ImGui::SetNextItemWidth(220.0f * dpiScale);
    ImGui::InputTextWithHint("##Search", "搜索进程名或窗口标题...",
                             g_app.searchFilter.data(), g_app.searchFilter.size());

    ImGui::SameLine();
    if (ImGui::Button("刷新")) {
        g_app.needRefresh = true;
    }

    ImGui::SameLine();
    ImGui::Checkbox("自动刷新", &g_app.autoRefresh);

    ImGui::SameLine();
    const bool hasModified = g_app.controller.HasModifiedWindows();
    if (!hasModified) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("全部恢复")) {
        g_app.controller.RestoreAll();
    }
    if (!hasModified) {
        ImGui::EndDisabled();
    }

    ImGui::SameLine();
    ImGui::Checkbox("退出时恢复", &g_app.restoreOnExit);

    ImGui::SameLine();
    if (g_app.isElevated) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "[管理员]");
    } else {
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "[标准权限]");
        ImGui::SameLine();
        if (ImGui::SmallButton("以管理员重启")) {
            if (g_app.singleInstanceMutex) {
                CloseHandle(g_app.singleInstanceMutex);
                g_app.singleInstanceMutex = nullptr;
            }
            if (WinController::RestartAsAdmin()) {
                g_app.requestExit = true;
            } else {
                g_app.singleInstanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
            }
        }
    }

    // 快捷键提示栏
    if (g_app.trayHotkey.AreHotkeysRegistered()) {
        ImGui::TextDisabled("全局快捷键: Ctrl+Alt+Up (+10%%) | Ctrl+Alt+Down (-10%%) | Ctrl+Alt+R (恢复100%%)   关闭窗口将最小化到托盘");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f),
                           "部分全局快捷键注册失败（可能被其他程序占用）   关闭窗口将最小化到托盘");
    }

    ImGui::Separator();

    // 窗口列表表格
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_BordersInnerH |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingStretchProp;

    if (ImGui::BeginTable("WindowListTable", 6, kTableFlags)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("应用", ImGuiTableColumnFlags_WidthFixed, 170.0f * dpiScale);
        ImGui::TableSetupColumn("窗口标题", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("透明度", ImGuiTableColumnFlags_WidthFixed, 190.0f * dpiScale);
        ImGui::TableSetupColumn("置顶", ImGuiTableColumnFlags_WidthFixed, 48.0f * dpiScale);
        ImGui::TableSetupColumn("穿透", ImGuiTableColumnFlags_WidthFixed, 48.0f * dpiScale);
        ImGui::TableSetupColumn("操作", ImGuiTableColumnFlags_WidthFixed, 64.0f * dpiScale);
        ImGui::TableHeadersRow();

        const float iconSize = 18.0f * dpiScale;

        for (const auto& win : g_app.windows) {
            const bool matchProc = ContainsCaseInsensitive(win.processNameUtf8, g_app.searchFilter.data());
            const bool matchTitle = ContainsCaseInsensitive(win.titleUtf8, g_app.searchFilter.data());
            if (!matchProc && !matchTitle) {
                continue;
            }

            ImGui::TableNextRow();
            ImGui::PushID(win.hwnd);

            WindowState state = g_app.controller.GetWindowState(win.hwnd);

            // 第1列：图标 + 进程名
            ImGui::TableSetColumnIndex(0);
            if (ID3D11ShaderResourceView* iconSrv = g_app.iconCache.GetOrCreateIcon(win.hwnd, win.exePath)) {
                ImGui::Image((ImTextureID)(intptr_t)iconSrv, ImVec2(iconSize, iconSize));
                ImGui::SameLine();
            } else {
                ImGui::Dummy(ImVec2(iconSize, iconSize));
                ImGui::SameLine();
            }
            ImGui::TextUnformatted(win.processNameUtf8.c_str());

            // 第2列：窗口标题
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(win.titleUtf8.c_str());
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) {
                ImGui::BeginTooltip();
                ImGui::Text("标题: %s", win.titleUtf8.c_str());
                ImGui::Text("类名: %s", win.classNameUtf8.c_str());
                ImGui::Text("PID: %lu | HWND: 0x%p", win.pid, static_cast<void*>(win.hwnd));
                ImGui::EndTooltip();
            }

            // 第3列：透明度滑块 / 权限提示
            ImGui::TableSetColumnIndex(2);
            if (state.accessDenied) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "权限不足(需管理员)");
            } else {
                int opacity = state.opacityPercent;
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderInt("##Opacity", &opacity,
                                     WinController::kMinOpacityPercent,
                                     WinController::kMaxOpacityPercent, "%d%%")) {
                    g_app.controller.SetOpacity(win.hwnd, opacity);
                }
            }

            // 第4列：置顶
            ImGui::TableSetColumnIndex(3);
            bool topmost = state.topmost;
            if (ImGui::Checkbox("##Topmost", &topmost)) {
                g_app.controller.SetTopmost(win.hwnd, topmost);
            }

            // 第5列：鼠标穿透
            ImGui::TableSetColumnIndex(4);
            bool clickThrough = state.clickThrough;
            if (ImGui::Checkbox("##ClickThrough", &clickThrough)) {
                g_app.controller.SetClickThrough(win.hwnd, clickThrough);
            }

            // 第6列：重置按钮
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("重置")) {
                g_app.controller.ResetWindow(win.hwnd);
            }

            ImGui::PopID();
        }

        ImGui::EndTable();
    }

    ImGui::End();
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }

    TrayHotkeyManager::Callbacks callbacks;
    callbacks.onShowWindow = []() { ShowMainWindow(); };
    callbacks.onRestoreAll = []() { g_app.controller.RestoreAll(); };
    callbacks.onExit = []() { g_app.requestExit = true; };
    callbacks.onForegroundOpacityDelta = [](int delta) {
        HWND fg = GetForegroundWindow();
        if (IsTargetForegroundSafe(fg)) {
            g_app.controller.AdjustOpacity(fg, delta);
        }
    };
    callbacks.onForegroundOpacityReset = []() {
        HWND fg = GetForegroundWindow();
        if (IsTargetForegroundSafe(fg)) {
            g_app.controller.ResetWindow(fg);
        }
    };

    if (g_app.trayHotkey.HandleMessage(msg, wParam, lParam, callbacks)) {
        return 0;
    }

    if (msg == kMsgShowExistingInstance) {
        ShowMainWindow();
        return 0;
    }

    switch (msg) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                g_app.d3d.resizeWidth = static_cast<UINT>(LOWORD(lParam));
                g_app.d3d.resizeHeight = static_cast<UINT>(HIWORD(lParam));
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) {
                return 0;
            }
            break;
        case WM_CLOSE:
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    g_app.singleInstanceMutex = CreateMutexW(nullptr, FALSE, kSingleInstanceMutexName);
    if (g_app.singleInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND existingHwnd = FindWindowW(L"GhostWinWindowClass", nullptr)) {
            DWORD existingPid = 0;
            GetWindowThreadProcessId(existingHwnd, &existingPid);
            if (existingPid != 0) {
                AllowSetForegroundWindow(existingPid);
            }
            PostMessageW(existingHwnd, kMsgShowExistingInstance, 0, 0);
        }
        CloseHandle(g_app.singleInstanceMutex);
        g_app.singleInstanceMutex = nullptr;
        return 0;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GhostWinWindowClass";
    RegisterClassExW(&wc);

    const UINT initDpi = GetDpiForSystem();
    const float initScale = static_cast<float>(initDpi) / 96.0f;

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"GhostWin 窗口透明度管理工具",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(920 * initScale),
        static_cast<int>(600 * initScale),
        nullptr, nullptr, hInstance, nullptr);

    if (!hwnd) {
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    g_app.mainHwnd = hwnd;
    g_app.isElevated = WinController::IsProcessElevated();
    ChangeWindowMessageFilterEx(hwnd, kMsgShowExistingInstance, MSGFLT_ALLOW, nullptr);

    if (!CreateDeviceD3D(hwnd, g_app.d3d)) {
        CleanupDeviceD3D(g_app.d3d);
        DestroyWindow(hwnd);
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    g_app.iconCache.SetDevice(g_app.d3d.device);
    g_app.trayHotkey.Initialize(hwnd);

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // 不生成 imgui.ini 保持单文件整洁

    const float dpiScale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    SetupFontsAndStyle(dpiScale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_app.d3d.device, g_app.d3d.deviceContext);

    RefreshWindowList();

    bool done = false;
    while (!done) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                done = true;
            }
        }
        if (done || g_app.requestExit) {
            break;
        }

        // 隐藏到托盘或最小化时挂起等待消息，实现极低功耗
        if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
            WaitMessage();
            continue;
        }

        if (g_app.d3d.swapChainOccluded &&
            g_app.d3d.swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(100);
            continue;
        }
        g_app.d3d.swapChainOccluded = false;

        if (g_app.d3d.resizeWidth != 0 && g_app.d3d.resizeHeight != 0) {
            CleanupRenderTarget(g_app.d3d);
            g_app.d3d.swapChain->ResizeBuffers(0, g_app.d3d.resizeWidth, g_app.d3d.resizeHeight,
                                               DXGI_FORMAT_UNKNOWN, 0);
            g_app.d3d.resizeWidth = 0;
            g_app.d3d.resizeHeight = 0;
            CreateRenderTarget(g_app.d3d);
        }

        const auto now = std::chrono::steady_clock::now();
        if (g_app.needRefresh ||
            (g_app.autoRefresh && (now - g_app.lastRefreshTime) >= std::chrono::seconds(2))) {
            RefreshWindowList();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RenderMainUI(dpiScale);

        ImGui::Render();
        const float clearColor[4] = {0.10f, 0.10f, 0.12f, 1.00f};
        g_app.d3d.deviceContext->OMSetRenderTargets(1, &g_app.d3d.mainRenderTargetView, nullptr);
        g_app.d3d.deviceContext->ClearRenderTargetView(g_app.d3d.mainRenderTargetView, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        const HRESULT hr = g_app.d3d.swapChain->Present(1, 0);
        g_app.d3d.swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    if (g_app.restoreOnExit) {
        g_app.controller.RestoreAll();
    }

    g_app.trayHotkey.Shutdown();
    g_app.iconCache.Clear();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D(g_app.d3d);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    if (g_app.singleInstanceMutex) {
        CloseHandle(g_app.singleInstanceMutex);
        g_app.singleInstanceMutex = nullptr;
    }

    return 0;
}
