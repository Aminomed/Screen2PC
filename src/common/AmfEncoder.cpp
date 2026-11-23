#include "AmfEncoder.h"
#include "AmfMinimal.h"
#include "CodecH264.h"
#include "Logger.h"
#include <algorithm>
#include <cstring>

namespace s2p {

using namespace s2p::amf_abi;

namespace {

bool AnnexBHasStartHeaders(const uint8_t* data, size_t size, CodecType codec) {
    if (!data || size < 5) return false;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t scLen = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            scLen = 3;
        } else if (i + 5 <= size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            scLen = 4;
        }
        if (scLen > 0 && i + scLen < size) {
            const uint8_t h = data[i + scLen];
            if (codec == CodecType::HEVC) {
                const uint8_t nalu = static_cast<uint8_t>((h >> 1) & 0x3F);
                if (nalu == 32 || nalu == 33 || nalu == 34) return true; // VPS/SPS/PPS
            } else {
                const uint8_t nalu = static_cast<uint8_t>(h & 0x1F);
                if (nalu == 7 || nalu == 8) return true; // SPS/PPS
            }
        }
    }
    return false;
}

bool AnnexBHasIdr(const uint8_t* data, size_t size, CodecType codec) {
    if (!data || size < 5) return false;
    for (size_t i = 0; i + 4 < size; ++i) {
        size_t scLen = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            scLen = 3;
        } else if (i + 5 <= size && data[i] == 0 && data[i + 1] == 0 &&
                   data[i + 2] == 0 && data[i + 3] == 1) {
            scLen = 4;
        }
        if (scLen > 0 && i + scLen < size) {
            const uint8_t h = data[i + scLen];
            if (codec == CodecType::HEVC) {
                const uint8_t nalu = static_cast<uint8_t>((h >> 1) & 0x3F);
                if (nalu >= 16 && nalu <= 21) return true; // BLA/IDR/CRA IRAP
            } else {
                const uint8_t nalu = static_cast<uint8_t>(h & 0x1F);
                if (nalu == 5 || nalu == 7) return true;
            }
        }
    }
    return false;
}

} // namespace

AmfEncoder::~AmfEncoder() {
    Shutdown();
}

std::wstring AmfEncoder::BackendName() const {
    return (m_codec == CodecType::HEVC) ? L"HEVC (AMF Direct)" : L"H.264 (AMF Direct)";
}

