#include "Autostart.h"

#include <cwctype>
#include <vector>

namespace s2p {

namespace {

constexpr const wchar_t* kRunRegKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kStartupApprovedKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

std::wstring ToLowerCopy(std::wstring s) {
    for (wchar_t& ch : s) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return s;
}

std::wstring TrimWhitespace(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && std::iswspace(s[start])) ++start;
    size_t end = s.size();
    while (end > start && std::iswspace(s[end - 1])) --end;
    return s.substr(start, end - start);
}

std::wstring CurrentExePath() {
    std::vector<wchar_t> buf(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return L"";
    return std::wstring(buf.data(), n);
}

bool IsStartupApprovedDisabled(const wchar_t* valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupApprovedKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    BYTE data[32]{};
    DWORD type = 0;
    DWORD cb = sizeof(data);
    LSTATUS st = RegQueryValueExW(hKey, valueName, nullptr, &type, data, &cb);
    RegCloseKey(hKey);
    if (st == ERROR_SUCCESS && type == REG_BINARY && cb >= 1) {
        // Windows Task Manager marks enabled entries with even status byte (0x02/0x06)
        // and disabled entries with odd status byte (0x03/0x07).
        return (data[0] & 0x01) != 0;
    }
    return false;
}

bool IsSenderTaskMode(const wchar_t* valueName) {
    return valueName && _wcsicmp(valueName, kAutostartSenderValueName) == 0;
}

bool RunSchtasks(const std::wstring& args, std::wstring* outStdout = nullptr) {
    std::wstring cmd = L"schtasks.exe " + args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    HANDLE hRead = nullptr;
    HANDLE hWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (outStdout) {
        CreatePipe(&hRead, &hWrite, &sa, 0);
        if (hRead) SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (outStdout && hWrite) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
    }

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr,
                             (outStdout && hWrite) ? TRUE : FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (hWrite) CloseHandle(hWrite);

    if (!ok) {
        if (hRead) CloseHandle(hRead);
        return false;
    }

    if (outStdout && hRead) {
        std::string raw;
        char buf[1024];
        DWORD bytesRead = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
            raw.append(buf, bytesRead);
        }
        if (!raw.empty()) {
            int wlen = MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
            if (wlen > 0) {
                outStdout->resize(wlen);
                MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()), outStdout->data(), wlen);
            }
        }
    }
    if (hRead) CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, 5000);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return exitCode == 0;
}

std::wstring EscapeForCmdArg(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t ch : s) {
        if (ch == L'"') {
            out += L"\\\"";
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

void DeleteLegacyRunEntries(const wchar_t* valueName) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_WRITE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, valueName);
        RegCloseKey(hKey);
    }
    HKEY hAppKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupApprovedKey, 0, KEY_WRITE, &hAppKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hAppKey, valueName);
        RegCloseKey(hAppKey);
    }
}

} // namespace

bool HasCliFlag(const wchar_t* cmdLine, const wchar_t* flag) {
    if (!cmdLine || !flag || !*flag) return false;
    std::wstring line(cmdLine);
    std::wstring needle = ToLowerCopy(flag);

    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::iswspace(line[i])) ++i;
        if (i >= line.size()) break;

        std::wstring token;
        if (line[i] == L'"') {
            ++i;
            while (i < line.size() && line[i] != L'"') {
                token.push_back(line[i++]);
            }
            if (i < line.size() && line[i] == L'"') ++i;
        } else {
            while (i < line.size() && !std::iswspace(line[i])) {
                token.push_back(line[i++]);
            }
        }
        if (ToLowerCopy(token) == needle) {
            return true;
        }
    }
    return false;
}

bool HasTrayLaunchFlag(const wchar_t* cmdLine) {
    return HasCliFlag(cmdLine, L"--tray") ||
           HasCliFlag(cmdLine, L"-tray") ||
           HasCliFlag(cmdLine, L"--minimized") ||
           HasCliFlag(cmdLine, L"-minimized");
}

std::wstring BuildAutostartCommand(const std::wstring& exePath) {
    if (exePath.empty()) return L"";
    return L"\"" + exePath + L"\" --tray";
}

