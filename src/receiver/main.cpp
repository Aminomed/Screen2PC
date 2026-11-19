#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <objbase.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../common/AppIcon.h"
#include "../common/Autostart.h"
#include "../common/ColorProfile.h"
#include "../common/KvmInput.h"
#include "../common/Logger.h"
#include "../common/NetSocket.h"
#include "../common/Pairing.h"
#include "../common/UiTheme.h"
#include "../common/Updater.h"
#include "ReceiverEngine.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_TRAYICON          = WM_APP + 10;
constexpr UINT WM_UPDATE_CHECK_DONE = WM_APP + 11;
constexpr UINT WM_UPDATE_DL_DONE    = WM_APP + 12;
constexpr UINT WM_DISCONNECT_DONE   = WM_APP + 13;
constexpr UINT WM_PIN_SHOW          = WM_APP + 14;
constexpr UINT WM_PIN_DISMISS       = WM_APP + 15;
constexpr UINT WM_VERSION_MISMATCH  = WM_APP + 16;

constexpr UINT_PTR TIMER_TELEMETRY  = 1;

constexpr int IDC_INFO_LABEL        = 201;
constexpr int IDC_CHK_FULLSCR       = 202;
constexpr int IDC_BTN_DISCONN       = 203;
constexpr int IDC_STATS_TEXT        = 204;
constexpr int IDC_BTN_FIREWALL      = 205;
constexpr int IDC_CHK_CLIPBOARD     = 206;
constexpr int IDC_BTN_SENDFILE      = 207;
constexpr int IDC_BTN_OPEN_DL       = 208;
constexpr int IDC_CHK_KVM           = 209;
constexpr int IDC_CHK_AUTOSTART     = 210;
constexpr int IDC_CHK_AUTOUPDATE    = 211;
constexpr int IDC_BTN_COLORS        = 212;
constexpr int IDC_BTN_UPDATE        = 213;
constexpr int IDC_BTN_SENDFOLDER    = 214;
constexpr int IDC_FILE_PROGRESS     = 215;
constexpr int IDC_BTN_CANCELFILE    = 216;
constexpr int IDC_FILE_STATUS       = 217;
constexpr int IDC_BTN_SHOW_STREAMS  = 218;
constexpr int IDC_BTN_TRUSTED       = 219;
constexpr int IDC_LBL_MOUSE_SPEED   = 220;
constexpr int IDC_TB_MOUSE_SPEED    = 221;
constexpr int IDC_VAL_MOUSE_SPEED   = 222;
constexpr int IDC_LBL_SCROLL_SPEED  = 223;
constexpr int IDC_TB_SCROLL_SPEED   = 224;
constexpr int IDC_VAL_SCROLL_SPEED  = 225;
constexpr int IDC_HERO_TITLE        = 226;
constexpr int IDC_LBL_TELEMETRY     = 227;
constexpr int IDC_DROP_TITLE        = 228;
constexpr int IDC_DROP_SUB          = 229;
constexpr int IDC_CHK_STREAM_HUD    = 230;

constexpr int IDC_TAB_OVERVIEW      = 240;
constexpr int IDC_TAB_TRANSFER      = 241;
constexpr int IDC_TAB_SETTINGS      = 242;

constexpr int IDM_SHOW              = 3001;
constexpr int IDM_EXIT              = 3002;
constexpr int IDM_CHECK_UPDATE      = 3003;
constexpr int IDM_SHOW_STREAMS      = 3004;

// Color calibration window control IDs
constexpr int IDC_CLR_LBL_MON       = 400;
constexpr int IDC_CLR_MON_COMBO     = 401;
constexpr int IDC_CLR_HDR_INFO      = 402;
constexpr int IDC_CLR_CHK_HDR       = 403;
constexpr int IDC_CLR_LBL_BASE      = 404; // 404..409 used by slider labels if needed; we use 430+ for safety
constexpr int IDC_CLR_TB_BRIGHT     = 410;
constexpr int IDC_CLR_TB_GAMMA      = 411;
constexpr int IDC_CLR_TB_CONTRAST   = 412;
constexpr int IDC_CLR_TB_SAT        = 413;
constexpr int IDC_CLR_TB_RED        = 414;
constexpr int IDC_CLR_TB_GREEN      = 415;
constexpr int IDC_CLR_TB_BLUE       = 416;
constexpr int IDC_CLR_VAL_BRIGHT    = 420;
constexpr int IDC_CLR_VAL_GAMMA     = 421;
constexpr int IDC_CLR_VAL_CONTRAST  = 422;
constexpr int IDC_CLR_VAL_SAT       = 423;
constexpr int IDC_CLR_VAL_RED       = 424;
constexpr int IDC_CLR_VAL_GREEN     = 425;
constexpr int IDC_CLR_VAL_BLUE      = 426;
constexpr int IDC_CLR_BTN_RESET     = 430;
constexpr int IDC_CLR_BTN_CLOSE     = 431;
constexpr int IDC_CLR_NAME_BASE     = 440; // 440..446 slider caption statics

// PIN Popup window control IDs
constexpr int IDC_PINP_TITLE        = 601;
constexpr int IDC_PINP_INFO         = 602;
constexpr int IDC_PINP_CODE         = 603;
constexpr int IDC_PINP_CHK_REMEMBER = 604;
constexpr int IDC_PINP_BTN_REJECT   = 605;

// Trusted devices window control IDs
constexpr int IDC_TR_INFO           = 701;
constexpr int IDC_TR_LIST           = 702;
constexpr int IDC_TR_BTN_REMOVE     = 703;
constexpr int IDC_TR_BTN_CLEAR      = 704;
constexpr int IDC_TR_BTN_REFRESH    = 705;
constexpr int IDC_TR_BTN_CLOSE      = 706;

constexpr int kMainBaseW    = 580;
constexpr int kMainBaseH    = 698;
constexpr int kColorBaseW   = 472;
constexpr int kColorBaseH   = 422;
constexpr int kPinPopBaseW  = 430;
constexpr int kPinPopBaseH  = 258;
constexpr int kTrustedBaseW = 590;
constexpr int kTrustedBaseH = 380;

struct UpdateCheckResult {
    s2p::UpdateReleaseInfo info;
    bool userInitiated = false;
};

struct UpdateDownloadResult {
    bool ok = false;
    std::wstring error;
};

struct VersionMismatchNotice {
    uint16_t remoteProtoVer = 0;
    s2p::AppVersionTuple remoteAppVer{};
    std::wstring message;
};

s2p::ReceiverEngine* g_engine = nullptr;
HWND g_hwnd = nullptr;
HWND g_colorWnd = nullptr;
HWND g_pinPopupWnd = nullptr;
HWND g_trustedWnd = nullptr;
HWND g_tooltip = nullptr;

HFONT g_font = nullptr;
HFONT g_boldFont = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_monoFont = nullptr;
HFONT g_colorFont = nullptr;
HFONT g_pinPopFont = nullptr;
HFONT g_pinPopBigFont = nullptr;
HFONT g_trustedFont = nullptr;
UINT g_currentDpi = 96;

s2p::ui::ThemePalette g_theme;
s2p::ui::StatusTone g_heroTone = s2p::ui::StatusTone::Neutral;
int g_activeTab = 0; // 0 = Uebersicht, 1 = Dateitransfer, 2 = Einstellungen

NOTIFYICONDATAW g_nid{};
UINT g_wmShowExisting = 0;
UINT g_wmTaskbarCreated = 0;

std::thread g_disconnThread;
std::thread g_updateThread;
std::atomic<bool> g_closing{false};
std::atomic<bool> g_disconnBusy{false};
std::atomic<bool> g_updateBusy{false};
bool g_promptOpen = false;
bool g_verPromptOpen = false;
ULONGLONG g_lastVerPromptTick = 0;
s2p::UpdateReleaseInfo g_pendingUpdate{};

uint32_t g_selectedColorMonitor = UINT32_MAX;
bool g_colorPopulating = false;

std::shared_ptr<s2p::PinPopupSession> g_activePinSession;
std::vector<s2p::TrustedDeviceEntry> g_trustedCache;
uint32_t g_lastNotifiedBatchId = 0;

bool IsToggleChecked(HWND hwnd, int id) {
    HWND c = GetDlgItem(hwnd, id);
    return c && (GetWindowLongPtrW(c, GWLP_USERDATA) != 0);
}

void SetToggleChecked(HWND hwnd, int id, bool checked) {
    HWND c = GetDlgItem(hwnd, id);
    if (!c) return;
    SetWindowLongPtrW(c, GWLP_USERDATA, checked ? 1 : 0);
    InvalidateRect(c, nullptr, FALSE);
}

bool ToggleSwitch(HWND hwnd, int id) {
    bool next = !IsToggleChecked(hwnd, id);
    SetToggleChecked(hwnd, id, next);
    return next;
}

bool IsReceiverCardControlId(int id) {
    switch (id) {
    case IDC_HERO_TITLE:
    case IDC_INFO_LABEL:
    case IDC_LBL_MOUSE_SPEED:
    case IDC_VAL_MOUSE_SPEED:
    case IDC_LBL_SCROLL_SPEED:
    case IDC_VAL_SCROLL_SPEED:
    case IDC_TB_MOUSE_SPEED:
    case IDC_TB_SCROLL_SPEED:
    case IDC_LBL_TELEMETRY:
    case IDC_DROP_TITLE:
    case IDC_DROP_SUB:
    case IDC_FILE_STATUS:
        return true;
    default:
        return false;
    }
}

void ApplyReceiverTabVisibility(HWND hwnd) {
    auto vis = [&](int id, bool show) {
        HWND c = GetDlgItem(hwnd, id);
        if (c) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
    };
    const bool t0 = (g_activeTab == 0);
    const bool t1 = (g_activeTab == 1);
    const bool t2 = (g_activeTab == 2);

    vis(IDC_CHK_FULLSCR, t0);
    vis(IDC_CHK_CLIPBOARD, t0);
    vis(IDC_CHK_KVM, t0);
    vis(IDC_LBL_MOUSE_SPEED, t0);
    vis(IDC_TB_MOUSE_SPEED, t0);
    vis(IDC_VAL_MOUSE_SPEED, t0);
    vis(IDC_LBL_SCROLL_SPEED, t0);
    vis(IDC_TB_SCROLL_SPEED, t0);
    vis(IDC_VAL_SCROLL_SPEED, t0);
    vis(IDC_LBL_TELEMETRY, t0);
    vis(IDC_STATS_TEXT, t0);

    vis(IDC_DROP_TITLE, t1);
    vis(IDC_DROP_SUB, t1);
    vis(IDC_BTN_SENDFILE, t1);
    vis(IDC_BTN_SENDFOLDER, t1);
    vis(IDC_BTN_OPEN_DL, t1);
    vis(IDC_FILE_PROGRESS, t1);
    vis(IDC_BTN_CANCELFILE, t1);
    vis(IDC_FILE_STATUS, t1);

    vis(IDC_CHK_AUTOSTART, t2);
    vis(IDC_CHK_AUTOUPDATE, t2);
    vis(IDC_CHK_STREAM_HUD, t2);
    vis(IDC_BTN_COLORS, t2);
    vis(IDC_BTN_TRUSTED, t2);
    vis(IDC_BTN_FIREWALL, t2);
    vis(IDC_BTN_UPDATE, t2);

    InvalidateRect(GetDlgItem(hwnd, IDC_TAB_OVERVIEW), nullptr, FALSE);
    InvalidateRect(GetDlgItem(hwnd, IDC_TAB_TRANSFER), nullptr, FALSE);
    InvalidateRect(GetDlgItem(hwnd, IDC_TAB_SETTINGS), nullptr, FALSE);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void ApplyReceiverTheme(HWND hwnd, bool dark) {
    g_theme.Init(dark);
    s2p::ui::ApplyWindowTheme(hwnd, dark);
    s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_STATS_TEXT), dark);
    if (g_colorWnd && IsWindow(g_colorWnd)) s2p::ui::ApplyWindowTheme(g_colorWnd, dark);
    if (g_pinPopupWnd && IsWindow(g_pinPopupWnd)) s2p::ui::ApplyWindowTheme(g_pinPopupWnd, dark);
    if (g_trustedWnd && IsWindow(g_trustedWnd)) s2p::ui::ApplyWindowTheme(g_trustedWnd, dark);
    InvalidateRect(hwnd, nullptr, TRUE);
}

