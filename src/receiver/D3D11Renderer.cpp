#include "D3D11Renderer.h"
#include "../common/KvmInput.h"
#include "../common/Logger.h"

#include <d3dcompiler.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>

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
    return tex0.Sample(samp0, i.uv);
}
)";

struct QuadCB {
    float x, y, w, h;
};

const wchar_t* kRendererClass = L"Screen2PCRenderWnd";

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
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kRendererClass;
    RegisterClassExW(&wc);
    registered = true;
}

} // namespace

D3D11Renderer::~D3D11Renderer() {
    Destroy();
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
    std::wstring title = L"Screen2PC Display " + std::to_wstring(m_monitorIndex + 1) +
                         L" (" + std::to_wstring(m_srcWidth) + L"x" + std::to_wstring(m_srcHeight) +
                         L") - F11 Vollbild / ESC Fenster - " + kvmState;
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

void D3D11Renderer::SetKvmEnabled(bool enabled) {
    if (!enabled) {
        m_suppressKeyUpMask = 0;
        SetKvmCapture(false);
        if (m_kvmEnabled && m_kvmKeyCb) {
            const uint16_t mods[] = { VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN };
            for (uint16_t vk : mods) {
                KvmKeyEvent ev{};
                ev.vk = vk;
                ev.scanCode = static_cast<uint16_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
                ev.isDown = 0;
                m_kvmKeyCb(ev);
            }
        }
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

    m_monitorIndex = monitorIndex;
    m_targetRect = targetMonitorRect;
    m_srcWidth = std::max<uint32_t>(sourceWidth, 1);
    m_srcHeight = std::max<uint32_t>(sourceHeight, 1);
    m_fullscreen = startFullscreen;
    m_closedByUser = false;
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
    int w = m_fullscreen ? monW : std::min(monW - 160, 1280);
    int h = m_fullscreen ? monH : std::min(monH - 160, 720);

    std::wstring title = L"Screen2PC Display " + std::to_wstring(monitorIndex + 1);

    m_hwnd = CreateWindowExW(
        0, kRendererClass, title.c_str(), style,
        x, y, w, h, nullptr, nullptr, GetModuleHandleW(nullptr), this);

    if (!m_hwnd) {
        Logger::Instance().Error("RND-WND", L"CreateWindowExW failed, err=" + std::to_wstring(GetLastError()));
        return false;
    }
    UpdateTitle();

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

    if (!CreateDeviceAndShaders() ||
        !CreateSwapChain(m_winWidth, m_winHeight) ||
        !EnsureFrameTexture(m_srcWidth, m_srcHeight)) {
        Destroy();
        return false;
    }

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
    m_cursorSrv.Reset();
    m_cursorTex.Reset();
    m_frameSrv.Reset();
    m_frameTex.Reset();
    m_quadCb.Reset();
    m_alphaBlend.Reset();
    m_sampler.Reset();
    m_ps.Reset();
    m_vs.Reset();
    m_rtv.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();

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

    CreateDefaultCursorTexture();
    return true;
}

void D3D11Renderer::CreateDefaultCursorTexture() {
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
    ComPtr<IDXGIDevice> dxgiDev;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(m_device.As(&dxgiDev)) ||
        FAILED(dxgiDev->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.Width = w;
    scd.Height = h;
    scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.Scaling = DXGI_SCALING_STRETCH;

    HRESULT hr = factory->CreateSwapChainForHwnd(m_device.Get(), m_hwnd, &scd, nullptr, nullptr, &m_swapChain);
    if (FAILED(hr)) {
        Logger::Instance().Error("RND-SC", L"CreateSwapChainForHwnd failed: " + FormatHr(hr));
        return false;
    }
    factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER);

    ComPtr<ID3D11Texture2D> backBuf;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf))) ||
        FAILED(m_device->CreateRenderTargetView(backBuf.Get(), nullptr, &m_rtv))) {
        return false;
    }
    return true;
}

void D3D11Renderer::ResizeSwapChain(uint32_t w, uint32_t h) {
    if (!m_swapChain || w == 0 || h == 0) return;
    m_winWidth = w;
    m_winHeight = h;
    m_rtv.Reset();
    m_context->OMSetRenderTargets(0, nullptr, nullptr);
    if (SUCCEEDED(m_swapChain->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        ComPtr<ID3D11Texture2D> backBuf;
        if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) {
            m_device->CreateRenderTargetView(backBuf.Get(), nullptr, &m_rtv);
        }
    }
    m_needRepaint = true;
}

