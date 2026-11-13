#include "DxgiCapture.h"
#include "../common/Logger.h"
#include <cstring>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")

namespace s2p {

using Microsoft::WRL::ComPtr;

std::vector<DisplayOutputInfo> DxgiCapture::EnumerateOutputs() {
    std::vector<DisplayOutputInfo> list;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return list;

    for (UINT a = 0; ; ++a) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(a, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 ad{};
        adapter->GetDesc1(&ad);

        for (UINT o = 0; ; ++o) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(o, &output) == DXGI_ERROR_NOT_FOUND) break;
            DXGI_OUTPUT_DESC od{};
            output->GetDesc(&od);
            if (!od.AttachedToDesktop) continue;

            DisplayOutputInfo info{};
            info.adapterIndex = a;
            info.outputIndex = o;
            info.deviceName = od.DeviceName;
            info.desktopRect = od.DesktopCoordinates;
            info.width = static_cast<uint32_t>(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
            info.height = static_cast<uint32_t>(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
            info.isPrimary = (od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0);
            info.adapterLuid = ad.AdapterLuid;
            list.push_back(info);
        }
    }
    return list;
}

DxgiCapture::~DxgiCapture() {
    Shutdown();
}

void DxgiCapture::Shutdown() {
    m_dupl.Reset();
    m_output1.Reset();
    m_stagingTex.Reset();
    m_frameTex.Reset();
    m_context.Reset();
    m_device.Reset();
    m_metaBuf.clear();
    m_width = 0;
    m_height = 0;
    m_initialized = false;
}

bool DxgiCapture::Init(uint32_t adapterIndex, uint32_t outputIndex) {
    Shutdown();
    m_adapterIndex = adapterIndex;
    m_outputIndex = outputIndex;

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    ComPtr<IDXGIAdapter1> adapter;
    hr = factory->EnumAdapters1(adapterIndex, &adapter);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIOutput> output;
    hr = adapter->EnumOutputs(outputIndex, &output);
    if (FAILED(hr)) return false;

    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    m_width = static_cast<uint32_t>(od.DesktopCoordinates.right - od.DesktopCoordinates.left);
    m_height = static_cast<uint32_t>(od.DesktopCoordinates.bottom - od.DesktopCoordinates.top);
    if (m_width == 0 || m_height == 0) return false;

    hr = output.As(&m_output1);
    if (FAILED(hr)) return false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    D3D_FEATURE_LEVEL fl{};
    hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags,
                           nullptr, 0, D3D11_SDK_VERSION, &m_device, &fl, &m_context);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, &m_device, &fl, &m_context);
        if (FAILED(hr)) {
            Logger::Instance().Error("DXGI_D3D", L"D3D11CreateDevice failed: " + FormatHr(hr));
            return false;
        }
    }

    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(m_device.As(&mt)) && mt) {
        mt->SetMultithreadProtected(TRUE);
    }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_width;
    td.Height = m_height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_frameTex))) {
        Shutdown();
        return false;
    }

    D3D11_TEXTURE2D_DESC st = td;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(m_device->CreateTexture2D(&st, nullptr, &m_stagingTex))) {
        Shutdown();
        return false;
    }

    if (!RecreateDuplication()) {
        Shutdown();
        return false;
    }

    m_initialized = true;
    return true;
}

bool DxgiCapture::RecreateDuplication() {
    m_dupl.Reset();
    if (!m_output1 || !m_device) return false;
    HRESULT hr = m_output1->DuplicateOutput(m_device.Get(), &m_dupl);
    if (FAILED(hr)) {
        Logger::Instance().Warn("DXGI_DUP", L"DuplicateOutput failed: " + FormatHr(hr));
        return false;
    }
    return true;
}

