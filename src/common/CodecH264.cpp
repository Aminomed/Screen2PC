#include "CodecH264.h"
#include "Logger.h"
#include "Protocol.h"
#include <d3d11_1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "strmiids.lib")

namespace s2p {

namespace {

inline uint8_t ClampByte(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

void SetBt709MediaTypeAttributes(IMFMediaType* mt) {
    if (!mt) return;
    mt->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    mt->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    mt->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    mt->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
}

void ClearColorMediaTypeAttributes(IMFMediaType* mt) {
    if (!mt) return;
    mt->DeleteItem(MF_MT_VIDEO_PRIMARIES);
    mt->DeleteItem(MF_MT_TRANSFER_FUNCTION);
    mt->DeleteItem(MF_MT_YUV_MATRIX);
    mt->DeleteItem(MF_MT_VIDEO_NOMINAL_RANGE);
}

void ConfigureVideoProcessorBt709(ID3D11VideoContext* videoCtx, ID3D11VideoProcessor* vp, bool bgraToNv12) {
    if (!videoCtx || !vp) return;
    videoCtx->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);

    Microsoft::WRL::ComPtr<ID3D11VideoContext1> vc1;
    if (SUCCEEDED(videoCtx->QueryInterface(IID_PPV_ARGS(&vc1))) && vc1) {
        if (bgraToNv12) {
            vc1->VideoProcessorSetStreamColorSpace1(vp, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
            vc1->VideoProcessorSetOutputColorSpace1(vp, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
        } else {
            vc1->VideoProcessorSetStreamColorSpace1(vp, 0, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
            vc1->VideoProcessorSetOutputColorSpace1(vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        }
        return;
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE rgbFull{};
    rgbFull.Usage = 0;
    rgbFull.RGB_Range = 0;
    rgbFull.YCbCr_Matrix = 1; // BT.709
    rgbFull.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE yuvStudio{};
    yuvStudio.Usage = 0;
    yuvStudio.RGB_Range = 1;
    yuvStudio.YCbCr_Matrix = 1; // BT.709
    yuvStudio.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;

    if (bgraToNv12) {
        videoCtx->VideoProcessorSetStreamColorSpace(vp, 0, &rgbFull);
        videoCtx->VideoProcessorSetOutputColorSpace(vp, &yuvStudio);
    } else {
        videoCtx->VideoProcessorSetStreamColorSpace(vp, 0, &yuvStudio);
        videoCtx->VideoProcessorSetOutputColorSpace(vp, &rgbFull);
    }
}

bool ContainsIdrNalu(const uint8_t* data, size_t size) {
    if (!data || size < 5) return false;
    for (size_t i = 0; i + 4 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0) {
            size_t hdrIdx = 0;
            if (data[i + 2] == 1) {
                hdrIdx = i + 3;
            } else if (i + 4 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                hdrIdx = i + 4;
            }
            if (hdrIdx > 0 && hdrIdx < size) {
                uint8_t nalType = data[hdrIdx] & 0x1F;
                if (nalType == 5) return true; // IDR slice
            }
        }
    }
    return false;
}

HRESULT SetOutputSampleBuffer(IMFTransform* mft, DWORD streamId, DWORD bufSize, MFT_OUTPUT_DATA_BUFFER& outBuf) {
    Microsoft::WRL::ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> buf;
    hr = MFCreateMemoryBuffer(std::max<DWORD>(bufSize, 1024 * 1024), &buf);
    if (FAILED(hr)) return hr;
    hr = sample->AddBuffer(buf.Get());
    if (FAILED(hr)) return hr;
    outBuf.dwStreamID = streamId;
    outBuf.pSample = sample.Detach();
    outBuf.dwStatus = 0;
    outBuf.pEvents = nullptr;
    return S_OK;
}

HRESULT ConfigureEncoderCodecApi(ICodecAPI* api, uint32_t bitrateBps, uint32_t fps, bool isInit) {
    if (!api) return E_POINTER;

    VARIANT vLow{};
    vLow.vt = VT_BOOL;
    vLow.boolVal = VARIANT_TRUE;
    api->SetValue(&CODECAPI_AVLowLatencyMode, &vLow);

    if (isInit) {
        constexpr UINT32 kRateControlLowDelayVbr = 4; // eAVEncCommonRateControlMode_LowDelayVBR
        VARIANT vRc{};
        vRc.vt = VT_UI4;
        vRc.ulVal = kRateControlLowDelayVbr;
        if (FAILED(api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &vRc))) {
            vRc.ulVal = eAVEncCommonRateControlMode_PeakConstrainedVBR;
            if (FAILED(api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &vRc))) {
                vRc.ulVal = eAVEncCommonRateControlMode_CBR;
                api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &vRc);
            }
        }
    }

    VARIANT vMean{};
    vMean.vt = VT_UI4;
    vMean.ulVal = bitrateBps;
    HRESULT hrMean = api->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &vMean);

    uint64_t maxBitrate = (static_cast<uint64_t>(bitrateBps) * 12ULL) / 10ULL;
    VARIANT vMax{};
    vMax.vt = VT_UI4;
    vMax.ulVal = static_cast<ULONG>(std::min<uint64_t>(0xFFFFFFFFULL, std::max<uint64_t>(bitrateBps, maxBitrate)));
    api->SetValue(&CODECAPI_AVEncCommonMaxBitRate, &vMax);

    // VBV buffer sized for ~1 frame at 15 FPS (~4 frames at 60 FPS) in bytes to prevent bufferbloat
    uint32_t vbvBytes = std::max<uint32_t>(65536u, (bitrateBps / 8u) / 15u);
    VARIANT vBuf{};
    vBuf.vt = VT_UI4;
    vBuf.ulVal = vbvBytes;
    api->SetValue(&CODECAPI_AVEncCommonBufferSize, &vBuf);

    if (isInit) {
        VARIANT vQvs{};
        vQvs.vt = VT_UI4;
        vQvs.ulVal = 15; // Fast low-latency preset
        api->SetValue(&CODECAPI_AVEncCommonQualityVsSpeed, &vQvs);

        VARIANT vB{};
        vB.vt = VT_UI4;
        vB.ulVal = 0;
        api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &vB);

        VARIANT vGop{};
        vGop.vt = VT_UI4;
        vGop.ulVal = std::clamp<uint32_t>(fps * 10u, 30u, 600u);
        api->SetValue(&CODECAPI_AVEncMPVGOPSize, &vGop);
    }

    return hrMean;
}

} // namespace

