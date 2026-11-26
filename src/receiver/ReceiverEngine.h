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
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/ClipboardSync.h"
#include "../common/FileTransfer.h"
#include "../common/ColorProfile.h"

#include <functional>

namespace s2p {

class D3D11Renderer;

struct PinPopupRequest {
    std::string pinCode;
    std::wstring senderName;
    std::string senderIp;
    uint8_t senderMac[6]{};
    uint8_t senderDeviceId[16]{};
    AppVersionTuple senderVersion{};
};

struct PinPopupSession {
    PinPopupRequest req;
    std::atomic<bool> remember{ true }; // Checked by default on PC popup
    std::atomic<bool> rejected{ false };
};

struct ReceiverStreamStats {
    uint32_t monitorIndex = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;
    double mbps = 0.0;
    uint64_t framesReceived = 0;
    std::wstring codecName;
    bool hardwareDxva = false;
    bool hiddenByUser = false;
    double avgDecodeMs = 0.0;
    bool directNv12Active = false;
};

struct ReceiverStatus {
    bool running = false;
    bool sessionActive = false;
    bool hasHiddenStreams = false;
    std::wstring senderName;
    std::string senderIp;
    AppVersionTuple senderVersion{};
    std::wstring stateText;
    std::vector<MonitorDesc> localMonitors;
    std::vector<MonitorHdrInfo> localMonitorHdr;
    std::vector<std::string> localIps;
    std::vector<ReceiverStreamStats> streams;
    FileTransferStats fileStats;
};

struct RemoteLaptopProbeResult {
    std::string ip;
    std::wstring laptopName;
    std::wstring macStr;
    std::wstring deviceIdHex;
    bool allowRemoteConnect = false;
    bool isConnected = false;
    bool isTrusted = false;
    AppVersionTuple version{};
};

struct RemoteConnectTriggerResult {
    bool responded = false;
    bool accepted = false;
    RemoteConnectStatus status = kRemoteConnectDisabled;
    std::string laptopIp;
    std::wstring laptopName;
    std::wstring message;
};

class ReceiverEngine {
public:
    using PinShowCallback = std::function<void(std::shared_ptr<PinPopupSession>)>;
    using PinDismissCallback = std::function<void()>;
    using VersionMismatchCallback = std::function<void(uint16_t remoteProtoVer, const AppVersionTuple& remoteAppVer, const std::wstring& message)>;

    ReceiverEngine() = default;
    ~ReceiverEngine();

    ReceiverEngine(const ReceiverEngine&) = delete;
    ReceiverEngine& operator=(const ReceiverEngine&) = delete;

    static std::vector<MonitorDesc> EnumerateLocalMonitors();
    static std::vector<MonitorDesc> EnumerateLocalMonitorsWithHdr(std::vector<MonitorHdrInfo>& outHdr);

    void SetConfigIniPath(const std::wstring& iniPath);
    void SetPinCallbacks(PinShowCallback onShow, PinDismissCallback onDismiss);
    void SetVersionMismatchCallback(VersionMismatchCallback cb);

    std::vector<TrustedDeviceEntry> GetTrustedDevices() const;
    bool RemoveTrustedDeviceByKey(const std::wstring& key);
    bool ClearAllTrustedDevices();

    std::vector<RemoteLaptopProbeResult> ProbeRemoteLaptops(uint32_t timeoutMs = 650) const;
    RemoteConnectTriggerResult RequestLaptopConnect(const TrustedDeviceEntry& laptop, uint32_t timeoutMs = 1500) const;

