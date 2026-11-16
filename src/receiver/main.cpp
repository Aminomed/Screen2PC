#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../common/Autostart.h"
#include "../common/ColorProfile.h"
#include "../common/Logger.h"
#include "../common/NetSocket.h"
#include "../common/Updater.h"
#include "ReceiverEngine.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr UINT WM_TRAYICON          = WM_APP + 10;
constexpr UINT WM_UPDATE_CHECK_DONE = WM_APP + 11;
constexpr UINT WM_UPDATE_DL_DONE    = WM_APP + 12;
constexpr UINT WM_DISCONNECT_DONE   = WM_APP + 13;
constexpr UINT_PTR TIMER_TELEMETRY  = 1;

constexpr int IDC_INFO_LABEL     = 201;
constexpr int IDC_CHK_FULLSCR    = 202;
constexpr int IDC_BTN_DISCONN    = 203;
constexpr int IDC_STATS_TEXT     = 204;
constexpr int IDC_BTN_FIREWALL   = 205;
constexpr int IDC_CHK_CLIPBOARD  = 206;
constexpr int IDC_BTN_SENDFILE   = 207;
constexpr int IDC_BTN_OPEN_DL    = 208;
constexpr int IDC_CHK_KVM        = 209;
constexpr int IDC_CHK_AUTOSTART  = 210;
constexpr int IDC_CHK_AUTOUPDATE = 211;
constexpr int IDC_BTN_COLORS     = 212;
constexpr int IDC_BTN_UPDATE     = 213;

constexpr int IDM_SHOW         = 3001;
constexpr int IDM_EXIT         = 3002;
constexpr int IDM_CHECK_UPDATE = 3003;

// Color calibration window control IDs
constexpr int IDC_CLR_MON_COMBO  = 401;
constexpr int IDC_CLR_HDR_INFO   = 402;
constexpr int IDC_CLR_CHK_HDR    = 403;
constexpr int IDC_CLR_TB_BRIGHT  = 410;
constexpr int IDC_CLR_TB_GAMMA   = 411;
constexpr int IDC_CLR_TB_CONTRAST= 412;
constexpr int IDC_CLR_TB_SAT     = 413;
constexpr int IDC_CLR_TB_RED     = 414;
constexpr int IDC_CLR_TB_GREEN   = 415;
constexpr int IDC_CLR_TB_BLUE    = 416;
constexpr int IDC_CLR_VAL_BRIGHT = 420;
constexpr int IDC_CLR_VAL_GAMMA  = 421;
constexpr int IDC_CLR_VAL_CONTRAST= 422;
constexpr int IDC_CLR_VAL_SAT    = 423;
constexpr int IDC_CLR_VAL_RED    = 424;
constexpr int IDC_CLR_VAL_GREEN  = 425;
constexpr int IDC_CLR_VAL_BLUE   = 426;
constexpr int IDC_CLR_BTN_RESET  = 430;
constexpr int IDC_CLR_BTN_CLOSE  = 431;

struct UpdateCheckResult {
    s2p::UpdateReleaseInfo info;
    bool userInitiated = false;
};

struct UpdateDownloadResult {
    bool ok = false;
    std::wstring error;
};

s2p::ReceiverEngine* g_engine = nullptr;
HWND g_hwnd = nullptr;
HWND g_colorWnd = nullptr;
HFONT g_font = nullptr;
HFONT g_monoFont = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_wmShowExisting = 0;

std::thread g_disconnThread;
std::thread g_updateThread;
std::atomic<bool> g_closing{false};
std::atomic<bool> g_disconnBusy{false};
std::atomic<bool> g_updateBusy{false};
bool g_promptOpen = false;
s2p::UpdateReleaseInfo g_pendingUpdate{};

uint32_t g_selectedColorMonitor = UINT32_MAX;
bool g_colorPopulating = false;

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

std::wstring DefaultUpdateButtonText() {
    if (g_pendingUpdate.valid && g_pendingUpdate.updateAvailable) {
        return L"Update verfuegbar: v" + s2p::FormatAppVersion(g_pendingUpdate.version);
    }
    return L"Nach Updates suchen (v" + s2p::FormatAppVersion(s2p::CurrentAppVersion()) + L")";
}

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

