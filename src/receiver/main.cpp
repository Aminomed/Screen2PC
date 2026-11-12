#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <objbase.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <thread>

#include "../common/Logger.h"
#include "../common/NetSocket.h"
#include "ReceiverEngine.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 10;
constexpr UINT_PTR TIMER_TELEMETRY = 1;

constexpr int IDC_INFO_LABEL   = 201;
constexpr int IDC_CHK_FULLSCR  = 202;
constexpr int IDC_BTN_DISCONN  = 203;
constexpr int IDC_STATS_TEXT   = 204;
constexpr int IDC_BTN_FIREWALL = 205;
constexpr int IDC_CHK_CLIPBOARD= 206;

constexpr int IDM_SHOW = 3001;
constexpr int IDM_EXIT = 3002;

s2p::ReceiverEngine* g_engine = nullptr;
HWND g_hwnd = nullptr;
HFONT g_font = nullptr;
HFONT g_monoFont = nullptr;
NOTIFYICONDATAW g_nid{};

std::wstring GetReceiverIniPath() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    std::wstring dir = (n > 0 && n < MAX_PATH) ? std::wstring(buf) + L"\\Screen2PC" : L".";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\receiver.ini";
}

bool LoadFullscreenPref() {
    return GetPrivateProfileIntW(L"Receiver", L"StartFullscreen", 1, GetReceiverIniPath().c_str()) != 0;
}

void SaveFullscreenPref(bool fs) {
    WritePrivateProfileStringW(L"Receiver", L"StartFullscreen", fs ? L"1" : L"0", GetReceiverIniPath().c_str());
}

bool LoadClipboardPref() {
    return GetPrivateProfileIntW(L"Receiver", L"ClipboardSync", 1, GetReceiverIniPath().c_str()) != 0;
}

void SaveClipboardPref(bool enabled) {
    WritePrivateProfileStringW(L"Receiver", L"ClipboardSync", enabled ? L"1" : L"0", GetReceiverIniPath().c_str());
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
    hdr << L"\r\nErkannte Monitore (" << st.localMonitors.size() << L"): ";
    for (size_t i = 0; i < st.localMonitors.size(); ++i) {
        if (i > 0) hdr << L"  |  ";
        hdr << L"#" << (i + 1) << L" " << st.localMonitors[i].width << L"x" << st.localMonitors[i].height
            << L"@" << st.localMonitors[i].refreshHz << L"Hz";
        if (st.localMonitors[i].isPrimary) hdr << L" (Primary)";
    }
    SetDlgItemTextW(g_hwnd, IDC_INFO_LABEL, hdr.str().c_str());
    EnableWindow(GetDlgItem(g_hwnd, IDC_BTN_DISCONN), st.sessionActive ? TRUE : FALSE);

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
    }
    SetDlgItemTextW(g_hwnd, IDC_STATS_TEXT, body.str().c_str());
}

void OpenFirewallPortsElevated() {
    std::wstring args = L"advfirewall firewall add rule name=\"Screen2PC\" dir=in action=allow protocol=TCP localport=48211-48215";
    ShellExecuteW(g_hwnd, L"runas", L"netsh.exe", args.c_str(), nullptr, SW_HIDE);
    std::wstring argsUdp = L"advfirewall firewall add rule name=\"Screen2PC Discovery\" dir=in action=allow protocol=UDP localport=48210";
    ShellExecuteW(g_hwnd, L"runas", L"netsh.exe", argsUdp.c_str(), nullptr, SW_HIDE);
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

    addCtrl(L"STATIC", L"Initialisiere...", 0, 16, 14, 520, 72, IDC_INFO_LABEL);

    HWND chk = addCtrl(L"BUTTON", L"Streams direkt im randlosen Vollbild auf PC-Monitoren oeffnen (F11 zum Umschalten)",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 16, 94, 520, 24, IDC_CHK_FULLSCR);
    SendMessageW(chk, BM_SETCHECK, LoadFullscreenPref() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND chkClip = addCtrl(L"BUTTON", L"Geteilte Zwischenablage (Text && Bilder zwischen Surface <-> PC)",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 16, 122, 520, 24, IDC_CHK_CLIPBOARD);
    bool clipPref = LoadClipboardPref();
    SendMessageW(chkClip, BM_SETCHECK, clipPref ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_engine) g_engine->SetClipboardEnabled(clipPref);

    addCtrl(L"BUTTON", L"Aktive Session trennen", BS_PUSHBUTTON | WS_TABSTOP, 16, 156, 250, 32, IDC_BTN_DISCONN);
    addCtrl(L"BUTTON", L"Windows Firewall Ports freigeben (Admin)", BS_PUSHBUTTON | WS_TABSTOP, 276, 156, 260, 32, IDC_BTN_FIREWALL);

    addCtrl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 16, 200, 520, 160, IDC_STATS_TEXT, g_monoFont);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_SHOW, L"Statusfenster oeffnen");
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
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CHK_FULLSCR:
            if (g_engine) {
                bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_FULLSCR, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_engine->SetStartFullscreen(checked);
                SaveFullscreenPref(checked);
            }
            return 0;
        case IDC_CHK_CLIPBOARD:
            if (g_engine) {
                bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_CLIPBOARD, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_engine->SetClipboardEnabled(checked);
                SaveClipboardPref(checked);
            }
            return 0;
        case IDC_BTN_DISCONN:
            if (g_engine) {
                std::thread([]() { g_engine->DisconnectSession(); }).detach();
            }
            return 0;
        case IDC_BTN_FIREWALL:
            OpenFirewallPortsElevated();
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

    case WM_TIMER:
        if (wp == TIMER_TELEMETRY) UpdateUi();
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
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        if (g_engine) g_engine->Stop();
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

    s2p::Logger::Instance().Init(L"Screen2PC_Receiver.log");
    s2p::WinsockGuard wsa;
    if (!wsa.Ok()) {
        MessageBoxW(nullptr, L"Winsock2 Initialisierung fehlgeschlagen.", L"Screen2PC Receiver", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    s2p::ReceiverEngine engine;
    g_engine = &engine;
    engine.SetClipboardEnabled(LoadClipboardPref());
    if (!engine.Start(LoadFullscreenPref())) {
        MessageBoxW(nullptr, L"Konnte Receiver-Port 48211 nicht oeffnen (evtl. laeuft Screen2PC Receiver bereits).",
                    L"Screen2PC Receiver", MB_ICONERROR);
        CoUninitialize();
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

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Screen2PC Receiver (PC - Treiberfrei)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 570, 418,
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

    engine.Stop();
    CoUninitialize();
    return 0;
}
