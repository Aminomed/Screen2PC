#include "Driver.h"
#include <sddl.h>
#include <algorithm>
#include <cstring>

using namespace Microsoft::WRL;

namespace s2p::vdd {

namespace {

inline void FillSignalInfo(DISPLAYCONFIG_VIDEO_SIGNAL_INFO& mode, DWORD width, DWORD height, DWORD vsync, bool bMonitorMode) {
    mode.totalSize.cx = mode.activeSize.cx = width;
    mode.totalSize.cy = mode.activeSize.cy = height;
    mode.AdditionalSignalInfo.vSyncFreqDivider = bMonitorMode ? 0 : 1;
    mode.AdditionalSignalInfo.videoStandard = 255;
    mode.vSyncFreq.Numerator = vsync;
    mode.vSyncFreq.Denominator = 1;
    mode.hSyncFreq.Numerator = vsync * height;
    mode.hSyncFreq.Denominator = 1;
    mode.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_PROGRESSIVE;
    mode.pixelRate = static_cast<UINT64>(vsync) * static_cast<UINT64>(width) * static_cast<UINT64>(height);
}

inline IDDCX_MONITOR_MODE CreateIddCxMonitorMode(DWORD width, DWORD height, DWORD vsync, IDDCX_MONITOR_MODE_ORIGIN origin = IDDCX_MONITOR_MODE_ORIGIN_DRIVER) {
    IDDCX_MONITOR_MODE mode{};
    mode.Size = sizeof(mode);
    mode.Origin = origin;
    FillSignalInfo(mode.MonitorVideoSignalInfo, width, height, vsync, true);
    return mode;
}

inline IDDCX_TARGET_MODE CreateIddCxTargetMode(DWORD width, DWORD height, DWORD vsync) {
    IDDCX_TARGET_MODE mode{};
    mode.Size = sizeof(mode);
    FillSignalInfo(mode.TargetVideoSignalInfo.targetVideoSignalInfo, width, height, vsync, false);
    return mode;
}

VddMonitorSpec SanitizeSpec(const VddMonitorSpec& in) {
    VddMonitorSpec out = in;
    if (out.width < 640 || out.width > 7680) out.width = 1920;
    if (out.height < 480 || out.height > 4320) out.height = 1080;
    if (out.refreshHz < 30 || out.refreshHz > 240) out.refreshHz = 60;
    return out;
}

} // namespace

struct IndirectDeviceContextWrapper {
    IndirectDeviceContext* pContext;
    void Cleanup() {
        delete pContext;
        pContext = nullptr;
    }
};

struct IndirectMonitorContextWrapper {
    IndirectMonitorContext* pContext;
    void Cleanup() {
        delete pContext;
        pContext = nullptr;
    }
};

} // namespace s2p::vdd

using IndirectDeviceContextWrapper = s2p::vdd::IndirectDeviceContextWrapper;
using IndirectMonitorContextWrapper = s2p::vdd::IndirectMonitorContextWrapper;

WDF_DECLARE_CONTEXT_TYPE(IndirectDeviceContextWrapper);
WDF_DECLARE_CONTEXT_TYPE(IndirectMonitorContextWrapper);

extern "C" DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD Screen2PCDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY Screen2PCDeviceD0Entry;
EVT_IDD_CX_ADAPTER_INIT_FINISHED Screen2PCAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES Screen2PCAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION Screen2PCParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES Screen2PCMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES Screen2PCMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN Screen2PCMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN Screen2PCMonitorUnassignSwapChain;

extern "C" BOOL WINAPI DllMain(HINSTANCE, UINT, LPVOID) {
    return TRUE;
}

_Use_decl_annotations_
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath) {
    WDF_DRIVER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    WDF_DRIVER_CONFIG_INIT(&config, Screen2PCDeviceAdd);
    return WdfDriverCreate(pDriverObject, pRegistryPath, &attributes, &config, WDF_NO_HANDLE);
}

