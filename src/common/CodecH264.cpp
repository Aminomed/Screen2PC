#include "CodecH264.h"
#include "Logger.h"
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

} // namespace

void CpuBgraToNv12(const uint8_t* bgra, uint32_t bgraStride, uint32_t width, uint32_t height, uint8_t* nv12Out) {
    uint8_t* yPlane = nv12Out;
    uint8_t* uvPlane = nv12Out + static_cast<size_t>(width) * height;

    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * bgraStride;
        uint8_t* yRow = yPlane + static_cast<size_t>(y) * width;
        for (uint32_t x = 0; x < width; ++x) {
            int b = row[x * 4 + 0];
            int g = row[x * 4 + 1];
            int r = row[x * 4 + 2];
            int yVal = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
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
            int uVal = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int vVal = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            uvRow[x + 0] = ClampByte(uVal);
            uvRow[x + 1] = ClampByte(vVal);
        }
    }
}

void CpuNv12ToBgra(const uint8_t* yPlane, uint32_t yStride, const uint8_t* uvPlane, uint32_t uvStride, uint32_t width, uint32_t height, uint8_t* bgraOut) {
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* yRow = yPlane + static_cast<size_t>(y) * yStride;
        const uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * uvStride;
        uint8_t* dstRow = bgraOut + static_cast<size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            int c = static_cast<int>(yRow[x]) - 16;
            uint32_t uvX = (x & ~1u);
            int d = static_cast<int>(uvRow[uvX + 0]) - 128;
            int e = static_cast<int>(uvRow[uvX + 1]) - 128;
            int r = (298 * c + 409 * e + 128) >> 8;
            int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
            int b = (298 * c + 516 * d + 128) >> 8;
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
    m_vpOutView.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();
    m_videoCtx.Reset();
    m_videoDev.Reset();
    m_nv12Tex.Reset();
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
    if (m_width == 0 || m_height == 0) return false;
    m_fps = std::max<uint32_t>(1, fps);
    m_bitrateBps = std::max<uint32_t>(2, bitrateMbps) * 1000000u;
    m_device = d3dDevice;
    m_context = d3dCtx;
    m_forceKeyframe = true;
    m_frameCounter = 0;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        Logger::Instance().Error("H264_MF_INIT", L"MFStartup failed: " + FormatHr(hr));
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
    Logger::Instance().Info("H264_ENC_OK", L"H.264 Encoder ready (" + m_friendlyName + L", hw=" + (m_isHardware ? L"1" : L"0") + L")");
    return true;
}

bool H264Encoder::InitMft(bool tryHardware) {
    m_mft.Reset();
    m_eventGen.Reset();
    m_codecApi.Reset();
    m_isHardware = false;
    m_isAsync = false;
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
            } else {
                m_isAsync = false;
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

        hr = candidate->SetOutputType(m_outputStreamId, outType.Get(), 0);
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
                if (SUCCEEDED(candidate->SetInputType(m_inputStreamId, availIn.Get(), 0))) {
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
                inputOk = SUCCEEDED(candidate->SetInputType(m_inputStreamId, inType.Get(), 0));
            }
        }
        if (!inputOk) continue;

        // Low-latency codec API hints (best effort)
        candidate.As(&m_codecApi);
        if (m_codecApi) {
            VARIANT v{};
            v.vt = VT_BOOL;
            v.boolVal = VARIANT_TRUE;
            m_codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);

            VARIANT vGop{};
            vGop.vt = VT_UI4;
            vGop.ulVal = m_fps;
            m_codecApi->SetValue(&CODECAPI_AVEncMPVGOPSize, &vGop);

            VARIANT vB{};
            vB.vt = VT_UI4;
            vB.ulVal = 0;
            m_codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &vB);
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

    return configured;
}

bool H264Encoder::EnsureVideoProcessor() {
    if (!m_device || !m_context) return false;
    if (m_vp && m_nv12Tex && m_vpOutView) return true;

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

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_width;
    td.Height = m_height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_nv12Tex))) return false;

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    if (FAILED(m_videoDev->CreateVideoProcessorOutputView(m_nv12Tex.Get(), m_vpEnum.Get(), &ovd, &m_vpOutView))) {
        return false;
    }

    // Staging texture for software MFT readback if needed
    D3D11_TEXTURE2D_DESC st = td;
    st.Usage = D3D11_USAGE_STAGING;
    st.BindFlags = 0;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    m_device->CreateTexture2D(&st, nullptr, &m_stagingNv12);

    return true;
}

bool H264Encoder::ConvertBgraToNv12Gpu(ID3D11Texture2D* bgraTex) {
    if (!EnsureVideoProcessor()) return false;

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC ivd{};
    ivd.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ivd.Texture2D.MipSlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inView;
    HRESULT hr = m_videoDev->CreateVideoProcessorInputView(bgraTex, m_vpEnum.Get(), &ivd, &inView);
    if (FAILED(hr)) return false;

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = inView.Get();

    hr = m_videoCtx->VideoProcessorBlt(m_vp.Get(), m_vpOutView.Get(), 0, 1, &stream);
    return SUCCEEDED(hr);
}

