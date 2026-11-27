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
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../common/AppIcon.h"
#include "../common/Autostart.h"
#include "../common/BitratePresets.h"
#include "../common/Lang.h"
#include "../common/Logger.h"
#include "../common/NetSocket.h"
#include "../common/Pairing.h"
#include "../common/UiTheme.h"
#include "../common/Updater.h"
#include "PowerPolicy.h"
#include "SenderEngine.h"
#include "VirtualDisplayCtl.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr UINT WM_TRAYICON          = WM_APP + 1;
constexpr UINT WM_DISCOVERY_DONE    = WM_APP + 2;
constexpr UINT WM_CONNECT_DONE      = WM_APP + 3;
constexpr UINT WM_UPDATE_CHECK_DONE = WM_APP + 4;
constexpr UINT WM_UPDATE_DL_DONE    = WM_APP + 5;
constexpr UINT WM_SHOW_PIN_PROMPT   = WM_APP + 6;
constexpr UINT WM_APP_DIM_DISMISSED  = WM_APP + 7;
constexpr UINT WM_REMOTE_CONNECT_REQ = WM_APP + 8;

constexpr UINT_PTR TIMER_TELEMETRY  = 1;
constexpr UINT_PTR TIMER_TRAY_RETRY = 2;
constexpr int HOTKEY_TOGGLE_CONNECT = 1001;

constexpr int IDC_PEER_COMBO        = 101;
constexpr int IDC_BTN_SCAN          = 102;
constexpr int IDC_IP_EDIT           = 103;
constexpr int IDC_MODE_COMBO        = 104;
constexpr int IDC_CODEC_COMBO       = 105;
constexpr int IDC_BITRATE_COMBO     = 106;
constexpr int IDC_BTN_CONNECT       = 107;
constexpr int IDC_STATUS_LABEL      = 108;
constexpr int IDC_DRIVER_LABEL      = 109;
constexpr int IDC_STATS_TEXT        = 110;
constexpr int IDC_CHK_CLIPBOARD     = 111;
constexpr int IDC_BTN_SENDFILE      = 112;
constexpr int IDC_BTN_OPEN_DL       = 113;
constexpr int IDC_CHK_KVM           = 114;
constexpr int IDC_CHK_AUTOUPDATE    = 115;
constexpr int IDC_BTN_UPDATE        = 116;
constexpr int IDC_CHK_AUTOSTART     = 117;
constexpr int IDC_FPS_COMBO         = 118;
constexpr int IDC_CHK_AUTOCONNECT   = 119;
constexpr int IDC_CHK_AUTORECONNECT = 120;
constexpr int IDC_CHK_LID_BYPASS    = 121;
constexpr int IDC_CHK_DIM_SURFACE   = 122;
constexpr int IDC_BTN_SENDFOLDER    = 123;
constexpr int IDC_FILE_PROGRESS     = 124;
constexpr int IDC_BTN_CANCELFILE    = 125;
constexpr int IDC_FILE_STATUS       = 126;
constexpr int IDC_CHK_ALLOW_REMOTE  = 127;
constexpr int IDC_BTN_TRUSTED_PCS   = 128;

constexpr int IDC_LBL_PEERS         = 150;
constexpr int IDC_LBL_IP            = 151;
constexpr int IDC_LBL_MODE          = 152;
constexpr int IDC_LBL_STREAM        = 153;
constexpr int IDC_HERO_TITLE        = 154;
constexpr int IDC_LBL_TELEMETRY     = 155;
constexpr int IDC_DROP_TITLE        = 156;
constexpr int IDC_DROP_SUB          = 157;
constexpr int IDC_LBL_LANG          = 158;
constexpr int IDC_LANG_COMBO        = 159;

constexpr int IDC_TAB_CONNECT       = 170;
constexpr int IDC_TAB_TRANSFER      = 171;
constexpr int IDC_TAB_SETTINGS      = 172;

// PIN prompt dialog controls
constexpr int IDC_PIN_INFO          = 501;
constexpr int IDC_PIN_STATUS        = 502;
constexpr int IDC_PIN_EDIT          = 503;

// Trusted PCs dialog controls
constexpr int IDC_TPC_INFO          = 601;
constexpr int IDC_TPC_LIST          = 602;
constexpr int IDC_TPC_BTN_REMOVE    = 603;
constexpr int IDC_TPC_BTN_CLEAR     = 604;
constexpr int IDC_TPC_BTN_REFRESH   = 605;
constexpr int IDC_TPC_BTN_CLOSE     = 606;

constexpr int IDM_SHOW              = 2001;
constexpr int IDM_TOGGLE            = 2002;
constexpr int IDM_EXIT              = 2003;
constexpr int IDM_CHECK_UPDATE      = 2004;

constexpr int kBaseClientW    = 540;
constexpr int kBaseClientH    = 660;
constexpr int kPinBaseW       = 430;
constexpr int kPinBaseH       = 244;
constexpr int kTrustedPcBaseW = 590;
constexpr int kTrustedPcBaseH = 380;

constexpr DWORD kReconnectBackoffsMs[] = { 2000, 5000, 10000, 20000 };

struct UpdateCheckResult {
    s2p::UpdateReleaseInfo info;
    bool userInitiated = false;
};

struct UpdateDownloadResult {
    bool ok = false;
    std::wstring error;
};

struct RemoteConnectRequestMsg {
    std::string pcIp;
    std::wstring pcName;
};

struct SenderPinRequest {
    std::wstring pcName;
    std::string pcIp;
    s2p::AppVersionTuple pcVersion{};
    uint8_t remainingAttempts = 3;
    std::wstring message;
    std::string outPin;
    bool submitted = false;
    bool done = false;
    bool canceled = false;
    std::mutex mtx;
    std::condition_variable cv;
};

s2p::SenderEngine* g_engine = nullptr;
HWND g_hwnd = nullptr;
HWND g_pinPromptWnd = nullptr;
HWND g_trustedPcsWnd = nullptr;
HWND g_tooltip = nullptr;
HFONT g_font = nullptr;
HFONT g_boldFont = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_monoFont = nullptr;
HFONT g_pinFont = nullptr;
HFONT g_pinBigFont = nullptr;
HFONT g_trustedPcsFont = nullptr;
UINT g_currentDpi = 96;
UINT g_pinDpi = 96;
std::vector<s2p::TrustedDeviceEntry> g_trustedPcsCache;

s2p::ui::ThemePalette g_theme;
s2p::ui::StatusTone g_heroTone = s2p::ui::StatusTone::Neutral;
int g_activeTab = 0; // 0 = Verbindung, 1 = Dateitransfer, 2 = Einstellungen

NOTIFYICONDATAW g_nid{};
bool g_trayIconAdded = false;
int g_trayRetryCount = 0;
UINT g_wmShowExisting = 0;
UINT g_wmTaskbarCreated = 0;

std::vector<s2p::DiscoveredPeer> g_peers;
bool g_scanning = false;
std::thread g_scanThread;
std::thread g_connectThread;
std::thread g_updateThread;
std::atomic<bool> g_closing{false};
std::atomic<bool> g_connectBusy{false};
std::atomic<bool> g_updateBusy{false};
bool g_promptOpen = false;
s2p::UpdateReleaseInfo g_pendingUpdate{};

std::mutex g_pinReqMutex;
std::shared_ptr<SenderPinRequest> g_activePinReq;
std::shared_ptr<SenderPinRequest> g_uiPinReq;

// Auto-connect & Auto-reconnect state (UI thread only)
bool g_initialAutoConnectHandled = false;
bool g_userManualAction = false;
bool g_userManualDisconnect = false;
bool g_wasConnected = false;
bool g_reconnectArmed = false;
int g_reconnectStep = 0;
ULONGLONG g_nextReconnectTick = 0;
bool g_versionMismatchPrompted = false;

// Surface power & dim policy (UI thread only)
s2p::LidCloseGuard g_lidGuard;
s2p::SurfaceDimOverlay g_dimOverlay;
bool g_dimSuppressAuto = false;
bool g_dimAttempted = false;
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

bool IsSenderCardControlId(int id) {
    switch (id) {
    case IDC_HERO_TITLE:
    case IDC_STATUS_LABEL:
    case IDC_DRIVER_LABEL:
    case IDC_LBL_PEERS:
    case IDC_LBL_IP:
    case IDC_LBL_MODE:
    case IDC_LBL_STREAM:
    case IDC_LBL_TELEMETRY:
    case IDC_DROP_TITLE:
    case IDC_DROP_SUB:
    case IDC_FILE_STATUS:
    case IDC_LBL_LANG:
        return true;
    default:
        return false;
    }
}

void ApplySenderTabVisibility(HWND hwnd) {
    auto vis = [&](int id, bool show) {
        HWND c = GetDlgItem(hwnd, id);
        if (c) ShowWindow(c, show ? SW_SHOW : SW_HIDE);
    };
    const bool t0 = (g_activeTab == 0);
    const bool t1 = (g_activeTab == 1);
    const bool t2 = (g_activeTab == 2);

    vis(IDC_LBL_PEERS, t0);
    vis(IDC_PEER_COMBO, t0);
    vis(IDC_BTN_SCAN, t0);
    vis(IDC_LBL_IP, t0);
    vis(IDC_IP_EDIT, t0);
    vis(IDC_LBL_MODE, t0);
    vis(IDC_MODE_COMBO, t0);
    vis(IDC_LBL_STREAM, t0);
    vis(IDC_CODEC_COMBO, t0);
    vis(IDC_BITRATE_COMBO, t0);
    vis(IDC_FPS_COMBO, t0);
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

    vis(IDC_CHK_CLIPBOARD, t2);
    vis(IDC_CHK_KVM, t2);
    vis(IDC_CHK_AUTOSTART, t2);
    vis(IDC_CHK_AUTOCONNECT, t2);
    vis(IDC_CHK_AUTORECONNECT, t2);
    vis(IDC_CHK_LID_BYPASS, t2);
    vis(IDC_CHK_DIM_SURFACE, t2);
    vis(IDC_CHK_AUTOUPDATE, t2);
    vis(IDC_CHK_ALLOW_REMOTE, t2);
    vis(IDC_LBL_LANG, t2);
    vis(IDC_LANG_COMBO, t2);
    vis(IDC_BTN_TRUSTED_PCS, t2);
    vis(IDC_BTN_UPDATE, t2);

    InvalidateRect(GetDlgItem(hwnd, IDC_TAB_CONNECT), nullptr, FALSE);
    InvalidateRect(GetDlgItem(hwnd, IDC_TAB_TRANSFER), nullptr, FALSE);
    InvalidateRect(GetDlgItem(hwnd, IDC_TAB_SETTINGS), nullptr, FALSE);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void EnsureSenderRemoteFirewallRuleAsync() {
    static std::atomic<bool> s_fwInFlight{false};
    if (s_fwInFlight.exchange(true)) return;
    std::thread([]() {
        auto runHidden = [](const std::wstring& cmdLine) -> DWORD {
            std::vector<wchar_t> buf(cmdLine.begin(), cmdLine.end());
            buf.push_back(L'\0');
            STARTUPINFOW si{};
            si.cb = sizeof(si);
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_HIDE;
            PROCESS_INFORMATION pi{};
            if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                                CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                return 1;
            }
            WaitForSingleObject(pi.hProcess, 5000);
            DWORD code = 1;
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return code;
        };

        if (runHidden(L"netsh.exe advfirewall firewall show rule name=\"Screen2PC Sender Remote\"") != 0) {
            runHidden(L"netsh.exe advfirewall firewall add rule name=\"Screen2PC Sender Remote\" dir=in action=allow protocol=UDP localport=" +
                      std::to_wstring(s2p::kPortRemoteConnect));
        }
        s_fwInFlight.store(false);
    }).detach();
}

