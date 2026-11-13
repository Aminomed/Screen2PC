#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/Protocol.h"
#include "../common/NetSocket.h"
#include "../common/ClipboardSync.h"
#include "../common/FileTransfer.h"

namespace s2p {

struct ReceiverStreamStats {
    uint32_t monitorIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    double mbps = 0.0;
    uint64_t framesReceived = 0;
    std::wstring codecName;
    bool hardwareDxva = false;
};

struct ReceiverStatus {
    bool running = false;
    bool sessionActive = false;
    std::wstring senderName;
    std::string senderIp;
    std::wstring stateText;
    std::vector<MonitorDesc> localMonitors;
    std::vector<std::string> localIps;
    std::vector<ReceiverStreamStats> streams;
    FileTransferStats fileStats;
};

class ReceiverEngine {
public:
    ReceiverEngine() = default;
    ~ReceiverEngine();

    ReceiverEngine(const ReceiverEngine&) = delete;
    ReceiverEngine& operator=(const ReceiverEngine&) = delete;

    static std::vector<MonitorDesc> EnumerateLocalMonitors();

    bool Start(bool startFullscreen = true);
    void Stop();
    void DisconnectSession();
    void SetStartFullscreen(bool fs) { m_startFullscreen = fs; }
    void SetClipboardEnabled(bool enabled) { m_clipboardEnabled = enabled; m_clipboard.SetEnabled(enabled); }
    size_t SendFiles(const std::vector<std::wstring>& paths) { return m_fileTransfer.QueueFiles(paths); }

    ReceiverStatus GetStatus() const;

private:
    void SendClipboardData(uint32_t kind, uint64_t hash64, const std::vector<uint8_t>& data);
    struct StreamReceiverWorker {
        uint32_t monitorIndex = 0;
        uint16_t port = kPortStreamBase;
        RECT targetMonitorRect{};
        uint32_t expectedWidth = 1920;
        uint32_t expectedHeight = 1080;
        bool startFullscreen = true;

        std::atomic<bool> stop{false};
        std::thread thread;
        TcpSocket listenSock;

        std::mutex statsMutex;
        ReceiverStreamStats stats;
    };

    void DiscoveryLoop();
    void ControlAcceptLoop();
    void HandleControlClient(TcpSocket clientSock, const std::string& peerIp);
    void RunStreamWorker(StreamReceiverWorker* w);
    void RequestKeyframe(uint32_t monitorIndex);
    void StopStreamWorkers();

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_sessionStop{false};
    std::atomic<bool> m_startFullscreen{true};
    std::atomic<bool> m_clipboardEnabled{true};

    UdpSocket m_discoverySock;
    TcpSocket m_controlListenSock;
    std::thread m_discoveryThread;
    std::thread m_controlThread;

    std::mutex m_ctrlSendMutex;
    TcpSocket m_activeCtrlSock;

    ClipboardSync m_clipboard;
    TcpSocket m_fileListenSock;
    std::thread m_fileAcceptThread;
    FileTransfer m_fileTransfer;

    mutable std::mutex m_stateMutex;
    ReceiverStatus m_status;
    std::vector<std::unique_ptr<StreamReceiverWorker>> m_workers;
};

} // namespace s2p
