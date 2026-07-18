#pragma once
#include <d3d11.h>
#include <windows.h>

class D3D11Device {
  public:
    D3D11Device() = default;
    ~D3D11Device() {
        Shutdown();
    }

    D3D11Device(const D3D11Device&) = delete;
    D3D11Device& operator=(const D3D11Device&) = delete;

    bool Create(HWND hwnd);
    void Shutdown();

    bool CreateRenderTarget();
    void CleanupRenderTarget();
    HRESULT ResizeBuffers(int width, int height);

    ID3D11Device* Device() const {
        return m_device;
    }
    ID3D11DeviceContext* Context() const {
        return m_context;
    }
    IDXGISwapChain* SwapChain() const {
        return m_swapChain;
    }
    ID3D11RenderTargetView* RenderTargetView() const {
        return m_renderTargetView;
    }
    ID3D11RenderTargetView* const* RenderTargetViewAddress() const {
        return &m_renderTargetView;
    }

  private:
    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;
    IDXGISwapChain* m_swapChain = nullptr;
    ID3D11RenderTargetView* m_renderTargetView = nullptr;
};
