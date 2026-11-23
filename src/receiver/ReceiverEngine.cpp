#include "ReceiverEngine.h"
#include "D3D11Renderer.h"
#include "../common/CodecH264.h"
#include "../common/CodecDelta.h"
#include "../common/KvmInput.h"
#include "../common/Logger.h"

#include <bcrypt.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cstring>

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

struct EnumMonEntry {
    MonitorDesc desc{};
    HMONITOR hMon = nullptr;
};

BOOL CALLBACK MonitorEnumCb(HMONITOR hMon, HDC, LPRECT, LPARAM data) {
    auto* list = reinterpret_cast<std::vector<EnumMonEntry>*>(data);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

    EnumMonEntry entry{};
    entry.hMon = hMon;
    MonitorDesc& md = entry.desc;
    md.left = mi.rcMonitor.left;
    md.top = mi.rcMonitor.top;
    md.right = mi.rcMonitor.right;
    md.bottom = mi.rcMonitor.bottom;
    md.width = static_cast<uint32_t>(std::max<LONG>(1, mi.rcMonitor.right - mi.rcMonitor.left));
    md.height = static_cast<uint32_t>(std::max<LONG>(1, mi.rcMonitor.bottom - mi.rcMonitor.top));
    md.isPrimary = (mi.dwFlags & MONITORINFOF_PRIMARY) ? 1 : 0;
    md.refreshHz = 60;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        if (dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
            md.width = dm.dmPelsWidth;
            md.height = dm.dmPelsHeight;
        }
        if (dm.dmDisplayFrequency >= 24) {
            md.refreshHz = dm.dmDisplayFrequency;
        }
    }

    wcsncpy_s(md.friendlyName, _countof(md.friendlyName), mi.szDevice, _TRUNCATE);
    list->push_back(entry);
    return TRUE;
}

std::vector<EnumMonEntry> EnumerateLocalMonitorEntries() {
    std::vector<EnumMonEntry> entries;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumCb, reinterpret_cast<LPARAM>(&entries));
    // Sort primary monitor first, then left-to-right
    std::sort(entries.begin(), entries.end(), [](const EnumMonEntry& a, const EnumMonEntry& b) {
        if (a.desc.isPrimary != b.desc.isPrimary) return a.desc.isPrimary > b.desc.isPrimary;
        if (a.desc.left != b.desc.left) return a.desc.left < b.desc.left;
        return a.desc.top < b.desc.top;
    });
    if (entries.size() > kMaxMonitors) {
        entries.resize(kMaxMonitors);
    }
    for (size_t i = 0; i < entries.size(); ++i) {
        entries[i].desc.index = static_cast<uint32_t>(i);
    }
    return entries;
}

void CopyWideSafe(wchar_t* dst, size_t dstChars, const std::wstring& src) {
    if (!dst || dstChars == 0) return;
    wcsncpy_s(dst, dstChars, src.c_str(), _TRUNCATE);
}

void FillRandomNonce(uint8_t* dst, size_t len) {
    if (!dst || len == 0) return;
    if (BCRYPT_SUCCESS(BCryptGenRandom(nullptr, dst, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return;
    }
    uint64_t t = GetTickCount64();
    for (size_t i = 0; i < len; ++i) {
        t ^= (t << 13);
        t ^= (t >> 7);
        dst[i] = static_cast<uint8_t>((t >> ((i & 7) * 8)) & 0xFFu);
    }
}

bool WaitAndRecvRaw(TcpSocket& sock,
                    PacketHeader& hdr,
                    std::vector<uint8_t>& payload,
                    uint16_t& outRemoteVer,
                    bool& outVerMismatch,
                    int maxWaitMs,
                    const std::atomic<bool>& stop) {
    outRemoteVer = 0;
    outVerMismatch = false;
    int waited = 0;
    while (waited < maxWaitMs && !stop.load() && sock.IsValid()) {
        int step = std::min(100, maxWaitMs - waited);
        int wr = sock.WaitReadable(step);
        if (wr < 0) return false;
        if (wr > 0) {
            return sock.RecvPacketRaw(hdr, payload, outRemoteVer, outVerMismatch);
        }
        waited += step;
    }
    return false;
}

} // namespace

ReceiverEngine::~ReceiverEngine() {
    Stop();
}

void ReceiverEngine::SetConfigIniPath(const std::wstring& iniPath) {
    std::lock_guard<std::mutex> lock(m_cfgMutex);
    m_configIniPath = iniPath;
}

void ReceiverEngine::SetPinCallbacks(PinShowCallback onShow, PinDismissCallback onDismiss) {
    std::lock_guard<std::mutex> lock(m_cfgMutex);
    m_onPinShow = std::move(onShow);
    m_onPinDismiss = std::move(onDismiss);
}

void ReceiverEngine::SetVersionMismatchCallback(VersionMismatchCallback cb) {
    std::lock_guard<std::mutex> lock(m_cfgMutex);
    m_onVersionMismatch = std::move(cb);
}

std::vector<TrustedDeviceEntry> ReceiverEngine::GetTrustedDevices() const {
    std::wstring ini;
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        ini = m_configIniPath;
    }
    return LoadTrustedDevices(ini);
}

bool ReceiverEngine::RemoveTrustedDeviceByKey(const std::wstring& key) {
    std::wstring ini;
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        ini = m_configIniPath;
    }
    return RemoveTrustedDevice(ini, key);
}

bool ReceiverEngine::ClearAllTrustedDevices() {
    std::wstring ini;
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        ini = m_configIniPath;
    }
    return ClearTrustedDevices(ini);
}

void ReceiverEngine::ShowHiddenStreamWindows() {
    std::lock_guard<std::mutex> wl(m_workersMutex);
    for (auto& w : m_workers) {
        w->showRequested.store(true, std::memory_order_release);
        std::lock_guard<std::mutex> rl(w->rendererPtrMutex);
        if (w->activeRenderer) {
            w->activeRenderer->SignalWake();
        }
    }
}

