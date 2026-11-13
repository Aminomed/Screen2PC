#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <vector>

namespace s2p {

class D3D11Renderer {
public:
    D3D11Renderer() = default;
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    // Must be called from the thread that will pump messages & Present() for this window.
    bool Create(uint32_t monitorIndex,
                const RECT& targetMonitorRect,
                uint32_t sourceWidth,
                uint32_t sourceHeight,
                bool startFullscreen = true);

    void Destroy();
    bool IsClosedByUser() const { return m_closedByUser; }

    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }

    // Texture suitable for H264Decoder GPU output (B8G8R8A8_UNORM, RENDER_TARGET | SHADER_RESOURCE)
    ID3D11Texture2D* FrameTexture() const { return m_frameTex.Get(); }
    bool EnsureFrameTexture(uint32_t width, uint32_t height);

    void UploadCpuBgra(const uint8_t* bgra, uint32_t strideBytes, uint32_t width, uint32_t height);
    void UpdateCursor(bool visible,
                      int32_t x,
                      int32_t y,
                      bool shapeChanged,
                      uint8_t shapeType,
                      uint32_t width,
                      uint32_t height,
                      uint32_t pitch,
                      uint32_t hotX,
                      uint32_t hotY,
                      const uint8_t* shapeData,
                      size_t shapeDataSize);

    void ToggleFullscreen();
    void SetFullscreen(bool fullscreen);
    void PumpMessages();
    void Present();
    bool NeedsRepaint() const { return m_needRepaint; }
    void ClearRepaint() { m_needRepaint = false; }

    uint32_t MonitorIndex() const { return m_monitorIndex; }
    uint32_t SourceWidth() const { return m_srcWidth; }
    uint32_t SourceHeight() const { return m_srcHeight; }
    uint32_t WindowWidth() const { return m_winWidth; }
    uint32_t WindowHeight() const { return m_winHeight; }
    HWND Hwnd() const { return m_hwnd; }

    // Callback-friendly handle for WndProc
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    bool CreateDeviceAndShaders();
    bool CreateSwapChain(uint32_t w, uint32_t h);
    void ResizeSwapChain(uint32_t w, uint32_t h);
    void UpdateCursorTexture(uint8_t shapeType,
                             uint32_t width,
                             uint32_t height,
                             uint32_t pitch,
                             const uint8_t* data,
                             size_t dataSize);

    uint32_t m_monitorIndex = 0;
    RECT m_targetRect{};
    uint32_t m_srcWidth = 0;
    uint32_t m_srcHeight = 0;
    uint32_t m_winWidth = 1280;
    uint32_t m_winHeight = 720;
    bool m_fullscreen = true;
    bool m_closedByUser = false;
    bool m_needRepaint = false;

    HWND m_hwnd = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_alphaBlend;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_quadCb;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_frameTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_frameSrv;

    // Cursor overlay
    bool m_cursorVisible = false;
    int32_t m_cursorX = 0;
    int32_t m_cursorY = 0;
    uint32_t m_cursorW = 0;
    uint32_t m_cursorH = 0;
    uint32_t m_cursorHotX = 0;
    uint32_t m_cursorHotY = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_cursorTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_cursorSrv;
    FileDropCallback m_fileDropCb;
};

} // namespace s2p
