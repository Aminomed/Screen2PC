#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "../common/CodecDelta.h"
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>

namespace s2p {

// GPU Compute-Shader (cs_5_0) frame differencer for Ultra-Low-Latency Delta encoding.
// Compares curBgraTex against the previously analyzed GPU texture in 64x64 tiles,
// reads back only ~3.6 KB of tile flags (zero full-frame staging readback on idle frames),
// and copies only the bounding region of changed tiles to CPU memory.
class GpuDeltaAnalyzer {
public:
    static constexpr uint32_t kTileSize = 64;

    GpuDeltaAnalyzer() = default;
    ~GpuDeltaAnalyzer() { Shutdown(); }

    GpuDeltaAnalyzer(const GpuDeltaAnalyzer&) = delete;
    GpuDeltaAnalyzer& operator=(const GpuDeltaAnalyzer&) = delete;

    bool Init(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height);
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

    // Synchronizes internal previous GPU texture when caller handles a frame outside AnalyzeDirty
    // (e.g. when DXGI MoveRects path was used).
    void SyncPrevTexture(ID3D11Texture2D* curBgraTex);

    // Analyzes differences between curBgraTex and previous frame on the iGPU.
    // On success returns true:
    //  - If no tiles changed (and !forceFullFrame), outDirtyRects is empty and zero pixel readback occurs.
    //  - If tiles changed, outDirtyRects holds coalesced changed rectangles and outCpuBgra (sized width*height*4)
    //    is updated in-place within the dirty bounding box.
    bool AnalyzeDirty(ID3D11Texture2D* curBgraTex,
                      bool forceFullFrame,
                      std::vector<DeltaRect>& outDirtyRects,
                      std::vector<uint8_t>& outCpuBgra,
                      uint32_t& outCpuStride);

private:
    bool CreateResources();
    bool EnsureCurSrv(ID3D11Texture2D* curBgraTex, DXGI_FORMAT fmt, ID3D11ShaderResourceView** outSrv);

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_tilesX = 0;
    uint32_t m_tilesY = 0;
    bool m_initialized = false;
    bool m_hasPrevValid = false;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<ID3D11ComputeShader> m_cs;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbParams;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_prevGpuTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_prevSrv;

    ID3D11Texture2D* m_curTexPtr = nullptr;
    DXGI_FORMAT m_curTexFmt = DXGI_FORMAT_UNKNOWN;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_curCopyTex;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_curSrv;

    Microsoft::WRL::ComPtr<ID3D11Buffer> m_flagsBuf;
    Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> m_flagsUav;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_flagsStaging;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_readbackStaging;
    std::vector<uint32_t> m_tileFlagsCpu;
};

} // namespace s2p
