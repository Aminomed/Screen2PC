#pragma once
#ifndef SCREEN2PC_APP_ICON_H
#define SCREEN2PC_APP_ICON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Resource.h"

namespace s2p {

inline HICON LoadAppLargeIcon(HINSTANCE hInst = nullptr) {
    HINSTANCE mod = hInst ? hInst : GetModuleHandleW(nullptr);
    HICON icon = static_cast<HICON>(LoadImageW(
        mod,
        MAKEINTRESOURCEW(IDI_SCREEN2PC_APP),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXICON),
        GetSystemMetrics(SM_CYICON),
        LR_SHARED));
    if (!icon) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

inline HICON LoadAppSmallIcon(HINSTANCE hInst = nullptr) {
    HINSTANCE mod = hInst ? hInst : GetModuleHandleW(nullptr);
    HICON icon = static_cast<HICON>(LoadImageW(
        mod,
        MAKEINTRESOURCEW(IDI_SCREEN2PC_APP),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_SHARED));
    if (!icon) {
        icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

} // namespace s2p

#endif // SCREEN2PC_APP_ICON_H
