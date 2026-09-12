#include "ui/rml/RmlRenderInterfaceD3D11.hpp"

#include <RmlUi/Core/Vertex.h>
#include <d3dcompiler.h>
#include <gdiplus.h>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
    using Microsoft::WRL::ComPtr;

    std::wstring ToWide(const std::string& utf8) {
        if (utf8.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(count), L'\0');
        if (count > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), count);
        return result;
    }

    std::string ResourceNameFromSource(std::string source) {
        std::replace(source.begin(), source.end(), '\\', '/');
        constexpr const char* prefix = "res://images/";
        if (source.rfind(prefix, 0) != 0) return {};
        source.erase(0, std::strlen(prefix));

        auto mapIndexed = [&](const char* folder, const char* resourcePrefix) -> std::string {
            const std::string begin = std::string(folder) + "/";
            if (source.rfind(begin, 0) != 0 || source.size() <= begin.size() + 4 || source.substr(source.size() - 4) != ".png") return {};
            const std::string index = source.substr(begin.size(), source.size() - begin.size() - 4);
            if (index.empty() || !std::all_of(index.begin(), index.end(), ::isdigit)) return {};
            return std::string(resourcePrefix) + index;
        };

        if (auto value = mapIndexed("Tiers", "RANK_TIER_"); !value.empty()) return value;
        if (auto value = mapIndexed("Divisions", "RANK_DIVISION_"); !value.empty()) return value;
        if (auto value = mapIndexed("Playlists", "RANK_PLAYLIST_"); !value.empty()) return value;
        if (source == "Logo.png") return "LOGO_PNG";
        if (source == "chevron.png") return "UI_CHEVRON";
        return {};
    }

    bool DecodePng(Gdiplus::Bitmap& bitmap, std::vector<unsigned char>& rgba, int& width, int& height) {
        width = static_cast<int>(bitmap.GetWidth());
        height = static_cast<int>(bitmap.GetHeight());
        if (width <= 0 || height <= 0) return false;

        Gdiplus::Rect rect(0, 0, width, height);
        Gdiplus::BitmapData data{};
        if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &data) != Gdiplus::Ok) return false;

        rgba.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
        for (int y = 0; y < height; ++y) {
            const auto* src = static_cast<const unsigned char*>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride;
            auto* dst = rgba.data() + static_cast<size_t>(y) * static_cast<size_t>(width) * 4;
            for (int x = 0; x < width; ++x) {
                const unsigned char b = src[x * 4 + 0];
                const unsigned char g = src[x * 4 + 1];
                const unsigned char r = src[x * 4 + 2];
                const unsigned char a = src[x * 4 + 3];
                // RmlUi 6 renders with premultiplied alpha. Generated font atlases
                // already use that convention, so decoded PNG assets must match it.
                dst[x * 4 + 0] = static_cast<unsigned char>((static_cast<unsigned int>(r) * a + 127u) / 255u);
                dst[x * 4 + 1] = static_cast<unsigned char>((static_cast<unsigned int>(g) * a + 127u) / 255u);
                dst[x * 4 + 2] = static_cast<unsigned char>((static_cast<unsigned int>(b) * a + 127u) / 255u);
                dst[x * 4 + 3] = a;
            }
        }
        bitmap.UnlockBits(&data);
        return true;
    }

    class MemoryStream final : public IStream {
      public:
        MemoryStream(const void* data, size_t size) : m_ref(1) {
            HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
            if (!memory) return;
            void* target = GlobalLock(memory);
            if (!target) {
                GlobalFree(memory);
                return;
            }
            std::memcpy(target, data, size);
            GlobalUnlock(memory);
            if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &m_stream))) GlobalFree(memory);
        }
        ~MemoryStream() {
            if (m_stream) m_stream->Release();
        }
        bool Valid() const {
            return m_stream != nullptr;
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override {
            if (!ppvObject) return E_POINTER;
            *ppvObject = nullptr;
            if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
                *ppvObject = static_cast<IStream*>(this);
                AddRef();
                return S_OK;
            }
            return E_NOINTERFACE;
        }
        ULONG STDMETHODCALLTYPE AddRef() override {
            return ++m_ref;
        }
        ULONG STDMETHODCALLTYPE Release() override {
            const ULONG ref = --m_ref;
            if (!ref) delete this;
            return ref;
        }
        HRESULT STDMETHODCALLTYPE Read(void* pv, ULONG cb, ULONG* pcbRead) override {
            return m_stream->Read(pv, cb, pcbRead);
        }
        HRESULT STDMETHODCALLTYPE Write(const void* pv, ULONG cb, ULONG* pcbWritten) override {
            return m_stream->Write(pv, cb, pcbWritten);
        }
        HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override {
            return m_stream->Seek(dlibMove, dwOrigin, plibNewPosition);
        }
        HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER libNewSize) override {
            return m_stream->SetSize(libNewSize);
        }
        HRESULT STDMETHODCALLTYPE CopyTo(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead, ULARGE_INTEGER* pcbWritten) override {
            return m_stream->CopyTo(pstm, cb, pcbRead, pcbWritten);
        }
        HRESULT STDMETHODCALLTYPE Commit(DWORD grfCommitFlags) override {
            return m_stream->Commit(grfCommitFlags);
        }
        HRESULT STDMETHODCALLTYPE Revert() override {
            return m_stream->Revert();
        }
        HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override {
            return m_stream->LockRegion(libOffset, cb, dwLockType);
        }
        HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override {
            return m_stream->UnlockRegion(libOffset, cb, dwLockType);
        }
        HRESULT STDMETHODCALLTYPE Stat(STATSTG* pstatstg, DWORD grfStatFlag) override {
            return m_stream->Stat(pstatstg, grfStatFlag);
        }
        HRESULT STDMETHODCALLTYPE Clone(IStream** ppstm) override {
            return m_stream->Clone(ppstm);
        }

      private:
        ULONG m_ref;
        IStream* m_stream = nullptr;
    };
}

