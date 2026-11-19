#include "UiTheme.h"

#include <algorithm>
#include <cstring>

namespace s2p::ui {

namespace {

using FnDwmSetWindowAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
using FnSetWindowTheme = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);

FnDwmSetWindowAttribute GetDwmSetWindowAttribute() {
    static FnDwmSetWindowAttribute fn = []() -> FnDwmSetWindowAttribute {
        HMODULE mod = LoadLibraryW(L"dwmapi.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<FnDwmSetWindowAttribute>(GetProcAddress(mod, "DwmSetWindowAttribute"));
    }();
    return fn;
}

FnSetWindowTheme GetSetWindowTheme() {
    static FnSetWindowTheme fn = []() -> FnSetWindowTheme {
        HMODULE mod = LoadLibraryW(L"uxtheme.dll");
        if (!mod) return nullptr;
        return reinterpret_cast<FnSetWindowTheme>(GetProcAddress(mod, "SetWindowTheme"));
    }();
    return fn;
}

COLORREF BlendColor(COLORREF a, COLORREF b, int num, int den) {
    int r = (GetRValue(a) * (den - num) + GetRValue(b) * num) / den;
    int g = (GetGValue(a) * (den - num) + GetGValue(b) * num) / den;
    int bl = (GetBValue(a) * (den - num) + GetBValue(b) * num) / den;
    return RGB(std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(bl, 0, 255));
}

} // namespace

int DpiScale(int px96, UINT dpi) {
    return MulDiv(px96, static_cast<int>(dpi), 96);
}

bool IsSystemDarkMode() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS) {
        return true;
    }
    DWORD val = 1;
    DWORD sz = sizeof(val);
    DWORD type = 0;
    LSTATUS st = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                  reinterpret_cast<LPBYTE>(&val), &sz);
    RegCloseKey(key);
    if (st == ERROR_SUCCESS && type == REG_DWORD) {
        return val == 0;
    }
    return true;
}

void ThemePalette::Init(bool useDark) {
    Destroy();
    dark = useDark;
    if (dark) {
        bgWindow     = RGB(22, 23, 27);
        bgCard       = RGB(33, 35, 42);
        bgCardHover  = RGB(45, 48, 58);
        bgInput      = RGB(41, 43, 52);
        bgDropZone   = RGB(28, 30, 37);
        border       = RGB(58, 61, 72);
        borderStrong = RGB(86, 90, 106);

        textPrimary   = RGB(244, 245, 248);
        textSecondary = RGB(160, 165, 180);
        textOnAccent  = RGB(255, 255, 255);

        accent       = RGB(59, 130, 246);
        accentHover  = RGB(82, 148, 252);
        danger       = RGB(220, 53, 69);
        dangerHover  = RGB(236, 74, 90);
        success      = RGB(34, 197, 94);
        warning      = RGB(245, 158, 11);
        neutralDot   = RGB(130, 135, 150);
    } else {
        bgWindow     = RGB(243, 244, 247);
        bgCard       = RGB(255, 255, 255);
        bgCardHover  = RGB(236, 239, 245);
        bgInput      = RGB(255, 255, 255);
        bgDropZone   = RGB(248, 250, 253);
        border       = RGB(218, 222, 230);
        borderStrong = RGB(180, 186, 198);

        textPrimary   = RGB(24, 25, 30);
        textSecondary = RGB(98, 104, 118);
        textOnAccent  = RGB(255, 255, 255);

        accent       = RGB(37, 99, 235);
        accentHover  = RGB(59, 130, 246);
        danger       = RGB(211, 47, 47);
        dangerHover  = RGB(229, 68, 68);
        success      = RGB(22, 163, 74);
        warning      = RGB(217, 119, 6);
        neutralDot   = RGB(148, 154, 168);
    }

    brushWindow   = CreateSolidBrush(bgWindow);
    brushCard     = CreateSolidBrush(bgCard);
    brushInput    = CreateSolidBrush(bgInput);
    brushDropZone = CreateSolidBrush(bgDropZone);
}

