#include "D3D11Device.hpp"
#include <iostream>

bool D3D11Device::Create(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    // The overlay depends on WS_EX_LAYERED alpha composition. Flip-model swapchains
    // can present transparent backbuffers as black without a DirectComposition path.
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
            featureLevelArray, 2, D3D11_SDK_VERSION, &sd,
            &m_swapChain, &m_device, &featureLevel, &m_context);
    if (FAILED(hr)) {
        std::cout << "[D3D11] Failed to create device and swap chain.\n";
        return false;
    }

    if (!CreateRenderTarget()) {
        std::cout << "[D3D11] Failed to create render target.\n";
        Shutdown();
        return false;
    }
    return true;
}

void D3D11Device::Shutdown() {
    CleanupRenderTarget();
    if (m_swapChain) { m_swapChain->Release(); m_swapChain = nullptr; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
}

bool D3D11Device::CreateRenderTarget() {
    if (!m_swapChain || !m_device) return false;
    ID3D11Texture2D* pBackBuffer = nullptr;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (FAILED(hr)) {
        std::cout << "[D3D11] Failed to get swapchain back buffer: " << std::hex << hr << std::dec << "\n";
        return false;
    }
    hr = m_device->CreateRenderTargetView(pBackBuffer, nullptr, &m_renderTargetView);
    pBackBuffer->Release();
    if (FAILED(hr)) {
        std::cout << "[D3D11] Failed to create render target view: " << std::hex << hr << std::dec << "\n";
        return false;
    }
    return true;
}

void D3D11Device::CleanupRenderTarget() {
    if (m_renderTargetView) { m_renderTargetView->Release(); m_renderTargetView = nullptr; }
}

HRESULT D3D11Device::ResizeBuffers(int width, int height) {
    if (!m_swapChain) return E_FAIL;
    CleanupRenderTarget();
    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        std::cout << "[D3D11] Failed to resize swapchain buffers: " << std::hex << hr << std::dec << "\n";
        return hr;
    }
    if (!CreateRenderTarget()) return E_FAIL;
    return S_OK;
}
