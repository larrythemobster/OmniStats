#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>
#include <d3d11.h>
#include <wrl/client.h>

class RmlRenderInterfaceD3D11 final : public Rml::RenderInterface {
  public:
    RmlRenderInterfaceD3D11() = default;
    ~RmlRenderInterfaceD3D11() override;

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height);
    void Shutdown();
    void SetViewport(int width, int height);

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void SetTransform(const Rml::Matrix4f* transform) override;

  private:
    struct GpuGeometry {
        Microsoft::WRL::ComPtr<ID3D11Buffer> vertexBuffer;
        Microsoft::WRL::ComPtr<ID3D11Buffer> indexBuffer;
        int indexCount = 0;
    };

    struct Constants {
        float viewport[2]{};
        float translation[2]{};
        float transform[16]{};
        int hasTransform = 0;
        int hasTexture = 0;
        float padding[2]{};
    };

    bool CreatePipeline();
    bool CreateTextureFromRgba(const unsigned char* rgba, int width, int height, Rml::TextureHandle& handle);
    bool LoadEmbeddedPng(const std::string& source, Rml::TextureHandle& handle, Rml::Vector2i& dimensions);
    bool LoadDiskPng(const std::string& source, Rml::TextureHandle& handle, Rml::Vector2i& dimensions);
    // Color-picker gradients. RmlUi paints CSS gradients through render-interface
    // shaders, which this interface does not implement, so the picker asks for
    // `gen://` textures instead.
    bool GenerateGradient(const std::string& source, Rml::TextureHandle& handle, Rml::Vector2i& dimensions);
    void DrawGeometry(ID3D11Buffer* vertexBuffer, ID3D11Buffer* indexBuffer, int indexCount,
                      Rml::TextureHandle texture, const Rml::Vector2f& translation);
    void UpdateConstants(Rml::TextureHandle texture, const Rml::Vector2f& translation);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    int m_width = 1;
    int m_height = 1;
    bool m_scissorEnabled = false;
    D3D11_RECT m_scissor{0, 0, 1, 1};
    Rml::Matrix4f m_transform = Rml::Matrix4f::Identity();
    bool m_hasTransform = false;
    ULONG_PTR m_gdiplusToken = 0;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_pixelShader;
    Microsoft::WRL::ComPtr<ID3D11InputLayout> m_inputLayout;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_constantBuffer;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerScissor;
    Microsoft::WRL::ComPtr<ID3D11RasterizerState> m_rasterizerNoScissor;
    Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
};
