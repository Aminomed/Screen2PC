#include "D3D11Renderer.h"
#include "../common/AppIcon.h"
#include "../common/KvmInput.h"
#include "../common/Logger.h"

#include <d3dcompiler.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace s2p {

namespace {

const char* kQuadShaderSrc = R"(
cbuffer QuadCB : register(b0) {
    float4 rectNorm; // xy = offset in clip space (-1..1), zw = size in clip space
};

cbuffer ColorCB : register(b1) {
    float4 params0; // x=gammaPower, y=contrast, z=saturation, w=whiteScale
    float4 params1; // x=redGain, y=greenGain, z=blueGain, w=isHdrOutput
};

struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    // Full-quad triangle strip: 0:(0,0), 1:(1,0), 2:(0,1), 3:(1,1)
    float2 uv = float2((vid & 1) ? 1.0 : 0.0, (vid & 2) ? 1.0 : 0.0);
    VSOut o;
    float x = rectNorm.x + uv.x * rectNorm.z;
    float y = rectNorm.y - uv.y * rectNorm.w;
    o.pos = float4(x, y, 0.0, 1.0);
    o.uv = uv;
    return o;
}

Texture2D tex0 : register(t0);
SamplerState samp0 : register(s0);

float4 PSMain(VSOut i) : SV_Target {
    float4 sampleCol = tex0.Sample(samp0, i.uv);
    float3 rgb = saturate(sampleCol.rgb);
    rgb = saturate((rgb - 0.5) * params0.y + 0.5);
    rgb = pow(max(rgb, 1e-6), params0.x) * step(1e-6, rgb);
    float luma = dot(rgb, float3(0.2126, 0.7152, 0.0722));
    rgb = lerp(luma.xxx, rgb, params0.z);
    rgb = max(rgb, 0.0) * params1.xyz * params0.w;
    if (params1.w < 0.5) {
        rgb = saturate(rgb);
    }
    return float4(rgb, sampleCol.a);
}
)";

struct QuadCB {
    float x, y, w, h;
};

const wchar_t* kRendererClass = L"Screen2PCRenderWnd";

bool QueryDisplayConfigSdrWhiteScale(const wchar_t* gdiDeviceName, float& outScale) {
    UINT32 numPaths = 0;
    UINT32 numModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) != ERROR_SUCCESS || numPaths == 0) {
        return false;
    }
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes);
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(), &numModes, modes.data(), nullptr) != ERROR_SUCCESS) {
        return false;
    }

    for (UINT32 i = 0; i < numPaths; ++i) {
        const auto& p = paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
        srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        srcName.header.size = sizeof(srcName);
        srcName.header.adapterId = p.sourceInfo.adapterId;
        srcName.header.id = p.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&srcName.header) == ERROR_SUCCESS &&
            _wcsicmp(srcName.viewGdiDeviceName, gdiDeviceName) == 0) {
            DISPLAYCONFIG_SDR_WHITE_LEVEL sdrWhite{};
            sdrWhite.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
            sdrWhite.header.size = sizeof(sdrWhite);
            sdrWhite.header.adapterId = p.targetInfo.adapterId;
            sdrWhite.header.id = p.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&sdrWhite.header) == ERROR_SUCCESS && sdrWhite.SDRWhiteLevel >= 500) {
                outScale = std::clamp(static_cast<float>(sdrWhite.SDRWhiteLevel) / 1000.0f, 1.0f, 12.5f);
                return true;
            }
            return false;
        }
    }
    return false;
}

LRESULT CALLBACK StaticRenderWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    D3D11Renderer* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<D3D11Renderer*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<D3D11Renderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleMessage(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterRenderClassOnce() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = StaticRenderWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hIcon = s2p::LoadAppLargeIcon();
    wc.hIconSm = s2p::LoadAppSmallIcon();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kRendererClass;
    RegisterClassExW(&wc);
    registered = true;
}

} // namespace

bool D3D11Renderer::QueryMonitorHdrInfo(HMONITOR hMon, MonitorHdrInfo& out) {
    out = MonitorHdrInfo{};
    out.colorSpaceLabel = L"SDR";
    if (!hMon) return false;

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        return false;
    }

    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    bool matched = false;
    for (UINT ai = 0; !matched; ++ai) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(ai, &adapter) == DXGI_ERROR_NOT_FOUND) break;

        for (UINT oi = 0; !matched; ++oi) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(oi, &output) == DXGI_ERROR_NOT_FOUND) break;

            ComPtr<IDXGIOutput6> out6;
            if (SUCCEEDED(output.As(&out6)) && out6) {
                DXGI_OUTPUT_DESC1 d1{};
                if (SUCCEEDED(out6->GetDesc1(&d1))) {
                    if (d1.Monitor == hMon || _wcsicmp(d1.DeviceName, mi.szDevice) == 0) {
                        matched = true;
                        out.isHdr = (d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020);
                        out.colorSpaceLabel = out.isHdr ? L"HDR10" : L"SDR";
                        break;
                    }
                }
            } else {
                DXGI_OUTPUT_DESC d{};
                if (SUCCEEDED(output->GetDesc(&d))) {
                    if (d.Monitor == hMon || _wcsicmp(d.DeviceName, mi.szDevice) == 0) {
                        matched = true;
                        out.isHdr = false;
                        out.colorSpaceLabel = L"SDR";
                        break;
                    }
                }
            }
        }
    }

    if (out.isHdr) {
        float scale = 2.5f; // Default 200 nits if QueryDisplayConfig is unavailable
        if (QueryDisplayConfigSdrWhiteScale(mi.szDevice, scale)) {
            out.sdrWhiteScale = scale;
        } else {
            out.sdrWhiteScale = 2.5f;
        }
    } else {
        out.sdrWhiteScale = 1.0f;
    }
    return matched;
}

