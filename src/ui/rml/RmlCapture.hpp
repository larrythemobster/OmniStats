#pragma once

#include <d3d11.h>

#include <string>

namespace RmlCapture {
    // Copies a pixel rectangle of the render target currently bound to
    // `context` and writes it as a PNG. Pixels are stored premultiplied, so
    // partially transparent pixels (rounded corners over a transparent
    // overlay) are converted back to straight alpha.
    bool SaveBoundRenderTargetRegion(ID3D11DeviceContext* context, int x, int y, int width, int height,
                                     const std::wstring& path, std::string& error);

    // %USERPROFILE%\Pictures\OmniStats\<prefix>-YYYYMMDD-HHMMSS.png, creating the folder.
    std::wstring NewPicturePath(const wchar_t* prefix);
}
