#include "SenderEngine.h"
#include "VirtualDisplayCtl.h"
#include "../common/BitratePresets.h"
#include "../common/KvmInput.h"
#include "../common/Logger.h"

#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <set>

namespace s2p {

namespace {

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) {
            CoUninitialize();
        }
    }
};

uint64_t NowUs() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void CopyWideSafe(wchar_t* dst, size_t dstChars, const std::wstring& src) {
    if (!dst || dstChars == 0) return;
    wcsncpy_s(dst, dstChars, src.c_str(), _TRUNCATE);
}

void SleepInterruptible(DWORD totalMs, const std::atomic<bool>& stop) {
    DWORD elapsed = 0;
    while (elapsed < totalMs && !stop.load()) {
        DWORD chunk = std::min<DWORD>(25, totalMs - elapsed);
        Sleep(chunk);
        elapsed += chunk;
    }
}

bool WaitForControlPacketRaw(TcpSocket& sock,
                             PacketHeader& hdr,
                             std::vector<uint8_t>& payload,
                             uint16_t& outRemoteVersion,
                             bool& outVersionMismatch,
                             bool& outClosedCleanly,
                             int maxWaitMs,
                             const std::atomic<bool>& stop) {
    outRemoteVersion = 0;
    outVersionMismatch = false;
    outClosedCleanly = false;
    int waited = 0;
    while (waited < maxWaitMs && !stop.load() && sock.IsValid()) {
        int step = std::min(100, maxWaitMs - waited);
        int wr = sock.WaitReadable(step);
        if (wr < 0) {
            return false;
        }
        if (wr > 0) {
            WSASetLastError(0);
            bool ok = sock.RecvPacketRaw(hdr, payload, outRemoteVersion, outVersionMismatch);
            if (!ok && !outVersionMismatch) {
                int err = WSAGetLastError();
                if (err == 0 || err == WSAECONNRESET) {
                    outClosedCleanly = true;
                }
            }
            return ok;
        }
        waited += step;
    }
    return false;
}

} // namespace

SenderEngine::~SenderEngine() {
    Disconnect();
}

void SenderEngine::SetConfigIniPath(const std::wstring& iniPath) {
    std::lock_guard<std::mutex> lock(m_cfgIoMutex);
    m_configIniPath = iniPath;
}

void SenderEngine::SetPinPromptCallback(PinPromptCallback cb) {
    std::lock_guard<std::mutex> lock(m_cfgIoMutex);
    m_pinPromptCb = std::move(cb);
}

std::vector<RECT> SenderEngine::GetActiveVirtualOutputRects() const {
    std::lock_guard<std::mutex> lock(m_virtualRectsMutex);
    return m_virtualOutputRects;
}

std::vector<DiscoveredPeer> SenderEngine::DiscoverPeers(uint32_t timeoutMs) {
    std::vector<DiscoveredPeer> peers;
    UdpSocket udp;
    if (!udp.CreateForBroadcast()) {
        return peers;
    }

    const uint32_t nonceV2 = static_cast<uint32_t>(GetTickCount64() & 0xFFFFFFFFu);
    const uint32_t nonceV1 = nonceV2 ^ 0xA5A55A5Au;
    const std::wstring hostName = GetHostComputerName();

    struct {
        PacketHeader hdr;
        DiscoverReqPayload body;
    } pktV2{};
    pktV2.hdr = MakeHeader(MsgType::DiscoverReq, sizeof(DiscoverReqPayload));
    pktV2.body.senderNonce = nonceV2;
    CopyWideSafe(pktV2.body.senderName, _countof(pktV2.body.senderName), hostName);

    struct {
        PacketHeader hdr;
        DiscoverReqPayload body;
    } pktV1{};
    pktV1.hdr = MakeHeader(MsgType::DiscoverReq, sizeof(DiscoverReqPayload));
    pktV1.hdr.version = 1;
    pktV1.body.senderNonce = nonceV1;
    CopyWideSafe(pktV1.body.senderName, _countof(pktV1.body.senderName), hostName);

    udp.Broadcast(kPortDiscovery, &pktV2, sizeof(pktV2));
    udp.Broadcast(kPortDiscovery, &pktV1, sizeof(pktV1));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::map<std::string, DiscoveredPeer> byIp;
    std::vector<std::string> order;
    auto upsertPeer = [&](const std::string& ip, DiscoveredPeer&& candidate, bool overwriteExisting) {
        auto it = byIp.find(ip);
        if (it == byIp.end()) {
            order.push_back(ip);
            byIp.emplace(ip, std::move(candidate));
        } else if (overwriteExisting || (it->second.versionMismatch && !candidate.versionMismatch)) {
            it->second = std::move(candidate);
        }
    };

    uint8_t buf[4096]{};
    const size_t minLegacyRespBytes = sizeof(PacketHeader) + sizeof(DiscoverRespPayload) - sizeof(AppVersionTuple);

    while (std::chrono::steady_clock::now() < deadline) {
        auto remMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
        if (remMs <= 0) break;

        std::string senderIp;
        uint16_t senderPort = 0;
        int n = udp.RecvFrom(buf, sizeof(buf), senderIp, senderPort, std::min(remMs, 150));
        if (n <= 0 || static_cast<size_t>(n) < sizeof(PacketHeader)) continue;

        PacketHeader hdr{};
        std::memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != kProtocolMagic || hdr.payloadSize > kMaxFrameBytes) continue;

        if (hdr.type == static_cast<uint16_t>(MsgType::VersionMismatch) &&
            static_cast<size_t>(n) >= sizeof(PacketHeader) + sizeof(VersionMismatchPayload)) {
            VersionMismatchPayload vm{};
            std::memcpy(&vm, buf + sizeof(PacketHeader), sizeof(vm));
            vm.message[_countof(vm.message) - 1] = L'\0';
            if (byIp.find(senderIp) == byIp.end()) {
                DiscoveredPeer peer;
                peer.ip = senderIp;
                peer.pcName = vm.message[0] ? std::wstring(vm.message) : (L"PC (" + Utf8ToWide(senderIp) + L")");
                peer.versionMismatch = true;
                peer.remoteProtocolVersion = vm.protocolVersion ? vm.protocolVersion : hdr.version;
                peer.receiverVersion = vm.appVersion;
                upsertPeer(senderIp, std::move(peer), false);
            }
            continue;
        }

        if (hdr.type != static_cast<uint16_t>(MsgType::DiscoverResp) ||
            static_cast<size_t>(n) < minLegacyRespBytes) {
            continue;
        }

        DiscoverRespPayload resp{};
        size_t copyBytes = std::min<size_t>(static_cast<size_t>(n) - sizeof(PacketHeader), sizeof(DiscoverRespPayload));
        std::memcpy(&resp, buf + sizeof(PacketHeader), copyBytes);
        resp.pcName[_countof(resp.pcName) - 1] = L'\0';

        if (hdr.version == kProtocolVersion) {
            if (resp.senderNonce != nonceV2) continue;

            DiscoveredPeer peer;
            peer.ip = senderIp;
            peer.pcName = resp.pcName;
            peer.controlPort = resp.controlPort ? resp.controlPort : kPortControl;
            peer.streamBasePort = resp.streamBasePort ? resp.streamBasePort : kPortStreamBase;
            peer.receiverVersion = resp.receiverVersion;
            peer.remoteProtocolVersion = kProtocolVersion;
            peer.versionMismatch = false;

            uint32_t cnt = std::min<uint32_t>(resp.monitorCount, static_cast<uint32_t>(kMaxMonitors));
            for (uint32_t i = 0; i < cnt; ++i) {
                resp.monitors[i].friendlyName[_countof(resp.monitors[i].friendlyName) - 1] = L'\0';
                peer.monitors.push_back(resp.monitors[i]);
            }
            upsertPeer(senderIp, std::move(peer), true);
        } else {
            if (resp.senderNonce != nonceV1 && resp.senderNonce != nonceV2) continue;

            DiscoveredPeer peer;
            peer.ip = senderIp;
            peer.pcName = resp.pcName[0] ? std::wstring(resp.pcName) : Utf8ToWide(senderIp);
            peer.controlPort = resp.controlPort ? resp.controlPort : kPortControl;
            peer.streamBasePort = resp.streamBasePort ? resp.streamBasePort : kPortStreamBase;
            peer.receiverVersion = resp.receiverVersion;
            peer.remoteProtocolVersion = hdr.version;
            peer.versionMismatch = true;

            uint32_t cnt = std::min<uint32_t>(resp.monitorCount, static_cast<uint32_t>(kMaxMonitors));
            for (uint32_t i = 0; i < cnt; ++i) {
                resp.monitors[i].friendlyName[_countof(resp.monitors[i].friendlyName) - 1] = L'\0';
                peer.monitors.push_back(resp.monitors[i]);
            }
            upsertPeer(senderIp, std::move(peer), false);
        }
    }

    peers.reserve(order.size());
    for (const auto& ip : order) {
        auto it = byIp.find(ip);
        if (it != byIp.end()) {
            peers.push_back(std::move(it->second));
        }
    }
    return peers;
}

