#include "PowerPolicy.h"
#include "../common/Logger.h"

#include <powrprof.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>

#pragma comment(lib, "powrprof.lib")

namespace s2p {

namespace {

constexpr GUID kSubgroupButtons = {
    0x4f971e89, 0xeebd, 0x4455, { 0xa8, 0xde, 0x9e, 0x59, 0x04, 0x0e, 0x73, 0x47 }
};
constexpr GUID kLidCloseAction = {
    0x5ca83367, 0x6e45, 0x459f, { 0xa2, 0x7b, 0x47, 0x6b, 0x1d, 0x01, 0xc9, 0x36 }
};

constexpr const wchar_t* kPowerSection = L"PowerRestore";
constexpr const wchar_t* kDimClassName = L"Screen2PCSurfaceDimWnd";

bool ContainsIgnoreCase(const wchar_t* haystack, const wchar_t* needle) {
    if (!haystack || !needle || !*needle) return false;
    std::wstring h(haystack);
    std::wstring n(needle);
    for (wchar_t& ch : h) ch = static_cast<wchar_t>(std::towlower(ch));
    for (wchar_t& ch : n) ch = static_cast<wchar_t>(std::towlower(ch));
    return h.find(n) != std::wstring::npos;
}

bool RectsMatchClose(const RECT& a, const RECT& b) {
    return std::abs(a.left - b.left) <= 2 &&
           std::abs(a.top - b.top) <= 2 &&
           std::abs(a.right - b.right) <= 2 &&
           std::abs(a.bottom - b.bottom) <= 2;
}

struct EnumPhysicalParams {
    const std::vector<RECT>* excludeRects = nullptr;
    std::vector<RECT> physicalRects;
};

BOOL CALLBACK EnumPhysicalMonitorCb(HMONITOR hMon, HDC, LPRECT, LPARAM lp) {
    auto* params = reinterpret_cast<EnumPhysicalParams*>(lp);
    if (!params) return TRUE;

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) {
        return TRUE;
    }

    if (params->excludeRects) {
        for (const auto& er : *params->excludeRects) {
            if (RectsMatchClose(mi.rcMonitor, er)) {
                return TRUE;
            }
        }
    }

    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    if (EnumDisplayDevicesW(mi.szDevice, 0, &dd, 0)) {
        if (ContainsIgnoreCase(dd.DeviceString, L"Screen2PC") ||
            ContainsIgnoreCase(dd.DeviceID, L"Screen2PC")) {
            return TRUE;
        }
    }
    DISPLAY_DEVICEW adapterDd{};
    adapterDd.cb = sizeof(adapterDd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &adapterDd, 0); ++i) {
        if (_wcsicmp(adapterDd.DeviceName, mi.szDevice) == 0) {
            if (ContainsIgnoreCase(adapterDd.DeviceString, L"Screen2PC") ||
                ContainsIgnoreCase(adapterDd.DeviceID, L"Screen2PC")) {
                return TRUE;
            }
            break;
        }
    }

    params->physicalRects.push_back(mi.rcMonitor);
    return TRUE;
}

LRESULT CALLBACK StaticDimWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SurfaceDimOverlay* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<SurfaceDimOverlay*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SurfaceDimOverlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->HandleWndProc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void RegisterDimClassOnce() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = StaticDimWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kDimClassName;
    RegisterClassExW(&wc);
    registered = true;
}

} // namespace

// ============================================================================
// LidCloseGuard
// ============================================================================

LidCloseGuard::~LidCloseGuard() {
    std::wstring iniCopy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_engaged) return;
        iniCopy = m_activeIniPath;
    }
    Restore(iniCopy);
}

std::wstring LidCloseGuard::FormatGuidString(const GUID& g) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               static_cast<unsigned long>(g.Data1),
               static_cast<unsigned int>(g.Data2),
               static_cast<unsigned int>(g.Data3),
               g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
               g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

bool LidCloseGuard::ParseGuidString(const std::wstring& s, GUID& out) {
    if (s.empty()) return false;
    unsigned long d1 = 0;
    unsigned int d2 = 0, d3 = 0;
    unsigned int b[8]{};
    const wchar_t* p = s.c_str();
    int parsed = swscanf_s(
        p, L"{%8lX-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X}",
        &d1, &d2, &d3,
        &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
    if (parsed != 11) {
        parsed = swscanf_s(
            p, L"%8lX-%4X-%4X-%2X%2X-%2X%2X%2X%2X%2X%2X",
            &d1, &d2, &d3,
            &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]);
    }
    if (parsed != 11) return false;
    out.Data1 = static_cast<uint32_t>(d1);
    out.Data2 = static_cast<uint16_t>(d2);
    out.Data3 = static_cast<uint16_t>(d3);
    for (int i = 0; i < 8; ++i) {
        out.Data4[i] = static_cast<uint8_t>(b[i] & 0xFFu);
    }
    return true;
}

