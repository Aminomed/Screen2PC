#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../common/Logger.h"
#include "../common/NetSocket.h"
#include "SenderEngine.h"
#include "VirtualDisplayCtl.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_DISCOVERY_DONE = WM_APP + 2;
constexpr UINT_PTR TIMER_TELEMETRY = 1;
constexpr int HOTKEY_TOGGLE_CONNECT = 1001;

constexpr int IDC_PEER_COMBO    = 101;
constexpr int IDC_BTN_SCAN      = 102;
constexpr int IDC_IP_EDIT       = 103;
constexpr int IDC_MODE_COMBO    = 104;
constexpr int IDC_CODEC_COMBO   = 105;
constexpr int IDC_BITRATE_COMBO = 106;
constexpr int IDC_BTN_CONNECT   = 107;
constexpr int IDC_STATUS_LABEL  = 108;
constexpr int IDC_DRIVER_LABEL  = 109;
constexpr int IDC_STATS_TEXT    = 110;
constexpr int IDC_CHK_CLIPBOARD = 111;
constexpr int IDC_BTN_SENDFILE  = 112;
constexpr int IDC_BTN_OPEN_DL   = 113;
constexpr int IDC_CHK_KVM       = 114;

constexpr int IDM_SHOW   = 2001;
constexpr int IDM_TOGGLE = 2002;
constexpr int IDM_EXIT   = 2003;

s2p::SenderEngine* g_engine = nullptr;
HWND g_hwnd = nullptr;
HFONT g_font = nullptr;
HFONT g_monoFont = nullptr;
NOTIFYICONDATAW g_nid{};
std::vector<s2p::DiscoveredPeer> g_peers;
bool g_scanning = false;

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

void UpdateUiFromState() {
    if (!g_hwnd || !g_engine) return;
    auto st = g_engine->GetStatus();

    SetDlgItemTextW(g_hwnd, IDC_DRIVER_LABEL,
        st.driverActive
            ? L"Virtual Display Treiber: Aktiv (0..2 virtuelle Monitore bereit)"
            : L"Virtual Display Treiber: Nicht installiert (nur Mirror-Modus oder scripts\\install_driver.ps1 ausfuehren)");

    std::wstring statusLine = L"Status: " + st.stateText;
    if (!st.lastError.empty()) {
        statusLine += L"\r\nHinweis: " + st.lastError;
    }
    SetDlgItemTextW(g_hwnd, IDC_STATUS_LABEL, statusLine.c_str());
    SetDlgItemTextW(g_hwnd, IDC_BTN_CONNECT, st.connected ? L"Trennen (Ctrl+Shift+K)" : L"Verbinden (Ctrl+Shift+K)");

    std::wostringstream ss;
    if (st.streams.empty()) {
        ss << L"Keine aktiven Streams. (Dateien per Drag & Drop ins Fenster ziehen!)\r\n\r\nLetzte Log-Meldungen:\r\n";
        auto logs = s2p::Logger::Instance().GetRecentLines(6);
        for (const auto& l : logs) ss << l << L"\r\n";
    } else {
        for (const auto& s : st.streams) {
            ss << L"Monitor " << (s.monitorIndex + 1) << L" [" << s.width << L"x" << s.height << L"]  "
               << s.codecName << L"  |  "
               << std::fixed << std::setprecision(1) << s.fps << L" FPS  |  "
               << std::setprecision(1) << s.mbps << L" Mbit/s  |  "
               << L"Frames: " << s.framesSent << L"\r\n";
        }
        ss << L"Dateien (Drag & Drop): Gesendet " << st.fileStats.filesSent
           << L" | Empfangen " << st.fileStats.filesReceived;
        if (!st.fileStats.lastEvent.empty()) {
            ss << L" | " << st.fileStats.lastEvent;
        }
        ss << L"\r\n";
    }
    SetDlgItemTextW(g_hwnd, IDC_STATS_TEXT, ss.str().c_str());
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_SENDFILE), st.fileStats.active ? TRUE : FALSE);
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

