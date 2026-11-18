#pragma once

#include <d3d11.h>
#include <windows.h>

#include <string>
#include <unordered_map>

class IconCache {
public:
    explicit IconCache(ID3D11Device* device = nullptr);
    ~IconCache();

    IconCache(const IconCache&) = delete;
    IconCache& operator=(const IconCache&) = delete;

    void SetDevice(ID3D11Device* device);

    // 获取窗口对应的 DX11 纹理视图（内部自动缓存）
    ID3D11ShaderResourceView* GetOrCreateIcon(HWND hwnd, const std::wstring& exePath);

    // 释放所有缓存的 DX11 纹理
    void Clear();

private:
    ID3D11ShaderResourceView* CreateTextureFromHIcon(HICON hIcon);

    ID3D11Device* device_ = nullptr;
    std::unordered_map<std::wstring, ID3D11ShaderResourceView*> cache_;
};
