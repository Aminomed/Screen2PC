#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace s2p {

constexpr uint32_t kClipKindText  = 1; // UTF-16LE null-terminated text
constexpr uint32_t kClipKindImage = 2; // CF_DIB packed bitmap
constexpr size_t   kMaxClipboardBytes = 16 * 1024 * 1024; // 16 MB cap

class ClipboardSync {
public:
    using ChangeCallback = std::function<void(uint32_t kind, uint64_t hash64, const std::vector<uint8_t>& data)>;

    ClipboardSync() = default;
    ~ClipboardSync();

    ClipboardSync(const ClipboardSync&) = delete;
    ClipboardSync& operator=(const ClipboardSync&) = delete;

    bool Start(ChangeCallback onLocalChange);
    void Stop();

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    bool IsEnabled() const { return m_enabled.load(); }

    // Writes data from remote peer into local Windows clipboard without echoing back.
    bool ApplyRemote(uint32_t kind, uint64_t hash64, const uint8_t* data, size_t size);

    // Pure helpers (unit-testable without interactive desktop)
    static uint64_t ComputeHash64(const void* data, size_t size);
    static bool ShouldEmit(uint64_t newHash, uint64_t lastRemoteHash);

private:
    void ThreadMain();
    void OnClipboardUpdate();
    static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    ChangeCallback m_callback;
    std::atomic<bool> m_enabled{ true };
    std::atomic<bool> m_running{ false };

    std::mutex m_hashMutex;
    uint64_t m_lastHash = 0;

    std::thread m_thread;
    DWORD m_threadId = 0;
    HWND m_hwnd = nullptr;
    HANDLE m_readyEvent = nullptr;
};

} // namespace s2p