bool AmfEncoder::Initialize(ID3D11Device* device,
                            ID3D11DeviceContext* context,
                            uint32_t width,
                            uint32_t height,
                            uint32_t fps,
                            uint64_t bitrateBps,
                            CodecType codec) {
    Shutdown();
    if (!device || !context || width == 0 || height == 0) return false;
    if (codec != CodecType::H264 && codec != CodecType::HEVC) return false;

    HMODULE dll = LoadLibraryW(L"amfrt64.dll");
    if (!dll) {
        return false;
    }

    auto fnQueryVer = reinterpret_cast<AMFQueryVersion_Fn>(GetProcAddress(dll, "AMFQueryVersion"));
    auto fnInit = reinterpret_cast<AMFInit_Fn>(GetProcAddress(dll, "AMFInit"));
    if (!fnQueryVer || !fnInit) {
        FreeLibrary(dll);
        return false;
    }

    amf_uint64 runtimeVer = 0;
    if (fnQueryVer(&runtimeVer) != AMF_OK || runtimeVer == 0) {
        FreeLibrary(dll);
        return false;
    }

    AMFFactory* factory = nullptr;
    AMF_RESULT res = fnInit(std::min<amf_uint64>(runtimeVer, kAmfSdkVersion), &factory);
    if (res != AMF_OK || !factory) {
        res = fnInit(runtimeVer, &factory);
    }
    if (res != AMF_OK || !factory) {
        FreeLibrary(dll);
        return false;
    }

    AMFContext* amfCtx = nullptr;
    if (factory->pVtbl->CreateContext(factory, &amfCtx) != AMF_OK || !amfCtx) {
        FreeLibrary(dll);
        return false;
    }

    res = amfCtx->pVtbl->InitDX11(amfCtx, device, AMF_DX11_1);
    if (res != AMF_OK) {
        res = amfCtx->pVtbl->InitDX11(amfCtx, device, AMF_DX11_0);
    }
    if (res != AMF_OK) {
        amfCtx->pVtbl->Release(amfCtx);
        FreeLibrary(dll);
        return false;
    }

    const wchar_t* compId = (codec == CodecType::HEVC) ? kCompHevc : kCompAvc;
    AMFComponent* enc = nullptr;
    if (factory->pVtbl->CreateComponent(factory, amfCtx, compId, &enc) != AMF_OK || !enc) {
        amfCtx->pVtbl->Terminate(amfCtx);
        amfCtx->pVtbl->Release(amfCtx);
        FreeLibrary(dll);
        return false;
    }

    m_dll = dll;
    m_factory = factory;
    m_amfCtx = amfCtx;
    m_encoder = enc;
    m_codec = codec;
    m_width = width;
    m_height = height;
    m_fps = std::max<uint32_t>(30, fps);
    m_bitrateBps = std::clamp<uint64_t>(bitrateBps, 2000000ULL, 250000000ULL);
    m_device = device;
    m_context = context;

    auto setProp = [&](const wchar_t* key, AMFVariantStruct val) -> AMF_RESULT {
        return enc->pVtbl->SetProperty(enc, key, val);
    };

    const amf_int32 w = static_cast<amf_int32>(width);
    const amf_int32 h = static_cast<amf_int32>(height);
    const amf_uint32 fr = m_fps;
    const amf_int64 targetBr = static_cast<amf_int64>(m_bitrateBps);
    const amf_int64 peakBr = static_cast<amf_int64>(m_bitrateBps + (m_bitrateBps / 4));
    const amf_int64 vbvBits = std::max<amf_int64>(targetBr / std::max<amf_int64>(fr / 2, 15), 256000);

    if (codec == CodecType::HEVC) {
        setProp(L"HevcUsage", MakeInt64(kHevcUsageUltraLowLatency));
        setProp(L"HevcQualityPreset", MakeInt64(kHevcQualitySpeed));
        setProp(L"HevcProfile", MakeInt64(kHevcProfileMain));
        setProp(L"HevcFrameSize", MakeSize(w, h));
        setProp(L"HevcFrameRate", MakeRate(fr, 1));
        setProp(L"HevcMaxNumRefFrames", MakeInt64(1));
        setProp(L"LowLatencyInternal", MakeBool(true));
        setProp(L"HevcRateControlMethod", MakeInt64(kHevcRcLatencyConstrainedVbr));
        setProp(L"HevcTargetBitrate", MakeInt64(targetBr));
        setProp(L"HevcPeakBitrate", MakeInt64(peakBr));
        setProp(L"HevcVBVBufferSize", MakeInt64(vbvBits));
        setProp(L"HevcInitialVBVBufferFullness", MakeInt64(64));
        setProp(L"HevcFillerDataEnable", MakeBool(false));
        setProp(L"HevcRateControlSkipFrameEnable", MakeBool(false));
        setProp(L"HevcHeaderInsertionMode", MakeInt64(kHevcHdrInsertIdrAligned));
        setProp(L"HevcGOPSize", MakeInt64(300));
        setProp(L"HevcGOPSPerIDR", MakeInt64(1));
        setProp(L"HevcSlicesPerFrame", MakeInt64(1));
        setProp(L"HevcQueryTimeout", MakeInt64(8));
    } else {
        setProp(L"Usage", MakeInt64(kAvcUsageUltraLowLatency));
        setProp(L"QualityPreset", MakeInt64(kAvcQualitySpeed));
        setProp(L"Profile", MakeInt64(kAvcProfileConstrainedBaseline));
        setProp(L"FrameSize", MakeSize(w, h));
        setProp(L"FrameRate", MakeRate(fr, 1));
        setProp(L"MaxNumRefFrames", MakeInt64(1));
        setProp(L"BPicturesPattern", MakeInt64(0));
        setProp(L"LowLatencyInternal", MakeBool(true));
        setProp(L"CABACEnable", MakeInt64(kAvcCodingCalv));
        setProp(L"RateControlMethod", MakeInt64(kAvcRcLatencyConstrainedVbr));
        setProp(L"TargetBitrate", MakeInt64(targetBr));
        setProp(L"PeakBitrate", MakeInt64(peakBr));
        setProp(L"VBVBufferSize", MakeInt64(vbvBits));
        setProp(L"InitialVBVBufferFullness", MakeInt64(64));
        setProp(L"FillerDataEnable", MakeBool(false));
        setProp(L"RateControlSkipFrameEnable", MakeBool(false));
        setProp(L"HeaderInsertionSpacing", MakeInt64(300));
        setProp(L"IDRPeriod", MakeInt64(300));
        setProp(L"SlicesPerFrame", MakeInt64(1));
        setProp(L"QueryTimeout", MakeInt64(8));
    }

    res = enc->pVtbl->Init(enc, AMF_SURFACE_NV12, w, h);
    if (res != AMF_OK && codec == CodecType::H264) {
        // Fallback to Main/Baseline profile if ConstrainedBaseline was rejected by older VCE
        setProp(L"Profile", MakeInt64(kAvcProfileBaseline));
        res = enc->pVtbl->Init(enc, AMF_SURFACE_NV12, w, h);
    }
    if (res != AMF_OK) {
        Logger::Instance().Warn("AMF-INIT",
            L"encoder->Init failed (codec=" + std::to_wstring(static_cast<int>(codec)) +
            L", res=" + std::to_wstring(static_cast<int>(res)) + L")");
        Shutdown();
        return false;
    }

    // Grab ExtraData (SPS/PPS[/VPS]) if exposed
    AMFVariantStruct extraVar{};
    const wchar_t* extraKey = (codec == CodecType::HEVC) ? L"HevcExtraData" : L"ExtraData";
    if (enc->pVtbl->GetProperty(enc, extraKey, &extraVar) == AMF_OK) {
        if (extraVar.type == AMF_VARIANT_INTERFACE && extraVar.pInterface) {
            AMFBuffer* buf = nullptr;
            if (extraVar.pInterface->pVtbl->QueryInterface(
                    extraVar.pInterface, &kIID_AMFBuffer, reinterpret_cast<void**>(&buf)) == AMF_OK && buf) {
                const amf_size sz = buf->pVtbl->GetSize(buf);
                const void* ptr = buf->pVtbl->GetNative(buf);
                if (sz > 0 && ptr) {
                    m_extradata.resize(sz);
                    std::memcpy(m_extradata.data(), ptr, sz);
                }
                buf->pVtbl->Release(buf);
            }
        }
        ClearVariant(extraVar);
    }

    // Setup D3D11 VideoProcessor for BGRA -> NV12 BT.709 Studio color conversion
    if (FAILED(device->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void**>(m_videoDev.GetAddressOf()))) ||
        FAILED(context->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void**>(m_videoCtx.GetAddressOf())))) {
        Shutdown();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC contentDesc{};
    contentDesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    contentDesc.InputFrameRate = { m_fps, 1 };
    contentDesc.InputWidth = width;
    contentDesc.InputHeight = height;
    contentDesc.OutputFrameRate = { m_fps, 1 };
    contentDesc.OutputWidth = width;
    contentDesc.OutputHeight = height;
    contentDesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(m_videoDev->CreateVideoProcessorEnumerator(&contentDesc, m_vpEnum.GetAddressOf())) ||
        FAILED(m_videoDev->CreateVideoProcessor(m_vpEnum.Get(), 0, m_vp.GetAddressOf()))) {
        Shutdown();
        return false;
    }

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inCs{};
    inCs.Usage = 0;
    inCs.RGB_Range = 0; // Full 0..255 sRGB
    inCs.YCbCr_Matrix = 1; // BT.709
    inCs.YCbCr_xvYCC = 0;
    inCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    m_videoCtx->VideoProcessorSetStreamColorSpace(m_vp.Get(), 0, &inCs);

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outCs{};
    outCs.Usage = 0;
    outCs.RGB_Range = 0;
    outCs.YCbCr_Matrix = 1; // BT.709
    outCs.YCbCr_xvYCC = 0;
    outCs.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
    m_videoCtx->VideoProcessorSetOutputColorSpace(m_vp.Get(), &outCs);
    m_videoCtx->VideoProcessorSetStreamAutoProcessingMode(m_vp.Get(), 0, FALSE);

    // Allocate 3 AMF DX11 NV12 surfaces in a ring
    m_directVpWrite = true;
    for (size_t i = 0; i < kRingSize; ++i) {
        if (m_amfCtx->pVtbl->AllocSurface(m_amfCtx, AMF_MEMORY_DX11, AMF_SURFACE_NV12,
                                          w, h, &m_amfSurfaces[i]) != AMF_OK || !m_amfSurfaces[i]) {
            Shutdown();
            return false;
        }
        AMFPlane* plane = m_amfSurfaces[i]->pVtbl->GetPlaneAt(m_amfSurfaces[i], 0);
        if (!plane) {
            Shutdown();
            return false;
        }
        auto* tex = reinterpret_cast<ID3D11Texture2D*>(plane->pVtbl->GetNative(plane));
        if (!tex) {
            Shutdown();
            return false;
        }
        m_amfTextures[i] = tex;

        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
        outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(m_videoDev->CreateVideoProcessorOutputView(
                tex, m_vpEnum.Get(), &outDesc, m_amfVpOutViews[i].GetAddressOf()))) {
            m_directVpWrite = false;
        }
    }

    if (!m_directVpWrite) {
        D3D11_TEXTURE2D_DESC nv12Desc{};
        nv12Desc.Width = width;
        nv12Desc.Height = height;
        nv12Desc.MipLevels = 1;
        nv12Desc.ArraySize = 1;
        nv12Desc.Format = DXGI_FORMAT_NV12;
        nv12Desc.SampleDesc.Count = 1;
        nv12Desc.Usage = D3D11_USAGE_DEFAULT;
        nv12Desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&nv12Desc, nullptr, m_vpNv12Tex.GetAddressOf()))) {
            Shutdown();
            return false;
        }
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC outDesc{};
        outDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(m_videoDev->CreateVideoProcessorOutputView(
                m_vpNv12Tex.Get(), m_vpEnum.Get(), &outDesc, m_vpOutView.GetAddressOf()))) {
            Shutdown();
            return false;
        }
    }

    m_initialized = true;
    Logger::Instance().Info("AMF-INIT",
        BackendName() + L" initialized " + std::to_wstring(width) + L"x" + std::to_wstring(height) +
        L"@" + std::to_wstring(m_fps) + L"Hz (" + std::to_wstring(m_bitrateBps / 1000000ULL) +
        L" Mbps, directVP=" + (m_directVpWrite ? L"1" : L"0") + L")");
    return true;
}