void CpuBgraToNv12(const uint8_t* bgra, uint32_t bgraStride, uint32_t width, uint32_t height, uint8_t* nv12Out) {
    // BT.709 Studio Range (Y: 16..235, UV: 16..240) matching D3D11 VideoProcessor & H.264 VUI
    uint8_t* yPlane = nv12Out;
    uint8_t* uvPlane = nv12Out + static_cast<size_t>(width) * height;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * bgraStride;
        uint8_t* yRow = yPlane + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];
            int yVal = ((748 * r + 2516 * g + 254 * b + 2048) >> 12) + 16;
            yRow[x] = ClampByte(yVal);
        }
    }

    for (uint32_t y = 0; y + 1 < height; y += 2) {
        const uint8_t* r0 = bgra + static_cast<size_t>(y) * bgraStride;
        const uint8_t* r1 = bgra + static_cast<size_t>(y + 1) * bgraStride;
        uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * width;
        for (uint32_t x = 0; x + 1 < width; x += 2) {
            int b = (r0[x * 4 + 0] + r0[(x + 1) * 4 + 0] + r1[x * 4 + 0] + r1[(x + 1) * 4 + 0]) >> 2;
            int g = (r0[x * 4 + 1] + r0[(x + 1) * 4 + 1] + r1[x * 4 + 1] + r1[(x + 1) * 4 + 1]) >> 2;
            int r = (r0[x * 4 + 2] + r0[(x + 1) * 4 + 2] + r1[x * 4 + 2] + r1[(x + 1) * 4 + 2]) >> 2;
            int uVal = ((-412 * r - 1387 * g + 1799 * b + 2048) >> 12) + 128;
            int vVal = ((1799 * r - 1634 * g - 165 * b + 2048) >> 12) + 128;
            uvRow[x + 0] = ClampByte(uVal);
            uvRow[x + 1] = ClampByte(vVal);
        }
    }
}

void CpuNv12ToBgra(const uint8_t* yPlane, uint32_t yStride, const uint8_t* uvPlane, uint32_t uvStride, uint32_t width, uint32_t height, uint8_t* bgraOut) {
    // BT.709 Studio Range -> Full RGB
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* yRow = yPlane + static_cast<size_t>(y) * yStride;
        const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * uvStride;
        uint8_t* dstRow = bgraOut + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            int c = static_cast<int>(yRow[x]) - 16;
            uint32_t uvX = (x & ~1u);
            int d = static_cast<int>(uvRow[uvX + 0]) - 128;
            int e = static_cast<int>(uvRow[uvX + 1]) - 128;
            int r = (4769 * c + 7343 * e + 2048) >> 12;
            int g = (4769 * c - 873 * d - 2183 * e + 2048) >> 12;
            int b = (4769 * c + 8652 * d + 2048) >> 12;
            dstRow[x * 4 + 0] = ClampByte(b);
            dstRow[x * 4 + 1] = ClampByte(g);
            dstRow[x * 4 + 2] = ClampByte(r);
            dstRow[x * 4 + 3] = 255;
        }
    }
}

// ============================================================================
// H264Encoder
// ============================================================================

H264Encoder::H264Encoder() = default;

H264Encoder::~H264Encoder() {
    Shutdown();
}

void H264Encoder::Shutdown() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    m_codecApi.Reset();
    m_eventGen.Reset();
    m_mft.Reset();
    m_vpInView.Reset();
    m_vpInTex = nullptr;
    for (size_t i = 0; i < kNv12RingSize; ++i) {
        m_vpOutViewRing[i].Reset();
        m_nv12Ring[i].Reset();
    }
    m_nv12RingIdx = 0;
    m_vp.Reset();
    m_vpEnum.Reset();
    m_videoCtx.Reset();
    m_videoDev.Reset();
    m_stagingNv12.Reset();
    m_fallbackStagingBgra.Reset();
    m_dxgiMgr.Reset();
    m_context.Reset();
    m_device.Reset();
    m_cpuNv12.clear();

    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
    m_width = 0;
    m_height = 0;
    m_inputStreamId = 0;
    m_outputStreamId = 0;
    m_outputBufferSize = 0;
    m_mftProvidesOutputSamples = false;
    m_friendlyName.clear();
    m_initialized = false;
    m_isHardware = false;
    m_isAsync = false;
    m_asyncNeedInput = false;
}

bool H264Encoder::Init(ID3D11Device* d3dDevice,
                       ID3D11DeviceContext* d3dCtx,
                       uint32_t width,
                       uint32_t height,
                       uint32_t fps,
                       uint32_t bitrateMbps,
                       bool allowHardware) {
    Shutdown();

    // H.264 requires even dimensions
    m_width = width & ~1u;
    m_height = height & ~1u;
    if (m_width < 2 || m_height < 2 ||
        m_width > 16384 || m_height > 16384 ||
        static_cast<uint64_t>(m_width) * m_height > 64ULL * 1024ULL * 1024ULL) {
        m_width = 0;
        m_height = 0;
        return false;
    }
    m_fps = std::max<uint32_t>(1, fps);
    m_bitrateBps = std::clamp<uint32_t>(bitrateMbps, 2u, 300u) * 1000000u;
    m_device = d3dDevice;
    m_context = d3dCtx;
    m_forceKeyframe = true;
    m_frameCounter = 0;
    m_nv12RingIdx = 0;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        Logger::Instance().Error("H264_MF_INIT", L"MFStartup failed: " + FormatHr(hr));
        Shutdown();
        return false;
    }
    m_mfStarted = true;

    if (m_device) {
        Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
        if (SUCCEEDED(m_device.As(&mt)) && mt) {
            mt->SetMultithreadProtected(TRUE);
        }
        UINT resetToken = 0;
        if (SUCCEEDED(MFCreateDXGIDeviceManager(&resetToken, &m_dxgiMgr))) {
            m_dxgiMgr->ResetDevice(m_device.Get(), resetToken);
        }
    }

    // Try hardware encoder first if allowed and we have a D3D11 device, otherwise software fallback
    bool ok = false;
    if (allowHardware && m_device) {
        ok = InitMft(true);
    }
    if (!ok) {
        if (allowHardware && m_device) {
            Logger::Instance().Warn("H264_HW_FALLBACK", L"Hardware H.264 MFT unavailable, falling back to software MFT.");
        }
        ok = InitMft(false);
    }

    if (!ok) {
        Shutdown();
        return false;
    }

    m_cpuNv12.resize(static_cast<size_t>(m_width) * m_height * 3 / 2);
    m_initialized = true;
    Logger::Instance().Info("H264_ENC_OK", L"H.264 Encoder ready (" + m_friendlyName + L", hw=" + (m_isHardware ? L"1" : L"0") +
                             L", " + std::to_wstring(m_fps) + L"fps, " + std::to_wstring(m_bitrateBps / 1000000u) + L"Mbit/s)");
    return true;
}

