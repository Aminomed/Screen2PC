#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdint>
#include <string>
#include <vector>
#include "../common/CodecDelta.h"
#include "../common/Protocol.h"

namespace s2p {

struct DisplayOutputInfo {
    uint32_t adapterIndex = 0;
    uint32_t outputIndex = 0;
    std::wstring deviceName;
    RECT desktopRect{};
    uint32_t width = 0;
    uint32_t height = 0;
    bool isPrimary = false;
    LUID adapterLuid{};
};

struct CapturedCursor {
    bool updated = false;
    bool visible = false;
    bool shapeChanged = false;
    int32_t x = 0;
    int32_t y = 0;
    uint32_t shapeType = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t pitch = 0;
    uint32_t hotX = 0;
    uint32_t hotY = 0;
    std::vector<uint8_t> shapeData;
};

class DxgiCapture {
public:
    DxgiCapture() = default;
    ~DxgiCapture();

    DxgiCapture(const DxgiCapture&) = delete;
    DxgiCapture& operator=(const DxgiCapture&) = delete;

    static std::vector<DisplayOutputInfo> EnumerateOutputs();

    bool Init(uint32_t adapterIndex, uint32_t outputIndex);
    void Shutdown();

    bool AcquireFrame(uint32_t timeoutMs,
                      bool& outHasNewImage,
                      std::vector<DeltaRect>& outDirtyRects,
                      CapturedCursor& outCursor);

    ID3D11Device* Device() const { return m_device.Get(); }
    ID3D11DeviceContext* Context() const { return m_context.Get(); }
    ID3D11Texture2D* FrameTexture() const { return m_frameTex.Get(); }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }

    bool CopyFrameToCpu(std::vector<uint8_t>& outBgra, uint32_t& outStrideBytes);

private:
    bool RecreateDuplication();

    uint32_t m_adapterIndex = 0;
    uint32_t m_outputIndex = 0;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;

    Microsoft::WRL::ComPtr<ID3D11Device> m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_context;
    Microsoft::WRL::ComPtr<IDXGIOutput1> m_output1;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_dupl;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_frameTex;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_stagingTex;
    std::vector<uint8_t> m_metaBuf;
};

} // namespace s2p