int DpiScale(int px96, UINT dpi) {
    return MulDiv(px96, static_cast<int>(dpi), 96);
}

UINT GetWindowDpiSafe(HWND hwnd) {
    if (hwnd) {
        UINT d = GetDpiForWindow(hwnd);
        if (d >= 72) return d;
    }
    return 96;
}

std::wstring GetReceiverIniPath() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) + L"\\Screen2PC" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\receiver.ini";
}

bool LoadBoolPref(const wchar_t* key, int defVal) {
    return GetPrivateProfileIntW(L"Receiver", key, defVal, GetReceiverIniPath().c_str()) != 0;
}

void SaveBoolPref(const wchar_t* key, bool val) {
    WritePrivateProfileStringW(L"Receiver", key, val ? L"1" : L"0", GetReceiverIniPath().c_str());
}

uint32_t LoadUintPref(const wchar_t* key, uint32_t defVal) {
    return static_cast<uint32_t>(
        GetPrivateProfileIntW(L"Receiver", key, static_cast<int>(defVal), GetReceiverIniPath().c_str()));
}

void SaveUintPref(const wchar_t* key, uint32_t val) {
    WritePrivateProfileStringW(L"Receiver", key, std::to_wstring(val).c_str(), GetReceiverIniPath().c_str());
}

std::wstring DefaultUpdateButtonText() {
    if (g_pendingUpdate.valid && g_pendingUpdate.updateAvailable) {
        return L"Update verfuegbar: v" + s2p::FormatAppVersion(g_pendingUpdate.version);
    }
    return L"Nach Updates suchen (v" + s2p::FormatAppVersion(s2p::CurrentAppVersion()) + L")";
}

void AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = s2p::LoadAppSmallIcon(GetModuleHandleW(nullptr));
    wcscpy_s(g_nid.szTip, L"Screen2PC Receiver (PC)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// ============================================================================
// Color & HDR Calibration Window
// ============================================================================

void UpdateSliderValueLabel(HWND colorHwnd, int tbId, int valPos) {
    int lblId = tbId + 10; // 410..416 -> 420..426
    std::wostringstream ss;
    if (tbId == IDC_CLR_TB_GAMMA) {
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(valPos) / 100.0);
    } else {
        ss << valPos << L" %";
    }
    SetDlgItemTextW(colorHwnd, lblId, ss.str().c_str());
}

std::wstring FormatColorMonitorLabel(uint32_t idx, const s2p::ReceiverStatus& st) {
    std::wostringstream ss;
    ss << L"Monitor #" << (idx + 1);
    if (idx < st.localMonitors.size()) {
        const auto& m = st.localMonitors[idx];
        ss << L" (" << m.width << L"x" << m.height << L"@" << m.refreshHz << L"Hz)";
    }
    if (idx < st.localMonitorHdr.size() && st.localMonitorHdr[idx].isHdr) {
        int nits = static_cast<int>(std::lround(st.localMonitorHdr[idx].sdrWhiteScale * 80.0f));
        ss << L" [HDR - " << nits << L" Nits]";
    } else {
        ss << L" [SDR]";
    }
    return ss.str();
}

void PopulateColorDialog() {
    if (!g_colorWnd || !g_engine) return;
    g_colorPopulating = true;

    auto st = g_engine->GetStatus();
    uint32_t monCount = static_cast<uint32_t>(std::max<size_t>(1, std::min<size_t>(st.localMonitors.size(), s2p::kMaxMonitors)));

    if (g_selectedColorMonitor == UINT32_MAX || g_selectedColorMonitor >= monCount) {
        g_selectedColorMonitor = 0;
        for (uint32_t i = 0; i < st.localMonitorHdr.size() && i < monCount; ++i) {
            if (st.localMonitorHdr[i].isHdr) {
                g_selectedColorMonitor = i;
                break;
            }
        }
    }

    HWND cb = GetDlgItem(g_colorWnd, IDC_CLR_MON_COMBO);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    for (uint32_t i = 0; i < monCount; ++i) {
        std::wstring lbl = FormatColorMonitorLabel(i, st);
        SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(lbl.c_str()));
    }
    SendMessageW(cb, CB_SETCURSEL, g_selectedColorMonitor, 0);

    bool isHdrMon = (g_selectedColorMonitor < st.localMonitorHdr.size()) && st.localMonitorHdr[g_selectedColorMonitor].isHdr;
    std::wostringstream info;
    if (isHdrMon) {
        int nits = static_cast<int>(std::lround(st.localMonitorHdr[g_selectedColorMonitor].sdrWhiteScale * 80.0f));
        info << L"Erkannt: Windows HDR aktiv (SDR-Weisslevel: " << nits
             << L" Nits). scRGB 16-Bit + Gamma 2.2 gleicht Farben exakt an SDR-Monitore an.";
    } else {
        info << L"Erkannt: Standard-SDR-Monitor. Empfehlung: Standardwerte belassen oder dezent nachjustieren.";
    }
    SetDlgItemTextW(g_colorWnd, IDC_CLR_HDR_INFO, info.str().c_str());

    s2p::MonitorColorConfig cfg = g_engine->GetMonitorColorConfig(g_selectedColorMonitor);
    s2p::ClampColorConfig(cfg);

    SendDlgItemMessageW(g_colorWnd, IDC_CLR_CHK_HDR, BM_SETCHECK,
                        cfg.hdrOutputEnabled ? BST_CHECKED : BST_UNCHECKED, 0);

    auto setTb = [&](int tbId, int val) {
        SendDlgItemMessageW(g_colorWnd, tbId, TBM_SETPOS, TRUE, val);
        UpdateSliderValueLabel(g_colorWnd, tbId, val);
    };
    setTb(IDC_CLR_TB_BRIGHT,   static_cast<int>(std::lround(cfg.brightnessPct)));
    setTb(IDC_CLR_TB_GAMMA,    static_cast<int>(std::lround(cfg.gamma * 100.0f)));
    setTb(IDC_CLR_TB_CONTRAST, static_cast<int>(std::lround(cfg.contrastPct)));
    setTb(IDC_CLR_TB_SAT,      static_cast<int>(std::lround(cfg.saturationPct)));
    setTb(IDC_CLR_TB_RED,      static_cast<int>(std::lround(cfg.redGainPct)));
    setTb(IDC_CLR_TB_GREEN,    static_cast<int>(std::lround(cfg.greenGainPct)));
    setTb(IDC_CLR_TB_BLUE,     static_cast<int>(std::lround(cfg.blueGainPct)));

    g_colorPopulating = false;
}

