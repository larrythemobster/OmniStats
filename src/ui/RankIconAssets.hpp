#pragma once
#include <array>
#include <d3d11.h>
#include <string>

class RankIconAssets {
public:
    RankIconAssets() = default;
    ~RankIconAssets() { Shutdown(); }

    RankIconAssets(const RankIconAssets&) = delete;
    RankIconAssets& operator=(const RankIconAssets&) = delete;

    bool Load(ID3D11Device* device);
    void Shutdown();
    bool IsLoaded() const { return m_loaded; }

    ID3D11ShaderResourceView* PlaylistTexture(const std::string& playlist) const;
    ID3D11ShaderResourceView* TierTexture(const std::string& tier) const;
    ID3D11ShaderResourceView* UnsyncedTierTexture() const;
    ID3D11ShaderResourceView* DivisionTexture(const std::string& tier, bool filled) const;
    ID3D11ShaderResourceView* LogoTexture() const { return m_logo.view; }
    static int DivisionLevel(const std::string& tier);

private:
    struct Texture {
        ID3D11ShaderResourceView* view = nullptr;
        int width = 0;
        int height = 0;
    };

    bool LoadResourceTexture(ID3D11Device* device, const char* resourceName, Texture& texture);
    static int PlaylistIndex(const std::string& playlist);
    static int TierIndex(const std::string& tier);
    static int DivisionColorIndex(int tierIndex);
    static void Release(Texture& texture);

    std::array<Texture, 10> m_playlists{};
    std::array<Texture, 24> m_tiers{};
    std::array<Texture, 8> m_divisions{};
    Texture m_logo{};
    bool m_loaded = false;
};