bool ReceiverEngine::HasHiddenStreamWindows() const {
    std::lock_guard<std::mutex> wl(m_workersMutex);
    for (const auto& w : m_workers) {
        if (w->hiddenByUser.load(std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

std::vector<MonitorDesc> ReceiverEngine::EnumerateLocalMonitors() {
    auto entries = EnumerateLocalMonitorEntries();
    std::vector<MonitorDesc> list;
    list.reserve(entries.size());
    for (const auto& e : entries) {
        list.push_back(e.desc);
    }
    return list;
}

std::vector<MonitorDesc> ReceiverEngine::EnumerateLocalMonitorsWithHdr(std::vector<MonitorHdrInfo>& outHdr) {
    auto entries = EnumerateLocalMonitorEntries();
    std::vector<MonitorDesc> list;
    list.reserve(entries.size());
    outHdr.clear();
    outHdr.reserve(entries.size());

    for (const auto& e : entries) {
        list.push_back(e.desc);
        MonitorHdrInfo hdrInfo{};
        D3D11Renderer::QueryMonitorHdrInfo(e.hMon, hdrInfo);
        outHdr.push_back(std::move(hdrInfo));
    }
    return list;
}

void ReceiverEngine::RefreshLocalMonitorInfo() {
    std::vector<MonitorHdrInfo> hdr;
    auto mons = EnumerateLocalMonitorsWithHdr(hdr);
    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_status.localMonitors = std::move(mons);
    m_status.localMonitorHdr = std::move(hdr);
}

void ReceiverEngine::SetMonitorColorConfig(uint32_t monitorIndex, const MonitorColorConfig& cfg) {
    if (monitorIndex >= kMaxMonitors) return;
    MonitorColorConfig clamped = cfg;
    ClampColorConfig(clamped);
    {
        std::lock_guard<std::mutex> lock(m_colorMutex);
        m_colorConfigs[monitorIndex] = clamped;
    }
    m_colorGeneration[monitorIndex].fetch_add(1, std::memory_order_release);
}

MonitorColorConfig ReceiverEngine::GetMonitorColorConfig(uint32_t monitorIndex) const {
    if (monitorIndex >= kMaxMonitors) return MonitorColorConfig{};
    std::lock_guard<std::mutex> lock(m_colorMutex);
    return m_colorConfigs[monitorIndex];
}

void ReceiverEngine::SetKvmMouseSpeedPct(uint32_t pct) {
    uint32_t clamped = KvmInput::ClampSpeedPct(pct);
    m_kvmMouseSpeedPct.store(clamped);
    std::lock_guard<std::mutex> wl(m_workersMutex);
    for (const auto& w : m_workers) {
        std::lock_guard<std::mutex> rl(w->rendererPtrMutex);
        if (w->activeRenderer) {
            w->activeRenderer->SetKvmMouseSpeedPct(clamped);
        }
    }
}

void ReceiverEngine::SetKvmScrollSpeedPct(uint32_t pct) {
    uint32_t clamped = KvmInput::ClampSpeedPct(pct);
    m_kvmScrollSpeedPct.store(clamped);
    std::lock_guard<std::mutex> wl(m_workersMutex);
    for (const auto& w : m_workers) {
        std::lock_guard<std::mutex> rl(w->rendererPtrMutex);
        if (w->activeRenderer) {
            w->activeRenderer->SetKvmScrollSpeedPct(clamped);
        }
    }
}

void ReceiverEngine::SetStreamHudEnabled(bool enabled) {
    m_streamHudEnabled.store(enabled);
    std::lock_guard<std::mutex> wl(m_workersMutex);
    for (const auto& w : m_workers) {
        std::lock_guard<std::mutex> rl(w->rendererPtrMutex);
        if (w->activeRenderer) {
            w->activeRenderer->SetHudVisible(enabled);
        }
    }
}

bool ReceiverEngine::Start(bool startFullscreen) {
    Stop();
    m_stop = false;
    m_startFullscreen = startFullscreen;

    std::vector<MonitorHdrInfo> hdr;
    auto mons = EnumerateLocalMonitorsWithHdr(hdr);
    auto ips = GetLocalIPv4Addresses();

    if (!m_discoverySock.Bind(kPortDiscovery, true)) {
        Logger::Instance().Warn("RCV-UDP", L"Could not bind UDP discovery port " + std::to_wstring(kPortDiscovery));
    }
    if (!m_realtimeUdpSock.Bind(kPortRealtimeUdp, true)) {
        Logger::Instance().Warn("RCV-UDP", L"Could not bind realtime UDP port " + std::to_wstring(kPortRealtimeUdp));
    }
    if (!m_controlListenSock.Listen(kPortControl, 4)) {
        Logger::Instance().Error("RCV-TCP", L"Failed to listen on control port " + std::to_wstring(kPortControl));
        m_discoverySock.Close();
        m_realtimeUdpSock.Close();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status = ReceiverStatus{};
        m_status.running = true;
        m_status.localMonitors = mons;
        m_status.localMonitorHdr = hdr;
        m_status.localIps = ips;
        m_status.stateText = L"Bereit - Warte auf Verbindung vom Surface (Port " +
                             std::to_wstring(kPortControl) + L")";
    }

    m_running = true;
    m_discoveryThread = std::thread(&ReceiverEngine::DiscoveryLoop, this);
    if (m_realtimeUdpSock.IsValid()) {
        m_realtimeUdpThread = std::thread(&ReceiverEngine::RealtimeUdpLoop, this);
    }
    m_controlThread = std::thread(&ReceiverEngine::ControlAcceptLoop, this);
    Logger::Instance().Info("RCV-OK", L"Receiver listening on UDP " + std::to_wstring(kPortDiscovery) +
                            L"/" + std::to_wstring(kPortRealtimeUdp) +
                            L" / TCP " + std::to_wstring(kPortControl));
    return true;
}

void ReceiverEngine::Stop() {
    m_stop = true;
    m_running = false;
    DisconnectSession();

    m_discoverySock.Close();
    m_realtimeUdpSock.Close();
    m_controlListenSock.Close();

    if (m_discoveryThread.joinable()) m_discoveryThread.join();
    if (m_realtimeUdpThread.joinable()) m_realtimeUdpThread.join();
    if (m_controlThread.joinable()) m_controlThread.join();

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_status.running = false;
    m_status.sessionActive = false;
    m_status.hasHiddenStreams = false;
    m_status.stateText = L"Gestoppt";
}

void ReceiverEngine::DiscoveryLoop() {
    uint8_t buf[2048]{};
    while (!m_stop && m_discoverySock.IsValid()) {
        std::string senderIp;
        uint16_t senderPort = 0;
        int n = m_discoverySock.RecvFrom(buf, sizeof(buf), senderIp, senderPort, 250);
        if (n <= 0) continue;
        if (static_cast<size_t>(n) < sizeof(PacketHeader) + sizeof(DiscoverReqPayload)) continue;

        PacketHeader hdr{};
        std::memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != kProtocolMagic || hdr.payloadSize > kMaxFrameBytes) continue;
        if (hdr.type != static_cast<uint16_t>(MsgType::DiscoverReq)) continue;

        if (hdr.version != kProtocolVersion) {
            VersionMismatchPayload vm{};
            vm.protocolVersion = kProtocolVersion;
            vm.appVersion = CurrentAppVersionTuple();
            CopyWideSafe(vm.message, _countof(vm.message), GetHostComputerName());

            struct {
                PacketHeader hdr;
                VersionMismatchPayload body;
            } vmReply{};
            vmReply.hdr = MakeHeader(MsgType::VersionMismatch, sizeof(VersionMismatchPayload));
            vmReply.body = vm;
            m_discoverySock.SendTo(senderIp, senderPort, &vmReply, sizeof(vmReply));
            continue;
        }

        DiscoverReqPayload req{};
        std::memcpy(&req, buf + sizeof(PacketHeader), sizeof(req));

        auto mons = EnumerateLocalMonitors();
        DiscoverRespPayload resp{};
        resp.senderNonce = req.senderNonce;
        resp.controlPort = kPortControl;
        resp.streamBasePort = kPortStreamBase;
        resp.monitorCount = static_cast<uint32_t>(std::min<size_t>(mons.size(), kMaxMonitors));
        CopyWideSafe(resp.pcName, _countof(resp.pcName), GetHostComputerName());
        for (uint32_t i = 0; i < resp.monitorCount; ++i) {
            resp.monitors[i] = mons[i];
        }
        resp.receiverVersion = CurrentAppVersionTuple();

        struct {
            PacketHeader hdr;
            DiscoverRespPayload body;
        } reply{};
        reply.hdr = MakeHeader(MsgType::DiscoverResp, sizeof(DiscoverRespPayload));
        reply.body = resp;
        m_discoverySock.SendTo(senderIp, senderPort, &reply, sizeof(reply));
    }
}

void ReceiverEngine::RealtimeUdpLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    uint8_t buf[256]{};
    while (!m_stop && m_realtimeUdpSock.IsValid()) {
        std::string senderIp;
        uint16_t senderPort = 0;
        int n = m_realtimeUdpSock.RecvFrom(buf, sizeof(buf), senderIp, senderPort, 100);
        if (n <= 0) continue;
        if (static_cast<size_t>(n) < sizeof(PacketHeader) + sizeof(CursorUpdateHeader)) continue;

        PacketHeader hdr{};
        std::memcpy(&hdr, buf, sizeof(hdr));
        if (hdr.magic != kProtocolMagic || hdr.version != kProtocolVersion) continue;
        if (hdr.type != static_cast<uint16_t>(MsgType::CursorUpdate) ||
            hdr.payloadSize < sizeof(CursorUpdateHeader)) continue;

        if (hdr.reserved != 0) {
            uint32_t prev = m_lastCursorUdpSeq.load(std::memory_order_relaxed);
            if (static_cast<int32_t>(hdr.reserved - prev) <= 0) {
                continue;
            }
            m_lastCursorUdpSeq.store(hdr.reserved, std::memory_order_relaxed);
        }

        CursorUpdateHeader ch{};
        std::memcpy(&ch, buf + sizeof(PacketHeader), sizeof(ch));

        std::lock_guard<std::mutex> wl(m_workersMutex);
        D3D11Renderer* targetRenderer = nullptr;
        std::unique_lock<std::mutex> rl;
        for (auto& w : m_workers) {
            if (w->targetMonitorIndex == ch.monitorIndex || w->monitorIndex == ch.monitorIndex) {
                rl = std::unique_lock<std::mutex>(w->rendererPtrMutex);
                targetRenderer = w->activeRenderer;
                break;
            }
        }
        if (!targetRenderer && !m_workers.empty()) {
            rl = std::unique_lock<std::mutex>(m_workers.front()->rendererPtrMutex);
            targetRenderer = m_workers.front()->activeRenderer;
        }
        if (targetRenderer) {
            targetRenderer->QueueCursorUpdate(ch.visible != 0, ch.x, ch.y, false, 0, 0, 0, 0, 0, 0, nullptr, 0);
        }
    }
}

void ReceiverEngine::ControlAcceptLoop() {
    while (!m_stop && m_controlListenSock.IsValid()) {
        std::string peerIp;
        TcpSocket client = m_controlListenSock.Accept(&peerIp, 250);
        if (!client.IsValid()) continue;

        Logger::Instance().Info("RCV-CONN", L"Incoming control connection from " + Utf8ToWide(peerIp));
        HandleControlClient(std::move(client), peerIp);
    }
}

void ReceiverEngine::HandleControlClient(TcpSocket clientSock, const std::string& peerIp) {
    // IMPORTANT: Never disconnect an active session until version check + PIN/MAC auth succeed.
    clientSock.TuneLowLatency(128 * 1024, 128 * 1024);
    clientSock.SetTimeouts(3000, 1000);

    std::wstring iniPath;
    PinShowCallback onPinShow;
    PinDismissCallback onPinDismiss;
    VersionMismatchCallback onVerMismatch;
    {
        std::lock_guard<std::mutex> lock(m_cfgMutex);
        iniPath = m_configIniPath;
        onPinShow = m_onPinShow;
        onPinDismiss = m_onPinDismiss;
        onVerMismatch = m_onVersionMismatch;
    }

    PacketHeader hdr{};
    std::vector<uint8_t> payload;
    uint16_t remoteProtoVer = 0;
    bool rawVerMismatch = false;
    if (!WaitAndRecvRaw(clientSock, hdr, payload, remoteProtoVer, rawVerMismatch, 3500, m_stop)) {
        if (rawVerMismatch) {
            std::wstring msg = L"Surface nutzt Protokoll v" + std::to_wstring(remoteProtoVer) +
                               L", dieser PC erwartet Protokoll v" + std::to_wstring(kProtocolVersion) +
                               L" (App v" + FormatVersionTuple(CurrentAppVersionTuple()) +
                               L"). Bitte beide Apps per GitHub-Updater aktualisieren.";
            VersionMismatchPayload vm{};
            vm.protocolVersion = kProtocolVersion;
            vm.appVersion = CurrentAppVersionTuple();
            CopyWideSafe(vm.message, _countof(vm.message), msg);
            clientSock.SendPacket(MsgType::VersionMismatch, &vm, sizeof(vm));
            Logger::Instance().Warn("RCV-VER", msg);
            if (onVerMismatch) {
                onVerMismatch(remoteProtoVer, AppVersionTuple{}, msg);
            }
        } else {
            Logger::Instance().Warn("RCV-CTL", L"Expected HelloSender packet (timed out or closed).");
        }
        return;
    }

    if (hdr.type != static_cast<uint16_t>(MsgType::HelloSender) ||
        payload.size() < sizeof(HelloSenderPayload)) {
        Logger::Instance().Warn("RCV-CTL", L"Expected HelloSender packet.");
        return;
    }

    HelloSenderPayload helloSender{};
    std::memcpy(&helloSender, payload.data(), sizeof(helloSender));
    helloSender.senderName[_countof(helloSender.senderName) - 1] = L'\0';

    // Resolve MAC from LAN ARP cache first, falling back to sender-supplied MAC
    uint8_t resolvedMac[6]{};
    if (!ResolvePeerMacFromIp(peerIp, resolvedMac) || IsAllZero(resolvedMac, 6)) {
        std::memcpy(resolvedMac, helloSender.senderMac, 6);
    }

    TrustedDeviceEntry matchedTrust{};
    bool trusted = false;
    if (!iniPath.empty()) {
        trusted = IsDeviceTrusted(iniPath, resolvedMac, helloSender.senderDeviceId, &matchedTrust);
    }

    if (trusted) {
        Logger::Instance().Info("RCV-AUTH",
            L"Trusted device auto-approved: " +
            (helloSender.senderName[0] ? std::wstring(helloSender.senderName) : Utf8ToWide(peerIp)) +
            L" (" + (!matchedTrust.macStr.empty() ? matchedTrust.macStr : matchedTrust.key) + L")");
        if (!iniPath.empty() && matchedTrust.lastIp != Utf8ToWide(peerIp) &&
            MakeTrustedDeviceKey(resolvedMac, helloSender.senderDeviceId) == matchedTrust.key) {
            SaveTrustedDevice(iniPath,
                              helloSender.senderName[0] ? std::wstring(helloSender.senderName) : matchedTrust.name,
                              resolvedMac,
                              helloSender.senderDeviceId,
                              peerIp);
        }
    } else {
        const std::string expectedPin = GeneratePin6();
        auto pinSession = std::make_shared<PinPopupSession>();
        pinSession->req.pinCode = expectedPin;
        pinSession->req.senderName = helloSender.senderName[0] ? std::wstring(helloSender.senderName) : L"Surface";
        pinSession->req.senderIp = peerIp;
        std::memcpy(pinSession->req.senderMac, resolvedMac, 6);
        std::memcpy(pinSession->req.senderDeviceId, helloSender.senderDeviceId, 16);
        pinSession->req.senderVersion = helloSender.senderVersion;

        if (onPinShow) {
            onPinShow(pinSession);
        }

        PairChallengePayload challenge{};
        FillRandomNonce(challenge.nonce, sizeof(challenge.nonce));
        CopyWideSafe(challenge.pcName, _countof(challenge.pcName), GetHostComputerName());
        challenge.pcVersion = CurrentAppVersionTuple();

        if (!clientSock.SendPacket(MsgType::PairChallenge, &challenge, sizeof(challenge))) {
            if (onPinDismiss) onPinDismiss();
            return;
        }

        Logger::Instance().Info("RCV-AUTH",
            L"PIN challenge issued for " + pinSession->req.senderName + L" (" + Utf8ToWide(peerIp) + L").");

        uint8_t remainingAttempts = 3;
        bool authed = false;
        const auto authDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(90);

        while (!m_stop && !pinSession->rejected.load() &&
               std::chrono::steady_clock::now() < authDeadline &&
               clientSock.IsValid()) {
            int wr = clientSock.WaitReadable(50);
            if (wr < 0) {
                break;
            }
            if (wr == 0) {
                continue;
            }

            PacketHeader phdr{};
            std::vector<uint8_t> ppayload;
            uint16_t pVer = 0;
            bool pMismatch = false;
            if (!clientSock.RecvPacketRaw(phdr, ppayload, pVer, pMismatch)) {
                if (pMismatch && onVerMismatch) {
                    onVerMismatch(pVer, AppVersionTuple{}, L"Versionskonflikt waehrend PIN-Pruefung.");
                }
                break;
            }

            if (phdr.type != static_cast<uint16_t>(MsgType::PairVerify) ||
                ppayload.size() < sizeof(PairVerifyPayload)) {
                break;
            }

            PairVerifyPayload pv{};
            std::memcpy(&pv, ppayload.data(), sizeof(pv));
            pv.pinCode[sizeof(pv.pinCode) - 1] = '\0';
            pv.senderName[_countof(pv.senderName) - 1] = L'\0';

            if (ConstantTimePinEquals(pv.pinCode, expectedPin.c_str(), 6)) {
                bool remember = pinSession->remember.load();
                uint8_t effectiveMac[6]{};
                if (!IsAllZero(resolvedMac, 6)) {
                    std::memcpy(effectiveMac, resolvedMac, 6);
                } else if (!IsAllZero(pv.senderMac, 6)) {
                    std::memcpy(effectiveMac, pv.senderMac, 6);
                } else {
                    std::memcpy(effectiveMac, helloSender.senderMac, 6);
                }
                const uint8_t* effectiveId = !IsAllZero(pv.senderDeviceId, 16)
                                                 ? pv.senderDeviceId
                                                 : helloSender.senderDeviceId;
                std::wstring devName = helloSender.senderName[0]
                                           ? std::wstring(helloSender.senderName)
                                           : (pv.senderName[0] ? std::wstring(pv.senderName) : Utf8ToWide(peerIp));

                bool saved = false;
                if (remember && !iniPath.empty()) {
                    saved = SaveTrustedDevice(iniPath, devName, effectiveMac, effectiveId, peerIp);
                }

                PairResultPayload pr{};
                pr.accepted = 1;
                pr.remembered = saved ? 1 : 0;
                pr.remainingAttempts = remainingAttempts;
                CopyWideSafe(pr.message, _countof(pr.message),
                             saved ? L"Gekoppelt und MAC gespeichert." : L"Gekoppelt.");
                if (clientSock.SendPacket(MsgType::PairResult, &pr, sizeof(pr))) {
                    authed = true;
                }
                Logger::Instance().Info("RCV-AUTH",
                    L"PIN verified for " + devName + (saved ? L" (MAC/Device saved to trusted list)." : L" (one-time session)."));
                break;
            }

            if (remainingAttempts > 0) {
                --remainingAttempts;
            }
            PairResultPayload pr{};
            pr.accepted = 0;
            pr.remembered = 0;
            pr.remainingAttempts = remainingAttempts;
            CopyWideSafe(pr.message, _countof(pr.message),
                         remainingAttempts > 0
                             ? L"Falsche PIN - bitte erneut versuchen."
                             : L"Zu viele falsche PIN-Eingaben.");
            clientSock.SendPacket(MsgType::PairResult, &pr, sizeof(pr));
            Logger::Instance().Warn("RCV-AUTH",
                L"Invalid PIN attempt from " + Utf8ToWide(peerIp) +
                L", remaining=" + std::to_wstring(remainingAttempts));
            if (remainingAttempts == 0) {
                break;
            }
        }

        if (!authed && pinSession->rejected.load() && clientSock.IsValid()) {
            PairResultPayload pr{};
            pr.accepted = 0;
            pr.remainingAttempts = 0;
            CopyWideSafe(pr.message, _countof(pr.message), L"Verbindung am PC abgelehnt.");
            clientSock.SendPacket(MsgType::PairResult, &pr, sizeof(pr));
            Logger::Instance().Warn("RCV-AUTH", L"Pairing request rejected by PC user.");
        }

        if (onPinDismiss) {
            onPinDismiss();
        }
        if (!authed) {
            clientSock.Close();
            return;
        }
    }

    // Authenticated! Now tear down any previous session cleanly before starting the new one.
    DisconnectSession();
    m_sessionStop = false;

    std::vector<MonitorHdrInfo> hdrList;
    auto mons = EnumerateLocalMonitorsWithHdr(hdrList);
    HelloReceiverPayload helloResp{};
    CopyWideSafe(helloResp.pcName, _countof(helloResp.pcName), GetHostComputerName());
    helloResp.monitorCount = static_cast<uint32_t>(std::min<size_t>(mons.size(), kMaxMonitors));
    for (uint32_t i = 0; i < helloResp.monitorCount; ++i) {
        helloResp.monitors[i] = mons[i];
    }
    helloResp.receiverVersion = CurrentAppVersionTuple();

    if (!clientSock.SendPacket(MsgType::HelloReceiver, &helloResp, sizeof(helloResp))) {
        return;
    }

    // Wait for StartSession
    if (!WaitAndRecvRaw(clientSock, hdr, payload, remoteProtoVer, rawVerMismatch, 5000, m_stop) ||
        hdr.type != static_cast<uint16_t>(MsgType::StartSession) ||
        payload.size() < sizeof(StartSessionPayload)) {
        Logger::Instance().Warn("RCV-CTL", L"Expected StartSession packet.");
        return;
    }

    StartSessionPayload startReq{};
    std::memcpy(&startReq, payload.data(), sizeof(startReq));
    uint32_t streamCount = std::min<uint32_t>(startReq.streamCount, static_cast<uint32_t>(kMaxMonitors));

    SessionAckPayload ack{};
    if (streamCount == 0 || mons.empty()) {
        ack.accepted = 0;
        ack.errorCode = 1;
        CopyWideSafe(ack.message, _countof(ack.message), L"Keine Zielmonitore verfuegbar.");
        clientSock.SendPacket(MsgType::SessionAck, &ack, sizeof(ack));
        return;
    }

    // Bind all stream listening sockets BEFORE sending SessionAck so Sender never gets ECONNREFUSED
    bool allListening = true;
    for (uint32_t i = 0; i < streamCount; ++i) {
        const auto& spec = startReq.streams[i];
        uint32_t targetIdx = std::min<uint32_t>(spec.monitorIndex, static_cast<uint32_t>(mons.size() - 1));
        const auto& md = mons[targetIdx];

        auto w = std::make_unique<StreamReceiverWorker>();
        w->monitorIndex = i;
        w->targetMonitorIndex = targetIdx;
        w->port = static_cast<uint16_t>(kPortStreamBase + i);
        w->targetMonitorRect = RECT{ md.left, md.top, md.right, md.bottom };
        w->expectedWidth = spec.width ? spec.width : md.width;
        w->expectedHeight = spec.height ? spec.height : md.height;
        uint32_t monHz = (md.refreshHz >= 24 && md.refreshHz <= 240) ? md.refreshHz : 60u;
        w->targetFps = (spec.targetFps >= 15 && spec.targetFps <= 240) ? spec.targetFps : monHz;
        w->desiredTargetFps.store(w->targetFps);
        w->startFullscreen = m_startFullscreen.load();
        w->stats.monitorIndex = i;
        w->stats.width = w->expectedWidth;
        w->stats.height = w->expectedHeight;

        if (!w->listenSock.Listen(w->port, 2)) {
            Logger::Instance().Error("RCV-STRM", L"Failed to listen on stream port " + std::to_wstring(w->port));
            allListening = false;
            break;
        }

        StreamReceiverWorker* raw = w.get();
        w->thread = std::thread([this, raw]() { RunStreamWorker(raw); });
        {
            std::lock_guard<std::mutex> wl(m_workersMutex);
            m_workers.push_back(std::move(w));
        }
    }

    if (!allListening) {
        StopStreamWorkers();
        ack.accepted = 0;
        ack.errorCode = 2;
        CopyWideSafe(ack.message, _countof(ack.message), L"Stream-Port konnte nicht geoeffnet werden.");
        clientSock.SendPacket(MsgType::SessionAck, &ack, sizeof(ack));
        return;
    }

    if (!m_fileListenSock.Listen(kPortFileTransfer, 2)) {
        Logger::Instance().Warn("FILE-RX", L"Could not listen on file transfer port " + std::to_wstring(kPortFileTransfer));
    }

    ack.accepted = 1;
    CopyWideSafe(ack.message, _countof(ack.message), L"OK");
    if (!clientSock.SendPacket(MsgType::SessionAck, &ack, sizeof(ack))) {
        m_fileListenSock.Close();
        StopStreamWorkers();
        return;
    }

    if (m_fileListenSock.IsValid()) {
        m_fileAcceptThread = std::thread([this]() {
            TcpSocket fsock;
            for (int iter = 0; iter < 50 && !m_stop && !m_sessionStop && m_fileListenSock.IsValid(); ++iter) {
                fsock = m_fileListenSock.Accept(nullptr, 100);
                if (fsock.IsValid()) break;
            }
            m_fileListenSock.Close();
            if (fsock.IsValid() && !m_stop && !m_sessionStop) {
                m_fileTransfer.Start(std::move(fsock));
            }
        });
    }

    {
        std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
        m_activeCtrlSock = std::move(clientSock);
    }
    {
        std::lock_guard<std::mutex> pl(m_peerIpMutex);
        m_activePeerIp = peerIp;
    }
    m_mouseUdpSeq = 0;
    m_mouseBtnSeq = 0;
    m_lastCursorUdpSeq = 0;

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.sessionActive = true;
        m_status.senderName = helloSender.senderName[0] ? std::wstring(helloSender.senderName) : Utf8ToWide(peerIp);
        m_status.senderIp = peerIp;
        m_status.senderVersion = helloSender.senderVersion;
        m_status.localMonitors = mons;
        m_status.localMonitorHdr = hdrList;
        m_status.stateText = L"Verbunden mit " + m_status.senderName + L" (" + Utf8ToWide(peerIp) +
                             L") - " + std::to_wstring(streamCount) + L" Display(s)";
    }

    m_clipboard.SetEnabled(m_clipboardEnabled.load());
    m_clipboard.Start([this](uint32_t kind, uint64_t hash64, const std::vector<uint8_t>& data) {
        SendClipboardData(kind, hash64, data);
    });

    auto lastHeartbeat = std::chrono::steady_clock::now();
    while (!m_stop && !m_sessionStop && m_activeCtrlSock.IsValid()) {
        int wr = m_activeCtrlSock.WaitReadable(50);
        if (wr < 0) {
            Logger::Instance().Warn("RCV-CTL", L"Control socket closed during wait.");
            break;
        }
        if (wr == 0) {
            if (std::chrono::steady_clock::now() - lastHeartbeat > std::chrono::seconds(8)) {
                Logger::Instance().Warn("RCV-CTL", L"Heartbeat timeout from sender.");
                break;
            }
            continue;
        }

        PacketHeader chdr{};
        std::vector<uint8_t> cpayload;
        if (m_activeCtrlSock.RecvPacket(chdr, cpayload)) {
            lastHeartbeat = std::chrono::steady_clock::now();
            if (chdr.type == static_cast<uint16_t>(MsgType::StopSession)) {
                Logger::Instance().Info("RCV-CTL", L"Sender closed session.");
                break;
            } else if (chdr.type == static_cast<uint16_t>(MsgType::ReconfigureStream) &&
                       cpayload.size() >= sizeof(ReconfigureStreamPayload)) {
                ReconfigureStreamPayload rc{};
                std::memcpy(&rc, cpayload.data(), sizeof(rc));
                Logger::Instance().Info("RCV-CTL",
                    L"Live stream reconfigure stream=" + std::to_wstring(rc.streamIndex) +
                    L" codec=" + std::to_wstring(rc.codec) +
                    L" bitrate=" + std::to_wstring(rc.bitrateMbps) + L" Mbit/s" +
                    L" fps=" + std::to_wstring(rc.targetFps));
                std::lock_guard<std::mutex> wl(m_workersMutex);
                for (auto& w : m_workers) {
                    if (w->monitorIndex == rc.streamIndex && rc.targetFps >= 15 && rc.targetFps <= 240) {
                        w->desiredTargetFps.store(rc.targetFps);
                    }
                }
            } else if (chdr.type == static_cast<uint16_t>(MsgType::ClipboardSync) &&
                       cpayload.size() >= sizeof(ClipboardHeader)) {
                ClipboardHeader ch{};
                std::memcpy(&ch, cpayload.data(), sizeof(ch));
                if (ch.dataSize > 0 && ch.dataSize <= kMaxClipboardBytes &&
                    cpayload.size() >= sizeof(ch) + ch.dataSize) {
                    m_clipboard.ApplyRemote(ch.kind, ch.hash64, cpayload.data() + sizeof(ch), ch.dataSize);
                }
            }
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT) {
                Logger::Instance().Warn("RCV-CTL", L"Control connection closed, wsa=" + std::to_wstring(err));
                break;
            }
        }
    }

    DisconnectSession();
}