LRESULT CALLBACK ColorWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) -> HWND {
            HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
            SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
            return c;
        };

        addCtrl(L"STATIC", L"PC-Monitor:", 0, 16, 16, 90, 20, 0);
        addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 112, 12, 342, 160, IDC_CLR_MON_COMBO);
        addCtrl(L"STATIC", L"", 0, 16, 44, 438, 34, IDC_CLR_HDR_INFO);

        addCtrl(L"BUTTON", L"HDR scRGB-Ausgabe && Gamma-2.2-Korrektur (empfohlen fuer HDR)",
                BS_AUTOCHECKBOX | WS_TABSTOP, 16, 84, 438, 22, IDC_CLR_CHK_HDR);

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

        int y = 116;
        for (const auto& r : rows) {
            addCtrl(L"STATIC", r.name, 0, 16, y + 4, 106, 20, 0);
            HWND tb = addCtrl(TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP, 124, y, 260, 28, r.tbId);
            SendMessageW(tb, TBM_SETRANGE, FALSE, MAKELONG(r.minV, r.maxV));
            SendMessageW(tb, TBM_SETTICFREQ, r.ticFreq, 0);
            addCtrl(L"STATIC", L"100 %", SS_RIGHT, 390, y + 4, 62, 20, r.lblId);
            y += 34;
        }

        addCtrl(L"BUTTON", L"Standardwerte wiederherstellen", BS_PUSHBUTTON | WS_TABSTOP, 16, y + 10, 240, 30, IDC_CLR_BTN_RESET);
        addCtrl(L"BUTTON", L"Schliessen", BS_DEFPUSHBUTTON | WS_TABSTOP, 334, y + 10, 120, 30, IDC_CLR_BTN_CLOSE);
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
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Screen2PCColorWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    g_colorWnd = CreateWindowExW(
        0, L"Screen2PCColorWnd",
        L"Farbe & HDR pro Monitor (Treiberfrei / Echtzeit)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, 480, 442,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (g_colorWnd) {
        PopulateColorDialog();
        ShowWindow(g_colorWnd, SW_SHOW);
        UpdateWindow(g_colorWnd);
    }
}

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

