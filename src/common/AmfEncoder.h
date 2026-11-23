#pragma once
#include "Protocol.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace s2p::amf_abi {
struct AMFFactory;
struct AMFContext;
struct AMFComponent;
struct AMFSurface;
} // namespace s2p::amf_abi

namespace s2p {

class AmfEncoder {
public:
    AmfEncoder() = default;
    ~AmfEncoder();

    AmfEncoder(const AmfEncoder&) = delete;
    AmfEncoder& operator=(const AmfEncoder&) = delete;

    // Attempts to load amfrt64.dll and configure ultra-low-latency VCN/VCE hardware encoding
    // for CodecType::H264 or CodecType::HEVC. Returns false cleanly on non-AMD systems.
    bool Initialize(ID3D11Device* device,
                    ID3D11DeviceContext* context,
                    uint32_t width,
                    uint32_t height,
                    uint32_t fps,
                    uint64_t bitrateBps,
                    CodecType codec);

    bool EncodeTexture(ID3D11Texture2D* bgraTex,
                       bool forceKeyframe,
                       std::vector<uint8_t>& outAnnexB,
                       bool& outIsKeyframe);

    void SetBitrate(uint64_t bitrateBps);
    void Shutdown();

    bool IsInitialized() const { return m_initialized; }
    CodecType Codec() const { return m_codec; }
    std::wstring BackendName() const;

private:
    static constexpr size_t kRingSize = 3;

    bool m_initialized = false;
    CodecType m_codec = CodecType::H264;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 60;
    uint64_t m_bitrateBps = 20000000;
    uint64_t m_frameCounter = 0;

    HMODULE m_dll = nullptr;
    amf_abi::AMFFactory*   m_factory = nullptr;
    amf_abi::AMFContext*   m_amfCtx = nullptr;
    amf_abi::AMFComponent* m_encoder = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device>                  m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>           m_context;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice>             m_videoDev;
    Microsoft::WRL::ComPtr<ID3D11VideoContext>            m_videoCtx;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor>          m_vp;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>               m_vpNv12Tex;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_vpOutView;

    std::array<amf_abi::AMFSurface*, kRingSize>           m_amfSurfaces{};
    std::array<ID3D11Texture2D*, kRingSize>               m_amfTextures{};
    std::array<Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView>, kRingSize> m_amfVpOutViews{};
    bool m_directVpWrite = false;
    size_t m_ringIdx = 0;

    ID3D11Texture2D* m_cachedInputTex = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_cachedInputView;

    std::vector<uint8_t> m_extradata;
};

} // namespace s2p