bool H264Encoder::SetBitrate(uint32_t bitrateMbps) {
    uint32_t clamped = std::clamp<uint32_t>(bitrateMbps, 2u, 300u);
    uint32_t newBps = clamped * 1000000u;
    if (!m_initialized || !m_mft) {
        return false;
    }
    if (newBps == m_bitrateBps) {
        return true;
    }
    m_bitrateBps = newBps;
    if (!m_codecApi) {
        return false;
    }
    HRESULT hr = ConfigureEncoderCodecApi(m_codecApi.Get(), m_bitrateBps, m_fps, false);
    return SUCCEEDED(hr);
}

bool H264Encoder::InitMft(bool tryHardware) {
    m_mft.Reset();
    m_eventGen.Reset();
    m_codecApi.Reset();
    m_isHardware = false;
    m_isAsync = false;
    m_asyncNeedInput = false;
    m_mftProvidesOutputSamples = false;
    m_outputBufferSize = 0;
    m_inputStreamId = 0;
    m_outputStreamId = 0;
    m_friendlyName.clear();

    MFT_REGISTER_TYPE_INFO outInfo{ MFMediaType_Video, MFVideoFormat_H264 };
    UINT32 flags = tryHardware
        ? (MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER)
        : (MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER);

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &outInfo, &activates, &count);
    std::vector<Microsoft::WRL::ComPtr<IMFTransform>> candidates;
    if (SUCCEEDED(hr) && activates) {
        for (UINT32 idx = 0; idx < count; ++idx) {
            Microsoft::WRL::ComPtr<IMFTransform> candidate;
            if (SUCCEEDED(activates[idx]->ActivateObject(IID_PPV_ARGS(&candidate)))) {
                candidates.push_back(candidate);
            }
            activates[idx]->Release();
        }
        CoTaskMemFree(activates);
    }
    if (!tryHardware && candidates.empty()) {
        Microsoft::WRL::ComPtr<IMFTransform> msEnc;
        if (SUCCEEDED(CoCreateInstance(CLSID_CMSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&msEnc)))) {
            candidates.push_back(msEnc);
        }
    }
    if (candidates.empty()) {
        return false;
    }

    bool configured = false;
    for (auto& candidate : candidates) {
        if (!candidate) continue;

        m_eventGen.Reset();
        m_codecApi.Reset();
        m_isAsync = false;
        m_asyncNeedInput = false;
        m_mftProvidesOutputSamples = false;
        m_outputBufferSize = 0;
        m_inputStreamId = 0;
        m_outputStreamId = 0;

        std::wstring candidateName = tryHardware ? L"Hardware H.264 MFT" : L"Microsoft H.264 MFT";
        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(candidate->GetAttributes(&attrs))) {
            UINT32 nameLen = 0;
            if (SUCCEEDED(attrs->GetStringLength(MFT_FRIENDLY_NAME_Attribute, &nameLen)) && nameLen > 0) {
                std::wstring fname(nameLen + 1, L'\0');
                if (SUCCEEDED(attrs->GetString(MFT_FRIENDLY_NAME_Attribute, fname.data(), static_cast<UINT32>(fname.size()), &nameLen))) {
                    fname.resize(nameLen);
                    candidateName = fname;
                }
            }
            UINT32 asyncFlag = 0;
            if (SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &asyncFlag)) && asyncFlag != 0) {
                attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
                m_isAsync = true;
            }
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        }

        if (m_isAsync) {
            if (FAILED(candidate.As(&m_eventGen))) {
                continue;
            }
        }

        DWORD inId = 0, outId = 0;
        hr = candidate->GetStreamIDs(1, &inId, 1, &outId);
        if (hr == E_NOTIMPL) {
            inId = 0;
            outId = 0;
            hr = S_OK;
        }
        if (FAILED(hr)) continue;
        m_inputStreamId = inId;
        m_outputStreamId = outId;

        if (tryHardware && m_dxgiMgr) {
            // Optional for some MFTs; ignore E_NOTIMPL
            candidate->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_dxgiMgr.Get()));
        }

        candidate.As(&m_codecApi);
        if (m_codecApi) {
            // Apply LowDelayVBR + rate control BEFORE SetOutputType (required by several GPU MFTs)
            ConfigureEncoderCodecApi(m_codecApi.Get(), m_bitrateBps, m_fps, true);
        }

        // Configure output type (H.264)
        Microsoft::WRL::ComPtr<IMFMediaType> outType;
        if (FAILED(MFCreateMediaType(&outType))) continue;
        outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        outType->SetUINT32(MF_MT_AVG_BITRATE, m_bitrateBps);
        MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, m_fps, 1);
        MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
        SetBt709MediaTypeAttributes(outType.Get());

        hr = candidate->SetOutputType(m_outputStreamId, outType.Get(), 0);
        if (FAILED(hr)) {
            ClearColorMediaTypeAttributes(outType.Get());
            hr = candidate->SetOutputType(m_outputStreamId, outType.Get(), 0);
        }
        if (FAILED(hr)) continue;

        // Configure input type (NV12)
        bool inputOk = false;
        for (DWORD t = 0; ; ++t) {
            Microsoft::WRL::ComPtr<IMFMediaType> availIn;
            if (FAILED(candidate->GetInputAvailableType(m_inputStreamId, t, &availIn))) break;
            GUID sub{};
            availIn->GetGUID(MF_MT_SUBTYPE, &sub);
            if (sub == MFVideoFormat_NV12 || t == 0) {
                availIn->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                availIn->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                MFSetAttributeSize(availIn.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
                MFSetAttributeRatio(availIn.Get(), MF_MT_FRAME_RATE, m_fps, 1);
                MFSetAttributeRatio(availIn.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                availIn->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                SetBt709MediaTypeAttributes(availIn.Get());
                hr = candidate->SetInputType(m_inputStreamId, availIn.Get(), 0);
                if (FAILED(hr)) {
                    ClearColorMediaTypeAttributes(availIn.Get());
                    hr = candidate->SetInputType(m_inputStreamId, availIn.Get(), 0);
                }
                if (SUCCEEDED(hr)) {
                    inputOk = true;
                    break;
                }
            }
        }
        if (!inputOk) {
            Microsoft::WRL::ComPtr<IMFMediaType> inType;
            if (SUCCEEDED(MFCreateMediaType(&inType))) {
                inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
                MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
                MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, m_fps, 1);
                MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                SetBt709MediaTypeAttributes(inType.Get());
                hr = candidate->SetInputType(m_inputStreamId, inType.Get(), 0);
                if (FAILED(hr)) {
                    ClearColorMediaTypeAttributes(inType.Get());
                    hr = candidate->SetInputType(m_inputStreamId, inType.Get(), 0);
                }
                inputOk = SUCCEEDED(hr);
            }
        }
        if (!inputOk) continue;

        // Re-apply after SetInputType as well so both pre- and post-type MFT drivers lock low-delay VBR/CBR
        if (m_codecApi) {
            ConfigureEncoderCodecApi(m_codecApi.Get(), m_bitrateBps, m_fps, true);
        }

        MFT_OUTPUT_STREAM_INFO osi{};
        if (SUCCEEDED(candidate->GetOutputStreamInfo(m_outputStreamId, &osi))) {
            m_mftProvidesOutputSamples = (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
            m_outputBufferSize = osi.cbSize;
        }

        candidate->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        candidate->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

        m_mft = candidate;
        m_friendlyName = candidateName;
        m_isHardware = tryHardware;
        configured = true;
        break;
    }

    if (!configured) {
        m_eventGen.Reset();
        m_codecApi.Reset();
        m_isAsync = false;
        m_asyncNeedInput = false;
        m_mftProvidesOutputSamples = false;
        m_outputBufferSize = 0;
        m_inputStreamId = 0;
        m_outputStreamId = 0;
        m_friendlyName.clear();
    }

    return configured;
}

