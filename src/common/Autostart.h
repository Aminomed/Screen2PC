#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>

namespace s2p {

constexpr const wchar_t* kAutostartValueName = L"Screen2PC Receiver";

// Command-line flag checks (case-insensitive)
bool HasCliFlag(const wchar_t* cmdLine, const wchar_t* flag);
bool HasTrayLaunchFlag(const wchar_t* cmdLine);

// Builds `"C:\path\to\exe" --tray` safely quoted for HKCU\...\Run
std::wstring BuildAutostartCommand(const std::wstring& exePath);

// Extracts the executable path from a HKCU\...\Run command string (quoted or unquoted)
std::wstring ExtractExePathFromCommand(const std::wstring& cmd);

// Checks both HKCU\Software\Microsoft\Windows\CurrentVersion\Run and Task Manager StartupApproved\Run
bool IsAutostartEnabled(const wchar_t* valueName = kAutostartValueName);

// Enables or disables autostart under HKCU\...\Run and clears any Task Manager disable flag in StartupApproved\Run
bool SetAutostartEnabled(bool enabled, const wchar_t* valueName = kAutostartValueName);

// Updates the stored executable path in HKCU\...\Run if autostart is currently enabled and points elsewhere
void RefreshAutostartPathIfNeeded(const wchar_t* valueName = kAutostartValueName);

} // namespace s2p