bool LidCloseGuard::SavePowerRestoreIni(const std::wstring& iniPath, const PowerRestoreState& st) {
    if (iniPath.empty()) return false;
    std::wstring guidStr = FormatGuidString(st.schemeGuid);
    bool ok = true;
    ok &= WritePrivateProfileStringW(kPowerSection, L"SchemeGuid", guidStr.c_str(), iniPath.c_str()) != FALSE;
    ok &= WritePrivateProfileStringW(kPowerSection, L"SavedAc", std::to_wstring(st.savedAc).c_str(), iniPath.c_str()) != FALSE;
    ok &= WritePrivateProfileStringW(kPowerSection, L"SavedDc", std::to_wstring(st.savedDc).c_str(), iniPath.c_str()) != FALSE;
    ok &= WritePrivateProfileStringW(kPowerSection, L"Pending", st.pending ? L"1" : L"0", iniPath.c_str()) != FALSE;
    return ok;
}

bool LidCloseGuard::LoadPowerRestoreIni(const std::wstring& iniPath, PowerRestoreState& out) {
    out = PowerRestoreState{};
    if (iniPath.empty()) return false;
    int pending = GetPrivateProfileIntW(kPowerSection, L"Pending", 0, iniPath.c_str());
    if (pending == 0) return false;

    wchar_t guidBuf[80]{};
    GetPrivateProfileStringW(kPowerSection, L"SchemeGuid", L"", guidBuf, _countof(guidBuf), iniPath.c_str());
    if (!ParseGuidString(guidBuf, out.schemeGuid)) {
        return false;
    }
    out.savedAc = static_cast<uint32_t>(GetPrivateProfileIntW(kPowerSection, L"SavedAc", 1, iniPath.c_str()));
    out.savedDc = static_cast<uint32_t>(GetPrivateProfileIntW(kPowerSection, L"SavedDc", 1, iniPath.c_str()));
    out.pending = true;
    return true;
}

void LidCloseGuard::ClearPowerRestoreIni(const std::wstring& iniPath) {
    if (iniPath.empty()) return;
    WritePrivateProfileStringW(kPowerSection, nullptr, nullptr, iniPath.c_str());
}

bool LidCloseGuard::ApplyPowerValues(const GUID& schemeGuid, uint32_t acVal, uint32_t dcVal) {
    DWORD rcAc = PowerWriteACValueIndex(nullptr, &schemeGuid, &kSubgroupButtons, &kLidCloseAction, acVal);
    DWORD rcDc = PowerWriteDCValueIndex(nullptr, &schemeGuid, &kSubgroupButtons, &kLidCloseAction, dcVal);
    DWORD rcSet = PowerSetActiveScheme(nullptr, &schemeGuid);
    return (rcAc == ERROR_SUCCESS || rcDc == ERROR_SUCCESS) && (rcSet == ERROR_SUCCESS);
}

bool LidCloseGuard::RecoverStaleStateOnStartup(const std::wstring& iniPath) {
    PowerRestoreState st{};
    if (!LoadPowerRestoreIni(iniPath, st)) {
        return false;
    }
    bool restored = ApplyPowerValues(st.schemeGuid, st.savedAc, st.savedDc);
    ClearPowerRestoreIni(iniPath);
    Logger::Instance().Info("PWR-REC",
        L"Recovered stale lid-close power policy from previous session (AC=" +
        std::to_wstring(st.savedAc) + L", DC=" + std::to_wstring(st.savedDc) +
        L", ok=" + (restored ? L"1" : L"0") + L")");
    return restored;
}

bool LidCloseGuard::Engage(const std::wstring& iniPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_engaged) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
        return true;
    }

    GUID* activeScheme = nullptr;
    if (PowerGetActiveScheme(nullptr, &activeScheme) != ERROR_SUCCESS || !activeScheme) {
        Logger::Instance().Warn("PWR-LID", L"PowerGetActiveScheme failed.");
        return false;
    }

    PowerRestoreState state{};
    state.schemeGuid = *activeScheme;
    LocalFree(activeScheme);

    DWORD acVal = 1;
    DWORD dcVal = 1;
    DWORD rcAc = PowerReadACValueIndex(nullptr, &state.schemeGuid, &kSubgroupButtons, &kLidCloseAction, &acVal);
    DWORD rcDc = PowerReadDCValueIndex(nullptr, &state.schemeGuid, &kSubgroupButtons, &kLidCloseAction, &dcVal);
    if (rcAc != ERROR_SUCCESS && rcDc != ERROR_SUCCESS) {
        Logger::Instance().Warn("PWR-LID", L"Failed reading current lid-close policy values.");
        return false;
    }

    state.savedAc = acVal;
    state.savedDc = dcVal;
    state.pending = true;

    // Persist original values BEFORE modifying Windows power scheme (crash-safe)
    if (!iniPath.empty() && !SavePowerRestoreIni(iniPath, state)) {
        Logger::Instance().Warn("PWR-LID", L"Could not persist [PowerRestore] to sender.ini.");
        return false;
    }

    if (!ApplyPowerValues(state.schemeGuid, 0, 0)) {
        ClearPowerRestoreIni(iniPath);
        Logger::Instance().Warn("PWR-LID", L"Failed applying lid-close Do-Nothing override.");
        return false;
    }

    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    m_saved = state;
    m_activeIniPath = iniPath;
    m_engaged = true;
    Logger::Instance().Info("PWR-LID",
        L"Lid-close sleep guard engaged (saved AC=" + std::to_wstring(state.savedAc) +
        L", DC=" + std::to_wstring(state.savedDc) + L").");
    return true;
}

