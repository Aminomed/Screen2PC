#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include "Protocol.h"

namespace s2p {

class KvmInput {
public:
    // Maps window client pixel coordinates (clientX, clientY) inside (winW x winH)
    // onto normalized [0..1] coordinates of an aspect-preserving letterboxed/pillarboxed
    // source display (srcW x srcH). Clamps outNormX/outNormY to [0..1].
    // Returns true if (clientX, clientY) lies inside the active video area.
    static bool MapWindowClientToDisplayNorm(int32_t clientX,
                                             int32_t clientY,
                                             uint32_t winW,
                                             uint32_t winH,
                                             uint32_t srcW,
                                             uint32_t srcH,
                                             float& outNormX,
                                             float& outNormY);

    // Converts normalized display coordinates [0..1] inside targetRect (which is part of
    // Windows virtual screen [vLeft, vTop, vWidth, vHeight]) into SendInput's 0..65535
    // MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK coordinate space.
    static void ComputeVirtualDesktopAbs(const RECT& targetRect,
                                         float normX,
                                         float normY,
                                         int32_t vLeft,
                                         int32_t vTop,
                                         int32_t vWidth,
                                         int32_t vHeight,
                                         int32_t& outDx,
                                         int32_t& outDy);

    // Pure user-mode injection via Win32 SendInput (zero kernel hooks/drivers).
    static bool InjectMouse(const KvmMouseEvent& ev, const RECT& targetDesktopRect);
    static bool InjectKey(const KvmKeyEvent& ev);
};

} // namespace s2p