bool AmfEncoder::EncodeTexture(ID3D11Texture2D* bgraTex,
                               bool forceKeyframe,
                               std::vector<uint8_t>& outAnnexB,
                               bool& outIsKeyframe) {
    outAnnexB.clear();
    outIsKeyframe = false;
    if (!m_initialized || !bgraTex || !m_encoder) return false;

    if (m_cachedInputTex != bgraTex) {
        m_cachedInputView.Reset();
        m_cachedInputTex = bgraTex;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC inDesc{};
        inDesc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
        if (FAILED(m_videoDev->CreateVideoProcessorInputView(
                bgraTex, m_vpEnum.Get(), &inDesc, m_cachedInputView.GetAddressOf()))) {
            m_cachedInputTex = nullptr;
            return false;
        }
    }

    const size_t slot = m_ringIdx;
    m_ringIdx = (m_ringIdx + 1) % kRingSize;

    ID3D11VideoProcessorOutputView* vpOut = m_directVpWrite ? m_amfVpOutViews[slot].Get() : m_vpOutView.Get();
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = m_cachedInputView.Get();
    if (FAILED(m_videoCtx->VideoProcessorBlt(m_vp.Get(), vpOut, 0, 1, &stream))) {
        return false;
    }

    if (!m_directVpWrite) {
        m_context->CopyResource(m_amfTextures[slot], m_vpNv12Tex.Get());
    }

    AMFSurface* surf = m_amfSurfaces[slot];
    const amf_pts pts = static_cast<amf_pts>((m_frameCounter++ * 10000000ULL) / m_fps);
    surf->pVtbl->SetPts(surf, pts);

    if (m_codec == CodecType::HEVC) {
        surf->pVtbl->SetProperty(surf, L"HevcForcePictureType",
                                 MakeInt64(forceKeyframe ? kHevcPicIdr : kHevcPicNone));
        surf->pVtbl->SetProperty(surf, L"HevcInsertHeader", MakeBool(forceKeyframe));
    } else {
        surf->pVtbl->SetProperty(surf, L"ForcePictureType",
                                 MakeInt64(forceKeyframe ? kAvcPicIdr : kAvcPicNone));
        surf->pVtbl->SetProperty(surf, L"InsertSPS", MakeBool(forceKeyframe));
        surf->pVtbl->SetProperty(surf, L"InsertPPS", MakeBool(forceKeyframe));
    }

    AMF_RESULT res = AMF_INPUT_FULL;
    for (int retry = 0; retry < 30; ++retry) {
        res = m_encoder->pVtbl->SubmitInput(m_encoder, reinterpret_cast<AMFData*>(surf));
        if (res != AMF_INPUT_FULL) break;
        Sleep(1);
    }
    if (res != AMF_OK && res != AMF_NEED_MORE_INPUT) {
        return false;
    }

    AMFData* outData = nullptr;
    for (int poll = 0; poll < 40; ++poll) {
        res = m_encoder->pVtbl->QueryOutput(m_encoder, &outData);
        if (outData != nullptr) break;
        if (res != AMF_REPEAT && res != AMF_OK) break;
        Sleep(1);
    }
    if (!outData) {
        return false;
    }

    AMFBuffer* buf = nullptr;
    bool ok = false;
    if (outData->pVtbl->QueryInterface(outData, &kIID_AMFBuffer, reinterpret_cast<void**>(&buf)) == AMF_OK && buf) {
        const void* ptr = buf->pVtbl->GetNative(buf);
        const amf_size sz = buf->pVtbl->GetSize(buf);
        if (ptr && sz > 0) {
            outAnnexB.resize(sz);
            std::memcpy(outAnnexB.data(), ptr, sz);

            AMFVariantStruct dtVar{};
            const wchar_t* dtKey = (m_codec == CodecType::HEVC) ? L"HevcOutputDataType" : L"OutputDataType";
            if (buf->pVtbl->GetProperty(buf, dtKey, &dtVar) == AMF_OK) {
                if (dtVar.type == AMF_VARIANT_INT64) {
                    const amf_int64 idrVal = (m_codec == CodecType::HEVC) ? kHevcOutIdr : kAvcOutIdr;
                    const amf_int64 iVal   = (m_codec == CodecType::HEVC) ? kHevcOutI   : kAvcOutI;
                    if (dtVar.int64Value == idrVal || dtVar.int64Value == iVal) {
                        outIsKeyframe = true;
                    }
                }
                ClearVariant(dtVar);
            }
            if (!outIsKeyframe) {
                outIsKeyframe = AnnexBHasIdr(outAnnexB.data(), outAnnexB.size(), m_codec);
            }

            if (outIsKeyframe && !m_extradata.empty() &&
                !AnnexBHasStartHeaders(outAnnexB.data(), outAnnexB.size(), m_codec)) {
                std::vector<uint8_t> combined;
                combined.reserve(m_extradata.size() + outAnnexB.size());
                combined.insert(combined.end(), m_extradata.begin(), m_extradata.end());
                combined.insert(combined.end(), outAnnexB.begin(), outAnnexB.end());
                outAnnexB.swap(combined);
            }

            if (m_codec == CodecType::H264) {
                EnsureLowLatencyH264Stream(outAnnexB);
            }
            ok = !outAnnexB.empty();
        }
        buf->pVtbl->Release(buf);
    }
    outData->pVtbl->Release(outData);
    return ok;
}