std::wstring ExtractExePathFromCommand(const std::wstring& cmd) {
    std::wstring s = TrimWhitespace(cmd);
    if (s.empty()) return L"";
    if (s.front() == L'"') {
        size_t close = s.find(L'"', 1);
        if (close != std::wstring::npos) {
            return s.substr(1, close - 1);
        }
        return s.substr(1);
    }
    size_t sp = s.find_first_of(L" \t");
    if (sp != std::wstring::npos) {
        return s.substr(0, sp);
    }
    return s;
}

bool IsAutostartEnabled(const wchar_t* valueName) {
    if (!valueName || !*valueName) return false;
    if (IsSenderTaskMode(valueName)) {
        return RunSchtasks(L"/Query /TN \"Screen2PC Sender\"");
    }

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    wchar_t buf[2048]{};
    DWORD type = 0;
    DWORD cb = sizeof(buf);
    LSTATUS st = RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(buf), &cb);
    RegCloseKey(hKey);
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || cb <= sizeof(wchar_t)) {
        return false;
    }
    if (IsStartupApprovedDisabled(valueName)) {
        return false;
    }
    return true;
}

bool SetAutostartEnabled(bool enabled, const wchar_t* valueName) {
    if (!valueName || !*valueName) return false;

    if (IsSenderTaskMode(valueName)) {
        DeleteLegacyRunEntries(valueName);
        if (enabled) {
            std::wstring exe = CurrentExePath();
            std::wstring cmd = BuildAutostartCommand(exe);
            if (cmd.empty()) return false;
            std::wstring args = L"/Create /TN \"Screen2PC Sender\" /TR \"" +
                                EscapeForCmdArg(cmd) + L"\" /SC ONLOGON /RL HIGHEST /F";
            return RunSchtasks(args);
        } else {
            RunSchtasks(L"/Delete /TN \"Screen2PC Sender\" /F");
            return true;
        }
    }

    if (enabled) {
        std::wstring exe = CurrentExePath();
        std::wstring cmd = BuildAutostartCommand(exe);
        if (cmd.empty()) return false;

        HKEY hKey = nullptr;
        LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, nullptr, 0,
                                     KEY_WRITE, nullptr, &hKey, nullptr);
        if (st != ERROR_SUCCESS) return false;
        DWORD bytes = static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t));
        st = RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()), bytes);
        RegCloseKey(hKey);
        if (st != ERROR_SUCCESS) return false;

        // Clear any Task Manager disabled state so it shows Enabled immediately
        HKEY hAppKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kStartupApprovedKey, 0, KEY_WRITE, &hAppKey) == ERROR_SUCCESS) {
            BYTE enabledVal[12]{ 0x02 };
            RegSetValueExW(hAppKey, valueName, 0, REG_BINARY, enabledVal, sizeof(enabledVal));
            RegCloseKey(hAppKey);
        }
        return true;
    } else {
        DeleteLegacyRunEntries(valueName);
        return true;
    }
}

void RefreshAutostartPathIfNeeded(const wchar_t* valueName) {
    if (IsSenderTaskMode(valueName)) {
        if (!IsAutostartEnabled(valueName)) return;
        std::wstring xml;
        if (RunSchtasks(L"/Query /TN \"Screen2PC Sender\" /XML", &xml)) {
            std::wstring exe = CurrentExePath();
            if (!exe.empty() && ToLowerCopy(xml).find(ToLowerCopy(exe)) == std::wstring::npos) {
                SetAutostartEnabled(true, valueName);
            }
        }
        return;
    }

    if (!IsAutostartEnabled(valueName)) return;

    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegKey, 0, KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return;
    }
    wchar_t buf[2048]{};
    DWORD type = 0;
    DWORD cb = sizeof(buf);
    if (RegQueryValueExW(hKey, valueName, nullptr, &type, reinterpret_cast<BYTE*>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        std::wstring currentCmd(buf);
        std::wstring expectedCmd = BuildAutostartCommand(CurrentExePath());
        if (!expectedCmd.empty() && _wcsicmp(currentCmd.c_str(), expectedCmd.c_str()) != 0) {
            DWORD bytes = static_cast<DWORD>((expectedCmd.size() + 1) * sizeof(wchar_t));
            RegSetValueExW(hKey, valueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(expectedCmd.c_str()), bytes);
        }
    }
    RegCloseKey(hKey);
}

} // namespace s2p