_Use_decl_annotations_
NTSTATUS Screen2PCDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT pDeviceInit) {
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceD0Entry = Screen2PCDeviceD0Entry;
    WdfDeviceInitSetPnpPowerEventCallbacks(pDeviceInit, &pnpCallbacks);

    IDD_CX_CLIENT_CONFIG iddConfig;
    IDD_CX_CLIENT_CONFIG_INIT(&iddConfig);
    iddConfig.EvtIddCxAdapterInitFinished = Screen2PCAdapterInitFinished;
    iddConfig.EvtIddCxParseMonitorDescription = Screen2PCParseMonitorDescription;
    iddConfig.EvtIddCxMonitorGetDefaultDescriptionModes = Screen2PCMonitorGetDefaultModes;
    iddConfig.EvtIddCxMonitorQueryTargetModes = Screen2PCMonitorQueryModes;
    iddConfig.EvtIddCxAdapterCommitModes = Screen2PCAdapterCommitModes;
    iddConfig.EvtIddCxMonitorAssignSwapChain = Screen2PCMonitorAssignSwapChain;
    iddConfig.EvtIddCxMonitorUnassignSwapChain = Screen2PCMonitorUnassignSwapChain;

    NTSTATUS status = IddCxDeviceInitConfig(pDeviceInit, &iddConfig);
    if (!NT_SUCCESS(status)) return status;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContextWrapper);
    attr.EvtCleanupCallback = [](WDFOBJECT obj) {
        auto* w = WdfObjectGet_IndirectDeviceContextWrapper(obj);
        if (w) w->Cleanup();
    };

    WDFDEVICE device = nullptr;
    status = WdfDeviceCreate(&pDeviceInit, &attr, &device);
    if (!NT_SUCCESS(status)) return status;

    status = IddCxDeviceInitialize(device);
    if (!NT_SUCCESS(status)) return status;

    auto* w = WdfObjectGet_IndirectDeviceContextWrapper(device);
    w->pContext = new s2p::vdd::IndirectDeviceContext(device);
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS Screen2PCDeviceD0Entry(WDFDEVICE device, WDF_POWER_DEVICE_STATE) {
    auto* w = WdfObjectGet_IndirectDeviceContextWrapper(device);
    if (w && w->pContext) {
        w->pContext->InitAdapter();
    }
    return STATUS_SUCCESS;
}

namespace s2p::vdd {

Direct3DDevice::Direct3DDevice(LUID adapterLuid) : AdapterLuid(adapterLuid) {}

HRESULT Direct3DDevice::Init() {
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&DxgiFactory));
    if (FAILED(hr)) return hr;
    hr = DxgiFactory->EnumAdapterByLuid(AdapterLuid, IID_PPV_ARGS(&Adapter));
    if (FAILED(hr)) return hr;
    return D3D11CreateDevice(Adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                             &Device, nullptr, &DeviceContext);
}

SwapChainProcessor::SwapChainProcessor(IDDCX_SWAPCHAIN hSwapChain, std::shared_ptr<Direct3DDevice> device, HANDLE newFrameEvent)
    : m_hSwapChain(hSwapChain), m_device(std::move(device)), m_hAvailableBufferEvent(newFrameEvent) {
    m_hTerminateEvent.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    m_hThread.Attach(CreateThread(nullptr, 0, RunThread, this, 0, nullptr));
}

SwapChainProcessor::~SwapChainProcessor() {
    if (m_hTerminateEvent.IsValid()) {
        SetEvent(m_hTerminateEvent.Get());
    }
    if (m_hThread.IsValid()) {
        WaitForSingleObject(m_hThread.Get(), INFINITE);
    }
}

DWORD CALLBACK SwapChainProcessor::RunThread(LPVOID arg) {
    reinterpret_cast<SwapChainProcessor*>(arg)->Run();
    return 0;
}

void SwapChainProcessor::Run() {
    DWORD avTask = 0;
    HANDLE avHandle = AvSetMmThreadCharacteristicsW(L"Distribution", &avTask);
    RunCore();
    if (m_hSwapChain) {
        WdfObjectDelete(reinterpret_cast<WDFOBJECT>(m_hSwapChain));
        m_hSwapChain = nullptr;
    }
    if (avHandle) {
        AvRevertMmThreadCharacteristics(avHandle);
    }
}