D3D11Renderer::D3D11Renderer() {
    m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

D3D11Renderer::~D3D11Renderer() {
    Destroy();
    if (m_wakeEvent) {
        CloseHandle(m_wakeEvent);
        m_wakeEvent = nullptr;
    }
}

void D3D11Renderer::SignalWake() {
    if (m_wakeEvent) {
        SetEvent(m_wakeEvent);
    }
}

void D3D11Renderer::ShowFromHidden() {
    m_hiddenByUser.store(false);
    if (m_hwnd && IsWindow(m_hwnd)) {
        if (IsIconic(m_hwnd)) {
            ShowWindow(m_hwnd, SW_RESTORE);
        } else {
            ShowWindow(m_hwnd, SW_SHOW);
        }
        SetForegroundWindow(m_hwnd);
    }
    m_needRepaint.store(true);
    SignalWake();
}

void D3D11Renderer::SetColorConfig(const MonitorColorConfig& cfg) {
    MonitorColorConfig clamped = cfg;
    ClampColorConfig(clamped);
    if (clamped.hdrOutputEnabled != m_colorConfig.hdrOutputEnabled && m_monitorIsHdr) {
        m_needSwapChainRebuild = true;
    }
    if (clamped != m_colorConfig) {
        m_colorConfig = clamped;
        m_needRepaint.store(true);
    }
}

void D3D11Renderer::UpdateTitle() {
    if (!m_hwnd) return;
    std::wstring kvmState;
    if (!m_kvmEnabled) {
        kvmState = L"[KVM AUS - Strg+Alt+K]";
    } else if (m_kvmCaptured) {
        kvmState = L"[KVM AN (Alle Bildschirme) - ESC / Strg+Alt+K loest Maus]";
    } else {
        kvmState = L"[KVM BEREIT (Klick ins Bild) - Strg+Alt+K]";
    }
    std::wstring hdrTag = m_hdrSwapChain ? L" [HDR scRGB]" : L"";
    std::wstring title = L"Screen2PC Display " + std::to_wstring(m_monitorIndex + 1) +
                         L" (" + std::to_wstring(m_srcWidth) + L"x" + std::to_wstring(m_srcHeight) +
                         L")" + hdrTag + L" - F11 Vollbild / ESC Fenster - " + kvmState;
    SetWindowTextW(m_hwnd, title.c_str());
}

void D3D11Renderer::UpdateCursorClip() {
    if (!m_hwnd || !m_kvmEnabled || !m_kvmCaptured) {
        ClipCursor(nullptr);
        return;
    }
    RECT rc{};
    if (!GetClientRect(m_hwnd, &rc) || rc.right <= rc.left || rc.bottom <= rc.top) {
        return;
    }
    POINT tl{ rc.left, rc.top };
    POINT br{ rc.right, rc.bottom };
    ClientToScreen(m_hwnd, &tl);
    ClientToScreen(m_hwnd, &br);

    RECT clip{ tl.x, tl.y, br.x, br.y };
    if (clip.right - clip.left > 16 && clip.bottom - clip.top > 16) {
        clip.left += 8;
        clip.top += 8;
        clip.right -= 8;
        clip.bottom -= 8;
    }
    ClipCursor(&clip);
}

void D3D11Renderer::SetKvmCapture(bool captured) {
    if (!m_kvmEnabled) captured = false;
    if (captured == m_kvmCaptured) {
        if (captured) UpdateCursorClip();
        return;
    }
    m_kvmCaptured = captured;
    m_pendingRelDx = 0;
    m_pendingRelDy = 0;
    m_hasLastAbsRaw = false;

    if (m_kvmCaptured && m_hwnd) {
        UpdateCursorClip();
        SetCursor(nullptr);
    } else {
        ClipCursor(nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    }
    UpdateTitle();
}

void D3D11Renderer::ReleaseRemoteModifiers() {
    if (!m_kvmEnabled || !m_kvmKeyCb) return;
    const uint16_t mods[] = { VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN };
    for (uint16_t vk : mods) {
        KvmKeyEvent ev{};
        ev.vk = vk;
        ev.scanCode = static_cast<uint16_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        ev.isDown = 0;
        m_kvmKeyCb(ev);
    }
}

void D3D11Renderer::SetKvmEnabled(bool enabled) {
    if (!enabled) {
        m_suppressKeyUpMask = 0;
        SetKvmCapture(false);
        ReleaseRemoteModifiers();
    }
    m_kvmEnabled = enabled;
    if (enabled && m_hwnd && GetForegroundWindow() == m_hwnd && m_fullscreen && !m_escReleasedCapture) {
        SetKvmCapture(true);
    }
    UpdateTitle();
}

void D3D11Renderer::FlushRelativeMouseMove() {
    if (!m_kvmEnabled || !m_kvmMouseCb) {
        m_pendingRelDx = 0;
        m_pendingRelDy = 0;
        return;
    }
    if (m_pendingRelDx == 0 && m_pendingRelDy == 0) {
        return;
    }
    KvmMouseEvent ev{};
    ev.monitorIndex = m_monitorIndex;
    ev.action = kMouseMove;
    ev.isRelative = 1;
    ev.normX = static_cast<float>(m_pendingRelDx);
    ev.normY = static_cast<float>(m_pendingRelDy);
    ev.wheelDelta = 0;
    m_pendingRelDx = 0;
    m_pendingRelDy = 0;
    m_kvmMouseCb(ev);
}

void D3D11Renderer::WarpSurfaceCursorToClientPoint(int32_t clientX, int32_t clientY) {
    if (!m_kvmEnabled || !m_kvmMouseCb) return;
    float nx = 0.0f, ny = 0.0f;
    if (!KvmInput::MapWindowClientToDisplayNorm(
            clientX, clientY, m_winWidth, m_winHeight, m_srcWidth, m_srcHeight, nx, ny)) {
        return;
    }
    KvmMouseEvent ev{};
    ev.monitorIndex = m_monitorIndex;
    ev.action = kMouseMove;
    ev.isRelative = 0;
    ev.normX = nx;
    ev.normY = ny;
    ev.wheelDelta = 0;
    m_kvmMouseCb(ev);
}

void D3D11Renderer::EmitMouseEvent(uint8_t action, int32_t clientX, int32_t clientY, int32_t wheelDelta) {
    if (!m_kvmEnabled || !m_kvmMouseCb) return;

    if (m_kvmCaptured) {
        if (action == kMouseMove) {
            return; // Handled via WM_INPUT raw deltas for unrestricted multi-monitor roaming
        }
        FlushRelativeMouseMove();
        KvmMouseEvent ev{};
        ev.monitorIndex = m_monitorIndex;
        ev.action = action;
        ev.isRelative = 1;
        ev.normX = 0.0f;
        ev.normY = 0.0f;
        ev.wheelDelta = wheelDelta;
        m_kvmMouseCb(ev);
        return;
    }

    float nx = 0.0f, ny = 0.0f;
    bool inside = KvmInput::MapWindowClientToDisplayNorm(
        clientX, clientY, m_winWidth, m_winHeight, m_srcWidth, m_srcHeight, nx, ny);

    bool isButtonUp = (action == kMouseLeftUp || action == kMouseRightUp ||
                       action == kMouseMidUp || action == kMouseX1Up || action == kMouseX2Up);
    if (!inside && !isButtonUp) {
        return;
    }

    if (action == kMouseMove) {
        if (m_fullscreen && !m_escReleasedCapture && GetForegroundWindow() == m_hwnd && inside) {
            WarpSurfaceCursorToClientPoint(clientX, clientY);
            SetKvmCapture(true);
        }
        return;
    }

    bool isButtonDown = (action == kMouseLeftDown || action == kMouseRightDown ||
                         action == kMouseMidDown || action == kMouseX1Down || action == kMouseX2Down);
    if (isButtonDown && inside) {
        m_escReleasedCapture = false;
        WarpSurfaceCursorToClientPoint(clientX, clientY);
        SetKvmCapture(true);
        KvmMouseEvent ev{};
        ev.monitorIndex = m_monitorIndex;
        ev.action = action;
        ev.isRelative = 1;
        ev.normX = 0.0f;
        ev.normY = 0.0f;
        ev.wheelDelta = wheelDelta;
        m_kvmMouseCb(ev);
        return;
    }

    KvmMouseEvent ev{};
    ev.monitorIndex = m_monitorIndex;
    ev.action = action;
    ev.isRelative = 0;
    ev.normX = nx;
    ev.normY = ny;
    ev.wheelDelta = wheelDelta;
    m_kvmMouseCb(ev);
}

bool D3D11Renderer::Create(uint32_t monitorIndex,
                           const RECT& targetMonitorRect,
                           uint32_t sourceWidth,
                           uint32_t sourceHeight,
                           bool startFullscreen) {
    Destroy();
    RegisterRenderClassOnce();
    if (!m_wakeEvent) {
        m_wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    }

    m_monitorIndex = monitorIndex;
    m_targetRect = targetMonitorRect;
    m_srcWidth = std::max<uint32_t>(sourceWidth, 1);
    m_srcHeight = std::max<uint32_t>(sourceHeight, 1);
    m_fullscreen = startFullscreen;
    m_hiddenByUser.store(false);
    m_hasPendingWork.store(false);
    m_deviceLost.store(false);
    m_kvmCaptured = false;
    m_escReleasedCapture = false;
    m_suppressEscUp = false;

    int monW = targetMonitorRect.right - targetMonitorRect.left;
    int monH = targetMonitorRect.bottom - targetMonitorRect.top;
    if (monW <= 0) monW = static_cast<int>(m_srcWidth);
    if (monH <= 0) monH = static_cast<int>(m_srcHeight);

    DWORD style = m_fullscreen ? WS_POPUP : WS_OVERLAPPEDWINDOW;
    int x = m_fullscreen ? targetMonitorRect.left : (targetMonitorRect.left + 80);
    int y = m_fullscreen ? targetMonitorRect.top : (targetMonitorRect.top + 80);
    int w = m_fullscreen ? monW : std::clamp(monW - 160, std::min(monW, 320), 1280);
    int h = m_fullscreen ? monH : std::clamp(monH - 160, std::min(monH, 240), 720);

    std::wstring title = L"Screen2PC Display " + std::to_wstring(monitorIndex + 1);

    m_hwnd = CreateWindowExW(
        0, kRendererClass, title.c_str(), style,
        x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!m_hwnd) {
        Logger::Instance().Error("RND-WND", L"CreateWindowExW failed, err=" + std::to_wstring(GetLastError()));
        return false;
    }

    m_currentMonitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MonitorHdrInfo hdrInfo{};
    QueryMonitorHdrInfo(m_currentMonitor, hdrInfo);
    m_monitorIsHdr = hdrInfo.isHdr;
    m_osSdrWhiteScale = hdrInfo.sdrWhiteScale;
    m_needSwapChainRebuild = false;

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;
    rid.usUsage = 0x02;
    rid.dwFlags = 0;
    rid.hwndTarget = nullptr; // Follow active keyboard focus across stream windows
    RegisterRawInputDevices(&rid, 1, sizeof(rid));

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    m_winWidth = std::max<uint32_t>(1, static_cast<uint32_t>(rc.right - rc.left));
    m_winHeight = std::max<uint32_t>(1, static_cast<uint32_t>(rc.bottom - rc.top));

    bool ok = false;
    {
        std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
        ok = CreateDeviceAndShaders() &&
             CreateSwapChain(m_winWidth, m_winHeight) &&
             EnsureFrameTexture(m_srcWidth, m_srcHeight);
        if (ok) {
            m_deviceGen.fetch_add(1, std::memory_order_release);
        }
    }
    if (!ok) {
        Destroy();
        return false;
    }
    UpdateTitle();

    ShowWindow(m_hwnd, (m_fullscreen && monitorIndex > 0) ? SW_SHOWNOACTIVATE : SW_SHOW);
    UpdateWindow(m_hwnd);
    if (m_fullscreen && monitorIndex == 0) {
        SetForegroundWindow(m_hwnd);
        SetFocus(m_hwnd);
    }
    DragAcceptFiles(m_hwnd, TRUE);
    return true;
}

void D3D11Renderer::Destroy() {
    SetKvmCapture(false);
    if (m_hwnd && GetCapture() == m_hwnd) {
        ReleaseCapture();
    }
    {
        std::lock_guard<std::mutex> fl(m_cpuFrameMutex);
        m_pendingCpuFrame = PendingCpuFrame{};
    }
    {
        std::lock_guard<std::mutex> cl(m_cursorMutex);
        m_pendingCursor = PendingCursorUpdate{};
    }
    {
        std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
        m_deviceGen.fetch_add(1, std::memory_order_release);
        if (m_frameLatencyWaitable) {
            CloseHandle(m_frameLatencyWaitable);
            m_frameLatencyWaitable = nullptr;
        }
        m_cursorSrv.Reset();
        m_cursorTex.Reset();
        m_frameSrv.Reset();
        m_frameTex.Reset();
        m_colorCb.Reset();
        m_quadCb.Reset();
        m_alphaBlend.Reset();
        m_sampler.Reset();
        m_ps.Reset();
        m_vs.Reset();
        m_rtv.Reset();
        m_swapChain.Reset();
        m_context.Reset();
        m_device.Reset();
        m_swapChainFlags = 0;
        m_allowTearing = false;
        m_useTearingOnPresent = true;
    }
    m_hdrSwapChain = false;
    m_monitorIsHdr = false;
    m_deviceLost.store(false);
    m_needSwapChainRebuild = false;
    m_currentMonitor = nullptr;

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool D3D11Renderer::CreateDeviceAndShaders() {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL flOut{};
    const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        fls, _countof(fls), D3D11_SDK_VERSION,
        &m_device, &flOut, &m_context);

    if (FAILED(hr)) {
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            fls, _countof(fls), D3D11_SDK_VERSION,
            &m_device, &flOut, &m_context);
    }
    if (FAILED(hr)) {
        Logger::Instance().Error("RND-D3D", L"D3D11CreateDevice failed: " + FormatHr(hr));
        return false;
    }

    // Allow multithreaded access for Media Foundation D3D11VA / VideoProcessor
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    ComPtr<ID3DBlob> vsBlob, psBlob, errBlob;
    hr = D3DCompile(kQuadShaderSrc, std::strlen(kQuadShaderSrc), nullptr, nullptr, nullptr,
                    "VSMain", "vs_5_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr)) {
        Logger::Instance().Error("RND-VS", L"D3DCompile VS failed: " + FormatHr(hr));
        return false;
    }
    hr = D3DCompile(kQuadShaderSrc, std::strlen(kQuadShaderSrc), nullptr, nullptr, nullptr,
                    "PSMain", "ps_5_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr)) {
        Logger::Instance().Error("RND-PS", L"D3DCompile PS failed: " + FormatHr(hr));
        return false;
    }

    if (FAILED(m_device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs)) ||
        FAILED(m_device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps))) {
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(m_device->CreateSamplerState(&sd, &m_sampler))) return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(m_device->CreateBlendState(&bd, &m_alphaBlend))) return false;

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(QuadCB);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&cbd, nullptr, &m_quadCb))) return false;

    D3D11_BUFFER_DESC ccbd{};
    ccbd.ByteWidth = sizeof(ColorShaderConstants);
    ccbd.Usage = D3D11_USAGE_DYNAMIC;
    ccbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ccbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(m_device->CreateBuffer(&ccbd, nullptr, &m_colorCb))) return false;

    CreateDefaultCursorTexture();
    return true;
}

