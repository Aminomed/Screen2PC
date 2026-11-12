#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wdf.h>
#include <iddcx.h>
#include <dxgi1_5.h>
#include <d3d11_2.h>
#include <avrt.h>
#include <wrl.h>
#include <memory>
#include <vector>
#include <mutex>
#include <atomic>
#include "DriverProtocol.h"

namespace s2p::vdd {

class Direct3DDevice {
public:
    explicit Direct3DDevice(LUID adapterLuid);
    HRESULT Init();

    LUID AdapterLuid{};
    Microsoft::WRL::ComPtr<IDXGIFactory5> DxgiFactory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> Adapter;
    Microsoft::WRL::ComPtr<ID3D11Device> Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> DeviceContext;
};

class SwapChainProcessor {
public:
    SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent);
    ~SwapChainProcessor();

private:
    static DWORD CALLBACK RunThread(LPVOID arg);
    void Run();
    void RunCore();

    IDDCX_SWAPCHAIN m_hSwapChain = nullptr;
    std::shared_ptr<Direct3DDevice> m_device;
    HANDLE m_hAvailableBufferEvent = nullptr;
    Microsoft::WRL::Wrappers::HandleT<Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits> m_hThread;
    Microsoft::WRL::Wrappers::HandleT<Microsoft::WRL::Wrappers::HandleTraits::HANDLENullTraits> m_hTerminateEvent;
};

class IndirectMonitorContext {
public:
    IndirectMonitorContext(IDDCX_MONITOR monitor, const VddMonitorSpec& preferredMode);
    ~IndirectMonitorContext();

    void AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent);
    void UnassignSwapChain();

    IDDCX_MONITOR MonitorHandle() const { return m_monitor; }
    VddMonitorSpec PreferredMode() const { return m_preferredMode; }

private:
    IDDCX_MONITOR m_monitor = nullptr;
    VddMonitorSpec m_preferredMode{};
    std::unique_ptr<SwapChainProcessor> m_processingThread;
};

class IndirectDeviceContext {
public:
    explicit IndirectDeviceContext(WDFDEVICE wdfDevice);
    ~IndirectDeviceContext();

    void InitAdapter();
    void OnAdapterInitFinished(NTSTATUS status);
    DWORD ApplyMonitorConfig(uint32_t count, const VddMonitorSpec* specs);
    uint32_t ActiveMonitorCount();

private:
    void StartPipeServer();
    void StopPipeServer();
    static DWORD CALLBACK PipeServerThread(LPVOID arg);
    void RunPipeServer();
    void LoadPersistedConfig();
    void DepartAllMonitorsLocked();

    WDFDEVICE m_wdfDevice = nullptr;
    IDDCX_ADAPTER m_adapter = nullptr;
    bool m_adapterReady = false;

    std::mutex m_monitorMutex;
    std::vector<IDDCX_MONITOR> m_monitors;

    std::atomic<bool> m_stopPipe{ false };
    HANDLE m_pipeThread = nullptr;
    HANDLE m_stopEvent = nullptr;
};

} // namespace s2p::vdd