    bool Start(bool startFullscreen = true);
    void Stop();
    void DisconnectSession();
    void RefreshLocalMonitorInfo();
    void SetStartFullscreen(bool fs) { m_startFullscreen = fs; }
    void SetClipboardEnabled(bool enabled) { m_clipboardEnabled = enabled; m_clipboard.SetEnabled(enabled); }
    void SetKvmEnabled(bool enabled) { m_kvmEnabled = enabled; }
    bool IsKvmEnabled() const { return m_kvmEnabled.load(); }
    void SetStreamHudEnabled(bool enabled);
    bool IsStreamHudEnabled() const { return m_streamHudEnabled.load(); }
    void SetKvmMouseSpeedPct(uint32_t pct);
    uint32_t GetKvmMouseSpeedPct() const { return m_kvmMouseSpeedPct.load(); }
    void SetKvmScrollSpeedPct(uint32_t pct);
    uint32_t GetKvmScrollSpeedPct() const { return m_kvmScrollSpeedPct.load(); }
    void SetMonitorColorConfig(uint32_t monitorIndex, const MonitorColorConfig& cfg);
    MonitorColorConfig GetMonitorColorConfig(uint32_t monitorIndex) const;
    size_t SendFiles(const std::vector<std::wstring>& paths) { return m_fileTransfer.QueuePaths(paths); }
    void CancelFileTransfer() { m_fileTransfer.CancelCurrent(); }

    void ShowHiddenStreamWindows();
    bool HasHiddenStreamWindows() const;

    ReceiverStatus GetStatus() const;

private:
    void SendClipboardData(uint32_t kind, uint64_t hash64, const std::vector<uint8_t>& data);
    void SendKvmMouse(const KvmMouseEvent& ev);
    void SendKvmKey(const KvmKeyEvent& ev);
    struct StreamReceiverWorker {
        uint32_t monitorIndex = 0;
        uint32_t targetMonitorIndex = 0;
        uint16_t port = kPortStreamBase;
        std::string expectedPeerIp;
        RECT targetMonitorRect{};
        uint32_t expectedWidth = 1920;
        uint32_t expectedHeight = 1080;
        uint32_t targetFps = 60;
        bool startFullscreen = true;

        std::atomic<bool> stop{false};
        std::atomic<bool> hiddenByUser{false};
        std::atomic<bool> showRequested{false};
        std::atomic<uint32_t> desiredTargetFps{60};
        std::mutex rendererPtrMutex;
        D3D11Renderer* activeRenderer = nullptr;
        std::thread thread;
        TcpSocket listenSock;
        UdpSocket videoUdpSock;

        std::mutex statsMutex;
        ReceiverStreamStats stats;
    };

    void DiscoveryLoop();
    void RealtimeUdpLoop();
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
    std::atomic<bool> m_kvmEnabled{true};
    std::atomic<bool> m_streamHudEnabled{true};
    std::atomic<uint32_t> m_kvmMouseSpeedPct{150};
    std::atomic<uint32_t> m_kvmScrollSpeedPct{200};

    mutable std::mutex m_cfgMutex;
    std::wstring m_configIniPath;
    PinShowCallback m_onPinShow;
    PinDismissCallback m_onPinDismiss;
    VersionMismatchCallback m_onVersionMismatch;

    mutable std::mutex m_colorMutex;
    MonitorColorConfig m_colorConfigs[kMaxMonitors]{};
    std::atomic<uint64_t> m_colorGeneration[kMaxMonitors]{};

    UdpSocket m_discoverySock;
    UdpSocket m_realtimeUdpSock;
    TcpSocket m_controlListenSock;
    std::thread m_discoveryThread;
    std::thread m_realtimeUdpThread;
    std::thread m_controlThread;

    std::mutex m_peerIpMutex;
    std::string m_activePeerIp;
    std::atomic<uint32_t> m_mouseUdpSeq{0};
    std::atomic<uint32_t> m_mouseBtnSeq{0};
    std::atomic<uint32_t> m_lastCursorUdpSeq{0};

    std::mutex m_ctrlSendMutex;
    TcpSocket m_activeCtrlSock;

    ClipboardSync m_clipboard;
    TcpSocket m_fileListenSock;
    std::thread m_fileAcceptThread;
    FileTransfer m_fileTransfer;

    std::mutex m_disconnectMutex;
    mutable std::mutex m_stateMutex;
    ReceiverStatus m_status;
    mutable std::mutex m_workersMutex;
    std::vector<std::unique_ptr<StreamReceiverWorker>> m_workers;
};

} // namespace s2p
