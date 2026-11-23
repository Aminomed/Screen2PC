#include "GpuDeltaAnalyzer.h"
#include "../common/Logger.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

namespace s2p {

namespace {

const char* kTileDiffCsHlsl = R"HLSL(
Texture2D<float4> CurTex  : register(t0);
Texture2D<float4> PrevTex : register(t1);
RWStructuredBuffer<uint> OutTiles : register(u0);

cbuffer Params : register(b0) {
    uint Width;
    uint Height;
    uint TilesX;
    uint TilesY;
};

groupshared uint g_dirty;

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    if (gtid.x == 0 && gtid.y == 0) {
        g_dirty = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    uint baseX = gid.x * 64u + gtid.x * 8u;
    uint baseY = gid.y * 64u + gtid.y * 8u;
    uint localDirty = 0;

    if (baseX < Width && baseY < Height) {
        uint maxX = min(baseX + 8u, Width);
        uint maxY = min(baseY + 8u, Height);
        for (uint y = baseY; y < maxY; ++y) {
            for (uint x = baseX; x < maxX; ++x) {
                float4 a = CurTex.Load(int3(int(x), int(y), 0));
                float4 b = PrevTex.Load(int3(int(x), int(y), 0));
                float3 d = abs(a.rgb - b.rgb);
                if (d.r > 0.001f || d.g > 0.001f || d.b > 0.001f) {
                    localDirty = 1u;
                    break;
                }
            }
            if (localDirty != 0u) break;
        }
    }

    if (localDirty != 0u) {
        InterlockedOr(g_dirty, 1u);
    }
    GroupMemoryBarrierWithGroupSync();

    if (gtid.x == 0 && gtid.y == 0) {
        uint idx = gid.y * TilesX + gid.x;
        OutTiles[idx] = g_dirty;
    }
}
)HLSL";

struct alignas(16) CsParams {
    uint32_t width;
    uint32_t height;
    uint32_t tilesX;
    uint32_t tilesY;
};

bool IsSupportedBgraFormat(DXGI_FORMAT fmt) {
    return fmt == DXGI_FORMAT_B8G8R8A8_UNORM ||
           fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM ||
           fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
}

DXGI_FORMAT MakeSrvFormat(DXGI_FORMAT fmt) {
    if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || fmt == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    return fmt;
}

} // namespace

bool GpuDeltaAnalyzer::Init(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height) {
    Shutdown();
    if (!device || !context || width == 0 || height == 0) return false;
    if (width > 16384 || height > 16384) return false;

    m_device = device;
    m_context = context;
    m_width = width;
    m_height = height;
    m_tilesX = (width + kTileSize - 1) / kTileSize;
    m_tilesY = (height + kTileSize - 1) / kTileSize;
    m_hasPrevValid = false;

    if (!CreateResources()) {
        Shutdown();
        return false;
    }

    m_tileFlagsCpu.resize(static_cast<size_t>(m_tilesX) * m_tilesY, 0);
    m_initialized = true;
    Logger::Instance().Info("GPU_DELTA_INIT",
        L"GPU Compute-Shader Delta Analyzer ready (" + std::to_wstring(m_width) + L"x" + std::to_wstring(m_height) +
        L", " + std::to_wstring(m_tilesX) + L"x" + std::to_wstring(m_tilesY) + L" tiles)");
    return true;
}

void GpuDeltaAnalyzer::Shutdown() {
    m_initialized = false;
    m_hasPrevValid = false;
    m_width = 0;
    m_height = 0;
    m_tilesX = 0;
    m_tilesY = 0;
    m_curTexPtr = nullptr;
    m_curTexFmt = DXGI_FORMAT_UNKNOWN;
    m_tileFlagsCpu.clear();

    m_readbackStaging.Reset();
    m_flagsStaging.Reset();
    m_flagsUav.Reset();
    m_flagsBuf.Reset();
    m_curSrv.Reset();
    m_curCopyTex.Reset();
    m_prevSrv.Reset();
    m_prevGpuTex.Reset();
    m_cbParams.Reset();
    m_cs.Reset();
    m_context.Reset();
    m_device.Reset();
}

