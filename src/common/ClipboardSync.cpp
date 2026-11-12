#include "ClipboardSync.h"
#include "Logger.h"
#include <cstring>

namespace s2p {

namespace {

const wchar_t* kClipWndClass = L"Screen2PCClipListenerWnd";
constexpr UINT WM_S2P_APPLY_CLIP = WM_APP + 50;

struct ApplyRequest {
    uint32_t kind = 0;
    uint64_t hash64 = 0;
    std::vector<uint8_t> data;
};

} // namespace

ClipboardSync::~ClipboardSync() {
    Stop();
}

uint64_t ClipboardSync::ComputeHash64(const void* data, size_t size) {
    // 64-bit FNV-1a
    uint64_t h = 14695981039346656037ULL;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ULL;
    }
    return h;
}

bool ClipboardSync::ShouldEmit(uint64_t newHash, uint64_t lastRemoteHash) {
    return newHash != 0 && newHash != lastRemoteHash;
}

bool ClipboardSync::Start(ChangeCallback onLocalChange) {
    Stop();
    m_callback = std::move(onLocalChange);
    m_running = true;
    m_readyEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_thread = std::thread(&ClipboardSync::ThreadMain, this);

    if (m_readyEvent) {
        WaitForSingleObject(m_readyEvent, 2000);
        CloseHandle(m_readyEvent);
        m_readyEvent = nullptr;
    }
    return m_hwnd != nullptr;
}

void ClipboardSync::Stop() {
    m_running = false;
    if (m_hwnd) {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    } else if (m_threadId != 0) {
        PostThreadMessageW(m_threadId, WM_QUIT, 0, 0);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_hwnd = nullptr;
    m_threadId = 0;
}

void ClipboardSync::ThreadMain() {
    m_threadId = GetCurrentThreadId();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClipWndClass;
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(
        0, kClipWndClass, L"Screen2PC_Clip", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), this);

    if (m_hwnd) {
        AddClipboardFormatListener(m_hwnd);
    }
    if (m_readyEvent) {
        SetEvent(m_readyEvent);
    }

    MSG msg{};
    while (m_running && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (m_hwnd) {
        RemoveClipboardFormatListener(m_hwnd);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

LRESULT CALLBACK ClipboardSync::HiddenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    ClipboardSync* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<ClipboardSync*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<ClipboardSync*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        if (msg == WM_CLIPBOARDUPDATE) {
            self->OnClipboardUpdate();
            return 0;
        }
        if (msg == WM_S2P_APPLY_CLIP) {
            auto* req = reinterpret_cast<ApplyRequest*>(lp);
            if (req) {
                {
                    std::lock_guard<std::mutex> lock(self->m_hashMutex);
                    self->m_lastHash = req->hash64;
                }
                bool opened = false;
                for (int attempt = 0; attempt < 3; ++attempt) {
                    if (OpenClipboard(hwnd)) {
                        opened = true;
                        break;
                    }
                    Sleep(15);
                }
                if (opened) {
                    EmptyClipboard();
                    UINT fmt = (req->kind == kClipKindImage) ? CF_DIB : CF_UNICODETEXT;
                    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, req->data.size());
                    if (hMem) {
                        void* dst = GlobalLock(hMem);
                        if (dst) {
                            std::memcpy(dst, req->data.data(), req->data.size());
                            GlobalUnlock(hMem);
                            if (!SetClipboardData(fmt, hMem)) {
                                GlobalFree(hMem);
                            }
                        } else {
                            GlobalFree(hMem);
                        }
                    }
                    CloseClipboard();
                }
                delete req;
            }
            return 0;
        }
        if (msg == WM_CLOSE) {
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void ClipboardSync::OnClipboardUpdate() {
    if (!m_enabled.load() || !m_callback) return;

    uint32_t kind = 0;
    std::vector<uint8_t> bytes;

    bool opened = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (OpenClipboard(m_hwnd)) {
            opened = true;
            break;
        }
        Sleep(15);
    }
    if (!opened) {
        return;
    }

    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            const void* ptr = GlobalLock(hData);
            size_t sz = GlobalSize(hData);
            if (ptr && sz >= sizeof(wchar_t) && sz <= 2 * 1024 * 1024) {
                const wchar_t* wptr = static_cast<const wchar_t*>(ptr);
                size_t maxChars = sz / sizeof(wchar_t);
                size_t len = wcsnlen(wptr, maxChars);
                size_t copyBytes = (len + 1) * sizeof(wchar_t);
                bytes.resize(copyBytes);
                std::memcpy(bytes.data(), ptr, len * sizeof(wchar_t));
                bytes[copyBytes - 2] = 0;
                bytes[copyBytes - 1] = 0;
                kind = kClipKindText;
            }
            if (ptr) GlobalUnlock(hData);
        }
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        HANDLE hData = GetClipboardData(CF_DIB);
        if (hData) {
            const void* ptr = GlobalLock(hData);
            size_t sz = GlobalSize(hData);
            if (ptr && sz >= sizeof(BITMAPINFOHEADER) && sz <= kMaxClipboardBytes) {
                bytes.resize(sz);
                std::memcpy(bytes.data(), ptr, sz);
                kind = kClipKindImage;
            }
            if (ptr) GlobalUnlock(hData);
        }
    }

    CloseClipboard();

    if (kind == 0 || bytes.empty()) return;

    uint64_t hash = ComputeHash64(bytes.data(), bytes.size());
    {
        std::lock_guard<std::mutex> lock(m_hashMutex);
        if (!ShouldEmit(hash, m_lastHash)) {
            return;
        }
        m_lastHash = hash;
    }

    m_callback(kind, hash, bytes);
}

bool ClipboardSync::ApplyRemote(uint32_t kind, uint64_t hash64, const uint8_t* data, size_t size) {
    if (!m_enabled.load() || !m_hwnd || !data || size == 0 || size > kMaxClipboardBytes) {
        return false;
    }
    if (kind != kClipKindText && kind != kClipKindImage) {
        return false;
    }

    auto* req = new ApplyRequest();
    req->kind = kind;
    req->data.assign(data, data + size);

    // Ensure null termination on text
    if (kind == kClipKindText) {
        if (req->data.size() < 2 || req->data[req->data.size() - 2] != 0 || req->data[req->data.size() - 1] != 0) {
            req->data.push_back(0);
            req->data.push_back(0);
        }
        req->hash64 = ComputeHash64(req->data.data(), req->data.size());
    } else {
        req->hash64 = hash64 ? hash64 : ComputeHash64(req->data.data(), req->data.size());
    }

    if (!PostMessageW(m_hwnd, WM_S2P_APPLY_CLIP, 0, reinterpret_cast<LPARAM>(req))) {
        delete req;
        return false;
    }
    return true;
}

} // namespace s2p
