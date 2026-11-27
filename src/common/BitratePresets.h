#pragma once
#ifndef SCREEN2PC_BITRATE_PRESETS_H
#define SCREEN2PC_BITRATE_PRESETS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace s2p {

inline constexpr uint32_t kBitrateAutoMbps = 0;
inline constexpr uint32_t kBitratePresetsMbps[] = {
    0, 15, 25, 40, 50, 60, 80, 100, 120, 150, 200
};
inline constexpr size_t kBitratePresetCount =
    sizeof(kBitratePresetsMbps) / sizeof(kBitratePresetsMbps[0]);
inline constexpr int kDefaultBitratePresetIdx = 0; // Auto (Dynamic)
inline constexpr uint32_t kDefaultBitrateMbps = kBitratePresetsMbps[kDefaultBitratePresetIdx];

struct AutoBitratePlan {
    uint32_t minMbps = 8;
    uint32_t startMbps = 40;
    uint32_t maxMbps = 80;
};

// Computes initial and dynamic bitrate bounds (Mbit/s) for a single stream based on
// resolution, target FPS, codec efficiency (HEVC vs H.264), and multi-monitor share.
inline AutoBitratePlan ComputeAutoBitratePlan(uint32_t width,
                                              uint32_t height,
                                              uint32_t fps,
                                              bool isHevc,
                                              uint32_t streamIdx,
                                              uint32_t streamCount) {
    const uint32_t safeW = std::clamp<uint32_t>(width > 0 ? width : 1920u, 320u, 7680u);
    const uint32_t safeH = std::clamp<uint32_t>(height > 0 ? height : 1080u, 240u, 4320u);
    const uint32_t safeFps = std::clamp<uint32_t>(fps > 0 ? fps : 60u, 30u, 120u);

    const double mpPerSec = (static_cast<double>(safeW) * static_cast<double>(safeH) *
                             static_cast<double>(safeFps)) / 1000000.0;
    double startD = mpPerSec * 0.25 + 8.0;
    double maxD   = mpPerSec * 0.45 + 12.0;

    if (isHevc) {
        startD *= 0.75;
        maxD   *= 0.75;
    }

    if (streamCount > 1) {
        const double share = (streamIdx == 0)
            ? 0.65
            : (0.45 / static_cast<double>(streamCount - 1u));
        startD *= share;
        maxD   *= share;
    }

    AutoBitratePlan plan{};
    plan.minMbps = 8;
    plan.maxMbps = std::clamp<uint32_t>(static_cast<uint32_t>(maxD + 0.5), 15u, 200u);
    plan.startMbps = std::clamp<uint32_t>(static_cast<uint32_t>(startD + 0.5), plan.minMbps, plan.maxMbps);
    return plan;
}

inline uint32_t BitrateFromPresetIndex(int idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= kBitratePresetCount) {
        return kDefaultBitrateMbps;
    }
    return kBitratePresetsMbps[idx];
}

inline int FindBitratePresetIndex(uint32_t mbps) {
    if (mbps == kBitrateAutoMbps) {
        return 0;
    }
    int bestIdx = 6; // 80 Mbit/s
    int64_t bestDiff = INT64_MAX;
    for (size_t i = 1; i < kBitratePresetCount; ++i) {
        if (kBitratePresetsMbps[i] == mbps) {
            return static_cast<int>(i);
        }
        int64_t diff = std::llabs(static_cast<int64_t>(kBitratePresetsMbps[i]) -
                                   static_cast<int64_t>(mbps));
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIdx = static_cast<int>(i);
        }
    }
    return bestIdx;
}

inline int ResolveSavedBitratePresetIndex(int savedBitrateMbps, int legacyBitrateIdx) {
    if (savedBitrateMbps == 0) {
        return 0; // Explicit Auto (Dynamic)
    }
    if (savedBitrateMbps > 0) {
        return FindBitratePresetIndex(static_cast<uint32_t>(savedBitrateMbps));
    }
    // Backward-compatible migration from v1.5.0.28 and earlier 5-step list (when BitrateMbps key is absent: -1).
    constexpr uint32_t kLegacyPresets[] = { 25, 50, 80, 120, 200 };
    if (legacyBitrateIdx >= 0 && legacyBitrateIdx < 5) {
        return FindBitratePresetIndex(kLegacyPresets[legacyBitrateIdx]);
    }
    return kDefaultBitratePresetIdx;
}

inline std::wstring FormatBitrateLabel(uint32_t mbps) {
    if (mbps == kBitrateAutoMbps) {
        return L"Auto (Dyn.)";
    }
    return std::to_wstring(mbps) + L" Mbit/s";
}

// Target FPS presets: 0 = Auto (follows monitor Hz clamped 60..120), 30, 60, 90, 120
inline constexpr uint32_t kFpsPresets[] = { 0, 30, 60, 90, 120 };
inline constexpr size_t kFpsPresetCount = sizeof(kFpsPresets) / sizeof(kFpsPresets[0]);
inline constexpr int kDefaultFpsPresetIdx = 0;

inline uint32_t FpsFromPresetIndex(int idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= kFpsPresetCount) {
        return 0;
    }
    return kFpsPresets[idx];
}

inline int FindFpsPresetIndex(uint32_t fpsSetting) {
    if (fpsSetting == 0) {
        return 0;
    }
    int bestIdx = 2; // 60 FPS
    int64_t bestDiff = INT64_MAX;
    for (size_t i = 1; i < kFpsPresetCount; ++i) {
        if (kFpsPresets[i] == fpsSetting) {
            return static_cast<int>(i);
        }
        int64_t diff = std::llabs(static_cast<int64_t>(kFpsPresets[i]) -
                                   static_cast<int64_t>(fpsSetting));
        if (diff < bestDiff) {
            bestDiff = diff;
            bestIdx = static_cast<int>(i);
        }
    }
    return bestIdx;
}

inline uint32_t ResolveEffectiveFps(uint32_t fpsSetting, uint32_t monitorRefreshHz) {
    if (fpsSetting == 0) {
        uint32_t hz = (monitorRefreshHz >= 30 && monitorRefreshHz <= 240) ? monitorRefreshHz : 60u;
        return std::clamp<uint32_t>(hz, 60u, 120u);
    }
    return std::clamp<uint32_t>(fpsSetting, 30u, 120u);
}

inline std::wstring FormatFpsLabel(uint32_t fpsSetting) {
    if (fpsSetting == 0) {
        return L"Auto (Monitor-Hz)";
    }
    return std::to_wstring(fpsSetting) + L" FPS";
}

} // namespace s2p

#endif // SCREEN2PC_BITRATE_PRESETS_H