void AmfEncoder::SetBitrate(uint64_t bitrateBps) {
    if (!m_initialized || !m_encoder) return;
    m_bitrateBps = std::clamp<uint64_t>(bitrateBps, 2000000ULL, 250000000ULL);
    const amf_int64 targetBr = static_cast<amf_int64>(m_bitrateBps);
    const amf_int64 peakBr = static_cast<amf_int64>(m_bitrateBps + (m_bitrateBps / 4));
    const amf_int64 vbvBits = std::max<amf_int64>(targetBr / std::max<amf_int64>(m_fps / 2, 15), 256000);
    if (m_codec == CodecType::HEVC) {
        m_encoder->pVtbl->SetProperty(m_encoder, L"HevcTargetBitrate", MakeInt64(targetBr));
        m_encoder->pVtbl->SetProperty(m_encoder, L"HevcPeakBitrate", MakeInt64(peakBr));
        m_encoder->pVtbl->SetProperty(m_encoder, L"HevcVBVBufferSize", MakeInt64(vbvBits));
    } else {
        m_encoder->pVtbl->SetProperty(m_encoder, L"TargetBitrate", MakeInt64(targetBr));
        m_encoder->pVtbl->SetProperty(m_encoder, L"PeakBitrate", MakeInt64(peakBr));
        m_encoder->pVtbl->SetProperty(m_encoder, L"VBVBufferSize", MakeInt64(vbvBits));
    }
}