bool H264Encoder::EnsureVideoProcessor() {
    if (!m_device || !m_context) return false;
    bool ringReady = (m_vp != nullptr);
    for (size_t i = 0; i < kNv12RingSize && ringReady; ++i) {
        if (!m_nv12Ring[i] || !m_vpOutViewRing[i]) ringReady = false;
    }
    if (ringReady) return true;

    if (FAILED(m_device.As(&m_videoDev)) || FAILED(m_context.As(&m_videoCtx))) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cdesc{};
    cdesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cdesc.InputWidth = m_width;
    cdesc.InputHeight = m_height;
    cdesc.OutputWidth = m_width;
    cdesc.OutputHeight = m_height;
    cdesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(m_videoDev->CreateVideoProcessorEnumerator(&cdesc, &m_vpEnum))) return false;
    if (FAILED(m_videoDev->CreateVideoProcessor(m_vpEnum.Get(), 0, &m_vp))) return false;
    ConfigureVideoProcessorBt709(m_videoCtx.Get(), m_vp.Get(), true);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_width;
    td.Height = m_height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;

    for (size_t i = 0; i < kNv12RingSize; ++i) {
        m_vpOutViewRing[i].Reset();
        m_nv12Ring[i].Reset();
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_nv12Ring[i]))) return false;
        if (FAILED(m_videoDev->CreateVideoProcessorOutputView(m_nv12Ring[i].Get(), m_vpEnum.Get(), &ovd, &m_vpOutViewRing[i]))) {
            return false;
        }
    }

    // Staging texture for software MFT readback if needed
    D3D11_TEXTURE2D_DESC st = td;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    m_device->CreateTexture2D(&st, nullptr, &m_stagingNv12);

    return true;
}