void D3D11Renderer::CreateDefaultCursorTexture() {
    if (!m_device) return;
    const uint32_t w = 16;
    const uint32_t h = 24;
    std::vector<uint8_t> bgra(w * h * 4, 0);
    for (uint32_t y = 0; y < 18; ++y) {
        uint32_t rowWidth = std::min<uint32_t>(y + 1, 12);
        for (uint32_t x = 0; x <= rowWidth; ++x) {
            bool border = (x == 0 || x == rowWidth || y == 17);
            size_t idx = (static_cast<size_t>(y) * w + x) * 4;
            bgra[idx + 0] = border ? 0 : 255;
            bgra[idx + 1] = border ? 0 : 255;
            bgra[idx + 2] = border ? 0 : 255;
            bgra[idx + 3] = 255;
        }
    }
    m_cursorW = w;
    m_cursorH = h;
    m_cursorHotX = 0;
    m_cursorHotY = 0;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = bgra.data();
    init.SysMemPitch = w * 4;
    if (SUCCEEDED(m_device->CreateTexture2D(&td, &init, &m_cursorTex))) {
        m_device->CreateShaderResourceView(m_cursorTex.Get(), nullptr, &m_cursorSrv);
    }
}

bool D3D11Renderer::CreateSwapChain(uint32_t w, uint32_t h) {
    if (!m_device || !m_hwnd) return false;
    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
    m_rtv.Reset();
    m_swapChain.Reset();
    m_swapChainFlags = 0;
    m_allowTearing = false;
    m_useTearingOnPresent = true;
    if (m_context) {
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_context->Flush();
    }

    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(m_device.As(&dxgiDev)) ||
        FAILED(dxgiDev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    bool tearingSupported = false;
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(factory.As(&factory5)) && factory5) {
        BOOL allow = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof(allow)))) {
            tearingSupported = (allow == TRUE);
        }
    }

    const UINT tearingBit = tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    const UINT waitableBit = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    UINT flagCandidates[4]{};
    size_t candCount = 0;
    auto addCandidate = [&](UINT f) {
        for (size_t i = 0; i < candCount; ++i) {
            if (flagCandidates[i] == f) return;
        }
        flagCandidates[candCount++] = f;
    };
    addCandidate(tearingBit | waitableBit);
    addCandidate(tearingBit);
    addCandidate(waitableBit);
    addCandidate(0);

    const bool wantHdr = m_monitorIsHdr && m_colorConfig.hdrOutputEnabled;
    m_hdrSwapChain = false;

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = std::max<uint32_t>(w, 1);
    scd.Height = std::max<uint32_t>(h, 1);
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Scaling = DXGI_SCALING_STRETCH;

    if (wantHdr) {
        scd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        for (size_t i = 0; i < candCount; ++i) {
            scd.Flags = flagCandidates[i];
            ComPtr<IDXGISwapChain1> candidateSc;
            HRESULT hrHdr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &scd, nullptr, nullptr, &candidateSc);
            if (SUCCEEDED(hrHdr) && candidateSc) {
                ComPtr<IDXGISwapChain3> sc3;
                UINT support = 0;
                if (SUCCEEDED(candidateSc.As(&sc3)) &&
                    SUCCEEDED(sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support)) &&
                    (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) &&
                    SUCCEEDED(sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709))) {
                    m_swapChain = candidateSc;
                    m_swapChainFlags = scd.Flags;
                    m_hdrSwapChain = true;
                    break;
                }
            }
        }
    }

    if (!m_swapChain) {
        scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        HRESULT lastHr = E_FAIL;
        for (size_t i = 0; i < candCount; ++i) {
            scd.Flags = flagCandidates[i];
            ComPtr<IDXGISwapChain1> candidateSc;
            lastHr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &scd, nullptr, nullptr, &candidateSc);
            if (SUCCEEDED(lastHr) && candidateSc) {
                m_swapChain = candidateSc;
                m_swapChainFlags = scd.Flags;
                break;
            }
        }
        if (!m_swapChain) {
            Logger::Instance().Error("RND-SC", L"CreateSwapChainForHwnd failed: " + FormatHr(lastHr));
            return false;
        }
        ComPtr<IDXGISwapChain3> sc3;
        if (SUCCEEDED(m_swapChain.As(&sc3))) {
            sc3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }
    }

    m_allowTearing = ((m_swapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0);
    if ((m_swapChainFlags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
        ComPtr<IDXGISwapChain2> sc2;
        if (SUCCEEDED(m_swapChain.As(&sc2)) && sc2) {
            sc2->SetMaximumFrameLatency(1);
            m_frameLatencyWaitable = sc2->GetFrameLatencyWaitableObject();
        }
    } else {
        ComPtr<IDXGIDevice1> dxgiDev1;
        if (SUCCEEDED(dxgiDev.As(&dxgiDev1)) && dxgiDev1) {
            dxgiDev1->SetMaximumFrameLatency(1);
        }
    }

    factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> backBuf;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf))) ||
        FAILED(m_device->CreateRenderTargetView(backBuf.Get(), nullptr, &m_rtv))) {
        return false;
    }

    Logger::Instance().Info("RND-CLR",
        L"Monitor #" + std::to_wstring(m_monitorIndex + 1) +
        (m_hdrSwapChain
            ? (L": scRGB 16-bit HDR active (whiteScale=" + std::to_wstring(m_osSdrWhiteScale) + L")")
            : L": SDR 8-bit B8G8R8A8 active") +
        L" | tearing=" + (m_allowTearing ? L"1" : L"0") +
        L" | waitable=" + (m_frameLatencyWaitable ? L"1" : L"0"));
    return true;
}