bool SenderEngine::Connect(const SenderConfig& cfg) {
    std::lock_guard<std::mutex> lifeLock(m_lifecycleMutex);
    DisconnectInternal();

    {
        bool active = VirtualDisplayCtl::IsDriverActive();
        m_cachedDriverActive.store(active);
        m_lastDriverCheckMs.store(GetTickCount64());
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_cfg = cfg;
        m_status = SenderStatus{};
        m_status.connecting = true;
        m_status.driverActive = active;
        m_status.peerIp = cfg.targetIp;
        m_status.stateText = L"Verbinde mit PC...";
    }

    {
        std::lock_guard<std::mutex> vl(m_virtualRectsMutex);
        m_virtualOutputRects.clear();
    }
    m_usingExternalDisplays = false;
    m_remoteStopped = false;
    m_stop = false;
    m_connected = false;
    m_connecting = true;
    m_createdVirtualDisplays = false;

    if (!m_controlSock.Connect(cfg.targetIp, cfg.controlPort, 3000, &m_stop)) {
        m_connecting = false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.stateText = L"Verbindung fehlgeschlagen";
        m_status.lastError = L"TCP-Verbindung zum PC (" + Utf8ToWide(cfg.targetIp) + L":" +
                             std::to_wstring(cfg.controlPort) + L") fehlgeschlagen.";
        Logger::Instance().Error("SND-CONN", m_status.lastError);
        return false;
    }

    m_controlSock.TuneLowLatency(256 * 1024, 256 * 1024);
    m_controlSock.SetTimeouts(3000, 5000);

    std::wstring iniPath;
    {
        std::lock_guard<std::mutex> cl(m_cfgIoMutex);
        iniPath = m_configIniPath;
    }

    HelloSenderPayload helloReq{};
    CopyWideSafe(helloReq.senderName, _countof(helloReq.senderName), GetHostComputerName());
    helloReq.preferredCodec = static_cast<uint8_t>(cfg.codec);
    helloReq.targetBitrateMbps = cfg.bitrateMbps;
    helloReq.targetFps = cfg.targetFps; // 0 = Auto (Monitor-Hz)
    GetMacForTargetIp(cfg.targetIp, helloReq.senderMac);
    if (!iniPath.empty()) {
        LoadOrCreateLocalDeviceId(iniPath, helloReq.senderDeviceId);
    }
    helloReq.senderVersion = CurrentAppVersionTuple();

    if (!m_controlSock.SendPacket(MsgType::HelloSender, &helloReq, sizeof(helloReq))) {
        m_connecting = false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.lastError = L"Handshake senden fehlgeschlagen.";
        m_controlSock.Close();
        return false;
    }

    PacketHeader hdr{};
    std::vector<uint8_t> payload;
    uint16_t remoteProtoVer = 0;
    bool rawVerMismatch = false;
    bool closedCleanly = false;

    auto setVersionMismatchError = [&](uint16_t peerProtoVer, const AppVersionTuple& peerAppVer, const std::wstring& extraMsg) {
        m_connecting = false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.versionMismatch = true;
        m_status.remoteProtocolVersion = peerProtoVer;
        m_status.remoteAppVersion = peerAppVer;
        m_status.stateText = L"Versionskonflikt mit PC";

        std::wstring msg = L"Versionskonflikt: Surface nutzt Protokoll v" + std::to_wstring(kProtocolVersion) +
                           L" (App v" + FormatAppVersion(CurrentAppVersion()) + L")";
        if (peerProtoVer > 0) {
            msg += L", PC meldet Protokoll v" + std::to_wstring(peerProtoVer);
            if (peerAppVer.major > 0 || peerAppVer.minor > 0) {
                msg += L" (App v" + FormatVersionTuple(peerAppVer) + L")";
            }
        } else {
            msg += L", PC verwendet vermutlich eine aeltere inkompatible Version (v1)";
        }
        if (!extraMsg.empty()) {
            msg += L" - " + extraMsg;
        } else {
            msg += L". Bitte beide Geraete ueber den integrierten GitHub-Updater aktualisieren.";
        }
        m_status.lastError = msg;
        Logger::Instance().Warn("SND-VER", msg);
        m_controlSock.Close();
    };

    if (!WaitForControlPacketRaw(m_controlSock, hdr, payload, remoteProtoVer, rawVerMismatch, closedCleanly, 5000, m_stop)) {
        if (rawVerMismatch) {
            setVersionMismatchError(remoteProtoVer, AppVersionTuple{}, L"");
            return false;
        }
        if (closedCleanly && !m_stop) {
            setVersionMismatchError(1, AppVersionTuple{},
                                    L"Verbindung wurde direkt nach Handshake geschlossen (aelterer Receiver v1?). Bitte beide Apps aktualisieren.");
            return false;
        }
        m_connecting = false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.lastError = L"Ungueltige oder fehlende Handshake-Antwort vom PC.";
        m_controlSock.Close();
        return false;
    }

    if (hdr.type == static_cast<uint16_t>(MsgType::VersionMismatch)) {
        VersionMismatchPayload vm{};
        if (payload.size() >= sizeof(vm)) {
            std::memcpy(&vm, payload.data(), sizeof(vm));
            vm.message[_countof(vm.message) - 1] = L'\0';
        }
        setVersionMismatchError(vm.protocolVersion ? vm.protocolVersion : remoteProtoVer,
                                vm.appVersion,
                                vm.message[0] ? std::wstring(vm.message) : L"");
        return false;
    }

    if (hdr.type == static_cast<uint16_t>(MsgType::PairChallenge)) {
        PairChallengePayload ch{};
        if (payload.size() >= sizeof(ch)) {
            std::memcpy(&ch, payload.data(), sizeof(ch));
            ch.pcName[_countof(ch.pcName) - 1] = L'\0';
        }
        std::wstring pcNameW = ch.pcName[0] ? std::wstring(ch.pcName) : Utf8ToWide(cfg.targetIp);
        uint8_t attemptsLeft = 3;
        std::wstring promptMsg = L"Bitte die 6-stellige PIN vom PC-Bildschirm eingeben:";
        bool authed = false;

        while (!m_stop && !authed) {
            {
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_status.stateText = L"PIN-Bestaetigung am PC (" + pcNameW + L")...";
            }

            PinPromptCallback pinCb;
            {
                std::lock_guard<std::mutex> cl(m_cfgIoMutex);
                pinCb = m_pinPromptCb;
            }
            std::string enteredPin;
            if (!pinCb || !pinCb(pcNameW, cfg.targetIp, ch.pcVersion, attemptsLeft, promptMsg, enteredPin) || m_stop) {
                m_connecting = false;
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_status.connecting = false;
                m_status.authRejected = true;
                m_status.stateText = L"PIN-Eingabe abgebrochen";
                m_status.lastError = L"PIN-Kopplung vom Benutzer abgebrochen.";
                m_controlSock.Close();
                return false;
            }

            PairVerifyPayload pv{};
            strncpy_s(pv.pinCode, _countof(pv.pinCode), enteredPin.c_str(), _TRUNCATE);
            CopyWideSafe(pv.senderName, _countof(pv.senderName), GetHostComputerName());
            std::memcpy(pv.senderMac, helloReq.senderMac, 6);
            std::memcpy(pv.senderDeviceId, helloReq.senderDeviceId, 16);

            if (!m_controlSock.SendPacket(MsgType::PairVerify, &pv, sizeof(pv))) {
                m_connecting = false;
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_status.connecting = false;
                m_status.authRejected = true;
                m_status.stateText = L"Kopplung abgelehnt";
                m_status.lastError = L"Verbindung waehrend der PIN-Pruefung beendet (evtl. vom PC abgelehnt).";
                m_controlSock.Close();
                return false;
            }

            if (!WaitForControlPacketRaw(m_controlSock, hdr, payload, remoteProtoVer, rawVerMismatch, closedCleanly, 10000, m_stop)) {
                m_connecting = false;
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_status.connecting = false;
                m_status.authRejected = true;
                m_status.stateText = L"PIN-Pruefung fehlgeschlagen";
                m_status.lastError = L"Keine Antwort auf PIN-Pruefung vom PC erhalten.";
                m_controlSock.Close();
                return false;
            }

            if (hdr.type == static_cast<uint16_t>(MsgType::PairResult) && payload.size() >= sizeof(PairResultPayload)) {
                PairResultPayload pr{};
                std::memcpy(&pr, payload.data(), sizeof(pr));
                pr.message[_countof(pr.message) - 1] = L'\0';
                if (pr.accepted) {
                    authed = true;
                    break;
                }
                attemptsLeft = pr.remainingAttempts;
                promptMsg = pr.message[0] ? std::wstring(pr.message) : L"Falsche PIN.";
                if (attemptsLeft == 0) {
                    m_connecting = false;
                    std::lock_guard<std::mutex> lock(m_stateMutex);
                    m_status.connecting = false;
                    m_status.authRejected = true;
                    m_status.stateText = L"Kopplung abgelehnt";
                    m_status.lastError = L"PIN-Kopplung abgelehnt: " + promptMsg;
                    m_controlSock.Close();
                    return false;
                }
            } else {
                m_connecting = false;
                std::lock_guard<std::mutex> lock(m_stateMutex);
                m_status.connecting = false;
                m_status.authRejected = true;
                m_status.lastError = L"Unerwartete Antwort waehrend der PIN-Pruefung.";
                m_controlSock.Close();
                return false;
            }
        }

        if (!authed) {
            m_connecting = false;
            m_controlSock.Close();
            return false;
        }

        if (!WaitForControlPacketRaw(m_controlSock, hdr, payload, remoteProtoVer, rawVerMismatch, closedCleanly, 5000, m_stop)) {
            if (rawVerMismatch) {
                setVersionMismatchError(remoteProtoVer, AppVersionTuple{}, L"");
                return false;
            }
            m_connecting = false;
            std::lock_guard<std::mutex> lock(m_stateMutex);
            m_status.connecting = false;
            m_status.lastError = L"HelloReceiver nach PIN-Bestaetigung ausgeblieben.";
            m_controlSock.Close();
            return false;
        }
    }

    if (hdr.type == static_cast<uint16_t>(MsgType::VersionMismatch)) {
        VersionMismatchPayload vm{};
        if (payload.size() >= sizeof(vm)) {
            std::memcpy(&vm, payload.data(), sizeof(vm));
            vm.message[_countof(vm.message) - 1] = L'\0';
        }
        setVersionMismatchError(vm.protocolVersion ? vm.protocolVersion : remoteProtoVer,
                                vm.appVersion,
                                vm.message[0] ? std::wstring(vm.message) : L"");
        return false;
    }

    if (hdr.type != static_cast<uint16_t>(MsgType::HelloReceiver) ||
        payload.size() < sizeof(HelloReceiverPayload)) {
        m_connecting = false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.lastError = L"Ungueltige Handshake-Antwort vom PC.";
        m_controlSock.Close();
        return false;
    }

    HelloReceiverPayload helloResp{};
    std::memcpy(&helloResp, payload.data(), sizeof(helloResp));
    helloResp.pcName[_countof(helloResp.pcName) - 1] = L'\0';

    if (helloResp.monitorCount == 0) {
        m_connecting = false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.lastError = L"PC meldet 0 Monitore.";
        m_controlSock.Close();
        return false;
    }

    std::vector<DisplayOutputInfo> captureOutputs;
    std::wstring vddErr;
    if (!SetupVirtualDisplays(helloResp, cfg, captureOutputs, vddErr)) {
        m_connecting = false;
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.stateText = L"Display-Setup fehlgeschlagen";
        m_status.lastError = vddErr;
        m_controlSock.Close();
        return false;
    }

    if (cfg.mode != DisplayMode::MirrorPrimary && !captureOutputs.empty()) {
        std::vector<RECT> vRects;
        vRects.reserve(captureOutputs.size());
        for (const auto& o : captureOutputs) {
            vRects.push_back(o.desktopRect);
        }
        {
            std::lock_guard<std::mutex> vl(m_virtualRectsMutex);
            m_virtualOutputRects = std::move(vRects);
        }
        m_usingExternalDisplays = true;
    }

    StartSessionPayload startReq{};
    uint32_t streamCount = static_cast<uint32_t>(std::min<size_t>(captureOutputs.size(), kMaxMonitors));
    startReq.streamCount = streamCount;
    for (uint32_t i = 0; i < streamCount; ++i) {
        uint32_t targetPcMon = (cfg.mode == DisplayMode::ExtendSecondary && helloResp.monitorCount >= 2) ? 1u : i;
        uint32_t monHz = (helloResp.monitors[targetPcMon].refreshHz >= 30 && helloResp.monitors[targetPcMon].refreshHz <= 240)
                             ? helloResp.monitors[targetPcMon].refreshHz
                             : 60u;
        uint32_t effFps = ResolveEffectiveFps(cfg.targetFps, monHz);
        startReq.streams[i].monitorIndex = targetPcMon;
        startReq.streams[i].width = captureOutputs[i].width;
        startReq.streams[i].height = captureOutputs[i].height;
        startReq.streams[i].refreshHz = monHz;
        startReq.streams[i].codec = static_cast<uint8_t>(cfg.codec);
        startReq.streams[i].bitrateMbps = cfg.bitrateMbps;
        startReq.streams[i].targetFps = effFps;
    }

    if (!m_controlSock.SendPacket(MsgType::StartSession, &startReq, sizeof(startReq))) {
        DisconnectInternal();
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.lastError = L"StartSession konnte nicht gesendet werden.";
        return false;
    }

    if (!WaitForControlPacketRaw(m_controlSock, hdr, payload, remoteProtoVer, rawVerMismatch, closedCleanly, 5000, m_stop) ||
        hdr.type != static_cast<uint16_t>(MsgType::SessionAck) ||
        payload.size() < sizeof(SessionAckPayload)) {
        DisconnectInternal();
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.lastError = L"Keine SessionAck-Antwort vom PC erhalten.";
        return false;
    }

    SessionAckPayload ack{};
    std::memcpy(&ack, payload.data(), sizeof(ack));
    ack.message[_countof(ack.message) - 1] = L'\0';
    if (!ack.accepted) {
        DisconnectInternal();
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.lastError = std::wstring(L"PC hat Session abgelehnt: ") + ack.message;
        return false;
    }

    // Set recv timeout for packet reads after WaitReadable wakes
    m_controlSock.SetTimeouts(2000, 1500);

    // Spawn per-monitor capture/encode/stream workers
    std::vector<std::unique_ptr<StreamWorker>> newWorkers;
    newWorkers.reserve(streamCount);
    for (uint32_t i = 0; i < streamCount; ++i) {
        auto w = std::make_unique<StreamWorker>();
        w->streamIndex = i;
        w->targetPcMonitor = startReq.streams[i].monitorIndex;
        w->adapterIndex = captureOutputs[i].adapterIndex;
        w->outputIndex = captureOutputs[i].outputIndex;
        w->desktopRect = captureOutputs[i].desktopRect;
        w->width = captureOutputs[i].width;
        w->height = captureOutputs[i].height;
        w->refreshHz = startReq.streams[i].refreshHz;
        w->codec = cfg.codec;
        w->bitrateMbps = cfg.bitrateMbps;
        w->targetFps = startReq.streams[i].targetFps;
        w->desiredCodec.store(static_cast<uint8_t>(cfg.codec));
        w->desiredBitrateMbps.store(cfg.bitrateMbps);
        w->desiredTargetFps.store(cfg.targetFps);
        w->peerIp = cfg.targetIp;
        w->streamPort = static_cast<uint16_t>(kPortStreamBase + i);
        w->stats.monitorIndex = startReq.streams[i].monitorIndex;
        w->stats.width = w->width;
        w->stats.height = w->height;
        w->stats.configuredBitrateMbps = cfg.bitrateMbps;
        w->stats.liveBitrateMbps = cfg.bitrateMbps;
        w->stats.targetFps = w->targetFps;

        StreamWorker* raw = w.get();
        w->thread = std::thread([this, raw]() { RunStreamWorker(raw); });
        newWorkers.push_back(std::move(w));
    }
    {
        std::lock_guard<std::mutex> wl(m_workersMutex);
        m_workers = std::move(newWorkers);
    }

    m_connected = true;
    m_connecting = false;
    m_clipboard.SetEnabled(m_clipboardEnabled.load());
    m_clipboard.Start([this](uint32_t kind, uint64_t hash64, const std::vector<uint8_t>& data) {
        SendClipboardData(kind, hash64, data);
    });

    {
        TcpSocket fileSock;
        if (fileSock.Connect(cfg.targetIp, kPortFileTransfer, 2000, &m_stop)) {
            m_fileTransfer.Start(std::move(fileSock));
        } else {
            Logger::Instance().Warn("FILE-TX", L"Could not connect dedicated file transfer port 48214.");
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.connected = true;
        m_status.remoteAppVersion = helloResp.receiverVersion;
        m_status.usingExternalDisplays = m_usingExternalDisplays.load();
        m_status.peerName = helloResp.pcName;
        m_status.peerIp = cfg.targetIp;
        m_status.stateText = L"Verbunden mit " + m_status.peerName + L" (" +
                             std::to_wstring(streamCount) + L" Monitor(e))";
    }

    m_controlThread = std::thread(&SenderEngine::ControlLoop, this);

    Logger::Instance().Info("SND-OK", L"Streaming session started with " + std::to_wstring(streamCount) + L" stream(s).");
    return true;
}

bool SenderEngine::SetupVirtualDisplays(const HelloReceiverPayload& hello,
                                        const SenderConfig& cfg,
                                        std::vector<DisplayOutputInfo>& outSelectedOutputs,
                                        std::wstring& outError) {
    outSelectedOutputs.clear();

    if (cfg.mode == DisplayMode::MirrorPrimary) {
        auto outs = DxgiCapture::EnumerateOutputs();
        if (outs.empty()) {
            outError = L"Kein aktiver DXGI-Bildschirm auf dem Surface gefunden.";
            return false;
        }
        auto it = std::find_if(outs.begin(), outs.end(), [](const DisplayOutputInfo& o) { return o.isPrimary; });
        outSelectedOutputs.push_back(it != outs.end() ? *it : outs.front());
        return true;
    }

    if (!VirtualDisplayCtl::IsDriverActive()) {
        outError = L"Screen2PC Virtual Display Treiber ist nicht aktiv. Bitte scripts\\install_driver.ps1 als Admin auf dem Surface ausfuehren oder Modus 'Mirror' waehlen.";
        return false;
    }

    uint32_t desiredCount = (cfg.mode == DisplayMode::ExtendAll)
                                ? std::min<uint32_t>(hello.monitorCount, static_cast<uint32_t>(kMaxMonitors))
                                : 1u;
    if (desiredCount == 0) desiredCount = 1;

    uint32_t startMonIdx = (cfg.mode == DisplayMode::ExtendSecondary && hello.monitorCount >= 2) ? 1u : 0u;

    std::vector<vdd::VddMonitorSpec> specs;
    for (uint32_t i = 0; i < desiredCount; ++i) {
        uint32_t srcIdx = std::min<uint32_t>(startMonIdx + i, hello.monitorCount - 1);
        vdd::VddMonitorSpec s{};
        s.width = hello.monitors[srcIdx].width ? hello.monitors[srcIdx].width : 1920;
        s.height = hello.monitors[srcIdx].height ? hello.monitors[srcIdx].height : 1080;
        s.refreshHz = hello.monitors[srcIdx].refreshHz ? hello.monitors[srcIdx].refreshHz : 60u;
        if (s.refreshHz < 30 || s.refreshHz > 240) s.refreshHz = 60;
        specs.push_back(s);
    }

    // Snapshot existing outputs before attaching virtual monitors
    auto before = DxgiCapture::EnumerateOutputs();
    std::set<std::wstring> beforeNames;
    for (const auto& o : before) {
        beforeNames.insert(o.deviceName);
    }

    if (!VirtualDisplayCtl::SetMonitors(specs)) {
        outError = L"VirtualDisplayCtl::SetMonitors fehlgeschlagen (Named Pipe Fehler).";
        return false;
    }
    m_createdVirtualDisplays = true;

    // Poll DXGI outputs until the virtual displays appear in Windows Display Manager
    const auto startPoll = std::chrono::steady_clock::now();
    const auto deadline = startPoll + std::chrono::milliseconds(5500);
    bool forcedExtend = false;

    while (std::chrono::steady_clock::now() < deadline && !m_stop) {
        if (!forcedExtend && std::chrono::steady_clock::now() - startPoll > std::chrono::milliseconds(1600)) {
            forcedExtend = true;
            // Ensure Windows topology is in Extend mode instead of Duplicate / PC-screen-only
            SetDisplayConfig(0, nullptr, 0, nullptr, SDC_APPLY | SDC_TOPOLOGY_EXTEND);
        }

        auto after = DxgiCapture::EnumerateOutputs();
        std::vector<DisplayOutputInfo> candidates;

        // First preference: newly appeared outputs
        for (const auto& o : after) {
            if (beforeNames.find(o.deviceName) == beforeNames.end()) {
                candidates.push_back(o);
            }
        }

        // Second preference (e.g., if monitors were already attached from prior run):
        // non-primary outputs matching requested dimensions
        if (candidates.size() < desiredCount) {
            candidates.clear();
            for (const auto& s : specs) {
                for (const auto& o : after) {
                    if (o.isPrimary) continue;
                    bool alreadyPicked = std::any_of(candidates.begin(), candidates.end(), [&](const DisplayOutputInfo& c) {
                        return c.deviceName == o.deviceName;
                    });
                    if (!alreadyPicked && o.width == s.width && o.height == s.height) {
                        candidates.push_back(o);
                        break;
                    }
                }
            }
        }

        if (candidates.size() >= desiredCount) {
            outSelectedOutputs.clear();
            for (const auto& s : specs) {
                auto it = std::find_if(candidates.begin(), candidates.end(), [&](const DisplayOutputInfo& c) {
                    bool used = std::any_of(outSelectedOutputs.begin(), outSelectedOutputs.end(), [&](const DisplayOutputInfo& u) {
                        return u.deviceName == c.deviceName;
                    });
                    return !used && c.width == s.width && c.height == s.height;
                });
                if (it != candidates.end()) {
                    outSelectedOutputs.push_back(*it);
                }
            }
            for (const auto& c : candidates) {
                if (outSelectedOutputs.size() >= desiredCount) break;
                bool used = std::any_of(outSelectedOutputs.begin(), outSelectedOutputs.end(), [&](const DisplayOutputInfo& u) {
                    return u.deviceName == c.deviceName;
                });
                if (!used) outSelectedOutputs.push_back(c);
            }
            if (outSelectedOutputs.size() == desiredCount) {
                return true;
            }
        }

        SleepInterruptible(200, m_stop);
    }

    outError = L"Virtuelle Monitore wurden unter Windows innerhalb von 5s nicht sichtbar. Pruefe Windows-Anzeigeeinstellungen (Desktop erweitern).";
    VirtualDisplayCtl::ClearMonitors();
    m_createdVirtualDisplays = false;
    return false;
}

bool SenderEngine::ReconfigureStream(CodecType codec, uint32_t bitrateMbps, uint32_t targetFps) {
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_cfg.codec = codec;
        m_cfg.bitrateMbps = bitrateMbps;
        m_cfg.targetFps = targetFps;
    }
    if (!m_connected.load()) {
        return false;
    }

    std::vector<ReconfigureStreamPayload> notices;
    {
        std::lock_guard<std::mutex> wl(m_workersMutex);
        for (auto& w : m_workers) {
            w->desiredCodec.store(static_cast<uint8_t>(codec));
            w->desiredBitrateMbps.store(bitrateMbps);
            w->desiredTargetFps.store(targetFps);
            w->forceKeyframe.store(true);
            w->reconfigPending.store(true);

            ReconfigureStreamPayload req{};
            req.streamIndex = w->streamIndex;
            req.codec = static_cast<uint8_t>(codec);
            req.bitrateMbps = bitrateMbps;
            req.targetFps = ResolveEffectiveFps(targetFps, w->refreshHz);
            notices.push_back(req);
        }
    }

    if (!notices.empty()) {
        std::lock_guard<std::mutex> lock(m_controlSendMutex);
        if (m_controlSock.IsValid()) {
            for (const auto& req : notices) {
                m_controlSock.SendPacket(MsgType::ReconfigureStream, &req, sizeof(req));
            }
        }
    }
    return true;
}