bool D3D11Renderer::EnsureFrameTexture(uint32_t width, uint32_t height) {
    if (m_frameTex && m_srcWidth == width && m_srcHeight == height) {
        return true;
    }
    m_srcWidth = std::max<uint32_t>(width, 1);
    m_srcHeight = std::max<uint32_t>(height, 1);
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

void D3D11Renderer::UploadCpuBgra(const uint8_t* bgra, uint32_t strideBytes, uint32_t width, uint32_t height) {
    if (!bgra || width == 0 || height == 0) return;
    if (!EnsureFrameTexture(width, height)) return;
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
    // Convert Monochrome (1), Color (2), or MaskedColor (4) to BGRA32
    uint32_t actualH = height;
    if (shapeType == 1) {
        // DXGI monochrome height includes both AND and XOR masks
        actualH = std::max<uint32_t>(1, height / 2);
    }

    std::vector<uint8_t> bgra(static_cast<size_t>(width) * actualH * 4, 0);
    if (shapeType == 2 || shapeType == 4) {
        for (uint32_t y = 0; y < actualH; ++y) {
            if (static_cast<size_t>(y) * pitch + width * 4 > dataSize) break;
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
        if (maskBytes * 2 <= dataSize) {
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
        int w = std::min(monW - 120, 1280);
        int h = std::min(monH - 120, 720);
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
    if (!m_swapChain || !m_rtv || !m_frameSrv) return;

    const float clear[4] = { 0.04f, 0.04f, 0.06f, 1.0f };
    m_context->ClearRenderTargetView(m_rtv.Get(), clear);
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

    // Aspect-preserving destination rect in normalized [0..1] window coords
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

    auto drawQuad = [&](float x, float y, float w, float h, ID3D11ShaderResourceView* srv, bool blend) {
        if (!m_quadCb) return;
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

    drawQuad(offX, offY, drawW, drawH, m_frameSrv.Get(), false);

    if (m_cursorVisible && m_cursorSrv && m_cursorW > 0 && m_cursorH > 0) {
        float cx = static_cast<float>(m_cursorX - static_cast<int32_t>(m_cursorHotX)) / static_cast<float>(m_srcWidth);
        float cy = static_cast<float>(m_cursorY - static_cast<int32_t>(m_cursorHotY)) / static_cast<float>(m_srcHeight);
        float cw = static_cast<float>(m_cursorW) / static_cast<float>(m_srcWidth);
        float ch = static_cast<float>(m_cursorH) / static_cast<float>(m_srcHeight);
        drawQuad(offX + cx * drawW, offY - cy * drawH, cw * drawW, ch * drawH, m_cursorSrv.Get(), true);
    }

    // Immediate present (syncInterval = 0) for minimum display latency on LAN
    m_swapChain->Present(0, 0);
}

LRESULT D3D11Renderer::HandleMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        m_needRepaint = true;
        return 0;
    }
    case WM_SETCURSOR:
        if (m_kvmEnabled && m_kvmCaptured && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            return TRUE;
        }
        break;
    case WM_MOVE:
        if (m_kvmCaptured) {
            UpdateCursorClip();
        }
        return 0;
    case WM_SIZE: {
        UINT w = LOWORD(lp);
        UINT h = HIWORD(lp);
        if (w > 0 && h > 0 && m_swapChain) {
            ResizeSwapChain(w, h);
        }
        if (m_kvmCaptured) {
            UpdateCursorClip();
        }
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
        if (m_kvmEnabled && m_kvmKeyCb) {
            const uint16_t mods[] = { VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN };
            for (uint16_t vk : mods) {
                KvmKeyEvent ev{};
                ev.vk = vk;
                ev.scanCode = static_cast<uint16_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
                ev.isDown = 0;
                m_kvmKeyCb(ev);
            }
        }
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
        EmitMouseEvent(kMouseRightDown, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_RBUTTONUP:
        EmitMouseEvent(kMouseRightUp, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_MBUTTONDOWN:
        EmitMouseEvent(kMouseMidDown, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_MBUTTONUP:
        EmitMouseEvent(kMouseMidUp, static_cast<int16_t>(LOWORD(lp)), static_cast<int16_t>(HIWORD(lp)));
        return 0;
    case WM_XBUTTONDOWN:
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
        m_closedByUser = true;
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace s2p