void ApplySenderTheme(HWND hwnd, bool dark) {
    g_theme.Init(dark);
    s2p::ui::ApplyWindowTheme(hwnd, dark);
    s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_PEER_COMBO), dark, L"DarkMode_CFD");
    s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_MODE_COMBO), dark, L"DarkMode_CFD");
    s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_CODEC_COMBO), dark, L"DarkMode_CFD");
    s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_BITRATE_COMBO), dark, L"DarkMode_CFD");
    s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_FPS_COMBO), dark, L"DarkMode_CFD");
    s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_LANG_COMBO), dark, L"DarkMode_CFD");
    s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_IP_EDIT), dark);
    s2p::ui::ApplyChildTheme(GetDlgItem(hwnd, IDC_STATS_TEXT), dark);
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

std::wstring GetIniPath() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) + L"\\Screen2PC" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\sender.ini";
}

void SaveSetting(const wchar_t* key, int val) {
    WritePrivateProfileStringW(L"Sender", key, std::to_wstring(val).c_str(), GetIniPath().c_str());
}

int LoadSetting(const wchar_t* key, int defVal) {
    return GetPrivateProfileIntW(L"Sender", key, defVal, GetIniPath().c_str());
}

void SaveLastIp(const std::wstring& ip) {
    WritePrivateProfileStringW(L"Sender", L"LastIp", ip.c_str(), GetIniPath().c_str());
}

std::wstring LoadLastIp() {
    wchar_t buf[128]{};
    GetPrivateProfileStringW(L"Sender", L"LastIp", L"", buf, _countof(buf), GetIniPath().c_str());
    return std::wstring(buf);
}

std::wstring TrimWide(const std::wstring& s) {
    size_t a = 0;
    while (a < s.size() && std::iswspace(s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::iswspace(s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::wstring DefaultUpdateButtonText() {
    if (g_pendingUpdate.valid && g_pendingUpdate.updateAvailable) {
        return L"Update verfuegbar: v" + s2p::FormatAppVersion(g_pendingUpdate.version);
    }
    return L"Nach Updates suchen (v" + s2p::FormatAppVersion(s2p::CurrentAppVersion()) + L")";
}

void ReadStreamSettingsFromUi(HWND hwnd,
                              s2p::CodecType& outCodec,
                              uint32_t& outBitrateMbps,
                              uint32_t& outTargetFps,
                              int& outCodecIdx,
                              int& outBrIdx,
                              int& outFpsIdx) {
    outCodecIdx = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_CODEC_COMBO, CB_GETCURSEL, 0, 0));
    if (outCodecIdx == 1) outCodec = s2p::CodecType::HEVC;
    else if (outCodecIdx == 2) outCodec = s2p::CodecType::DeltaLossless;
    else outCodec = s2p::CodecType::H264;

    outBrIdx = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_BITRATE_COMBO, CB_GETCURSEL, 0, 0));
    outBitrateMbps = s2p::BitrateFromPresetIndex(outBrIdx);

    outFpsIdx = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_FPS_COMBO, CB_GETCURSEL, 0, 0));
    outTargetFps = s2p::FpsFromPresetIndex(outFpsIdx);
}

void ApplyLiveStreamReconfigureIfConnected(HWND hwnd) {
    s2p::CodecType codec = s2p::CodecType::H264;
    uint32_t bitrateMbps = s2p::kDefaultBitrateMbps;
    uint32_t targetFps = 0;
    int codecIdx = 0, brIdx = s2p::kDefaultBitratePresetIdx, fpsIdx = 0;
    ReadStreamSettingsFromUi(hwnd, codec, bitrateMbps, targetFps, codecIdx, brIdx, fpsIdx);

    SaveSetting(L"CodecIdx", codecIdx);
    SaveSetting(L"BitrateMbps", static_cast<int>(bitrateMbps));
    SaveSetting(L"BitrateIdx", brIdx);
    SaveSetting(L"FpsSetting", static_cast<int>(targetFps));
    SaveSetting(L"FpsIdx", fpsIdx);

    if (g_engine && g_engine->IsConnected()) {
        g_engine->ReconfigureStream(codec, bitrateMbps, targetFps);
    }
}

bool AddTrayIcon(HWND hwnd) {
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = s2p::LoadAppSmallIcon(GetModuleHandleW(nullptr));
    wcscpy_s(g_nid.szTip, L"Screen2PC Sender (Laptop)");
    if (g_trayIconAdded && Shell_NotifyIconW(NIM_MODIFY, &g_nid)) {
        return true;
    }
    g_trayIconAdded = (Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE);
    return g_trayIconAdded;
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
        << L"(Der Virtual Display Treiber wird nur bei Aenderungen aktualisiert. Die App startet automatisch neu.)";

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
        dlRes->ok = s2p::DownloadAndLaunchSelfUpdate(copyInfo, false, true, dlRes->error);
        if (!g_closing.load() && g_hwnd) {
            PostMessageW(g_hwnd, WM_UPDATE_DL_DONE, 0, reinterpret_cast<LPARAM>(dlRes));
        } else {
            delete dlRes;
            g_updateBusy = false;
        }
    });
}

void SyncSurfacePowerAndDim(const s2p::SenderStatus& st) {
    const std::wstring iniPath = GetIniPath();
    const bool extActive = st.connected && st.usingExternalDisplays && !g_closing.load();

    bool lidChecked = IsToggleChecked(g_hwnd, IDC_CHK_LID_BYPASS);
    if (extActive && lidChecked) {
        g_lidGuard.Engage(iniPath);
    } else if (g_lidGuard.IsEngaged()) {
        g_lidGuard.Restore(iniPath);
    }

    bool dimChecked = IsToggleChecked(g_hwnd, IDC_CHK_DIM_SURFACE);
    if (extActive && dimChecked && !g_dimSuppressAuto) {
        if (!g_dimOverlay.IsVisible() && !g_dimAttempted && g_engine) {
            g_dimAttempted = true;
            auto vRects = g_engine->GetActiveVirtualOutputRects();
            g_dimOverlay.Show(vRects, []() {
                if (!g_closing.load() && g_hwnd) {
                    PostMessageW(g_hwnd, WM_APP_DIM_DISMISSED, 0, 0);
                }
            });
        }
    } else {
        if (g_dimOverlay.IsVisible()) {
            g_dimOverlay.Hide(false);
        }
        if (!extActive) {
            g_dimSuppressAuto = false;
            g_dimAttempted = false;
        }
    }
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
        return L"Dateitransfer bereit (Dateien oder Ordner per Drag & Drop ins Fenster ziehen)";
    }
    return L"Dateitransfer: Nur bei aktiver Verbindung verfuegbar";
}

void UpdateUiFromState() {
    if (!g_hwnd || !g_engine) return;
    auto st = g_engine->GetStatus();

    if (st.connected) {
        g_wasConnected = true;
        g_reconnectArmed = false;
        g_reconnectStep = 0;
        g_versionMismatchPrompted = false;
    } else if (g_wasConnected && !st.connecting) {
        g_wasConnected = false;
        g_dimAttempted = false;
        g_dimSuppressAuto = false;
        if (!g_userManualDisconnect &&
            !st.remoteStopped &&
            !st.authRejected &&
            !st.versionMismatch &&
            !g_closing.load() &&
            IsToggleChecked(g_hwnd, IDC_CHK_AUTORECONNECT)) {
            g_reconnectArmed = true;
            g_reconnectStep = 0;
            g_nextReconnectTick = GetTickCount64() + kReconnectBackoffsMs[0];
        }
    }

    SyncSurfacePowerAndDim(st);

    s2p::ui::StatusTone newTone = s2p::ui::StatusTone::Neutral;
    std::wstring heroTitle = L"Bereit zum Verbinden";
    if (st.connected) {
        newTone = s2p::ui::StatusTone::Success;
        heroTitle = L"Verbunden";
        if (!st.streams.empty()) {
            double totalMbps = 0.0;
            for (const auto& s : st.streams) totalMbps += s.mbps;
            std::wostringstream hs;
            hs << L"Verbunden  (" << st.streams.size() << L" Display"
               << (st.streams.size() > 1 ? L"s" : L"")
               << L"  \u2022  " << std::fixed << std::setprecision(1) << st.streams[0].fps << L" FPS"
               << L"  \u2022  " << std::setprecision(1) << totalMbps << L" Mbit/s)";
            heroTitle = hs.str();
        }
    } else if (st.connecting || g_reconnectArmed) {
        newTone = s2p::ui::StatusTone::Warning;
        heroTitle = st.connecting ? L"Verbindungsaufbau..." : L"Wiederverbindung geplant...";
    } else if (!st.lastError.empty()) {
        newTone = s2p::ui::StatusTone::Error;
        heroTitle = L"Verbindung fehlgeschlagen";
    }
    if (newTone != g_heroTone) {
        g_heroTone = newTone;
        RECT dotRc{ 0, 0, DpiScale(60, g_currentDpi), DpiScale(128, g_currentDpi) };
        InvalidateRect(g_hwnd, &dotRc, FALSE);
    }
    SetDlgItemTextW(g_hwnd, IDC_HERO_TITLE, heroTitle.c_str());

    SetDlgItemTextW(g_hwnd, IDC_DRIVER_LABEL,
        st.driverActive
            ? L"VDD-Treiber: Aktiv (0..2 virtuelle Monitore bereit)"
            : L"VDD-Treiber: Nicht installiert (nur Mirror-Modus)");

    std::wstring statusLine = st.stateText;
    if (g_reconnectArmed && !st.connected && !st.connecting) {
        ULONGLONG now = GetTickCount64();
        long remSec = (g_nextReconnectTick > now) ? static_cast<long>((g_nextReconnectTick - now + 999) / 1000) : 0;
        statusLine = L"Unterbrochen \u2014 Wiederverbindung in " + std::to_wstring(remSec) + L" s...";
    }
    if (!st.lastError.empty()) {
        statusLine += L"  |  " + st.lastError;
    }
    SetDlgItemTextW(g_hwnd, IDC_STATUS_LABEL, statusLine.c_str());

    if (st.connected) {
        SetDlgItemTextW(g_hwnd, IDC_BTN_CONNECT, L"Trennen (Ctrl+Shift+K)");
    } else if (g_reconnectArmed) {
        SetDlgItemTextW(g_hwnd, IDC_BTN_CONNECT, L"Wiederverbindung abbrechen (Ctrl+Shift+K)");
    } else {
        SetDlgItemTextW(g_hwnd, IDC_BTN_CONNECT, L"Verbinden (Ctrl+Shift+K)");
    }
    InvalidateRect(GetDlgItem(g_hwnd, IDC_BTN_CONNECT), nullptr, FALSE);

    std::wostringstream ss;
    if (st.streams.empty()) {
        ss << L"Keine aktiven Streams. (Dateien/Ordner per Drag & Drop ins Fenster ziehen!)\r\n\r\nLetzte Log-Meldungen:\r\n";
        auto logs = s2p::Logger::Instance().GetRecentLines(6);
        for (const auto& l : logs) ss << l << L"\r\n";
    } else {
        for (const auto& s : st.streams) {
            ss << L"Monitor " << (s.monitorIndex + 1) << L" [" << s.width << L"x" << s.height << L"]  "
               << s.codecName << L"  |  "
               << std::fixed << std::setprecision(1) << s.fps << L"/" << s.targetFps << L" FPS  |  "
               << std::setprecision(1) << s.mbps << L" Mbit/s";
            if (s.autoBitrate && s.liveBitrateMbps > 0) {
                ss << L" (Auto " << s.liveBitrateMbps << L" Mbit/s)";
            } else if (s.configuredBitrateMbps > 0 && s.liveBitrateMbps > 0 && s.liveBitrateMbps < s.configuredBitrateMbps) {
                ss << L" (ABR " << s.liveBitrateMbps << L"/" << s.configuredBitrateMbps << L")";
            }
            ss << L"  |  Enc: " << std::setprecision(1) << s.avgEncodeMs
               << L"ms Send: " << std::setprecision(1) << s.avgSendMs << L"ms";
            if (s.losslessSettleActive) {
                ss << L" [100% Settle]";
            }
            ss << L"  |  Frames: " << s.framesSent << L"\r\n";
        }
        ss << L"Dateien: Gesendet " << st.fileStats.filesSent
           << L" | Empfangen " << st.fileStats.filesReceived;
        if (st.usingExternalDisplays && g_lidGuard.IsEngaged()) {
            ss << L" | Zuklappen-Guard: AKTIV";
        }
        ss << L"\r\n";
    }
    SetDlgItemTextW(g_hwnd, IDC_STATS_TEXT, ss.str().c_str());

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
}