void UpdateUi() {
    if (!g_hwnd || !g_engine) return;
    auto st = g_engine->GetStatus();

    std::wostringstream hdr;
    hdr << L"Status: " << st.stateText << L"\r\nPC-IPs: ";
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
    SetDlgItemTextW(g_hwnd, IDC_INFO_LABEL, hdr.str().c_str());
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_DISCONN), (st.sessionActive && !g_disconnBusy.load()) ? TRUE : FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_SENDFILE), st.fileStats.active ? TRUE : FALSE);

    bool kvmOn = g_engine->IsKvmEnabled();
    LRESULT chkState = SendDlgItemMessageW(g_hwnd, IDC_CHK_KVM, BM_GETCHECK, 0, 0);
    if ((chkState == BST_CHECKED) != kvmOn) {
        SendDlgItemMessageW(g_hwnd, IDC_CHK_KVM, BM_SETCHECK, kvmOn ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    std::wostringstream body;
    if (!st.sessionActive) {
        body << L"Keine aktive Session. Starte Screen2PC.Sender.exe auf dem Surface.\r\n"
             << L"Hinweis: Dieser Empfaenger arbeitet 100% im User-Mode (kein Treiber, kein Test-Signing, Anti-Cheat-sicher).\r\n\r\n"
             << L"Letzte Log-Meldungen:\r\n";
        auto logs = s2p::Logger::Instance().GetRecentLines(5);
        for (const auto& l : logs) body << l << L"\r\n";
    } else {
        for (const auto& s : st.streams) {
            body << L"Display " << (s.monitorIndex + 1) << L" [" << s.width << L"x" << s.height << L"]  "
                 << s.codecName << L"  |  "
                 << std::fixed << std::setprecision(1) << s.fps << L" FPS  |  "
                 << std::setprecision(1) << s.mbps << L" Mbit/s  |  "
                 << L"Frames: " << s.framesReceived << L"\r\n";
        }
        body << L"Dateien (Drag & Drop): Gesendet " << st.fileStats.filesSent
             << L" | Empfangen " << st.fileStats.filesReceived;
        if (!st.fileStats.lastEvent.empty()) {
            body << L" | " << st.fileStats.lastEvent;
        }
        body << L"\r\n";
    }
    SetDlgItemTextW(g_hwnd, IDC_STATS_TEXT, body.str().c_str());
}

void OpenFirewallPortsElevated() {
    const wchar_t* cmdArgs =
        L"/c netsh advfirewall firewall delete rule name=\"Screen2PC\" >nul 2>&1 & "
        L"netsh advfirewall firewall delete rule name=\"Screen2PC Discovery\" >nul 2>&1 & "
        L"netsh advfirewall firewall add rule name=\"Screen2PC\" dir=in action=allow protocol=TCP localport=48211-48215 & "
        L"netsh advfirewall firewall add rule name=\"Screen2PC Discovery\" dir=in action=allow protocol=UDP localport=48210";
    ShellExecuteW(g_hwnd, L"runas", L"cmd.exe", cmdArgs, nullptr, SW_HIDE);
}

void CreateUiControls(HWND hwnd) {
    g_font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_monoFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, HFONT font = nullptr) -> HWND {
        HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_font), TRUE);
        return c;
    };

    addCtrl(L"STATIC", L"Initialisiere...", 0, 16, 12, 520, 68, IDC_INFO_LABEL);

    HWND chk = addCtrl(L"BUTTON", L"Streams direkt im randlosen Vollbild auf PC-Monitoren oeffnen (F11 zum Umschalten)",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 16, 86, 520, 22, IDC_CHK_FULLSCR);
    SendMessageW(chk, BM_SETCHECK, LoadBoolPref(L"StartFullscreen", 1) ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND chkClip = addCtrl(L"BUTTON", L"Geteilte Zwischenablage (Text && Bilder zwischen Surface <-> PC)",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 16, 112, 520, 22, IDC_CHK_CLIPBOARD);
    bool clipPref = LoadBoolPref(L"ClipboardSync", 1);
    SendMessageW(chkClip, BM_SETCHECK, clipPref ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_engine) g_engine->SetClipboardEnabled(clipPref);

    HWND chkKvm = addCtrl(L"BUTTON", L"KVM: Maus && Tastatur ans Surface senden (Strg+Alt+K im Streamfenster)",
                          BS_AUTOCHECKBOX | WS_TABSTOP, 16, 138, 520, 22, IDC_CHK_KVM);
    bool kvmPref = LoadBoolPref(L"KvmEnabled", 1);
    SendMessageW(chkKvm, BM_SETCHECK, kvmPref ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_engine) g_engine->SetKvmEnabled(kvmPref);

    HWND chkAuto = addCtrl(L"BUTTON", L"Mit Windows starten (minimiert im System-Tray, Task-Manager Autostart)",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 16, 164, 520, 22, IDC_CHK_AUTOSTART);
    SendMessageW(chkAuto, BM_SETCHECK, s2p::IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND chkUpd = addCtrl(L"BUTTON", L"Beim Start automatisch auf GitHub nach Updates suchen",
                          BS_AUTOCHECKBOX | WS_TABSTOP, 16, 190, 520, 22, IDC_CHK_AUTOUPDATE);
    SendMessageW(chkUpd, BM_SETCHECK, LoadBoolPref(L"AutoUpdateCheck", 1) ? BST_CHECKED : BST_UNCHECKED, 0);

    addCtrl(L"BUTTON", L"Aktive Session trennen", BS_PUSHBUTTON | WS_TABSTOP, 16, 220, 254, 30, IDC_BTN_DISCONN);
    addCtrl(L"BUTTON", L"Windows Firewall Ports freigeben (Admin)", BS_PUSHBUTTON | WS_TABSTOP, 282, 220, 254, 30, IDC_BTN_FIREWALL);

    addCtrl(L"BUTTON", L"Datei(en) ans Surface senden...", BS_PUSHBUTTON | WS_TABSTOP, 16, 256, 254, 28, IDC_BTN_SENDFILE);
    addCtrl(L"BUTTON", L"Downloads\\Screen2PC oeffnen", BS_PUSHBUTTON | WS_TABSTOP, 282, 256, 254, 28, IDC_BTN_OPEN_DL);

    addCtrl(L"BUTTON", L"Farbe & HDR pro Monitor anpassen...", BS_PUSHBUTTON | WS_TABSTOP, 16, 290, 254, 28, IDC_BTN_COLORS);
    addCtrl(L"BUTTON", DefaultUpdateButtonText().c_str(), BS_PUSHBUTTON | WS_TABSTOP, 282, 290, 254, 28, IDC_BTN_UPDATE);

    addCtrl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 16, 326, 520, 148, IDC_STATS_TEXT, g_monoFont);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Statusfenster oeffnen");
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

    switch (msg) {
    case WM_CREATE:
        CreateUiControls(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        SetTimer(hwnd, TIMER_TELEMETRY, 500, nullptr);

        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(g_nid.szTip, L"Screen2PC Receiver (PC)");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        UpdateUi();
        if (LoadBoolPref(L"AutoUpdateCheck", 1)) {
            StartAsyncUpdateCheck(false);
        }
        return 0;

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) {
            SendDlgItemMessageW(hwnd, IDC_CHK_AUTOSTART, BM_SETCHECK,
                                s2p::IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        }
        break;

    case WM_DISPLAYCHANGE:
        if (g_engine) {
            g_engine->RefreshLocalMonitorInfo();
            UpdateUi();
            if (g_colorWnd && IsWindow(g_colorWnd)) {
                PopulateColorDialog();
            }
        }
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CHK_FULLSCR:
            if (g_engine) {
                bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_FULLSCR, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_engine->SetStartFullscreen(checked);
                SaveBoolPref(L"StartFullscreen", checked);
            }
            return 0;
        case IDC_CHK_CLIPBOARD:
            if (g_engine) {
                bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_CLIPBOARD, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_engine->SetClipboardEnabled(checked);
                SaveBoolPref(L"ClipboardSync", checked);
            }
            return 0;
        case IDC_CHK_KVM:
            if (g_engine) {
                bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_KVM, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_engine->SetKvmEnabled(checked);
                SaveBoolPref(L"KvmEnabled", checked);
            }
            return 0;
        case IDC_CHK_AUTOSTART: {
            bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_AUTOSTART, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!s2p::SetAutostartEnabled(checked)) {
                SendDlgItemMessageW(hwnd, IDC_CHK_AUTOSTART, BM_SETCHECK,
                                    s2p::IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
                MessageBoxW(hwnd, L"Autostart-Eintrag in der Registry konnte nicht geschrieben werden.",
                            L"Screen2PC Receiver", MB_ICONWARNING);
            }
            return 0;
        }
        case IDC_CHK_AUTOUPDATE: {
            bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_AUTOUPDATE, BM_GETCHECK, 0, 0) == BST_CHECKED;
            SaveBoolPref(L"AutoUpdateCheck", checked);
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
        case IDC_BTN_FIREWALL:
            OpenFirewallPortsElevated();
            return 0;
        case IDC_BTN_SENDFILE:
            if (g_engine) {
                auto files = PickFilesDialog(hwnd);
                if (!files.empty()) g_engine->SendFiles(files);
            }
            return 0;
        case IDC_BTN_OPEN_DL:
            ShellExecuteW(hwnd, L"open", s2p::FileTransfer::DefaultDownloadDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        case IDC_BTN_COLORS:
            OpenColorSettingsWindow(hwnd);
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
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_engine) g_engine->Stop();
        if (g_disconnThread.joinable()) g_disconnThread.join();
        if (g_updateThread.joinable()) g_updateThread.join();
        if (g_font) DeleteObject(g_font);
        if (g_monoFont) DeleteObject(g_monoFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR lpCmdLine, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const bool startInTray = s2p::HasTrayLaunchFlag(lpCmdLine);
    g_wmShowExisting = RegisterWindowMessageW(L"Screen2PC_Receiver_ShowWindow");

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

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES };
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
    engine.SetClipboardEnabled(LoadBoolPref(L"ClipboardSync", 1));
    engine.SetKvmEnabled(LoadBoolPref(L"KvmEnabled", 1));

    const std::wstring iniPath = GetReceiverIniPath();
    for (uint32_t i = 0; i < s2p::kMaxMonitors; ++i) {
        engine.SetMonitorColorConfig(i, s2p::LoadMonitorColorConfigIni(iniPath, i));
    }

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
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"Screen2PCReceiverWnd";
    RegisterClassExW(&wc);

    std::wstring title = L"Screen2PC Receiver v" + s2p::FormatAppVersion(s2p::CurrentAppVersion()) +
                         L" (PC - Treiberfrei)";

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, title.c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 570, 530,
        nullptr, nullptr, hInst, nullptr);

    if (!startInTray) {
        ShowWindow(g_hwnd, (nCmdShow == SW_HIDE) ? SW_SHOW : nCmdShow);
        UpdateWindow(g_hwnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (g_colorWnd && IsWindow(g_colorWnd) && IsDialogMessageW(g_colorWnd, &msg)) {
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