void SwapChainProcessor::RunCore() {
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device->Device.As(&dxgiDevice))) return;

    IDARG_IN_SWAPCHAINSETDEVICE setDev{};
    setDev.pDevice = dxgiDevice.Get();
    if (FAILED(IddCxSwapChainSetDevice(m_hSwapChain, &setDev))) return;

    for (;;) {
        ComPtr<IDXGIResource> acquiredBuffer;
        IDARG_OUT_RELEASEANDACQUIREBUFFER buffer{};
        HRESULT hr = IddCxSwapChainReleaseAndAcquireBuffer(m_hSwapChain, &buffer);
        if (hr == E_PENDING) {
            HANDLE waits[] = { m_hAvailableBufferEvent, m_hTerminateEvent.Get() };
            DWORD wr = WaitForMultipleObjects(ARRAYSIZE(waits), waits, FALSE, 16);
            if (wr == WAIT_OBJECT_0 || wr == WAIT_TIMEOUT) {
                continue;
            }
            break;
        } else if (SUCCEEDED(hr)) {
            acquiredBuffer.Attach(buffer.MetaData.pSurface);
            acquiredBuffer.Reset();
            hr = IddCxSwapChainFinishedProcessingFrame(m_hSwapChain);
            if (FAILED(hr)) break;
        } else {
            break;
        }
    }
}

IndirectMonitorContext::IndirectMonitorContext(IDDCX_MONITOR monitor, const VddMonitorSpec& preferredMode)
    : m_monitor(monitor), m_preferredMode(preferredMode) {}

IndirectMonitorContext::~IndirectMonitorContext() {
    m_processingThread.reset();
}

void IndirectMonitorContext::AssignSwapChain(IDDCX_SWAPCHAIN swapChain, LUID renderAdapter, HANDLE newFrameEvent) {
    m_processingThread.reset();
    auto dev = std::make_shared<Direct3DDevice>(renderAdapter);
    if (FAILED(dev->Init())) {
        WdfObjectDelete(swapChain);
    } else {
        m_processingThread = std::make_unique<SwapChainProcessor>(swapChain, dev, newFrameEvent);
    }
}

void IndirectMonitorContext::UnassignSwapChain() {
    m_processingThread.reset();
}

static std::atomic<IndirectDeviceContext*> g_lastDeviceCtx{ nullptr };

IndirectDeviceContext::IndirectDeviceContext(WDFDEVICE wdfDevice) : m_wdfDevice(wdfDevice) {
    m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_lastDeviceCtx.store(this);
}