void ThemePalette::Destroy() {
    if (brushWindow)   { DeleteObject(brushWindow);   brushWindow = nullptr; }
    if (brushCard)     { DeleteObject(brushCard);     brushCard = nullptr; }
    if (brushInput)    { DeleteObject(brushInput);    brushInput = nullptr; }
    if (brushDropZone) { DeleteObject(brushDropZone); brushDropZone = nullptr; }
}

COLORREF ThemePalette::ToneColor(StatusTone tone) const {
    switch (tone) {
    case StatusTone::Success: return success;
    case StatusTone::Warning: return warning;
    case StatusTone::Error:   return danger;
    default:                  return neutralDot;
    }
}

void ApplyWindowTheme(HWND hwnd, bool dark) {
    if (!hwnd || !IsWindow(hwnd)) return;
    auto fnDwm = GetDwmSetWindowAttribute();
    if (!fnDwm) return;

    BOOL darkVal = dark ? TRUE : FALSE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Win11 / Win10 2004+), fallback 19
    if (FAILED(fnDwm(hwnd, 20, &darkVal, sizeof(darkVal)))) {
        fnDwm(hwnd, 19, &darkVal, sizeof(darkVal));
    }
    // DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWCP_ROUND = 2 (Win11)
    DWORD corner = 2;
    fnDwm(hwnd, 33, &corner, sizeof(corner));
}

void ApplyChildTheme(HWND ctrl, bool dark, const wchar_t* darkThemeName) {
    if (!ctrl || !IsWindow(ctrl)) return;
    auto fnTheme = GetSetWindowTheme();
    if (!fnTheme) return;
    fnTheme(ctrl, dark ? darkThemeName : L"Explorer", nullptr);
}

LRESULT HandleCtlColor(UINT msg, WPARAM wp, const ThemePalette& pal, bool insideCard) {
    HDC hdc = reinterpret_cast<HDC>(wp);
    if (!hdc) return 0;

    switch (msg) {
    case WM_CTLCOLORDLG:
        SetTextColor(hdc, pal.textPrimary);
        SetBkColor(hdc, pal.bgWindow);
        return reinterpret_cast<LRESULT>(pal.brushWindow ? pal.brushWindow : GetStockObject(NULL_BRUSH));
    case WM_CTLCOLORSTATIC:
        SetTextColor(hdc, pal.textPrimary);
        SetBkColor(hdc, insideCard ? pal.bgCard : pal.bgWindow);
        return reinterpret_cast<LRESULT>(insideCard ? pal.brushCard : pal.brushWindow);
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(hdc, pal.textPrimary);
        SetBkColor(hdc, pal.bgInput);
        return reinterpret_cast<LRESULT>(pal.brushInput);
    default:
        return 0;
    }
}

void DrawCard(HDC hdc, const RECT& rc, const ThemePalette& pal, UINT dpi, bool dashed) {
    int radius = DpiScale(10, dpi);
    HBRUSH fill = CreateSolidBrush(dashed ? pal.bgDropZone : pal.bgCard);
    HPEN pen = CreatePen(dashed ? PS_DASH : PS_SOLID, 1, dashed ? pal.borderStrong : pal.border);
    HGDIOBJ oldBrush = SelectObject(hdc, fill);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    int oldBk = SetBkMode(hdc, TRANSPARENT);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SetBkMode(hdc, oldBk);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(pen);
    DeleteObject(fill);
}

