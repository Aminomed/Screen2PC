#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <string>

namespace s2p {

struct AppVersion {
    int major = 0;
    int minor = 0;
    int patch = 0;
    int build = 0;

    bool operator==(const AppVersion& other) const = default;
};

struct UpdateReleaseInfo {
    bool valid = false;
    bool updateAvailable = false;
    bool prerelease = false;
    uint32_t httpStatus = 0;
    AppVersion version;
    std::wstring tagName;
    std::wstring releaseName;
    std::wstring htmlUrl;
    std::wstring downloadUrl;
    std::wstring releaseNotes;
    uint64_t assetSize = 0;
    std::wstring error;
};

AppVersion CurrentAppVersion();
std::wstring FormatAppVersion(const AppVersion& v);

bool ParseVersionString(const std::string& raw, AppVersion& out);
int CompareVersions(const AppVersion& a, const AppVersion& b);

// Parses GitHub API /releases/latest JSON safely (depth-aware, ignores nested author fields)
bool ParseLatestReleaseJson(const std::string& json,
                            const AppVersion& currentVersion,
                            UpdateReleaseInfo& out);

// Synchronous HTTPS check against GitHub Releases API (call from worker thread)
UpdateReleaseInfo CheckForGitHubUpdate(
    const wchar_t* apiUrl = L"https://api.github.com/repos/Aminomed/Screen2PC/releases/latest");

// Downloads release ZIP to %TEMP%\Screen2PC_Update_<pid>\update.zip, verifies ZIP magic/size,
// writes atomic PowerShell self-updater script, and spawns it waiting for this PID to exit.
// Caller MUST exit promptly after this returns true so the updater can overwrite binaries & relaunch.
bool DownloadAndLaunchSelfUpdate(const UpdateReleaseInfo& info,
                                 bool relaunchMinimizedToTray,
                                 bool checkDriverUpdate,
                                 std::wstring& outError);

} // namespace s2p