bool H264Encoder::ConvertBgraToNv12Gpu(ID3D11Texture2D* bgraTex, size_t slotIdx) {
    if (slotIdx >= kNv12RingSize || !EnsureVideoProcessor()) return false;

    if (!m_vpInView || m_vpInTex != bgraTex) {
        m_vpInView.Reset();
        m_vpInTex = bgraTex;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
        ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        ivd.Texture2D.MipSlice = 0;
        HRESULT hr = m_videoDev->CreateVideoProcessorInputView(bgraTex, m_vpEnum.Get(), &ivd, &m_vpInView);
        if (FAILED(hr)) {
            m_vpInTex = nullptr;
            return false;
        }
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = m_vpInView.Get();

    HRESULT hr = m_videoCtx->VideoProcessorBlt(m_vp.Get(), m_vpOutViewRing[slotIdx].Get(), 0, 1, &stream);
    return SUCCEEDED(hr);
}

bool H264Encoder::EncodeTexture(ID3D11Texture2D* bgraTexture,
                                uint64_t timestampUs,
                                std::vector<uint8_t>& outAnnexB,
                                bool& outIsKeyframe) {
    if (!m_initialized || !bgraTexture || !m_device || !m_context) return false;

    D3D11_TEXTURE2D_DESC desc{};
    bgraTexture->GetDesc(&desc);
    if (desc.Width < m_width || desc.Height < m_height) {
        return false;
    }

    const size_t slot = m_nv12RingIdx;
    m_nv12RingIdx = (m_nv12RingIdx + 1) % kNv12RingSize;

    if (ConvertBgraToNv12Gpu(bgraTexture, slot)) {
        if (m_isHardware) {
            return EncodeNv12TextureOrBuffer(m_nv12Ring[slot].Get(), nullptr, 0, timestampUs, outAnnexB, outIsKeyframe);
        }
        // Software MFT: read back NV12 staging
        if (m_stagingNv12) {
            m_context->CopyResource(m_stagingNv12.Get(), m_nv12Ring[slot].Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(m_context->Map(m_stagingNv12.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
                if (!mapped.pData || mapped.RowPitch < m_width) {
                    m_context->Unmap(m_stagingNv12.Get(), 0);
                    return false;
                }
                for (uint32_t y = 0; y < m_height; ++y) {
                    std::memcpy(m_cpuNv12.data() + static_cast<size_t>(y) * m_width,
                                static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                                m_width);
                }
                const uint8_t* srcUv = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(m_height) * mapped.RowPitch;
                uint8_t* dstUv = m_cpuNv12.data() + static_cast<size_t>(m_width) * m_height;
                for (uint32_t y = 0; y < m_height / 2; ++y) {
                    std::memcpy(dstUv + static_cast<size_t>(y) * m_width,
                                srcUv + static_cast<size_t>(y) * mapped.RowPitch,
                                m_width);
                }
                m_context->Unmap(m_stagingNv12.Get(), 0);
                return EncodeNv12TextureOrBuffer(nullptr, m_cpuNv12.data(), m_width, timestampUs, outAnnexB, outIsKeyframe);
            }
        }
    }

    // CPU fallback if VideoProcessor unavailable
    bool needCreate = !m_fallbackStagingBgra;
    if (!needCreate) {
        D3D11_TEXTURE2D_DESC cur{};
        m_fallbackStagingBgra->GetDesc(&cur);
        if (cur.Width != desc.Width || cur.Height != desc.Height || cur.Format != desc.Format) {
            m_fallbackStagingBgra.Reset();
            needCreate = true;
        }
    }
    if (needCreate) {
        D3D11_TEXTURE2D_DESC st = desc;
        st.Usage = D3D11_USAGE_STAGING;
        st.BindFlags = 0;
        st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        st.MiscFlags = 0;
        if (FAILED(m_device->CreateTexture2D(&st, nullptr, &m_fallbackStagingBgra))) return false;
    }
    m_context->CopyResource(m_fallbackStagingBgra.Get(), bgraTexture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(m_context->Map(m_fallbackStagingBgra.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    if (!mapped.pData || mapped.RowPitch < m_width * 4) {
        m_context->Unmap(m_fallbackStagingBgra.Get(), 0);
        return false;
    }
    CpuBgraToNv12(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, m_width, m_height, m_cpuNv12.data());
    m_context->Unmap(m_fallbackStagingBgra.Get(), 0);
    return EncodeNv12TextureOrBuffer(nullptr, m_cpuNv12.data(), m_width, timestampUs, outAnnexB, outIsKeyframe);
}

bool H264Encoder::EncodeBgra(const uint8_t* bgraData,
                             uint32_t strideBytes,
                             uint64_t timestampUs,
                             std::vector<uint8_t>& outAnnexB,
                             bool& outIsKeyframe) {
    if (!m_initialized || !bgraData || static_cast<uint64_t>(strideBytes) < static_cast<uint64_t>(m_width) * 4) {
        return false;
    }
    CpuBgraToNv12(bgraData, strideBytes, m_width, m_height, m_cpuNv12.data());
    return EncodeNv12TextureOrBuffer(nullptr, m_cpuNv12.data(), m_width, timestampUs, outAnnexB, outIsKeyframe);
}

bool H264Encoder::EncodeNv12TextureOrBuffer(ID3D11Texture2D* nv12Tex,
                                            const uint8_t* nv12Cpu,
                                            uint32_t nv12Stride,
                                            uint64_t timestampUs,
                                            std::vector<uint8_t>& outAnnexB,
                                            bool& outIsKeyframe) {
    outAnnexB.clear();
    outIsKeyframe = false;
    if (!m_mft || (m_isAsync && !m_eventGen)) return false;
    if (!nv12Tex && (!nv12Cpu || nv12Stride < m_width)) return false;

    if (m_forceKeyframe && m_codecApi) {
        VARIANT v{};
        v.vt = VT_UI4;
        v.ulVal = 1;
        m_codecApi->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &v);
        m_forceKeyframe = false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> inBuf;
    HRESULT hr = E_FAIL;
    if (nv12Tex) {
        hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12Tex, 0, FALSE, &inBuf);
        if (SUCCEEDED(hr)) {
            DWORD len = 0;
            Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
            if (SUCCEEDED(inBuf.As(&buf2d)) && SUCCEEDED(buf2d->GetContiguousLength(&len))) {
                inBuf->SetCurrentLength(len);
            }
        }
    } else if (nv12Cpu) {
        DWORD totalBytes = static_cast<DWORD>(static_cast<uint64_t>(m_width) * m_height * 3 / 2);
        hr = MFCreateMemoryBuffer(totalBytes, &inBuf);
        if (SUCCEEDED(hr)) {
            BYTE* ptr = nullptr;
            hr = inBuf->Lock(&ptr, nullptr, nullptr);
            if (SUCCEEDED(hr) && ptr) {
                if (nv12Stride == m_width) {
                    std::memcpy(ptr, nv12Cpu, totalBytes);
                } else {
                    for (uint32_t y = 0; y < m_height * 3 / 2; ++y) {
                        std::memcpy(ptr + static_cast<size_t>(y) * m_width, nv12Cpu + static_cast<size_t>(y) * nv12Stride, m_width);
                    }
                }
                inBuf->Unlock();
                inBuf->SetCurrentLength(totalBytes);
            } else {
                hr = FAILED(hr) ? hr : E_FAIL;
            }
        }
    }
    if (FAILED(hr) || !inBuf) return false;

    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(inBuf.Get());

    LONGLONG sampleTime100ns = static_cast<LONGLONG>(timestampUs) * 10;
    if (sampleTime100ns == 0) {
        sampleTime100ns = static_cast<LONGLONG>(m_frameCounter) * (10000000LL / m_fps);
    }
    sample->SetSampleTime(sampleTime100ns);
    sample->SetSampleDuration(10000000LL / m_fps);
    ++m_frameCounter;

    if (m_isAsync) {
        bool fedInput = false;
        bool sawSecondNeedInput = false;
        if (m_asyncNeedInput) {
            m_asyncNeedInput = false;
            hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
            if (FAILED(hr)) return false;
            fedInput = true;
        }
        for (int iter = 0; iter < 32; ++iter) {
            Microsoft::WRL::ComPtr<IMFMediaEvent> ev;
            // After feeding input, drain all immediately ready events non-blockingly whenever we already got output OR a second NeedInput arrived
            DWORD waitFlag = (fedInput && (!outAnnexB.empty() || sawSecondNeedInput)) ? MF_EVENT_FLAG_NO_WAIT : 0;
            hr = m_eventGen->GetEvent(waitFlag, &ev);
            if (hr == MF_E_NO_EVENTS_AVAILABLE) {
                break;
            }
            if (FAILED(hr)) return !outAnnexB.empty();
            MediaEventType met = MEUnknown;
            ev->GetType(&met);
            if (met == METransformNeedInput) {
                if (!fedInput) {
                    hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
                    if (FAILED(hr)) return false;
                    fedInput = true;
                } else {
                    m_asyncNeedInput = true;
                    // Remember need-input for next call, but keep draining non-blockingly in case HaveOutput is already queued
                    sawSecondNeedInput = true;
                    continue;
                }
            } else if (met == METransformHaveOutput) {
                DWORD status = 0;
                MFT_OUTPUT_DATA_BUFFER outBuf{};
                if (!m_mftProvidesOutputSamples) {
                    if (FAILED(SetOutputSampleBuffer(m_mft.Get(), m_outputStreamId, m_outputBufferSize, outBuf))) return false;
                } else {
                    outBuf.dwStreamID = m_outputStreamId;
                }
                hr = m_mft->ProcessOutput(0, 1, &outBuf, &status);
                if (SUCCEEDED(hr) && outBuf.pSample) {
                    Microsoft::WRL::ComPtr<IMFMediaBuffer> mb;
                    if (SUCCEEDED(outBuf.pSample->ConvertToContiguousBuffer(&mb))) {
                        BYTE* data = nullptr;
                        DWORD dataLen = 0;
                        if (SUCCEEDED(mb->Lock(&data, nullptr, &dataLen)) && data && dataLen > 0) {
                            outAnnexB.insert(outAnnexB.end(), data, data + dataLen);
                            mb->Unlock();
                        }
                    }
                    UINT32 cleanPoint = 0;
                    if (SUCCEEDED(outBuf.pSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0) {
                        outIsKeyframe = true;
                    }
                }
                if (outBuf.pSample) outBuf.pSample->Release();
                if (outBuf.pEvents) outBuf.pEvents->Release();
                if (!outAnnexB.empty()) {
                    if (!outIsKeyframe) outIsKeyframe = ContainsIdrNalu(outAnnexB.data(), outAnnexB.size());
                }
            } else if (met == MEError) {
                break;
            }
        }
        return !outAnnexB.empty();
    } else {
        hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
        if (FAILED(hr)) return false;

        for (;;) {
            DWORD status = 0;
            MFT_OUTPUT_DATA_BUFFER outBuf{};
            if (!m_mftProvidesOutputSamples) {
                if (FAILED(SetOutputSampleBuffer(m_mft.Get(), m_outputStreamId, m_outputBufferSize, outBuf))) return false;
            } else {
                outBuf.dwStreamID = m_outputStreamId;
            }
            hr = m_mft->ProcessOutput(0, 1, &outBuf, &status);
            if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
                if (outBuf.pSample) outBuf.pSample->Release();
                if (outBuf.pEvents) outBuf.pEvents->Release();
                break;
            }
            if (SUCCEEDED(hr) && outBuf.pSample) {
                Microsoft::WRL::ComPtr<IMFMediaBuffer> mb;
                if (SUCCEEDED(outBuf.pSample->ConvertToContiguousBuffer(&mb))) {
                    BYTE* data = nullptr;
                    DWORD dataLen = 0;
                    if (SUCCEEDED(mb->Lock(&data, nullptr, &dataLen)) && data && dataLen > 0) {
                        outAnnexB.insert(outAnnexB.end(), data, data + dataLen);
                        mb->Unlock();
                    }
                }
                UINT32 cleanPoint = 0;
                if (SUCCEEDED(outBuf.pSample->GetUINT32(MFSampleExtension_CleanPoint, &cleanPoint)) && cleanPoint != 0) {
                    outIsKeyframe = true;
                }
            }
            if (outBuf.pSample) outBuf.pSample->Release();
            if (outBuf.pEvents) outBuf.pEvents->Release();
            if (FAILED(hr)) break;
        }
        if (!outIsKeyframe && !outAnnexB.empty()) {
            outIsKeyframe = ContainsIdrNalu(outAnnexB.data(), outAnnexB.size());
        }
        return !outAnnexB.empty();
    }
}

// ============================================================================
// H264Decoder
// ============================================================================

H264Decoder::H264Decoder() = default;

H264Decoder::~H264Decoder() {
    Shutdown();
}

void H264Decoder::Shutdown() {
    if (m_mft) {
        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    m_vpOutView.Reset();
    m_vpOutTex = nullptr;
    m_uploadNv12.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();
    m_videoCtx.Reset();
    m_videoDev.Reset();
    m_mft.Reset();
    m_dxgiMgr.Reset();
    m_context.Reset();
    m_device.Reset();
    m_scratchNv12.clear();
    m_scratchBgra.clear();
    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
    m_width = 0;
    m_height = 0;
    m_fps = 60;
    m_codedWidth = 0;
    m_codedHeight = 0;
    m_decodedStride = 0;
    m_vpW = 0;
    m_vpH = 0;
    m_mftProvidesOutputSamples = false;
    m_outputBufferSize = 0;
    m_initialized = false;
    m_dxvaEnabled = false;
}

bool H264Decoder::Init(ID3D11Device* d3dDevice,
                       ID3D11DeviceContext* d3dCtx,
                       uint32_t width,
                       uint32_t height,
                       bool allowHardware,
                       uint32_t fps) {
    Shutdown();

    if (width < 2 || height < 2 ||
        width > 16384 || height > 16384 ||
        static_cast<uint64_t>(width) * height > 64ULL * 1024ULL * 1024ULL) {
        return false;
    }

    m_width = width;
    m_height = height;
    m_fps = std::clamp<uint32_t>(fps, 24u, 240u);
    m_codedWidth = width;
    m_codedHeight = height;
    m_decodedStride = width;
    m_device = d3dDevice;
    m_context = d3dCtx;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        Logger::Instance().Error("H264_DEC_MF", L"MFStartup failed: " + FormatHr(hr));
        Shutdown();
        return false;
    }
    m_mfStarted = true;

    MFT_REGISTER_TYPE_INFO inInfo{ MFMediaType_Video, MFVideoFormat_H264 };
    UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    std::vector<Microsoft::WRL::ComPtr<IMFTransform>> candidates;
    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inInfo, nullptr, &activates, &count);
    if (SUCCEEDED(hr) && activates) {
        for (UINT32 i = 0; i < count; ++i) {
            Microsoft::WRL::ComPtr<IMFTransform> cand;
            if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&cand))) && cand) {
                candidates.push_back(cand);
            }
            activates[i]->Release();
        }
        CoTaskMemFree(activates);
    }

    auto tryConfigure = [&](const Microsoft::WRL::ComPtr<IMFTransform>& cand) -> bool {
        if (!cand) return false;

        m_mft = cand;
        m_dxgiMgr.Reset();
        m_dxvaEnabled = false;
        m_mftProvidesOutputSamples = false;
        m_outputBufferSize = 0;
        m_width = width;
        m_height = height;
        m_codedWidth = width;
        m_codedHeight = height;
        m_decodedStride = width;

        Microsoft::WRL::ComPtr<IMFAttributes> attrs;
        if (SUCCEEDED(m_mft->GetAttributes(&attrs))) {
            attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
            UINT32 d3d11Aware = 0;
            if (allowHardware && m_device && SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware)) && d3d11Aware != 0) {
                Microsoft::WRL::ComPtr<ID3D10Multithread> mt;
                if (SUCCEEDED(m_device.As(&mt)) && mt) {
                    mt->SetMultithreadProtected(TRUE);
                }
                UINT resetToken = 0;
                if (SUCCEEDED(MFCreateDXGIDeviceManager(&resetToken, &m_dxgiMgr))) {
                    m_dxgiMgr->ResetDevice(m_device.Get(), resetToken);
                    if (SUCCEEDED(m_mft->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_dxgiMgr.Get())))) {
                        m_dxvaEnabled = true;
                    } else {
                        m_dxgiMgr.Reset();
                    }
                }
            }
        }

        Microsoft::WRL::ComPtr<ICodecAPI> decCodecApi;
        if (SUCCEEDED(m_mft.As(&decCodecApi)) && decCodecApi) {
            VARIANT vLow{};
            vLow.vt = VT_BOOL;
            vLow.boolVal = VARIANT_TRUE;
            decCodecApi->SetValue(&CODECAPI_AVLowLatencyMode, &vLow);
        }

        Microsoft::WRL::ComPtr<IMFMediaType> inType;
        if (FAILED(MFCreateMediaType(&inType))) {
            m_mft.Reset();
            m_dxgiMgr.Reset();
            m_dxvaEnabled = false;
            return false;
        }
        inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
        MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, m_fps, 1);
        inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        SetBt709MediaTypeAttributes(inType.Get());

        HRESULT hrIn = m_mft->SetInputType(0, inType.Get(), 0);
        if (FAILED(hrIn)) {
            ClearColorMediaTypeAttributes(inType.Get());
            hrIn = m_mft->SetInputType(0, inType.Get(), 0);
        }
        if (FAILED(hrIn) || !ConfigureOutputType()) {
            m_mft.Reset();
            m_dxgiMgr.Reset();
            m_dxvaEnabled = false;
            return false;
        }

        m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
        m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
        return true;
    };

    bool configured = false;
    for (const auto& cand : candidates) {
        if (tryConfigure(cand)) {
            configured = true;
            break;
        }
    }
    if (!configured) {
        Microsoft::WRL::ComPtr<IMFTransform> msDec;
        if (SUCCEEDED(CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&msDec))) && msDec) {
            configured = tryConfigure(msDec);
        }
    }
    if (!configured) {
        Logger::Instance().Error("H264_DEC_ENUM", L"No working H.264 decoder MFT found.");
        Shutdown();
        return false;
    }

    m_initialized = true;
    Logger::Instance().Info("H264_DEC_OK", L"H.264 Decoder ready (dxva=" + std::wstring(m_dxvaEnabled ? L"1" : L"0") +
                             L", fps=" + std::to_wstring(m_fps) + L")");
    return true;
}