IndirectDeviceContext::~IndirectDeviceContext() {
    IndirectDeviceContext* expected = this;
    g_lastDeviceCtx.compare_exchange_strong(expected, nullptr);
    StopPipeServer();
    if (m_stopEvent) {
        CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

void IndirectDeviceContext::InitAdapter() {
    if (m_adapter != nullptr) return;

    IDDCX_ADAPTER_CAPS caps{};
    caps.Size = sizeof(caps);
    caps.MaxMonitorsSupported = kMaxVirtualMonitors;
    caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
    caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
    caps.EndPointDiagnostics.TransmissionType = IDDCX_TRANSMISSION_TYPE_WIRED_OTHER;
    caps.EndPointDiagnostics.pEndPointFriendlyName = L"Screen2PC Virtual Display";
    caps.EndPointDiagnostics.pEndPointManufacturerName = L"Screen2PC";
    caps.EndPointDiagnostics.pEndPointModelName = L"Screen2PC Virtual Monitor";

    IDDCX_ENDPOINT_VERSION ver{};
    ver.Size = sizeof(ver);
    ver.MajorVer = 1;
    caps.EndPointDiagnostics.pFirmwareVersion = &ver;
    caps.EndPointDiagnostics.pHardwareVersion = &ver;

    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectDeviceContextWrapper);

    IDARG_IN_ADAPTER_INIT init{};
    init.WdfDevice = m_wdfDevice;
    init.pCaps = &caps;
    init.ObjectAttributes = &attr;

    g_lastDeviceCtx.store(this);

    IDARG_OUT_ADAPTER_INIT initOut{};
    NTSTATUS status = IddCxAdapterInitAsync(&init, &initOut);
    if (NT_SUCCESS(status)) {
        m_adapter = initOut.AdapterObject;
        auto* w = WdfObjectGet_IndirectDeviceContextWrapper(initOut.AdapterObject);
        w->pContext = this;
    }
}

void IndirectDeviceContext::OnAdapterInitFinished(NTSTATUS status) {
    if (!NT_SUCCESS(status)) return;
    m_adapterReady = true;
    LoadPersistedConfig();
    StartPipeServer();
}

void IndirectDeviceContext::LoadPersistedConfig() {
    HANDLE hFile = CreateFileW(kConfigFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return;
    }
    VddCommand cmd{};
    DWORD readBytes = 0;
    BOOL ok = ReadFile(hFile, &cmd, sizeof(cmd), &readBytes, nullptr);
    CloseHandle(hFile);
    if (ok && readBytes == sizeof(cmd) && cmd.magic == kVddMagic) {
        ApplyMonitorConfig(cmd.monitorCount, cmd.monitors);
    }
}

void IndirectDeviceContext::DepartAllMonitorsLocked() {
    for (IDDCX_MONITOR mon : m_monitors) {
        if (mon) {
            IddCxMonitorDeparture(mon);
        }
    }
    m_monitors.clear();
}

DWORD IndirectDeviceContext::ApplyMonitorConfig(uint32_t count, const VddMonitorSpec* specs) {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    if (!m_adapterReady || !m_adapter) return ERROR_NOT_READY;

    DepartAllMonitorsLocked();

    uint32_t clamped = std::min(count, kMaxVirtualMonitors);
    for (uint32_t i = 0; i < clamped; ++i) {
        VddMonitorSpec spec = SanitizeSpec(specs ? specs[i] : VddMonitorSpec{ 1920, 1080, 60 });

        WDF_OBJECT_ATTRIBUTES attr;
        WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attr, IndirectMonitorContextWrapper);
        attr.EvtCleanupCallback = [](WDFOBJECT obj) {
            auto* w = WdfObjectGet_IndirectMonitorContextWrapper(obj);
            if (w) w->Cleanup();
        };

        IDDCX_MONITOR_INFO info{};
        info.Size = sizeof(info);
        info.MonitorType = DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI;
        info.ConnectorIndex = i;
        info.MonitorDescription.Size = sizeof(info.MonitorDescription);
        info.MonitorDescription.Type = IDDCX_MONITOR_DESCRIPTION_TYPE_EDID;
        info.MonitorDescription.DataSize = 0;
        info.MonitorDescription.pData = nullptr;
        CoCreateGuid(&info.MonitorContainerId);

        IDARG_IN_MONITORCREATE mc{};
        mc.ObjectAttributes = &attr;
        mc.pMonitorInfo = &info;

        IDARG_OUT_MONITORCREATE mcOut{};
        NTSTATUS st = IddCxMonitorCreate(m_adapter, &mc, &mcOut);
        if (!NT_SUCCESS(st)) {
            OutputDebugStringW(L"Screen2PCDriver: IddCxMonitorCreate failed\n");
            return static_cast<DWORD>(st);
        }

        auto* mw = WdfObjectGet_IndirectMonitorContextWrapper(mcOut.MonitorObject);
        mw->pContext = new IndirectMonitorContext(mcOut.MonitorObject, spec);

        IDARG_OUT_MONITORARRIVAL arrOut{};
        st = IddCxMonitorArrival(mcOut.MonitorObject, &arrOut);
        if (!NT_SUCCESS(st)) {
            OutputDebugStringW(L"Screen2PCDriver: IddCxMonitorArrival failed\n");
            WdfObjectDelete(mcOut.MonitorObject);
            return static_cast<DWORD>(st);
        }
        m_monitors.push_back(mcOut.MonitorObject);
    }

    return ERROR_SUCCESS;
}

uint32_t IndirectDeviceContext::ActiveMonitorCount() {
    std::lock_guard<std::mutex> lock(m_monitorMutex);
    return static_cast<uint32_t>(m_monitors.size());
}

void IndirectDeviceContext::StartPipeServer() {
    if (m_pipeThread) return;
    m_stopPipe = false;
    if (m_stopEvent) ResetEvent(m_stopEvent);
    m_pipeThread = CreateThread(nullptr, 0, PipeServerThread, this, 0, nullptr);
}

