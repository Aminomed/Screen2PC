#include "SenderEngine.h"
#include "VirtualDisplayCtl.h"
#include "../common/Logger.h"

#include <algorithm>
#include <chrono>
#include <set>

namespace s2p {

namespace {

uint64_t NowUs() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

void CopyWideSafe(wchar_t* dst, size_t dstChars, const std::wstring& src) {
    if (!dst || dstChars == 0) return;
    wcsncpy_s(dst, dstChars, src.c_str(), _TRUNCATE);
}

} // namespace

SenderEngine::~SenderEngine() {
    Disconnect();
}

std::vector<DiscoveredPeer> SenderEngine::DiscoverPeers(uint32_t timeoutMs) {
    std::vector<DiscoveredPeer> peers;
    UdpSocket udp;
    if (!udp.CreateForBroadcast()) {
        return peers;
    }

    DiscoverReqPayload req{};
    req.senderNonce = static_cast<uint32_t>(GetTickCount64() & 0xFFFFFFFFu);
    CopyWideSafe(req.senderName, _countof(req.senderName), GetHostComputerName());

    struct {
        PacketHeader hdr;
        DiscoverReqPayload body;
    } pkt{};
    pkt.hdr = MakeHeader(MsgType::DiscoverReq, sizeof(DiscoverReqPayload));
    pkt.body = req;

    udp.Broadcast(kPortDiscovery, &pkt, sizeof(pkt));

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::set<std::string> seenIps;
    uint8_t buf[4096]{};

    while (std::chrono::steady_clock::now() < deadline) {
        auto remMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
        if (remMs <= 0) break;

        std::string senderIp;
        uint16_t senderPort = 0;
        int n = udp.RecvFrom(buf, sizeof(buf), senderIp, senderPort, std::min(remMs, 150));
        if (n <= 0) continue;
        if (static_cast<size_t>(n) < sizeof(PacketHeader) + sizeof(DiscoverRespPayload)) continue;

        PacketHeader hdr{};
        std::memcpy(&hdr, buf, sizeof(hdr));
        if (!ValidateHeader(hdr) || hdr.type != static_cast<uint16_t>(MsgType::DiscoverResp)) continue;

        DiscoverRespPayload resp{};
        std::memcpy(&resp, buf + sizeof(PacketHeader), sizeof(resp));
        if (resp.senderNonce != req.senderNonce) continue;
        if (!seenIps.insert(senderIp).second) continue;

        DiscoveredPeer peer;
        peer.ip = senderIp;
        resp.pcName[_countof(resp.pcName) - 1] = L'\0';
        peer.pcName = resp.pcName;
        peer.controlPort = resp.controlPort ? resp.controlPort : kPortControl;
        peer.streamBasePort = resp.streamBasePort ? resp.streamBasePort : kPortStreamBase;

        uint32_t cnt = std::min<uint32_t>(resp.monitorCount, static_cast<uint32_t>(kMaxMonitors));
        for (uint32_t i = 0; i < cnt; ++i) {
            resp.monitors[i].friendlyName[_countof(resp.monitors[i].friendlyName) - 1] = L'\0';
            peer.monitors.push_back(resp.monitors[i]);
        }
        peers.push_back(std::move(peer));
    }

    return peers;
}

bool SenderEngine::Connect(const SenderConfig& cfg) {
    Disconnect();

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_cfg = cfg;
        m_status = SenderStatus{};
        m_status.connecting = true;
        m_status.driverActive = VirtualDisplayCtl::IsDriverActive();
        m_status.peerIp = cfg.targetIp;
        m_status.stateText = L"Verbinde mit PC...";
    }

    m_stop = false;
    m_connected = false;
    m_createdVirtualDisplays = false;