void DrawStatusDot(HDC hdc, int cx, int cy, int radiusPx, COLORREF color, COLORREF bgFill) {
    COLORREF halo = BlendColor(bgFill, color, 1, 3);
    int haloR = radiusPx + std::max(2, radiusPx / 2);
    HBRUSH hBrush = CreateSolidBrush(halo);
    HPEN hPen = CreatePen(PS_SOLID, 1, halo);
    HGDIOBJ ob = SelectObject(hdc, hBrush);
    HGDIOBJ op = SelectObject(hdc, hPen);
    Ellipse(hdc, cx - haloR, cy - haloR, cx + haloR, cy + haloR);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(hPen);
    DeleteObject(hBrush);

    HBRUSH dBrush = CreateSolidBrush(color);
    HPEN dPen = CreatePen(PS_SOLID, 1, color);
    ob = SelectObject(hdc, dBrush);
    op = SelectObject(hdc, dPen);
    Ellipse(hdc, cx - radiusPx, cy - radiusPx, cx + radiusPx, cy + radiusPx);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(dPen);
    DeleteObject(dBrush);
}

void DrawModernButton(const DRAWITEMSTRUCT* dis, const ThemePalette& pal, ButtonStyle style, UINT dpi) {
    if (!dis || !dis->hDC) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused  = (dis->itemState & ODS_FOCUS) != 0;

    // Fill outer background with card or window color so rounded corners look clean
    COLORREF outerBg = (style == ButtonStyle::TabActive || style == ButtonStyle::TabInactive)
                           ? pal.bgWindow
                           : pal.bgCard;
    HBRUSH outerBrush = CreateSolidBrush(outerBg);
    FillRect(hdc, &rc, outerBrush);
    DeleteObject(outerBrush);

    COLORREF fillCol = pal.bgInput;
    COLORREF borderCol = pal.border;
    COLORREF textCol = pal.textPrimary;

    switch (style) {
    case ButtonStyle::Primary:
        fillCol = disabled ? pal.border : (pressed ? BlendColor(pal.accent, RGB(0, 0, 0), 1, 5) : pal.accent);
        borderCol = fillCol;
        textCol = disabled ? pal.textSecondary : pal.textOnAccent;
        break;
    case ButtonStyle::Danger:
        fillCol = disabled ? pal.border : (pressed ? BlendColor(pal.danger, RGB(0, 0, 0), 1, 5) : pal.danger);
        borderCol = fillCol;
        textCol = disabled ? pal.textSecondary : pal.textOnAccent;
        break;
    case ButtonStyle::Secondary:
        fillCol = disabled ? pal.bgWindow : (pressed ? pal.bgCardHover : pal.bgInput);
        borderCol = focused ? pal.accent : (disabled ? pal.border : pal.borderStrong);
        textCol = disabled ? pal.textSecondary : pal.textPrimary;
        break;
    case ButtonStyle::TabActive:
        fillCol = pal.bgCard;
        borderCol = pal.border;
        textCol = pal.textPrimary;
        break;
    case ButtonStyle::TabInactive:
        fillCol = pressed ? pal.bgCardHover : pal.bgWindow;
        borderCol = pal.bgWindow;
        textCol = pal.textSecondary;
        break;
    }

    int radius = DpiScale(7, dpi);
    RECT drawRc = rc;
    InflateRect(&drawRc, -1, -1);

    HBRUSH fillBr = CreateSolidBrush(fillCol);
    HPEN borderPen = CreatePen(PS_SOLID, 1, borderCol);
    HGDIOBJ ob = SelectObject(hdc, fillBr);
    HGDIOBJ op = SelectObject(hdc, borderPen);
    RoundRect(hdc, drawRc.left, drawRc.top, drawRc.right, drawRc.bottom, radius, radius);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(borderPen);
    DeleteObject(fillBr);

    // Accent bar at bottom of active tab
    if (style == ButtonStyle::TabActive) {
        int barH = std::max(2, DpiScale(3, dpi));
        int inset = DpiScale(12, dpi);
        RECT barRc{ drawRc.left + inset, drawRc.bottom - barH - 1, drawRc.right - inset, drawRc.bottom - 1 };
        if (barRc.right > barRc.left) {
            HBRUSH accBr = CreateSolidBrush(pal.accent);
            FillRect(hdc, &barRc, accBr);
            DeleteObject(accBr);
        }
    }

    wchar_t text[256]{};
    GetWindowTextW(dis->hwndItem, text, _countof(text));
    if (text[0] != L'\0') {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, textCol);
        RECT textRc = drawRc;
        if (pressed) OffsetRect(&textRc, 0, 1);
        DrawTextW(hdc, text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (focused && style != ButtonStyle::TabActive && style != ButtonStyle::TabInactive) {
        RECT focRc = drawRc;
        InflateRect(&focRc, -DpiScale(3, dpi), -DpiScale(3, dpi));
        DrawFocusRect(hdc, &focRc);
    }
}

void DrawToggleSwitch(const DRAWITEMSTRUCT* dis, const ThemePalette& pal, bool checked, UINT dpi) {
    if (!dis || !dis->hDC) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused  = (dis->itemState & ODS_FOCUS) != 0;

    HBRUSH bgBr = CreateSolidBrush(pal.bgCard);
    FillRect(hdc, &rc, bgBr);
    DeleteObject(bgBr);

    int trackW = DpiScale(36, dpi);
    int trackH = DpiScale(18, dpi);
    int trackX = rc.left + DpiScale(2, dpi);
    int trackY = rc.top + ((rc.bottom - rc.top) - trackH) / 2;
    RECT trackRc{ trackX, trackY, trackX + trackW, trackY + trackH };

    COLORREF trackFill = checked
                             ? (disabled ? pal.border : (pressed ? pal.accentHover : pal.accent))
                             : (pressed ? pal.bgCardHover : pal.bgInput);
    COLORREF trackBorder = checked ? trackFill : pal.borderStrong;

    HBRUSH trBr = CreateSolidBrush(trackFill);
    HPEN trPen = CreatePen(PS_SOLID, 1, trackBorder);
    HGDIOBJ ob = SelectObject(hdc, trBr);
    HGDIOBJ op = SelectObject(hdc, trPen);
    RoundRect(hdc, trackRc.left, trackRc.top, trackRc.right, trackRc.bottom, trackH, trackH);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(trPen);
    DeleteObject(trBr);

    int pad = DpiScale(3, dpi);
    int knobD = trackH - pad * 2;
    int knobX = checked ? (trackRc.right - pad - knobD) : (trackRc.left + pad);
    int knobY = trackRc.top + pad;
    COLORREF knobCol = checked ? pal.textOnAccent : (disabled ? pal.border : pal.textSecondary);
    HBRUSH knBr = CreateSolidBrush(knobCol);
    HPEN knPen = CreatePen(PS_SOLID, 1, knobCol);
    ob = SelectObject(hdc, knBr);
    op = SelectObject(hdc, knPen);
    Ellipse(hdc, knobX, knobY, knobX + knobD, knobY + knobD);
    SelectObject(hdc, op);
    SelectObject(hdc, ob);
    DeleteObject(knPen);
    DeleteObject(knBr);

    wchar_t text[256]{};
    GetWindowTextW(dis->hwndItem, text, _countof(text));
    if (text[0] != L'\0') {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, disabled ? pal.textSecondary : pal.textPrimary);
        RECT textRc{ trackRc.right + DpiScale(10, dpi), rc.top, rc.right - DpiScale(2, dpi), rc.bottom };
        DrawTextW(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    if (focused) {
        RECT focRc = rc;
        InflateRect(&focRc, -1, -1);
        DrawFocusRect(hdc, &focRc);
    }
}

HWND CreateTooltipWindow(HWND owner) {
    HWND tt = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                              WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                              owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (tt) {
        SendMessageW(tt, TTM_SETMAXTIPWIDTH, 0, 320);
        SendMessageW(tt, TTM_SETDELAYTIME, TTDT_INITIAL, 350);
    }
    return tt;
}

void SetControlTooltip(HWND tooltipWnd, HWND owner, HWND ctrl, const wchar_t* text) {
    if (!tooltipWnd || !owner || !ctrl || !text) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = reinterpret_cast<UINT_PTR>(ctrl);
    ti.lpszText = const_cast<LPWSTR>(text);
    SendMessageW(tooltipWnd, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(tooltipWnd, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

} // namespace s2p::ui
