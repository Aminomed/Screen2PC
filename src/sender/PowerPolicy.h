#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace s2p {

struct PowerRestoreState {
    bool pending = false;
    GUID schemeGuid{};
    uint32_t savedAc = 1;
    uint32_t savedDc = 1;
};

class LidCloseGuard {
public:
    LidCloseGuard() = default;
    ~LidCloseGuard();

    LidCloseGuard(const LidCloseGuard&) = delete;
    LidCloseGuard& operator=(const LidCloseGuard&) = delete;

    // Restores any leftover lid-close setting saved before an unexpected crash.
    static bool RecoverStaleStateOnStartup(const std::wstring& iniPath);

    // Engages lid-close sleep bypass (sets AC/DC lid action to 0 = Do Nothing)
    // and keeps ES_SYSTEM_REQUIRED active until Restore() is called.
    bool Engage(const std::wstring& iniPath);
    bool Restore(const std::wstring& iniPath);
    bool IsEngaged() const;

    // Pure helpers (unit-testable without mutating system power policy)
    static std::wstring FormatGuidString(const GUID& g);
    static bool ParseGuidString(const std::wstring& s, GUID& out);
    static bool SavePowerRestoreIni(const std::wstring& iniPath, const PowerRestoreState& st);
    static bool LoadPowerRestoreIni(const std::wstring& iniPath, PowerRestoreState& out);
    static void ClearPowerRestoreIni(const std::wstring& iniPath);

private:
    static bool ApplyPowerValues(const GUID& schemeGuid, uint32_t acVal, uint32_t dcVal);

    mutable std::mutex m_mutex;
    bool m_engaged = false;
    std::wstring m_activeIniPath;
    PowerRestoreState m_saved{};
};

// Creates borderless black topmost toolwindows ONLY on physical Surface monitors
// (never on Screen2PC virtual monitors) to save battery/reduce glare while keeping
// external PC monitors active. Dismissible on ESC or double-click.
class SurfaceDimOverlay {
public:
    using DismissCallback = std::function<void()>;

    SurfaceDimOverlay() = default;
    ~SurfaceDimOverlay();

    SurfaceDimOverlay(const SurfaceDimOverlay&) = delete;
    SurfaceDimOverlay& operator=(const SurfaceDimOverlay&) = delete;

    // Must be called on the main UI thread so overlay windows pump messages normally.
    bool Show(const std::vector<RECT>& virtualExcludeRects, DismissCallback onDismiss);
    void Hide(bool invokeCallback = false);
    bool IsVisible() const;

    LRESULT HandleWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    std::vector<HWND> m_hwnds;
    DismissCallback m_onDismiss;
    bool m_dismissing = false;
};

} // namespace s2p