void IndirectDeviceContext::StopPipeServer() {
    m_stopPipe = true;
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_pipeThread) {
        WaitForSingleObject(m_pipeThread, 2000);
        CloseHandle(m_pipeThread);
        m_pipeThread = nullptr;
    }
}

DWORD CALLBACK IndirectDeviceContext::PipeServerThread(LPVOID arg) {
    reinterpret_cast<IndirectDeviceContext*>(arg)->RunPipeServer();
    return 0;
}

void IndirectDeviceContext::RunPipeServer() {
    PSECURITY_DESCRIPTOR pSd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;

    // Allow SYSTEM, Administrators, and Authenticated Users (Session 1 Sender app) R/W access
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)",
            SDDL_REVISION_1, &pSd, nullptr)) {
        sa.lpSecurityDescriptor = pSd;
    }

    while (!m_stopPipe.load()) {
        HANDLE hPipe = CreateNamedPipeW(
            kPipeName,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            4,
            sizeof(VddReply),
            sizeof(VddCommand),
            0,
            pSd ? &sa : nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            Sleep(250);
            continue;
        }

        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(hPipe, &ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE waits[] = { ov.hEvent, m_stopEvent };
                DWORD wr = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wr != WAIT_OBJECT_0) {
                    CancelIo(hPipe);
                    CloseHandle(ov.hEvent);
                    CloseHandle(hPipe);
                    break;
                }
                DWORD dummy = 0;
                connected = GetOverlappedResult(hPipe, &ov, &dummy, FALSE);
            } else if (err == ERROR_PIPE_CONNECTED) {
                connected = TRUE;
            }
        }
        CloseHandle(ov.hEvent);

        if (connected && !m_stopPipe.load()) {
            VddCommand cmd{};
            DWORD bytesRead = 0;
            if (ReadFile(hPipe, &cmd, sizeof(cmd), &bytesRead, nullptr) &&
                bytesRead == sizeof(cmd) && cmd.magic == kVddMagic) {
                VddReply reply{};
                reply.magic = kVddMagic;
                if (cmd.op == static_cast<uint32_t>(VddOp::Ping)) {
                    reply.status = ERROR_SUCCESS;
                } else if (cmd.op == static_cast<uint32_t>(VddOp::SetMonitors)) {
                    reply.status = ApplyMonitorConfig(cmd.monitorCount, cmd.monitors);
                } else {
                    reply.status = ERROR_INVALID_PARAMETER;
                }
                reply.activeMonitors = ActiveMonitorCount();
                DWORD written = 0;
                WriteFile(hPipe, &reply, sizeof(reply), &written, nullptr);
                FlushFileBuffers(hPipe);
            }
        }

        DisconnectNamedPipe(hPipe);
        CloseHandle(hPipe);
    }

    if (pSd) {
        LocalFree(pSd);
    }
}

} // namespace s2p::vdd