RmlRenderInterfaceD3D11::~RmlRenderInterfaceD3D11() {
    Shutdown();
}

bool RmlRenderInterfaceD3D11::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, int width, int height) {
    Shutdown();
    m_device = device;
    m_context = context;
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
    if (!m_device || !m_context) return false;
    Gdiplus::GdiplusStartupInput startupInput;
    if (Gdiplus::GdiplusStartup(&m_gdiplusToken, &startupInput, nullptr) != Gdiplus::Ok) {
        m_gdiplusToken = 0;
        return false;
    }
    if (!CreatePipeline()) {
        Shutdown();
        return false;
    }
    return true;
}

void RmlRenderInterfaceD3D11::Shutdown() {
    m_depthState.Reset();
    m_rasterizerNoScissor.Reset();
    m_rasterizerScissor.Reset();
    m_blendState.Reset();
    m_sampler.Reset();
    m_constantBuffer.Reset();
    m_inputLayout.Reset();
    m_pixelShader.Reset();
    m_vertexShader.Reset();
    m_device = nullptr;
    m_context = nullptr;
    m_scissorEnabled = false;
    m_scissor = {0, 0, 1, 1};
    m_hasTransform = false;
    m_transform = Rml::Matrix4f::Identity();
    if (m_gdiplusToken) {
        Gdiplus::GdiplusShutdown(m_gdiplusToken);
        m_gdiplusToken = 0;
    }
}

void RmlRenderInterfaceD3D11::SetViewport(int width, int height) {
    m_width = std::max(width, 1);
    m_height = std::max(height, 1);
}