bool GpuDeltaAnalyzer::CreateResources() {
    Microsoft::WRL::ComPtr<ID3DBlob> csBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> errBlob;
    HRESULT hr = D3DCompile(kTileDiffCsHlsl,
                            std::strlen(kTileDiffCsHlsl),
                            "GpuDeltaTileDiff.hlsl",
                            nullptr,
                            nullptr,
                            "CSMain",
                            "cs_5_0",
                            D3DCOMPILE_OPTIMIZATION_LEVEL3,
                            0,
                            &csBlob,
                            &errBlob);
    if (FAILED(hr) || !csBlob) {
        Logger::Instance().Warn("GPU_DELTA_CS", L"Failed to compile cs_5_0 tile diff shader.");
        return false;
    }

    hr = m_device->CreateComputeShader(csBlob->GetBufferPointer(), csBlob->GetBufferSize(), nullptr, &m_cs);
    if (FAILED(hr) || !m_cs) return false;

    D3D11_BUFFER_DESC cbDesc{};
    cbDesc.ByteWidth = sizeof(CsParams);
    cbDesc.Usage = D3D11_USAGE_DEFAULT;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CsParams initParams{ m_width, m_height, m_tilesX, m_tilesY };
    D3D11_SUBRESOURCE_DATA cbData{ &initParams, 0, 0 };
    hr = m_device->CreateBuffer(&cbDesc, &cbData, &m_cbParams);
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC prevDesc{};
    prevDesc.Width = m_width;
    prevDesc.Height = m_height;
    prevDesc.MipLevels = 1;
    prevDesc.ArraySize = 1;
    prevDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    prevDesc.SampleDesc.Count = 1;
    prevDesc.Usage = D3D11_USAGE_DEFAULT;
    prevDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = m_device->CreateTexture2D(&prevDesc, nullptr, &m_prevGpuTex);
    if (FAILED(hr)) return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    hr = m_device->CreateShaderResourceView(m_prevGpuTex.Get(), &srvDesc, &m_prevSrv);
    if (FAILED(hr)) return false;

    const UINT tileCount = m_tilesX * m_tilesY;
    D3D11_BUFFER_DESC bufDesc{};
    bufDesc.ByteWidth = tileCount * sizeof(uint32_t);
    bufDesc.Usage = D3D11_USAGE_DEFAULT;
    bufDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    bufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufDesc.StructureByteStride = sizeof(uint32_t);
    hr = m_device->CreateBuffer(&bufDesc, nullptr, &m_flagsBuf);
    if (FAILED(hr)) return false;

    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
    uavDesc.Format = DXGI_FORMAT_UNKNOWN;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uavDesc.Buffer.FirstElement = 0;
    uavDesc.Buffer.NumElements = tileCount;
    hr = m_device->CreateUnorderedAccessView(m_flagsBuf.Get(), &uavDesc, &m_flagsUav);
    if (FAILED(hr)) return false;

    D3D11_BUFFER_DESC stgDesc{};
    stgDesc.ByteWidth = tileCount * sizeof(uint32_t);
    stgDesc.Usage = D3D11_USAGE_STAGING;
    stgDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateBuffer(&stgDesc, nullptr, &m_flagsStaging);
    if (FAILED(hr)) return false;

    D3D11_TEXTURE2D_DESC rbDesc{};
    rbDesc.Width = m_width;
    rbDesc.Height = m_height;
    rbDesc.MipLevels = 1;
    rbDesc.ArraySize = 1;
    rbDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    rbDesc.SampleDesc.Count = 1;
    rbDesc.Usage = D3D11_USAGE_STAGING;
    rbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = m_device->CreateTexture2D(&rbDesc, nullptr, &m_readbackStaging);
    if (FAILED(hr)) return false;

    return true;
}

