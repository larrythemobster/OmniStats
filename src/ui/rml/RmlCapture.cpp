#include "ui/rml/RmlCapture.hpp"

#include <knownfolders.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {
    bool IsBgra(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
               format == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    }

    bool IsRgba(DXGI_FORMAT format) {
        return format == DXGI_FORMAT_R8G8B8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
               format == DXGI_FORMAT_R8G8B8A8_TYPELESS;
    }

    bool WritePng(const std::wstring& path, UINT width, UINT height, std::vector<unsigned char>& bgra, std::string& error) {
        const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool mustUninit = init == S_OK || init == S_FALSE;
        bool ok = false;
        {
            ComPtr<IWICImagingFactory> factory;
            ComPtr<IWICStream> stream;
            ComPtr<IWICBitmapEncoder> encoder;
            ComPtr<IWICBitmapFrameEncode> frame;
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
                error = "Windows Imaging Component is unavailable.";
            } else if (FAILED(factory->CreateStream(&stream)) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
                error = "Could not create the image file.";
            } else if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
                       FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
                       FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
                       FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format)) ||
                       format != GUID_WICPixelFormat32bppBGRA) {
                error = "Could not start the PNG encoder.";
            } else if (FAILED(frame->WritePixels(height, width * 4, static_cast<UINT>(bgra.size()), bgra.data())) ||
                       FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
                error = "Could not write the PNG.";
            } else {
                ok = true;
            }
        }
        if (mustUninit) CoUninitialize();
        return ok;
    }
}

namespace RmlCapture {
    bool SaveBoundRenderTargetRegion(ID3D11DeviceContext* context, int x, int y, int width, int height,
                                     float cornerRadius, const std::wstring& path, std::string& error) {
        if (!context) {
            error = "No render context.";
            return false;
        }
        ComPtr<ID3D11RenderTargetView> rtv;
        context->OMGetRenderTargets(1, &rtv, nullptr);
        if (!rtv) {
            error = "No render target is bound.";
            return false;
        }
        ComPtr<ID3D11Resource> resource;
        rtv->GetResource(&resource);
        ComPtr<ID3D11Texture2D> source;
        if (FAILED(resource.As(&source))) {
            error = "The render target is not a 2D texture.";
            return false;
        }

        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        if (!IsBgra(desc.Format) && !IsRgba(desc.Format)) {
            error = "Unsupported render target format.";
            return false;
        }
        const int left = std::clamp(x, 0, static_cast<int>(desc.Width));
        const int top = std::clamp(y, 0, static_cast<int>(desc.Height));
        const int right = std::clamp(x + width, left, static_cast<int>(desc.Width));
        const int bottom = std::clamp(y + height, top, static_cast<int>(desc.Height));
        if (right <= left || bottom <= top) {
            error = "The capture area is empty.";
            return false;
        }

        ComPtr<ID3D11Device> device;
        context->GetDevice(&device);
        ComPtr<ID3D11Texture2D> resolved = source;
        if (desc.SampleDesc.Count > 1) {
            D3D11_TEXTURE2D_DESC resolveDesc = desc;
            resolveDesc.SampleDesc = {1, 0};
            resolveDesc.BindFlags = 0;
            resolveDesc.MiscFlags = 0;
            if (FAILED(device->CreateTexture2D(&resolveDesc, nullptr, &resolved))) {
                error = "Could not resolve the render target.";
                return false;
            }
            context->ResolveSubresource(resolved.Get(), 0, source.Get(), 0, desc.Format);
        }

        D3D11_TEXTURE2D_DESC stagingDesc{};
        stagingDesc.Width = static_cast<UINT>(right - left);
        stagingDesc.Height = static_cast<UINT>(bottom - top);
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        stagingDesc.Format = desc.Format;
        stagingDesc.SampleDesc = {1, 0};
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, &staging))) {
            error = "Could not allocate a readback texture.";
            return false;
        }
        const D3D11_BOX box{static_cast<UINT>(left), static_cast<UINT>(top), 0, static_cast<UINT>(right), static_cast<UINT>(bottom), 1};
        context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, resolved.Get(), 0, &box);

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            error = "Could not read the render target.";
            return false;
        }
        const UINT outWidth = stagingDesc.Width;
        const UINT outHeight = stagingDesc.Height;
        std::vector<unsigned char> pixels(static_cast<size_t>(outWidth) * outHeight * 4);
        const bool rgba = IsRgba(desc.Format);
        const float radius = std::clamp(cornerRadius, 0.0f, static_cast<float>(std::min(outWidth, outHeight)) * 0.5f);
        const auto coverage = [&](UINT col, UINT row) {
            if (radius <= 0.0f) return 1.0f;
            const float px = static_cast<float>(col) + 0.5f;
            const float py = static_cast<float>(row) + 0.5f;
            const float cx = std::clamp(px, radius, static_cast<float>(outWidth) - radius);
            const float cy = std::clamp(py, radius, static_cast<float>(outHeight) - radius);
            const float distance = std::hypot(px - cx, py - cy);
            return std::clamp(radius - distance + 0.5f, 0.0f, 1.0f);
        };
        for (UINT row = 0; row < outHeight; ++row) {
            const auto* src = static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(row) * mapped.RowPitch;
            unsigned char* dst = pixels.data() + static_cast<size_t>(row) * outWidth * 4;
            for (UINT col = 0; col < outWidth; ++col, src += 4, dst += 4) {
                const unsigned char alpha = src[3];
                const auto straight = [alpha](unsigned char channel) {
                    return alpha == 0 ? 0 : static_cast<unsigned char>(std::min(255, channel * 255 / alpha));
                };
                dst[0] = straight(rgba ? src[2] : src[0]);
                dst[1] = straight(src[1]);
                dst[2] = straight(rgba ? src[0] : src[2]);
                dst[3] = static_cast<unsigned char>(std::lround(alpha * coverage(col, row)));
            }
        }
        context->Unmap(staging.Get(), 0);
        return WritePng(path, outWidth, outHeight, pixels, error);
    }

    std::wstring NewPicturePath(const wchar_t* prefix) {
        std::filesystem::path folder;
        PWSTR pictures = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures)) && pictures) {
            folder = pictures;
            CoTaskMemFree(pictures);
        } else {
            folder = std::filesystem::temp_directory_path();
        }
        folder /= L"OmniStats";
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);

        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        wchar_t stamp[32]{};
        std::wcsftime(stamp, 32, L"%Y%m%d-%H%M%S", &local);
        return (folder / (std::wstring(prefix) + L"-" + stamp + L".png")).wstring();
    }
}
