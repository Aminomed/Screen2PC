#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "../common/Protocol.h"
#include "../common/ColorProfile.h"

namespace s2p {

class D3D11Renderer {
public:
    using FileDropCallback  = std::function<void(const std::vector<std::wstring>&)>;
    using KvmMouseCallback  = std::function<void(const KvmMouseEvent&)>;
    using KvmKeyCallback    = std::function<void(const KvmKeyEvent&)>;
    using KvmToggleCallback = std::function<void(bool)>;

    D3D11Renderer();
    ~D3D11Renderer();

    D3D11Renderer(const D3D11Renderer&) = delete;
    D3D11Renderer& operator=(const D3D11Renderer&) = delete;

    void SetFileDropCallback(FileDropCallback cb) { m_fileDropCb = std::move(cb); }
    void SetKvmCallbacks(KvmMouseCallback mouseCb, KvmKeyCallback keyCb, KvmToggleCallback toggleCb = nullptr) {
        m_kvmMouseCb = std::move(mouseCb);
        m_kvmKeyCb = std::move(keyCb);
        m_kvmToggleCb = std::move(toggleCb);
    }
    void SetKvmEnabled(bool enabled);
    bool IsKvmEnabled() const { return m_kvmEnabled; }

    // Must be called from the thread that will pump messages & Present() for this window.
    bool Create(uint32_t monitorIndex,
                const RECT& targetMonitorRect,
                uint32_t sourceWidth,
                uint32_t sourceHeight,
                bool startFullscreen = true);

    void Destroy();

    // Hide-on-close semantics: WM_CLOSE hides window instead of ending session
    bool IsClosedByUser() const { return false; }
    bool IsHiddenByUser() const { return m_hiddenByUser.load(); }
    void ShowFromHidden();

    // Thread-safe accessors for decoupled recv+decode worker thread
    std::mutex& GpuMutex() { return m_gpuMutex; }
    uint64_t DeviceGeneration() const { return m_deviceGen.load(std::memory_order_acquire); }
    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }
    ID3D11Texture2D* FrameTexture() const { return m_frameTex.Get(); }
    ID3D11Texture2D* Nv12Texture() const { return m_nv12Tex.Get(); }
    bool HasNv12Pipeline() const { return m_psNv12 != nullptr && m_nv12Tex != nullptr && m_nv12YSrv != nullptr && m_nv12UvSrv != nullptr; }
    bool EnsureFrameTexture(uint32_t width, uint32_t height);

