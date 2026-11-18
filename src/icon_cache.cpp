#include "icon_cache.h"
#include "win_enumerator.h"

#include <algorithm>
#include <cstdint>
#include <vector>

IconCache::IconCache(ID3D11Device* device) : device_(device) {}

IconCache::~IconCache() {
    Clear();
}

void IconCache::SetDevice(ID3D11Device* device) {
    if (device_ != device) {
        Clear();
        device_ = device;
    }
}

void IconCache::Clear() {
    for (auto& [_, srv] : cache_) {
        if (srv) {
            srv->Release();
        }
    }
    cache_.clear();
}

ID3D11ShaderResourceView* IconCache::GetOrCreateIcon(HWND hwnd, const std::wstring& exePath) {
    if (!device_) {
        return nullptr;
    }

    const std::wstring key = exePath.empty()
        ? (L"hwnd:" + std::to_wstring(reinterpret_cast<uintptr_t>(hwnd)))
        : exePath;

    auto it = cache_.find(key);
    if (it != cache_.end()) {
        return it->second;
    }

    bool ownsIcon = false;
    HICON hIcon = ExtractWindowIcon(hwnd, exePath, ownsIcon);
    ID3D11ShaderResourceView* srv = nullptr;
    if (hIcon) {
        srv = CreateTextureFromHIcon(hIcon);
        if (ownsIcon) {
            DestroyIcon(hIcon);
        }
    }

    cache_.emplace(key, srv);
    return srv;
}

ID3D11ShaderResourceView* IconCache::CreateTextureFromHIcon(HICON hIcon) {
    ICONINFO iconInfo{};
    if (!GetIconInfo(hIcon, &iconInfo)) {
        return nullptr;
    }

    auto cleanupBitmaps = [&iconInfo]() {
        if (iconInfo.hbmColor) {
            DeleteObject(iconInfo.hbmColor);
        }
        if (iconInfo.hbmMask) {
            DeleteObject(iconInfo.hbmMask);
        }
    };

    if (!iconInfo.hbmColor) {
        cleanupBitmaps();
        return nullptr;
    }

    BITMAP bmp{};
    if (GetObjectW(iconInfo.hbmColor, sizeof(BITMAP), &bmp) == 0 || bmp.bmWidth <= 0 || bmp.bmHeight <= 0) {
        cleanupBitmaps();
        return nullptr;
    }

    const int width = bmp.bmWidth;
    const int height = bmp.bmHeight;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;  // top-down DIB
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    std::vector<uint8_t> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0);
    HDC hdc = GetDC(nullptr);
    const int scanLines = GetDIBits(hdc, iconInfo.hbmColor, 0, static_cast<UINT>(height),
                                    pixels.data(), &bmi, DIB_RGB_COLORS);
    if (scanLines == 0) {
        ReleaseDC(nullptr, hdc);
        cleanupBitmaps();
        return nullptr;
    }

    bool hasAlpha = false;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        std::swap(pixels[i], pixels[i + 2]);  // BGRA -> RGBA
        if (pixels[i + 3] != 0) {
            hasAlpha = true;
        }
    }

    // 部分旧式图标无 Alpha 通道，使用 hbmMask 推导透明像素
    if (!hasAlpha && iconInfo.hbmMask) {
        std::vector<uint8_t> maskPixels(pixels.size(), 0);
        if (GetDIBits(hdc, iconInfo.hbmMask, 0, static_cast<UINT>(height),
                      maskPixels.data(), &bmi, DIB_RGB_COLORS) != 0) {
            for (size_t i = 0; i < pixels.size(); i += 4) {
                const bool isTransparent = (maskPixels[i] != 0);
                pixels[i + 3] = isTransparent ? 0 : 255;
            }
        } else {
            for (size_t i = 0; i < pixels.size(); i += 4) {
                pixels[i + 3] = 255;
            }
        }
    }

    ReleaseDC(nullptr, hdc);
    cleanupBitmaps();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource{};
    subResource.pSysMem = pixels.data();
    subResource.SysMemPitch = static_cast<UINT>(width * 4);

    ID3D11Texture2D* texture = nullptr;
    if (FAILED(device_->CreateTexture2D(&desc, &subResource, &texture)) || !texture) {
        return nullptr;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    const HRESULT hr = device_->CreateShaderResourceView(texture, &srvDesc, &srv);
    texture->Release();

    if (FAILED(hr)) {
        return nullptr;
    }
    return srv;
}