bool DxgiCapture::AcquireFrame(uint32_t timeoutMs,
                               bool& outHasNewImage,
                               std::vector<DeltaRect>& outDirtyRects,
                               CapturedCursor& outCursor) {
    outHasNewImage = false;
    outDirtyRects.clear();
    outCursor = CapturedCursor{};

    if (!m_initialized) return false;
    if (!m_dupl) {
        if (!RecreateDuplication()) {
            Sleep(50);
            return true; // transient
        }
    }

    DXGI_OUTDUPL_FRAME_INFO fi{};
    ComPtr<IDXGIResource> res;
    HRESULT hr = m_dupl->AcquireNextFrame(timeoutMs, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return true;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        m_dupl.Reset();
        return true;
    }
    if (FAILED(hr)) {
        Logger::Instance().Warn("DXGI_ACQ", L"AcquireNextFrame failed: " + FormatHr(hr));
        m_dupl.Reset();
        return true;
    }

    if (fi.LastMouseUpdateTime.QuadPart != 0) {
        outCursor.updated = true;
        outCursor.visible = (fi.PointerPosition.Visible != FALSE);
        outCursor.x = fi.PointerPosition.Position.x;
        outCursor.y = fi.PointerPosition.Position.y;
        if (fi.PointerShapeBufferSize > 0) {
            outCursor.shapeData.resize(fi.PointerShapeBufferSize);
            UINT req = 0;
            DXGI_OUTDUPL_POINTER_SHAPE_INFO psi{};
            if (SUCCEEDED(m_dupl->GetFramePointerShape(fi.PointerShapeBufferSize, outCursor.shapeData.data(), &req, &psi))) {
                outCursor.shapeChanged = true;
                outCursor.shapeType = psi.Type;
                outCursor.width = psi.Width;
                outCursor.height = psi.Height;
                outCursor.pitch = psi.Pitch;
                outCursor.hotX = psi.HotSpot.x;
                outCursor.hotY = psi.HotSpot.y;
                outCursor.shapeData.resize(req);
            } else {
                outCursor.shapeData.clear();
            }
        }
    }

    if (fi.LastPresentTime.QuadPart != 0 && res) {
        ComPtr<ID3D11Texture2D> srcTex;
        if (SUCCEEDED(res.As(&srcTex))) {
            m_context->CopyResource(m_frameTex.Get(), srcTex.Get());
            outHasNewImage = true;

            if (fi.TotalMetadataBufferSize > 0) {
                if (m_metaBuf.size() < fi.TotalMetadataBufferSize) {
                    m_metaBuf.resize(fi.TotalMetadataBufferSize);
                }
                UINT moveBytes = 0;
                if (SUCCEEDED(m_dupl->GetFrameMoveRects(static_cast<UINT>(m_metaBuf.size()),
                                                        reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(m_metaBuf.data()),
                                                        &moveBytes))) {
                    UINT moveCount = moveBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
                    auto* moves = reinterpret_cast<const DXGI_OUTDUPL_MOVE_RECT*>(m_metaBuf.data());
                    for (UINT i = 0; i < moveCount; ++i) {
                        const RECT& d = moves[i].DestinationRect;
                        if (d.right > d.left && d.bottom > d.top) {
                            outDirtyRects.push_back({
                                static_cast<uint32_t>(d.left),
                                static_cast<uint32_t>(d.top),
                                static_cast<uint32_t>(d.right - d.left),
                                static_cast<uint32_t>(d.bottom - d.top)
                            });
                        }
                    }
                }
                UINT dirtyBytes = 0;
                if (SUCCEEDED(m_dupl->GetFrameDirtyRects(static_cast<UINT>(m_metaBuf.size()),
                                                         reinterpret_cast<RECT*>(m_metaBuf.data()),
                                                         &dirtyBytes))) {
                    UINT dirtyCount = dirtyBytes / sizeof(RECT);
                    auto* rects = reinterpret_cast<const RECT*>(m_metaBuf.data());
                    for (UINT i = 0; i < dirtyCount; ++i) {
                        const RECT& r = rects[i];
                        if (r.right > r.left && r.bottom > r.top) {
                            outDirtyRects.push_back({
                                static_cast<uint32_t>(r.left),
                                static_cast<uint32_t>(r.top),
                                static_cast<uint32_t>(r.right - r.left),
                                static_cast<uint32_t>(r.bottom - r.top)
                            });
                        }
                    }
                }
            }
        }
    }

    m_dupl->ReleaseFrame();
    return true;
}

bool DxgiCapture::CopyFrameToCpu(std::vector<uint8_t>& outBgra, uint32_t& outStrideBytes) {
    if (!m_initialized || !m_frameTex || !m_stagingTex) return false;
    m_context->CopyResource(m_stagingTex.Get(), m_frameTex.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_stagingTex.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }
    outStrideBytes = m_width * 4;
    outBgra.resize(static_cast<size_t>(outStrideBytes) * m_height);
    for (uint32_t y = 0; y < m_height; ++y) {
        std::memcpy(outBgra.data() + static_cast<size_t>(y) * outStrideBytes,
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    outStrideBytes);
    }
    m_context->Unmap(m_stagingTex.Get(), 0);
    return true;
}

} // namespace s2p