bool H264Encoder::EncodeTexture(ID3D11Texture2D* bgraTexture,
                                uint64_t timestampUs,
                                std::vector<uint8_t>& outAnnexB,
                                bool& outIsKeyframe) {
    if (!m_initialized || !bgraTexture) return false;

    if (ConvertBgraToNv12Gpu(bgraTexture)) {
        if (m_isHardware) {
            return EncodeNv12TextureOrBuffer(m_nv12Tex.Get(), nullptr, 0, timestampUs, outAnnexB, outIsKeyframe);
        }
        // Software MFT: read back NV12 staging
        if (m_stagingNv12) {
            m_context->CopyResource(m_stagingNv12.Get(), m_nv12Tex.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(m_context->Map(m_stagingNv12.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
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
    D3D11_TEXTURE2D_DESC desc{};
    bgraTexture->GetDesc(&desc);
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
    CpuBgraToNv12(static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, m_width, m_height, m_cpuNv12.data());
    m_context->Unmap(m_fallbackStagingBgra.Get(), 0);
    return EncodeNv12TextureOrBuffer(nullptr, m_cpuNv12.data(), m_width, timestampUs, outAnnexB, outIsKeyframe);
}

bool H264Encoder::EncodeBgra(const uint8_t* bgraData,
                             uint32_t strideBytes,
                             uint64_t timestampUs,
                             std::vector<uint8_t>& outAnnexB,
                             bool& outIsKeyframe) {
    if (!m_initialized || !bgraData) return false;
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
        DWORD totalBytes = m_width * m_height * 3 / 2;
        hr = MFCreateMemoryBuffer(totalBytes, &inBuf);
        if (SUCCEEDED(hr)) {
            BYTE* ptr = nullptr;
            if (SUCCEEDED(inBuf->Lock(&ptr, nullptr, nullptr))) {
                if (nv12Stride == m_width) {
                    std::memcpy(ptr, nv12Cpu, totalBytes);
                } else {
                    for (uint32_t y = 0; y < m_height * 3 / 2; ++y) {
                        std::memcpy(ptr + static_cast<size_t>(y) * m_width, nv12Cpu + static_cast<size_t>(y) * nv12Stride, m_width);
                    }
                }
                inBuf->Unlock();
                inBuf->SetCurrentLength(totalBytes);
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
        if (m_asyncNeedInput) {
            m_asyncNeedInput = false;
            hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
            if (FAILED(hr)) return false;
            fedInput = true;
        }
        for (int iter = 0; iter < 16; ++iter) {
            Microsoft::WRL::ComPtr<IMFMediaEvent> ev;
            hr = m_eventGen->GetEvent(0, &ev);
            if (FAILED(hr)) return false;
            MediaEventType met = MEUnknown;
            ev->GetType(&met);
            if (met == METransformNeedInput) {
                if (!fedInput) {
                    hr = m_mft->ProcessInput(m_inputStreamId, sample.Get(), 0);
                    if (FAILED(hr)) return false;
                    fedInput = true;
                } else {
                    m_asyncNeedInput = true;
                    return true;
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
                        if (SUCCEEDED(mb->Lock(&data, nullptr, &dataLen)) && dataLen > 0) {
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
                    return true;
                }
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
                    if (SUCCEEDED(mb->Lock(&data, nullptr, &dataLen)) && dataLen > 0) {
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
    m_uploadNv12.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();
    m_videoCtx.Reset();
    m_videoDev.Reset();
    m_mft.Reset();
    m_dxgiMgr.Reset();
    m_context.Reset();
    m_device.Reset();
    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
    m_initialized = false;
    m_dxvaEnabled = false;
}

bool H264Decoder::Init(ID3D11Device* d3dDevice, ID3D11DeviceContext* d3dCtx, uint32_t width, uint32_t height, bool allowHardware) {
    Shutdown();

    m_width = width;
    m_height = height;
    m_codedWidth = width;
    m_codedHeight = height;
    m_decodedStride = width;
    m_device = d3dDevice;
    m_context = d3dCtx;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        Logger::Instance().Error("H264_DEC_MF", L"MFStartup failed: " + FormatHr(hr));
        return false;
    }
    m_mfStarted = true;

    MFT_REGISTER_TYPE_INFO inInfo{ MFMediaType_Video, MFVideoFormat_H264 };
    UINT32 flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inInfo, nullptr, &activates, &count);
    if (FAILED(hr) || count == 0 || !activates) {
        // Fallback to CoCreateInstance CLSID_CMSH264DecoderMFT
        hr = CoCreateInstance(CLSID_CMSH264DecoderMFT, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&m_mft));
        if (FAILED(hr)) {
            Logger::Instance().Error("H264_DEC_ENUM", L"No H.264 decoder MFT found: " + FormatHr(hr));
            Shutdown();
            return false;
        }
    } else {
        hr = activates[0]->ActivateObject(IID_PPV_ARGS(&m_mft));
        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        if (FAILED(hr)) {
            Shutdown();
            return false;
        }
    }

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
                }
            }
        }
    }

    Microsoft::WRL::ComPtr<IMFMediaType> inType;
    if (FAILED(MFCreateMediaType(&inType))) {
        Shutdown();
        return false;
    }
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, 60, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = m_mft->SetInputType(0, inType.Get(), 0);
    if (FAILED(hr)) {
        Logger::Instance().Error("H264_DEC_INTYPE", L"SetInputType failed: " + FormatHr(hr));
        Shutdown();
        return false;
    }

    if (!ConfigureOutputType()) {
        Shutdown();
        return false;
    }

    m_mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    m_initialized = true;
    Logger::Instance().Info("H264_DEC_OK", L"H.264 Decoder ready (dxva=" + std::wstring(m_dxvaEnabled ? L"1" : L"0") + L")");
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

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ovd{};
    ovd.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    ovd.Texture2D.MipSlice = 0;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> outView;
    if (FAILED(m_videoDev->CreateVideoProcessorOutputView(outBgraTex, m_vpEnum.Get(), &ovd, &outView))) {
        return false;
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

    return SUCCEEDED(m_videoCtx->VideoProcessorBlt(m_vp.Get(), outView.Get(), 0, 1, &stream));
}

bool H264Decoder::Decode(const uint8_t* annexBData,
                         size_t dataSize,
                         uint64_t timestampUs,
                         ID3D11Texture2D* outBgraTexture,
                         std::vector<uint8_t>* outBgraCpu,
                         uint32_t* outWidth,
                         uint32_t* outHeight) {
    if (!m_initialized || !annexBData || dataSize == 0) return false;

    Microsoft::WRL::ComPtr<IMFMediaBuffer> inBuf;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(dataSize), &inBuf))) return false;
    BYTE* ptr = nullptr;
    if (FAILED(inBuf->Lock(&ptr, nullptr, nullptr))) return false;
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
                    bool locked2d = false;
                    if (SUCCEEDED(mb.As(&buf2d)) && SUCCEEDED(buf2d->Lock2D(&scan0, &pitch))) {
                        locked2d = true;
                    } else {
                        DWORD len = 0;
                        if (SUCCEEDED(mb->Lock(&scan0, nullptr, &len))) {
                            pitch = static_cast<LONG>(m_decodedStride);
                        }
                    }

                    if (scan0) {
                        uint32_t stride = static_cast<uint32_t>(std::abs(pitch));
                        uint32_t planeRows = (m_codedHeight >= m_height && m_codedHeight > 0) ? m_codedHeight : m_height;
                        const uint8_t* yPlane = scan0;
                        const uint8_t* uvPlane = scan0 + static_cast<size_t>(stride) * planeRows;

                        if (outBgraCpu) {
                            outBgraCpu->resize(static_cast<size_t>(m_width) * m_height * 4);
                            CpuNv12ToBgra(yPlane, stride, uvPlane, stride, m_width, m_height, outBgraCpu->data());
                            produced = true;
                        }
                        if (outBgraTexture && !produced && m_context) {
                            // Try GPU VideoProcessor upload or CPU fallback upload
                            if (EnsureVideoProcessor(m_width, m_height) && m_uploadNv12) {
                                // Pack tightly if stride != m_width or update via staging
                                std::vector<uint8_t> packed(static_cast<size_t>(m_width) * m_height * 3 / 2);
                                for (uint32_t y = 0; y < m_height; ++y) {
                                    std::memcpy(packed.data() + static_cast<size_t>(y) * m_width, yPlane + static_cast<size_t>(y) * stride, m_width);
                                }
                                uint8_t* dstUv = packed.data() + static_cast<size_t>(m_width) * m_height;
                                for (uint32_t y = 0; y < m_height / 2; ++y) {
                                    std::memcpy(dstUv + static_cast<size_t>(y) * m_width, uvPlane + static_cast<size_t>(y) * stride, m_width);
                                }
                                m_context->UpdateSubresource(m_uploadNv12.Get(), 0, nullptr, packed.data(), m_width, 0);
                                if (ConvertNv12TexToBgraTex(m_uploadNv12.Get(), 0, outBgraTexture)) {
                                    produced = true;
                                }
                            }
                            if (!produced) {
                                std::vector<uint8_t> tmp(static_cast<size_t>(m_width) * m_height * 4);
                                CpuNv12ToBgra(yPlane, stride, uvPlane, stride, m_width, m_height, tmp.data());
                                m_context->UpdateSubresource(outBgraTexture, 0, nullptr, tmp.data(), m_width * 4, 0);
                                produced = true;
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
