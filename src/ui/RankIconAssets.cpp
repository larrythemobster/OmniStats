#include "RankIconAssets.hpp"
#include <windows.h>
#include <gdiplus.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace {

std::string ToLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool Contains(const std::string& value, const std::string& needle) {
    return value.find(needle) != std::string::npos;
}

std::string RankPart(const std::string& tier) {
    std::string value = ToLower(tier);
    size_t divPos = value.find(" div");
    if (divPos != std::string::npos) {
        value = value.substr(0, divPos);
    }
    while (!value.empty() && value.back() == ' ') {
        value.pop_back();
    }
    return value;
}

} // namespace

bool RankIconAssets::Load(ID3D11Device* device) {
    Shutdown();
    if (!device) {
        return false;
    }

    Gdiplus::GdiplusStartupInput startupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok) {
        std::cout << "[RankIcons] Failed to start GDI+ for rank icon loading.\n";
        return false;
    }

    bool loadedAny = false;
    for (int i = 0; i < static_cast<int>(m_playlists.size()); ++i) {
        std::string name = "RANK_PLAYLIST_" + std::to_string(i);
        loadedAny = LoadResourceTexture(device, name.c_str(), m_playlists[i]) || loadedAny;
    }
    for (int i = 0; i < static_cast<int>(m_tiers.size()); ++i) {
        std::string name = "RANK_TIER_" + std::to_string(i);
        loadedAny = LoadResourceTexture(device, name.c_str(), m_tiers[i]) || loadedAny;
    }
    for (int i = 0; i < static_cast<int>(m_divisions.size()); ++i) {
        std::string name = "RANK_DIVISION_" + std::to_string(i);
        loadedAny = LoadResourceTexture(device, name.c_str(), m_divisions[i]) || loadedAny;
    }
    LoadResourceTexture(device, "LOGO_PNG", m_logo);

    Gdiplus::GdiplusShutdown(gdiplusToken);
    m_loaded = loadedAny;
    return loadedAny;
}

void RankIconAssets::Shutdown() {
    for (auto& texture : m_playlists) {
        Release(texture);
    }
    for (auto& texture : m_tiers) {
        Release(texture);
    }
    for (auto& texture : m_divisions) {
        Release(texture);
    }
    Release(m_logo);
    m_loaded = false;
}

ID3D11ShaderResourceView* RankIconAssets::PlaylistTexture(const std::string& playlist) const {
    int index = PlaylistIndex(playlist);
    if (index < 0 || index >= static_cast<int>(m_playlists.size())) {
        return nullptr;
    }
    return m_playlists[index].view;
}

ID3D11ShaderResourceView* RankIconAssets::TierTexture(const std::string& tier) const {
    int index = TierIndex(tier);
    if (index < 0 || index >= static_cast<int>(m_tiers.size())) {
        return nullptr;
    }
    return m_tiers[index].view;
}

ID3D11ShaderResourceView* RankIconAssets::UnsyncedTierTexture() const {
    return m_tiers[23].view;
}

ID3D11ShaderResourceView* RankIconAssets::DivisionTexture(const std::string& tier, bool filled) const {
    int index = filled ? DivisionColorIndex(TierIndex(tier)) : 0;
    if (index < 0 || index >= static_cast<int>(m_divisions.size())) {
        return nullptr;
    }
    return m_divisions[index].view;
}

int RankIconAssets::DivisionLevel(const std::string& tier) {
    int tierIndex = TierIndex(tier);
    if (tierIndex <= 0 || tierIndex >= 22) {
        return 0;
    }

    std::string value = ToLower(tier);
    if (Contains(value, "div iv") || Contains(value, "division iv") || Contains(value, "div 4") || Contains(value, "division 4")) return 4;
    if (Contains(value, "div iii") || Contains(value, "division iii") || Contains(value, "div 3") || Contains(value, "division 3")) return 3;
    if (Contains(value, "div ii") || Contains(value, "division ii") || Contains(value, "div 2") || Contains(value, "division 2")) return 2;
    if (Contains(value, "div i") || Contains(value, "division i") || Contains(value, "div 1") || Contains(value, "division 1")) return 1;
    return 0;
}