void AmfEncoder::Shutdown() {
    m_initialized = false;
    m_cachedInputView.Reset();
    m_cachedInputTex = nullptr;
    m_extradata.clear();

    for (size_t i = 0; i < kRingSize; ++i) {
        m_amfVpOutViews[i].Reset();
        m_amfTextures[i] = nullptr;
        if (m_amfSurfaces[i]) {
            m_amfSurfaces[i]->pVtbl->Release(m_amfSurfaces[i]);
            m_amfSurfaces[i] = nullptr;
        }
    }

    m_vpOutView.Reset();
    m_vpNv12Tex.Reset();
    m_vp.Reset();
    m_vpEnum.Reset();
    m_videoCtx.Reset();
    m_videoDev.Reset();
    m_context.Reset();
    m_device.Reset();

    if (m_encoder) {
        m_encoder->pVtbl->Terminate(m_encoder);
        m_encoder->pVtbl->Release(m_encoder);
        m_encoder = nullptr;
    }
    if (m_amfCtx) {
        m_amfCtx->pVtbl->Terminate(m_amfCtx);
        m_amfCtx->pVtbl->Release(m_amfCtx);
        m_amfCtx = nullptr;
    }
    m_factory = nullptr;
    if (m_dll) {
        FreeLibrary(m_dll);
        m_dll = nullptr;
    }
}

} // namespace s2p