s2p::MonitorColorConfig ReadColorDialogValues() {
    s2p::MonitorColorConfig cfg{};
    if (!g_colorWnd) return cfg;
    cfg.hdrOutputEnabled = SendDlgItemMessageW(g_colorWnd, IDC_CLR_CHK_HDR, BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.brightnessPct    = static_cast<float>(SendDlgItemMessageW(g_colorWnd, IDC_CLR_TB_BRIGHT, TBM_GETPOS, 0, 0));
    cfg.gamma            = static_cast<float>(SendDlgItemMessageW(g_colorWnd, IDC_CLR_TB_GAMMA, TBM_GETPOS, 0, 0)) / 100.0f;
    cfg.contrastPct      = static_cast<float>(SendDlgItemMessageW(g_colorWnd, IDC_CLR_TB_CONTRAST, TBM_GETPOS, 0, 0));
    cfg.saturationPct    = static_cast<float>(SendDlgItemMessageW(g_colorWnd, IDC_CLR_TB_SAT, TBM_GETPOS, 0, 0));
    cfg.redGainPct       = static_cast<float>(SendDlgItemMessageW(g_colorWnd, IDC_CLR_TB_RED, TBM_GETPOS, 0, 0));
    cfg.greenGainPct     = static_cast<float>(SendDlgItemMessageW(g_colorWnd, IDC_CLR_TB_GREEN, TBM_GETPOS, 0, 0));
    cfg.blueGainPct      = static_cast<float>(SendDlgItemMessageW(g_colorWnd, IDC_CLR_TB_BLUE, TBM_GETPOS, 0, 0));
    s2p::ClampColorConfig(cfg);
    return cfg;
}

void LayoutColorControls(HWND hwnd, UINT dpi) {
    if (g_colorFont) DeleteObject(g_colorFont);
    g_colorFont = CreateFontW(-DpiScale(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    auto mv = [&](int id, int x, int y, int w, int h) {
        HWND c = GetDlgItem(hwnd, id);
        if (!c) return;
        MoveWindow(c, DpiScale(x, dpi), DpiScale(y, dpi), DpiScale(w, dpi), DpiScale(h, dpi), TRUE);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_colorFont), TRUE);
    };

    mv(IDC_CLR_LBL_MON,   16, 16,  90,  20);
    mv(IDC_CLR_MON_COMBO,112, 12, 344, 160);
    mv(IDC_CLR_HDR_INFO,  16, 44, 440,  34);
    mv(IDC_CLR_CHK_HDR,   16, 84, 440,  22);

    int y = 116;
    for (int i = 0; i < 7; ++i) {
        mv(IDC_CLR_NAME_BASE + i, 16,  y + 4, 108, 20);
        mv(IDC_CLR_TB_BRIGHT + i, 126, y,     260, 28);
        mv(IDC_CLR_VAL_BRIGHT + i,392, y + 4,  62, 20);
        y += 34;
    }

    mv(IDC_CLR_BTN_RESET, 16,  y + 10, 240, 30);
    mv(IDC_CLR_BTN_CLOSE, 336, y + 10, 120, 30);
}

LRESULT CALLBACK ColorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 10, 10, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
        };

        addCtrl(L"STATIC", L"PC-Monitor:", 0, IDC_CLR_LBL_MON);
        addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_CLR_MON_COMBO);
        addCtrl(L"STATIC", L"", 0, IDC_CLR_HDR_INFO);

        addCtrl(L"BUTTON", L"HDR scRGB-Ausgabe && Gamma-2.2-Korrektur (empfohlen fuer HDR)",
                BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CLR_CHK_HDR);

        struct SliderRow {
            int tbId;
            int lblId;
            const wchar_t* name;
            int minV;
            int maxV;
            int ticFreq;
        };
        const SliderRow rows[] = {
            { IDC_CLR_TB_BRIGHT,   IDC_CLR_VAL_BRIGHT,   L"Helligkeit:",  50, 200, 15 },
            { IDC_CLR_TB_GAMMA,    IDC_CLR_VAL_GAMMA,    L"Gamma (EOTF):",160, 280, 10 },
            { IDC_CLR_TB_CONTRAST, IDC_CLR_VAL_CONTRAST, L"Kontrast:",    60, 140, 10 },
            { IDC_CLR_TB_SAT,      IDC_CLR_VAL_SAT,      L"Saettigung:",  50, 160, 10 },
            { IDC_CLR_TB_RED,      IDC_CLR_VAL_RED,      L"Rot-Anteil:",  70, 130, 10 },
            { IDC_CLR_TB_GREEN,    IDC_CLR_VAL_GREEN,    L"Gruen-Anteil:",70, 130, 10 },
            { IDC_CLR_TB_BLUE,     IDC_CLR_VAL_BLUE,     L"Blau-Anteil:", 70, 130, 10 },
        };

        for (int i = 0; i < 7; ++i) {
            const auto& r = rows[i];
            addCtrl(L"STATIC", r.name, 0, IDC_CLR_NAME_BASE + i);
            HWND tb = addCtrl(TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP, r.tbId);
            SendMessageW(tb, TBM_SETRANGE, FALSE, MAKELONG(r.minV, r.maxV));
            SendMessageW(tb, TBM_SETTICFREQ, r.ticFreq, 0);
            addCtrl(L"STATIC", L"100 %", SS_RIGHT, r.lblId);
        }

        addCtrl(L"BUTTON", L"Standardwerte wiederherstellen", BS_PUSHBUTTON | WS_TABSTOP, IDC_CLR_BTN_RESET);
        addCtrl(L"BUTTON", L"Schliessen", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_CLR_BTN_CLOSE);

        LayoutColorControls(hwnd, GetWindowDpiSafe(hwnd));
        s2p::ui::ApplyWindowTheme(hwnd, g_theme.dark);
        s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_CLR_MON_COMBO), g_theme.dark, L"DarkMode_CFD");
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (g_theme.brushWindow) {
            FillRect(reinterpret_cast<HDC>(wp), &rc, g_theme.brushWindow);
            return 1;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORDLG:
        return s2p::ui::HandleCtlColor(msg, wp, g_theme, false);

    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutColorControls(hwnd, HIWORD(wp));
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CLR_MON_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT idx = SendDlgItemMessageW(hwnd, IDC_CLR_MON_COMBO, CB_GETCURSEL, 0, 0);
                if (idx >= 0) {
                    g_selectedColorMonitor = static_cast<uint32_t>(idx);
                    PopulateColorDialog();
                }
            }
            return 0;
        case IDC_CLR_CHK_HDR:
            if (!g_colorPopulating && g_engine) {
                auto cfg = ReadColorDialogValues();
                g_engine->SetMonitorColorConfig(g_selectedColorMonitor, cfg);
                s2p::SaveMonitorColorConfigIni(GetReceiverIniPath(), g_selectedColorMonitor, cfg);
            }
            return 0;
        case IDC_CLR_BTN_RESET:
            if (g_engine) {
                s2p::MonitorColorConfig def{};
                g_engine->SetMonitorColorConfig(g_selectedColorMonitor, def);
                s2p::SaveMonitorColorConfigIni(GetReceiverIniPath(), g_selectedColorMonitor, def);
                PopulateColorDialog();
            }
            return 0;
        case IDC_CLR_BTN_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_HSCROLL: {
        if (g_colorPopulating || !g_engine || !lp) break;
        HWND tb = reinterpret_cast<HWND>(lp);
        int id = GetDlgCtrlID(tb);
        if (id >= IDC_CLR_TB_BRIGHT && id <= IDC_CLR_TB_BLUE) {
            int pos = static_cast<int>(SendMessageW(tb, TBM_GETPOS, 0, 0));
            UpdateSliderValueLabel(hwnd, id, pos);
            auto cfg = ReadColorDialogValues();
            g_engine->SetMonitorColorConfig(g_selectedColorMonitor, cfg);
            if (LOWORD(wp) != TB_THUMBTRACK) {
                s2p::SaveMonitorColorConfigIni(GetReceiverIniPath(), g_selectedColorMonitor, cfg);
            }
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_engine && g_selectedColorMonitor < s2p::kMaxMonitors) {
            s2p::SaveMonitorColorConfigIni(GetReceiverIniPath(), g_selectedColorMonitor, ReadColorDialogValues());
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_engine && g_selectedColorMonitor < s2p::kMaxMonitors) {
            s2p::SaveMonitorColorConfigIni(GetReceiverIniPath(), g_selectedColorMonitor,
                                           g_engine->GetMonitorColorConfig(g_selectedColorMonitor));
        }
        if (g_colorFont) { DeleteObject(g_colorFont); g_colorFont = nullptr; }
        g_colorWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenColorSettingsWindow(HWND parent) {
    if (g_engine) {
        g_engine->RefreshLocalMonitorInfo();
    }
    if (g_colorWnd && IsWindow(g_colorWnd)) {
        PopulateColorDialog();
        ShowWindow(g_colorWnd, SW_SHOW);
        SetForegroundWindow(g_colorWnd);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ColorWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = s2p::LoadAppLargeIcon();
        wc.hIconSm = s2p::LoadAppSmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Screen2PCColorWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    UINT dpi = GetWindowDpiSafe(parent);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT wr{ 0, 0, DpiScale(kColorBaseW, dpi), DpiScale(kColorBaseH, dpi) };
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, dpi);

    g_colorWnd = CreateWindowExW(
        0, L"Screen2PCColorWnd",
        L"Farbe & HDR pro Monitor (Treiberfrei / Echtzeit)",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (g_colorWnd) {
        PopulateColorDialog();
        ShowWindow(g_colorWnd, SW_SHOW);
        UpdateWindow(g_colorWnd);
    }
}

// ============================================================================
// Non-blocking PIN Popup Window on PC
// ============================================================================

void LayoutPinPopupControls(HWND hwnd, UINT dpi) {
    if (g_pinPopFont) DeleteObject(g_pinPopFont);
    if (g_pinPopBigFont) DeleteObject(g_pinPopBigFont);
    g_pinPopFont = CreateFontW(-DpiScale(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_pinPopBigFont = CreateFontW(-DpiScale(38, dpi), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    auto mv = [&](int id, int x, int y, int w, int h, HFONT font = nullptr) {
        HWND c = GetDlgItem(hwnd, id);
        if (!c) return;
        MoveWindow(c, DpiScale(x, dpi), DpiScale(y, dpi), DpiScale(w, dpi), DpiScale(h, dpi), TRUE);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_pinPopFont), TRUE);
    };

    mv(IDC_PINP_TITLE,        16,  12, 398, 22);
    mv(IDC_PINP_INFO,         16,  38, 398, 56);
    mv(IDC_PINP_CODE,         16, 102, 398, 54, g_pinPopBigFont);
    mv(IDC_PINP_CHK_REMEMBER, 16, 168, 398, 24);
    mv(IDC_PINP_BTN_REJECT,  140, 204, 150, 34);
}

void PopulatePinPopupContent() {
    if (!g_pinPopupWnd || !g_activePinSession) return;
    const auto& req = g_activePinSession->req;

    std::wostringstream info;
    info << L"Geraet: " << (req.senderName.empty() ? L"Surface" : req.senderName)
         << L"  (IP: " << s2p::Utf8ToWide(req.senderIp) << L")\r\n";
    if (!s2p::IsAllZero(req.senderMac, 6)) {
        info << L"MAC-Adresse: " << s2p::FormatMacAddress(req.senderMac) << L"\r\n";
    } else if (!s2p::IsAllZero(req.senderDeviceId, 16)) {
        info << L"Geraete-ID: " << s2p::FormatHex(req.senderDeviceId, 8) << L"... (LAN)\r\n";
    } else {
        info << L"MAC-Adresse: Unbekannt\r\n";
    }
    info << L"Diese einmalige 6-stellige PIN am Surface eingeben:";
    SetDlgItemTextW(g_pinPopupWnd, IDC_PINP_INFO, info.str().c_str());

    std::wstring pinW = s2p::Utf8ToWide(req.pinCode);
    if (pinW.size() == 6) {
        pinW.insert(3, L"  ");
    }
    SetDlgItemTextW(g_pinPopupWnd, IDC_PINP_CODE, pinW.c_str());

    SendDlgItemMessageW(g_pinPopupWnd, IDC_PINP_CHK_REMEMBER, BM_SETCHECK,
                        g_activePinSession->remember.load() ? BST_CHECKED : BST_UNCHECKED, 0);
}

LRESULT CALLBACK PinPopupWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 10, 10, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
        };
        addCtrl(L"STATIC", L"Neue Screen2PC Verbindungsanfrage", 0, IDC_PINP_TITLE);
        addCtrl(L"STATIC", L"", 0, IDC_PINP_INFO);
        addCtrl(L"STATIC", L"------", SS_CENTER | SS_SUNKEN, IDC_PINP_CODE);
        addCtrl(L"BUTTON", L"Dieses Geraet dauerhaft merken (MAC speichern)",
                BS_AUTOCHECKBOX | WS_TABSTOP, IDC_PINP_CHK_REMEMBER);
        addCtrl(L"BUTTON", L"Ablehnen", BS_PUSHBUTTON | WS_TABSTOP, IDC_PINP_BTN_REJECT);

        LayoutPinPopupControls(hwnd, GetWindowDpiSafe(hwnd));
        s2p::ui::ApplyWindowTheme(hwnd, g_theme.dark);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (g_theme.brushWindow) {
            FillRect(reinterpret_cast<HDC>(wp), &rc, g_theme.brushWindow);
            return 1;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORDLG:
        return s2p::ui::HandleCtlColor(msg, wp, g_theme, false);

    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, HWND_TOPMOST, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOACTIVATE);
        }
        LayoutPinPopupControls(hwnd, HIWORD(wp));
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDC_PINP_CHK_REMEMBER) {
            if (g_activePinSession) {
                bool chk = SendDlgItemMessageW(hwnd, IDC_PINP_CHK_REMEMBER, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_activePinSession->remember.store(chk);
            }
            return 0;
        }
        if (LOWORD(wp) == IDC_PINP_BTN_REJECT) {
            if (g_activePinSession) {
                g_activePinSession->rejected.store(true);
                g_activePinSession.reset();
            }
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        if (g_activePinSession) {
            g_activePinSession->rejected.store(true);
            g_activePinSession.reset();
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_pinPopFont) { DeleteObject(g_pinPopFont); g_pinPopFont = nullptr; }
        if (g_pinPopBigFont) { DeleteObject(g_pinPopBigFont); g_pinPopBigFont = nullptr; }
        g_pinPopupWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ShowOrUpdatePinPopupWindow(const std::shared_ptr<s2p::PinPopupSession>& session) {
    if (!session || g_closing.load()) return;
    g_activePinSession = session;

    if (g_pinPopupWnd && IsWindow(g_pinPopupWnd)) {
        PopulatePinPopupContent();
        SetWindowPos(g_pinPopupWnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOACTIVATE);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PinPopupWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = s2p::LoadAppLargeIcon();
        wc.hIconSm = s2p::LoadAppSmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Screen2PCPinPopupWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    UINT dpi = GetDpiForSystem();
    if (dpi < 72) dpi = 96;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    DWORD exStyle = WS_EX_TOPMOST;
    RECT wr{ 0, 0, DpiScale(kPinPopBaseW, dpi), DpiScale(kPinPopBaseH, dpi) };
    AdjustWindowRectExForDpi(&wr, style, FALSE, exStyle, dpi);

    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;

    g_pinPopupWnd = CreateWindowExW(
        exStyle, L"Screen2PCPinPopupWnd", L"Screen2PC Kopplungs-PIN",
        style, x, y, w, h,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (g_pinPopupWnd) {
        PopulatePinPopupContent();
        ShowWindow(g_pinPopupWnd, SW_SHOWNOACTIVATE);
        UpdateWindow(g_pinPopupWnd);
    }
}

void DismissPinPopupWindow() {
    g_activePinSession.reset();
    if (g_pinPopupWnd && IsWindow(g_pinPopupWnd)) {
        DestroyWindow(g_pinPopupWnd);
        g_pinPopupWnd = nullptr;
    }
}

// ============================================================================
// Trusted Devices Management Window on PC
// ============================================================================

void PopulateTrustedDevicesList() {
    if (!g_trustedWnd || !g_engine) return;
    HWND lv = GetDlgItem(g_trustedWnd, IDC_TR_LIST);
    if (!lv) return;

    ListView_DeleteAllItems(lv);
    g_trustedCache = g_engine->GetTrustedDevices();

    for (size_t i = 0; i < g_trustedCache.size(); ++i) {
        const auto& e = g_trustedCache[i];
        std::wstring name = e.name.empty() ? L"Surface" : e.name;
        std::wstring mac = !e.macStr.empty() ? e.macStr
                                             : (!e.deviceIdHex.empty() ? (L"ID: " + e.deviceIdHex.substr(0, 12)) : e.key);
        std::wstring ip = e.lastIp.empty() ? L"-" : e.lastIp;
        std::wstring utc = e.addedUtc.empty() ? L"-" : e.addedUtc;

        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = name.data();
        int row = ListView_InsertItem(lv, &item);
        if (row >= 0) {
            ListView_SetItemText(lv, row, 1, mac.data());
            ListView_SetItemText(lv, row, 2, ip.data());
            ListView_SetItemText(lv, row, 3, utc.data());
        }
    }

    std::wstring info = L"Gespeicherte vertraute Geraete (" + std::to_wstring(g_trustedCache.size()) +
                        L"): Diese Geraete duerfen sich ohne erneute PIN verbinden.";
    SetDlgItemTextW(g_trustedWnd, IDC_TR_INFO, info.c_str());
}

void LayoutTrustedControls(HWND hwnd, UINT dpi) {
    if (g_trustedFont) DeleteObject(g_trustedFont);
    g_trustedFont = CreateFontW(-DpiScale(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    auto mv = [&](int id, int x, int y, int w, int h) {
        HWND c = GetDlgItem(hwnd, id);
        if (!c) return;
        MoveWindow(c, DpiScale(x, dpi), DpiScale(y, dpi), DpiScale(w, dpi), DpiScale(h, dpi), TRUE);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_trustedFont), TRUE);
    };

    mv(IDC_TR_INFO,        16,  12, 558,  22);
    mv(IDC_TR_LIST,        16,  40, 558, 282);
    mv(IDC_TR_BTN_REMOVE,  16, 334, 174,  30);
    mv(IDC_TR_BTN_CLEAR,  198, 334, 134,  30);
    mv(IDC_TR_BTN_REFRESH,340, 334, 114,  30);
    mv(IDC_TR_BTN_CLOSE,  462, 334, 112,  30);

    HWND lv = GetDlgItem(hwnd, IDC_TR_LIST);
    if (lv) {
        ListView_SetColumnWidth(lv, 0, DpiScale(140, dpi));
        ListView_SetColumnWidth(lv, 1, DpiScale(150, dpi));
        ListView_SetColumnWidth(lv, 2, DpiScale(116, dpi));
        ListView_SetColumnWidth(lv, 3, DpiScale(136, dpi));
    }
}

LRESULT CALLBACK TrustedWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 10, 10, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
        };

        addCtrl(L"STATIC", L"", 0, IDC_TR_INFO);
        HWND lv = addCtrl(WC_LISTVIEWW, L"",
                          LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER | WS_TABSTOP,
                          IDC_TR_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        const wchar_t* cols[] = { L"Geraet", L"MAC / ID", L"Letzte IP", L"Hinzugefuegt (UTC)" };
        for (int c = 0; c < 4; ++c) {
            LVCOLUMNW lvc{};
            lvc.mask = LVCF_TEXT | LVCF_WIDTH;
            lvc.cx = 120;
            lvc.pszText = const_cast<LPWSTR>(cols[c]);
            ListView_InsertColumn(lv, c, &lvc);
        }

        addCtrl(L"BUTTON", L"Ausgewaehltes entfernen", BS_PUSHBUTTON | WS_TABSTOP, IDC_TR_BTN_REMOVE);
        addCtrl(L"BUTTON", L"Alle entfernen", BS_PUSHBUTTON | WS_TABSTOP, IDC_TR_BTN_CLEAR);
        addCtrl(L"BUTTON", L"Aktualisieren", BS_PUSHBUTTON | WS_TABSTOP, IDC_TR_BTN_REFRESH);
        addCtrl(L"BUTTON", L"Schliessen", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_TR_BTN_CLOSE);

        LayoutTrustedControls(hwnd, GetWindowDpiSafe(hwnd));
        s2p::ui::ApplyWindowTheme(hwnd, g_theme.dark);
        s2p::ui::ApplyChildTheme(lv, g_theme.dark);
        return 0;
    }

    case WM_ERASEBKGND: {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        if (g_theme.brushWindow) {
            FillRect(reinterpret_cast<HDC>(wp), &rc, g_theme.brushWindow);
            return 1;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORDLG:
        return s2p::ui::HandleCtlColor(msg, wp, g_theme, false);

    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutTrustedControls(hwnd, HIWORD(wp));
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TR_BTN_REMOVE: {
            HWND lv = GetDlgItem(hwnd, IDC_TR_LIST);
            int sel = lv ? ListView_GetNextItem(lv, -1, LVNI_SELECTED) : -1;
            if (sel >= 0 && static_cast<size_t>(sel) < g_trustedCache.size() && g_engine) {
                g_engine->RemoveTrustedDeviceByKey(g_trustedCache[sel].key);
                PopulateTrustedDevicesList();
            }
            return 0;
        }
        case IDC_TR_BTN_CLEAR:
            if (g_engine && !g_trustedCache.empty()) {
                int ans = MessageBoxW(hwnd,
                    L"Wirklich alle gespeicherten vertrauten Geraete entfernen?\r\nBeim naechsten Verbinden wird wieder eine 6-stellige PIN abgefragt.",
                    L"Vertraute Geraete", MB_YESNO | MB_ICONQUESTION);
                if (ans == IDYES) {
                    g_engine->ClearAllTrustedDevices();
                    PopulateTrustedDevicesList();
                }
            }
            return 0;
        case IDC_TR_BTN_REFRESH:
            PopulateTrustedDevicesList();
            return 0;
        case IDC_TR_BTN_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_trustedFont) { DeleteObject(g_trustedFont); g_trustedFont = nullptr; }
        g_trustedWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenTrustedDevicesWindow(HWND parent) {
    if (g_trustedWnd && IsWindow(g_trustedWnd)) {
        PopulateTrustedDevicesList();
        ShowWindow(g_trustedWnd, SW_SHOW);
        SetForegroundWindow(g_trustedWnd);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TrustedWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = s2p::LoadAppLargeIcon();
        wc.hIconSm = s2p::LoadAppSmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Screen2PCTrustedWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    UINT dpi = GetWindowDpiSafe(parent);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT wr{ 0, 0, DpiScale(kTrustedBaseW, dpi), DpiScale(kTrustedBaseH, dpi) };
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, dpi);

    g_trustedWnd = CreateWindowExW(
        0, L"Screen2PCTrustedWnd",
        L"Vertraute Geraete (PIN-Kopplung / MAC-Liste)",
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (g_trustedWnd) {
        PopulateTrustedDevicesList();
        ShowWindow(g_trustedWnd, SW_SHOW);
        UpdateWindow(g_trustedWnd);
    }
}

// ============================================================================
// File & Folder Pickers + Auto-Updater
// ============================================================================

std::vector<std::wstring> PickFilesDialog(HWND owner) {
    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Alle Dateien (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Dateien ans Surface senden";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    std::vector<std::wstring> out;
    if (!GetOpenFileNameW(&ofn)) return out;

    const wchar_t* p = buf.data();
    std::wstring first = p;
    p += first.size() + 1;
    if (*p == L'\0') {
        out.push_back(first);
    } else {
        while (*p != L'\0') {
            std::wstring name = p;
            out.push_back(first + L"\\" + name);
            p += name.size() + 1;
        }
    }
    return out;
}

std::vector<std::wstring> PickFoldersDialog(HWND owner) {
    std::vector<std::wstring> out;
    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
    if (FAILED(hr) || !dlg) return out;

    DWORD opts = 0;
    if (SUCCEEDED(dlg->GetOptions(&opts))) {
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_ALLOWMULTISELECT | FOS_PATHMUSTEXIST | FOS_FORCEFILESYSTEM);
    }
    dlg->SetTitle(L"Ordner (inkl. Unterordner) ans Surface senden");
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items)) && items) {
            DWORD count = 0;
            items->GetCount(&count);
            for (DWORD i = 0; i < count; ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        out.emplace_back(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            items->Release();
        }
    }
    dlg->Release();
    return out;
}

void StartAsyncUpdateCheck(bool userInitiated) {
    if (g_closing.load()) return;
    if (g_updateBusy.exchange(true)) {
        return;
    }
    if (g_hwnd && userInitiated) {
        EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_UPDATE), FALSE);
        SetDlgItemTextW(g_hwnd, IDC_BTN_UPDATE, L"Pruefe GitHub...");
    }
    if (g_updateThread.joinable()) g_updateThread.join();
    g_updateThread = std::thread([userInitiated]() {
        auto* res = new UpdateCheckResult{};
        res->info = s2p::CheckForGitHubUpdate();
        res->userInitiated = userInitiated;
        if (!g_closing.load() && g_hwnd) {
            PostMessageW(g_hwnd, WM_UPDATE_CHECK_DONE, 0, reinterpret_cast<LPARAM>(res));
        } else {
            delete res;
            g_updateBusy = false;
        }
    });
}

void PromptAndApplyUpdate(HWND hwnd, const s2p::UpdateReleaseInfo& info) {
    if (!info.valid || !info.updateAvailable || g_promptOpen || g_closing.load()) return;
    g_promptOpen = true;

    std::wostringstream msg;
    msg << L"Es ist ein neues Screen2PC-Update auf GitHub verfuegbar!\r\n\r\n"
        << L"Installierte Version: v" << s2p::FormatAppVersion(s2p::CurrentAppVersion()) << L"\r\n"
        << L"Neue Version:         v" << s2p::FormatAppVersion(info.version)
        << L" (" << info.tagName << L")\r\n\r\n"
        << L"Moechtest du das Update jetzt automatisch herunterladen, entpacken und installieren?\r\n"
        << L"(Dieser PC bleibt weiterhin 100 % treiberfrei. Die App startet nach dem Update automatisch neu.)";

    int ans = MessageBoxW(hwnd, msg.str().c_str(), L"Screen2PC Update verfuegbar", MB_YESNO | MB_ICONINFORMATION);
    g_promptOpen = false;
    if (ans != IDYES || g_closing.load()) {
        return;
    }

    if (g_updateBusy.exchange(true)) {
        return;
    }
    EnableWindow(GetDlgItem(hwnd, IDC_BTN_UPDATE), FALSE);
    SetDlgItemTextW(hwnd, IDC_BTN_UPDATE, L"Lade Update herunter...");

    if (g_updateThread.joinable()) g_updateThread.join();
    s2p::UpdateReleaseInfo copyInfo = info;
    g_updateThread = std::thread([copyInfo]() {
        auto* dlRes = new UpdateDownloadResult{};
        dlRes->ok = s2p::DownloadAndLaunchSelfUpdate(copyInfo, false, false, dlRes->error);
        if (!g_closing.load() && g_hwnd) {
            PostMessageW(g_hwnd, WM_UPDATE_DL_DONE, 0, reinterpret_cast<LPARAM>(dlRes));
        } else {
            delete dlRes;
            g_updateBusy = false;
        }
    });
}

void ShowMainWindow(HWND hwnd) {
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

std::wstring FormatFileProgressText(const s2p::FileTransferStats& fs) {
    if (fs.transferring) {
        std::wostringstream ss;
        if (fs.isSending && fs.batchTotalFiles > 0) {
            uint64_t doneBytes = fs.batchCompletedBytes + fs.currentFileBytes;
            uint64_t totalBytes = std::max<uint64_t>(doneBytes, fs.batchTotalBytes);
            double pct = (totalBytes > 0)
                             ? std::min(100.0, (100.0 * static_cast<double>(doneBytes)) / static_cast<double>(totalBytes))
                             : (100.0 * static_cast<double>(fs.batchCompletedFiles) / static_cast<double>(fs.batchTotalFiles));
            double doneMb = static_cast<double>(doneBytes) / (1024.0 * 1024.0);
            double totalMb = static_cast<double>(totalBytes) / (1024.0 * 1024.0);
            double remMb = std::max(0.0, totalMb - doneMb);
            uint32_t curFileNum = std::min(fs.batchTotalFiles, fs.batchCompletedFiles + 1);

            ss << L"Sende (" << curFileNum << L"/" << fs.batchTotalFiles << L"): "
               << std::fixed << std::setprecision(0) << pct << L" %  |  "
               << std::setprecision(1) << doneMb << L" / " << totalMb << L" MB (noch " << remMb << L" MB)";
            if (fs.currentSpeedMBps > 0.05) {
                ss << L"  |  " << std::setprecision(1) << fs.currentSpeedMBps << L" MB/s";
            }
            return ss.str();
        }
        ss << (fs.isSending ? L"Sende: " : L"Empfange: ");
        ss << (fs.currentFileName.empty() ? L"Datei..." : fs.currentFileName);
        if (fs.currentFileTotal > 0) {
            double pct = std::min(100.0, (100.0 * static_cast<double>(fs.currentFileBytes)) /
                                           static_cast<double>(fs.currentFileTotal));
            ss << L"  (" << std::fixed << std::setprecision(0) << pct << L" %";
            if (fs.currentSpeedMBps > 0.05) {
                ss << L", " << std::setprecision(1) << fs.currentSpeedMBps << L" MB/s";
            }
            ss << L")";
        }
        if (fs.queuedFiles > 0) {
            ss << L"  [+" << fs.queuedFiles << L" in Warteschlange]";
        }
        return ss.str();
    }
    if (!fs.lastEvent.empty()) {
        return fs.lastEvent;
    }
    if (fs.active) {
        return L"Dateitransfer bereit (Dateien/Ordner hierher ziehen)";
    }
    return L"Dateitransfer: Nur bei aktiver Session verfuegbar";
}

void UpdateUi() {
    if (!g_hwnd || !g_engine) return;
    auto st = g_engine->GetStatus();

    std::wostringstream hdr;
    hdr << st.stateText << L"\r\nPC-IPs: ";
    for (size_t i = 0; i < st.localIps.size(); ++i) {
        if (i > 0) hdr << L", ";
        hdr << s2p::Utf8ToWide(st.localIps[i]);
    }
    hdr << L"\r\nMonitore (" << st.localMonitors.size() << L"): ";
    for (size_t i = 0; i < st.localMonitors.size(); ++i) {
        if (i > 0) hdr << L"  |  ";
        hdr << L"#" << (i + 1) << L" " << st.localMonitors[i].width << L"x" << st.localMonitors[i].height
            << L"@" << st.localMonitors[i].refreshHz << L"Hz";
        if (i < st.localMonitorHdr.size() && st.localMonitorHdr[i].isHdr) {
            int nits = static_cast<int>(std::lround(st.localMonitorHdr[i].sdrWhiteScale * 80.0f));
            hdr << L" [HDR " << nits << L" Nits]";
        } else {
            hdr << L" [SDR]";
        }
        if (st.localMonitors[i].isPrimary) hdr << L" (Primary)";
    }

    s2p::ui::StatusTone newTone = st.sessionActive ? s2p::ui::StatusTone::Success : s2p::ui::StatusTone::Neutral;
    std::wstring heroTitle = st.sessionActive ? L"Session aktiv" : L"Bereit \u2014 Warte auf Surface";
    if (st.sessionActive && !st.streams.empty()) {
        double totalMbps = 0.0;
        for (const auto& s : st.streams) totalMbps += s.mbps;
        std::wostringstream hs;
        hs << (st.senderName.empty() ? L"Surface" : st.senderName)
           << L"  \u2022  " << std::fixed << std::setprecision(1) << st.streams[0].fps << L" FPS"
           << L"  \u2022  " << std::setprecision(1) << st.streams[0].avgDecodeMs << L" ms"
           << L"  \u2022  " << std::setprecision(1) << totalMbps << L" Mbit/s";
        heroTitle = hs.str();
    }
    if (newTone != g_heroTone) {
        g_heroTone = newTone;
        RECT dotRc{ 0, 0, DpiScale(60, g_currentDpi), DpiScale(132, g_currentDpi) };
        InvalidateRect(g_hwnd, &dotRc, FALSE);
    }
    SetDlgItemTextW(g_hwnd, IDC_HERO_TITLE, heroTitle.c_str());

    SetDlgItemTextW(g_hwnd, IDC_INFO_LABEL, hdr.str().c_str());
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_DISCONN), (st.sessionActive && !g_disconnBusy.load()) ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_SHOW_STREAMS), st.hasHiddenStreams ? TRUE : FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_BTN_DISCONN), nullptr, FALSE);
    InvalidateRect(GetDlgItem(g_hwnd, IDC_BTN_SHOW_STREAMS), nullptr, FALSE);

    BOOL fileReady = st.fileStats.active ? TRUE : FALSE;
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_SENDFILE), fileReady);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_SENDFOLDER), fileReady);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_CANCELFILE), st.fileStats.transferring ? TRUE : FALSE);

    int progVal = 0;
    if (st.fileStats.transferring) {
        if (st.fileStats.isSending && st.fileStats.batchTotalBytes > 0) {
            uint64_t doneBytes = st.fileStats.batchCompletedBytes + st.fileStats.currentFileBytes;
            progVal = static_cast<int>(std::clamp<uint64_t>(
                (doneBytes * 1000ULL) / st.fileStats.batchTotalBytes, 0ULL, 1000ULL));
        } else if (st.fileStats.isSending && st.fileStats.batchTotalFiles > 0) {
            progVal = static_cast<int>(std::clamp<uint64_t>(
                (static_cast<uint64_t>(st.fileStats.batchCompletedFiles) * 1000ULL) / st.fileStats.batchTotalFiles, 0ULL, 1000ULL));
        } else if (st.fileStats.currentFileTotal > 0) {
            progVal = static_cast<int>(std::clamp<uint64_t>(
                (st.fileStats.currentFileBytes * 1000ULL) / st.fileStats.currentFileTotal, 0ULL, 1000ULL));
        }
    } else if (st.fileStats.batchCompleted) {
        progVal = 1000;
    }
    SendDlgItemMessageW(g_hwnd, IDC_FILE_PROGRESS, PBM_SETPOS, progVal, 0);
    SetDlgItemTextW(g_hwnd, IDC_FILE_STATUS, FormatFileProgressText(st.fileStats).c_str());

    if (!st.fileStats.active) {
        g_lastNotifiedBatchId = 0;
    } else if (st.fileStats.batchCompleted && st.fileStats.batchId != 0 &&
               st.fileStats.batchId != g_lastNotifiedBatchId && !g_closing.load()) {
        g_lastNotifiedBatchId = st.fileStats.batchId;
        if (IsWindowVisible(g_hwnd)) {
            MessageBoxW(g_hwnd, st.fileStats.lastEvent.c_str(), L"Dateitransfer abgeschlossen", MB_OK | MB_ICONINFORMATION);
        } else {
            g_nid.uFlags = NIF_INFO;
            g_nid.dwInfoFlags = NIIF_INFO;
            wcscpy_s(g_nid.szInfoTitle, L"Dateitransfer abgeschlossen");
            wcsncpy_s(g_nid.szInfo, st.fileStats.lastEvent.c_str(), _TRUNCATE);
            Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        }
    }

    bool kvmOn = g_engine->IsKvmEnabled();
    if (IsToggleChecked(g_hwnd, IDC_CHK_KVM) != kvmOn) {
        SetToggleChecked(g_hwnd, IDC_CHK_KVM, kvmOn);
    }

    std::wostringstream body;
    if (!st.sessionActive) {
        body << L"Keine aktive Session. Starte Screen2PC.Sender.exe auf dem Surface.\r\n"
             << L"Hinweis: 100 % User-Mode (kein Treiber, kein Test-Signing, Anti-Cheat-sicher).\r\n\r\n"
             << L"Letzte Log-Meldungen:\r\n";
        auto logs = s2p::Logger::Instance().GetRecentLines(5);
        for (const auto& l : logs) body << l << L"\r\n";
    } else {
        for (const auto& s : st.streams) {
            body << L"Display " << (s.monitorIndex + 1) << L" [" << s.width << L"x" << s.height << L"]  "
                 << s.codecName << L"  |  "
                 << std::fixed << std::setprecision(1) << s.fps << L" FPS  |  "
                 << std::setprecision(1) << s.mbps << L" Mbit/s  |  "
                 << L"Dec: " << std::setprecision(1) << s.avgDecodeMs << L" ms  |  "
                 << L"Frames: " << s.framesReceived;
            if (s.hiddenByUser) body << L" [Versteckt]";
            body << L"\r\n";
        }
        body << L"Dateien: Gesendet " << st.fileStats.filesSent
             << L" | Empfangen " << st.fileStats.filesReceived << L"\r\n";
    }
    SetDlgItemTextW(g_hwnd, IDC_STATS_TEXT, body.str().c_str());
}