bool RmlRenderInterfaceD3D11::CreatePipeline() {
    static constexpr char shaderSource[] = R"(
cbuffer UiConstants : register(b0) {
    float2 viewport;
    float2 translation;
    float4x4 transform;
    int hasTransform;
    int hasTexture;
    float2 padding;
};
struct VSInput { float2 pos : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
struct PSInput { float4 pos : SV_POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
PSInput VSMain(VSInput input) {
    float4 p = float4(input.pos + translation, 0.0, 1.0);
    if (hasTransform != 0) p = mul(transform, p);
    if (abs(p.w) > 0.00001) p.xyz /= p.w;
    PSInput output;
    output.pos = float4((p.x / viewport.x) * 2.0 - 1.0, 1.0 - (p.y / viewport.y) * 2.0, p.z, 1.0);
    output.color = input.color;
    output.uv = input.uv;
    return output;
}
Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);
float4 PSMain(PSInput input) : SV_TARGET {
    return input.color * (hasTexture != 0 ? tex0.Sample(samp0, input.uv) : float4(1,1,1,1));
}
)";

    ComPtr<ID3DBlob> vsBlob, psBlob, errors;
    HRESULT hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsBlob, &errors);
    if (FAILED(hr)) {
        if (errors) std::cerr << "[RmlUi] Vertex shader compile failed: " << static_cast<const char*>(errors->GetBufferPointer()) << "\n";
        return false;
    }
    errors.Reset();
    hr = D3DCompile(shaderSource, sizeof(shaderSource) - 1, nullptr, nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psBlob, &errors);
    if (FAILED(hr)) {
        if (errors) std::cerr << "[RmlUi] Pixel shader compile failed: " << static_cast<const char*>(errors->GetBufferPointer()) << "\n";
        return false;
    }

    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vertexShader))) return false;
    if (FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_pixelShader))) return false;

    const D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(Rml::Vertex, position)), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, static_cast<UINT>(offsetof(Rml::Vertex, colour)), D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, static_cast<UINT>(offsetof(Rml::Vertex, tex_coord)), D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    if (FAILED(m_device->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &m_inputLayout))) return false;

    D3D11_BUFFER_DESC constantDesc{};
    constantDesc.ByteWidth = (sizeof(Constants) + 15u) & ~15u;
    constantDesc.Usage = D3D11_USAGE_DYNAMIC;
    constantDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    constantDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&constantDesc, nullptr, &m_constantBuffer))) return false;

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_device->CreateSamplerState(&samplerDesc, &m_sampler))) return false;

    D3D11_BLEND_DESC blendDesc{};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    // RmlUi 6 submits premultiplied vertex colors and generated textures.
    // Match the official DX11 backend's ONE / INV_SRC_ALPHA blend convention.
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_device->CreateBlendState(&blendDesc, &m_blendState))) return false;

    auto makeRasterizer = [&](bool scissor, ComPtr<ID3D11RasterizerState>& target) {
        D3D11_RASTERIZER_DESC desc{};
        desc.FillMode = D3D11_FILL_SOLID;
        desc.CullMode = D3D11_CULL_NONE;
        desc.ScissorEnable = scissor ? TRUE : FALSE;
        desc.DepthClipEnable = TRUE;
        return SUCCEEDED(m_device->CreateRasterizerState(&desc, &target));
    };
    if (!makeRasterizer(false, m_rasterizerNoScissor) || !makeRasterizer(true, m_rasterizerScissor)) return false;

    D3D11_DEPTH_STENCIL_DESC depthDesc{};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    if (FAILED(m_device->CreateDepthStencilState(&depthDesc, &m_depthState))) return false;
    return true;
}

void RmlRenderInterfaceD3D11::UpdateConstants(Rml::TextureHandle texture, const Rml::Vector2f& translation) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    auto* constants = static_cast<Constants*>(mapped.pData);
    constants->viewport[0] = static_cast<float>(m_width);
    constants->viewport[1] = static_cast<float>(m_height);
    constants->translation[0] = translation.x;
    constants->translation[1] = translation.y;
    std::memcpy(constants->transform, m_transform.data(), sizeof(constants->transform));
    constants->hasTransform = m_hasTransform ? 1 : 0;
    constants->hasTexture = texture ? 1 : 0;
    m_context->Unmap(m_constantBuffer.Get(), 0);
}

