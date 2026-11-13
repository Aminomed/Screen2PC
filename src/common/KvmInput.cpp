#include "KvmInput.h"

#include <algorithm>
#include <cmath>

namespace s2p {

bool KvmInput::MapWindowClientToDisplayNorm(int32_t clientX,
                                            int32_t clientY,
                                            uint32_t winW,
                                            uint32_t winH,
                                            uint32_t srcW,
                                            uint32_t srcH,
                                            float& outNormX,
                                            float& outNormY) {
    if (winW == 0 || winH == 0 || srcW == 0 || srcH == 0) {
        outNormX = 0.0f;
        outNormY = 0.0f;
        return false;
    }

    float fWinW = static_cast<float>(winW);
    float fWinH = static_cast<float>(winH);
    float winAspect = fWinW / fWinH;
    float srcAspect = static_cast<float>(srcW) / static_cast<float>(srcH);

    float drawW = fWinW;
    float drawH = fWinH;
    float offX = 0.0f;
    float offY = 0.0f;

    if (winAspect > srcAspect) {
        drawW = fWinH * srcAspect;
        offX = (fWinW - drawW) * 0.5f;
    } else if (srcAspect > winAspect) {
        drawH = fWinW / srcAspect;
        offY = (fWinH - drawH) * 0.5f;
    }

    float rawX = (static_cast<float>(clientX) - offX) / std::max(drawW, 1.0f);
    float rawY = (static_cast<float>(clientY) - offY) / std::max(drawH, 1.0f);

    bool inside = (rawX >= 0.0f && rawX <= 1.0f && rawY >= 0.0f && rawY <= 1.0f);
    outNormX = std::clamp(rawX, 0.0f, 1.0f);
    outNormY = std::clamp(rawY, 0.0f, 1.0f);
    return inside;
}

void KvmInput::ComputeVirtualDesktopAbs(const RECT& targetRect,
                                        float normX,
                                        float normY,
                                        int32_t vLeft,
                                        int32_t vTop,
                                        int32_t vWidth,
                                        int32_t vHeight,
                                        int32_t& outDx,
                                        int32_t& outDy) {
    float nx = std::clamp(normX, 0.0f, 1.0f);
    float ny = std::clamp(normY, 0.0f, 1.0f);

    int32_t monW = std::max<int32_t>(1, targetRect.right - targetRect.left);
    int32_t monH = std::max<int32_t>(1, targetRect.bottom - targetRect.top);

    double px = static_cast<double>(targetRect.left) + static_cast<double>(nx) * static_cast<double>(std::max<int32_t>(1, monW - 1));
    double py = static_cast<double>(targetRect.top) + static_cast<double>(ny) * static_cast<double>(std::max<int32_t>(1, monH - 1));

    double denomX = static_cast<double>(std::max<int32_t>(1, vWidth - 1));
    double denomY = static_cast<double>(std::max<int32_t>(1, vHeight - 1));

    double dx = ((px - static_cast<double>(vLeft)) * 65535.0) / denomX;
    double dy = ((py - static_cast<double>(vTop)) * 65535.0) / denomY;

    outDx = static_cast<int32_t>(std::clamp(std::llround(dx), 0LL, 65535LL));
    outDy = static_cast<int32_t>(std::clamp(std::llround(dy), 0LL, 65535LL));
}

bool KvmInput::InjectMouse(const KvmMouseEvent& ev, const RECT& targetDesktopRect) {
    int32_t vLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int32_t vTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int32_t vWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int32_t vHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vWidth <= 0 || vHeight <= 0) {
        vLeft = 0;
        vTop = 0;
        vWidth = std::max<int32_t>(1, GetSystemMetrics(SM_CXSCREEN));
        vHeight = std::max<int32_t>(1, GetSystemMetrics(SM_CYSCREEN));
    }

    int32_t dx = 0, dy = 0;
    ComputeVirtualDesktopAbs(targetDesktopRect, ev.normX, ev.normY, vLeft, vTop, vWidth, vHeight, dx, dy);

    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;

    switch (static_cast<KvmMouseAction>(ev.action)) {
    case kMouseMove:      break;
    case kMouseLeftDown:  in.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN; break;
    case kMouseLeftUp:    in.mi.dwFlags |= MOUSEEVENTF_LEFTUP; break;
    case kMouseRightDown: in.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN; break;
    case kMouseRightUp:   in.mi.dwFlags |= MOUSEEVENTF_RIGHTUP; break;
    case kMouseMidDown:   in.mi.dwFlags |= MOUSEEVENTF_MIDDLEDOWN; break;
    case kMouseMidUp:     in.mi.dwFlags |= MOUSEEVENTF_MIDDLEUP; break;
    case kMouseWheel:
        in.mi.dwFlags |= MOUSEEVENTF_WHEEL;
        in.mi.mouseData = static_cast<DWORD>(ev.wheelDelta);
        break;
    case kMouseHWheel:
        in.mi.dwFlags |= MOUSEEVENTF_HWHEEL;
        in.mi.mouseData = static_cast<DWORD>(ev.wheelDelta);
        break;
    case kMouseX1Down:
        in.mi.dwFlags |= MOUSEEVENTF_XDOWN;
        in.mi.mouseData = XBUTTON1;
        break;
    case kMouseX1Up:
        in.mi.dwFlags |= MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON1;
        break;
    case kMouseX2Down:
        in.mi.dwFlags |= MOUSEEVENTF_XDOWN;
        in.mi.mouseData = XBUTTON2;
        break;
    case kMouseX2Up:
        in.mi.dwFlags |= MOUSEEVENTF_XUP;
        in.mi.mouseData = XBUTTON2;
        break;
    default:
        return false;
    }

    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool KvmInput::InjectKey(const KvmKeyEvent& ev) {
    if (ev.vk == 0 && ev.scanCode == 0) return false;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = ev.vk;
    in.ki.wScan = ev.scanCode;
    in.ki.dwFlags = (ev.isExtended ? KEYEVENTF_EXTENDEDKEY : 0) |
                    (ev.isDown ? 0 : KEYEVENTF_KEYUP);
    return SendInput(1, &in, sizeof(INPUT)) == 1;
}

} // namespace s2p
