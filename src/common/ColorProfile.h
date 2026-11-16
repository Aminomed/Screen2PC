#pragma once
#include <cstdint>
#include <string>

namespace s2p {

struct MonitorColorConfig {
    bool hdrOutputEnabled = true;   // Use scRGB R16G16B16A16_FLOAT + Gamma 2.2 on HDR monitors
    float brightnessPct = 100.0f;   // 50 .. 200
    float gamma = 2.20f;            // 1.60 .. 2.80 (direct Gamma EOTF on HDR; relative gamma/2.20 on SDR)
    float contrastPct = 100.0f;     // 60 .. 140
    float saturationPct = 100.0f;   // 50 .. 160
    float redGainPct = 100.0f;      // 70 .. 130
    float greenGainPct = 100.0f;    // 70 .. 130
    float blueGainPct = 100.0f;     // 70 .. 130

    bool operator==(const MonitorColorConfig& other) const = default;
};

struct MonitorHdrInfo {
    bool isHdr = false;
    float sdrWhiteScale = 1.0f; // Relative to 80 nits (e.g. 240 nits -> 3.0f)
    std::wstring colorSpaceLabel;
};

struct alignas(16) ColorShaderConstants {
    float params0[4]; // x=gammaPower, y=contrast, z=saturation, w=whiteScale
    float params1[4]; // x=redGain, y=greenGain, z=blueGain, w=isHdrOutput (1 or 0)
};

void ClampColorConfig(MonitorColorConfig& cfg);

ColorShaderConstants ComputeColorShaderConstants(const MonitorColorConfig& rawCfg,
                                                 bool hdrSwapChain,
                                                 float osSdrWhiteScale,
                                                 bool isCursor = false);

// CPU reference mirror of PSMain color transform for automated unit testing.
void EvaluateColorPixelCpu(float inR, float inG, float inB,
                           const ColorShaderConstants& cb,
                           float& outR, float& outG, float& outB);

MonitorColorConfig LoadMonitorColorConfigIni(const std::wstring& iniPath, uint32_t monitorIndex);
void SaveMonitorColorConfigIni(const std::wstring& iniPath, uint32_t monitorIndex, const MonitorColorConfig& rawCfg);

} // namespace s2p