void RmlRenderInterfaceD3D11::DrawGeometry(ID3D11Buffer* vertexBuffer, ID3D11Buffer* indexBuffer, int indexCount,
                                           Rml::TextureHandle texture, const Rml::Vector2f& translation) {
    if (!m_context || !vertexBuffer || !indexBuffer || indexCount <= 0) return;
    UpdateConstants(texture, translation);

    D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height), 0.0f, 1.0f};
    m_context->RSSetViewports(1, &viewport);
    m_context->RSSetState(m_scissorEnabled ? m_rasterizerScissor.Get() : m_rasterizerNoScissor.Get());
    if (m_scissorEnabled) m_context->RSSetScissorRects(1, &m_scissor);

    UINT stride = sizeof(Rml::Vertex), offset = 0;
    m_context->IASetInputLayout(m_inputLayout.Get());
    m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    m_context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_context->VSSetShader(m_vertexShader.Get(), nullptr, 0);
    ID3D11Buffer* cb = m_constantBuffer.Get();
    m_context->VSSetConstantBuffers(0, 1, &cb);
    // UiConstants is declared in both shader stages. Without binding b0 to the
    // pixel shader, `hasTexture` reads as zero and every glyph renders as its
    // solid geometry quad instead of sampling the font atlas.
    m_context->PSSetConstantBuffers(0, 1, &cb);
    m_context->PSSetShader(m_pixelShader.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = m_sampler.Get();
    m_context->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* srv = reinterpret_cast<ID3D11ShaderResourceView*>(texture);
    m_context->PSSetShaderResources(0, 1, &srv);
    const float blendFactor[4] = {0, 0, 0, 0};
    m_context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xffffffffu);
    m_context->OMSetDepthStencilState(m_depthState.Get(), 0);
    m_context->DrawIndexed(static_cast<UINT>(indexCount), 0, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    m_context->PSSetShaderResources(0, 1, &nullSrv);
}

Rml::CompiledGeometryHandle RmlRenderInterfaceD3D11::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                                     Rml::Span<const int> indices) {
    if (!m_device || vertices.empty() || indices.empty()) return 0;
    if (vertices.size() > (std::numeric_limits<UINT>::max() / sizeof(Rml::Vertex)) ||
        indices.size() > (std::numeric_limits<UINT>::max() / sizeof(int)) ||
        indices.size() > static_cast<size_t>(std::numeric_limits<int>::max())) return 0;

    auto geometry = std::make_unique<GpuGeometry>();
    geometry->indexCount = static_cast<int>(indices.size());

    D3D11_BUFFER_DESC vbDesc{};
    vbDesc.ByteWidth = static_cast<UINT>(sizeof(Rml::Vertex) * vertices.size());
    vbDesc.Usage = D3D11_USAGE_IMMUTABLE;
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA vbData{vertices.data(), 0, 0};
    if (FAILED(m_device->CreateBuffer(&vbDesc, &vbData, &geometry->vertexBuffer))) return 0;

    D3D11_BUFFER_DESC ibDesc{};
    ibDesc.ByteWidth = static_cast<UINT>(sizeof(int) * indices.size());
    ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
    ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA ibData{indices.data(), 0, 0};
    if (FAILED(m_device->CreateBuffer(&ibDesc, &ibData, &geometry->indexBuffer))) return 0;

    return reinterpret_cast<Rml::CompiledGeometryHandle>(geometry.release());
}

void RmlRenderInterfaceD3D11::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                                             Rml::TextureHandle texture) {
    auto* geometry = reinterpret_cast<GpuGeometry*>(handle);
    if (!geometry) return;
    // Texture ownership/state is deliberately supplied per draw by RmlUi 6. Do
    // not cache it in compiled geometry: font atlas textures may be regenerated
    // as new glyphs are requested (for example when Settings opens).
    DrawGeometry(geometry->vertexBuffer.Get(), geometry->indexBuffer.Get(), geometry->indexCount, texture, translation);
}

void RmlRenderInterfaceD3D11::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    delete reinterpret_cast<GpuGeometry*>(handle);
}

void RmlRenderInterfaceD3D11::EnableScissorRegion(bool enable) {
    m_scissorEnabled = enable;
}