void D3D11Renderer::RebuildSwapChainIfNeeded() {
    if (!m_hwnd || !m_device) return;
    HMONITOR curMon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    if (!m_needSwapChainRebuild && curMon == m_currentMonitor) {
        return;
    }

    m_needSwapChainRebuild = false;
    m_currentMonitor = curMon;

    MonitorHdrInfo hdrInfo{};
    QueryMonitorHdrInfo(curMon, hdrInfo);
    bool wantHdr = hdrInfo.isHdr && m_colorConfig.hdrOutputEnabled;
    bool monitorChangedHdr = (hdrInfo.isHdr != m_monitorIsHdr) || (wantHdr != m_hdrSwapChain);
    m_monitorIsHdr = hdrInfo.isHdr;
    m_osSdrWhiteScale = hdrInfo.sdrWhiteScale;

    if (monitorChangedHdr) {
        CreateSwapChain(m_winWidth, m_winHeight);
        UpdateTitle();
    }
    m_needRepaint.store(true);
}

void D3D11Renderer::ResizeSwapChain(uint32_t w, uint32_t h) {
    if (!m_swapChain || !m_context || !m_device || w == 0 || h == 0) return;
    m_winWidth = w;
    m_winHeight = h;
    m_rtv.Reset();
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    HRESULT hr = m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, m_swapChainFlags);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        m_deviceLost.store(true);
        return;
    }
    if (SUCCEEDED(hr)) {
        ComPtr<ID3D11Texture2D> backBuf;
        if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) {
            m_device->CreateRenderTargetView(backBuf.Get(), nullptr, &m_rtv);
        }
    }
    m_needRepaint.store(true);
}

