#include "Autostart.h"

#include <sddl.h>
#include <cstring>
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

std::wstring XmlEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 16);
    for (wchar_t ch : s) {
        switch (ch) {
        case L'&':  out += L"&amp;"; break;
        case L'<':  out += L"&lt;"; break;
        case L'>':  out += L"&gt;"; break;
        case L'"':  out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default:    out.push_back(ch); break;
        }
    }
    return out;
}

std::wstring CurrentUserSidString() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        return L"";
    }
    DWORD needed = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &needed);
    std::wstring sidStr;
    if (needed > 0) {
        std::vector<BYTE> buf(needed);
        if (GetTokenInformation(hToken, TokenUser, buf.data(), needed, &needed)) {
            const auto* user = reinterpret_cast<const TOKEN_USER*>(buf.data());
            LPWSTR pSid = nullptr;
            if (user && user->User.Sid && ConvertSidToStringSidW(user->User.Sid, &pSid) && pSid) {
                sidStr = pSid;
                LocalFree(pSid);
            }
        }
    }
    CloseHandle(hToken);
    return sidStr;
}

bool WriteUtf16LeFile(const std::wstring& path, const std::wstring& content) {
    HANDLE hFile = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }
    const uint16_t bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(hFile, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    if (ok && !content.empty()) {
        const DWORD bytes = static_cast<DWORD>(content.size() * sizeof(wchar_t));
        ok = WriteFile(hFile, content.data(), bytes, &written, nullptr) && written == bytes;
    }
    CloseHandle(hFile);
    if (!ok) {
        DeleteFileW(path.c_str());
    }
    return ok;
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

bool HasRunRegEntry(const wchar_t* valueName) {
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
    return !IsStartupApprovedDisabled(valueName);
}

bool IsSenderTaskMode(const wchar_t* valueName) {
    return valueName && _wcsicmp(valueName, kAutostartSenderValueName) == 0;
}

std::wstring DecodeProcessBytesToWide(const std::string& raw) {
    if (raw.empty()) return L"";

    // UTF-16LE with BOM
    if (raw.size() >= 2 &&
        static_cast<unsigned char>(raw[0]) == 0xFF &&
        static_cast<unsigned char>(raw[1]) == 0xFE) {
        size_t wcharCount = (raw.size() - 2) / sizeof(wchar_t);
        std::wstring out(wcharCount, L'\0');
        if (wcharCount > 0) {
            std::memcpy(out.data(), raw.data() + 2, wcharCount * sizeof(wchar_t));
        }
        return out;
    }

    // Heuristic for UTF-16LE without BOM (e.g. "<?xml" -> '<', 0, '?', 0)
    if (raw.size() >= 4 && raw[1] == '\0' && raw[3] == '\0') {
        size_t wcharCount = raw.size() / sizeof(wchar_t);
        std::wstring out(wcharCount, L'\0');
        std::memcpy(out.data(), raw.data(), wcharCount * sizeof(wchar_t));
        return out;
    }

    const char* ptr = raw.data();
    int len = static_cast<int>(raw.size());
    if (raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
        ptr += 3;
        len -= 3;
    }
    if (len <= 0) return L"";

    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ptr, len, nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (wlen <= 0) {
        cp = CP_OEMCP;
        flags = 0;
        wlen = MultiByteToWideChar(cp, flags, ptr, len, nullptr, 0);
    }
    if (wlen <= 0) return L"";

    std::wstring out(wlen, L'\0');
    MultiByteToWideChar(cp, flags, ptr, len, out.data(), wlen);
    return out;
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
        *outStdout = DecodeProcessBytesToWide(raw);
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

std::wstring BuildSenderTaskXml(const std::wstring& exePath, const std::wstring& userSid) {
    if (exePath.empty()) return L"";

    std::wstring workDir;
    const size_t slash = exePath.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 0) {
        workDir = exePath.substr(0, slash);
    }

    const std::wstring escExe = XmlEscape(exePath);
    const std::wstring escDir = XmlEscape(workDir);
    const std::wstring escSid = XmlEscape(userSid);

    std::wstring xml;
    xml.reserve(1800);
    xml += L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n";
    xml += L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n";
    xml += L"  <RegistrationInfo>\r\n";
    xml += L"    <Description>Starts Screen2PC Sender minimized in the system tray at user logon.</Description>\r\n";
    xml += L"  </RegistrationInfo>\r\n";
    xml += L"  <Triggers>\r\n";
    xml += L"    <LogonTrigger>\r\n";
    xml += L"      <Enabled>true</Enabled>\r\n";
    if (!escSid.empty()) {
        xml += L"      <UserId>" + escSid + L"</UserId>\r\n";
    }
    xml += L"    </LogonTrigger>\r\n";
    xml += L"  </Triggers>\r\n";
    xml += L"  <Principals>\r\n";
    xml += L"    <Principal id=\"Author\">\r\n";
    if (!escSid.empty()) {
        xml += L"      <UserId>" + escSid + L"</UserId>\r\n";
    }
    xml += L"      <LogonType>InteractiveToken</LogonType>\r\n";
    xml += L"      <RunLevel>HighestAvailable</RunLevel>\r\n";
    xml += L"    </Principal>\r\n";
    xml += L"  </Principals>\r\n";
    xml += L"  <Settings>\r\n";
    xml += L"    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>\r\n";
    xml += L"    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>\r\n";
    xml += L"    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>\r\n";
    xml += L"    <AllowHardTerminate>false</AllowHardTerminate>\r\n";
    xml += L"    <StartWhenAvailable>true</StartWhenAvailable>\r\n";
    xml += L"    <RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>\r\n";
    xml += L"    <IdleSettings>\r\n";
    xml += L"      <StopOnIdleEnd>false</StopOnIdleEnd>\r\n";
    xml += L"      <RestartOnIdle>false</RestartOnIdle>\r\n";
    xml += L"    </IdleSettings>\r\n";
    xml += L"    <AllowStartOnDemand>true</AllowStartOnDemand>\r\n";
    xml += L"    <Enabled>true</Enabled>\r\n";
    xml += L"    <Hidden>false</Hidden>\r\n";
    xml += L"    <RunOnlyIfIdle>false</RunOnlyIfIdle>\r\n";
    xml += L"    <WakeToRun>false</WakeToRun>\r\n";
    xml += L"    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>\r\n";
    xml += L"    <Priority>7</Priority>\r\n";
    xml += L"  </Settings>\r\n";
    xml += L"  <Actions Context=\"Author\">\r\n";
    xml += L"    <Exec>\r\n";
    xml += L"      <Command>" + escExe + L"</Command>\r\n";
    xml += L"      <Arguments>--tray</Arguments>\r\n";
    if (!escDir.empty()) {
        xml += L"      <WorkingDirectory>" + escDir + L"</WorkingDirectory>\r\n";
    }
    xml += L"    </Exec>\r\n";
    xml += L"  </Actions>\r\n";
    xml += L"</Task>\r\n";
    return xml;
}

bool IsSenderTaskXmlEnabled(const std::wstring& taskXml) {
    if (taskXml.empty()) return false;
    const std::wstring lower = ToLowerCopy(taskXml);
    return lower.find(L"<enabled>false</enabled>") == std::wstring::npos;
}

bool NeedsSenderTaskUpgrade(const std::wstring& taskXml, const std::wstring& expectedExePath) {
    if (taskXml.empty() || expectedExePath.empty()) return true;
    const std::wstring lower = ToLowerCopy(taskXml);
    const std::wstring lowerExe = ToLowerCopy(expectedExePath);

    if (lower.find(lowerExe) == std::wstring::npos &&
        lower.find(ToLowerCopy(XmlEscape(expectedExePath))) == std::wstring::npos) {
        return true;
    }
    if (lower.find(L"--tray") == std::wstring::npos) {
        return true;
    }
    if (lower.find(L"<disallowstartifonbatteries>false</disallowstartifonbatteries>") == std::wstring::npos) {
        return true;
    }
    if (lower.find(L"<stopifgoingonbatteries>false</stopifgoingonbatteries>") == std::wstring::npos) {
        return true;
    }
    if (lower.find(L"<startwhenavailable>true</startwhenavailable>") == std::wstring::npos) {
        return true;
    }
    if (lower.find(L"<executiontimelimit>pt0s</executiontimelimit>") == std::wstring::npos) {
        return true;
    }
    if (lower.find(L"<runlevel>highestavailable</runlevel>") == std::wstring::npos) {
        return true;
    }
    return false;
}

bool IsAutostartEnabled(const wchar_t* valueName) {
    if (!valueName || !*valueName) return false;
    if (IsSenderTaskMode(valueName)) {
        std::wstring xml;
        if (!RunSchtasks(L"/Query /TN \"Screen2PC Sender\" /XML", &xml)) {
            return false;
        }
        return IsSenderTaskXmlEnabled(xml);
    }

    return HasRunRegEntry(valueName);
}

bool SetAutostartEnabled(bool enabled, const wchar_t* valueName) {
    if (!valueName || !*valueName) return false;

    if (IsSenderTaskMode(valueName)) {
        DeleteLegacyRunEntries(valueName);
        if (enabled) {
            std::wstring exe = CurrentExePath();
            if (exe.empty()) return false;

            const std::wstring xml = BuildSenderTaskXml(exe, CurrentUserSidString());
            wchar_t tempDir[MAX_PATH]{};
            DWORD tn = GetTempPathW(MAX_PATH, tempDir);
            if (!xml.empty() && tn > 0 && tn < MAX_PATH) {
                std::wstring xmlPath = std::wstring(tempDir) + L"s2p_sender_task_" +
                                       std::to_wstring(GetCurrentProcessId()) + L".xml";
                if (WriteUtf16LeFile(xmlPath, xml)) {
                    std::wstring args = L"/Create /TN \"Screen2PC Sender\" /XML \"" +
                                        EscapeForCmdArg(xmlPath) + L"\" /F";
                    bool created = RunSchtasks(args);
                    DeleteFileW(xmlPath.c_str());
                    if (created) return true;
                }
            }

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
        std::wstring xml;
        if (RunSchtasks(L"/Query /TN \"Screen2PC Sender\" /XML", &xml)) {
            DeleteLegacyRunEntries(valueName);
            std::wstring exe = CurrentExePath();
            if (!exe.empty() && IsSenderTaskXmlEnabled(xml) && NeedsSenderTaskUpgrade(xml, exe)) {
                SetAutostartEnabled(true, valueName);
            }
        } else if (HasRunRegEntry(valueName)) {
            // Migrate legacy HKCU\...\Run entry (blocked by UAC RequireAdministrator) to Task Scheduler XML
            SetAutostartEnabled(true, valueName);
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