bool H264Decoder::ConfigureOutputType() {
    for (DWORD i = 0; ; ++i) {
        Microsoft::WRL::ComPtr<IMFMediaType> outType;
        HRESULT hr = m_mft->GetOutputAvailableType(0, i, &outType);
        if (FAILED(hr)) break;
        GUID sub{};
        outType->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_NV12) {
            if (SUCCEEDED(m_mft->SetOutputType(0, outType.Get(), 0))) {
                UINT32 w = 0, h = 0;
                if (SUCCEEDED(MFGetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, &w, &h)) && w > 0 && h > 0) {
                    m_codedWidth = w;
                    m_codedHeight = h;
                    if (m_width == 0 || m_height == 0) {
                        m_width = w;
                        m_height = h;
                    }
                }
                MFVideoArea area{};
                UINT32 areaBytes = 0;
                if (SUCCEEDED(outType->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &areaBytes)) &&
                    areaBytes == sizeof(MFVideoArea) && area.Area.cx > 0 && area.Area.cy > 0) {
                    m_width = static_cast<uint32_t>(area.Area.cx);
                    m_height = static_cast<uint32_t>(area.Area.cy);
                }
                UINT32 stride = 0;
                if (SUCCEEDED(outType->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) && stride > 0) {
                    m_decodedStride = stride;
                } else {
                    m_decodedStride = m_codedWidth;
                }
                MFT_OUTPUT_STREAM_INFO osi{};
                if (SUCCEEDED(m_mft->GetOutputStreamInfo(0, &osi))) {
                    m_mftProvidesOutputSamples = (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
                    m_outputBufferSize = osi.cbSize;
                }
                return true;
            }
        }
    }
    return false;
}