void SenderEngine::ControlLoop() {
    ComScope comScope;
    auto lastHeartbeat = std::chrono::steady_clock::now();
    while (!m_stop && m_controlSock.IsValid()) {
        int wr = m_controlSock.WaitReadable(50);
        if (wr < 0) {
            Logger::Instance().Warn("SND-CTL", L"Control socket closed during wait.");
            m_stop = true;
            break;
        }
        if (wr > 0) {
            PacketHeader hdr{};
            std::vector<uint8_t> payload;
            if (m_controlSock.RecvPacket(hdr, payload)) {
                if (hdr.type == static_cast<uint16_t>(MsgType::RequestKeyframe) &&
                    payload.size() >= sizeof(RequestKeyframePayload)) {
                    RequestKeyframePayload req{};
                    std::memcpy(&req, payload.data(), sizeof(req));
                    std::lock_guard<std::mutex> wl(m_workersMutex);
                    for (auto& w : m_workers) {
                        if (w->streamIndex == req.monitorIndex || w->targetPcMonitor == req.monitorIndex) {
                            w->forceKeyframe = true;
                        }
                    }
                } else if (hdr.type == static_cast<uint16_t>(MsgType::StopSession)) {
                    Logger::Instance().Info("SND-CTL", L"Receiver requested StopSession.");
                    if (!m_disconnecting.load()) {
                        m_remoteStopped = true;
                    }
                    m_stop = true;
                    break;
                } else if (hdr.type == static_cast<uint16_t>(MsgType::ClipboardSync) &&
                           payload.size() >= sizeof(ClipboardHeader)) {
                    ClipboardHeader ch{};
                    std::memcpy(&ch, payload.data(), sizeof(ch));
                    if (ch.dataSize > 0 && ch.dataSize <= kMaxClipboardBytes &&
                        payload.size() >= sizeof(ch) + ch.dataSize) {
                        m_clipboard.ApplyRemote(ch.kind, ch.hash64, payload.data() + sizeof(ch), ch.dataSize);
                    }
                } else if (hdr.type == static_cast<uint16_t>(MsgType::KvmMouse) &&
                           payload.size() >= sizeof(KvmMouseEvent)) {
                    if (m_kvmEnabled.load()) {
                        KvmMouseEvent ev{};
                        std::memcpy(&ev, payload.data(), sizeof(ev));
                        RECT rect{};
                        bool haveRect = false;
                        {
                            std::lock_guard<std::mutex> wl(m_workersMutex);
                            for (const auto& w : m_workers) {
                                if (w->streamIndex == ev.monitorIndex || w->targetPcMonitor == ev.monitorIndex) {
                                    rect = w->desktopRect;
                                    if (rect.right <= rect.left || rect.bottom <= rect.top) {
                                        rect = RECT{ 0, 0, static_cast<LONG>(w->width), static_cast<LONG>(w->height) };
                                    }
                                    haveRect = true;
                                    break;
                                }
                            }
                            if (!haveRect && !m_workers.empty()) {
                                const auto& w = m_workers.front();
                                rect = w->desktopRect;
                                if (rect.right <= rect.left || rect.bottom <= rect.top) {
                                    rect = RECT{ 0, 0, static_cast<LONG>(w->width), static_cast<LONG>(w->height) };
                                }
                                haveRect = true;
                            }
                        }
                        if (haveRect) {
                            KvmInput::InjectMouse(ev, rect);
                        }
                    }
                } else if (hdr.type == static_cast<uint16_t>(MsgType::KvmKey) &&
                           payload.size() >= sizeof(KvmKeyEvent)) {
                    if (m_kvmEnabled.load()) {
                        KvmKeyEvent ev{};
                        std::memcpy(&ev, payload.data(), sizeof(ev));
                        KvmInput::InjectKey(ev);
                    }
                }
            } else {
                int err = WSAGetLastError();
                if (err != WSAETIMEDOUT) {
                    Logger::Instance().Warn("SND-CTL", L"Control socket closed, wsa=" + std::to_wstring(err));
                    m_stop = true;
                    break;
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastHeartbeat >= std::chrono::seconds(2)) {
            lastHeartbeat = now;
            std::lock_guard<std::mutex> lock(m_controlSendMutex);
            if (!m_controlSock.SendPacket(MsgType::Heartbeat, nullptr, 0)) {
                m_stop = true;
                break;
            }
        }
    }

    m_stop = true;
    m_connected = false;
    CleanupSessionResources();

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_status.usingExternalDisplays = false;
    if (m_remoteStopped.load()) {
        m_status.remoteStopped = true;
    }
    if (m_status.connected) {
        m_status.connected = false;
        m_status.stateText = m_remoteStopped.load()
                                 ? L"Getrennt (Vom PC beendet)"
                                 : L"Getrennt (Verbindung beendet)";
    }
    m_status.streams.clear();
}

void SenderEngine::SendClipboardData(uint32_t kind, uint64_t hash64, const std::vector<uint8_t>& data) {
    if (!m_clipboardEnabled.load() || !m_connected.load() || data.empty() || data.size() > kMaxClipboardBytes) return;
    std::lock_guard<std::mutex> lock(m_controlSendMutex);
    if (!m_controlSock.IsValid()) return;
    ClipboardHeader ch{ kind, hash64, static_cast<uint32_t>(data.size()) };
    std::vector<uint8_t> pkt(sizeof(ch) + data.size());
    std::memcpy(pkt.data(), &ch, sizeof(ch));
    std::memcpy(pkt.data() + sizeof(ch), data.data(), data.size());
    m_controlSock.SendPacket(MsgType::ClipboardSync, pkt.data(), static_cast<uint32_t>(pkt.size()));
}

void SenderEngine::RunStreamWorker(StreamWorker* w) {
    ComScope comScope;
    Logger::Instance().Info("SND-WRK", L"Starting worker for monitor " + std::to_wstring(w->targetPcMonitor) +
                            L" (" + std::to_wstring(w->width) + L"x" + std::to_wstring(w->height) + L")");

    TcpSocket sock;
    bool connected = false;
    for (int attempt = 0; attempt < 10 && !m_stop && !w->stop; ++attempt) {
        if (sock.Connect(w->peerIp, w->streamPort, 1500, &m_stop)) {
            connected = true;
            break;
        }
        SleepInterruptible(150, m_stop);
    }
    if (!connected) {
        Logger::Instance().Error("SND-WRK", L"Failed to connect stream socket on port " + std::to_wstring(w->streamPort));
        return;
    }
    // Asymmetric low-latency TCP buffers: small SNDBUF prevents multi-frame stale backlog on dynamic scenes
    sock.TuneLowLatency(256 * 1024, 64 * 1024);
    sock.SetTimeouts(1500, 1500);

    DxgiCapture capture;
    bool capOk = false;
    for (int retry = 0; retry < 5 && !m_stop && !w->stop; ++retry) {
        if (capture.Init(w->adapterIndex, w->outputIndex)) {
            capOk = true;
            break;
        }
        SleepInterruptible(200, m_stop);
    }
    if (!capOk) {
        Logger::Instance().Error("SND-WRK", L"DxgiCapture::Init failed for adapter=" +
                                 std::to_wstring(w->adapterIndex) + L" output=" + std::to_wstring(w->outputIndex));
        return;
    }

    if (capture.Width() > 0 && capture.Height() > 0) {
        w->width = capture.Width();
        w->height = capture.Height();
        std::lock_guard<std::mutex> sl(w->statsMutex);
        w->stats.width = w->width;
        w->stats.height = w->height;
    }

    CodecType activeCodec = w->codec;
    if (activeCodec == CodecType::Auto) activeCodec = CodecType::H264;

    LARGE_INTEGER qpcFreqLi{};
    QueryPerformanceFrequency(&qpcFreqLi);
    const int64_t qpcFreq = std::max<int64_t>(1, qpcFreqLi.QuadPart);

    uint32_t effFps = ResolveEffectiveFps(w->desiredTargetFps.load(), w->refreshHz);
    uint32_t configBitrateMbps = w->desiredBitrateMbps.load();
    uint32_t liveBitrateMbps = configBitrateMbps;

    H264Encoder h264;
    if (activeCodec == CodecType::H264) {
        if (!h264.Init(capture.Device(), capture.Context(), w->width, w->height, effFps, liveBitrateMbps)) {
            Logger::Instance().Warn("SND-WRK", L"H264Encoder init failed; falling back to DeltaLossless.");
            activeCodec = CodecType::DeltaLossless;
        }
    }

    DeltaEncoder delta;
    std::vector<uint8_t> encodedPayload;
    std::vector<uint8_t> sendPkt;
    std::vector<uint8_t> cpuBgra;
    std::vector<DeltaRect> pendingDirtyRects;
    bool pendingImageUpdate = false;
    bool forceFullDirtyRect = false;
    uint32_t cpuStride = 0;
    uint64_t frameIndex = 0;

    {
        std::lock_guard<std::mutex> sl(w->statsMutex);
        w->stats.configuredBitrateMbps = configBitrateMbps;
        w->stats.liveBitrateMbps = liveBitrateMbps;
        w->stats.targetFps = effFps;
        if (activeCodec == CodecType::H264) {
            w->stats.codecName = h264.IsHardware() ? L"H.264 (GPU)" : L"H.264 (CPU)";
            w->stats.hardwareAccelerated = h264.IsHardware();
        } else {
            w->stats.codecName = L"Delta Lossless";
            w->stats.hardwareAccelerated = false;
        }
    }

    auto windowStart = std::chrono::steady_clock::now();
    uint64_t windowFrames = 0;
    uint64_t windowBytes = 0;
    bool sentInitialFrame = false;
    uint32_t consecutiveH264Fails = 0;
    uint32_t stallCount = 0;
    uint32_t cleanSendStreak = 0;
    int64_t nextEncodeQpc = 0;

    ID3D11Device* activeD3dDevice = capture.Device();
    auto handleDynamicResizeIfNeeded = [&]() {
        uint32_t cw = capture.Width();
        uint32_t ch = capture.Height();
        ID3D11Device* curDev = capture.Device();
        if (cw == 0 || ch == 0 || !curDev ||
            (cw == w->width && ch == w->height && curDev == activeD3dDevice)) {
            return;
        }
        Logger::Instance().Info("SND-WRK", L"Capture reset/resize: " +
                                std::to_wstring(w->width) + L"x" + std::to_wstring(w->height) +
                                L" -> " + std::to_wstring(cw) + L"x" + std::to_wstring(ch));
        w->width = cw;
        w->height = ch;
        activeD3dDevice = curDev;
        delta.Reset();
        if (activeCodec == CodecType::H264) {
            if (!h264.Init(capture.Device(), capture.Context(), cw, ch, effFps, liveBitrateMbps)) {
                Logger::Instance().Warn("SND-WRK", L"H264Encoder re-init failed after reset/resize; switching to DeltaLossless.");
                h264.Shutdown();
                activeCodec = CodecType::DeltaLossless;
            }
        }
        {
            std::lock_guard<std::mutex> sl(w->statsMutex);
            w->stats.width = cw;
            w->stats.height = ch;
            w->stats.configuredBitrateMbps = configBitrateMbps;
            w->stats.liveBitrateMbps = liveBitrateMbps;
            w->stats.targetFps = effFps;
            if (activeCodec == CodecType::H264) {
                w->stats.codecName = h264.IsHardware() ? L"H.264 (GPU)" : L"H.264 (CPU)";
                w->stats.hardwareAccelerated = h264.IsHardware();
            } else {
                w->stats.codecName = L"Delta Lossless";
                w->stats.hardwareAccelerated = false;
            }
        }
        pendingDirtyRects.clear();
        forceFullDirtyRect = true;
        sentInitialFrame = false;
        consecutiveH264Fails = 0;
        nextEncodeQpc = 0;
    };

    auto applyLiveReconfigureIfNeeded = [&]() {
        if (!w->reconfigPending.exchange(false)) return;

        CodecType newCodec = static_cast<CodecType>(w->desiredCodec.load());
        if (newCodec == CodecType::Auto) newCodec = CodecType::H264;
        uint32_t newBitrate = w->desiredBitrateMbps.load();
        uint32_t newEffFps = ResolveEffectiveFps(w->desiredTargetFps.load(), w->refreshHz);

        const bool codecChanged = (newCodec != activeCodec);
        const bool fpsChanged = (newEffFps != effFps);
        const bool bitrateChanged = (newBitrate != configBitrateMbps || newBitrate != liveBitrateMbps);

        configBitrateMbps = newBitrate;
        liveBitrateMbps = newBitrate;
        effFps = newEffFps;
        stallCount = 0;
        cleanSendStreak = 0;
        nextEncodeQpc = 0;

        if (codecChanged) {
            if (newCodec == CodecType::DeltaLossless) {
                h264.Shutdown();
                delta.Reset();
                activeCodec = CodecType::DeltaLossless;
                sentInitialFrame = false;
                pendingDirtyRects.clear();
                forceFullDirtyRect = true;
            } else {
                delta.Reset();
                if (h264.Init(capture.Device(), capture.Context(), w->width, w->height, effFps, liveBitrateMbps)) {
                    activeCodec = CodecType::H264;
                } else {
                    Logger::Instance().Warn("SND-WRK", L"Live switch to H.264 failed; keeping Delta Lossless.");
                    activeCodec = CodecType::DeltaLossless;
                }
                sentInitialFrame = false;
            }
        } else if (activeCodec == CodecType::H264) {
            if (fpsChanged) {
                if (!h264.Init(capture.Device(), capture.Context(), w->width, w->height, effFps, liveBitrateMbps)) {
                    h264.Shutdown();
                    activeCodec = CodecType::DeltaLossless;
                }
                sentInitialFrame = false;
            } else if (bitrateChanged) {
                h264.SetBitrate(liveBitrateMbps);
            }
        }

        std::lock_guard<std::mutex> sl(w->statsMutex);
        w->stats.configuredBitrateMbps = configBitrateMbps;
        w->stats.liveBitrateMbps = liveBitrateMbps;
        w->stats.targetFps = effFps;
        if (activeCodec == CodecType::H264) {
            w->stats.codecName = h264.IsHardware() ? L"H.264 (GPU)" : L"H.264 (CPU)";
            w->stats.hardwareAccelerated = h264.IsHardware();
        } else {
            w->stats.codecName = L"Delta Lossless";
            w->stats.hardwareAccelerated = false;
        }
        Logger::Instance().Info("SND-CFG",
            L"Live reconfigure monitor #" + std::to_wstring(w->targetPcMonitor + 1) +
            L": " + w->stats.codecName + L", " + std::to_wstring(liveBitrateMbps) +
            L" Mbit/s, " + std::to_wstring(effFps) + L" FPS");
    };

    while (!m_stop && !w->stop) {
        applyLiveReconfigureIfNeeded();

        const uint32_t acqTimeoutMs = std::max<uint32_t>(2u, std::min<uint32_t>(6u, 500u / std::max<uint32_t>(30u, effFps)));
        const bool collectDirty = (activeCodec == CodecType::DeltaLossless);

        bool hasNewImage = false;
        std::vector<DeltaRect> dirtyRects;
        CapturedCursor cursor;
        if (!capture.AcquireFrame(acqTimeoutMs, hasNewImage, dirtyRects, cursor, collectDirty)) {
            if (!capture.IsInitialized()) {
                SleepInterruptible(100, m_stop);
                capture.Init(w->adapterIndex, w->outputIndex);
            }
            if (capture.IsInitialized()) {
                handleDynamicResizeIfNeeded();
            }
            continue;
        }

        handleDynamicResizeIfNeeded();

        // Dispatch cursor updates immediately regardless of video frame pacer schedule
        if (cursor.updated) {
            CursorUpdateHeader ch{};
            ch.monitorIndex = w->targetPcMonitor;
            ch.visible = cursor.visible ? 1 : 0;
            ch.shapeChanged = cursor.shapeChanged ? 1 : 0;
            ch.shapeType = static_cast<uint8_t>(cursor.shapeType);
            ch.x = cursor.x;
            ch.y = cursor.y;
            ch.shapeWidth = cursor.width;
            ch.shapeHeight = cursor.height;
            ch.shapePitch = cursor.pitch;
            ch.hotX = cursor.hotX;
            ch.hotY = cursor.hotY;
            ch.dataSize = cursor.shapeChanged ? static_cast<uint32_t>(cursor.shapeData.size()) : 0;

            if (static_cast<size_t>(ch.dataSize) <= kMaxFrameBytes - sizeof(CursorUpdateHeader)) {
                sendPkt.resize(sizeof(CursorUpdateHeader) + ch.dataSize);
                std::memcpy(sendPkt.data(), &ch, sizeof(CursorUpdateHeader));
                if (ch.dataSize > 0) {
                    std::memcpy(sendPkt.data() + sizeof(CursorUpdateHeader), cursor.shapeData.data(), ch.dataSize);
                }
                if (!sock.SendPacket(MsgType::CursorUpdate, sendPkt.data(), static_cast<uint32_t>(sendPkt.size()))) {
                    break;
                }
            }
        }

        if (hasNewImage) {
            pendingImageUpdate = true;
            if (collectDirty && !forceFullDirtyRect) {
                if (dirtyRects.empty() || pendingDirtyRects.size() + dirtyRects.size() > 64) {
                    pendingDirtyRects.clear();
                    forceFullDirtyRect = true;
                } else {
                    pendingDirtyRects.insert(pendingDirtyRects.end(), dirtyRects.begin(), dirtyRects.end());
                }
            }
        }

        LARGE_INTEGER nowQpcLi{};
        QueryPerformanceCounter(&nowQpcLi);
        const int64_t nowQpc = nowQpcLi.QuadPart;
        const bool needKey = w->forceKeyframe.exchange(false) || !sentInitialFrame;
        const bool dueForEncode = (nextEncodeQpc == 0 || nowQpc >= nextEncodeQpc);

        if ((pendingImageUpdate && dueForEncode) || needKey) {
            const int64_t intervalQpc = std::max<int64_t>(1, qpcFreq / static_cast<int64_t>(effFps));
            nextEncodeQpc = nowQpc + intervalQpc; // No catch-up bursts after slow frames

            encodedPayload.clear();
            bool isKeyframe = false;
            bool encodeOk = false;

            if (activeCodec == CodecType::H264) {
                if (needKey) h264.RequestKeyframe();
                encodeOk = h264.EncodeTexture(capture.FrameTexture(), NowUs(), encodedPayload, isKeyframe);
                if (!encodeOk && !sentInitialFrame) {
                    if (++consecutiveH264Fails >= 4) {
                        if (h264.IsHardware()) {
                            Logger::Instance().Warn("SND-WRK", L"GPU H.264 encode failed at runtime; switching to CPU H.264.");
                            if (h264.Init(capture.Device(), capture.Context(), w->width, w->height, effFps, liveBitrateMbps, false)) {
                                std::lock_guard<std::mutex> sl(w->statsMutex);
                                w->stats.codecName = L"H.264 (CPU)";
                                w->stats.hardwareAccelerated = false;
                                consecutiveH264Fails = 0;
                            } else {
                                activeCodec = CodecType::DeltaLossless;
                                std::lock_guard<std::mutex> sl(w->statsMutex);
                                w->stats.codecName = L"Delta Lossless";
                                w->stats.hardwareAccelerated = false;
                            }
                        } else {
                            Logger::Instance().Warn("SND-WRK", L"H.264 encode failed at runtime; switching to Delta Lossless.");
                            activeCodec = CodecType::DeltaLossless;
                            std::lock_guard<std::mutex> sl(w->statsMutex);
                            w->stats.codecName = L"Delta Lossless";
                            w->stats.hardwareAccelerated = false;
                        }
                    }
                } else if (encodeOk) {
                    consecutiveH264Fails = 0;
                }
            }
            if (activeCodec == CodecType::DeltaLossless) {
                if (capture.CopyFrameToCpu(cpuBgra, cpuStride)) {
                    const std::vector<DeltaRect> emptyHints;
                    const auto& hints = forceFullDirtyRect ? emptyHints : pendingDirtyRects;
                    encodeOk = delta.Encode(cpuBgra.data(), capture.Width(), capture.Height(),
                                            cpuStride, hints, needKey, encodedPayload, isKeyframe);
                }
            }

            if (encodeOk && !encodedPayload.empty() &&
                encodedPayload.size() <= kMaxFrameBytes - sizeof(VideoFrameHeader)) {
                VideoFrameHeader vh{};
                vh.monitorIndex = w->targetPcMonitor;
                vh.frameIndex = ++frameIndex;
                vh.timestampUs = NowUs();
                vh.width = capture.Width();
                vh.height = capture.Height();
                vh.codec = static_cast<uint8_t>(activeCodec);
                vh.flags = (isKeyframe ? kFrameFlagKeyframe : kFrameFlagNone);
                vh.dataSize = static_cast<uint32_t>(encodedPayload.size());

                sendPkt.resize(sizeof(VideoFrameHeader) + encodedPayload.size());
                std::memcpy(sendPkt.data(), &vh, sizeof(VideoFrameHeader));
                std::memcpy(sendPkt.data() + sizeof(VideoFrameHeader), encodedPayload.data(), encodedPayload.size());

                LARGE_INTEGER sendStartLi{}, sendEndLi{};
                QueryPerformanceCounter(&sendStartLi);
                bool sendOk = sock.SendPacket(MsgType::VideoFrame, sendPkt.data(), static_cast<uint32_t>(sendPkt.size()));
                QueryPerformanceCounter(&sendEndLi);
                if (!sendOk) {
                    Logger::Instance().Warn("SND-WRK", L"SendPacket(VideoFrame) failed on monitor " + std::to_wstring(w->targetPcMonitor));
                    break;
                }

                sentInitialFrame = true;
                pendingImageUpdate = false;
                pendingDirtyRects.clear();
                forceFullDirtyRect = false;
                ++windowFrames;
                windowBytes += sendPkt.size() + sizeof(PacketHeader);

                if (activeCodec == CodecType::H264) {
                    const double sendMs = static_cast<double>(sendEndLi.QuadPart - sendStartLi.QuadPart) * 1000.0 /
                                          static_cast<double>(qpcFreq);
                    const double stallThreshMs = std::max<double>(18.0, 1200.0 / static_cast<double>(effFps));
                    if (sendMs > stallThreshMs) {
                        cleanSendStreak = 0;
                        if (++stallCount >= 2 && liveBitrateMbps > 15) {
                            stallCount = 0;
                            uint32_t newBr = std::max<uint32_t>(15u, (liveBitrateMbps * 3u) / 4u);
                            if (newBr < liveBitrateMbps && h264.SetBitrate(newBr)) {
                                liveBitrateMbps = newBr;
                                std::lock_guard<std::mutex> sl(w->statsMutex);
                                w->stats.liveBitrateMbps = liveBitrateMbps;
                                Logger::Instance().Warn("SND-ABR",
                                    L"Send stall (" + std::to_wstring(static_cast<int>(sendMs)) +
                                    L" ms) -> reduced H.264 bitrate to " + std::to_wstring(liveBitrateMbps) + L" Mbit/s");
                            }
                        }
                    } else {
                        stallCount = 0;
                        if (sendMs < 10.0 && liveBitrateMbps < configBitrateMbps) {
                            if (++cleanSendStreak >= effFps * 3u) {
                                cleanSendStreak = 0;
                                uint32_t step = std::max<uint32_t>(5u, configBitrateMbps / 8u);
                                uint32_t newBr = std::min<uint32_t>(configBitrateMbps, liveBitrateMbps + step);
                                if (newBr > liveBitrateMbps && h264.SetBitrate(newBr)) {
                                    liveBitrateMbps = newBr;
                                    std::lock_guard<std::mutex> sl(w->statsMutex);
                                    w->stats.liveBitrateMbps = liveBitrateMbps;
                                    Logger::Instance().Info("SND-ABR",
                                        L"Network recovered -> raised H.264 bitrate to " + std::to_wstring(liveBitrateMbps) + L" Mbit/s");
                                }
                            }
                        } else if (sendMs >= 10.0) {
                            cleanSendStreak = 0;
                        }
                    }
                }
            }
        }

        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - windowStart).count();
        if (elapsedSec >= 0.5) {
            std::lock_guard<std::mutex> sl(w->statsMutex);
            w->stats.fps = static_cast<double>(windowFrames) / elapsedSec;
            w->stats.mbps = (static_cast<double>(windowBytes) * 8.0) / (elapsedSec * 1000000.0);
            w->stats.framesSent = frameIndex;
            w->stats.configuredBitrateMbps = configBitrateMbps;
            w->stats.liveBitrateMbps = liveBitrateMbps;
            w->stats.targetFps = effFps;
            windowStart = now;
            windowFrames = 0;
            windowBytes = 0;
        }
    }

    if (sock.IsValid()) {
        sock.SendPacket(MsgType::StreamEnd, nullptr, 0);
        sock.Close();
    }
    capture.Shutdown();
    h264.Shutdown();
}

void SenderEngine::SignalStop() {
    m_stop = true;
    std::lock_guard<std::mutex> wl(m_workersMutex);
    for (auto& w : m_workers) {
        w->stop = true;
    }
}

void SenderEngine::Disconnect() {
    SignalStop();
    std::lock_guard<std::mutex> lifeLock(m_lifecycleMutex);
    DisconnectInternal();
}

void SenderEngine::CleanupSessionResources() {
    std::lock_guard<std::mutex> cleanLock(m_cleanupMutex);
    m_clipboard.Stop();
    m_fileTransfer.Stop();
    m_controlSock.Close();

    std::vector<std::unique_ptr<StreamWorker>> toJoin;
    {
        std::lock_guard<std::mutex> wl(m_workersMutex);
        for (auto& w : m_workers) {
            w->stop = true;
        }
        toJoin.swap(m_workers);
    }
    for (auto& w : toJoin) {
        if (w->thread.joinable()) w->thread.join();
    }

    m_usingExternalDisplays = false;
    {
        std::lock_guard<std::mutex> vl(m_virtualRectsMutex);
        m_virtualOutputRects.clear();
    }

    if (m_createdVirtualDisplays.exchange(false)) {
        VirtualDisplayCtl::ClearMonitors();
    }
}

void SenderEngine::DisconnectInternal() {
    m_disconnecting = true;
    m_stop = true;
    m_connected = false;
    m_connecting = false;

    if (m_controlSock.IsValid()) {
        {
            std::lock_guard<std::mutex> lock(m_controlSendMutex);
            m_controlSock.SendPacket(MsgType::StopSession, nullptr, 0);
        }
        m_controlSock.Close();
    }

    // Wait for ControlLoop to exit cleanly before CleanupSessionResources runs
    // (ControlLoop calls CleanupSessionResources itself; CleanupSessionResources is mutex-serialized
    // and ClipboardSync/FileTransfer Stop() are thread-safe, so there is no double-join race).
    std::thread ctrlToJoin;
    if (m_controlThread.joinable()) {
        ctrlToJoin = std::move(m_controlThread);
    }
    if (ctrlToJoin.joinable()) {
        if (ctrlToJoin.get_id() == std::this_thread::get_id()) {
            ctrlToJoin.detach();
        } else {
            ctrlToJoin.join();
        }
    }

    CleanupSessionResources();
    m_disconnecting = false;

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_status.connected = false;
    m_status.connecting = false;
    m_status.usingExternalDisplays = false;
    if (m_status.lastError.empty()) {
        m_status.stateText = L"Getrennt";
    }
    m_status.streams.clear();
}

SenderStatus SenderEngine::GetStatus() const {
    SenderStatus s;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        s = m_status;
    }

    // Avoid synchronous named pipe probing during active connect, stream, or teardown;
    // cache result and refresh every ~2.5s only when idle.
    ULONGLONG nowMs = GetTickCount64();
    ULONGLONG lastMs = m_lastDriverCheckMs.load();
    if (!m_connecting.load() && !m_connected.load() && !m_disconnecting.load() &&
        (lastMs == 0 || (nowMs - lastMs) >= 2500)) {
        bool active = VirtualDisplayCtl::IsDriverActive();
        m_cachedDriverActive.store(active);
        m_lastDriverCheckMs.store(nowMs);
    }
    s.driverActive = m_cachedDriverActive.load();
    s.fileStats = m_fileTransfer.GetStats();
    s.streams.clear();
    std::lock_guard<std::mutex> wl(m_workersMutex);
    for (const auto& w : m_workers) {
        std::lock_guard<std::mutex> sl(w->statsMutex);
        s.streams.push_back(w->stats);
    }
    return s;
}

} // namespace s2p
