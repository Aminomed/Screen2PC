#pragma once
#include "../common/Protocol.h"
#include "../common/NetSocket.h"
#include "../common/Pairing.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/CodecH264.h"
#include "../common/CodecDelta.h"
#include "../common/ClipboardSync.h"
#include "../common/FileTransfer.h"
#include "DxgiCapture.h"

namespace s2p {

enum class DisplayMode : uint8_t {
    ExtendAll       = 0, // 1 virtual monitor per PC monitor (up to 2)
    ExtendPrimary   = 1, // 1 virtual monitor matching PC monitor 1
    MirrorPrimary   = 2, // Mirror Surface main screen (no VDD needed)
    ExtendSecondary = 3  // 1 virtual monitor matching PC monitor 2 (keeps PC monitor 1 free)
};

using PinPromptCallback = std::function<bool(const std::wstring& pcName,
                                           const std::string& pcIp,
                                           const AppVersionTuple& pcVersion,
                                           uint8_t remainingAttempts,
                                           const std::wstring& message,
                                           std::string& outPin)>;

using RemoteConnectCallback = std::function<void(const std::string& pcIp, const std::wstring& pcName)>;

struct SenderConfig {
    std::string targetIp;
    uint16_t controlPort = kPortControl;
    DisplayMode mode = DisplayMode::ExtendAll;
    CodecType codec = CodecType::H264;
    uint32_t bitrateMbps = 80;
    uint32_t targetFps = 0; // 0 = Auto (Monitor-Hz), 60, 90, 120
};

struct StreamStats {
    uint32_t monitorIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    double mbps = 0.0;
    uint64_t framesSent = 0;
    uint32_t configuredBitrateMbps = 0;
    uint32_t liveBitrateMbps = 0;
    uint32_t targetFps = 0;
    std::wstring codecName;
    bool hardwareAccelerated = false;
    double avgEncodeMs = 0.0;
    double avgSendMs = 0.0;
    bool losslessSettleActive = false;
};

struct SenderStatus {
    bool connected = false;
    bool connecting = false;
    bool driverActive = false;
    bool versionMismatch = false;
    uint16_t remoteProtocolVersion = 0;
    AppVersionTuple remoteAppVersion{};
    bool remoteStopped = false;
    bool authRejected = false;
    bool usingExternalDisplays = false;
    std::wstring peerName;
    std::string peerIp;
    std::wstring stateText;
    std::wstring lastError;
    std::vector<StreamStats> streams;
    FileTransferStats fileStats;
};

class SenderEngine {
public:
    SenderEngine() = default;
    ~SenderEngine();

    SenderEngine(const SenderEngine&) = delete;
    SenderEngine& operator=(const SenderEngine&) = delete;

    static std::vector<DiscoveredPeer> DiscoverPeers(uint32_t timeoutMs = 750);

    void SetConfigIniPath(const std::wstring& iniPath);
    void SetPinPromptCallback(PinPromptCallback cb);
    void SetRemoteConnectCallback(RemoteConnectCallback cb);

    void SetAllowRemoteConnect(bool enabled);
    bool IsAllowRemoteConnect() const { return m_allowRemoteConnect.load(); }
    bool StartRemoteConnectListener();
    void StopRemoteConnectListener();

    std::vector<TrustedDeviceEntry> GetTrustedPcs() const;
    bool RemoveTrustedPcByKey(const std::wstring& key);
    bool ClearAllTrustedPcs();

    bool Connect(const SenderConfig& cfg);
    void SignalStop();
    void Disconnect();

    bool ReconfigureStream(CodecType codec, uint32_t bitrateMbps, uint32_t targetFps);

    SenderStatus GetStatus() const;
    bool IsConnected() const { return m_connected.load(); }
    bool IsUsingExternalDisplays() const { return m_usingExternalDisplays.load(); }
    std::vector<RECT> GetActiveVirtualOutputRects() const;

    void SetClipboardEnabled(bool enabled) { m_clipboardEnabled = enabled; m_clipboard.SetEnabled(enabled); }
    void SetKvmEnabled(bool enabled) { m_kvmEnabled = enabled; }
    size_t SendFiles(const std::vector<std::wstring>& paths) { return m_fileTransfer.QueuePaths(paths); }
    void CancelFileTransfer() { m_fileTransfer.CancelCurrent(); }

private:
    void DisconnectInternal();
    void CleanupSessionResources();
    void SendClipboardData(uint32_t kind, uint64_t hash64, const std::vector<uint8_t>& data);
    struct StreamWorker {
        uint32_t streamIndex = 0;
        uint32_t targetPcMonitor = 0;
        uint32_t adapterIndex = 0;
        uint32_t outputIndex = 0;
        RECT     desktopRect{};
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t refreshHz = 60;
        CodecType codec = CodecType::H264;
        uint32_t bitrateMbps = 80;
        uint32_t targetFps = 60;
        uint32_t sessionStreamCount = 1;
        std::string peerIp;
        uint16_t streamPort = kPortStreamBase;

        std::atomic<uint8_t> desiredCodec{static_cast<uint8_t>(CodecType::H264)};
        std::atomic<uint32_t> desiredBitrateMbps{80};
        std::atomic<uint32_t> desiredTargetFps{0}; // raw preset: 0=Auto, 60, 90, 120
        std::atomic<bool> reconfigPending{false};
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
    void RealtimeUdpLoop();
    void RemoteConnectListenerLoop();
    bool DispatchKvmMouse(const KvmMouseEvent& ev);
    void RunStreamWorker(StreamWorker* w);

    std::mutex m_lifecycleMutex;
    std::mutex m_cleanupMutex;
    mutable std::mutex m_stateMutex;
    SenderStatus m_status;
    SenderConfig m_cfg;

    mutable std::mutex m_cfgIoMutex;
    std::wstring m_configIniPath;
    PinPromptCallback m_pinPromptCb;
    RemoteConnectCallback m_remoteConnectCb;

    mutable std::mutex m_virtualRectsMutex;
    std::vector<RECT> m_virtualOutputRects;
    std::atomic<bool> m_usingExternalDisplays{false};

    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_connecting{false};
    std::atomic<bool> m_disconnecting{false};
    std::atomic<bool> m_remoteStopped{false};
    std::atomic<bool> m_clipboardEnabled{true};
    std::atomic<bool> m_kvmEnabled{true};
    std::atomic<bool> m_allowRemoteConnect{true};
    std::atomic<bool> m_remoteListenerStop{false};
    std::atomic<bool> m_createdVirtualDisplays{false};
    mutable std::atomic<bool> m_cachedDriverActive{false};
    mutable std::atomic<ULONGLONG> m_lastDriverCheckMs{0};

    TcpSocket m_controlSock;
    std::mutex m_controlSendMutex;
    std::thread m_controlThread;
    UdpSocket m_realtimeUdpSock;
    std::thread m_realtimeUdpThread;
    std::mutex m_remoteListenerMutex;
    UdpSocket m_remoteConnectSock;
    std::thread m_remoteConnectThread;
    std::atomic<uint32_t> m_cursorUdpSeq{0};
    std::atomic<uint32_t> m_lastMouseMoveSeq{0};
    std::atomic<uint32_t> m_lastMouseBtnSeq{0};
    ClipboardSync m_clipboard;
    FileTransfer m_fileTransfer;

    mutable std::mutex m_workersMutex;
    std::vector<std::unique_ptr<StreamWorker>> m_workers;
};

} // namespace s2p