bool RankIconAssets::LoadResourceTexture(ID3D11Device* device, const char* resourceName, Texture& texture) {
    HMODULE module = GetModuleHandle(nullptr);
    HRSRC resource = FindResourceA(module, resourceName, RT_RCDATA);
    if (!resource) {
        return false;
    }

    HGLOBAL resourceData = LoadResource(module, resource);
    DWORD resourceSize = SizeofResource(module, resource);
    void* resourceBytes = LockResource(resourceData);
    if (!resourceBytes || resourceSize == 0) {
        return false;
    }

    HGLOBAL global = GlobalAlloc(GMEM_MOVEABLE, resourceSize);
    if (!global) {
        return false;
    }

    void* globalBytes = GlobalLock(global);
    if (!globalBytes) {
        GlobalFree(global);
        return false;
    }
    memcpy(globalBytes, resourceBytes, resourceSize);
    GlobalUnlock(global);

    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(global, TRUE, &stream) != S_OK || !stream) {
        GlobalFree(global);
        return false;
    }

    std::unique_ptr<Gdiplus::Bitmap> bitmap(Gdiplus::Bitmap::FromStream(stream));
    stream->Release();
    if (!bitmap || bitmap->GetLastStatus() != Gdiplus::Ok) {
        return false;
    }

    int width = static_cast<int>(bitmap->GetWidth());
    int height = static_cast<int>(bitmap->GetHeight());
    if (width <= 0 || height <= 0) {
        return false;
    }

    Gdiplus::Bitmap converted(width, height, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics graphics(&converted);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.DrawImage(bitmap.get(), 0, 0, width, height);
    }

    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData bits{};
    if (converted.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bits) != Gdiplus::Ok) {
        return false;
    }

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    for (int y = 0; y < height; ++y) {
        const unsigned char* src = static_cast<const unsigned char*>(bits.Scan0) + y * bits.Stride;
        unsigned char* dst = pixels.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
        memcpy(dst, src, static_cast<size_t>(width) * 4);
    }
    converted.UnlockBits(&bits);

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA subResource{};
    subResource.pSysMem = pixels.data();
    subResource.SysMemPitch = static_cast<UINT>(width * 4);

    ID3D11Texture2D* d3dTexture = nullptr;
    HRESULT hr = device->CreateTexture2D(&desc, &subResource, &d3dTexture);
    if (FAILED(hr) || !d3dTexture) {
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = device->CreateShaderResourceView(d3dTexture, &srvDesc, &texture.view);
    d3dTexture->Release();
    if (FAILED(hr)) {
        texture.view = nullptr;
        return false;
    }

    texture.width = width;
    texture.height = height;
    return true;
}

int RankIconAssets::PlaylistIndex(const std::string& playlist) {
    std::string value = ToLower(playlist);
    if (value == "1v1" || value == "duel") return 0;
    if (value == "2v2" || value == "doubles") return 1;
    if (value == "3v3" || value == "standard") return 2;
    if (value == "hoops") return 3;
    if (value == "rumble") return 4;
    if (value == "dropshot") return 5;
    if (value == "snowday" || value == "snow day") return 6;
    if (value == "t" || value == "tournament" || value == "tournaments") return 7;
    if (value == "4v4") return 8;
    if (value == "heatseeker") return 9;
    return -1;
}

int RankIconAssets::TierIndex(const std::string& tier) {
    std::string value = RankPart(tier);
    if (value.empty() || value == "-" || Contains(value, "unranked")) {
        return 0;
    }
    if (Contains(value, "supersonic")) {
        return 22;
    }

    int base = -1;
    if (Contains(value, "bronze")) base = 1;
    else if (Contains(value, "silver")) base = 4;
    else if (Contains(value, "gold")) base = 7;
    else if (Contains(value, "platinum")) base = 10;
    else if (Contains(value, "diamond")) base = 13;
    else if (Contains(value, "grand champion")) base = 19;
    else if (Contains(value, "champion")) base = 16;

    if (base < 0) {
        return 0;
    }

    if (Contains(value, " iii") || Contains(value, " 3")) return base + 2;
    if (Contains(value, " ii") || Contains(value, " 2")) return base + 1;
    return base;
}

int RankIconAssets::DivisionColorIndex(int tierIndex) {
    if (tierIndex >= 1 && tierIndex <= 3) return 1;
    if (tierIndex >= 4 && tierIndex <= 6) return 2;
    if (tierIndex >= 7 && tierIndex <= 9) return 3;
    if (tierIndex >= 10 && tierIndex <= 12) return 4;
    if (tierIndex >= 13 && tierIndex <= 15) return 5;
    if (tierIndex >= 16 && tierIndex <= 18) return 6;
    if (tierIndex >= 19 && tierIndex <= 21) return 7;
    return 7;
}

void RankIconAssets::Release(Texture& texture) {
    if (texture.view) {
        texture.view->Release();
        texture.view = nullptr;
    }
    texture.width = 0;
    texture.height = 0;
}