_Use_decl_annotations_
NTSTATUS Screen2PCAdapterInitFinished(IDDCX_ADAPTER adapterObject, const IDARG_IN_ADAPTER_INIT_FINISHED* pInArgs) {
    auto* w = WdfObjectGet_IndirectDeviceContextWrapper(adapterObject);
    s2p::vdd::IndirectDeviceContext* ctx = (w && w->pContext) ? w->pContext : s2p::vdd::g_lastDeviceCtx.load();
    if (ctx) {
        ctx->OnAdapterInitFinished(pInArgs->AdapterInitStatus);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS Screen2PCAdapterCommitModes(IDDCX_ADAPTER, const IDARG_IN_COMMITMODES*) {
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS Screen2PCParseMonitorDescription(const IDARG_IN_PARSEMONITORDESCRIPTION*, IDARG_OUT_PARSEMONITORDESCRIPTION* pOutArgs) {
    pOutArgs->MonitorModeBufferOutputCount = 0;
    return STATUS_INVALID_PARAMETER;
}

_Use_decl_annotations_
NTSTATUS Screen2PCMonitorGetDefaultModes(IDDCX_MONITOR monitorObject,
                                         const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* pInArgs,
                                         IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* pOutArgs) {
    auto* mw = WdfObjectGet_IndirectMonitorContextWrapper(monitorObject);
    s2p::vdd::VddMonitorSpec pref{ 1920, 1080, 60 };
    if (mw && mw->pContext) {
        pref = mw->pContext->PreferredMode();
    }

    std::vector<IDDCX_MONITOR_MODE> modes;
    modes.push_back(s2p::vdd::CreateIddCxMonitorMode(pref.width, pref.height, pref.refreshHz, IDDCX_MONITOR_MODE_ORIGIN_DRIVER));
    if (pref.width != 2560 || pref.height != 1440) {
        modes.push_back(s2p::vdd::CreateIddCxMonitorMode(2560, 1440, 60, IDDCX_MONITOR_MODE_ORIGIN_DRIVER));
    }
    if (pref.width != 1920 || pref.height != 1080) {
        modes.push_back(s2p::vdd::CreateIddCxMonitorMode(1920, 1080, 60, IDDCX_MONITOR_MODE_ORIGIN_DRIVER));
    }
    modes.push_back(s2p::vdd::CreateIddCxMonitorMode(1600, 900, 60, IDDCX_MONITOR_MODE_ORIGIN_DRIVER));
    modes.push_back(s2p::vdd::CreateIddCxMonitorMode(1280, 720, 60, IDDCX_MONITOR_MODE_ORIGIN_DRIVER));

    pOutArgs->DefaultMonitorModeBufferOutputCount = static_cast<UINT>(modes.size());
    pOutArgs->PreferredMonitorModeIdx = 0;
    if (pInArgs->DefaultMonitorModeBufferInputCount >= modes.size()) {
        std::copy(modes.begin(), modes.end(), pInArgs->pDefaultMonitorModes);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS Screen2PCMonitorQueryModes(IDDCX_MONITOR monitorObject,
                                    const IDARG_IN_QUERYTARGETMODES* pInArgs,
                                    IDARG_OUT_QUERYTARGETMODES* pOutArgs) {
    auto* mw = WdfObjectGet_IndirectMonitorContextWrapper(monitorObject);
    s2p::vdd::VddMonitorSpec pref{ 1920, 1080, 60 };
    if (mw && mw->pContext) {
        pref = mw->pContext->PreferredMode();
    }

    std::vector<IDDCX_TARGET_MODE> targets;
    targets.push_back(s2p::vdd::CreateIddCxTargetMode(pref.width, pref.height, pref.refreshHz));
    targets.push_back(s2p::vdd::CreateIddCxTargetMode(3840, 2160, 60));
    targets.push_back(s2p::vdd::CreateIddCxTargetMode(2560, 1440, 144));
    targets.push_back(s2p::vdd::CreateIddCxTargetMode(2560, 1440, 60));
    targets.push_back(s2p::vdd::CreateIddCxTargetMode(1920, 1080, 144));
    targets.push_back(s2p::vdd::CreateIddCxTargetMode(1920, 1080, 60));
    targets.push_back(s2p::vdd::CreateIddCxTargetMode(1600, 900, 60));
    targets.push_back(s2p::vdd::CreateIddCxTargetMode(1280, 720, 60));
    targets.push_back(s2p::vdd::CreateIddCxTargetMode(1024, 768, 60));

    pOutArgs->TargetModeBufferOutputCount = static_cast<UINT>(targets.size());
    if (pInArgs->TargetModeBufferInputCount >= targets.size()) {
        std::copy(targets.begin(), targets.end(), pInArgs->pTargetModes);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS Screen2PCMonitorAssignSwapChain(IDDCX_MONITOR monitorObject, const IDARG_IN_SETSWAPCHAIN* pInArgs) {
    auto* mw = WdfObjectGet_IndirectMonitorContextWrapper(monitorObject);
    if (mw && mw->pContext) {
        mw->pContext->AssignSwapChain(pInArgs->hSwapChain, pInArgs->RenderAdapterLuid, pInArgs->hNextSurfaceAvailable);
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
NTSTATUS Screen2PCMonitorUnassignSwapChain(IDDCX_MONITOR monitorObject) {
    auto* mw = WdfObjectGet_IndirectMonitorContextWrapper(monitorObject);
    if (mw && mw->pContext) {
        mw->pContext->UnassignSwapChain();
    }
    return STATUS_SUCCESS;
}
