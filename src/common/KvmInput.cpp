#include "KvmInput.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <vector>

namespace s2p {

namespace {

std::mutex g_cursorStateMutex;
bool g_hasVirtualPos = false;
int32_t g_virtualPosX = 0;
int32_t g_virtualPosY = 0;
std::chrono::steady_clock::time_point g_lastInjectTick{};
std::chrono::steady_clock::time_point g_lastMonitorEnumTick{};
std::vector<RECT> g_cachedMonitorRects;

BOOL CALLBACK CollectMonitorRectsProc(HMONITOR, HDC, LPRECT lprc, LPARAM dwData) {
    if (lprc && dwData) {
        auto* vec = reinterpret_cast<std::vector<RECT>*>(dwData);
        vec->push_back(*lprc);
    }
    return TRUE;
}

const std::vector<RECT>& GetCachedMonitorRectsLocked(std::chrono::steady_clock::time_point now) {
    if (g_cachedMonitorRects.empty() ||
        (now - g_lastMonitorEnumTick) > std::chrono::milliseconds(1000)) {
        g_cachedMonitorRects.clear();
        EnumDisplayMonitors(nullptr, nullptr, CollectMonitorRectsProc,
                            reinterpret_cast<LPARAM>(&g_cachedMonitorRects));
        g_lastMonitorEnumTick = now;
    }
    return g_cachedMonitorRects;
}

int64_t DistSqToRect(int32_t x, int32_t y, const RECT& r) {
    int32_t cx = std::clamp<int32_t>(x, r.left, std::max<int32_t>(r.left, r.right - 1));
    int32_t cy = std::clamp<int32_t>(y, r.top, std::max<int32_t>(r.top, r.bottom - 1));
    int64_t dx = static_cast<int64_t>(x) - cx;
    int64_t dy = static_cast<int64_t>(y) - cy;
    return dx * dx + dy * dy;
}

} // namespace

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

void KvmInput::StepVirtualDesktopPoint(int32_t curX,
                                       int32_t curY,
                                       int32_t deltaX,
                                       int32_t deltaY,
                                       const RECT* monitorRects,
                                       size_t monitorCount,
                                       int32_t vLeft,
                                       int32_t vTop,
                                       int32_t vWidth,
                                       int32_t vHeight,
                                       int32_t& outNewX,
                                       int32_t& outNewY,
                                       int32_t& outDx,
                                       int32_t& outDy) {
    int32_t maxX = vLeft + std::max<int32_t>(1, vWidth) - 1;
    int32_t maxY = vTop + std::max<int32_t>(1, vHeight) - 1;
    int32_t rawX = std::clamp<int32_t>(curX + deltaX, vLeft, maxX);
    int32_t rawY = std::clamp<int32_t>(curY + deltaY, vTop, maxY);

    if (monitorRects && monitorCount > 0) {
        bool insideAny = false;
        for (size_t i = 0; i < monitorCount; ++i) {
            const RECT& r = monitorRects[i];
            if (rawX >= r.left && rawX < r.right && rawY >= r.top && rawY < r.bottom) {
                insideAny = true;
                break;
            }
        }
        if (!insideAny) {
            // Prefer monitors whose span along the movement axis contains rawX/rawY so
            // transitions between monitors of different heights (e.g. 1504p <-> 1440p <-> 1080p)
            // never get stuck on corner dead zones.
            size_t bestIdx = 0;
            int64_t bestScore = std::numeric_limits<int64_t>::max();
            for (size_t i = 0; i < monitorCount; ++i) {
                const RECT& r = monitorRects[i];
                if (r.right <= r.left || r.bottom <= r.top) continue;
                int64_t d2 = DistSqToRect(rawX, rawY, r);
                bool xMatch = (rawX >= r.left && rawX < r.right);
                bool yMatch = (rawY >= r.top && rawY < r.bottom);
                if (!xMatch && !yMatch) {
                    d2 += 100000000LL;
                }
                if (d2 < bestScore) {
                    bestScore = d2;
                    bestIdx = i;
                }
            }
            const RECT& best = monitorRects[bestIdx];
            if (best.right > best.left && best.bottom > best.top) {
                rawX = std::clamp<int32_t>(rawX, best.left, best.right - 1);
                rawY = std::clamp<int32_t>(rawY, best.top, best.bottom - 1);
            }
        }
    }

    outNewX = rawX;
    outNewY = rawY;

    double denomX = static_cast<double>(std::max<int32_t>(1, vWidth - 1));
    double denomY = static_cast<double>(std::max<int32_t>(1, vHeight - 1));
    double dx = ((static_cast<double>(outNewX) - static_cast<double>(vLeft)) * 65535.0) / denomX;
    double dy = ((static_cast<double>(outNewY) - static_cast<double>(vTop)) * 65535.0) / denomY;

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
    {
        std::lock_guard<std::mutex> lock(g_cursorStateMutex);
        auto now = std::chrono::steady_clock::now();

        if (ev.isRelative != 0) {
            POINT osPt{};
            bool gotOsPt = (GetCursorPos(&osPt) != FALSE);
            if (!g_hasVirtualPos ||
                (now - g_lastInjectTick) > std::chrono::milliseconds(200) ||
                (gotOsPt && (now - g_lastInjectTick) > std::chrono::milliseconds(40) &&
                 (std::abs(osPt.x - g_virtualPosX) > 6 || std::abs(osPt.y - g_virtualPosY) > 6))) {
                if (gotOsPt) {
                    g_virtualPosX = osPt.x;
                    g_virtualPosY = osPt.y;
                    g_hasVirtualPos = true;
                } else {
                    g_virtualPosX = targetDesktopRect.left + std::max<int32_t>(1, (targetDesktopRect.right - targetDesktopRect.left) / 2);
                    g_virtualPosY = targetDesktopRect.top + std::max<int32_t>(1, (targetDesktopRect.bottom - targetDesktopRect.top) / 2);
                    g_hasVirtualPos = true;
                }
            }

            const auto& mons = GetCachedMonitorRectsLocked(now);
            int32_t dpx = static_cast<int32_t>(std::llround(ev.normX));
            int32_t dpy = static_cast<int32_t>(std::llround(ev.normY));
            StepVirtualDesktopPoint(g_virtualPosX, g_virtualPosY, dpx, dpy,
                                    mons.data(), mons.size(),
                                    vLeft, vTop, vWidth, vHeight,
                                    g_virtualPosX, g_virtualPosY, dx, dy);
            g_lastInjectTick = now;
        } else {
            ComputeVirtualDesktopAbs(targetDesktopRect, ev.normX, ev.normY, vLeft, vTop, vWidth, vHeight, dx, dy);
            float nx = std::clamp(ev.normX, 0.0f, 1.0f);
            float ny = std::clamp(ev.normY, 0.0f, 1.0f);
            int32_t monW = std::max<int32_t>(1, targetDesktopRect.right - targetDesktopRect.left);
            int32_t monH = std::max<int32_t>(1, targetDesktopRect.bottom - targetDesktopRect.top);
            g_virtualPosX = targetDesktopRect.left + static_cast<int32_t>(std::llround(nx * static_cast<float>(std::max<int32_t>(1, monW - 1))));
            g_virtualPosY = targetDesktopRect.top + static_cast<int32_t>(std::llround(ny * static_cast<float>(std::max<int32_t>(1, monH - 1))));
            g_hasVirtualPos = true;
            g_lastInjectTick = now;
        }
    }

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