    void QueueCpuFrame(const uint8_t* bgra,
                       uint32_t strideBytes,
                       uint32_t width,
                       uint32_t height,
                       uint32_t dirtyX = 0,
                       uint32_t dirtyY = 0,
                       uint32_t dirtyW = 0,
                       uint32_t dirtyH = 0);
    void QueueCursorUpdate(bool visible,
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
    void MarkGpuFrameUpdated(bool isNv12 = false);

    // Legacy direct calls (Render-thread only)
    void UploadCpuBgra(const uint8_t* bgra,
                       uint32_t strideBytes,
                       uint32_t width,
                       uint32_t height,
                       uint32_t dirtyX = 0,
                       uint32_t dirtyY = 0,
                       uint32_t dirtyW = 0,
                       uint32_t dirtyH = 0);
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

    static bool QueryMonitorHdrInfo(HMONITOR hMon, MonitorHdrInfo& out);

    void SetColorConfig(const MonitorColorConfig& cfg);
    MonitorColorConfig GetColorConfig() const { return m_colorConfig; }
    bool IsHdrSwapChain() const { return m_hdrSwapChain; }
    bool IsMonitorHdr() const { return m_monitorIsHdr; }
    float OsSdrWhiteScale() const { return m_osSdrWhiteScale; }

    void ToggleFullscreen();
    void SetFullscreen(bool fullscreen);
    void PumpMessages();
    void Present();
    bool ConsumeDeviceLost();
    bool RecreateDeviceAndResources();

    bool NeedsRepaint() const { return m_needRepaint.load(); }
    void ClearRepaint() { m_needRepaint = false; }
    bool ConsumePendingWork() { return m_hasPendingWork.exchange(false); }
    void SignalWake();
    HANDLE WakeHandle() const { return m_wakeEvent; }

    uint32_t MonitorIndex() const { return m_monitorIndex; }
    uint32_t SourceWidth() const { return m_srcWidth; }
    uint32_t SourceHeight() const { return m_srcHeight; }
    uint32_t WindowWidth() const { return m_winWidth; }
    uint32_t WindowHeight() const { return m_winHeight; }
    HWND Hwnd() const { return m_hwnd; }

    // Callback-friendly handle for WndProc
    LRESULT HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    struct PendingCpuFrame {
        bool dirty = false;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t stride = 0;
        uint32_t dirtyX = 0;
        uint32_t dirtyY = 0;
        uint32_t dirtyW = 0;
        uint32_t dirtyH = 0;
        std::vector<uint8_t> bgra;
    };

    struct PendingCursorUpdate {
        bool posDirty = false;
        bool visible = false;
        int32_t x = 0;
        int32_t y = 0;
        bool shapeDirty = false;
        uint8_t shapeType = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t pitch = 0;
        uint32_t hotX = 0;
        uint32_t hotY = 0;
        std::vector<uint8_t> shapeData;
    };

    bool CreateDeviceAndShaders();
    void CreateDefaultCursorTexture();
    bool CreateSwapChain(uint32_t w, uint32_t h);
    void RebuildSwapChainIfNeeded();
    void ResizeSwapChain(uint32_t w, uint32_t h);
    void UpdateTitle();
    void SetKvmCapture(bool captured);
    void ReleaseRemoteModifiers();
    void UpdateCursorClip();
    void FlushRelativeMouseMove();
    void WarpSurfaceCursorToClientPoint(int32_t clientX, int32_t clientY);
    void EmitMouseEvent(uint8_t action, int32_t clientX, int32_t clientY, int32_t wheelDelta = 0);
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
    std::atomic<bool> m_hiddenByUser{false};
    std::atomic<bool> m_needRepaint{false};
    std::atomic<bool> m_hasPendingWork{false};
    std::atomic<bool> m_deviceLost{false};
    std::atomic<bool> m_frameIsNv12{false};
    bool m_kvmEnabled = true;
    bool m_kvmCaptured = false;
    bool m_escReleasedCapture = false;
    bool m_suppressEscUp = false;
    uint8_t m_suppressKeyUpMask = 0;
    int32_t m_pendingRelDx = 0;
    int32_t m_pendingRelDy = 0;
    uint64_t m_lastMouseFlushQpc = 0;
    bool m_hasLastAbsRaw = false;
    LONG m_lastAbsRawX = 0;
    LONG m_lastAbsRawY = 0;

    HMONITOR m_currentMonitor = nullptr;
    bool m_monitorIsHdr = false;
    float m_osSdrWhiteScale = 1.0f;
    bool m_hdrSwapChain = false;
    bool m_needSwapChainRebuild = false;
    bool m_allowTearing = false;
    bool m_useTearingOnPresent = true;
    UINT m_swapChainFlags = 0;
    HANDLE m_frameLatencyWaitable = nullptr;
    HANDLE m_wakeEvent = nullptr;
    MonitorColorConfig m_colorConfig{};

    HWND m_hwnd = nullptr;

    mutable std::mutex m_gpuMutex;
    std::atomic<uint64_t> m_deviceGen{0};

    std::mutex m_cpuFrameMutex;
    PendingCpuFrame m_pendingCpuFrame;
    std::vector<uint8_t> m_cpuUploadScratch;

    std::mutex m_cursorMutex;
    PendingCursorUpdate m_pendingCursor;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ps;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_psNv12;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
    Microsoft::WRL::ComPtr<ID3D11BlendState> m_alphaBlend;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_quadCb;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_colorCb;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_frameTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_frameSrv;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_nv12Tex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_nv12YSrv;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_nv12UvSrv;

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
    KvmMouseCallback m_kvmMouseCb;
    KvmKeyCallback m_kvmKeyCb;
    KvmToggleCallback m_kvmToggleCb;
};

} // namespace s2p
