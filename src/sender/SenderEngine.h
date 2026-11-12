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
#include "../common/CodecH264.h"
#include "../common/CodecDelta.h"
#include "../common/ClipboardSync.h"
#include "DxgiCapture.h"

namespace s2p {

enum class DisplayMode : uint8_t {
    ExtendAll       = 0, // 1 virtual monitor per PC monitor (up to 2)
    ExtendPrimary   = 1, // 1 virtual monitor matching PC monitor 1
    MirrorPrimary   = 2, // Mirror Surface main screen (no VDD needed)
    ExtendSecondary = 3  // 1 virtual monitor matching PC monitor 2 (keeps PC monitor 1 free)
};

struct SenderConfig {
    std::string targetIp;
    uint16_t controlPort = kPortControl;
    DisplayMode mode = DisplayMode::ExtendAll;
    CodecType codec = CodecType::H264;
    uint32_t bitrateMbps = 25;
    uint32_t targetFps = 60;
};

struct StreamStats {
    uint32_t monitorIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    double mbps = 0.0;
    uint64_t framesSent = 0;
    std::wstring codecName;
    bool hardwareAccelerated = false;
};

struct SenderStatus {
    bool connected = false;
    bool connecting = false;
    bool driverActive = false;
    std::wstring peerName;
    std::string peerIp;
    std::wstring stateText;
    std::wstring lastError;
    std::vector<StreamStats> streams;
};

class SenderEngine {
public:
    SenderEngine() = default;
    ~SenderEngine();

    SenderEngine(const SenderEngine&) = delete;
    SenderEngine& operator=(const SenderEngine&) = delete;

    static std::vector<DiscoveredPeer> DiscoverPeers(uint32_t timeoutMs = 750);

    bool Connect(const SenderConfig& cfg);
    void Disconnect();

    SenderStatus GetStatus() const;
    bool IsConnected() const { return m_connected.load(); }
    void SetClipboardEnabled(bool enabled) { m_clipboardEnabled = enabled; m_clipboard.SetEnabled(enabled); }

private:
    void SendClipboardData(uint32_t kind, uint64_t hash64, const std::vector<uint8_t>& data);
    struct StreamWorker {
        uint32_t targetPcMonitor = 0;
        uint32_t adapterIndex = 0;
        uint32_t outputIndex = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t refreshHz = 60;
        CodecType codec = CodecType::H264;
        uint32_t bitrateMbps = 25;
        uint32_t targetFps = 60;
        std::string peerIp;
        uint16_t streamPort = kPortStreamBase;

        std::atomic<bool> forceKeyframe{true};
        std::atomic<bool> stop{false};
        std::thread thread;

        // Live telemetry updated by worker
        std::mutex statsMutex;
        StreamStats stats;
    };

    bool SetupVirtualDisplays(const HelloReceiverPayload& hello,
                              const SenderConfig& cfg,
                              std::vector<DisplayOutputInfo>& outSelectedOutputs,
                              std::wstring& outError);
    void ControlLoop();
    void RunStreamWorker(StreamWorker* w);

    mutable std::mutex m_stateMutex;
    SenderStatus m_status;
    SenderConfig m_cfg;

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_clipboardEnabled{true};
    bool m_createdVirtualDisplays = false;

    TcpSocket m_controlSock;
    std::mutex m_controlSendMutex;
    std::thread m_controlThread;
    ClipboardSync m_clipboard;
    std::vector<std::unique_ptr<StreamWorker>> m_workers;
};

} // namespace s2p