    if (!m_controlSock.Connect(cfg.targetIp, cfg.controlPort, 3000)) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.stateText = L"Verbindung fehlgeschlagen";
        m_status.lastError = L"TCP-Verbindung zum PC (" + Utf8ToWide(cfg.targetIp) + L":" +
                             std::to_wstring(cfg.controlPort) + L") fehlgeschlagen.";
        Logger::Instance().Error("SND-CONN", m_status.lastError);
        return false;
    }

    m_controlSock.TuneLowLatency(1024 * 1024);
    m_controlSock.SetTimeouts(3000, 5000);

    HelloSenderPayload helloReq{};
    CopyWideSafe(helloReq.senderName, _countof(helloReq.senderName), GetHostComputerName());
    helloReq.preferredCodec = static_cast<uint8_t>(cfg.codec);
    helloReq.targetBitrateMbps = cfg.bitrateMbps;
    helloReq.targetFps = cfg.targetFps;

    if (!m_controlSock.SendPacket(MsgType::HelloSender, &helloReq, sizeof(helloReq))) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.lastError = L"Handshake senden fehlgeschlagen.";
        m_controlSock.Close();
        return false;
    }

    PacketHeader hdr{};
    std::vector<uint8_t> payload;
    if (!m_controlSock.RecvPacket(hdr, payload) ||
        hdr.type != static_cast<uint16_t>(MsgType::HelloReceiver) ||
        payload.size() < sizeof(HelloReceiverPayload)) {
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
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.lastError = L"PC meldet 0 Monitore.";
        m_controlSock.Close();
        return false;
    }

    std::vector<DisplayOutputInfo> captureOutputs;
    std::wstring vddErr;
    if (!SetupVirtualDisplays(helloResp, cfg, captureOutputs, vddErr)) {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.stateText = L"Display-Setup fehlgeschlagen";
        m_status.lastError = vddErr;
        m_controlSock.Close();
        return false;
    }

    StartSessionPayload startReq{};
    uint32_t streamCount = static_cast<uint32_t>(std::min<size_t>(captureOutputs.size(), kMaxMonitors));
    startReq.streamCount = streamCount;
    for (uint32_t i = 0; i < streamCount; ++i) {
        startReq.streams[i].monitorIndex = i;
        startReq.streams[i].width = captureOutputs[i].width;
        startReq.streams[i].height = captureOutputs[i].height;
        startReq.streams[i].refreshHz = (helloResp.monitors[i].refreshHz > 0) ? helloResp.monitors[i].refreshHz : cfg.targetFps;
        startReq.streams[i].codec = static_cast<uint8_t>(cfg.codec);
        startReq.streams[i].bitrateMbps = cfg.bitrateMbps;
    }

    if (!m_controlSock.SendPacket(MsgType::StartSession, &startReq, sizeof(startReq))) {
        Disconnect();
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.lastError = L"StartSession konnte nicht gesendet werden.";
        return false;
    }

    if (!m_controlSock.RecvPacket(hdr, payload) ||
        hdr.type != static_cast<uint16_t>(MsgType::SessionAck) ||
        payload.size() < sizeof(SessionAckPayload)) {
        Disconnect();
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.lastError = L"Keine SessionAck-Antwort vom PC erhalten.";
        return false;
    }

    SessionAckPayload ack{};
    std::memcpy(&ack, payload.data(), sizeof(ack));
    ack.message[_countof(ack.message) - 1] = L'\0';
    if (!ack.accepted) {
        Disconnect();
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.lastError = std::wstring(L"PC hat Session abgelehnt: ") + ack.message;
        return false;
    }

    // Set shorter recv timeout for non-blocking control loop checks
    m_controlSock.SetTimeouts(2000, 300);

    // Spawn per-monitor capture/encode/stream workers
    for (uint32_t i = 0; i < streamCount; ++i) {
        auto w = std::make_unique<StreamWorker>();
        w->targetPcMonitor = i;
        w->adapterIndex = captureOutputs[i].adapterIndex;
        w->outputIndex = captureOutputs[i].outputIndex;
        w->width = captureOutputs[i].width;
        w->height = captureOutputs[i].height;
        w->refreshHz = startReq.streams[i].refreshHz;
        w->codec = cfg.codec;
        w->bitrateMbps = cfg.bitrateMbps;
        w->targetFps = cfg.targetFps;
        w->peerIp = cfg.targetIp;
        w->streamPort = static_cast<uint16_t>(kPortStreamBase + i);
        w->stats.monitorIndex = i;
        w->stats.width = w->width;
        w->stats.height = w->height;

        StreamWorker* raw = w.get();
        w->thread = std::thread([this, raw]() { RunStreamWorker(raw); });
        m_workers.push_back(std::move(w));
    }

    m_connected = true;
    m_controlThread = std::thread(&SenderEngine::ControlLoop, this);

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.connecting = false;
        m_status.connected = true;
        m_status.peerName = helloResp.pcName;
        m_status.peerIp = cfg.targetIp;
        m_status.stateText = L"Verbunden mit " + m_status.peerName + L" (" +
                             std::to_wstring(streamCount) + L" Monitor(e))";
    }

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

    std::vector<vdd::VddMonitorSpec> specs;
    for (uint32_t i = 0; i < desiredCount; ++i) {
        vdd::VddMonitorSpec s{};
        s.width = hello.monitors[i].width ? hello.monitors[i].width : 1920;
        s.height = hello.monitors[i].height ? hello.monitors[i].height : 1080;
        s.refreshHz = hello.monitors[i].refreshHz ? hello.monitors[i].refreshHz : cfg.targetFps;
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
    while (std::chrono::steady_clock::now() < deadline && !m_stop) {
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
            // Order candidates to match requested spec resolutions as closely as possible
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

        Sleep(200);
    }

    outError = L"Virtuelle Monitore wurden unter Windows innerhalb von 5s nicht sichtbar. Pruefe Windows-Anzeigeeinstellungen (Desktop erweitern).";
    VirtualDisplayCtl::ClearMonitors();
    m_createdVirtualDisplays = false;
    return false;
}