bool H264Decoder::EnsureVideoProcessor(uint32_t width, uint32_t height) {
    if (!m_device || !m_context) return false;
    if (m_vp && m_vpW == width && m_vpH == height) return true;

    m_vpOutView.Reset();
    m_vpOutTex = nullptr;
    m_vp.Reset();
    m_vpEnum.Reset();
    m_uploadNv12.Reset();

    if (FAILED(m_device.As(&m_videoDev)) || FAILED(m_context.As(&m_videoCtx))) {
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cdesc{};
    cdesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cdesc.InputWidth = width;
    cdesc.InputHeight = height;
    cdesc.OutputWidth = width;
    cdesc.OutputHeight = height;
    cdesc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    if (FAILED(m_videoDev->CreateVideoProcessorEnumerator(&cdesc, &m_vpEnum))) return false;
    if (FAILED(m_videoDev->CreateVideoProcessor(m_vpEnum.Get(), 0, &m_vp))) return false;
    ConfigureVideoProcessorBt709(m_videoCtx.Get(), m_vp.Get(), false);

    D3D11_TEXTURE2D_DESC td{};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    m_device->CreateTexture2D(&td, nullptr, &m_uploadNv12);

    m_vpW = width;
    m_vpH = height;
    return true;
}

bool H264Decoder::ConvertNv12TexToBgraTex(ID3D11Texture2D* nv12Tex, uint32_t subresourceIdx, ID3D11Texture2D* outBgraTex) {
    if (!nv12Tex || !outBgraTex) return false;
    D3D11_TEXTURE2D_DESC inDesc{};
    nv12Tex->GetDesc(&inDesc);
    if (!EnsureVideoProcessor(inDesc.Width, inDesc.Height)) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    ivd.Texture2D.ArraySlice = subresourceIdx;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
    if (FAILED(m_videoDev->CreateVideoProcessorInputView(nv12Tex, m_vpEnum.Get(), &ivd, &inView))) {
        return false;
    }

    if (!m_vpOutView || m_vpOutTex != outBgraTex) {
        m_vpOutView.Reset();
        m_vpOutTex = nullptr;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
        ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        ovd.Texture2D.MipSlice = 0;
        if (FAILED(m_videoDev->CreateVideoProcessorOutputView(outBgraTex, m_vpEnum.Get(), &ovd, &m_vpOutView))) {
            return false;
        }
        m_vpOutTex = outBgraTex;
    }

    if (m_width > 0 && m_height > 0 && (inDesc.Width > m_width || inDesc.Height > m_height)) {
        RECT srcRc{ 0, 0, static_cast<LONG>(std::min(m_width, inDesc.Width)), static_cast<LONG>(std::min(m_height, inDesc.Height)) };
        m_videoCtx->VideoProcessorSetStreamSourceRect(m_vp.Get(), 0, TRUE, &srcRc);
    } else {
        m_videoCtx->VideoProcessorSetStreamSourceRect(m_vp.Get(), 0, FALSE, nullptr);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();

    return SUCCEEDED(m_videoCtx->VideoProcessorBlt(m_vp.Get(), m_vpOutView.Get(), 0, 1, &stream));
}

bool H264Decoder::Decode(const uint8_t* annexBData,
                         size_t dataSize,
                         uint64_t timestampUs,
                         ID3D11Texture2D* outBgraTexture,
                         std::vector<uint8_t>* outBgraCpu,
                         uint32_t* outWidth,
                         uint32_t* outHeight) {
    if (!m_initialized || !m_mft || !annexBData || dataSize == 0 || dataSize > kMaxFrameBytes) return false;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> inBuf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(dataSize), &inBuf))) return false;
    BYTE* ptr = nullptr;
    if (FAILED(inBuf->Lock(&ptr, nullptr, nullptr)) || !ptr) return false;
    std::memcpy(ptr, annexBData, dataSize);
    inBuf->Unlock();
    inBuf->SetCurrentLength(static_cast<DWORD>(dataSize));

    Microsoft::WRL::ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(inBuf.Get());
    sample->SetSampleTime(static_cast<LONGLONG>(timestampUs) * 10);

    HRESULT hr = m_mft->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) return false;

    return DrainOutputs(outBgraTexture, outBgraCpu, outWidth, outHeight);
}

