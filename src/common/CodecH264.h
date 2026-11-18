#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>
#include <vector>
#include <string>

struct IMFTransform;
struct IMFMediaEventGenerator;
struct IMFDXGIDeviceManager;
struct ICodecAPI;

namespace s2p {

class H264Encoder {
public:
    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // d3dDevice may be nullptr for pure CPU-buffer testing.
    bool Init(ID3D11Device* d3dDevice,
              ID3D11DeviceContext* d3dCtx,
              uint32_t width,
              uint32_t height,
              uint32_t fps,
              uint32_t bitrateMbps,
              bool allowHardware = true);

    void Shutdown();
    bool IsInitialized() const { return m_initialized; }
    bool IsHardware() const { return m_isHardware; }
    bool IsAmfActive() const { return m_isAmf; }
    bool WasLastCallWarmup() const { return m_lastWasWarmup; }
    std::wstring FriendlyName() const { return m_friendlyName; }

    void RequestKeyframe() { m_forceKeyframe = true; }
    bool SetBitrate(uint32_t bitrateMbps);
    uint32_t BitrateMbps() const { return m_bitrateBps / 1000000u; }

    // Encodes a BGRA D3D11 texture (preferred path).
    bool EncodeTexture(ID3D11Texture2D* bgraTexture,
                       uint64_t timestampUs,
                       std::vector<uint8_t>& outAnnexB,
                       bool& outIsKeyframe);

    // Encodes a CPU BGRA buffer (testing / fallback path).
    bool EncodeBgra(const uint8_t* bgraData,
                    uint32_t strideBytes,
                    uint64_t timestampUs,
                    std::vector<uint8_t>& outAnnexB,
                    bool& outIsKeyframe);

private:
    static constexpr size_t kNv12RingSize = 4;

    bool InitMft(bool tryHardware);
    bool EnsureVideoProcessor();
    bool ConvertBgraToNv12Gpu(ID3D11Texture2D* bgraTex, size_t slotIdx);
    bool EncodeNv12TextureOrBuffer(ID3D11Texture2D* nv12Tex,
                                   const uint8_t* nv12Cpu,
                                   uint32_t nv12Stride,
                                   uint64_t timestampUs,
                                   std::vector<uint8_t>& outAnnexB,
                                   bool& outIsKeyframe);

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 60;
    uint32_t m_bitrateBps = 25000000;
    bool m_initialized = false;
    bool m_mfStarted = false;
    bool m_isHardware = false;
    bool m_isAmf = false;
    bool m_isAsync = false;
    bool m_asyncNeedInput = false;
    bool m_lastWasWarmup = false;
    uint32_t m_warmupFramesPending = 0;
    bool m_forceKeyframe = true;
    bool m_mftProvidesOutputSamples = false;
    DWORD m_inputStreamId = 0;
    DWORD m_outputStreamId = 0;
    DWORD m_outputBufferSize = 0;
    uint64_t m_frameCounter = 0;
    std::wstring m_friendlyName;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxgiMgr;
    Microsoft::WRL::ComPtr<IMFTransform> m_mft;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> m_eventGen;
    Microsoft::WRL::ComPtr<ICodecAPI> m_codecApi;

    // GPU color conversion BGRA -> NV12 (4-slot ring prevents overwriting async MFT in-flight frames)
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDev;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoCtx;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_vp;
    ID3D11Texture2D* m_vpInTex = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> m_vpInView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_nv12Ring[kNv12RingSize];
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_vpOutViewRing[kNv12RingSize];
    size_t m_nv12RingIdx = 0;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingNv12;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_fallbackStagingBgra;

    std::vector<uint8_t> m_cpuNv12;
};

class H264Decoder {
public:
    H264Decoder();
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    bool Init(ID3D11Device* d3dDevice,
              ID3D11DeviceContext* d3dCtx,
              uint32_t width,
              uint32_t height,
              bool allowHardware = true,
              uint32_t fps = 60);
    void Shutdown();
    bool IsInitialized() const { return m_initialized; }
    bool IsHardwareDxva() const { return m_dxvaEnabled; }
    bool WasLastCallNeedMoreInput() const { return m_lastNeedMoreInput; }

    // Decodes Annex-B H.264 frame into outBgraTexture (if provided and GPU path active)
    // and/or outBgraCpu (if provided), OR directly into outNv12Texture (zero-VP direct NV12 shader path!).
    bool Decode(const uint8_t* annexBData,
                size_t dataSize,
                uint64_t timestampUs,
                ID3D11Texture2D* outBgraTexture,
                std::vector<uint8_t>* outBgraCpu,
                uint32_t* outWidth = nullptr,
                uint32_t* outHeight = nullptr,
                ID3D11Texture2D* outNv12Texture = nullptr,
                bool* outDecodedToNv12 = nullptr);

private:
    struct CachedInputView {
        ID3D11Texture2D* tex = nullptr;
        uint32_t subIdx = 0;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> view;
    };
    static constexpr size_t kMaxCachedInViews = 24;

    bool ConfigureOutputType();
    bool EnsureVideoProcessor(uint32_t width, uint32_t height);
    bool ConvertNv12TexToBgraTex(ID3D11Texture2D* nv12Tex, uint32_t subresourceIdx, ID3D11Texture2D* outBgraTex);
    bool CopyNv12TexToTarget(ID3D11Texture2D* srcNv12Tex, uint32_t subresourceIdx, ID3D11Texture2D* dstNv12Tex);
    bool DrainOutputs(ID3D11Texture2D* outBgraTexture,
                      std::vector<uint8_t>* outBgraCpu,
                      uint32_t* outW,
                      uint32_t* outH,
                      ID3D11Texture2D* outNv12Texture,
                      bool* outDecodedToNv12);

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_fps = 60;
    uint32_t m_codedWidth = 0;
    uint32_t m_codedHeight = 0;
    uint32_t m_decodedStride = 0;
    bool m_initialized = false;
    bool m_mfStarted = false;
    bool m_dxvaEnabled = false;
    bool m_lastNeedMoreInput = false;
    bool m_mftProvidesOutputSamples = false;
    DWORD m_outputBufferSize = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_dxgiMgr;
    Microsoft::WRL::ComPtr<IMFTransform> m_mft;

    Microsoft::WRL::ComPtr<ID3D11VideoDevice> m_videoDev;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> m_videoCtx;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> m_vpEnum;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> m_vp;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_uploadNv12;
    ID3D11Texture2D* m_vpOutTex = nullptr;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> m_vpOutView;
    CachedInputView m_inViewCache[kMaxCachedInViews];
    size_t m_inViewCacheCount = 0;
    uint32_t m_vpW = 0;
    uint32_t m_vpH = 0;
    std::vector<uint8_t> m_scratchNv12;
    std::vector<uint8_t> m_scratchBgra;
};

void CpuBgraToNv12(const uint8_t* bgra, uint32_t bgraStride, uint32_t width, uint32_t height, uint8_t* nv12Out);
void CpuNv12ToBgra(const uint8_t* yPlane, uint32_t yStride, const uint8_t* uvPlane, uint32_t uvStride, uint32_t width, uint32_t height, uint8_t* bgraOut);

// Rewrites H.264 Annex-B SPS NAL units to enforce Constrained Baseline / VUI bitstream_restriction
// with max_num_reorder_frames = 0 and max_dec_frame_buffering = 1 so hardware & software decoders
// output frames on the very first ProcessOutput call with 0-frame DPB reordering delay.
void EnsureLowLatencyH264Stream(std::vector<uint8_t>& annexB);

} // namespace s2p