void ReceiverEngine::SendClipboardData(uint32_t kind, uint64_t hash64, const std::vector<uint8_t>& data) {
    if (!m_clipboardEnabled.load() || m_sessionStop || data.empty() || data.size() > kMaxClipboardBytes) return;
    std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
    if (!m_activeCtrlSock.IsValid()) return;
    ClipboardHeader ch{ kind, hash64, static_cast<uint32_t>(data.size()) };
    std::vector<uint8_t> pkt(sizeof(ch) + data.size());
    std::memcpy(pkt.data(), &ch, sizeof(ch));
    std::memcpy(pkt.data() + sizeof(ch), data.data(), data.size());
    m_activeCtrlSock.SendPacket(MsgType::ClipboardSync, pkt.data(), static_cast<uint32_t>(pkt.size()));
}

void ReceiverEngine::SendKvmMouse(const KvmMouseEvent& ev) {
    if (!m_kvmEnabled.load() || m_sessionStop) return;

    std::string peerIp;
    {
        std::lock_guard<std::mutex> pl(m_peerIpMutex);
        peerIp = m_activePeerIp;
    }

    if (ev.action == kMouseMove) {
        if (m_realtimeUdpSock.IsValid() && !peerIp.empty()) {
            struct {
                PacketHeader hdr;
                KvmMouseEvent body;
            } pkt{};
            pkt.hdr = MakeHeader(MsgType::KvmMouse, sizeof(KvmMouseEvent),
                                 ++m_mouseUdpSeq);
            pkt.body = ev;
            m_realtimeUdpSock.SendTo(peerIp, kPortRealtimeUdp, &pkt, sizeof(pkt));
            return;
        }
        std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
        if (!m_activeCtrlSock.IsValid()) return;
        m_activeCtrlSock.SendPacket(MsgType::KvmMouse, &ev, sizeof(ev));
        return;
    }

    uint32_t seq = ++m_mouseBtnSeq;
    if (m_realtimeUdpSock.IsValid() && !peerIp.empty()) {
        struct {
            PacketHeader hdr;
            KvmMouseEvent body;
        } pkt{};
        pkt.hdr = MakeHeader(MsgType::KvmMouse, sizeof(KvmMouseEvent), seq);
        pkt.body = ev;
        m_realtimeUdpSock.SendTo(peerIp, kPortRealtimeUdp, &pkt, sizeof(pkt));
    }
    std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
    if (!m_activeCtrlSock.IsValid()) return;
    m_activeCtrlSock.SendPacket(MsgType::KvmMouse, &ev, sizeof(ev), seq);
}