void StartAsyncDiscovery() {
    if (g_scanning) return;
    g_scanning = true;
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_SCAN), FALSE);
    SetDlgItemTextW(g_hwnd, IDC_BTN_SCAN, L"Suche...");

    std::thread([]() {
        auto* found = new std::vector<s2p::DiscoveredPeer>(s2p::SenderEngine::DiscoverPeers(800));
        if (g_hwnd) {
            PostMessageW(g_hwnd, WM_DISCOVERY_DONE, 0, reinterpret_cast<LPARAM>(found));
        } else {
            delete found;
        }
    }).detach();
}

void OnToggleConnect() {
    if (!g_engine || !g_hwnd) return;
    if (!IsWindowEnabled(GetDlgItem(g_hwnd, IDC_BTN_CONNECT))) return;
    if (g_engine->IsConnected()) {
        g_engine->Disconnect();
        UpdateUiFromState();
        return;
    }

    wchar_t ipBuf[128]{};
    GetDlgItemTextW(g_hwnd, IDC_IP_EDIT, ipBuf, _countof(ipBuf));
    std::wstring ipW(ipBuf);
    while (!ipW.empty() && ipW.back() == L' ') ipW.pop_back();
    if (ipW.empty()) {
        MessageBoxW(g_hwnd, L"Bitte waehle einen gefundenen PC aus oder gib die IP-Adresse des PCs ein.", L"Screen2PC Sender", MB_ICONINFORMATION);
        return;
    }
    SaveLastIp(ipW);

    s2p::SenderConfig cfg;
    cfg.targetIp = s2p::WideToUtf8(ipW);

    LRESULT modeIdx = SendDlgItemMessageW(g_hwnd, IDC_MODE_COMBO, CB_GETCURSEL, 0, 0);
    if (modeIdx == 1) cfg.mode = s2p::DisplayMode::ExtendPrimary;
    else if (modeIdx == 2) cfg.mode = s2p::DisplayMode::ExtendSecondary;
    else if (modeIdx == 3) cfg.mode = s2p::DisplayMode::MirrorPrimary;
    else cfg.mode = s2p::DisplayMode::ExtendAll;

    LRESULT codecIdx = SendDlgItemMessageW(g_hwnd, IDC_CODEC_COMBO, CB_GETCURSEL, 0, 0);
    cfg.codec = (codecIdx == 1) ? s2p::CodecType::DeltaLossless : s2p::CodecType::H264;

    LRESULT brIdx = SendDlgItemMessageW(g_hwnd, IDC_BITRATE_COMBO, CB_GETCURSEL, 0, 0);
    const uint32_t bitrates[] = { 15, 25, 40, 60 };
    cfg.bitrateMbps = (brIdx >= 0 && brIdx < 4) ? bitrates[brIdx] : 25;
    cfg.targetFps = 60;

    SaveSetting(L"ModeIdx", static_cast<int>(modeIdx));
    SaveSetting(L"CodecIdx", static_cast<int>(codecIdx));
    SaveSetting(L"BitrateIdx", static_cast<int>(brIdx));

    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_CONNECT), FALSE);
    SetDlgItemTextW(g_hwnd, IDC_STATUS_LABEL, L"Status: Verbinde und richte Monitore ein...");

    std::thread([cfg]() {
        g_engine->Connect(cfg);
        if (g_hwnd) {
            PostMessageW(g_hwnd, WM_APP + 3, 0, 0);
        }
    }).detach();
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

    addCtrl(L"STATIC", L"Gefundene PCs im LAN:", 0, 16, 14, 180, 20, 0);
    addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 16, 36, 360, 200, IDC_PEER_COMBO);
    addCtrl(L"BUTTON", L"Suchen", BS_PUSHBUTTON | WS_TABSTOP, 386, 35, 110, 26, IDC_BTN_SCAN);

    addCtrl(L"STATIC", L"Ziel-IP (manuell):", 0, 16, 72, 140, 20, 0);
    HWND ipEdit = addCtrl(L"EDIT", LoadLastIp().c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 160, 70, 216, 24, IDC_IP_EDIT);
    (void)ipEdit;

    addCtrl(L"STATIC", L"Modus:", 0, 16, 108, 120, 20, 0);
    HWND modeCb = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 160, 104, 336, 170, IDC_MODE_COMBO);
    SendMessageW(modeCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Erweitern (Alle PC-Monitore, bis zu 2)"));
    SendMessageW(modeCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Erweitern (Nur PC-Monitor 1)"));
    SendMessageW(modeCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Erweitern (Nur PC-Monitor 2)"));
    SendMessageW(modeCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Surface-Display spiegeln (ohne Treiber)"));
    int savedMode = std::clamp(LoadSetting(L"ModeIdx", 0), 0, 3);
    SendMessageW(modeCb, CB_SETCURSEL, savedMode, 0);

    addCtrl(L"STATIC", L"Codec / Bitrate:", 0, 16, 142, 120, 20, 0);
    HWND codecCb = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 160, 138, 216, 120, IDC_CODEC_COMBO);
    SendMessageW(codecCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"H.264 Low-Latency (GPU/CPU)"));
    SendMessageW(codecCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Delta Lossless (4:4:4 Text-scharf)"));
    int savedCodec = std::clamp(LoadSetting(L"CodecIdx", 0), 0, 1);
    SendMessageW(codecCb, CB_SETCURSEL, savedCodec, 0);

    HWND brCb = addCtrl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 386, 138, 110, 120, IDC_BITRATE_COMBO);
    SendMessageW(brCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"15 Mbit/s"));
    SendMessageW(brCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"25 Mbit/s"));
    SendMessageW(brCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"40 Mbit/s"));
    SendMessageW(brCb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60 Mbit/s"));
    int savedBr = std::clamp(LoadSetting(L"BitrateIdx", 1), 0, 3);
    SendMessageW(brCb, CB_SETCURSEL, savedBr, 0);

    addCtrl(L"BUTTON", L"Verbinden (Ctrl+Shift+K)", BS_DEFPUSHBUTTON | WS_TABSTOP, 16, 178, 480, 34, IDC_BTN_CONNECT);

    HWND chkClip = addCtrl(L"BUTTON", L"Geteilte Zwischenablage (Text && Bilder zwischen Surface <-> PC)",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 16, 220, 480, 22, IDC_CHK_CLIPBOARD);
    bool clipEnabled = LoadSetting(L"ClipboardSync", 1) != 0;
    SendMessageW(chkClip, BM_SETCHECK, clipEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_engine) g_engine->SetClipboardEnabled(clipEnabled);

    HWND chkKvm = addCtrl(L"BUTTON", L"KVM: Maus && Tastatur vom PC auf Surface empfangen",
                          BS_AUTOCHECKBOX | WS_TABSTOP, 16, 246, 480, 22, IDC_CHK_KVM);
    bool kvmEnabled = LoadSetting(L"KvmEnabled", 1) != 0;
    SendMessageW(chkKvm, BM_SETCHECK, kvmEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_engine) g_engine->SetKvmEnabled(kvmEnabled);

    addCtrl(L"BUTTON", L"Datei(en) an PC senden...", BS_PUSHBUTTON | WS_TABSTOP, 16, 274, 234, 28, IDC_BTN_SENDFILE);
    addCtrl(L"BUTTON", L"Downloads\\Screen2PC oeffnen", BS_PUSHBUTTON | WS_TABSTOP, 262, 274, 234, 28, IDC_BTN_OPEN_DL);

    addCtrl(L"STATIC", L"", 0, 16, 310, 480, 36, IDC_DRIVER_LABEL);
    addCtrl(L"STATIC", L"Status: Bereit", 0, 16, 348, 480, 40, IDC_STATUS_LABEL);
    addCtrl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 16, 392, 480, 120, IDC_STATS_TEXT, g_monoFont);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Fenster oeffnen");
    AppendMenuW(menu, MF_STRING, IDM_TOGGLE,
                (g_engine && g_engine->IsConnected()) ? L"Trennen" : L"Verbinden");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Beenden");
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        CreateUiControls(hwnd);
        DragAcceptFiles(hwnd, TRUE);
        RegisterHotKey(hwnd, HOTKEY_TOGGLE_CONNECT, MOD_CONTROL | MOD_SHIFT, 'K');
        SetTimer(hwnd, TIMER_TELEMETRY, 500, nullptr);

        g_nid.cbSize = sizeof(g_nid);
        g_nid.hWnd = hwnd;
        g_nid.uID = 1;
        g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        g_nid.uCallbackMessage = WM_TRAYICON;
        g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(g_nid.szTip, L"Screen2PC Sender (Surface)");
        Shell_NotifyIconW(NIM_ADD, &g_nid);

        UpdateUiFromState();
        StartAsyncDiscovery();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_SCAN:
            StartAsyncDiscovery();
            return 0;
        case IDC_BTN_CONNECT:
        case IDM_TOGGLE:
            OnToggleConnect();
            return 0;
        case IDC_CHK_CLIPBOARD:
            if (g_engine) {
                bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_CLIPBOARD, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_engine->SetClipboardEnabled(checked);
                SaveSetting(L"ClipboardSync", checked ? 1 : 0);
            }
            return 0;
        case IDC_CHK_KVM:
            if (g_engine) {
                bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_KVM, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_engine->SetKvmEnabled(checked);
                SaveSetting(L"KvmEnabled", checked ? 1 : 0);
            }
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
        case IDC_PEER_COMBO:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                LRESULT idx = SendDlgItemMessageW(hwnd, IDC_PEER_COMBO, CB_GETCURSEL, 0, 0);
                if (idx >= 0 && static_cast<size_t>(idx) < g_peers.size()) {
                    SetDlgItemTextW(hwnd, IDC_IP_EDIT, s2p::Utf8ToWide(g_peers[idx].ip).c_str());
                }
            }
            return 0;
        case IDM_SHOW:
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
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
        if (wp == TIMER_TELEMETRY) {
            UpdateUiFromState();
        }
        return 0;

    case WM_DISCOVERY_DONE: {
        auto* found = reinterpret_cast<std::vector<s2p::DiscoveredPeer>*>(lp);
        g_peers = *found;
        delete found;
        g_scanning = false;
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_SCAN), TRUE);
        SetDlgItemTextW(hwnd, IDC_BTN_SCAN, L"Suchen");

        HWND cb = GetDlgItem(hwnd, IDC_PEER_COMBO);
        SendMessageW(cb, CB_RESETCONTENT, 0, 0);
        for (const auto& p : g_peers) {
            std::wstring label = p.pcName + L" (" + s2p::Utf8ToWide(p.ip) + L" - " +
                                 std::to_wstring(p.monitors.size()) + L" Mon.)";
            SendMessageW(cb, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (!g_peers.empty()) {
            SendMessageW(cb, CB_SETCURSEL, 0, 0);
            SetDlgItemTextW(hwnd, IDC_IP_EDIT, s2p::Utf8ToWide(g_peers[0].ip).c_str());
        }
        return 0;
    }

    case WM_APP + 3:
        EnableWindow(GetDlgItem(hwnd, IDC_BTN_CONNECT), TRUE);
        UpdateUiFromState();
        return 0;

    case WM_TRAYICON:
        if (lp == WM_LBUTTONDBLCLK) {
            ShowWindow(hwnd, SW_SHOW);
            SetForegroundWindow(hwnd);
        } else if (lp == WM_RBUTTONUP) {
            ShowTrayMenu(hwnd);
        }
        return 0;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TELEMETRY);
        UnregisterHotKey(hwnd, HOTKEY_TOGGLE_CONNECT);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_engine) g_engine->Disconnect();
        if (g_font) DeleteObject(g_font);
        if (g_monoFont) DeleteObject(g_monoFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    s2p::Logger::Instance().Init(L"Screen2PC_Sender.log");
    s2p::WinsockGuard wsa;
    if (!wsa.Ok()) {
        MessageBoxW(nullptr, L"Winsock2 Initialisierung fehlgeschlagen.", L"Screen2PC Sender", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    s2p::SenderEngine engine;
    g_engine = &engine;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"Screen2PCSenderWnd";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Screen2PC Sender (Surface)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 530, 564,
        nullptr, nullptr, hInst, nullptr);

    ShowWindow(g_hwnd, nCmdShow);
    UpdateWindow(g_hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    engine.Disconnect();
    CoUninitialize();
    return 0;
}