void OpenFirewallPortsElevated() {
    const wchar_t* cmdArgs =
        L"/c netsh advfirewall firewall delete rule name=\"Screen2PC\" >nul 2>&1 & "
        L"netsh advfirewall firewall delete rule name=\"Screen2PC Discovery\" >nul 2>&1 & "
        L"netsh advfirewall firewall add rule name=\"Screen2PC\" dir=in action=allow protocol=TCP localport=48211-48215 & "
        L"netsh advfirewall firewall add rule name=\"Screen2PC Discovery\" dir=in action=allow protocol=UDP localport=48210,48215";
    ShellExecuteW(g_hwnd, L"runas", L"cmd.exe", cmdArgs, nullptr, SW_HIDE);
}

void LayoutMainControls(HWND hwnd, UINT dpi) {
    g_currentDpi = dpi;
    if (g_font) DeleteObject(g_font);
    if (g_boldFont) DeleteObject(g_boldFont);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (g_monoFont) DeleteObject(g_monoFont);
    g_font = CreateFontW(-DpiScale(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_boldFont = CreateFontW(-DpiScale(14, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_titleFont = CreateFontW(-DpiScale(16, dpi), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_monoFont = CreateFontW(-DpiScale(12, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    auto mv = [&](int id, int x, int y, int w, int h, HFONT font = nullptr) {
        HWND c = GetDlgItem(hwnd, id);
        if (!c) return;
        MoveWindow(c, DpiScale(x, dpi), DpiScale(y, dpi), DpiScale(w, dpi), DpiScale(h, dpi), TRUE);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
    };

    // Hero Card (16, 12, 548, 158)
    mv(IDC_HERO_TITLE,       46,  20, 504, 22, g_titleFont);
    mv(IDC_INFO_LABEL,       46,  44, 504, 80);
    mv(IDC_BTN_DISCONN,      28, 130, 258, 30, g_boldFont);
    mv(IDC_BTN_SHOW_STREAMS, 294,130, 258, 30, g_boldFont);

    // Segmented Tab Bar (16, 180, 548, 32)
    mv(IDC_TAB_OVERVIEW,     16, 180, 178, 32, g_boldFont);
    mv(IDC_TAB_TRANSFER,    201, 180, 178, 32, g_boldFont);
    mv(IDC_TAB_SETTINGS,    386, 180, 178, 32, g_boldFont);

    // Tab 0: Uebersicht
    // Card 1: Schnellsteuerung (16, 222, 548, 168)
    mv(IDC_CHK_FULLSCR,      30, 232, 520, 26);
    mv(IDC_CHK_CLIPBOARD,    30, 261, 520, 26);
    mv(IDC_CHK_KVM,          30, 290, 520, 26);
    mv(IDC_LBL_MOUSE_SPEED,  30, 323, 164, 22, g_boldFont);
    mv(IDC_TB_MOUSE_SPEED,  198, 321, 290, 26);
    mv(IDC_VAL_MOUSE_SPEED, 492, 323,  58, 22, g_boldFont);
    mv(IDC_LBL_SCROLL_SPEED, 30, 354, 164, 22, g_boldFont);
    mv(IDC_TB_SCROLL_SPEED, 198, 352, 290, 26);
    mv(IDC_VAL_SCROLL_SPEED,492, 354,  58, 22, g_boldFont);

    // Card 2: Live-Stream-Daten (16, 402, 548, 280)
    mv(IDC_LBL_TELEMETRY,    30, 414, 520, 20, g_boldFont);
    mv(IDC_STATS_TEXT,       28, 438, 524, 232, g_monoFont);

    // Tab 1: Dateitransfer
    // Drop Zone Card (16, 222, 548, 146)
    mv(IDC_DROP_TITLE,       32, 272, 516, 24, g_titleFont);
    mv(IDC_DROP_SUB,         32, 300, 516, 20);
    // Transfer Action Card (16, 380, 548, 134)
    mv(IDC_BTN_SENDFILE,     30, 396, 168, 32);
    mv(IDC_BTN_SENDFOLDER,  206, 396, 168, 32);
    mv(IDC_BTN_OPEN_DL,     382, 396, 168, 32);
    mv(IDC_FILE_PROGRESS,    30, 442, 412, 22);
    mv(IDC_BTN_CANCELFILE,  450, 440, 100, 26);
    mv(IDC_FILE_STATUS,      30, 474, 520, 20);

    // Tab 2: Einstellungen
    // Card 1: App & Autostart (16, 222, 548, 112)
    mv(IDC_CHK_AUTOSTART,    30, 234, 520, 26);
    mv(IDC_CHK_AUTOUPDATE,   30, 264, 520, 26);
    mv(IDC_CHK_STREAM_HUD,   30, 294, 520, 26);

    // Card 2: Werkzeuge & Sicherheit (16, 346, 548, 106)
    mv(IDC_BTN_COLORS,       30, 362, 256, 34);
    mv(IDC_BTN_TRUSTED,     294, 362, 256, 34);
    mv(IDC_BTN_FIREWALL,     30, 404, 256, 34);
    mv(IDC_BTN_UPDATE,      294, 404, 256, 34);
}

void CreateUiControls(HWND hwnd) {
    auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                               0, 0, 10, 10, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    };

    // Hero
    addCtrl(L"STATIC", L"Bereit \u2014 Warte auf Surface", SS_ENDELLIPSIS, IDC_HERO_TITLE);
    addCtrl(L"STATIC", L"Initialisiere...", 0, IDC_INFO_LABEL);
    addCtrl(L"BUTTON", L"Aktive Session trennen", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_DISCONN);
    addCtrl(L"BUTTON", L"Streamfenster einblenden", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_SHOW_STREAMS);

    // Tabs
    addCtrl(L"BUTTON", L"Uebersicht", BS_OWNERDRAW | WS_TABSTOP, IDC_TAB_OVERVIEW);
    addCtrl(L"BUTTON", L"Dateitransfer", BS_OWNERDRAW | WS_TABSTOP, IDC_TAB_TRANSFER);
    addCtrl(L"BUTTON", L"Einstellungen", BS_OWNERDRAW | WS_TABSTOP, IDC_TAB_SETTINGS);

    // Tab 0: Uebersicht
    HWND chk = addCtrl(L"BUTTON", L"Streams im randlosen Vollbild starten (F11)", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_FULLSCR);
    SetToggleChecked(hwnd, IDC_CHK_FULLSCR, LoadBoolPref(L"StartFullscreen", 1));

    HWND chkClip = addCtrl(L"BUTTON", L"Geteilte Zwischenablage (Text & Bilder)", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_CLIPBOARD);
    bool clipPref = LoadBoolPref(L"ClipboardSync", 1);
    SetToggleChecked(hwnd, IDC_CHK_CLIPBOARD, clipPref);
    if (g_engine) g_engine->SetClipboardEnabled(clipPref);

    HWND chkKvm = addCtrl(L"BUTTON", L"KVM: Maus & Tastatur ans Surface (Strg+Alt+K)", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_KVM);
    bool kvmPref = LoadBoolPref(L"KvmEnabled", 1);
    SetToggleChecked(hwnd, IDC_CHK_KVM, kvmPref);
    if (g_engine) g_engine->SetKvmEnabled(kvmPref);

    addCtrl(L"STATIC", L"Maus-Tempo (KVM):", SS_CENTERIMAGE, IDC_LBL_MOUSE_SPEED);
    HWND tbMouse = addCtrl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, IDC_TB_MOUSE_SPEED);
    SendMessageW(tbMouse, TBM_SETRANGE, TRUE, MAKELONG(s2p::KvmInput::kMinSpeedPct, s2p::KvmInput::kMaxSpeedPct));
    SendMessageW(tbMouse, TBM_SETTICFREQ, 50, 0);
    SendMessageW(tbMouse, TBM_SETPAGESIZE, 0, 25);
    uint32_t mousePct = s2p::KvmInput::ClampSpeedPct(LoadUintPref(L"KvmMouseSpeedPct", s2p::KvmInput::kDefaultMouseSpeedPct));
    SendMessageW(tbMouse, TBM_SETPOS, TRUE, static_cast<LPARAM>(mousePct));
    addCtrl(L"STATIC", (std::to_wstring(mousePct) + L" %").c_str(), SS_RIGHT | SS_CENTERIMAGE, IDC_VAL_MOUSE_SPEED);
    if (g_engine) g_engine->SetKvmMouseSpeedPct(mousePct);

    addCtrl(L"STATIC", L"Scroll-Tempo (KVM):", SS_CENTERIMAGE, IDC_LBL_SCROLL_SPEED);
    HWND tbScroll = addCtrl(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_AUTOTICKS | WS_TABSTOP, IDC_TB_SCROLL_SPEED);
    SendMessageW(tbScroll, TBM_SETRANGE, TRUE, MAKELONG(s2p::KvmInput::kMinSpeedPct, s2p::KvmInput::kMaxSpeedPct));
    SendMessageW(tbScroll, TBM_SETTICFREQ, 50, 0);
    SendMessageW(tbScroll, TBM_SETPAGESIZE, 0, 25);
    uint32_t scrollPct = s2p::KvmInput::ClampSpeedPct(LoadUintPref(L"KvmScrollSpeedPct", s2p::KvmInput::kDefaultScrollSpeedPct));
    SendMessageW(tbScroll, TBM_SETPOS, TRUE, static_cast<LPARAM>(scrollPct));
    addCtrl(L"STATIC", (std::to_wstring(scrollPct) + L" %").c_str(), SS_RIGHT | SS_CENTERIMAGE, IDC_VAL_SCROLL_SPEED);
    if (g_engine) g_engine->SetKvmScrollSpeedPct(scrollPct);

    addCtrl(L"STATIC", L"Live-Stream-Daten (FPS, Decode-Latenz, Mbit/s)", SS_NOPREFIX, IDC_LBL_TELEMETRY);
    addCtrl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, IDC_STATS_TEXT);

    // Tab 1: Dateitransfer
    addCtrl(L"STATIC", L"Dateien oder Ordner hierher ziehen", SS_CENTER, IDC_DROP_TITLE);
    addCtrl(L"STATIC", L"Werden direkt ans verbundene Surface gesendet", SS_CENTER, IDC_DROP_SUB);
    addCtrl(L"BUTTON", L"Datei(en) senden...", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_SENDFILE);
    addCtrl(L"BUTTON", L"Ordner senden...", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_SENDFOLDER);
    addCtrl(L"BUTTON", L"Downloads oeffnen", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_OPEN_DL);
    HWND prog = addCtrl(PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_FILE_PROGRESS);
    SendMessageW(prog, PBM_SETRANGE32, 0, 1000);
    addCtrl(L"BUTTON", L"Abbrechen", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_CANCELFILE);
    addCtrl(L"STATIC", L"Dateitransfer: Bereit", SS_ENDELLIPSIS, IDC_FILE_STATUS);

    // Tab 2: Einstellungen
    HWND chkAuto = addCtrl(L"BUTTON", L"Mit Windows starten (System-Tray)", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_AUTOSTART);
    SetToggleChecked(hwnd, IDC_CHK_AUTOSTART, s2p::IsAutostartEnabled());

    HWND chkUpd = addCtrl(L"BUTTON", L"Beim Start auf GitHub-Updates pruefen", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_AUTOUPDATE);
    SetToggleChecked(hwnd, IDC_CHK_AUTOUPDATE, LoadBoolPref(L"AutoUpdateCheck", 1));

    HWND chkHud = addCtrl(L"BUTTON", L"In-Stream-Overlay (FPS / Latenz / Mbit/s) anzeigen", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_STREAM_HUD);
    bool hudPref = LoadBoolPref(L"StreamHudEnabled", 1);
    SetToggleChecked(hwnd, IDC_CHK_STREAM_HUD, hudPref);
    if (g_engine) g_engine->SetStreamHudEnabled(hudPref);

    addCtrl(L"BUTTON", L"Farbe & HDR pro Monitor...", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_COLORS);
    addCtrl(L"BUTTON", L"Vertraute Geraete...", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_TRUSTED);
    addCtrl(L"BUTTON", L"Firewall-Ports oeffnen (Admin)", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_FIREWALL);
    addCtrl(L"BUTTON", DefaultUpdateButtonText().c_str(), BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_UPDATE);

    g_tooltip = s2p::ui::CreateTooltipWindow(hwnd);
    s2p::ui::SetControlTooltip(g_tooltip, hwnd, chk, L"Oeffnet Streamfenster direkt im randlosen Vollbild (F11 zum Umschalten).");
    s2p::ui::SetControlTooltip(g_tooltip, hwnd, chkKvm, L"Aktiviert Maus- und Tastatursteuerung des Surface direkt im Streamfenster (Strg+Alt+K).");
    s2p::ui::SetControlTooltip(g_tooltip, hwnd, chkAuto, L"Startet Screen2PC Receiver minimiert im System-Tray (Task-Manager Autostart).");
    s2p::ui::SetControlTooltip(g_tooltip, hwnd, chkHud, L"Zeigt FPS, Latenz, Bitrate und KVM-Status oben im Streamfenster an (Shortcut: Strg+Alt+H).");

    LayoutMainControls(hwnd, GetWindowDpiSafe(hwnd));
    ApplyReceiverTheme(hwnd, s2p::ui::IsSystemDarkMode());
    ApplyReceiverTabVisibility(hwnd);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Statusfenster oeffnen");
    bool hasHidden = g_engine && g_engine->HasHiddenStreamWindows();
    AppendMenuW(menu, MF_STRING | (hasHidden ? MF_ENABLED : MF_GRAYED),
                IDM_SHOW_STREAMS, L"Streamfenster wieder einblenden");
    AppendMenuW(menu, MF_STRING, IDM_CHECK_UPDATE, L"Nach Updates suchen");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Beenden");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_wmShowExisting != 0 && msg == g_wmShowExisting) {
        ShowMainWindow(hwnd);
        return 0;
    }
    if (g_wmTaskbarCreated != 0 && msg == g_wmTaskbarCreated) {
        AddTrayIcon(hwnd);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        CreateUiControls(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, TIMER_TELEMETRY, 250, nullptr);
        AddTrayIcon(hwnd);

        UpdateUi();
        if (LoadBoolPref(L"AutoUpdateCheck", 1)) {
            StartAsyncUpdateCheck(false);
        }
        return 0;

    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutMainControls(hwnd, HIWORD(wp));
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) {
            SetToggleChecked(hwnd, IDC_CHK_AUTOSTART, s2p::IsAutostartEnabled());
        }
        break;

    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            ApplyReceiverTheme(hwnd, s2p::ui::IsSystemDarkMode());
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT clientRc{};
        GetClientRect(hwnd, &clientRc);
        if (g_theme.brushWindow) {
            FillRect(hdc, &clientRc, g_theme.brushWindow);
        }
        auto scRect = [&](int x, int y, int w, int h) -> RECT {
            return RECT{ DpiScale(x, g_currentDpi), DpiScale(y, g_currentDpi),
                         DpiScale(x + w, g_currentDpi), DpiScale(y + h, g_currentDpi) };
        };

        // Hero Card
        s2p::ui::DrawCard(hdc, scRect(16, 12, 548, 158), g_theme, g_currentDpi);
        s2p::ui::DrawStatusDot(hdc, DpiScale(31, g_currentDpi), DpiScale(31, g_currentDpi),
                               std::max(4, DpiScale(5, g_currentDpi)),
                               g_theme.ToneColor(g_heroTone), g_theme.bgCard);

        if (g_activeTab == 0) {
            s2p::ui::DrawCard(hdc, scRect(16, 222, 548, 168), g_theme, g_currentDpi);
            s2p::ui::DrawCard(hdc, scRect(16, 402, 548, 280), g_theme, g_currentDpi);
        } else if (g_activeTab == 1) {
            s2p::ui::DrawCard(hdc, scRect(16, 222, 548, 146), g_theme, g_currentDpi, true);
            s2p::ui::DrawCard(hdc, scRect(16, 380, 548, 134), g_theme, g_currentDpi);
        } else if (g_activeTab == 2) {
            s2p::ui::DrawCard(hdc, scRect(16, 222, 548, 112), g_theme, g_currentDpi);
            s2p::ui::DrawCard(hdc, scRect(16, 346, 548, 106), g_theme, g_currentDpi);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORDLG: {
        bool inCard = false;
        if (msg == WM_CTLCOLORSTATIC && lp) {
            int ctrlId = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
            if (ctrlId == IDC_DROP_TITLE || ctrlId == IDC_DROP_SUB) {
                HDC hdc = reinterpret_cast<HDC>(wp);
                SetTextColor(hdc, (ctrlId == IDC_DROP_TITLE) ? g_theme.textPrimary : g_theme.textSecondary);
                SetBkColor(hdc, g_theme.bgDropZone);
                return reinterpret_cast<LRESULT>(g_theme.brushDropZone);
            }
            inCard = IsReceiverCardControlId(ctrlId);
        }
        return s2p::ui::HandleCtlColor(msg, wp, g_theme, inCard);
    }

    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (!dis || dis->CtlType != ODT_BUTTON) break;
        const int id = static_cast<int>(dis->CtlID);
        if (id == IDC_TAB_OVERVIEW || id == IDC_TAB_TRANSFER || id == IDC_TAB_SETTINGS) {
            bool active = (id == IDC_TAB_OVERVIEW && g_activeTab == 0) ||
                          (id == IDC_TAB_TRANSFER && g_activeTab == 1) ||
                          (id == IDC_TAB_SETTINGS && g_activeTab == 2);
            s2p::ui::DrawModernButton(dis, g_theme,
                                      active ? s2p::ui::ButtonStyle::TabActive : s2p::ui::ButtonStyle::TabInactive,
                                      g_currentDpi);
            return TRUE;
        }
        if (id == IDC_BTN_DISCONN) {
            s2p::ui::DrawModernButton(dis, g_theme, s2p::ui::ButtonStyle::Danger, g_currentDpi);
            return TRUE;
        }
        if (id == IDC_BTN_SHOW_STREAMS) {
            bool hidden = g_engine && g_engine->HasHiddenStreamWindows();
            s2p::ui::DrawModernButton(dis, g_theme,
                                      hidden ? s2p::ui::ButtonStyle::Primary : s2p::ui::ButtonStyle::Secondary,
                                      g_currentDpi);
            return TRUE;
        }
        if (id == IDC_CHK_FULLSCR || id == IDC_CHK_CLIPBOARD || id == IDC_CHK_KVM ||
            id == IDC_CHK_AUTOSTART || id == IDC_CHK_AUTOUPDATE || id == IDC_CHK_STREAM_HUD) {
            s2p::ui::DrawToggleSwitch(dis, g_theme, IsToggleChecked(hwnd, id), g_currentDpi);
            return TRUE;
        }
        s2p::ui::DrawModernButton(dis, g_theme, s2p::ui::ButtonStyle::Secondary, g_currentDpi);
        return TRUE;
    }

    case WM_DISPLAYCHANGE:
        if (g_engine) {
            g_engine->RefreshLocalMonitorInfo();
            UpdateUi();
            if (g_colorWnd && IsWindow(g_colorWnd)) {
                PopulateColorDialog();
            }
        }
        return 0;

    case WM_HSCROLL: {
        HWND tb = reinterpret_cast<HWND>(lp);
        if (tb) {
            int id = GetDlgCtrlID(tb);
            if (id == IDC_TB_MOUSE_SPEED || id == IDC_TB_SCROLL_SPEED) {
                uint32_t pct = s2p::KvmInput::ClampSpeedPct(
                    static_cast<uint32_t>(SendMessageW(tb, TBM_GETPOS, 0, 0)));
                std::wstring label = std::to_wstring(pct) + L" %";
                if (id == IDC_TB_MOUSE_SPEED) {
                    SetDlgItemTextW(hwnd, IDC_VAL_MOUSE_SPEED, label.c_str());
                    if (g_engine) g_engine->SetKvmMouseSpeedPct(pct);
                    SaveUintPref(L"KvmMouseSpeedPct", pct);
                } else {
                    SetDlgItemTextW(hwnd, IDC_VAL_SCROLL_SPEED, label.c_str());
                    if (g_engine) g_engine->SetKvmScrollSpeedPct(pct);
                    SaveUintPref(L"KvmScrollSpeedPct", pct);
                }
                return 0;
            }
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TAB_OVERVIEW:
            g_activeTab = 0;
            ApplyReceiverTabVisibility(hwnd);
            return 0;
        case IDC_TAB_TRANSFER:
            g_activeTab = 1;
            ApplyReceiverTabVisibility(hwnd);
            return 0;
        case IDC_TAB_SETTINGS:
            g_activeTab = 2;
            ApplyReceiverTabVisibility(hwnd);
            return 0;
        case IDC_CHK_FULLSCR: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_FULLSCR);
            if (g_engine) g_engine->SetStartFullscreen(checked);
            SaveBoolPref(L"StartFullscreen", checked);
            return 0;
        }
        case IDC_CHK_CLIPBOARD: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_CLIPBOARD);
            if (g_engine) g_engine->SetClipboardEnabled(checked);
            SaveBoolPref(L"ClipboardSync", checked);
            return 0;
        }
        case IDC_CHK_KVM: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_KVM);
            if (g_engine) g_engine->SetKvmEnabled(checked);
            SaveBoolPref(L"KvmEnabled", checked);
            return 0;
        }
        case IDC_CHK_AUTOSTART: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_AUTOSTART);
            if (!s2p::SetAutostartEnabled(checked)) {
                SetToggleChecked(hwnd, IDC_CHK_AUTOSTART, s2p::IsAutostartEnabled());
                MessageBoxW(hwnd, L"Autostart-Eintrag in der Registry konnte nicht geschrieben werden.",
                            L"Screen2PC Receiver", MB_ICONWARNING);
            }
            return 0;
        }
        case IDC_CHK_AUTOUPDATE: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_AUTOUPDATE);
            SaveBoolPref(L"AutoUpdateCheck", checked);
            return 0;
        }
        case IDC_CHK_STREAM_HUD: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_STREAM_HUD);
            if (g_engine) g_engine->SetStreamHudEnabled(checked);
            SaveBoolPref(L"StreamHudEnabled", checked);
            return 0;
        }
        case IDC_BTN_DISCONN:
            if (g_engine && !g_disconnBusy.exchange(true)) {
                EnableWindow(GetDlgItem(hwnd, IDC_BTN_DISCONN), FALSE);
                if (g_disconnThread.joinable()) g_disconnThread.join();
                g_disconnThread = std::thread([]() {
                    if (g_engine && !g_closing.load()) {
                        g_engine->DisconnectSession();
                    }
                    g_disconnBusy = false;
                    if (!g_closing.load() && g_hwnd) {
                        PostMessageW(g_hwnd, WM_DISCONNECT_DONE, 0, 0);
                    }
                });
            }
            return 0;
        case IDC_BTN_SHOW_STREAMS:
        case IDM_SHOW_STREAMS:
            if (g_engine) {
                g_engine->ShowHiddenStreamWindows();
                UpdateUi();
            }
            return 0;
        case IDC_BTN_FIREWALL:
            OpenFirewallPortsElevated();
            return 0;
        case IDC_BTN_SENDFILE:
            if (g_engine) {
                auto files = PickFilesDialog(hwnd);
                if (!files.empty()) g_engine->SendFiles(files);
            }
            return 0;
        case IDC_BTN_SENDFOLDER:
            if (g_engine) {
                auto folders = PickFoldersDialog(hwnd);
                if (!folders.empty()) g_engine->SendFiles(folders);
            }
            return 0;
        case IDC_BTN_CANCELFILE:
            if (g_engine) {
                g_engine->CancelFileTransfer();
            }
            return 0;
        case IDC_BTN_OPEN_DL:
            ShellExecuteW(hwnd, L"open", s2p::FileTransfer::DefaultDownloadDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case IDC_BTN_COLORS:
            OpenColorSettingsWindow(hwnd);
            return 0;
        case IDC_BTN_TRUSTED:
            OpenTrustedDevicesWindow(hwnd);
            return 0;
        case IDC_BTN_UPDATE:
            if (g_pendingUpdate.valid && g_pendingUpdate.updateAvailable) {
                PromptAndApplyUpdate(hwnd, g_pendingUpdate);
            } else {
                StartAsyncUpdateCheck(true);
            }
            return 0;
        case IDM_SHOW:
            ShowMainWindow(hwnd);
            return 0;
        case IDM_CHECK_UPDATE:
            ShowMainWindow(hwnd);
            if (g_pendingUpdate.valid && g_pendingUpdate.updateAvailable) {
                PromptAndApplyUpdate(hwnd, g_pendingUpdate);
            } else {
                StartAsyncUpdateCheck(true);
            }
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DISCONNECT_DONE:
        UpdateUi();
        return 0;

    case WM_PIN_SHOW: {
        auto* holder = reinterpret_cast<std::shared_ptr<s2p::PinPopupSession>*>(lp);
        std::shared_ptr<s2p::PinPopupSession> session;
        if (holder) {
            session = *holder;
            delete holder;
        }
        if (session && !g_closing.load()) {
            ShowOrUpdatePinPopupWindow(session);
        }
        return 0;
    }

    case WM_PIN_DISMISS:
        DismissPinPopupWindow();
        return 0;

    case WM_VERSION_MISMATCH: {
        auto* notice = reinterpret_cast<VersionMismatchNotice*>(lp);
        if (notice) {
            ULONGLONG now = GetTickCount64();
            if (!g_closing.load() && !g_verPromptOpen &&
                (g_lastVerPromptTick == 0 || (now - g_lastVerPromptTick) > 8000)) {
                g_verPromptOpen = true;
                g_lastVerPromptTick = now;
                ShowMainWindow(hwnd);
                std::wostringstream msg;
                msg << notice->message << L"\r\n\r\n"
                    << L"Moechtest du jetzt auf GitHub nach der aktuellen Version suchen?";
                int ans = MessageBoxW(hwnd, msg.str().c_str(),
                                      L"Screen2PC Versionskonflikt", MB_YESNO | MB_ICONWARNING);
                g_verPromptOpen = false;
                if (ans == IDYES) {
                    StartAsyncUpdateCheck(true);
                }
            }
            delete notice;
        }
        return 0;
    }

    case WM_UPDATE_CHECK_DONE: {
        auto* res = reinterpret_cast<UpdateCheckResult*>(lp);
        g_updateBusy = false;
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_UPDATE), TRUE);

        if (res && !g_closing.load()) {
            if (res->info.valid && res->info.updateAvailable) {
                g_pendingUpdate = res->info;
                SetDlgItemTextW(hwnd, IDC_BTN_UPDATE, DefaultUpdateButtonText().c_str());

                if (!res->userInitiated && !IsWindowVisible(hwnd)) {
                    g_nid.uFlags = NIF_INFO;
                    g_nid.dwInfoFlags = NIIF_INFO;
                    wcscpy_s(g_nid.szInfoTitle, L"Screen2PC Update verfuegbar");
                    std::wstring tip = L"Neue Version v" + s2p::FormatAppVersion(res->info.version) +
                                       L" auf GitHub verfuegbar. Hier klicken zum Aktualisieren.";
                    wcsncpy_s(g_nid.szInfo, tip.c_str(), _TRUNCATE);
                    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
                    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
                } else {
                    PromptAndApplyUpdate(hwnd, res->info);
                }
            } else {
                SetDlgItemTextW(hwnd, IDC_BTN_UPDATE, DefaultUpdateButtonText().c_str());
                if (res->userInitiated) {
                    if (!res->info.valid) {
                        std::wstring errMsg = L"Update-Pruefung auf GitHub fehlgeschlagen:\r\n" + res->info.error;
                        MessageBoxW(hwnd, errMsg.c_str(), L"Screen2PC Update", MB_ICONWARNING);
                    } else {
                        std::wstring okMsg = L"Screen2PC Receiver ist auf dem neuesten Stand (v" +
                                             s2p::FormatAppVersion(s2p::CurrentAppVersion()) + L").";
                        MessageBoxW(hwnd, okMsg.c_str(), L"Screen2PC Update", MB_ICONINFORMATION);
                    }
                }
            }
        }
        delete res;
        return 0;
    }

    case WM_UPDATE_DL_DONE: {
        auto* dlRes = reinterpret_cast<UpdateDownloadResult*>(lp);
        g_updateBusy = false;
        if (dlRes && dlRes->ok) {
            delete dlRes;
            DestroyWindow(hwnd);
            return 0;
        }
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_UPDATE), TRUE);
        SetDlgItemTextW(hwnd, IDC_BTN_UPDATE, DefaultUpdateButtonText().c_str());
        if (dlRes && !g_closing.load()) {
            std::wstring msg = L"Update konnte nicht gestartet werden:\r\n" + dlRes->error;
            MessageBoxW(hwnd, msg.c_str(), L"Screen2PC Update", MB_ICONERROR);
        }
        delete dlRes;
        return 0;
    }

    case WM_DROPFILES:
        if (g_engine) {
            HDROP hDrop = reinterpret_cast<HDROP>(wp);
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFFu, nullptr, 0);
            std::vector<std::wstring> files;
            files.reserve(count);
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
                if (len > 0 && len < 32768) {
                    std::wstring path(len, L'\0');
                    DragQueryFileW(hDrop, i, path.data(), len + 1);
                    files.push_back(std::move(path));
                }
            }
            DragFinish(hDrop);
            if (!files.empty()) {
                g_engine->SendFiles(files);
            }
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_TELEMETRY) UpdateUi();
        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            ShowMainWindow(hwnd);
        } else if (lp == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        } else if (lp == NIN_BALLOONUSERCLICK) {
            ShowMainWindow(hwnd);
            if (g_pendingUpdate.valid && g_pendingUpdate.updateAvailable) {
                PromptAndApplyUpdate(hwnd, g_pendingUpdate);
            }
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        g_closing = true;
        KillTimer(hwnd, TIMER_TELEMETRY);
        if (g_colorWnd && IsWindow(g_colorWnd)) {
            DestroyWindow(g_colorWnd);
            g_colorWnd = nullptr;
        }
        DismissPinPopupWindow();
        if (g_trustedWnd && IsWindow(g_trustedWnd)) {
            DestroyWindow(g_trustedWnd);
            g_trustedWnd = nullptr;
        }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_engine) g_engine->Stop();
        if (g_disconnThread.joinable()) g_disconnThread.join();
        if (g_updateThread.joinable()) g_updateThread.join();
        if (g_font) { DeleteObject(g_font); g_font = nullptr; }
        if (g_boldFont) { DeleteObject(g_boldFont); g_boldFont = nullptr; }
        if (g_titleFont) { DeleteObject(g_titleFont); g_titleFont = nullptr; }
        if (g_monoFont) { DeleteObject(g_monoFont); g_monoFont = nullptr; }
        g_theme.Destroy();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const bool startInTray = s2p::HasTrayLaunchFlag(lpCmdLine);
    g_wmShowExisting = RegisterWindowMessageW(L"Screen2PC_Receiver_ShowWindow");
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HANDLE hSingleMutex = CreateMutexW(nullptr, FALSE, L"Local\\Screen2PC_Receiver_SingleInstance");
    if (hSingleMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startInTray) {
            HWND existing = nullptr;
            for (int i = 0; i < 10; ++i) {
                existing = FindWindowW(L"Screen2PCReceiverWnd", nullptr);
                if (existing) break;
                Sleep(50);
            }
            if (existing && g_wmShowExisting != 0) {
                PostMessageW(existing, g_wmShowExisting, 0, 0);
            }
        }
        CloseHandle(hSingleMutex);
        CoUninitialize();
        timeEndPeriod(1);
        return 0;
    }

    s2p::RefreshAutostartPathIfNeeded();

    INITCOMMONCONTROLSEX icc{
        sizeof(icc),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES
    };
    InitCommonControlsEx(&icc);

    s2p::Logger::Instance().Init(L"Screen2PC_Receiver.log");
    s2p::WinsockGuard wsa;
    if (!wsa.Ok()) {
        if (!startInTray) {
            MessageBoxW(nullptr, L"Winsock2 Initialisierung fehlgeschlagen.", L"Screen2PC Receiver", MB_ICONERROR);
        }
        if (hSingleMutex) CloseHandle(hSingleMutex);
        CoUninitialize();
        timeEndPeriod(1);
        return 1;
    }

    s2p::ReceiverEngine engine;
    g_engine = &engine;
    const std::wstring iniPath = GetReceiverIniPath();
    engine.SetConfigIniPath(iniPath);
    engine.SetClipboardEnabled(LoadBoolPref(L"ClipboardSync", 1));
    engine.SetKvmEnabled(LoadBoolPref(L"KvmEnabled", 1));
    engine.SetStreamHudEnabled(LoadBoolPref(L"StreamHudEnabled", 1));

    for (uint32_t i = 0; i < s2p::kMaxMonitors; ++i) {
        engine.SetMonitorColorConfig(i, s2p::LoadMonitorColorConfigIni(iniPath, i));
    }

    engine.SetPinCallbacks(
        [](std::shared_ptr<s2p::PinPopupSession> session) {
            if (g_closing.load() || !g_hwnd || !IsWindow(g_hwnd) || !session) return;
            auto* holder = new std::shared_ptr<s2p::PinPopupSession>(std::move(session));
            if (!PostMessageW(g_hwnd, WM_PIN_SHOW, 0, reinterpret_cast<LPARAM>(holder))) {
                delete holder;
            }
        },
        []() {
            if (!g_closing.load() && g_hwnd && IsWindow(g_hwnd)) {
                PostMessageW(g_hwnd, WM_PIN_DISMISS, 0, 0);
            }
        });

    engine.SetVersionMismatchCallback(
        [](uint16_t remoteProtoVer, const s2p::AppVersionTuple& remoteAppVer, const std::wstring& message) {
            if (g_closing.load() || !g_hwnd || !IsWindow(g_hwnd)) return;
            auto* notice = new VersionMismatchNotice{ remoteProtoVer, remoteAppVer, message };
            if (!PostMessageW(g_hwnd, WM_VERSION_MISMATCH, 0, reinterpret_cast<LPARAM>(notice))) {
                delete notice;
            }
        });

    if (!engine.Start(LoadBoolPref(L"StartFullscreen", 1))) {
        if (!startInTray) {
            MessageBoxW(nullptr, L"Konnte Receiver-Port 48211 nicht oeffnen (evtl. blockiert ein anderer Prozess diesen Port).",
                        L"Screen2PC Receiver", MB_ICONERROR);
        }
        if (hSingleMutex) CloseHandle(hSingleMutex);
        CoUninitialize();
        timeEndPeriod(1);
        return 1;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = s2p::LoadAppLargeIcon(hInst);
    wc.hIconSm = s2p::LoadAppSmallIcon(hInst);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"Screen2PCReceiverWnd";
    RegisterClassExW(&wc);

    std::wstring title = L"Screen2PC Receiver v" + s2p::FormatAppVersion(s2p::CurrentAppVersion()) +
                         L" (PC - Treiberfrei)";
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    UINT sysDpi = GetDpiForSystem();
    if (sysDpi < 72) sysDpi = 96;
    RECT wr{ 0, 0, DpiScale(kMainBaseW, sysDpi), DpiScale(kMainBaseH, sysDpi) };
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, sysDpi);

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, title.c_str(),
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInst, nullptr);

    if (g_hwnd) {
        UINT realDpi = GetWindowDpiSafe(g_hwnd);
        if (realDpi != sysDpi) {
            RECT r2{ 0, 0, DpiScale(kMainBaseW, realDpi), DpiScale(kMainBaseH, realDpi) };
            AdjustWindowRectExForDpi(&r2, style, FALSE, 0, realDpi);
            SetWindowPos(g_hwnd, nullptr, 0, 0, r2.right - r2.left, r2.bottom - r2.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutMainControls(g_hwnd, realDpi);
        }
    }

    if (!startInTray && g_hwnd) {
        ShowWindow(g_hwnd, (nCmdShow == SW_HIDE) ? SW_SHOW : nCmdShow);
        UpdateWindow(g_hwnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_colorWnd && IsWindow(g_colorWnd) && IsDialogMessageW(g_colorWnd, &msg)) {
            continue;
        }
        if (g_pinPopupWnd && IsWindow(g_pinPopupWnd) && IsDialogMessageW(g_pinPopupWnd, &msg)) {
            continue;
        }
        if (g_trustedWnd && IsWindow(g_trustedWnd) && IsDialogMessageW(g_trustedWnd, &msg)) {
            continue;
        }
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    engine.Stop();
    if (hSingleMutex) CloseHandle(hSingleMutex);
    CoUninitialize();
    timeEndPeriod(1);
    return 0;
}