std::vector<std::wstring> PickFilesDialog(HWND owner) {
    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Alle Dateien (*.*)\0*.*\0\0";
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    ofn.lpstrTitle = L"Dateien an PC senden";
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
    dlg->SetTitle(L"Ordner (inkl. Unterordner) an PC senden");
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

void StartConnectAttempt(const std::wstring& targetIpW, bool manualAction) {
    if (!g_engine || !g_hwnd || g_closing.load()) return;
    if (g_connectBusy.exchange(true)) return;

    std::wstring ipW = TrimWide(targetIpW);
    if (ipW.empty()) {
        g_connectBusy = false;
        if (manualAction) {
            MessageBoxW(g_hwnd, L"Bitte waehle einen gefundenen PC aus oder gib die IP-Adresse des PCs ein.",
                        L"Screen2PC Sender", MB_ICONINFORMATION);
        }
        return;
    }

    if (manualAction) {
        SaveLastIp(ipW);
        g_userManualAction = true;
        g_userManualDisconnect = false;
        g_reconnectArmed = false;
        g_reconnectStep = 0;
    }

    s2p::SenderConfig cfg;
    cfg.targetIp = s2p::WideToUtf8(ipW);

    LRESULT modeIdx = SendDlgItemMessageW(g_hwnd, IDC_MODE_COMBO, CB_GETCURSEL, 0, 0);
    if (modeIdx == 1) cfg.mode = s2p::DisplayMode::ExtendPrimary;
    else if (modeIdx == 2) cfg.mode = s2p::DisplayMode::ExtendSecondary;
    else if (modeIdx == 3) cfg.mode = s2p::DisplayMode::MirrorPrimary;
    else cfg.mode = s2p::DisplayMode::ExtendAll;

    int codecIdx = 0, brIdx = s2p::kDefaultBitratePresetIdx, fpsIdx = 0;
    ReadStreamSettingsFromUi(g_hwnd, cfg.codec, cfg.bitrateMbps, cfg.targetFps, codecIdx, brIdx, fpsIdx);

    SaveSetting(L"ModeIdx", static_cast<int>(modeIdx));
    SaveSetting(L"CodecIdx", codecIdx);
    SaveSetting(L"BitrateMbps", static_cast<int>(cfg.bitrateMbps));
    SaveSetting(L"BitrateIdx", brIdx);
    SaveSetting(L"FpsSetting", static_cast<int>(cfg.targetFps));
    SaveSetting(L"FpsIdx", fpsIdx);

    g_versionMismatchPrompted = false;
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_CONNECT), FALSE);
    SetDlgItemTextW(g_hwnd, IDC_STATUS_LABEL, L"Status: Verbinde und richte Monitore ein...");

    if (g_connectThread.joinable()) {
        g_connectThread.join();
    }
    g_connectThread = std::thread([cfg]() {
        if (g_engine && !g_closing.load()) {
            g_engine->Connect(cfg);
        }
        g_connectBusy = false;
        if (!g_closing.load() && g_hwnd) {
            PostMessageW(g_hwnd, WM_CONNECT_DONE, 0, 0);
        }
    });
}

void SwitchDisplayModeWhileConnected(HWND hwnd) {
    if (!g_engine || !g_engine->IsConnected() || g_closing.load()) return;
    wchar_t ipBuf[128]{};
    GetDlgItemTextW(hwnd, IDC_IP_EDIT, ipBuf, _countof(ipBuf));
    std::wstring ipW = TrimWide(ipBuf);
    if (ipW.empty()) return;

    if (g_connectBusy.exchange(true)) return;

    g_userManualDisconnect = false;
    g_reconnectArmed = false;
    g_wasConnected = false;

    s2p::SenderConfig cfg;
    cfg.targetIp = s2p::WideToUtf8(ipW);

    LRESULT modeIdx = SendDlgItemMessageW(hwnd, IDC_MODE_COMBO, CB_GETCURSEL, 0, 0);
    if (modeIdx == 1) cfg.mode = s2p::DisplayMode::ExtendPrimary;
    else if (modeIdx == 2) cfg.mode = s2p::DisplayMode::ExtendSecondary;
    else if (modeIdx == 3) cfg.mode = s2p::DisplayMode::MirrorPrimary;
    else cfg.mode = s2p::DisplayMode::ExtendAll;

    int codecIdx = 0, brIdx = s2p::kDefaultBitratePresetIdx, fpsIdx = 0;
    ReadStreamSettingsFromUi(hwnd, cfg.codec, cfg.bitrateMbps, cfg.targetFps, codecIdx, brIdx, fpsIdx);

    EnableWindow(GetDlgItem(hwnd, IDC_BTN_CONNECT), FALSE);
    SetDlgItemTextW(hwnd, IDC_STATUS_LABEL, L"Wechsle Anzeigemodus...");

    if (g_connectThread.joinable()) {
        g_connectThread.join();
    }
    g_connectThread = std::thread([cfg]() {
        if (g_engine && !g_closing.load()) {
            g_engine->SignalStop();
            g_engine->Disconnect();
        }
        if (g_engine && !g_closing.load()) {
            g_engine->Connect(cfg);
        }
        g_connectBusy = false;
        if (!g_closing.load() && g_hwnd) {
            PostMessageW(g_hwnd, WM_CONNECT_DONE, 0, 0);
        }
    });
}

void StartAsyncDiscovery() {
    if (g_scanning || g_closing.load()) return;
    g_scanning = true;
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_SCAN), FALSE);
    SetDlgItemTextW(g_hwnd, IDC_BTN_SCAN, L"Suche...");

    if (g_scanThread.joinable()) {
        g_scanThread.join();
    }
    g_scanThread = std::thread([]() {
        auto* found = new std::vector<s2p::DiscoveredPeer>(s2p::SenderEngine::DiscoverPeers(800));
        if (!g_closing.load() && g_hwnd) {
            PostMessageW(g_hwnd, WM_DISCOVERY_DONE, 0, reinterpret_cast<LPARAM>(found));
        } else {
            delete found;
        }
    });
}

void OnToggleConnect() {
    if (!g_engine || !g_hwnd) return;
    if (g_reconnectArmed && !g_engine->IsConnected() && !g_connectBusy.load()) {
        g_reconnectArmed = false;
        g_userManualDisconnect = true;
        UpdateUiFromState();
        return;
    }
    if (g_connectBusy.exchange(true)) return;
    if (!IsWindowEnabled(GetDlgItem(g_hwnd, IDC_BTN_CONNECT))) {
        g_connectBusy = false;
        return;
    }
    if (g_engine->IsConnected()) {
        g_userManualAction = true;
        g_userManualDisconnect = true;
        g_reconnectArmed = false;
        g_dimOverlay.Hide(false);
        g_lidGuard.Restore(GetIniPath());

        EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_CONNECT), FALSE);
        SetDlgItemTextW(g_hwnd, IDC_STATUS_LABEL, L"Status: Trenne Verbindung...");
        if (g_connectThread.joinable()) {
            g_connectThread.join();
        }
        g_engine->SignalStop();
        g_connectThread = std::thread([]() {
            if (g_engine && !g_closing.load()) {
                g_engine->Disconnect();
            }
            g_connectBusy = false;
            if (!g_closing.load() && g_hwnd) {
                PostMessageW(g_hwnd, WM_CONNECT_DONE, 0, 0);
            }
        });
        return;
    }

    g_connectBusy = false;
    wchar_t ipBuf[128]{};
    GetDlgItemTextW(g_hwnd, IDC_IP_EDIT, ipBuf, _countof(ipBuf));
    StartConnectAttempt(ipBuf, true);
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

    // Hero Card (x=16, y=12, w=508, h=116)
    mv(IDC_HERO_TITLE,       46,  22, 464, 22, g_titleFont);
    mv(IDC_STATUS_LABEL,     46,  46, 464, 18);
    mv(IDC_DRIVER_LABEL,     46,  64, 464, 18);
    mv(IDC_BTN_CONNECT,      28,  86, 484, 34, g_boldFont);

    // Segmented Tab Bar (x=16, y=136, w=508, h=32)
    mv(IDC_TAB_CONNECT,      16, 136, 166, 32, g_boldFont);
    mv(IDC_TAB_TRANSFER,    187, 136, 166, 32, g_boldFont);
    mv(IDC_TAB_SETTINGS,    358, 136, 166, 32, g_boldFont);

    // Tab 0: Verbindung
    // Card 1: Ziel-PC (16, 178, 508, 102)
    mv(IDC_LBL_PEERS,        30, 190, 124, 20, g_boldFont);
    mv(IDC_PEER_COMBO,      160, 188, 250, 200);
    mv(IDC_BTN_SCAN,        418, 187,  92, 26);
    mv(IDC_LBL_IP,           30, 228, 124, 20, g_boldFont);
    mv(IDC_IP_EDIT,         160, 225, 350, 24);

    // Card 2: Display & Stream (16, 290, 508, 98)
    mv(IDC_LBL_MODE,         30, 304, 124, 20, g_boldFont);
    mv(IDC_MODE_COMBO,      160, 301, 350, 170);
    mv(IDC_LBL_STREAM,       30, 342, 124, 20, g_boldFont);
    mv(IDC_CODEC_COMBO,     160, 339, 152, 140);
    mv(IDC_BITRATE_COMBO,   318, 339,  98, 260);
    mv(IDC_FPS_COMBO,       422, 339,  88, 160);

    // Card 3: Live-Telemetrie (16, 398, 508, 246)
    mv(IDC_LBL_TELEMETRY,    30, 410, 480, 20, g_boldFont);
    mv(IDC_STATS_TEXT,       28, 434, 484, 198, g_monoFont);

    // Tab 1: Dateitransfer
    // Drop Zone Card (16, 178, 508, 146)
    mv(IDC_DROP_TITLE,       32, 228, 476, 24, g_titleFont);
    mv(IDC_DROP_SUB,         32, 256, 476, 20);
    // Transfer Action Card (16, 336, 508, 134)
    mv(IDC_BTN_SENDFILE,     30, 352, 154, 32);
    mv(IDC_BTN_SENDFOLDER,  193, 352, 154, 32);
    mv(IDC_BTN_OPEN_DL,     356, 352, 154, 32);
    mv(IDC_FILE_PROGRESS,    30, 398, 376, 22);
    mv(IDC_BTN_CANCELFILE,  414, 396,  96, 26);
    mv(IDC_FILE_STATUS,      30, 430, 480, 20);

    // Tab 2: Einstellungen
    // Card: Verhalten & Energie (16, 178, 508, 286)
    mv(IDC_CHK_CLIPBOARD,    30, 188, 480, 26);
    mv(IDC_CHK_KVM,          30, 218, 480, 26);
    mv(IDC_CHK_AUTOSTART,    30, 248, 480, 26);
    mv(IDC_CHK_AUTOCONNECT,  30, 278, 480, 26);
    mv(IDC_CHK_AUTORECONNECT,30, 308, 480, 26);
    mv(IDC_CHK_LID_BYPASS,   30, 338, 480, 26);
    mv(IDC_CHK_DIM_SURFACE,  30, 368, 480, 26);
    mv(IDC_CHK_AUTOUPDATE,   30, 398, 480, 26);
    mv(IDC_CHK_ALLOW_REMOTE, 30, 428, 480, 26);

    // Card: Updates, Vertrauen & Sprache (16, 476, 508, 148)
    mv(IDC_LBL_LANG,         30, 490, 150, 22, g_boldFont);
    mv(IDC_LANG_COMBO,      186, 487, 324, 140);
    mv(IDC_BTN_TRUSTED_PCS,  30, 530, 480, 32);
    mv(IDC_BTN_UPDATE,       30, 572, 480, 32);
}