bool D3D11Renderer::EnsureFrameTexture(uint32_t width, uint32_t height) {
    if (!m_device) return false;
    uint32_t targetW = std::max<uint32_t>(width, 1);
    uint32_t targetH = std::max<uint32_t>(height, 1);
    if (m_frameTex && m_srcWidth == targetW && m_srcHeight == targetH) {
        return true;
    }
    m_srcWidth = targetW;
    m_srcHeight = targetH;
    UpdateTitle();
    m_frameSrv.Reset();
    m_frameTex.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_srcWidth;
    td.Height = m_srcHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_frameTex)) ||
        FAILED(m_device->CreateShaderResourceView(m_frameTex.Get(), nullptr, &m_frameSrv))) {
        return false;
    }
    return true;
}

void D3D11Renderer::QueueCpuFrame(const uint8_t* bgra, uint32_t strideBytes, uint32_t width, uint32_t height) {
    if (!bgra || width == 0 || height == 0 ||
        static_cast<uint64_t>(strideBytes) < static_cast<uint64_t>(width) * 4) {
        return;
    }
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    const size_t totalBytes = rowBytes * height;
    {
        std::lock_guard<std::mutex> lock(m_cpuFrameMutex);
        m_pendingCpuFrame.bgra.resize(totalBytes);
        if (strideBytes == rowBytes) {
            std::memcpy(m_pendingCpuFrame.bgra.data(), bgra, totalBytes);
        } else {
            for (uint32_t y = 0; y < height; ++y) {
                std::memcpy(m_pendingCpuFrame.bgra.data() + static_cast<size_t>(y) * rowBytes,
                            bgra + static_cast<size_t>(y) * strideBytes,
                            rowBytes);
            }
        }
        m_pendingCpuFrame.width = width;
        m_pendingCpuFrame.height = height;
        m_pendingCpuFrame.stride = static_cast<uint32_t>(rowBytes);
        m_pendingCpuFrame.dirty = true;
    }
    m_hasPendingWork.store(true, std::memory_order_release);
    SignalWake();
}

void D3D11Renderer::QueueCursorUpdate(bool visible,
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
                                      size_t shapeDataSize) {
    {
        std::lock_guard<std::mutex> lock(m_cursorMutex);
        m_pendingCursor.posDirty = true;
        m_pendingCursor.visible = visible;
        m_pendingCursor.x = x;
        m_pendingCursor.y = y;
        if (shapeChanged && shapeData && shapeDataSize > 0 && width > 0 && height > 0) {
            m_pendingCursor.shapeDirty = true;
            m_pendingCursor.shapeType = shapeType;
            m_pendingCursor.width = width;
            m_pendingCursor.height = height;
            m_pendingCursor.pitch = pitch;
            m_pendingCursor.hotX = hotX;
            m_pendingCursor.hotY = hotY;
            m_pendingCursor.shapeData.assign(shapeData, shapeData + shapeDataSize);
        }
    }
    m_hasPendingWork.store(true, std::memory_order_release);
    SignalWake();
}

void D3D11Renderer::MarkGpuFrameUpdated() {
    m_hasPendingWork.store(true, std::memory_order_release);
    SignalWake();
}

void D3D11Renderer::UploadCpuBgra(const uint8_t* bgra, uint32_t strideBytes, uint32_t width, uint32_t height) {
    if (!m_context || !bgra || width == 0 || height == 0 ||
        static_cast<uint64_t>(strideBytes) < static_cast<uint64_t>(width) * 4) {
        return;
    }
    if (!EnsureFrameTexture(width, height) || !m_frameTex) return;
    m_context->UpdateSubresource(m_frameTex.Get(), 0, nullptr, bgra, strideBytes, 0);
}

void D3D11Renderer::UpdateCursor(bool visible,
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
                                 size_t shapeDataSize) {
    m_cursorVisible = visible;
    m_cursorX = x;
    m_cursorY = y;
    if (shapeChanged && shapeData && shapeDataSize > 0 && width > 0 && height > 0) {
        m_cursorHotX = hotX;
        m_cursorHotY = hotY;
        UpdateCursorTexture(shapeType, width, height, pitch, shapeData, shapeDataSize);
    }
}

void D3D11Renderer::UpdateCursorTexture(uint8_t shapeType,
                                        uint32_t width,
                                        uint32_t height,
                                        uint32_t pitch,
                                        const uint8_t* data,
                                        size_t dataSize) {
    if (!m_device || !data) return;
    // Convert Monochrome (1), Color (2), or MaskedColor (4) to BGRA32
    uint32_t actualH = 0;
    if (!ValidateCursorShapeBuffer(shapeType, width, height, pitch, dataSize, actualH)) {
        return;
    }

    std::vector<uint8_t> bgra(static_cast<size_t>(width) * actualH * 4, 0);
    if (shapeType == 2 || shapeType == 4) {
        for (uint32_t y = 0; y < actualH; ++y) {
            const uint8_t* srcRow = data + static_cast<size_t>(y) * pitch;
            uint8_t* dstRow = bgra.data() + static_cast<size_t>(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x) {
                dstRow[x * 4 + 0] = srcRow[x * 4 + 0];
                dstRow[x * 4 + 1] = srcRow[x * 4 + 1];
                dstRow[x * 4 + 2] = srcRow[x * 4 + 2];
                if (shapeType == 4) {
                    dstRow[x * 4 + 3] = (srcRow[x * 4 + 3] == 0) ? 255 : 0;
                } else {
                    dstRow[x * 4 + 3] = srcRow[x * 4 + 3];
                }
            }
        }
    } else if (shapeType == 1) {
        const size_t maskBytes = static_cast<size_t>(actualH) * pitch;
        for (uint32_t y = 0; y < actualH; ++y) {
            const uint8_t* andRow = data + static_cast<size_t>(y) * pitch;
            const uint8_t* xorRow = data + maskBytes + static_cast<size_t>(y) * pitch;
            uint8_t* dstRow = bgra.data() + static_cast<size_t>(y) * width * 4;
            for (uint32_t x = 0; x < width; ++x) {
                uint8_t bit = static_cast<uint8_t>(0x80 >> (x & 7));
                bool andBit = (andRow[x >> 3] & bit) != 0;
                bool xorBit = (xorRow[x >> 3] & bit) != 0;
                if (!andBit && !xorBit) { // Black
                    dstRow[x * 4 + 0] = 0;
                    dstRow[x * 4 + 1] = 0;
                    dstRow[x * 4 + 2] = 0;
                    dstRow[x * 4 + 3] = 255;
                } else if (!andBit && xorBit) { // White
                    dstRow[x * 4 + 0] = 255;
                    dstRow[x * 4 + 1] = 255;
                    dstRow[x * 4 + 2] = 255;
                    dstRow[x * 4 + 3] = 255;
                } else if (andBit && !xorBit) { // Transparent
                    dstRow[x * 4 + 3] = 0;
                } else { // Inverted -> semi-opaque highlight
                    dstRow[x * 4 + 0] = 180;
                    dstRow[x * 4 + 1] = 180;
                    dstRow[x * 4 + 2] = 180;
                    dstRow[x * 4 + 3] = 160;
                }
            }
        }
    }

    m_cursorW = width;
    m_cursorH = actualH;
    m_cursorSrv.Reset();
    m_cursorTex.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = actualH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = bgra.data();
    init.SysMemPitch = width * 4;

    if (SUCCEEDED(m_device->CreateTexture2D(&td, &init, &m_cursorTex))) {
        m_device->CreateShaderResourceView(m_cursorTex.Get(), nullptr, &m_cursorSrv);
    }
}