bool H264Decoder::DrainOutputs(ID3D11Texture2D* outBgraTexture,
                               std::vector<uint8_t>* outBgraCpu,
                               uint32_t* outW,
                               uint32_t* outH) {
    bool produced = false;
    for (;;) {
        DWORD status = 0;
        MFT_OUTPUT_DATA_BUFFER outBuf{};
        if (!m_mftProvidesOutputSamples) {
            if (FAILED(SetOutputSampleBuffer(m_mft.Get(), 0, m_outputBufferSize, outBuf))) break;
        }
        HRESULT hr = m_mft->ProcessOutput(0, 1, &outBuf, &status);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (outBuf.pSample) outBuf.pSample->Release();
            if (outBuf.pEvents) outBuf.pEvents->Release();
            if (!ConfigureOutputType()) break;
            continue;
        }
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (outBuf.pSample) outBuf.pSample->Release();
            if (outBuf.pEvents) outBuf.pEvents->Release();
            break;
        }
        if (FAILED(hr)) {
            if (outBuf.pSample) outBuf.pSample->Release();
            if (outBuf.pEvents) outBuf.pEvents->Release();
            break;
        }

        if (outBuf.pSample) {
            Microsoft::WRL::ComPtr<IMFMediaBuffer> mb;
            if (SUCCEEDED(outBuf.pSample->GetBufferByIndex(0, &mb))) {
                Microsoft::WRL::ComPtr<IMFDXGIBuffer> dxgiBuf;
                if (outBgraTexture && SUCCEEDED(mb.As(&dxgiBuf))) {
                    Microsoft::WRL::ComPtr<ID3D11Texture2D> nv12Tex;
                    UINT subIdx = 0;
                    if (SUCCEEDED(dxgiBuf->GetResource(IID_PPV_ARGS(&nv12Tex))) &&
                        SUCCEEDED(dxgiBuf->GetSubresourceIndex(&subIdx))) {
                        if (ConvertNv12TexToBgraTex(nv12Tex.Get(), subIdx, outBgraTexture)) {
                            produced = true;
                        }
                    }
                }

                if (!produced || outBgraCpu) {
                    Microsoft::WRL::ComPtr<IMF2DBuffer> buf2d;
                    BYTE* scan0 = nullptr;
                    LONG pitch = 0;
                    DWORD len = 0;
                    bool locked2d = false;
                    if (SUCCEEDED(mb.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
                        locked2d = true;
                    } else if (SUCCEEDED(mb->Lock(&scan0, nullptr, &len))) {
                        pitch = static_cast<LONG>(m_decodedStride);
                    }

                    if (scan0) {
                        uint32_t planeRows = (m_codedHeight >= m_height && m_codedHeight > 0) ? m_codedHeight : m_height;
                        uint32_t stride = (pitch > 0) ? static_cast<uint32_t>(pitch) : 0;
                        uint32_t uvRowBytes = (m_width + 1u) & ~1u;
                        bool layoutOk = (m_width >= 2 && m_height >= 2 && stride >= uvRowBytes && planeRows >= m_height);
                        if (layoutOk && !locked2d) {
                            uint64_t uvRows = (static_cast<uint64_t>(m_height) + 1u) / 2u;
                            uint64_t neededBytes = static_cast<uint64_t>(stride) * planeRows;
                            if (uvRows > 0) {
                                neededBytes += static_cast<uint64_t>(stride) * (uvRows - 1) + uvRowBytes;
                            }
                            if (static_cast<uint64_t>(len) < neededBytes) {
                                layoutOk = false;
                            }
                        }

                        if (layoutOk) {
                            const uint8_t* yPlane = scan0;
                            const uint8_t* uvPlane = scan0 + static_cast<size_t>(stride) * planeRows;

                            if (outBgraCpu) {
                                outBgraCpu->resize(static_cast<size_t>(m_width) * m_height * 4);
                                CpuNv12ToBgra(yPlane, stride, uvPlane, stride, m_width, m_height, outBgraCpu->data());
                                produced = true;
                            }
                            if (outBgraTexture && !produced && m_context) {
                                D3D11_TEXTURE2D_DESC outDesc{};
                                outBgraTexture->GetDesc(&outDesc);
                                if (outDesc.Width == m_width && outDesc.Height == m_height) {
                                    // Try GPU VideoProcessor upload or CPU fallback upload (reusing scratch buffers)
                                    if (EnsureVideoProcessor(m_width, m_height) && m_uploadNv12) {
                                        m_scratchNv12.resize(static_cast<size_t>(m_width) * m_height * 3 / 2);
                                        for (uint32_t y = 0; y < m_height; ++y) {
                                            std::memcpy(m_scratchNv12.data() + static_cast<size_t>(y) * m_width,
                                                        yPlane + static_cast<size_t>(y) * stride, m_width);
                                        }
                                        uint8_t* dstUv = m_scratchNv12.data() + static_cast<size_t>(m_width) * m_height;
                                        for (uint32_t y = 0; y < m_height / 2; ++y) {
                                            std::memcpy(dstUv + static_cast<size_t>(y) * m_width,
                                                        uvPlane + static_cast<size_t>(y) * stride, m_width);
                                        }
                                        m_context->UpdateSubresource(m_uploadNv12.Get(), 0, nullptr, m_scratchNv12.data(), m_width, 0);
                                        if (ConvertNv12TexToBgraTex(m_uploadNv12.Get(), 0, outBgraTexture)) {
                                            produced = true;
                                        }
                                    }
                                    if (!produced) {
                                        m_scratchBgra.resize(static_cast<size_t>(m_width) * m_height * 4);
                                        CpuNv12ToBgra(yPlane, stride, uvPlane, stride, m_width, m_height, m_scratchBgra.data());
                                        m_context->UpdateSubresource(outBgraTexture, 0, nullptr, m_scratchBgra.data(), m_width * 4, 0);
                                        produced = true;
                                    }
                                }
                            }
                        }

                        if (locked2d) buf2d->Unlock2D();
                        else mb->Unlock();
                    }
                }
            }
            outBuf.pSample->Release();
        }
        if (outBuf.pEvents) outBuf.pEvents->Release();
    }

    if (produced) {
        if (outW) *outW = m_width;
        if (outH) *outH = m_height;
    }
    return produced;
}

} // namespace s2p