void ReceiverEngine::SendKvmKey(const KvmKeyEvent& ev) {
    if (!m_kvmEnabled.load() || m_sessionStop) return;
    std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
    if (!m_activeCtrlSock.IsValid()) return;
    m_activeCtrlSock.SendPacket(MsgType::KvmKey, &ev, sizeof(ev));
}

void ReceiverEngine::RequestKeyframe(uint32_t monitorIndex) {
    std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
    if (!m_activeCtrlSock.IsValid()) return;
    RequestKeyframePayload req{ monitorIndex };
    m_activeCtrlSock.SendPacket(MsgType::RequestKeyframe, &req, sizeof(req));
}

void ReceiverEngine::RunStreamWorker(StreamReceiverWorker* w) {
    ComScope comScope;

    TcpSocket sock;
    for (int iter = 0; iter < 50 && !m_stop && !m_sessionStop && !w->stop && w->listenSock.IsValid(); ++iter) {
        sock = w->listenSock.Accept(nullptr, 100);
        if (sock.IsValid()) break;
    }
    w->listenSock.Close();
    if (!sock.IsValid()) {
        Logger::Instance().Warn("RCV-STRM", L"Timed out or canceled waiting for stream connection on port " + std::to_wstring(w->port));
        return;
    }
    // Asymmetric low-latency stream socket (small SNDBUF, tight 128KB RCVBUF to prevent multi-frame queue bloat)
    sock.TuneLowLatency(64 * 1024, 128 * 1024);
    sock.SetTimeouts(3000, 1500);

    const uint32_t colorIdx = std::min<uint32_t>(w->targetMonitorIndex, kMaxMonitors - 1);
    uint64_t lastColorGen = m_colorGeneration[colorIdx].load(std::memory_order_acquire);

    D3D11Renderer renderer;
    renderer.SetColorConfig(GetMonitorColorConfig(colorIdx));
    if (!renderer.Create(w->monitorIndex, w->targetMonitorRect, w->expectedWidth, w->expectedHeight, w->startFullscreen)) {
        Logger::Instance().Error("RCV-RND", L"Failed to create D3D11Renderer for monitor " + std::to_wstring(w->monitorIndex));
        return;
    }

    {
        std::lock_guard<std::mutex> rl(w->rendererPtrMutex);
        w->activeRenderer = &renderer;
    }

    bool lastGlobalKvm = m_kvmEnabled.load();
    renderer.SetKvmEnabled(lastGlobalKvm);
    renderer.SetHudVisible(m_streamHudEnabled.load());
    renderer.SetKvmMouseSpeedPct(m_kvmMouseSpeedPct.load());
    renderer.SetKvmScrollSpeedPct(m_kvmScrollSpeedPct.load());
    renderer.SetKvmCallbacks(
        [this](const KvmMouseEvent& ev) { SendKvmMouse(ev); },
        [this](const KvmKeyEvent& ev) { SendKvmKey(ev); },
        [this, &lastGlobalKvm](bool enabled) {
            lastGlobalKvm = enabled;
            m_kvmEnabled = enabled;
        });
    renderer.SetFileDropCallback([this](const std::vector<std::wstring>& files) {
        SendFiles(files);
    });

    std::atomic<bool> recvStop{ false };
    std::atomic<bool> streamEnded{ false };

    // Dedicated recv + decode worker thread: never stalls on Present(), Win32 resizing, or RawInput
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    std::thread recvDecodeThread([&]() {
        ComScope decodeCom;
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
        LARGE_INTEGER qpcFreqLi{};
        QueryPerformanceFrequency(&qpcFreqLi);
        const int64_t qpcFreq = std::max<int64_t>(1, qpcFreqLi.QuadPart);

        H264Decoder h264;
        uint32_t h264W = 0, h264H = 0;
        uint64_t h264DeviceGen = renderer.DeviceGeneration();
        bool h264InitFailed = false;
        bool decodedAnyH264 = false;
        uint32_t consecutiveH264Fails = 0;
        uint32_t needMoreInputStreak = 0;
        auto lastKeyframeReq = std::chrono::steady_clock::time_point{};
        DeltaDecoder delta;
        std::vector<uint8_t> cpuBgra;

        auto maybeRequestKeyframe = [&]() {
            auto now = std::chrono::steady_clock::now();
            if (lastKeyframeReq.time_since_epoch().count() == 0 ||
                now - lastKeyframeReq >= std::chrono::milliseconds(1000)) {
                RequestKeyframe(w->monitorIndex);
                lastKeyframeReq = now;
            }
        };

        auto windowStart = std::chrono::steady_clock::now();
        uint64_t windowFrames = 0;
        uint64_t windowBytes = 0;
        double windowDecodeMsSum = 0.0;
        uint64_t windowDecodeCount = 0;
        uint64_t totalFrames = 0;

        while (!m_stop && !m_sessionStop && !w->stop && !recvStop && !streamEnded) {
            int wr = sock.WaitReadable(50);
            if (wr < 0) {
                streamEnded.store(true);
                break;
            }
            if (wr == 0) {
                continue;
            }

            // Drain burst of queued packets so we always render the freshest cursor & video frame
            bool pendingDeltaCommit = false;
            bool deltaUnionFull = false;
            uint32_t deltaUx0 = 0, deltaUy0 = 0, deltaUx1 = 0, deltaUy1 = 0;
            uint32_t deltaLastW = 0, deltaLastH = 0;

            bool pendingGpuCommit = false;
            bool pendingGpuIsNv12 = false;
            bool pendingCpuH264Commit = false;
            uint32_t cpuH264W = 0, cpuH264H = 0;

            auto flushPendingBurstFrames = [&]() {
                if (pendingDeltaCommit && delta.Width() > 0 && delta.Height() > 0) {
                    if (deltaUnionFull || deltaUx1 <= deltaUx0 || deltaUy1 <= deltaUy0) {
                        renderer.QueueCpuFrame(delta.FrameData(), delta.StrideBytes(),
                                               delta.Width(), delta.Height());
                    } else {
                        renderer.QueueCpuFrame(delta.FrameData(), delta.StrideBytes(),
                                               delta.Width(), delta.Height(),
                                               deltaUx0, deltaUy0, deltaUx1 - deltaUx0, deltaUy1 - deltaUy0);
                    }
                    pendingDeltaCommit = false;
                    deltaUnionFull = false;
                }
                if (pendingGpuCommit) {
                    {
                        std::lock_guard<std::mutex> gpuLock(renderer.GpuMutex());
                        if (renderer.Context()) {
                            renderer.Context()->Flush();
                        }
                    }
                    renderer.MarkGpuFrameUpdated(pendingGpuIsNv12);
                    pendingGpuCommit = false;
                } else if (pendingCpuH264Commit && !cpuBgra.empty()) {
                    renderer.QueueCpuFrame(cpuBgra.data(), cpuH264W * 4, cpuH264W, cpuH264H);
                    pendingCpuH264Commit = false;
                }
            };

            for (int drained = 0; drained < 8 && wr > 0 && !recvStop && !m_stop && !m_sessionStop && !w->stop; ++drained) {
                PacketHeader hdr{};
                std::vector<uint8_t> payload;
                if (!sock.RecvPacket(hdr, payload)) {
                    streamEnded.store(true);
                    break;
                }

                if (hdr.type == static_cast<uint16_t>(MsgType::StreamEnd)) {
                    streamEnded.store(true);
                    break;
                } else if (hdr.type == static_cast<uint16_t>(MsgType::CursorUpdate) &&
                           payload.size() >= sizeof(CursorUpdateHeader)) {
                    CursorUpdateHeader ch{};
                    std::memcpy(&ch, payload.data(), sizeof(ch));
                    const uint8_t* shapePtr = (ch.dataSize > 0 && payload.size() >= sizeof(ch) + ch.dataSize)
                                                  ? (payload.data() + sizeof(ch))
                                                  : nullptr;
                    renderer.QueueCursorUpdate(ch.visible != 0, ch.x, ch.y, ch.shapeChanged != 0, ch.shapeType,
                                               ch.shapeWidth, ch.shapeHeight, ch.shapePitch, ch.hotX, ch.hotY,
                                               shapePtr, ch.dataSize);
                } else if (hdr.type == static_cast<uint16_t>(MsgType::VideoFrame) &&
                           payload.size() >= sizeof(VideoFrameHeader)) {
                    VideoFrameHeader vh{};
                    std::memcpy(&vh, payload.data(), sizeof(vh));
                    if (payload.size() < sizeof(vh) + vh.dataSize) {
                        wr = sock.WaitReadable(0);
                        continue;
                    }
                    const uint8_t* frameData = payload.data() + sizeof(vh);

                    bool decodedOk = false;
                    std::wstring codecLabel;
                    bool isDxva = false;
                    bool isDirectNv12 = false;
                    bool needKeyframeNow = false;
                    bool needKeyframeThrottled = false;

                    LARGE_INTEGER decStartLi{}, decEndLi{};
                    QueryPerformanceCounter(&decStartLi);

                    if (vh.codec == static_cast<uint8_t>(CodecType::H264)) {
                        if (pendingDeltaCommit) {
                            flushPendingBurstFrames();
                        }
                        uint32_t outW = vh.width, outH = vh.height;
                        bool pushCpuFrame = false;
                        bool pushGpuFrame = false;
                        bool decodedToNv12 = false;

                        {
                            std::unique_lock<std::mutex> gpuLock(renderer.GpuMutex());
                            uint64_t curGen = renderer.DeviceGeneration();
                            if (curGen != h264DeviceGen) {
                                h264.Shutdown();
                                h264DeviceGen = curGen;
                                h264W = 0;
                                h264H = 0;
                                h264InitFailed = false;
                                decodedAnyH264 = false;
                                consecutiveH264Fails = 0;
                                needMoreInputStreak = 0;
                                delta.Reset();
                            }

                            if (!renderer.Device() || !renderer.Context()) {
                                needKeyframeThrottled = true;
                            } else {
                                if (h264W != vh.width || h264H != vh.height) {
                                    h264.Shutdown();
                                    h264W = vh.width;
                                    h264H = vh.height;
                                    h264InitFailed = false;
                                    decodedAnyH264 = false;
                                    consecutiveH264Fails = 0;
                                    needMoreInputStreak = 0;
                                }

                                if (!h264.IsInitialized() && !h264InitFailed) {
                                    uint32_t fpsHint = w->desiredTargetFps.load();
                                    bool initOk = h264.Init(renderer.Device(), renderer.Context(),
                                                            vh.width, vh.height, true, fpsHint);
                                    if (!initOk) {
                                        initOk = h264.Init(renderer.Device(), renderer.Context(),
                                                           vh.width, vh.height, false, fpsHint);
                                    }
                                    if (!initOk) {
                                        h264InitFailed = true;
                                    } else {
                                        renderer.EnsureFrameTexture(vh.width, vh.height);
                                    }
                                }

                                if (h264.IsInitialized()) {
                                    if (h264.IsHardwareDxva()) {
                                        if (renderer.EnsureFrameTexture(vh.width, vh.height)) {
                                            ID3D11Texture2D* nv12Target = renderer.HasNv12Pipeline() ? renderer.Nv12Texture() : nullptr;
                                            decodedOk = h264.Decode(frameData, vh.dataSize, vh.timestampUs,
                                                                    nv12Target ? nullptr : renderer.FrameTexture(),
                                                                    nullptr, &outW, &outH,
                                                                    nv12Target, &decodedToNv12);
                                            if (decodedOk) {
                                                pushGpuFrame = true;
                                            }
                                        }
                                        codecLabel = decodedToNv12 ? L"H.264 (NVDEC Direct)" : L"H.264 (NVDEC/DXVA)";
                                        isDxva = true;
                                        isDirectNv12 = decodedToNv12;
                                    } else {
                                        decodedOk = h264.Decode(frameData, vh.dataSize, vh.timestampUs,
                                                                nullptr, &cpuBgra, &outW, &outH);
                                        if (decodedOk && !cpuBgra.empty()) {
                                            pushCpuFrame = true;
                                        }
                                        codecLabel = L"H.264 (CPU)";
                                    }

                                    if (decodedOk) {
                                        decodedAnyH264 = true;
                                        consecutiveH264Fails = 0;
                                        needMoreInputStreak = 0;
                                    } else if (h264.WasLastCallNeedMoreInput() && ++needMoreInputStreak <= 25) {
                                        // Normal DPB warmup on initial frames; do not trigger DXVA->CPU fallback
                                    } else {
                                        ++consecutiveH264Fails;
                                        if (!decodedAnyH264 && h264.IsHardwareDxva() && consecutiveH264Fails >= 4) {
                                            Logger::Instance().Warn("RCV-DEC",
                                                L"DXVA H.264 decode failed at runtime; falling back to CPU H.264 decoder.");
                                            uint32_t fpsHint = w->desiredTargetFps.load();
                                            if (!h264.Init(renderer.Device(), renderer.Context(),
                                                           vh.width, vh.height, false, fpsHint)) {
                                                h264InitFailed = true;
                                            }
                                            consecutiveH264Fails = 0;
                                            needMoreInputStreak = 0;
                                            needKeyframeNow = true;
                                        } else if (consecutiveH264Fails >= 2) {
                                            needKeyframeThrottled = true;
                                        }
                                    }
                                } else {
                                    needKeyframeThrottled = true;
                                }
                            }
                        } // Unlock m_gpuMutex before queuing/waking/network I/O

                        if (pushGpuFrame) {
                            pendingGpuCommit = true;
                            pendingGpuIsNv12 = decodedToNv12;
                            pendingCpuH264Commit = false;
                        } else if (pushCpuFrame) {
                            pendingCpuH264Commit = true;
                            cpuH264W = outW;
                            cpuH264H = outH;
                            pendingGpuCommit = false;
                        }
                        if (needKeyframeNow) {
                            RequestKeyframe(w->monitorIndex);
                            lastKeyframeReq = std::chrono::steady_clock::now();
                        } else if (needKeyframeThrottled) {
                            maybeRequestKeyframe();
                        }
                    } else if (vh.codec == static_cast<uint8_t>(CodecType::DeltaLossless)) {
                        if (pendingGpuCommit || pendingCpuH264Commit) {
                            flushPendingBurstFrames();
                        }
                        if (delta.Decode(frameData, vh.dataSize, vh.width, vh.height)) {
                            const DeltaRect& bounds = delta.LastBoundingRect();
                            if (!pendingDeltaCommit || delta.Width() != deltaLastW || delta.Height() != deltaLastH) {
                                deltaLastW = delta.Width();
                                deltaLastH = delta.Height();
                                deltaUnionFull = (bounds.w == 0 || bounds.h == 0);
                                deltaUx0 = bounds.x;
                                deltaUy0 = bounds.y;
                                deltaUx1 = std::min(deltaLastW, bounds.x + bounds.w);
                                deltaUy1 = std::min(deltaLastH, bounds.y + bounds.h);
                            } else if (bounds.w == 0 || bounds.h == 0) {
                                deltaUnionFull = true;
                            } else if (!deltaUnionFull) {
                                deltaUx0 = std::min(deltaUx0, bounds.x);
                                deltaUy0 = std::min(deltaUy0, bounds.y);
                                deltaUx1 = std::max(deltaUx1, std::min(deltaLastW, bounds.x + bounds.w));
                                deltaUy1 = std::max(deltaUy1, std::min(deltaLastH, bounds.y + bounds.h));
                            }
                            pendingDeltaCommit = true;
                            decodedOk = true;
                            codecLabel = delta.LastDecodeUsedFastMotion() ? L"Delta Fast-Motion" : L"Delta Lossless";
                        } else {
                            maybeRequestKeyframe();
                        }
                    }

                    QueryPerformanceCounter(&decEndLi);

                    if (decodedOk) {
                        double decMs = static_cast<double>(decEndLi.QuadPart - decStartLi.QuadPart) * 1000.0 / static_cast<double>(qpcFreq);
                        windowDecodeMsSum += decMs;
                        ++windowDecodeCount;
                        ++totalFrames;
                        ++windowFrames;
                        windowBytes += payload.size() + sizeof(PacketHeader);

                        auto now = std::chrono::steady_clock::now();
                        double elapsed = std::chrono::duration<double>(now - windowStart).count();
                        if (elapsed >= 0.5) {
                            std::lock_guard<std::mutex> sl(w->statsMutex);
                            w->stats.width = vh.width;
                            w->stats.height = vh.height;
                            w->stats.fps = static_cast<double>(windowFrames) / elapsed;
                            w->stats.mbps = (static_cast<double>(windowBytes) * 8.0) / (elapsed * 1000000.0);
                            w->stats.framesReceived = totalFrames;
                            w->stats.codecName = codecLabel;
                            w->stats.hardwareDxva = isDxva;
                            w->stats.directNv12Active = isDirectNv12;
                            w->stats.avgDecodeMs = windowDecodeCount > 0 ? (windowDecodeMsSum / static_cast<double>(windowDecodeCount)) : 0.0;
                            renderer.SetHudStats(w->stats.fps, w->stats.mbps, w->stats.avgDecodeMs, w->stats.codecName);
                            windowStart = now;
                            windowFrames = 0;
                            windowBytes = 0;
                            windowDecodeMsSum = 0.0;
                            windowDecodeCount = 0;
                        }
                    }
                }

                wr = sock.WaitReadable(0);
                if (wr < 0) {
                    streamEnded.store(true);
                    break;
                }
            }
            flushPendingBurstFrames();
        }

        {
            std::lock_guard<std::mutex> gpuLock(renderer.GpuMutex());
            h264.Shutdown();
        }
        renderer.SignalWake();
    });

    // Dedicated UI/Render loop on window-owning thread
    while (!m_stop && !m_sessionStop && !w->stop && !streamEnded) {
        if (w->showRequested.exchange(false, std::memory_order_acquire)) {
            renderer.ShowFromHidden();
        }
        w->hiddenByUser.store(renderer.IsHiddenByUser(), std::memory_order_release);

        bool curGlobalKvm = m_kvmEnabled.load();
        if (curGlobalKvm != lastGlobalKvm) {
            lastGlobalKvm = curGlobalKvm;
            renderer.SetKvmEnabled(curGlobalKvm);
        }
        uint64_t curColorGen = m_colorGeneration[colorIdx].load(std::memory_order_acquire);
        if (curColorGen != lastColorGen) {
            lastColorGen = curColorGen;
            renderer.SetColorConfig(GetMonitorColorConfig(colorIdx));
        }

        renderer.PumpMessages();

        bool doPresent = renderer.ConsumePendingWork() || renderer.NeedsRepaint();
        renderer.ClearRepaint();
        if (doPresent) {
            renderer.Present();
        }

        if (renderer.ConsumeDeviceLost()) {
            if (!renderer.RecreateDeviceAndResources()) {
                Logger::Instance().Error("RCV-D3D", L"Failed recreating D3D11 device after device lost.");
                break;
            }
            RequestKeyframe(w->monitorIndex);
        }

        if (!renderer.NeedsRepaint() && !w->showRequested.load() &&
            !streamEnded.load() && !m_stop && !m_sessionStop && !w->stop) {
            HANDLE wake = renderer.WakeHandle();
            if (wake) {
                MsgWaitForMultipleObjectsEx(1, &wake, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            } else {
                MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            }
        }
    }

    recvStop.store(true);
    sock.Close();
    if (recvDecodeThread.joinable()) {
        recvDecodeThread.join();
    }

    {
        std::lock_guard<std::mutex> rl(w->rendererPtrMutex);
        w->activeRenderer = nullptr;
    }
    w->hiddenByUser.store(false);
    renderer.Destroy();
}

void ReceiverEngine::StopStreamWorkers() {
    std::vector<std::unique_ptr<StreamReceiverWorker>> toJoin;
    {
        std::lock_guard<std::mutex> wl(m_workersMutex);
        for (auto& w : m_workers) {
            w->stop = true;
            w->listenSock.Close();
            std::lock_guard<std::mutex> rl(w->rendererPtrMutex);
            if (w->activeRenderer) {
                w->activeRenderer->SignalWake();
            }
        }
        toJoin.swap(m_workers);
    }
    for (auto& w : toJoin) {
        if (w->thread.joinable()) w->thread.join();
    }
}

void ReceiverEngine::DisconnectSession() {
    m_sessionStop = true;
    std::lock_guard<std::mutex> dl(m_disconnectMutex);
    m_clipboard.Stop();
    m_fileListenSock.Close();
    if (m_fileAcceptThread.joinable()) m_fileAcceptThread.join();
    m_fileTransfer.Stop();
    {
        std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
        if (m_activeCtrlSock.IsValid()) {
            m_activeCtrlSock.SendPacket(MsgType::StopSession, nullptr, 0);
            m_activeCtrlSock.Close();
        }
    }
    StopStreamWorkers();
    {
        std::lock_guard<std::mutex> pl(m_peerIpMutex);
        m_activePeerIp.clear();
    }

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_status.sessionActive = false;
    m_status.hasHiddenStreams = false;
    m_status.senderName.clear();
    m_status.senderIp.clear();
    m_status.senderVersion = AppVersionTuple{};
    m_status.streams.clear();
    if (m_running) {
        m_status.stateText = L"Bereit - Warte auf Verbindung vom Laptop (Port " +
                             std::to_wstring(kPortControl) + L")";
    }
}

ReceiverStatus ReceiverEngine::GetStatus() const {
    ReceiverStatus st;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        st = m_status;
    }
    st.fileStats = m_fileTransfer.GetStats();
    st.streams.clear();
    st.hasHiddenStreams = false;
    std::lock_guard<std::mutex> wl(m_workersMutex);
    for (const auto& w : m_workers) {
        ReceiverStreamStats rs;
        {
            std::lock_guard<std::mutex> sl(w->statsMutex);
            rs = w->stats;
        }
        rs.hiddenByUser = w->hiddenByUser.load(std::memory_order_acquire);
        if (rs.hiddenByUser) {
            st.hasHiddenStreams = true;
        }
        st.streams.push_back(std::move(rs));
    }
    return st;
}

} // namespace s2p
