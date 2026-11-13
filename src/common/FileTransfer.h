#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "NetSocket.h"

namespace s2p {

constexpr uint32_t kFileChunkSize = 64 * 1024; // 64 KB per TCP frame
constexpr uint64_t kMaxFileTransferBytes = 4ULL * 1024ULL * 1024ULL * 1024ULL; // 4 GB

struct FileTransferStats {
    uint32_t filesSent = 0;
    uint32_t filesReceived = 0;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    bool active = false;
    std::wstring lastEvent;
    std::wstring downloadDir;
};

class FileTransfer {
public:
    FileTransfer() = default;
    ~FileTransfer();

    FileTransfer(const FileTransfer&) = delete;
    FileTransfer& operator=(const FileTransfer&) = delete;

    // Starts bidirectional send/recv workers over an already-connected TCP socket.
    // Optionally override download directory (defaults to %USERPROFILE%\Downloads\Screen2PC).
    void Start(TcpSocket sock, const std::wstring& customDownloadDir = L"");
    void Stop();

    bool IsConnected() const { return m_connected.load(); }

    // Non-blocking: queues local files to send to the peer. Ignores directories / missing files.
    size_t QueueFiles(const std::vector<std::wstring>& paths);

    FileTransferStats GetStats() const;

    // Pure helpers (unit-testable)
    static std::wstring DefaultDownloadDir();
    static std::wstring SanitizeFilename(const std::wstring& rawName);
    static std::wstring ResolveUniqueDestPath(const std::wstring& dir, const std::wstring& safeFilename);
    static uint32_t ComputeCrc32(const void* data, size_t size, uint32_t seed = 0);

private:
    void SendWorker();
    void RecvWorker();
    bool SendSingleFile(const std::wstring& fullPath);

    std::atomic<bool> m_stop{ false };
    std::atomic<bool> m_connected{ false };
    std::atomic<uint32_t> m_nextId{ 1 };

    TcpSocket m_sock;
    std::wstring m_downloadDir;

    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<std::wstring> m_sendQueue;

    mutable std::mutex m_statsMutex;
    FileTransferStats m_stats;

    std::mutex m_lifecycleMutex;
    std::thread m_sendThread;
    std::thread m_recvThread;
};

} // namespace s2p