void SenderEngine::ControlLoop() {
    auto lastHeartbeat = std::chrono::steady_clock::now();
    while (!m_stop && m_controlSock.IsValid()) {
        PacketHeader hdr{};
        std::vector<uint8_t> payload;
        if (m_controlSock.RecvPacket(hdr, payload)) {
            if (hdr.type == static_cast<uint16_t>(MsgType::RequestKeyframe) &&
                payload.size() >= sizeof(RequestKeyframePayload)) {
                RequestKeyframePayload req{};
                std::memcpy(&req, payload.data(), sizeof(req));
                if (req.monitorIndex < m_workers.size()) {
                    m_workers[req.monitorIndex]->forceKeyframe = true;
                }
            } else if (hdr.type == static_cast<uint16_t>(MsgType::StopSession)) {
                Logger::Instance().Info("SND-CTL", L"Receiver requested StopSession.");
                m_stop = true;
                break;
            }
        } else {
            // Timeout or socket error? Only break if socket disconnected or fatal
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT && err != 0) {
                Logger::Instance().Warn("SND-CTL", L"Control socket closed, wsa=" + std::to_wstring(err));
                m_stop = true;
                break;
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

    m_connected = false;
    std::lock_guard<std::mutex> lock(m_stateMutex);
    if (m_status.connected) {
        m_status.connected = false;
        m_status.stateText = L"Getrennt";
    }
}

void SenderEngine::RunStreamWorker(StreamWorker* w) {
    Logger::Instance().Info("SND-WRK", L"Starting worker for monitor " + std::to_wstring(w->targetPcMonitor) +
                            L" (" + std::to_wstring(w->width) + L"x" + std::to_wstring(w->height) + L")");

    TcpSocket sock;
    bool connected = false;
    for (int attempt = 0; attempt < 10 && !m_stop && !w->stop; ++attempt) {
        if (sock.Connect(w->peerIp, w->streamPort, 1500)) {
            connected = true;
            break;
        }
        Sleep(150);
    }
    if (!connected) {
        Logger::Instance().Error("SND-WRK", L"Failed to connect stream socket on port " + std::to_wstring(w->streamPort));
        return;
    }
    sock.TuneLowLatency(4 * 1024 * 1024);
    sock.SetTimeouts(4000, 4000);

    DxgiCapture capture;
    if (!capture.Init(w->adapterIndex, w->outputIndex)) {
        Logger::Instance().Error("SND-WRK", L"DxgiCapture::Init failed for adapter=" +
                                 std::to_wstring(w->adapterIndex) + L" output=" + std::to_wstring(w->outputIndex));
        return;
    }

    CodecType activeCodec = w->codec;
    if (activeCodec == CodecType::Auto) activeCodec = CodecType::H264;

    H264Encoder h264;
    bool h264Ok = false;
    if (activeCodec == CodecType::H264) {
        h264Ok = h264.Init(capture.Device(), capture.Context(), w->width, w->height, w->targetFps, w->bitrateMbps);
        if (!h264Ok) {
            Logger::Instance().Warn("SND-WRK", L"H264Encoder init failed; falling back to DeltaLossless.");
            activeCodec = CodecType::DeltaLossless;
        }
    }

    DeltaEncoder delta;
    std::vector<uint8_t> encodedPayload;
    std::vector<uint8_t> cpuBgra;
    uint32_t cpuStride = 0;
    uint64_t frameIndex = 0;

    {
        std::lock_guard<std::mutex> sl(w->statsMutex);
        if (activeCodec == CodecType::H264) {
            w->stats.codecName = h264.IsHardware() ? L"H.264 (GPU)" : L"H.264 (CPU)";
            w->stats.hardwareAccelerated = h264.IsHardware();
        } else {
            w->stats.codecName = L"Delta Lossless";
            w->stats.hardwareAccelerated = false;
        }
    }

    const uint32_t frameIntervalMs = std::max<uint32_t>(4u, 1000u / std::max<uint32_t>(15u, w->targetFps));
    auto windowStart = std::chrono::steady_clock::now();
    uint64_t windowFrames = 0;
    uint64_t windowBytes = 0;
    bool sentInitialFrame = false;

    while (!m_stop && !w->stop) {
        auto frameStart = std::chrono::steady_clock::now();

        bool hasNewImage = false;
        std::vector<DeltaRect> dirtyRects;
        CapturedCursor cursor;
        if (!capture.AcquireFrame(frameIntervalMs, hasNewImage, dirtyRects, cursor)) {
            Sleep(50);
            continue;
        }

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

            std::vector<uint8_t> pkt(sizeof(CursorUpdateHeader) + ch.dataSize);
            std::memcpy(pkt.data(), &ch, sizeof(CursorUpdateHeader));
            if (ch.dataSize > 0) {
                std::memcpy(pkt.data() + sizeof(CursorUpdateHeader), cursor.shapeData.data(), ch.dataSize);
            }
            if (!sock.SendPacket(MsgType::CursorUpdate, pkt.data(), static_cast<uint32_t>(pkt.size()))) {
                break;
            }
        }

        bool needKey = w->forceKeyframe.exchange(false) || !sentInitialFrame;
        if (hasNewImage || needKey) {
            encodedPayload.clear();
            bool isKeyframe = false;
            bool encodeOk = false;

            if (activeCodec == CodecType::H264) {
                if (needKey) h264.RequestKeyframe();
                encodeOk = h264.EncodeTexture(capture.FrameTexture(), NowUs(), encodedPayload, isKeyframe);
                if (!encodeOk && !h264.IsHardware()) {
                    // Nothing yet if priming
                }
            } else {
                if (capture.CopyFrameToCpu(cpuBgra, cpuStride)) {
                    encodeOk = delta.Encode(cpuBgra.data(), capture.Width(), capture.Height(),
                                            cpuStride, dirtyRects, needKey, encodedPayload, isKeyframe);
                }
            }

            if (encodeOk && !encodedPayload.empty()) {
                VideoFrameHeader vh{};
                vh.monitorIndex = w->targetPcMonitor;
                vh.frameIndex = ++frameIndex;
                vh.timestampUs = NowUs();
                vh.width = capture.Width();
                vh.height = capture.Height();
                vh.codec = static_cast<uint8_t>(activeCodec);
                vh.flags = (isKeyframe ? kFrameFlagKeyframe : kFrameFlagNone);
                vh.dataSize = static_cast<uint32_t>(encodedPayload.size());

                std::vector<uint8_t> pkt(sizeof(VideoFrameHeader) + encodedPayload.size());
                std::memcpy(pkt.data(), &vh, sizeof(VideoFrameHeader));
                std::memcpy(pkt.data() + sizeof(VideoFrameHeader), encodedPayload.data(), encodedPayload.size());

                if (!sock.SendPacket(MsgType::VideoFrame, pkt.data(), static_cast<uint32_t>(pkt.size()))) {
                    Logger::Instance().Warn("SND-WRK", L"SendPacket(VideoFrame) failed on monitor " + std::to_wstring(w->targetPcMonitor));
                    break;
                }
                sentInitialFrame = true;
                ++windowFrames;
                windowBytes += pkt.size() + sizeof(PacketHeader);
            }
        }

        auto now = std::chrono::steady_clock::now();
        double elapsedSec = std::chrono::duration<double>(now - windowStart).count();
        if (elapsedSec >= 0.5) {
            std::lock_guard<std::mutex> sl(w->statsMutex);
            w->stats.fps = static_cast<double>(windowFrames) / elapsedSec;
            w->stats.mbps = (static_cast<double>(windowBytes) * 8.0) / (elapsedSec * 1000000.0);
            w->stats.framesSent = frameIndex;
            windowStart = now;
            windowFrames = 0;
            windowBytes = 0;
        }

        auto spentMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - frameStart).count();
        if (spentMs < static_cast<long long>(frameIntervalMs) && !hasNewImage) {
            Sleep(static_cast<DWORD>(frameIntervalMs - spentMs));
        }
    }

    if (sock.IsValid()) {
        sock.SendPacket(MsgType::StreamEnd, nullptr, 0);
        sock.Close();
    }
    capture.Shutdown();
    h264.Shutdown();
}

void SenderEngine::Disconnect() {
    m_stop = true;
    m_connected = false;

    if (m_controlSock.IsValid()) {
        {
            std::lock_guard<std::mutex> lock(m_controlSendMutex);
            m_controlSock.SendPacket(MsgType::StopSession, nullptr, 0);
        }
        m_controlSock.Close();
    }

    for (auto& w : m_workers) {
        w->stop = true;
    }
    for (auto& w : m_workers) {
        if (w->thread.joinable()) w->thread.join();
    }
    m_workers.clear();

    if (m_controlThread.joinable()) {
        m_controlThread.join();
    }

    if (m_createdVirtualDisplays) {
        VirtualDisplayCtl::ClearMonitors();
        m_createdVirtualDisplays = false;
    }

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_status.connected = false;
    m_status.connecting = false;
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
    s.driverActive = VirtualDisplayCtl::IsDriverActive();
    s.streams.clear();
    for (const auto& w : m_workers) {
        std::lock_guard<std::mutex> sl(w->statsMutex);
        s.streams.push_back(w->stats);
    }
    return s;
}

} // namespace s2p
