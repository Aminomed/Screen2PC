#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <cstdint>

namespace s2p::ui {

enum class ButtonStyle {
    Primary,
    Danger,
    Secondary,
    TabActive,
    TabInactive
};

enum class StatusTone {
    Neutral,
    Success,
    Warning,
    Error
};

struct ThemePalette {
    bool dark = true;

    COLORREF bgWindow     = RGB(24, 24, 28);
    COLORREF bgCard       = RGB(34, 35, 41);
    COLORREF bgCardHover  = RGB(44, 46, 54);
    COLORREF bgInput      = RGB(42, 43, 50);
    COLORREF bgDropZone   = RGB(30, 32, 39);
    COLORREF border       = RGB(60, 62, 72);
    COLORREF borderStrong = RGB(84, 88, 102);

    COLORREF textPrimary   = RGB(244, 244, 246);
    COLORREF textSecondary = RGB(162, 165, 178);
    COLORREF textOnAccent  = RGB(255, 255, 255);

    COLORREF accent       = RGB(59, 130, 246);
    COLORREF accentHover  = RGB(79, 148, 252);
    COLORREF danger       = RGB(220, 53, 69);
    COLORREF dangerHover  = RGB(235, 72, 88);
    COLORREF success      = RGB(34, 197, 94);
    COLORREF warning      = RGB(245, 158, 11);
    COLORREF neutralDot   = RGB(130, 134, 148);

    HBRUSH brushWindow   = nullptr;
    HBRUSH brushCard     = nullptr;
    HBRUSH brushInput    = nullptr;
    HBRUSH brushDropZone = nullptr;

    void Init(bool useDark);
    void Destroy();
    COLORREF ToneColor(StatusTone tone) const;
};

int DpiScale(int px96, UINT dpi);
bool IsSystemDarkMode();

void ApplyWindowTheme(HWND hwnd, bool dark);
void ApplyChildTheme(HWND ctrl, bool dark, const wchar_t* darkThemeName = L"DarkMode_Explorer");

LRESULT HandleCtlColor(UINT msg, WPARAM wp, const ThemePalette& pal, bool insideCard = false);

void DrawCard(HDC hdc, const RECT& rc, const ThemePalette& pal, UINT dpi, bool dashed = false);
void DrawStatusDot(HDC hdc, int cx, int cy, int radiusPx, COLORREF color, COLORREF bgFill);
void DrawModernButton(const DRAWITEMSTRUCT* dis, const ThemePalette& pal, ButtonStyle style, UINT dpi);
void DrawToggleSwitch(const DRAWITEMSTRUCT* dis, const ThemePalette& pal, bool checked, UINT dpi);

HWND CreateTooltipWindow(HWND owner);
void SetControlTooltip(HWND tooltipWnd, HWND owner, HWND ctrl, const wchar_t* text);

inline bool SetDlgItemTextIfChanged(HWND hwnd, int ctrlId, const std::wstring& text) {
    HWND ctrl = GetDlgItem(hwnd, ctrlId);
    if (!ctrl) return false;
    int len = GetWindowTextLengthW(ctrl);
    if (len == static_cast<int>(text.size())) {
        std::wstring cur(static_cast<size_t>(len) + 1, L'\0');
        int copied = GetWindowTextW(ctrl, cur.data(), len + 1);
        cur.resize(static_cast<size_t>(copied));
        if (cur == text) return false;
    }
    SetWindowTextW(ctrl, text.c_str());
    return true;
}

inline bool EnableDlgItemIfChanged(HWND hwnd, int ctrlId, bool enabled) {
    HWND ctrl = GetDlgItem(hwnd, ctrlId);
    if (!ctrl) return false;
    BOOL cur = IsWindowEnabled(ctrl);
    BOOL target = enabled ? TRUE : FALSE;
    if (cur == target) return false;
    EnableWindow(ctrl, target);
    return true;
}

inline bool SetProgressIfChanged(HWND hwnd, int ctrlId, int pos) {
    HWND ctrl = GetDlgItem(hwnd, ctrlId);
    if (!ctrl) return false;
    LRESULT cur = SendMessageW(ctrl, PBM_GETPOS, 0, 0);
    if (static_cast<int>(cur) == pos) return false;
    SendMessageW(ctrl, PBM_SETPOS, static_cast<WPARAM>(pos), 0);
    return true;
}

} // namespace s2p::ui