bool LidCloseGuard::Restore(const std::wstring& iniPath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    SetThreadExecutionState(ES_CONTINUOUS);

    const std::wstring path = !iniPath.empty() ? iniPath : m_activeIniPath;
    if (!m_engaged) {
        PowerRestoreState stale{};
        if (LoadPowerRestoreIni(path, stale)) {
            bool ok = ApplyPowerValues(stale.schemeGuid, stale.savedAc, stale.savedDc);
            ClearPowerRestoreIni(path);
            return ok;
        }
        return true;
    }

    bool ok = ApplyPowerValues(m_saved.schemeGuid, m_saved.savedAc, m_saved.savedDc);
    ClearPowerRestoreIni(path);
    m_engaged = false;
    m_activeIniPath.clear();
    Logger::Instance().Info("PWR-LID",
        L"Lid-close sleep guard restored (AC=" + std::to_wstring(m_saved.savedAc) +
        L", DC=" + std::to_wstring(m_saved.savedDc) + L", ok=" + (ok ? L"1" : L"0") + L").");
    return ok;
}

bool LidCloseGuard::IsEngaged() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_engaged;
}

// ============================================================================
// SurfaceDimOverlay
// ============================================================================

SurfaceDimOverlay::~SurfaceDimOverlay() {
    Hide(false);
}

bool SurfaceDimOverlay::Show(const std::vector<RECT>& virtualExcludeRects, DismissCallback onDismiss) {
    Hide(false);
    RegisterDimClassOnce();
    m_onDismiss = std::move(onDismiss);
    m_dismissing = false;

    EnumPhysicalParams params{};
    params.excludeRects = &virtualExcludeRects;
    EnumDisplayMonitors(nullptr, nullptr, EnumPhysicalMonitorCb, reinterpret_cast<LPARAM>(&params));

    if (params.physicalRects.empty()) {
        Logger::Instance().Warn("PWR-DIM", L"No physical Laptop monitor found to dim.");
        return false;
    }

    for (const auto& rc : params.physicalRects) {
        int w = std::max<int>(1, rc.right - rc.left);
        int h = std::max<int>(1, rc.bottom - rc.top);
        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            kDimClassName,
            L"Screen2PC Laptop Dim",
            WS_POPUP | WS_VISIBLE,
            rc.left, rc.top, w, h,
            nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (hwnd) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            UpdateWindow(hwnd);
            m_hwnds.push_back(hwnd);
        }
    }
    return !m_hwnds.empty();
}

void SurfaceDimOverlay::Hide(bool invokeCallback) {
    if (m_dismissing) return;
    m_dismissing = true;

    std::vector<HWND> toDestroy;
    toDestroy.swap(m_hwnds);
    for (HWND hwnd : toDestroy) {
        if (hwnd && IsWindow(hwnd)) {
            DestroyWindow(hwnd);
        }
    }

    DismissCallback cb = std::move(m_onDismiss);
    m_onDismiss = nullptr;
    m_dismissing = false;

    if (invokeCallback && cb) {
        cb();
    }
}

bool SurfaceDimOverlay::IsVisible() const {
    for (HWND hwnd : m_hwnds) {
        if (hwnd && IsWindow(hwnd)) return true;
    }
    return false;
}

LRESULT SurfaceDimOverlay::HandleWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(45, 45, 45));
        const wchar_t* hint = L"Screen2PC \u2014 Laptop-Display abgedunkelt (Doppelklick oder ESC zum Reaktivieren)";
        DrawTextW(hdc, hint, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        Hide(true);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            Hide(true);
            return 0;
        }
        break;
    case WM_DESTROY:
        m_hwnds.erase(std::remove(m_hwnds.begin(), m_hwnds.end(), hwnd), m_hwnds.end());
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace s2p