void ApplySenderLanguage(HWND hwnd) {
    using s2p::lang::Tr;
    std::wstring title = L"Screen2PC Sender v" + s2p::FormatAppVersion(s2p::CurrentAppVersion()) + L" (Laptop)";
    SetWindowTextW(hwnd, title.c_str());

    SetDlgItemTextW(hwnd, IDC_TAB_CONNECT,  Tr(L"Verbindung", L"Connection"));
    SetDlgItemTextW(hwnd, IDC_TAB_TRANSFER, Tr(L"Dateitransfer", L"File Transfer"));
    SetDlgItemTextW(hwnd, IDC_TAB_SETTINGS, Tr(L"Einstellungen", L"Settings"));

    SetDlgItemTextW(hwnd, IDC_LBL_PEERS,     Tr(L"PC im LAN:", L"PC on LAN:"));
    SetDlgItemTextW(hwnd, IDC_BTN_SCAN,      g_scanning ? Tr(L"Suche...", L"Scanning...") : Tr(L"Suchen", L"Scan"));
    SetDlgItemTextW(hwnd, IDC_LBL_IP,        Tr(L"Ziel-IP (manuell):", L"Target IP (manual):"));
    SetDlgItemTextW(hwnd, IDC_LBL_MODE,      Tr(L"Anzeigemodus:", L"Display Mode:"));
    SetDlgItemTextW(hwnd, IDC_LBL_STREAM,    Tr(L"Stream (Live):", L"Stream (Live):"));
    SetDlgItemTextW(hwnd, IDC_LBL_TELEMETRY, Tr(L"Live-Telemetrie & Protokoll", L"Live Telemetry & Log"));

    SetDlgItemTextW(hwnd, IDC_DROP_TITLE,    Tr(L"Dateien oder Ordner hierher ziehen", L"Drop files or folders here"));
    SetDlgItemTextW(hwnd, IDC_DROP_SUB,      Tr(L"Werden in Downloads\\Screen2PC auf dem Ziel-PC gespeichert", L"Saved to Downloads\\Screen2PC on the target PC"));
    SetDlgItemTextW(hwnd, IDC_BTN_SENDFILE,  Tr(L"Datei(en) senden...", L"Send file(s)..."));
    SetDlgItemTextW(hwnd, IDC_BTN_SENDFOLDER,Tr(L"Ordner senden...", L"Send folder..."));
    SetDlgItemTextW(hwnd, IDC_BTN_OPEN_DL,   Tr(L"Downloads oeffnen", L"Open Downloads"));
    SetDlgItemTextW(hwnd, IDC_BTN_CANCELFILE,Tr(L"Abbrechen", L"Cancel"));

    SetDlgItemTextW(hwnd, IDC_CHK_CLIPBOARD,     Tr(L"Geteilte Zwischenablage (Text & Bilder)", L"Shared Clipboard (Text & Images)"));
    SetDlgItemTextW(hwnd, IDC_CHK_KVM,           Tr(L"KVM-Steuerung (Maus & Tastatur vom PC)", L"KVM Control (PC Mouse & Keyboard)"));
    SetDlgItemTextW(hwnd, IDC_CHK_AUTOSTART,     Tr(L"Mit Windows starten (System-Tray)", L"Start with Windows (System Tray)"));
    SetDlgItemTextW(hwnd, IDC_CHK_AUTOCONNECT,   Tr(L"Beim Start automatisch verbinden", L"Auto-connect on startup"));
    SetDlgItemTextW(hwnd, IDC_CHK_AUTORECONNECT, Tr(L"Bei Abbruch automatisch wiederverbinden", L"Auto-reconnect on disconnect"));
    SetDlgItemTextW(hwnd, IDC_CHK_LID_BYPASS,    Tr(L"Standby beim Zuklappen pausieren", L"Pause sleep on lid close"));
    SetDlgItemTextW(hwnd, IDC_CHK_DIM_SURFACE,   Tr(L"Laptop-Bildschirm abdunkeln (ESC/Doppelklick)", L"Dim laptop screen (ESC/Double-click)"));
    SetDlgItemTextW(hwnd, IDC_CHK_AUTOUPDATE,    Tr(L"Beim Start auf GitHub-Updates pruefen", L"Check GitHub updates on startup"));
    SetDlgItemTextW(hwnd, IDC_CHK_ALLOW_REMOTE,  Tr(L"Verbindungen von vertrauten PCs erlauben", L"Allow connections from trusted PCs"));
    SetDlgItemTextW(hwnd, IDC_LBL_LANG,          Tr(L"Sprache / Language:", L"Language / Sprache:"));
    SetDlgItemTextW(hwnd, IDC_BTN_TRUSTED_PCS,   Tr(L"Vertraute PCs verwalten...", L"Manage Trusted PCs..."));
    SetDlgItemTextW(hwnd, IDC_BTN_UPDATE,        DefaultUpdateButtonText().c_str());

    if (g_tooltip) {
        s2p::ui::SetControlTooltip(g_tooltip, hwnd, GetDlgItem(hwnd, IDC_CHK_AUTOSTART),
            Tr(L"Startet Screen2PC Sender automatisch beim Anmelden minimiert im System-Tray (Aufgabenplanung mit Admin-Rechten, auch im Akkubetrieb).",
               L"Starts Screen2PC Sender automatically at logon minimized in the system tray (Task Scheduler with admin rights, even on battery)."));
        s2p::ui::SetControlTooltip(g_tooltip, hwnd, GetDlgItem(hwnd, IDC_CHK_AUTOCONNECT),
            Tr(L"Verbindet beim Start automatisch mit dem zuletzt verwendeten oder gefundenen PC.",
               L"Automatically connects to the last used or discovered PC on startup."));
        s2p::ui::SetControlTooltip(g_tooltip, hwnd, GetDlgItem(hwnd, IDC_CHK_LID_BYPASS),
            Tr(L"Verhindert Laptop-Standby beim Zuklappen, solange externe PC-Displays aktiv sind.",
               L"Prevents laptop sleep when closing the lid while external PC displays are active."));
        s2p::ui::SetControlTooltip(g_tooltip, hwnd, GetDlgItem(hwnd, IDC_CHK_DIM_SURFACE),
            Tr(L"Schaltet die physischen Laptop-Displays schwarz, solange am PC gearbeitet wird.",
               L"Dims physical laptop displays black while working on the PC."));
        s2p::ui::SetControlTooltip(g_tooltip, hwnd, GetDlgItem(hwnd, IDC_CHK_ALLOW_REMOTE),
            Tr(L"Erlaubt bereits gekoppelten PCs, eine Verbindung zum Laptop per Klick am PC anzufordern (UDP 48218).",
               L"Allows previously paired PCs to initiate a connection to this laptop directly from the PC (UDP 48218)."));
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

void CreateUiControls(HWND hwnd) {
    const UINT dpi = GetWindowDpiSafe(hwnd);
    auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND {
        return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                               0, 0, 10, 10, hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    };

    s2p::lang::SetLangMode(LoadSetting(L"LangMode", 0));

    // Hero
    addCtrl(L"STATIC", L"Bereit zum Verbinden", SS_ENDELLIPSIS, IDC_HERO_TITLE);
    addCtrl(L"STATIC", L"Bereit", SS_ENDELLIPSIS, IDC_STATUS_LABEL);
    addCtrl(L"STATIC", L"", SS_ENDELLIPSIS, IDC_DRIVER_LABEL);
    addCtrl(L"BUTTON", L"Verbinden (Ctrl+Shift+K)", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_CONNECT);

    // Tabs
    addCtrl(L"BUTTON", L"Verbindung", BS_OWNERDRAW | WS_TABSTOP, IDC_TAB_CONNECT);
    addCtrl(L"BUTTON", L"Dateitransfer", BS_OWNERDRAW | WS_TABSTOP, IDC_TAB_TRANSFER);
    addCtrl(L"BUTTON", L"Einstellungen", BS_OWNERDRAW | WS_TABSTOP, IDC_TAB_SETTINGS);

    // Tab 0: Verbindung
    addCtrl(L"STATIC", L"PC im LAN:", SS_CENTERIMAGE, IDC_LBL_PEERS);
    addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_PEER_COMBO);
    addCtrl(L"BUTTON", L"Suchen", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_SCAN);

    addCtrl(L"STATIC", L"Ziel-IP (manuell):", SS_CENTERIMAGE, IDC_LBL_IP);
    addCtrl(L"EDIT", LoadLastIp().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_IP_EDIT);

    addCtrl(L"STATIC", L"Anzeigemodus:", SS_CENTERIMAGE, IDC_LBL_MODE);
    HWND modeCb = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_MODE_COMBO);
    SendMessageW(modeCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Erweitern (Alle PC-Monitore, bis zu 2)"));
    SendMessageW(modeCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Erweitern (Nur PC-Monitor 1)"));
    SendMessageW(modeCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Erweitern (Nur PC-Monitor 2)"));
    SendMessageW(modeCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Laptop-Display spiegeln (ohne Treiber)"));
    int savedMode = std::clamp(LoadSetting(L"ModeIdx", 0), 0, 3);
    SendMessageW(modeCb, CB_SETCURSEL, savedMode, 0);

    addCtrl(L"STATIC", L"Stream (Live):", SS_CENTERIMAGE, IDC_LBL_STREAM);
    HWND codecCb = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_CODEC_COMBO);
    SendMessageW(codecCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"H.264 Low-Latency"));
    SendMessageW(codecCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"HEVC (H.265) Low-Latency"));
    SendMessageW(codecCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Delta Lossless (4:4:4)"));
    int savedCodec = std::clamp(LoadSetting(L"CodecIdx", 0), 0, 2);
    SendMessageW(codecCb, CB_SETCURSEL, savedCodec, 0);

    HWND brCb = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_BITRATE_COMBO);
    for (uint32_t mbps : s2p::kBitratePresetsMbps) {
        std::wstring label = s2p::FormatBitrateLabel(mbps);
        SendMessageW(brCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    int savedBr = s2p::ResolveSavedBitratePresetIndex(
        LoadSetting(L"BitrateMbps", -1),
        LoadSetting(L"BitrateIdx", -1));
    SendMessageW(brCb, CB_SETCURSEL, savedBr, 0);

    HWND fpsCb = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_FPS_COMBO);
    for (uint32_t f : s2p::kFpsPresets) {
        std::wstring label = s2p::FormatFpsLabel(f);
        SendMessageW(fpsCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    int savedFpsSetting = LoadSetting(L"FpsSetting", -1);
    int savedFpsIdx = (savedFpsSetting >= 0)
                          ? s2p::FindFpsPresetIndex(static_cast<uint32_t>(savedFpsSetting))
                          : std::clamp(LoadSetting(L"FpsIdx", 0), 0, static_cast<int>(s2p::kFpsPresetCount) - 1);
    SendMessageW(fpsCb, CB_SETCURSEL, savedFpsIdx, 0);

    addCtrl(L"STATIC", L"Live-Telemetrie & Protokoll", SS_NOPREFIX, IDC_LBL_TELEMETRY);
    addCtrl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, IDC_STATS_TEXT);

    // Tab 1: Dateitransfer
    addCtrl(L"STATIC", L"Dateien oder Ordner hierher ziehen", SS_CENTER, IDC_DROP_TITLE);
    addCtrl(L"STATIC", L"Werden in Downloads\\Screen2PC auf dem Ziel-PC gespeichert", SS_CENTER, IDC_DROP_SUB);
    addCtrl(L"BUTTON", L"Datei(en) senden...", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_SENDFILE);
    addCtrl(L"BUTTON", L"Ordner senden...", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_SENDFOLDER);
    addCtrl(L"BUTTON", L"Downloads oeffnen", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_OPEN_DL);
    HWND prog = addCtrl(PROGRESS_CLASSW, L"", PBS_SMOOTH, IDC_FILE_PROGRESS);
    SendMessageW(prog, PBM_SETRANGE32, 0, 1000);
    addCtrl(L"BUTTON", L"Abbrechen", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_CANCELFILE);
    addCtrl(L"STATIC", L"Dateitransfer: Bereit", SS_ENDELLIPSIS, IDC_FILE_STATUS);

    // Tab 2: Einstellungen (Modern Toggle Switches)
    HWND chkClip = addCtrl(L"BUTTON", L"Geteilte Zwischenablage (Text & Bilder)", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_CLIPBOARD);
    bool clipEnabled = LoadSetting(L"ClipboardSync", 1) != 0;
    SetToggleChecked(hwnd, IDC_CHK_CLIPBOARD, clipEnabled);
    if (g_engine) g_engine->SetClipboardEnabled(clipEnabled);

    HWND chkKvm = addCtrl(L"BUTTON", L"KVM-Steuerung (Maus & Tastatur vom PC)", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_KVM);
    bool kvmEnabled = LoadSetting(L"KvmEnabled", 1) != 0;
    SetToggleChecked(hwnd, IDC_CHK_KVM, kvmEnabled);
    if (g_engine) g_engine->SetKvmEnabled(kvmEnabled);

    HWND chkAuto = addCtrl(L"BUTTON", L"Mit Windows starten (System-Tray)", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_AUTOSTART);
    SetToggleChecked(hwnd, IDC_CHK_AUTOSTART, s2p::IsAutostartEnabled(s2p::kAutostartSenderValueName));

    HWND chkAutoConn = addCtrl(L"BUTTON", L"Beim Start automatisch verbinden", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_AUTOCONNECT);
    SetToggleChecked(hwnd, IDC_CHK_AUTOCONNECT, LoadSetting(L"AutoConnect", 0) != 0);

    HWND chkAutoReconn = addCtrl(L"BUTTON", L"Bei Abbruch automatisch wiederverbinden", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_AUTORECONNECT);
    SetToggleChecked(hwnd, IDC_CHK_AUTORECONNECT, LoadSetting(L"AutoReconnect", 1) != 0);

    HWND chkLid = addCtrl(L"BUTTON", L"Standby beim Zuklappen pausieren", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_LID_BYPASS);
    SetToggleChecked(hwnd, IDC_CHK_LID_BYPASS, LoadSetting(L"LidCloseBypass", 1) != 0);

    HWND chkDim = addCtrl(L"BUTTON", L"Laptop-Bildschirm abdunkeln (ESC/Doppelklick)", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_DIM_SURFACE);
    SetToggleChecked(hwnd, IDC_CHK_DIM_SURFACE, LoadSetting(L"DimSurfaceOnExternal", 0) != 0);

    HWND chkUpd = addCtrl(L"BUTTON", L"Beim Start auf GitHub-Updates pruefen", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_AUTOUPDATE);
    SetToggleChecked(hwnd, IDC_CHK_AUTOUPDATE, LoadSetting(L"AutoUpdateCheck", 1) != 0);

    HWND chkRem = addCtrl(L"BUTTON", L"Verbindungen von vertrauten PCs erlauben", BS_OWNERDRAW | WS_TABSTOP, IDC_CHK_ALLOW_REMOTE);
    bool allowRem = LoadSetting(L"AllowRemoteConnect", 1) != 0;
    SetToggleChecked(hwnd, IDC_CHK_ALLOW_REMOTE, allowRem);
    if (g_engine) g_engine->SetAllowRemoteConnect(allowRem);

    addCtrl(L"STATIC", L"Sprache / Language:", SS_CENTERIMAGE, IDC_LBL_LANG);
    HWND langCb = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_LANG_COMBO);
    SendMessageW(langCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Auto (Windows)"));
    SendMessageW(langCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Deutsch"));
    SendMessageW(langCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(langCb, CB_SETCURSEL, s2p::lang::GetLangMode(), 0);

    addCtrl(L"BUTTON", L"Vertraute PCs verwalten...", BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_TRUSTED_PCS);
    addCtrl(L"BUTTON", DefaultUpdateButtonText().c_str(), BS_OWNERDRAW | WS_TABSTOP, IDC_BTN_UPDATE);

    g_tooltip = s2p::ui::CreateTooltipWindow(hwnd);

    LayoutMainControls(hwnd, dpi);
    ApplySenderTheme(hwnd, s2p::ui::IsSystemDarkMode());
    ApplySenderLanguage(hwnd);
    ApplySenderTabVisibility(hwnd);
}

void ShowMainWindow(HWND hwnd) {
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Fenster oeffnen");
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE,
                (g_engine && g_engine->IsConnected()) ? L"Trennen" : L"Verbinden");
    AppendMenuW(menu, MF_STRING, IDM_CHECK_UPDATE, L"Nach Updates suchen");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Beenden");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

// ============================================================================
// Modal deadlock-free PIN prompt window on Surface
// ============================================================================

void FinishUiPinPrompt(bool submit) {
    auto req = g_uiPinReq;
    if (!req) return;

    if (submit && g_pinPromptWnd && IsWindow(g_pinPromptWnd)) {
        wchar_t pinBuf[16]{};
        GetDlgItemTextW(g_pinPromptWnd, IDC_PIN_EDIT, pinBuf, _countof(pinBuf));
        std::string digits;
        for (int i = 0; pinBuf[i] != L'\0'; ++i) {
            if (pinBuf[i] >= L'0' && pinBuf[i] <= L'9') {
                digits.push_back(static_cast<char>(pinBuf[i]));
            }
        }
        if (digits.size() != 6) {
            SetDlgItemTextW(g_pinPromptWnd, IDC_PIN_STATUS, L"Bitte genau 6 Ziffern eingeben!");
            MessageBeep(MB_ICONWARNING);
            SetFocus(GetDlgItem(g_pinPromptWnd, IDC_PIN_EDIT));
            return;
        }
        {
            std::lock_guard<std::mutex> lk(req->mtx);
            req->outPin = digits;
            req->submitted = true;
            req->done = true;
        }
        req->cv.notify_all();
    } else {
        {
            std::lock_guard<std::mutex> lk(req->mtx);
            if (!req->done) {
                req->canceled = true;
                req->done = true;
            }
        }
        req->cv.notify_all();
    }

    g_uiPinReq.reset();
    if (g_pinPromptWnd && IsWindow(g_pinPromptWnd)) {
        DestroyWindow(g_pinPromptWnd);
    }
}

void LayoutPinPromptControls(HWND hwnd, UINT dpi) {
    g_pinDpi = dpi;
    if (g_pinFont) DeleteObject(g_pinFont);
    if (g_pinBigFont) DeleteObject(g_pinBigFont);
    g_pinFont = CreateFontW(-DpiScale(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_pinBigFont = CreateFontW(-DpiScale(28, dpi), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

    auto mv = [&](int id, int x, int y, int w, int h, HFONT font = nullptr) {
        HWND c = GetDlgItem(hwnd, id);
        if (!c) return;
        MoveWindow(c, DpiScale(x, dpi), DpiScale(y, dpi), DpiScale(w, dpi), DpiScale(h, dpi), TRUE);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_pinFont), TRUE);
    };

    mv(IDC_PIN_INFO,   16,  14, 398, 56);
    mv(IDC_PIN_STATUS, 16,  76, 398, 36);
    mv(IDC_PIN_EDIT,  115, 120, 200, 44, g_pinBigFont);
    mv(IDOK,           98, 184, 112, 34);
    mv(IDCANCEL,      220, 184, 112, 34);
}

LRESULT CALLBACK PinPromptWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 10, 10, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
        };
        addCtrl(L"STATIC", L"", 0, IDC_PIN_INFO);
        addCtrl(L"STATIC", L"", 0, IDC_PIN_STATUS);
        HWND ed = addCtrl(L"EDIT", L"", WS_BORDER | ES_CENTER | ES_NUMBER | WS_TABSTOP, IDC_PIN_EDIT);
        SendMessageW(ed, EM_SETLIMITTEXT, 6, 0);
        addCtrl(L"BUTTON", L"Koppeln", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK);
        addCtrl(L"BUTTON", L"Abbrechen", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL);

        LayoutPinPromptControls(hwnd, GetWindowDpiSafe(hwnd));
        s2p::ui::ApplyWindowTheme(hwnd, g_theme.dark);
        s2p::ui::ApplyChildTheme(ed, g_theme.dark);
        if (g_uiPinReq) {
            std::wostringstream info;
            info << L"PC: " << g_uiPinReq->pcName << L" (" << s2p::Utf8ToWide(g_uiPinReq->pcIp) << L")\r\n";
            if (g_uiPinReq->pcVersion.major > 0 || g_uiPinReq->pcVersion.minor > 0) {
                info << L"Receiver-Version: v" << s2p::FormatVersionTuple(g_uiPinReq->pcVersion) << L"\r\n";
            }
            info << L"Gib die 6-stellige PIN vom PC-Bildschirm ein:";
            SetDlgItemTextW(hwnd, IDC_PIN_INFO, info.str().c_str());

            std::wostringstream st;
            st << g_uiPinReq->message << L" (Verbleibende Versuche: "
               << static_cast<int>(g_uiPinReq->remainingAttempts) << L")";
            SetDlgItemTextW(hwnd, IDC_PIN_STATUS, st.str().c_str());
        }
        SetFocus(ed);
        return 0;
    }

    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutPinPromptControls(hwnd, HIWORD(wp));
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

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            FinishUiPinPrompt(true);
            return 0;
        }
        if (LOWORD(wp) == IDCANCEL) {
            FinishUiPinPrompt(false);
            return 0;
        }
        break;

    case WM_CLOSE:
        FinishUiPinPrompt(false);
        return 0;

    case WM_DESTROY:
        if (g_uiPinReq) {
            auto req = g_uiPinReq;
            g_uiPinReq.reset();
            {
                std::lock_guard<std::mutex> lk(req->mtx);
                if (!req->done) {
                    req->canceled = true;
                    req->done = true;
                }
            }
            req->cv.notify_all();
        }
        if (g_hwnd && IsWindow(g_hwnd)) {
            EnableWindow(g_hwnd, TRUE);
        }
        if (g_pinFont) { DeleteObject(g_pinFont); g_pinFont = nullptr; }
        if (g_pinBigFont) { DeleteObject(g_pinBigFont); g_pinBigFont = nullptr; }
        g_pinPromptWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenPinPromptWindow(HWND owner, const std::shared_ptr<SenderPinRequest>& req) {
    if (!req || g_closing.load()) return;

    if (g_pinPromptWnd && IsWindow(g_pinPromptWnd)) {
        DestroyWindow(g_pinPromptWnd);
        g_pinPromptWnd = nullptr;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PinPromptWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = s2p::LoadAppLargeIcon();
        wc.hIconSm = s2p::LoadAppSmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Screen2PCPinPromptWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    ShowMainWindow(owner);
    g_uiPinReq = req;

    const UINT dpi = GetWindowDpiSafe(owner);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT wr{ 0, 0, DpiScale(kPinBaseW, dpi), DpiScale(kPinBaseH, dpi) };
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, dpi);

    RECT ownerRc{};
    GetWindowRect(owner, &ownerRc);
    int w = wr.right - wr.left;
    int h = wr.bottom - wr.top;
    int x = ownerRc.left + ((ownerRc.right - ownerRc.left) - w) / 2;
    int y = ownerRc.top + ((ownerRc.bottom - ownerRc.top) - h) / 2;

    EnableWindow(owner, FALSE);
    g_pinPromptWnd = CreateWindowExW(
        0, L"Screen2PCPinPromptWnd", L"Screen2PC Kopplungs-PIN",
        style, x, y, w, h,
        owner, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (g_pinPromptWnd) {
        ShowWindow(g_pinPromptWnd, SW_SHOW);
        UpdateWindow(g_pinPromptWnd);
        SetForegroundWindow(g_pinPromptWnd);
        SetFocus(GetDlgItem(g_pinPromptWnd, IDC_PIN_EDIT));
    } else {
        EnableWindow(owner, TRUE);
        FinishUiPinPrompt(false);
    }
}

void CancelActivePinPrompt() {
    if (g_pinPromptWnd && IsWindow(g_pinPromptWnd)) {
        DestroyWindow(g_pinPromptWnd);
        g_pinPromptWnd = nullptr;
    }
    std::shared_ptr<SenderPinRequest> req;
    {
        std::lock_guard<std::mutex> lk(g_pinReqMutex);
        req = g_activePinReq;
    }
    if (req) {
        {
            std::lock_guard<std::mutex> lk(req->mtx);
            if (!req->done) {
                req->canceled = true;
                req->done = true;
            }
        }
        req->cv.notify_all();
    }
}

void PopulateTrustedPcsList() {
    if (!g_trustedPcsWnd || !IsWindow(g_trustedPcsWnd) || !g_engine) return;
    HWND lv = GetDlgItem(g_trustedPcsWnd, IDC_TPC_LIST);
    if (!lv) return;

    ListView_DeleteAllItems(lv);
    g_trustedPcsCache = g_engine->GetTrustedPcs();

    for (size_t i = 0; i < g_trustedPcsCache.size(); ++i) {
        const auto& e = g_trustedPcsCache[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(e.name.c_str());
        ListView_InsertItem(lv, &item);

        std::wstring macOrId = !e.macStr.empty() ? e.macStr : (L"ID:" + e.deviceIdHex.substr(0, 12));
        ListView_SetItemText(lv, static_cast<int>(i), 1, const_cast<LPWSTR>(macOrId.c_str()));
        ListView_SetItemText(lv, static_cast<int>(i), 2, const_cast<LPWSTR>(e.lastIp.c_str()));
        ListView_SetItemText(lv, static_cast<int>(i), 3, const_cast<LPWSTR>(e.addedUtc.c_str()));
    }

    using s2p::lang::Tr;
    std::wstring info = g_trustedPcsCache.empty()
        ? Tr(L"Noch keine vertrauten PCs gespeichert. Nach der ersten erfolgreichen Verbindung erscheint der PC automatisch hier.",
             L"No trusted PCs saved yet. After the first successful connection, the PC will appear here automatically.")
        : (Tr(L"Gespeicherte PCs (", L"Saved PCs (") + std::to_wstring(g_trustedPcsCache.size()) +
           Tr(L"): Duermfen Verbindungen zum Laptop per Klick am PC starten.",
              L"): Allowed to initiate connections to this laptop remotely."));
    // Fix German typo cleanly
    if (!g_trustedPcsCache.empty()) {
        info = Tr(L"Gespeicherte PCs (", L"Saved PCs (") + std::to_wstring(g_trustedPcsCache.size()) +
               Tr(L"): Duerfen Verbindungen zum Laptop per Klick am PC starten.",
                  L"): Allowed to initiate connections to this laptop remotely.");
    }
    SetDlgItemTextW(g_trustedPcsWnd, IDC_TPC_INFO, info.c_str());
}

void LayoutTrustedPcsControls(HWND hwnd, UINT dpi) {
    if (g_trustedPcsFont) DeleteObject(g_trustedPcsFont);
    g_trustedPcsFont = CreateFontW(-DpiScale(14, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    auto mv = [&](int id, int x, int y, int w, int h) {
        HWND c = GetDlgItem(hwnd, id);
        if (!c) return;
        MoveWindow(c, DpiScale(x, dpi), DpiScale(y, dpi), DpiScale(w, dpi), DpiScale(h, dpi), TRUE);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_trustedPcsFont), TRUE);
    };
    mv(IDC_TPC_INFO,        14,  12, 562,  36);
    mv(IDC_TPC_LIST,        14,  54, 562, 266);
    mv(IDC_TPC_BTN_REMOVE,  14, 332, 162,  34);
    mv(IDC_TPC_BTN_CLEAR,  186, 332, 152,  34);
    mv(IDC_TPC_BTN_REFRESH,348, 332, 110,  34);
    mv(IDC_TPC_BTN_CLOSE,  468, 332, 108,  34);

    HWND lv = GetDlgItem(hwnd, IDC_TPC_LIST);
    if (lv) {
        ListView_SetColumnWidth(lv, 0, DpiScale(150, dpi));
        ListView_SetColumnWidth(lv, 1, DpiScale(150, dpi));
        ListView_SetColumnWidth(lv, 2, DpiScale(116, dpi));
        ListView_SetColumnWidth(lv, 3, DpiScale(138, dpi));
    }
}

LRESULT CALLBACK TrustedPcsWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    using s2p::lang::Tr;
    switch (msg) {
    case WM_CREATE: {
        auto addCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id) -> HWND {
            return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                                   0, 0, 10, 10, hwnd,
                                   reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
        };
        addCtrl(L"STATIC", L"", 0, IDC_TPC_INFO);
        HWND lv = addCtrl(WC_LISTVIEWW, L"", WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_TPC_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

        auto addCol = [&](int idx, const wchar_t* title, int width) {
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            col.pszText = const_cast<LPWSTR>(title);
            col.cx = width;
            col.iSubItem = idx;
            ListView_InsertColumn(lv, idx, &col);
        };
        addCol(0, Tr(L"PC-Name", L"PC Name"), 150);
        addCol(1, L"MAC / ID", 150);
        addCol(2, Tr(L"Letzte IP", L"Last IP"), 116);
        addCol(3, Tr(L"Gespeichert (UTC)", L"Saved (UTC)"), 138);

        addCtrl(L"BUTTON", Tr(L"Auswahl entfernen", L"Remove Selected"), WS_TABSTOP, IDC_TPC_BTN_REMOVE);
        addCtrl(L"BUTTON", Tr(L"Alle entfernen", L"Remove All"), WS_TABSTOP, IDC_TPC_BTN_CLEAR);
        addCtrl(L"BUTTON", Tr(L"Aktualisieren", L"Refresh"), WS_TABSTOP, IDC_TPC_BTN_REFRESH);
        addCtrl(L"BUTTON", Tr(L"Schliessen", L"Close"), WS_TABSTOP, IDC_TPC_BTN_CLOSE);

        LayoutTrustedPcsControls(hwnd, GetWindowDpiSafe(hwnd));
        s2p::ui::ApplyWindowTheme(hwnd, g_theme.dark);
        s2p::ui::ApplyChildTheme(lv, g_theme.dark);
        return 0;
    }

    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutTrustedPcsControls(hwnd, HIWORD(wp));
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
    case WM_CTLCOLORDLG:
        return s2p::ui::HandleCtlColor(msg, wp, g_theme, false);

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TPC_BTN_REMOVE: {
            HWND lv = GetDlgItem(hwnd, IDC_TPC_LIST);
            int sel = lv ? ListView_GetNextItem(lv, -1, LVNI_SELECTED) : -1;
            if (sel >= 0 && static_cast<size_t>(sel) < g_trustedPcsCache.size() && g_engine) {
                g_engine->RemoveTrustedPcByKey(g_trustedPcsCache[sel].key);
                PopulateTrustedPcsList();
            }
            return 0;
        }
        case IDC_TPC_BTN_CLEAR:
            if (g_engine && !g_trustedPcsCache.empty()) {
                if (MessageBoxW(hwnd,
                                Tr(L"Alle vertrauten PCs wirklich entfernen?", L"Remove all trusted PCs?"),
                                Tr(L"Vertraute PCs", L"Trusted PCs"),
                                MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    g_engine->ClearAllTrustedPcs();
                    PopulateTrustedPcsList();
                }
            }
            return 0;
        case IDC_TPC_BTN_REFRESH:
            PopulateTrustedPcsList();
            return 0;
        case IDC_TPC_BTN_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_trustedPcsFont) {
            DeleteObject(g_trustedPcsFont);
            g_trustedPcsFont = nullptr;
        }
        g_trustedPcsWnd = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void OpenTrustedPcsWindow(HWND parent) {
    if (g_trustedPcsWnd && IsWindow(g_trustedPcsWnd)) {
        PopulateTrustedPcsList();
        ShowWindow(g_trustedPcsWnd, SW_SHOW);
        SetForegroundWindow(g_trustedPcsWnd);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = TrustedPcsWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hIcon = s2p::LoadAppLargeIcon();
        wc.hIconSm = s2p::LoadAppSmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = L"Screen2PCTrustedPcsWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    const UINT dpi = GetWindowDpiSafe(parent);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    RECT wr{ 0, 0, DpiScale(kTrustedPcBaseW, dpi), DpiScale(kTrustedPcBaseH, dpi) };
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, dpi);

    g_trustedPcsWnd = CreateWindowExW(
        0, L"Screen2PCTrustedPcsWnd",
        s2p::lang::Tr(L"Screen2PC \u2014 Vertraute PCs (Laptop)", L"Screen2PC \u2014 Trusted PCs (Laptop)"),
        style, CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (g_trustedPcsWnd) {
        PopulateTrustedPcsList();
        ShowWindow(g_trustedPcsWnd, SW_SHOW);
        UpdateWindow(g_trustedPcsWnd);
    }
}

// ============================================================================
// Main window procedure
// ============================================================================

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_wmShowExisting != 0 && msg == g_wmShowExisting) {
        ShowMainWindow(hwnd);
        return 0;
    }
    if (g_wmTaskbarCreated != 0 && msg == g_wmTaskbarCreated) {
        g_trayIconAdded = false;
        if (!AddTrayIcon(hwnd)) {
            g_trayRetryCount = 0;
            SetTimer(hwnd, TIMER_TRAY_RETRY, 1000, nullptr);
        }
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        if (g_wmTaskbarCreated != 0) {
            ChangeWindowMessageFilterEx(hwnd, g_wmTaskbarCreated, MSGFLT_ALLOW, nullptr);
        }
        if (g_wmShowExisting != 0) {
            ChangeWindowMessageFilterEx(hwnd, g_wmShowExisting, MSGFLT_ALLOW, nullptr);
        }
        CreateUiControls(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        RegisterHotKey(hwnd, HOTKEY_TOGGLE_CONNECT, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'K');
        SetTimer(hwnd, TIMER_TELEMETRY, 250, nullptr);
        if (!AddTrayIcon(hwnd)) {
            g_trayRetryCount = 0;
            SetTimer(hwnd, TIMER_TRAY_RETRY, 1000, nullptr);
        }

        UpdateUiFromState();
        StartAsyncDiscovery();
        if (LoadSetting(L"AutoUpdateCheck", 1) != 0) {
            StartAsyncUpdateCheck(false);
        }
        return 0;

    case WM_DPICHANGED: {
        UINT newDpi = HIWORD(wp);
        RECT* suggested = reinterpret_cast<RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutMainControls(hwnd, newDpi);
        return 0;
    }

    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) {
            SetToggleChecked(hwnd, IDC_CHK_AUTOSTART, s2p::IsAutostartEnabled(s2p::kAutostartSenderValueName));
        }
        break;

    case WM_SETTINGCHANGE:
        if (lp && wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) {
            ApplySenderTheme(hwnd, s2p::ui::IsSystemDarkMode());
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
        s2p::ui::DrawCard(hdc, scRect(16, 12, 508, 116), g_theme, g_currentDpi);
        s2p::ui::DrawStatusDot(hdc, DpiScale(31, g_currentDpi), DpiScale(33, g_currentDpi),
                               std::max(4, DpiScale(5, g_currentDpi)),
                               g_theme.ToneColor(g_heroTone), g_theme.bgCard);

        if (g_activeTab == 0) {
            s2p::ui::DrawCard(hdc, scRect(16, 178, 508, 84), g_theme, g_currentDpi);
            s2p::ui::DrawCard(hdc, scRect(16, 290, 508, 92), g_theme, g_currentDpi);
            s2p::ui::DrawCard(hdc, scRect(16, 398, 508, 246), g_theme, g_currentDpi);
        } else if (g_activeTab == 1) {
            s2p::ui::DrawCard(hdc, scRect(16, 178, 508, 146), g_theme, g_currentDpi, true);
            s2p::ui::DrawCard(hdc, scRect(16, 336, 508, 134), g_theme, g_currentDpi);
        } else if (g_activeTab == 2) {
            s2p::ui::DrawCard(hdc, scRect(16, 178, 508, 286), g_theme, g_currentDpi);
            s2p::ui::DrawCard(hdc, scRect(16, 476, 508, 148), g_theme, g_currentDpi);
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
            inCard = IsSenderCardControlId(ctrlId);
        }
        return s2p::ui::HandleCtlColor(msg, wp, g_theme, inCard);
    }

    case WM_DRAWITEM: {
        const auto* dis = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (!dis || dis->CtlType != ODT_BUTTON) break;
        const int id = static_cast<int>(dis->CtlID);
        if (id == IDC_TAB_CONNECT || id == IDC_TAB_TRANSFER || id == IDC_TAB_SETTINGS) {
            bool active = (id == IDC_TAB_CONNECT && g_activeTab == 0) ||
                          (id == IDC_TAB_TRANSFER && g_activeTab == 1) ||
                          (id == IDC_TAB_SETTINGS && g_activeTab == 2);
            s2p::ui::DrawModernButton(dis, g_theme,
                                      active ? s2p::ui::ButtonStyle::TabActive : s2p::ui::ButtonStyle::TabInactive,
                                      g_currentDpi);
            return TRUE;
        }
        if (id == IDC_BTN_CONNECT) {
            bool isConn = (g_engine && g_engine->IsConnected()) || g_reconnectArmed;
            s2p::ui::DrawModernButton(dis, g_theme,
                                      isConn ? s2p::ui::ButtonStyle::Danger : s2p::ui::ButtonStyle::Primary,
                                      g_currentDpi);
            return TRUE;
        }
        if (id == IDC_CHK_CLIPBOARD || id == IDC_CHK_KVM || id == IDC_CHK_AUTOSTART ||
            id == IDC_CHK_AUTOCONNECT || id == IDC_CHK_AUTORECONNECT || id == IDC_CHK_LID_BYPASS ||
            id == IDC_CHK_DIM_SURFACE || id == IDC_CHK_AUTOUPDATE || id == IDC_CHK_ALLOW_REMOTE) {
            s2p::ui::DrawToggleSwitch(dis, g_theme, IsToggleChecked(hwnd, id), g_currentDpi);
            return TRUE;
        }
        s2p::ui::DrawModernButton(dis, g_theme, s2p::ui::ButtonStyle::Secondary, g_currentDpi);
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_TAB_CONNECT:
            g_activeTab = 0;
            ApplySenderTabVisibility(hwnd);
            return 0;
        case IDC_TAB_TRANSFER:
            g_activeTab = 1;
            ApplySenderTabVisibility(hwnd);
            return 0;
        case IDC_TAB_SETTINGS:
            g_activeTab = 2;
            ApplySenderTabVisibility(hwnd);
            return 0;
        case IDC_BTN_SCAN:
            StartAsyncDiscovery();
            return 0;
        case IDC_BTN_CONNECT:
        case IDM_TOGGLE:
            OnToggleConnect();
            return 0;
        case IDC_MODE_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT idx = SendDlgItemMessageW(hwnd, IDC_MODE_COMBO, CB_GETCURSEL, 0, 0);
                SaveSetting(L"ModeIdx", static_cast<int>(idx));
                if (g_engine && g_engine->IsConnected() && !g_connectBusy.load()) {
                    SwitchDisplayModeWhileConnected(hwnd);
                }
            }
            return 0;
        case IDC_LANG_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int lm = static_cast<int>(SendDlgItemMessageW(hwnd, IDC_LANG_COMBO, CB_GETCURSEL, 0, 0));
                s2p::lang::SetLangMode(lm);
                SaveSetting(L"LangMode", lm);
                ApplySenderLanguage(hwnd);
            }
            return 0;
        case IDC_CODEC_COMBO:
        case IDC_BITRATE_COMBO:
        case IDC_FPS_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                ApplyLiveStreamReconfigureIfConnected(hwnd);
            }
            return 0;
        case IDC_CHK_CLIPBOARD: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_CLIPBOARD);
            if (g_engine) g_engine->SetClipboardEnabled(checked);
            SaveSetting(L"ClipboardSync", checked ? 1 : 0);
            return 0;
        }
        case IDC_CHK_KVM: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_KVM);
            if (g_engine) g_engine->SetKvmEnabled(checked);
            SaveSetting(L"KvmEnabled", checked ? 1 : 0);
            return 0;
        }
        case IDC_CHK_AUTOSTART: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_AUTOSTART);
            if (!s2p::SetAutostartEnabled(checked, s2p::kAutostartSenderValueName)) {
                SetToggleChecked(hwnd, IDC_CHK_AUTOSTART, s2p::IsAutostartEnabled(s2p::kAutostartSenderValueName));
                MessageBoxW(hwnd, L"Autostart-Aufgabe konnte nicht in der Aufgabenplanung eingerichtet werden.",
                            L"Screen2PC Sender", MB_ICONWARNING);
            }
            return 0;
        }
        case IDC_CHK_AUTOCONNECT: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_AUTOCONNECT);
            SaveSetting(L"AutoConnect", checked ? 1 : 0);
            return 0;
        }
        case IDC_CHK_AUTORECONNECT: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_AUTORECONNECT);
            SaveSetting(L"AutoReconnect", checked ? 1 : 0);
            if (!checked) g_reconnectArmed = false;
            return 0;
        }
        case IDC_CHK_LID_BYPASS: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_LID_BYPASS);
            SaveSetting(L"LidCloseBypass", checked ? 1 : 0);
            if (g_engine) SyncSurfacePowerAndDim(g_engine->GetStatus());
            return 0;
        }
        case IDC_CHK_DIM_SURFACE: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_DIM_SURFACE);
            SaveSetting(L"DimSurfaceOnExternal", checked ? 1 : 0);
            g_dimSuppressAuto = false;
            g_dimAttempted = false;
            if (g_engine) SyncSurfacePowerAndDim(g_engine->GetStatus());
            return 0;
        }
        case IDC_CHK_AUTOUPDATE: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_AUTOUPDATE);
            SaveSetting(L"AutoUpdateCheck", checked ? 1 : 0);
            return 0;
        }
        case IDC_CHK_ALLOW_REMOTE: {
            bool checked = ToggleSwitch(hwnd, IDC_CHK_ALLOW_REMOTE);
            SaveSetting(L"AllowRemoteConnect", checked ? 1 : 0);
            if (g_engine) g_engine->SetAllowRemoteConnect(checked);
            if (checked) EnsureSenderRemoteFirewallRuleAsync();
            return 0;
        }
        case IDC_BTN_TRUSTED_PCS:
            OpenTrustedPcsWindow(hwnd);
            return 0;
        case IDC_BTN_UPDATE:
            if (g_pendingUpdate.valid && g_pendingUpdate.updateAvailable) {
                PromptAndApplyUpdate(hwnd, g_pendingUpdate);
            } else {
                StartAsyncUpdateCheck(true);
            }
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
        case IDC_PEER_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT idx = SendDlgItemMessageW(hwnd, IDC_PEER_COMBO, CB_GETCURSEL, 0, 0);
                if (idx >= 0 && static_cast<size_t>(idx) < g_peers.size()) {
                    SetDlgItemTextW(hwnd, IDC_IP_EDIT, s2p::Utf8ToWide(g_peers[idx].ip).c_str());
                }
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

    case WM_HOTKEY:
        if (wp == HOTKEY_TOGGLE_CONNECT) {
            OnToggleConnect();
        }
        return 0;

    case WM_TIMER:
        if (wp == TIMER_TRAY_RETRY) {
            ++g_trayRetryCount;
            if (AddTrayIcon(hwnd) || g_trayRetryCount >= 60) {
                KillTimer(hwnd, TIMER_TRAY_RETRY);
            }
            return 0;
        }
        if (wp == TIMER_TELEMETRY) {
            UpdateUiFromState();
            if (g_reconnectArmed &&
                g_engine &&
                !g_engine->IsConnected() &&
                !g_connectBusy.load() &&
                !g_closing.load() &&
                !g_userManualDisconnect &&
                IsToggleChecked(hwnd, IDC_CHK_AUTORECONNECT) &&
                GetTickCount64() >= g_nextReconnectTick) {
                wchar_t ipBuf[128]{};
                GetDlgItemTextW(hwnd, IDC_IP_EDIT, ipBuf, _countof(ipBuf));
                StartConnectAttempt(ipBuf, false);
            }
        }
        return 0;

    case WM_DISCOVERY_DONE: {
        auto* found = reinterpret_cast<std::vector<s2p::DiscoveredPeer>*>(lp);
        if (found) {
            g_peers = *found;
            delete found;
        } else {
            g_peers.clear();
        }
        g_scanning = false;
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_SCAN), TRUE);
        SetDlgItemTextW(hwnd, IDC_BTN_SCAN, L"Suchen");

        wchar_t curIpBuf[128]{};
        GetDlgItemTextW(hwnd, IDC_IP_EDIT, curIpBuf, _countof(curIpBuf));
        const std::wstring curIpW = TrimWide(curIpBuf);
        const std::wstring savedIpW = TrimWide(LoadLastIp());

        HWND cb = GetDlgItem(hwnd, IDC_PEER_COMBO);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        int matchedCurIdx = -1;
        int matchedSavedIdx = -1;
        int firstCompatIdx = -1;

        for (size_t i = 0; i < g_peers.size(); ++i) {
            const auto& p = g_peers[i];
            std::wstring pIpW = s2p::Utf8ToWide(p.ip);
            std::wstring label = p.pcName + L" (" + pIpW;
            if (p.versionMismatch) {
                label += L" - [Inkompatible Version";
                if (p.remoteProtocolVersion > 0) {
                    label += L" v" + std::to_wstring(p.remoteProtocolVersion);
                }
                if (p.receiverVersion.major > 0 || p.receiverVersion.minor > 0) {
                    label += L" / App v" + s2p::FormatVersionTuple(p.receiverVersion);
                }
                label += L"]";
            } else {
                label += L" - " + std::to_wstring(p.monitors.size()) + L" Mon.";
                if (p.receiverVersion.major > 0 || p.receiverVersion.minor > 0) {
                    label += L", v" + s2p::FormatVersionTuple(p.receiverVersion);
                }
            }
            label += L")";
            SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));

            if (firstCompatIdx < 0 && !p.versionMismatch) firstCompatIdx = static_cast<int>(i);
            if (!curIpW.empty() && pIpW == curIpW) matchedCurIdx = static_cast<int>(i);
            if (!savedIpW.empty() && pIpW == savedIpW) matchedSavedIdx = static_cast<int>(i);
        }

        if (!g_peers.empty()) {
            int selIdx = (matchedCurIdx >= 0) ? matchedCurIdx
                                              : ((matchedSavedIdx >= 0) ? matchedSavedIdx
                                                                        : (firstCompatIdx >= 0 ? firstCompatIdx : 0));
            SendMessageW(cb, CB_SETCURSEL, selIdx, 0);
            // Do not blindly overwrite a user-saved/entered IP unless the IP field was empty.
            if (curIpW.empty()) {
                SetDlgItemTextW(hwnd, IDC_IP_EDIT, s2p::Utf8ToWide(g_peers[selIdx].ip).c_str());
            }
        }

        if (!g_initialAutoConnectHandled) {
            g_initialAutoConnectHandled = true;
            bool autoConn = IsToggleChecked(hwnd, IDC_CHK_AUTOCONNECT);
            if (autoConn && !g_userManualAction && g_engine && !g_engine->IsConnected() &&
                !g_connectBusy.load() && !g_closing.load()) {
                std::wstring targetIpW;
                if (matchedSavedIdx >= 0 && !g_peers[matchedSavedIdx].versionMismatch) {
                    targetIpW = s2p::Utf8ToWide(g_peers[matchedSavedIdx].ip);
                } else if (!savedIpW.empty()) {
                    targetIpW = savedIpW;
                } else if (firstCompatIdx >= 0) {
                    targetIpW = s2p::Utf8ToWide(g_peers[firstCompatIdx].ip);
                }
                if (!targetIpW.empty()) {
                    SetDlgItemTextW(hwnd, IDC_IP_EDIT, targetIpW.c_str());
                    StartConnectAttempt(targetIpW, false);
                }
            }
        }
        return 0;
    }

    case WM_SHOW_PIN_PROMPT: {
        auto* holder = reinterpret_cast<std::shared_ptr<SenderPinRequest>*>(lp);
        std::shared_ptr<SenderPinRequest> req;
        if (holder) {
            req = *holder;
            delete holder;
        }
        if (req && !g_closing.load()) {
            OpenPinPromptWindow(hwnd, req);
        }
        return 0;
    }

    case WM_APP_DIM_DISMISSED:
        g_dimSuppressAuto = true;
        SetToggleChecked(hwnd, IDC_CHK_DIM_SURFACE, false);
        SaveSetting(L"DimSurfaceOnExternal", 0);
        return 0;

    case WM_CONNECT_DONE: {
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_CONNECT), TRUE);
        if (g_engine && !g_closing.load()) {
            auto st = g_engine->GetStatus();
            if (!st.connected && g_reconnectArmed) {
                if (st.authRejected || st.versionMismatch || st.remoteStopped || g_userManualDisconnect) {
                    g_reconnectArmed = false;
                } else {
                    g_reconnectStep = std::min<int>(g_reconnectStep + 1, static_cast<int>(_countof(kReconnectBackoffsMs)) - 1);
                    g_nextReconnectTick = GetTickCount64() + kReconnectBackoffsMs[g_reconnectStep];
                }
            }
            if (!st.connected && st.versionMismatch && !g_versionMismatchPrompted) {
                g_versionMismatchPrompted = true;
                ShowMainWindow(hwnd);
                std::wostringstream msg;
                msg << st.lastError << L"\r\n\r\n"
                    << L"Moechtest du jetzt auf GitHub nach der aktuellen Version suchen?";
                if (MessageBoxW(hwnd, msg.str().c_str(), L"Screen2PC Versionskonflikt",
                                MB_YESNO | MB_ICONWARNING) == IDYES) {
                    StartAsyncUpdateCheck(true);
                }
            }
        }
        UpdateUiFromState();
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
                        std::wstring okMsg = L"Screen2PC Sender ist auf dem neuesten Stand (v" +
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

    case WM_REMOTE_CONNECT_REQ: {
        auto* req = reinterpret_cast<RemoteConnectRequestMsg*>(lp);
        if (req && !g_closing.load() && g_engine && !g_engine->IsConnected() && !g_connectBusy.load()) {
            std::wstring ipW = s2p::Utf8ToWide(req->pcIp);
            if (!ipW.empty()) {
                SetDlgItemTextW(hwnd, IDC_IP_EDIT, ipW.c_str());
                SaveLastIp(ipW);
                StartConnectAttempt(ipW, false);
            }
        }
        delete req;
        return 0;
    }

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wp) {
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_DESTROY:
        g_closing = true;
        g_reconnectArmed = false;
        KillTimer(hwnd, TIMER_TELEMETRY);
        KillTimer(hwnd, TIMER_TRAY_RETRY);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE_CONNECT);
        g_dimOverlay.Hide(false);
        g_lidGuard.Restore(GetIniPath());
        CancelActivePinPrompt();
        if (g_trustedPcsWnd && IsWindow(g_trustedPcsWnd)) {
            DestroyWindow(g_trustedPcsWnd);
            g_trustedPcsWnd = nullptr;
        }
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        g_trayIconAdded = false;
        if (g_engine) {
            g_engine->StopRemoteConnectListener();
            g_engine->SignalStop();
        }
        if (g_connectThread.joinable()) g_connectThread.join();
        if (g_scanThread.joinable()) g_scanThread.join();
        if (g_updateThread.joinable()) g_updateThread.join();
        if (g_engine) g_engine->Disconnect();
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
    if (s2p::HasCliFlag(lpCmdLine, L"--install-autostart")) {
        return s2p::SetAutostartEnabled(true, s2p::kAutostartSenderValueName) ? 0 : 1;
    }
    if (s2p::HasCliFlag(lpCmdLine, L"--uninstall-autostart")) {
        return s2p::SetAutostartEnabled(false, s2p::kAutostartSenderValueName) ? 0 : 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    const bool startInTray = s2p::HasTrayLaunchFlag(lpCmdLine);
    g_wmShowExisting = RegisterWindowMessageW(L"Screen2PC_Sender_ShowWindow");
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    HANDLE hSingleMutex = CreateMutexW(nullptr, FALSE, L"Local\\Screen2PC_Sender_SingleInstance");
    if (hSingleMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!startInTray) {
            HWND existing = nullptr;
            for (int i = 0; i < 10; ++i) {
                existing = FindWindowW(L"Screen2PCSenderWnd", nullptr);
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

    s2p::RefreshAutostartPathIfNeeded(s2p::kAutostartSenderValueName);

    INITCOMMONCONTROLSEX icc{
        sizeof(icc),
        ICC_STANDARD_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES
    };
    InitCommonControlsEx(&icc);

    s2p::Logger::Instance().Init(L"Screen2PC_Sender.log");

    // Crash recovery: restore Windows lid-close sleep setting if an earlier session ended abruptly.
    s2p::LidCloseGuard::RecoverStaleStateOnStartup(GetIniPath());

    s2p::WinsockGuard wsa;
    if (!wsa.Ok()) {
        if (!startInTray) {
            MessageBoxW(nullptr, L"Winsock2 Initialisierung fehlgeschlagen.", L"Screen2PC Sender", MB_ICONERROR);
        }
        if (hSingleMutex) CloseHandle(hSingleMutex);
        CoUninitialize();
        timeEndPeriod(1);
        return 1;
    }

    s2p::SenderEngine engine;
    g_engine = &engine;
    engine.SetConfigIniPath(GetIniPath());
    const bool allowRemoteInit = (LoadSetting(L"AllowRemoteConnect", 1) != 0);
    engine.SetAllowRemoteConnect(allowRemoteInit);
    engine.SetRemoteConnectCallback([](const std::string& pcIp, const std::wstring& pcName) {
        if (g_closing.load() || !g_hwnd || !IsWindow(g_hwnd)) return;
        auto* msg = new RemoteConnectRequestMsg{ pcIp, pcName };
        if (!PostMessageW(g_hwnd, WM_REMOTE_CONNECT_REQ, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    });
    engine.StartRemoteConnectListener();
    if (allowRemoteInit) {
        EnsureSenderRemoteFirewallRuleAsync();
    }
    engine.SetPinPromptCallback([](const std::wstring& pcName,
                                   const std::string& pcIp,
                                   const s2p::AppVersionTuple& pcVersion,
                                   uint8_t remainingAttempts,
                                   const std::wstring& message,
                                   std::string& outPin) -> bool {
        if (g_closing.load() || !g_hwnd || !IsWindow(g_hwnd)) return false;

        auto req = std::make_shared<SenderPinRequest>();
        req->pcName = pcName;
        req->pcIp = pcIp;
        req->pcVersion = pcVersion;
        req->remainingAttempts = remainingAttempts;
        req->message = message;

        {
            std::lock_guard<std::mutex> lk(g_pinReqMutex);
            g_activePinReq = req;
        }

        auto* holder = new std::shared_ptr<SenderPinRequest>(req);
        if (!PostMessageW(g_hwnd, WM_SHOW_PIN_PROMPT, 0, reinterpret_cast<LPARAM>(holder))) {
            delete holder;
            std::lock_guard<std::mutex> lk(g_pinReqMutex);
            if (g_activePinReq == req) g_activePinReq.reset();
            return false;
        }

        {
            std::unique_lock<std::mutex> lk(req->mtx);
            while (!req->done && !req->canceled && !g_closing.load()) {
                req->cv.wait_for(lk, std::chrono::milliseconds(100));
            }
        }

        {
            std::lock_guard<std::mutex> lk(g_pinReqMutex);
            if (g_activePinReq == req) g_activePinReq.reset();
        }

        if (g_closing.load() || req->canceled || !req->submitted) {
            return false;
        }
        outPin = req->outPin;
        return true;
    });

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hIcon = s2p::LoadAppLargeIcon(hInst);
    wc.hIconSm = s2p::LoadAppSmallIcon(hInst);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"Screen2PCSenderWnd";
    RegisterClassExW(&wc);

    std::wstring title = L"Screen2PC Sender v" + s2p::FormatAppVersion(s2p::CurrentAppVersion()) + L" (Laptop)";
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    UINT sysDpi = GetDpiForSystem();
    if (sysDpi < 72) sysDpi = 96;
    RECT wr{ 0, 0, DpiScale(kBaseClientW, sysDpi), DpiScale(kBaseClientH, sysDpi) };
    AdjustWindowRectExForDpi(&wr, style, FALSE, 0, sysDpi);

    g_hwnd = CreateWindowExW(
        WS_EX_COMPOSITED, wc.lpszClassName, title.c_str(),
        style,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInst, nullptr);

    if (g_hwnd) {
        UINT realDpi = GetWindowDpiSafe(g_hwnd);
        if (realDpi != sysDpi) {
            RECT r2{ 0, 0, DpiScale(kBaseClientW, realDpi), DpiScale(kBaseClientH, realDpi) };
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
        if (g_pinPromptWnd && IsWindow(g_pinPromptWnd) && IsDialogMessageW(g_pinPromptWnd, &msg)) {
            continue;
        }
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    g_dimOverlay.Hide(false);
    g_lidGuard.Restore(GetIniPath());
    engine.Disconnect();
    if (hSingleMutex) CloseHandle(hSingleMutex);
    CoUninitialize();
    timeEndPeriod(1);
    return 0;
}
