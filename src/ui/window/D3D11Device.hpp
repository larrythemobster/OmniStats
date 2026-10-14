#pragma once
#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

class D3D11Device {
  public:
    D3D11Device() = default;
    ~D3D11Device() {
        Shutdown();
    }

    D3D11Device(const D3D11Device&) = delete;
    D3D11Device& operator=(const D3D11Device&) = delete;

    [[nodiscard]] bool Create(HWND hwnd);
    void Shutdown();

    [[nodiscard]] bool CreateRenderTarget();
    void CleanupRenderTarget();
    HRESULT ResizeBuffers(int width, int height);

    ID3D11Device* Device() const {
        return m_device.Get();
    }
    ID3D11DeviceContext* Context() const {
        return m_context.Get();
    }
    IDXGISwapChain* SwapChain() const {
        return m_swapChain.Get();
    }
    ID3D11RenderTargetView* RenderTargetView() const {
        return m_renderTargetView.Get();
    }
    ID3D11RenderTargetView* const* RenderTargetViewAddress() const {
        return m_renderTargetView.GetAddressOf();
    }
    int Width() const {
        return m_width;
    }
    int Height() const {
        return m_height;
    }

  private:
    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_renderTargetView;
    int m_width = 0;
    int m_height = 0;
};