void RmlRenderInterfaceD3D11::SetScissorRegion(Rml::Rectanglei region) {
    // Scissor coordinates are always in window space in the RmlUi 6 API.
    const long left = std::clamp<long>(region.Left(), 0, m_width);
    const long top = std::clamp<long>(region.Top(), 0, m_height);
    const long right = std::max(left, std::clamp<long>(region.Right(), 0, m_width));
    const long bottom = std::max(top, std::clamp<long>(region.Bottom(), 0, m_height));
    m_scissor = {left, top, right, bottom};
}

bool RmlRenderInterfaceD3D11::CreateTextureFromRgba(const unsigned char* rgba, int width, int height, Rml::TextureHandle& handle) {
    if (!m_device || !rgba || width <= 0 || height <= 0) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{rgba, static_cast<UINT>(width * 4), 0};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(m_device->CreateTexture2D(&desc, &data, &texture))) return false;
    ID3D11ShaderResourceView* srv = nullptr;
    if (FAILED(m_device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) return false;
    handle = reinterpret_cast<Rml::TextureHandle>(srv);
    return true;
}

bool RmlRenderInterfaceD3D11::LoadEmbeddedPng(const std::string& source, Rml::TextureHandle& handle, Rml::Vector2i& dimensions) {
    const std::string resourceName = ResourceNameFromSource(source);
    if (resourceName.empty()) return false;
    HMODULE module = GetModuleHandleW(nullptr);
    HRSRC resource = FindResourceA(module, resourceName.c_str(), RT_RCDATA);
    if (!resource) {
        // Fallback to disk for tests or unpacked runs without Windows RC resources
        std::string subPath = source;
        constexpr const char* prefix = "res://images/";
        if (subPath.rfind(prefix, 0) == 0) subPath.erase(0, std::strlen(prefix));
        const std::string relPath = (subPath == "Logo.png") ? "resources/Logo.png" : ("resources/images/" + subPath);
        std::vector<std::string> candidates = {relPath};
#ifdef OMNISTATS_SOURCE_DIR
        candidates.push_back(std::string(OMNISTATS_SOURCE_DIR) + "/" + relPath);
#endif
        for (const auto& candidate : candidates) {
            if (LoadDiskPng(candidate, handle, dimensions)) return true;
        }
        return false;
    }
    HGLOBAL loaded = LoadResource(module, resource);
    const void* bytes = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = loaded ? SizeofResource(module, resource) : 0;
    if (!bytes || !size) return false;

    auto* stream = new MemoryStream(bytes, size);
    if (!stream->Valid()) {
        stream->Release();
        return false;
    }
    Gdiplus::Bitmap bitmap(stream, FALSE);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) {
        stream->Release();
        return false;
    }
    // GDI+ requires the source stream to remain alive for the lifetime of the
    // image while pixels are being decoded.
    std::vector<unsigned char> rgba;
    int width = 0, height = 0;
    const bool decoded = DecodePng(bitmap, rgba, width, height);
    stream->Release();
    if (!decoded) return false;
    dimensions = {width, height};
    return CreateTextureFromRgba(rgba.data(), width, height, handle);
}

bool RmlRenderInterfaceD3D11::LoadDiskPng(const std::string& source, Rml::TextureHandle& handle, Rml::Vector2i& dimensions) {
    Gdiplus::Bitmap bitmap(ToWide(source).c_str(), FALSE);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return false;
    std::vector<unsigned char> rgba;
    int width = 0, height = 0;
    if (!DecodePng(bitmap, rgba, width, height)) return false;
    dimensions = {width, height};
    return CreateTextureFromRgba(rgba.data(), width, height, handle);
}