void D3D11Renderer::SetFullscreen(bool fullscreen) {
    if (!m_hwnd || m_fullscreen == fullscreen) return;
    m_fullscreen = fullscreen;
    if (m_fullscreen) {
        m_escReleasedCapture = false;
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        int w = m_targetRect.right - m_targetRect.left;
        int h = m_targetRect.bottom - m_targetRect.top;
        if (w <= 0) w = static_cast<int>(m_srcWidth);
        if (h <= 0) h = static_cast<int>(m_srcHeight);
        SetWindowPos(m_hwnd, HWND_TOP, m_targetRect.left, m_targetRect.top, w, h,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        if (m_kvmEnabled && GetForegroundWindow() == m_hwnd) {
            SetKvmCapture(true);
        }
    } else {
        SetKvmCapture(false);
        SetWindowLongPtrW(m_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        int monW = m_targetRect.right - m_targetRect.left;
        int monH = m_targetRect.bottom - m_targetRect.top;
        if (monW <= 0) monW = static_cast<int>(m_srcWidth);
        if (monH <= 0) monH = static_cast<int>(m_srcHeight);
        int w = std::clamp(monW - 120, std::min(monW, 320), 1280);
        int h = std::clamp(monH - 120, std::min(monH, 240), 720);
        SetWindowPos(m_hwnd, HWND_NOTOPMOST, m_targetRect.left + 60, m_targetRect.top + 60, w, h,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    }
}

void D3D11Renderer::ToggleFullscreen() {
    SetFullscreen(!m_fullscreen);
}

void D3D11Renderer::PumpMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    FlushRelativeMouseMove();
}

void D3D11Renderer::Present() {
    const bool visibleForPresent = !m_hiddenByUser.load(std::memory_order_acquire) &&
                                   (!m_hwnd || !IsIconic(m_hwnd));

    // Wait outside m_gpuMutex so recv+decode thread can feed the GPU unhindered
    if (visibleForPresent && m_frameLatencyWaitable) {
        WaitForSingleObjectEx(m_frameLatencyWaitable, 8, FALSE);
    }

    bool hasCpuFrame = false;
    uint32_t cpuW = 0, cpuH = 0, cpuStride = 0;
    {
        std::lock_guard<std::mutex> lock(m_cpuFrameMutex);
        if (m_pendingCpuFrame.dirty) {
            hasCpuFrame = true;
            cpuW = m_pendingCpuFrame.width;
            cpuH = m_pendingCpuFrame.height;
            cpuStride = m_pendingCpuFrame.stride;
            m_cpuUploadScratch.swap(m_pendingCpuFrame.bgra);
            m_pendingCpuFrame.dirty = false;
        }
    }

    PendingCursorUpdate cursorUpd;
    {
        std::lock_guard<std::mutex> lock(m_cursorMutex);
        if (m_pendingCursor.posDirty || m_pendingCursor.shapeDirty) {
            cursorUpd.posDirty = m_pendingCursor.posDirty;
            cursorUpd.visible = m_pendingCursor.visible;
            cursorUpd.x = m_pendingCursor.x;
            cursorUpd.y = m_pendingCursor.y;
            cursorUpd.shapeDirty = m_pendingCursor.shapeDirty;
            if (cursorUpd.shapeDirty) {
                cursorUpd.shapeType = m_pendingCursor.shapeType;
                cursorUpd.width = m_pendingCursor.width;
                cursorUpd.height = m_pendingCursor.height;
                cursorUpd.pitch = m_pendingCursor.pitch;
                cursorUpd.hotX = m_pendingCursor.hotX;
                cursorUpd.hotY = m_pendingCursor.hotY;
                cursorUpd.shapeData.swap(m_pendingCursor.shapeData);
            }
            m_pendingCursor.posDirty = false;
            m_pendingCursor.shapeDirty = false;
        }
    }

    ComPtr<IDXGISwapChain1> scToPresent;
    UINT presentFlags = 0;
    {
        std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
        if (hasCpuFrame && !m_cpuUploadScratch.empty()) {
            UploadCpuBgra(m_cpuUploadScratch.data(), cpuStride, cpuW, cpuH);
        }
        if (cursorUpd.posDirty || cursorUpd.shapeDirty) {
            UpdateCursor(cursorUpd.visible, cursorUpd.x, cursorUpd.y,
                         cursorUpd.shapeDirty, cursorUpd.shapeType,
                         cursorUpd.width, cursorUpd.height, cursorUpd.pitch,
                         cursorUpd.hotX, cursorUpd.hotY,
                         cursorUpd.shapeDirty ? cursorUpd.shapeData.data() : nullptr,
                         cursorUpd.shapeDirty ? cursorUpd.shapeData.size() : 0);
        }

        if (!visibleForPresent) {
            return;
        }

        RebuildSwapChainIfNeeded();
        if (!m_context || !m_swapChain || !m_rtv || !m_frameSrv) return;

        const float clearHdr[4] = { 0.0004f, 0.0004f, 0.0008f, 1.0f };
        const float clearSdr[4] = { 0.02f, 0.02f, 0.03f, 1.0f };
        m_context->ClearRenderTargetView(m_rtv.Get(), m_hdrSwapChain ? clearHdr : clearSdr);
        m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);

        D3D11_VIEWPORT vp{};
        vp.Width = static_cast<float>(m_winWidth);
        vp.Height = static_cast<float>(m_winHeight);
        vp.MaxDepth = 1.0f;
        m_context->RSSetViewports(1, &vp);

        m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        m_context->IASetInputLayout(nullptr);
        m_context->VSSetShader(m_vs.Get(), nullptr, 0);
        m_context->PSSetShader(m_ps.Get(), nullptr, 0);
        m_context->PSSetSamplers(0, 1, m_sampler.GetAddressOf());

        float winAspect = static_cast<float>(m_winWidth) / static_cast<float>(std::max<uint32_t>(1, m_winHeight));
        float srcAspect = static_cast<float>(m_srcWidth) / static_cast<float>(std::max<uint32_t>(1, m_srcHeight));
        float drawW = 2.0f, drawH = 2.0f, offX = -1.0f, offY = 1.0f;
        if (winAspect > srcAspect) {
            drawW = 2.0f * (srcAspect / winAspect);
            offX = -drawW * 0.5f;
        } else if (srcAspect > winAspect) {
            drawH = 2.0f * (winAspect / srcAspect);
            offY = drawH * 0.5f;
        }

        auto uploadColor = [&](bool isCursor) {
            if (!m_colorCb) return;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(m_context->Map(m_colorCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                ColorShaderConstants cb = ComputeColorShaderConstants(m_colorConfig, m_hdrSwapChain, m_osSdrWhiteScale, isCursor);
                std::memcpy(mapped.pData, &cb, sizeof(cb));
                m_context->Unmap(m_colorCb.Get(), 0);
                m_context->PSSetConstantBuffers(1, 1, m_colorCb.GetAddressOf());
            }
        };

        auto drawQuad = [&](float x, float y, float w, float h, ID3D11ShaderResourceView* srv, bool blend, bool isCursor) {
            if (!m_quadCb) return;
            uploadColor(isCursor);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(m_context->Map(m_quadCb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                QuadCB cb{ x, y, w, h };
                std::memcpy(mapped.pData, &cb, sizeof(cb));
                m_context->Unmap(m_quadCb.Get(), 0);
                m_context->VSSetConstantBuffers(0, 1, m_quadCb.GetAddressOf());
                m_context->PSSetShaderResources(0, 1, &srv);
                m_context->OMSetBlendState(blend ? m_alphaBlend.Get() : nullptr, nullptr, 0xFFFFFFFF);
                m_context->Draw(4, 0);
            }
        };

        drawQuad(offX, offY, drawW, drawH, m_frameSrv.Get(), false, false);

        if (m_cursorVisible && m_cursorSrv && m_cursorW > 0 && m_cursorH > 0) {
            float cx = static_cast<float>(m_cursorX - static_cast<int32_t>(m_cursorHotX)) / static_cast<float>(m_srcWidth);
            float cy = static_cast<float>(m_cursorY - static_cast<int32_t>(m_cursorHotY)) / static_cast<float>(m_srcHeight);
            float cw = static_cast<float>(m_cursorW) / static_cast<float>(m_srcWidth);
            float ch = static_cast<float>(m_cursorH) / static_cast<float>(m_srcHeight);
            drawQuad(offX + cx * drawW, offY - cy * drawH, cw * drawW, ch * drawH, m_cursorSrv.Get(), true, true);
        }

        scToPresent = m_swapChain;
        if (m_allowTearing && m_useTearingOnPresent) {
            presentFlags |= DXGI_PRESENT_ALLOW_TEARING;
        }
    }

    // Never hold m_gpuMutex across IDXGISwapChain::Present()
    HRESULT hr = scToPresent->Present(0, presentFlags);
    if (hr == DXGI_ERROR_INVALID_CALL && (presentFlags & DXGI_PRESENT_ALLOW_TEARING) != 0) {
        Logger::Instance().Warn("RND-D3D", L"DXGI_PRESENT_ALLOW_TEARING returned DXGI_ERROR_INVALID_CALL; falling back to standard flip Present.");
        m_useTearingOnPresent = false;
        hr = scToPresent->Present(0, presentFlags & ~DXGI_PRESENT_ALLOW_TEARING);
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        Logger::Instance().Warn("RND-D3D", L"DXGI device lost during Present: " + FormatHr(hr));
        m_deviceLost.store(true);
    }
}

bool D3D11Renderer::ConsumeDeviceLost() {
    bool lost = m_deviceLost.exchange(false);
    if (!lost) {
        std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
        if (m_device && FAILED(m_device->GetDeviceRemovedReason())) {
            lost = true;
        }
    }
    return lost;
}

bool D3D11Renderer::RecreateDeviceAndResources() {
    if (!m_hwnd || !IsWindow(m_hwnd)) return false;

    std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
    m_deviceGen.fetch_add(1, std::memory_order_release);

    if (m_frameLatencyWaitable) {
        CloseHandle(m_frameLatencyWaitable);
        m_frameLatencyWaitable = nullptr;
    }
    m_cursorSrv.Reset();
    m_cursorTex.Reset();
    m_frameSrv.Reset();
    m_frameTex.Reset();
    m_colorCb.Reset();
    m_quadCb.Reset();
    m_alphaBlend.Reset();
    m_sampler.Reset();
    m_ps.Reset();
    m_vs.Reset();
    m_rtv.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
    m_swapChainFlags = 0;
    m_allowTearing = false;
    m_useTearingOnPresent = true;

    m_currentMonitor = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    MonitorHdrInfo hdrInfo{};
    QueryMonitorHdrInfo(m_currentMonitor, hdrInfo);
    m_monitorIsHdr = hdrInfo.isHdr;
    m_osSdrWhiteScale = hdrInfo.sdrWhiteScale;
    m_needSwapChainRebuild = false;

    if (!CreateDeviceAndShaders() ||
        !CreateSwapChain(m_winWidth, m_winHeight) ||
        !EnsureFrameTexture(m_srcWidth, m_srcHeight)) {
        return false;
    }

    m_deviceLost.store(false);
    m_needRepaint.store(true);
    UpdateTitle();
    SignalWake();
    return true;
}

LRESULT D3D11Renderer::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        m_needRepaint.store(true);
        SignalWake();
        return 0;
    }
    case WM_SETCURSOR:
        if (m_kvmEnabled && m_kvmCaptured && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_DISPLAYCHANGE:
        m_needSwapChainRebuild = true;
        m_needRepaint.store(true);
        SignalWake();
        return 0;
    case WM_MOVE: {
        HMONITOR curMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (curMon != m_currentMonitor) {
            m_needSwapChainRebuild = true;
            m_needRepaint.store(true);
            SignalWake();
        }
        if (m_kvmCaptured) {
            UpdateCursorClip();
        }
        return 0;
    }
    case WM_DPICHANGED: {
        if (!m_fullscreen) {
            const RECT* suggested = reinterpret_cast<const RECT*>(lp);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        m_needSwapChainRebuild = true;
        m_needRepaint.store(true);
        SignalWake();
        return 0;
    }
    case WM_SIZE: {
        UINT w = LOWORD(lp);
        UINT h = HIWORD(lp);
        if (w > 0 && h > 0) {
            std::lock_guard<std::mutex> gpuLock(m_gpuMutex);
            if (m_swapChain) {
                ResizeSwapChain(w, h);
            }
        }
        if (m_kvmCaptured) {
            UpdateCursorClip();
        }
        SignalWake();
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            SetKvmCapture(false);
        }
        break;
    case WM_KILLFOCUS: {
        m_suppressKeyUpMask = 0;
        m_suppressEscUp = false;
        SetKvmCapture(false);
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        ReleaseRemoteModifiers();
        break;
    }
    case WM_INPUT: {
        if (!m_kvmEnabled || !m_kvmCaptured) {
            break;
        }
        uint8_t rawBuf[128]{};
        UINT dwSize = sizeof(rawBuf);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, rawBuf, &dwSize, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
            const auto* raw = reinterpret_cast<const RAWINPUT*>(rawBuf);
            if (raw->header.dwType == RIM_TYPEMOUSE) {
                int32_t dx = 0, dy = 0;
                if ((raw->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                    dx = static_cast<int32_t>(raw->data.mouse.lLastX);
                    dy = static_cast<int32_t>(raw->data.mouse.lLastY);
                } else {
                    const bool isVirtualDesk = (raw->data.mouse.usFlags & MOUSE_VIRTUAL_DESKTOP) != 0;
                    int32_t scrW = std::max<int32_t>(1, GetSystemMetrics(isVirtualDesk ? SM_CXVIRTUALSCREEN : SM_CXSCREEN));
                    int32_t scrH = std::max<int32_t>(1, GetSystemMetrics(isVirtualDesk ? SM_CYVIRTUALSCREEN : SM_CYSCREEN));
                    int32_t px = static_cast<int32_t>((static_cast<int64_t>(raw->data.mouse.lLastX) * scrW) / 65535);
                    int32_t py = static_cast<int32_t>((static_cast<int64_t>(raw->data.mouse.lLastY) * scrH) / 65535);
                    if (m_hasLastAbsRaw) {
                        dx = px - m_lastAbsRawX;
                        dy = py - m_lastAbsRawY;
                    }
                    m_lastAbsRawX = px;
                    m_lastAbsRawY = py;
                    m_hasLastAbsRaw = true;
                }
                m_pendingRelDx += dx;
                m_pendingRelDy += dy;
            }
        }
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        if (wp == VK_F11 && ((lp & (1 << 30)) == 0)) {
            ToggleFullscreen();
            return 0;
        }
        if (wp == VK_ESCAPE && (m_fullscreen || m_kvmCaptured) && ((lp & (1 << 30)) == 0)) {
            m_suppressEscUp = true;
            m_escReleasedCapture = true;
            if (m_fullscreen) {
                SetFullscreen(false);
            }
            SetKvmCapture(false);
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) < 0) && (GetKeyState(VK_MENU) < 0) && wp == 'K' && ((lp & (1 << 30)) == 0)) {
            bool enabling = !m_kvmEnabled;
            m_escReleasedCapture = false;
            m_suppressKeyUpMask = 0x7;
            SetKvmEnabled(enabling);
            if (m_kvmToggleCb) {
                m_kvmToggleCb(enabling);
            }
            return 0;
        }
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) {
            break;
        }
        if (m_kvmEnabled && m_kvmKeyCb) {
            KvmKeyEvent ev{};
            ev.vk = static_cast<uint16_t>(wp);
            ev.scanCode = static_cast<uint16_t>((lp >> 16) & 0xFF);
            ev.isDown = 1;
            ev.isExtended = (lp & (1 << 24)) ? 1 : 0;
            m_kvmKeyCb(ev);
            return 0;
        }
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        if (wp == VK_F11) {
            return 0;
        }
        if (wp == VK_ESCAPE && m_suppressEscUp) {
            m_suppressEscUp = false;
            return 0;
        }
        if (msg == WM_SYSKEYUP && wp == VK_F4) {
            break;
        }
        if (m_suppressKeyUpMask != 0) {
            if (wp == 'K' && (m_suppressKeyUpMask & 0x1)) {
                m_suppressKeyUpMask &= ~0x1;
                return 0;
            }
            if (wp == VK_CONTROL && (m_suppressKeyUpMask & 0x2)) {
                m_suppressKeyUpMask &= ~0x2;
                return 0;
            }
            if (wp == VK_MENU && (m_suppressKeyUpMask & 0x4)) {
                m_suppressKeyUpMask &= ~0x4;
                return 0;
            }
        }
        if (m_kvmEnabled && m_kvmKeyCb) {
            KvmKeyEvent ev{};
            ev.vk = static_cast<uint16_t>(wp);
            ev.scanCode = static_cast<uint16_t>((lp >> 16) & 0xFF);
            ev.isDown = 0;
            ev.isExtended = (lp & (1 << 24)) ? 1 : 0;
            m_kvmKeyCb(ev);
            return 0;
        }
        break;
    }
    case WM_MOUSEMOVE:
        EmitMouseEvent(kMouseMove, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        break;
    case WM_LBUTTONDOWN:
        if (GetFocus() != hwnd) {
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        }
        SetCapture(hwnd);
        EmitMouseEvent(kMouseLeftDown, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        EmitMouseEvent(kMouseLeftUp, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_RBUTTONDOWN:
        if (GetFocus() != hwnd) {
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        }
        EmitMouseEvent(kMouseRightDown, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_RBUTTONUP:
        EmitMouseEvent(kMouseRightUp, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_MBUTTONDOWN:
        if (GetFocus() != hwnd) {
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        }
        EmitMouseEvent(kMouseMidDown, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_MBUTTONUP:
        EmitMouseEvent(kMouseMidUp, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_XBUTTONDOWN:
        if (GetFocus() != hwnd) {
            SetForegroundWindow(hwnd);
            SetFocus(hwnd);
        }
        EmitMouseEvent((HIWORD(wp) == XBUTTON1) ? kMouseX1Down : kMouseX2Down,
                       static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return TRUE;
    case WM_XBUTTONUP:
        EmitMouseEvent((HIWORD(wp) == XBUTTON1) ? kMouseX1Up : kMouseX2Up,
                       static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return TRUE;
    case WM_MOUSEWHEEL: {
        POINT pt{ static_cast<LONG>(static_cast<int16_t>(LOWORD(lp))), static_cast<LONG>(static_cast<int16_t>(HIWORD(lp))) };
        ScreenToClient(hwnd, &pt);
        EmitMouseEvent(kMouseWheel, pt.x, pt.y, static_cast<int16_t>(HIWORD(wp)));
        return 0;
    }
    case WM_MOUSEHWHEEL: {
        POINT pt{ static_cast<LONG>(static_cast<int16_t>(LOWORD(lp))), static_cast<LONG>(static_cast<int16_t>(HIWORD(lp))) };
        ScreenToClient(hwnd, &pt);
        EmitMouseEvent(kMouseHWheel, pt.x, pt.y, static_cast<int16_t>(HIWORD(wp)));
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wp);
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFFu, nullptr, 0);
        std::vector<std::wstring> files;
        files.reserve(count);
        for (UINT i = 0; i < count; ++i) {
            UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
            if (len > 0 && len < 32768) {
                std::wstring path(len, L'\0');
                DragQueryFileW(hDrop, i, path.data(), len + 1);
                files.push_back(std::move(path));
            }
        }
        DragFinish(hDrop);
        if (!files.empty() && m_fileDropCb) {
            m_fileDropCb(files);
        }
        return 0;
    }
    case WM_CLOSE:
        SetKvmCapture(false);
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        ReleaseRemoteModifiers();
        m_hiddenByUser.store(true);
        ShowWindow(hwnd, SW_HIDE);
        SignalWake();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace s2p
