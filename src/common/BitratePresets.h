#pragma once
#ifndef SCREEN2PC_BITRATE_PRESETS_H
#define SCREEN2PC_BITRATE_PRESETS_H

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace s2p {

inline constexpr uint32_t kBitratePresetsMbps[] = {
    15, 25, 40, 50, 60, 80, 100, 120, 150, 200
};
inline constexpr size_t kBitratePresetCount =
    sizeof(kBitratePresetsMbps) / sizeof(kBitratePresetsMbps[0]);
inline constexpr int kDefaultBitratePresetIdx = 5; // 80 Mbit/s
inline constexpr uint32_t kDefaultBitrateMbps = kBitratePresetsMbps[kDefaultBitratePresetIdx];

inline uint32_t BitrateFromPresetIndex(int idx) {
    if (idx < 0 || static_cast<size_t>(idx) >= kBitratePresetCount) {
        return kDefaultBitrateMbps;
    }
    return kBitratePresetsMbps[idx];
}

inline int FindBitratePresetIndex(uint32_t mbps) {
    int bestIdx = kDefaultBitratePresetIdx;
    int64_t bestDiff = INT64_MAX;
    for (size_t i = 0; i < kBitratePresetCount; ++i) {
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
    if (savedBitrateMbps > 0) {
        return FindBitratePresetIndex(static_cast<uint32_t>(savedBitrateMbps));
    }
    // Backward-compatible migration from v1.5.0.28 and earlier 5-step list.
    constexpr uint32_t kLegacyPresets[] = { 25, 50, 80, 120, 200 };
    if (legacyBitrateIdx >= 0 && legacyBitrateIdx < 5) {
        return FindBitratePresetIndex(kLegacyPresets[legacyBitrateIdx]);
    }
    return kDefaultBitratePresetIdx;
}

inline std::wstring FormatBitrateLabel(uint32_t mbps) {
    return std::to_wstring(mbps) + L" Mbit/s";
}

} // namespace s2p

#endif // SCREEN2PC_BITRATE_PRESETS_H
