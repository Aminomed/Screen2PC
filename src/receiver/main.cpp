#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <timeapi.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../common/Logger.h"
#include "../common/NetSocket.h"
#include "ReceiverEngine.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

constexpr UINT WM_TRAYICON = WM_APP + 10;
constexpr UINT_PTR TIMER_TELEMETRY = 1;

constexpr int IDC_INFO_LABEL   = 201;
constexpr int IDC_CHK_FULLSCR  = 202;
constexpr int IDC_BTN_DISCONN  = 203;
constexpr int IDC_STATS_TEXT   = 204;
constexpr int IDC_BTN_FIREWALL = 205;
constexpr int IDC_CHK_CLIPBOARD= 206;
constexpr int IDC_BTN_SENDFILE = 207;
constexpr int IDC_BTN_OPEN_DL  = 208;
constexpr int IDC_CHK_KVM      = 209;

constexpr int IDM_SHOW = 3001;
constexpr int IDM_EXIT = 3002;

s2p::ReceiverEngine* g_engine = nullptr;
HWND g_hwnd = nullptr;
HFONT g_font = nullptr;
HFONT g_monoFont = nullptr;
NOTIFYICONDATAW g_nid{};
std::thread g_disconnThread;

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

bool LoadKvmPref() {
    return GetPrivateProfileIntW(L"Receiver", L"KvmEnabled", 1, GetReceiverIniPath().c_str()) != 0;
}

void SaveKvmPref(bool enabled) {
    WritePrivateProfileStringW(L"Receiver", L"KvmEnabled", enabled ? L"1" : L"0", GetReceiverIniPath().c_str());
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

    addCtrl(L"STATIC", L"Initialisiere...", 0, 16, 14, 520, 72, IDC_INFO_LABEL);

    HWND chk = addCtrl(L"BUTTON", L"Streams direkt im randlosen Vollbild auf PC-Monitoren oeffnen (F11 zum Umschalten)",
                       BS_AUTOCHECKBOX | WS_TABSTOP, 16, 94, 520, 24, IDC_CHK_FULLSCR);
    SendMessageW(chk, BM_SETCHECK, LoadFullscreenPref() ? BST_CHECKED : BST_UNCHECKED, 0);

    HWND chkClip = addCtrl(L"BUTTON", L"Geteilte Zwischenablage (Text && Bilder zwischen Surface <-> PC)",
                           BS_AUTOCHECKBOX | WS_TABSTOP, 16, 122, 520, 24, IDC_CHK_CLIPBOARD);
    bool clipPref = LoadClipboardPref();
    SendMessageW(chkClip, BM_SETCHECK, clipPref ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_engine) g_engine->SetClipboardEnabled(clipPref);

    HWND chkKvm = addCtrl(L"BUTTON", L"KVM: Maus && Tastatur ans Surface senden (Strg+Alt+K im Streamfenster)",
                          BS_AUTOCHECKBOX | WS_TABSTOP, 16, 150, 520, 24, IDC_CHK_KVM);
    bool kvmPref = LoadKvmPref();
    SendMessageW(chkKvm, BM_SETCHECK, kvmPref ? BST_CHECKED : BST_UNCHECKED, 0);
    if (g_engine) g_engine->SetKvmEnabled(kvmPref);

    addCtrl(L"BUTTON", L"Aktive Session trennen", BS_PUSHBUTTON | WS_TABSTOP, 16, 184, 250, 32, IDC_BTN_DISCONN);
    addCtrl(L"BUTTON", L"Windows Firewall Ports freigeben (Admin)", BS_PUSHBUTTON | WS_TABSTOP, 276, 184, 260, 32, IDC_BTN_FIREWALL);

    addCtrl(L"BUTTON", L"Datei(en) ans Surface senden...", BS_PUSHBUTTON | WS_TABSTOP, 16, 222, 250, 28, IDC_BTN_SENDFILE);
    addCtrl(L"BUTTON", L"Downloads\\Screen2PC oeffnen", BS_PUSHBUTTON | WS_TABSTOP, 276, 222, 260, 28, IDC_BTN_OPEN_DL);

    addCtrl(L"EDIT", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | WS_VSCROLL, 16, 258, 520, 160, IDC_STATS_TEXT, g_monoFont);
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
        case IDC_CHK_KVM:
            if (g_engine) {
                bool checked = SendDlgItemMessageW(hwnd, IDC_CHK_KVM, BM_GETCHECK, 0, 0) == BST_CHECKED;
                g_engine->SetKvmEnabled(checked);
                SaveKvmPref(checked);
            }
            return 0;
        case IDC_BTN_DISCONN:
            if (g_engine) {
                if (g_disconnThread.joinable()) g_disconnThread.join();
                g_disconnThread = std::thread([]() { if (g_engine) g_engine->DisconnectSession(); });
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
        if (g_disconnThread.joinable()) g_disconnThread.join();
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
    timeBeginPeriod(1);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    s2p::Logger::Instance().Init(L"Screen2PC_Receiver.log");
    s2p::WinsockGuard wsa;
    if (!wsa.Ok()) {
        MessageBoxW(nullptr, L"Winsock2 Initialisierung fehlgeschlagen.", L"Screen2PC Receiver", MB_ICONERROR);
        CoUninitialize();
        timeEndPeriod(1);
        return 1;
    }

    s2p::ReceiverEngine engine;
    g_engine = &engine;
    engine.SetClipboardEnabled(LoadClipboardPref());
    engine.SetKvmEnabled(LoadKvmPref());
    if (!engine.Start(LoadFullscreenPref())) {
        MessageBoxW(nullptr, L"Konnte Receiver-Port 48211 nicht oeffnen (evtl. laeuft Screen2PC Receiver bereits).",
                    L"Screen2PC Receiver", MB_ICONERROR);
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

    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"Screen2PC Receiver (PC - Treiberfrei)",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 570, 474,
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
    timeEndPeriod(1);
    return 0;
}
