#include "ColorProfile.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace s2p {

namespace {

inline float Clamp01(float v) {
    return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

inline float SafePow(float x, float e) {
    if (x < 1e-6f) return 0.0f;
    return std::pow(x, e);
}

std::wstring ColorSectionName(uint32_t monitorIndex) {
    return L"ColorMonitor" + std::to_wstring(monitorIndex);
}

int FloatToScaledInt(float val, float scale) {
    return static_cast<int>(std::lround(val * scale));
}

} // namespace

void ClampColorConfig(MonitorColorConfig& cfg) {
    auto clampF = [](float v, float lo, float hi, float defVal) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : defVal;
    };
    cfg.brightnessPct = clampF(cfg.brightnessPct, 50.0f, 200.0f, 100.0f);
    cfg.gamma         = clampF(cfg.gamma,         1.60f, 2.80f, 2.20f);
    cfg.contrastPct   = clampF(cfg.contrastPct,   60.0f, 140.0f, 100.0f);
    cfg.saturationPct = clampF(cfg.saturationPct, 50.0f, 160.0f, 100.0f);
    cfg.redGainPct    = clampF(cfg.redGainPct,    70.0f, 130.0f, 100.0f);
    cfg.greenGainPct  = clampF(cfg.greenGainPct,  70.0f, 130.0f, 100.0f);
    cfg.blueGainPct   = clampF(cfg.blueGainPct,   70.0f, 130.0f, 100.0f);
}

ColorShaderConstants ComputeColorShaderConstants(const MonitorColorConfig& rawCfg,
                                                 bool hdrSwapChain,
                                                 float osSdrWhiteScale,
                                                 bool isCursor) {
    MonitorColorConfig cfg = rawCfg;
    ClampColorConfig(cfg);

    float safeOsScale = std::isfinite(osSdrWhiteScale) ? osSdrWhiteScale : 1.0f;
    float baseOsScale = hdrSwapChain ? std::clamp(safeOsScale, 1.0f, 12.5f) : 1.0f;
    float gammaPower  = hdrSwapChain ? cfg.gamma : (cfg.gamma / 2.20f);

    ColorShaderConstants out{};
    out.params0[0] = gammaPower;
    out.params0[1] = isCursor ? 1.0f : (cfg.contrastPct / 100.0f);
    out.params0[2] = isCursor ? 1.0f : (cfg.saturationPct / 100.0f);
    out.params0[3] = baseOsScale * (cfg.brightnessPct / 100.0f);

    out.params1[0] = cfg.redGainPct / 100.0f;
    out.params1[1] = cfg.greenGainPct / 100.0f;
    out.params1[2] = cfg.blueGainPct / 100.0f;
    out.params1[3] = hdrSwapChain ? 1.0f : 0.0f;
    return out;
}

void EvaluateColorPixelCpu(float inR, float inG, float inB,
                           const ColorShaderConstants& cb,
                           float& outR, float& outG, float& outB) {
    float gammaPower = cb.params0[0];
    float contrast   = cb.params0[1];
    float saturation = cb.params0[2];
    float whiteScale = cb.params0[3];
    float redGain    = cb.params1[0];
    float greenGain  = cb.params1[1];
    float blueGain   = cb.params1[2];
    bool isHdr       = cb.params1[3] > 0.5f;

    float r = Clamp01((Clamp01(inR) - 0.5f) * contrast + 0.5f);
    float g = Clamp01((Clamp01(inG) - 0.5f) * contrast + 0.5f);
    float b = Clamp01((Clamp01(inB) - 0.5f) * contrast + 0.5f);

    r = SafePow(r, gammaPower);
    g = SafePow(g, gammaPower);
    b = SafePow(b, gammaPower);

    float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    r = luma + (r - luma) * saturation;
    g = luma + (g - luma) * saturation;
    b = luma + (b - luma) * saturation;

    if (isHdr) {
        r = std::max(0.0f, r) * redGain * whiteScale;
        g = std::max(0.0f, g) * greenGain * whiteScale;
        b = std::max(0.0f, b) * blueGain * whiteScale;
    } else {
        r = Clamp01(r * redGain * whiteScale);
        g = Clamp01(g * greenGain * whiteScale);
        b = Clamp01(b * blueGain * whiteScale);
    }

    outR = r;
    outG = g;
    outB = b;
}

MonitorColorConfig LoadMonitorColorConfigIni(const std::wstring& iniPath, uint32_t monitorIndex) {
    MonitorColorConfig cfg{};
    if (iniPath.empty()) return cfg;
    std::wstring sec = ColorSectionName(monitorIndex);
    cfg.hdrOutputEnabled = GetPrivateProfileIntW(sec.c_str(), L"HdrOutput", 1, iniPath.c_str()) != 0;
    cfg.brightnessPct    = static_cast<float>(GetPrivateProfileIntW(sec.c_str(), L"BrightnessPct", 100, iniPath.c_str()));
    cfg.gamma            = static_cast<float>(GetPrivateProfileIntW(sec.c_str(), L"GammaX100", 220, iniPath.c_str())) / 100.0f;
    cfg.contrastPct      = static_cast<float>(GetPrivateProfileIntW(sec.c_str(), L"ContrastPct", 100, iniPath.c_str()));
    cfg.saturationPct    = static_cast<float>(GetPrivateProfileIntW(sec.c_str(), L"SaturationPct", 100, iniPath.c_str()));
    cfg.redGainPct       = static_cast<float>(GetPrivateProfileIntW(sec.c_str(), L"RedGainPct", 100, iniPath.c_str()));
    cfg.greenGainPct     = static_cast<float>(GetPrivateProfileIntW(sec.c_str(), L"GreenGainPct", 100, iniPath.c_str()));
    cfg.blueGainPct      = static_cast<float>(GetPrivateProfileIntW(sec.c_str(), L"BlueGainPct", 100, iniPath.c_str()));
    ClampColorConfig(cfg);
    return cfg;
}

void SaveMonitorColorConfigIni(const std::wstring& iniPath, uint32_t monitorIndex, const MonitorColorConfig& rawCfg) {
    if (iniPath.empty()) return;
    MonitorColorConfig cfg = rawCfg;
    ClampColorConfig(cfg);
    std::wstring sec = ColorSectionName(monitorIndex);
    WritePrivateProfileStringW(sec.c_str(), L"HdrOutput",     cfg.hdrOutputEnabled ? L"1" : L"0", iniPath.c_str());
    WritePrivateProfileStringW(sec.c_str(), L"BrightnessPct", std::to_wstring(FloatToScaledInt(cfg.brightnessPct, 1.0f)).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(sec.c_str(), L"GammaX100",     std::to_wstring(FloatToScaledInt(cfg.gamma, 100.0f)).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(sec.c_str(), L"ContrastPct",   std::to_wstring(FloatToScaledInt(cfg.contrastPct, 1.0f)).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(sec.c_str(), L"SaturationPct", std::to_wstring(FloatToScaledInt(cfg.saturationPct, 1.0f)).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(sec.c_str(), L"RedGainPct",    std::to_wstring(FloatToScaledInt(cfg.redGainPct, 1.0f)).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(sec.c_str(), L"GreenGainPct",  std::to_wstring(FloatToScaledInt(cfg.greenGainPct, 1.0f)).c_str(), iniPath.c_str());
    WritePrivateProfileStringW(sec.c_str(), L"BlueGainPct",   std::to_wstring(FloatToScaledInt(cfg.blueGainPct, 1.0f)).c_str(), iniPath.c_str());
}

} // namespace s2p