bool GpuDeltaAnalyzer::EnsureCurSrv(ID3D11Texture2D* curBgraTex, DXGI_FORMAT fmt, ID3D11ShaderResourceView** outSrv) {
    if (!curBgraTex || !outSrv) return false;
    *outSrv = nullptr;

    D3D11_TEXTURE2D_DESC desc{};
    curBgraTex->GetDesc(&desc);
    const DXGI_FORMAT srvFmt = MakeSrvFormat(fmt);

    if ((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0) {
        if (m_curTexPtr != curBgraTex || m_curTexFmt != srvFmt || !m_curSrv) {
            m_curSrv.Reset();
            m_curCopyTex.Reset();
            m_curTexPtr = nullptr;

            D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
            srvDesc.Format = srvFmt;
            srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = 1;
            if (FAILED(m_device->CreateShaderResourceView(curBgraTex, &srvDesc, &m_curSrv))) {
                return false;
            }
            m_curTexPtr = curBgraTex;
            m_curTexFmt = srvFmt;
        }
        *outSrv = m_curSrv.Get();
        return true;
    }

    if (!m_curCopyTex || m_curTexFmt != srvFmt || !m_curSrv) {
        m_curSrv.Reset();
        m_curCopyTex.Reset();
        m_curTexPtr = nullptr;

        D3D11_TEXTURE2D_DESC cdesc{};
        cdesc.Width = m_width;
        cdesc.Height = m_height;
        cdesc.MipLevels = 1;
        cdesc.ArraySize = 1;
        cdesc.Format = srvFmt;
        cdesc.SampleDesc.Count = 1;
        cdesc.Usage = D3D11_USAGE_DEFAULT;
        cdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&cdesc, nullptr, &m_curCopyTex))) {
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = srvFmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(m_device->CreateShaderResourceView(m_curCopyTex.Get(), &srvDesc, &m_curSrv))) {
            m_curCopyTex.Reset();
            return false;
        }
        m_curTexFmt = srvFmt;
    }

    D3D11_BOX srcBox{ 0, 0, 0, m_width, m_height, 1 };
    m_context->CopySubresourceRegion(m_curCopyTex.Get(), 0, 0, 0, 0, curBgraTex, 0, &srcBox);
    *outSrv = m_curSrv.Get();
    return true;
}

void GpuDeltaAnalyzer::SyncPrevTexture(ID3D11Texture2D* curBgraTex) {
    if (!m_initialized || !curBgraTex || !m_prevGpuTex || !m_context) return;
    D3D11_TEXTURE2D_DESC desc{};
    curBgraTex->GetDesc(&desc);
    if (desc.Width < m_width || desc.Height < m_height || !IsSupportedBgraFormat(desc.Format)) return;
    D3D11_BOX box{ 0, 0, 0, m_width, m_height, 1 };
    m_context->CopySubresourceRegion(m_prevGpuTex.Get(), 0, 0, 0, 0, curBgraTex, 0, &box);
    m_hasPrevValid = true;
}