// `gen://sv?h=<degrees>` renders a saturation/value square for one hue and
// `gen://hue` the full hue ramp. Both are small; RmlUi caches them per source
// string, and the picker quantizes the hue so the cache stays bounded.
bool RmlRenderInterfaceD3D11::GenerateGradient(const std::string& source, Rml::TextureHandle& handle, Rml::Vector2i& dimensions) {
    const auto hsvToRgb = [](float hueDegrees, float saturation, float value, unsigned char* out) {
        const float h = std::fmod(std::fmod(hueDegrees, 360.0f) + 360.0f, 360.0f) / 60.0f;
        const float c = value * saturation;
        const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
        const float m = value - c;
        float rgb[3] = {m, m, m};
        switch (static_cast<int>(h)) {
        case 0:
            rgb[0] += c;
            rgb[1] += x;
            break;
        case 1:
            rgb[0] += x;
            rgb[1] += c;
            break;
        case 2:
            rgb[1] += c;
            rgb[2] += x;
            break;
        case 3:
            rgb[1] += x;
            rgb[2] += c;
            break;
        case 4:
            rgb[0] += x;
            rgb[2] += c;
            break;
        default:
            rgb[0] += c;
            rgb[2] += x;
            break;
        }
        for (int i = 0; i < 3; ++i)
            out[i] = static_cast<unsigned char>(std::lround(std::clamp(rgb[i], 0.0f, 1.0f) * 255.0f));
        out[3] = 255;
    };

    if (source.rfind("gen://hue", 0) == 0) {
        constexpr int width = 256;
        constexpr int height = 8;
        std::vector<unsigned char> rgba(static_cast<size_t>(width) * height * 4);
        for (int x = 0; x < width; ++x) {
            unsigned char pixel[4];
            hsvToRgb(360.0f * static_cast<float>(x) / static_cast<float>(width - 1), 1.0f, 1.0f, pixel);
            for (int y = 0; y < height; ++y)
                std::memcpy(rgba.data() + (static_cast<size_t>(y) * width + x) * 4, pixel, 4);
        }
        dimensions = {width, height};
        return CreateTextureFromRgba(rgba.data(), width, height, handle);
    }

    if (source.rfind("gen://sv", 0) == 0) {
        float hue = 0.0f;
        if (const size_t pos = source.find("h="); pos != std::string::npos)
            hue = std::strtof(source.c_str() + pos + 2, nullptr);
        constexpr int size = 64;
        std::vector<unsigned char> rgba(static_cast<size_t>(size) * size * 4);
        for (int y = 0; y < size; ++y) {
            const float value = 1.0f - static_cast<float>(y) / static_cast<float>(size - 1);
            for (int x = 0; x < size; ++x) {
                const float saturation = static_cast<float>(x) / static_cast<float>(size - 1);
                hsvToRgb(hue, saturation, value, rgba.data() + (static_cast<size_t>(y) * size + x) * 4);
            }
        }
        dimensions = {size, size};
        return CreateTextureFromRgba(rgba.data(), size, size, handle);
    }

    return false;
}

Rml::TextureHandle RmlRenderInterfaceD3D11::LoadTexture(Rml::Vector2i& dimensions, const Rml::String& source) {
    Rml::TextureHandle textureHandle = 0;
    const bool loaded = source.rfind("gen://", 0) == 0
                            ? GenerateGradient(source, textureHandle, dimensions)
                        : source.rfind("res://", 0) == 0
                            ? LoadEmbeddedPng(source, textureHandle, dimensions)
                            : LoadDiskPng(source, textureHandle, dimensions);
    return loaded ? textureHandle : Rml::TextureHandle(0);
}

Rml::TextureHandle RmlRenderInterfaceD3D11::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    const size_t expected = static_cast<size_t>(std::max(dimensions.x, 0)) * static_cast<size_t>(std::max(dimensions.y, 0)) * 4u;
    if (expected == 0 || source.size() < expected) return 0;
    Rml::TextureHandle textureHandle = 0;
    return CreateTextureFromRgba(source.data(), dimensions.x, dimensions.y, textureHandle) ? textureHandle : Rml::TextureHandle(0);
}

void RmlRenderInterfaceD3D11::ReleaseTexture(Rml::TextureHandle texture) {
    if (auto* srv = reinterpret_cast<ID3D11ShaderResourceView*>(texture)) srv->Release();
}

void RmlRenderInterfaceD3D11::SetTransform(const Rml::Matrix4f* transform) {
    if (transform) {
        m_transform = *transform;
        m_hasTransform = true;
    } else {
        m_transform = Rml::Matrix4f::Identity();
        m_hasTransform = false;
    }
}