bool GpuDeltaAnalyzer::AnalyzeDirty(ID3D11Texture2D* curBgraTex,
                                    bool forceFullFrame,
                                    std::vector<DeltaRect>& outDirtyRects,
                                    std::vector<uint8_t>& outCpuBgra,
                                    uint32_t& outCpuStride) {
    outDirtyRects.clear();
    if (!m_initialized || !curBgraTex || !m_device || !m_context) return false;

    D3D11_TEXTURE2D_DESC desc{};
    curBgraTex->GetDesc(&desc);
    if (desc.Width < m_width || desc.Height < m_height || !IsSupportedBgraFormat(desc.Format)) {
        return false;
    }

    const uint32_t stride = m_width * 4u;
    outCpuStride = stride;
    if (outCpuBgra.size() != static_cast<size_t>(stride) * m_height) {
        outCpuBgra.resize(static_cast<size_t>(stride) * m_height);
    }

    if (forceFullFrame || !m_hasPrevValid) {
        D3D11_BOX fullBox{ 0, 0, 0, m_width, m_height, 1 };
        m_context->CopySubresourceRegion(m_prevGpuTex.Get(), 0, 0, 0, 0, curBgraTex, 0, &fullBox);
        m_context->CopySubresourceRegion(m_readbackStaging.Get(), 0, 0, 0, 0, curBgraTex, 0, &fullBox);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(m_context->Map(m_readbackStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped)) || !mapped.pData) {
            return false;
        }
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < m_height; ++y) {
            std::memcpy(outCpuBgra.data() + static_cast<size_t>(y) * stride,
                        src + static_cast<size_t>(y) * mapped.RowPitch,
                        stride);
        }
        m_context->Unmap(m_readbackStaging.Get(), 0);
        m_hasPrevValid = true;
        outDirtyRects.push_back(DeltaRect{ 0, 0, m_width, m_height });
        return true;
    }

    ID3D11ShaderResourceView* curSrv = nullptr;
    if (!EnsureCurSrv(curBgraTex, desc.Format, &curSrv) || !curSrv) {
        return false;
    }

    ID3D11ShaderResourceView* srvs[2] = { curSrv, m_prevSrv.Get() };
    ID3D11UnorderedAccessView* uav = m_flagsUav.Get();
    ID3D11Buffer* cb = m_cbParams.Get();

    m_context->CSSetShader(m_cs.Get(), nullptr, 0);
    m_context->CSSetConstantBuffers(0, 1, &cb);
    m_context->CSSetShaderResources(0, 2, srvs);
    m_context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    m_context->Dispatch(m_tilesX, m_tilesY, 1);

    ID3D11ShaderResourceView* nullSrvs[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView* nullUav = nullptr;
    m_context->CSSetShaderResources(0, 2, nullSrvs);
    m_context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    m_context->CSSetShader(nullptr, nullptr, 0);

    m_context->CopyResource(m_flagsStaging.Get(), m_flagsBuf.Get());
    D3D11_MAPPED_SUBRESOURCE mappedFlags{};
    if (FAILED(m_context->Map(m_flagsStaging.Get(), 0, D3D11_MAP_READ, 0, &mappedFlags)) || !mappedFlags.pData) {
        return false;
    }
    std::memcpy(m_tileFlagsCpu.data(), mappedFlags.pData, m_tileFlagsCpu.size() * sizeof(uint32_t));
    m_context->Unmap(m_flagsStaging.Get(), 0);

    // Coalesce contiguous dirty tiles horizontally per tile row, then merge identical X-runs vertically
    std::vector<DeltaRect> rawRects;
    rawRects.reserve(64);
    for (uint32_t ty = 0; ty < m_tilesY; ++ty) {
        uint32_t tx = 0;
        while (tx < m_tilesX) {
            if (m_tileFlagsCpu[static_cast<size_t>(ty) * m_tilesX + tx] == 0) {
                ++tx;
                continue;
            }
            const uint32_t runStart = tx;
            while (tx < m_tilesX && m_tileFlagsCpu[static_cast<size_t>(ty) * m_tilesX + tx] != 0) {
                ++tx;
            }
            const uint32_t px = runStart * kTileSize;
            const uint32_t py = ty * kTileSize;
            const uint32_t pw = std::min(tx * kTileSize, m_width) - px;
            const uint32_t ph = std::min((ty + 1u) * kTileSize, m_height) - py;
            if (!rawRects.empty()) {
                auto& prev = rawRects.back();
                if (prev.x == px && prev.w == pw && prev.y + prev.h == py) {
                    prev.h += ph;
                    continue;
                }
            }
            rawRects.push_back(DeltaRect{ px, py, pw, ph });
        }
    }

    if (rawRects.empty()) {
        // Zero dirty pixels on screen -> 0 bytes readback!
        return true;
    }

    outDirtyRects = std::move(rawRects);

    // Compute union bounding box for minimal GPU->CPU readback
    uint32_t minX = m_width, minY = m_height, maxX = 0, maxY = 0;
    for (const auto& r : outDirtyRects) {
        minX = std::min(minX, r.x);
        minY = std::min(minY, r.y);
        maxX = std::max(maxX, r.x + r.w);
        maxY = std::max(maxY, r.y + r.h);
    }

    D3D11_BOX dirtyBox{ minX, minY, 0, maxX, maxY, 1 };
    m_context->CopySubresourceRegion(m_prevGpuTex.Get(), 0, minX, minY, 0, curBgraTex, 0, &dirtyBox);
    m_context->CopySubresourceRegion(m_readbackStaging.Get(), 0, minX, minY, 0, curBgraTex, 0, &dirtyBox);

    D3D11_MAPPED_SUBRESOURCE mappedPx{};
    if (FAILED(m_context->Map(m_readbackStaging.Get(), 0, D3D11_MAP_READ, 0, &mappedPx)) || !mappedPx.pData) {
        return false;
    }
    const uint8_t* srcBase = static_cast<const uint8_t*>(mappedPx.pData);
    const size_t copyRowBytes = static_cast<size_t>(maxX - minX) * 4u;
    const size_t xByteOffset = static_cast<size_t>(minX) * 4u;
    for (uint32_t y = minY; y < maxY; ++y) {
        std::memcpy(outCpuBgra.data() + static_cast<size_t>(y) * stride + xByteOffset,
                    srcBase + static_cast<size_t>(y) * mappedPx.RowPitch + xByteOffset,
                    copyRowBytes);
    }
    m_context->Unmap(m_readbackStaging.Get(), 0);
    return true;
}

} // namespace s2p
