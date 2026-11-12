#include "ReceiverEngine.h"
#include "D3D11Renderer.h"
#include "../common/CodecH264.h"
#include "../common/CodecDelta.h"
#include "../common/Logger.h"

#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace s2p {

namespace {

BOOL CALLBACK MonitorEnumCb(HMONITOR hMon, HDC, LPRECT, LPARAM data) {
    auto* list = reinterpret_cast<std::vector<MonitorDesc>*>(data);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

    MonitorDesc md{};
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
    if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency >= 24) {
        md.refreshHz = dm.dmDisplayFrequency;
    }

    wcsncpy_s(md.friendlyName, _countof(md.friendlyName), mi.szDevice, _TRUNCATE);
    list->push_back(md);
    return TRUE;
}

void CopyWideSafe(wchar_t* dst, size_t dstChars, const std::wstring& src) {
    if (!dst || dstChars == 0) return;
    wcsncpy_s(dst, dstChars, src.c_str(), _TRUNCATE);
}

} // namespace

ReceiverEngine::~ReceiverEngine() {
    Stop();
}

std::vector<MonitorDesc> ReceiverEngine::EnumerateLocalMonitors() {
    std::vector<MonitorDesc> list;
    EnumDisplayMonitors(nullptr, nullptr, MonitorEnumCb, reinterpret_cast<LPARAM>(&list));
    // Sort primary monitor first, then left-to-right
    std::sort(list.begin(), list.end(), [](const MonitorDesc& a, const MonitorDesc& b) {
        if (a.isPrimary != b.isPrimary) return a.isPrimary > b.isPrimary;
        if (a.left != b.left) return a.left < b.left;
        return a.top < b.top;
    });
    for (size_t i = 0; i < list.size(); ++i) {
        list[i].index = static_cast<uint32_t>(i);
    }
    if (list.size() > kMaxMonitors) {
        list.resize(kMaxMonitors);
    }
    return list;
}

bool ReceiverEngine::Start(bool startFullscreen) {
    Stop();
    m_stop = false;
    m_startFullscreen = startFullscreen;

    auto mons = EnumerateLocalMonitors();
    auto ips = GetLocalIPv4Addresses();

    if (!m_discoverySock.Bind(kPortDiscovery, true)) {
        Logger::Instance().Warn("RCV-UDP", L"Could not bind UDP discovery port " + std::to_wstring(kPortDiscovery));
    }
    if (!m_controlListenSock.Listen(kPortControl, 4)) {
        Logger::Instance().Error("RCV-TCP", L"Failed to listen on control port " + std::to_wstring(kPortControl));
        m_discoverySock.Close();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status = ReceiverStatus{};
        m_status.running = true;
        m_status.localMonitors = mons;
        m_status.localIps = ips;
        m_status.stateText = L"Bereit - Warte auf Verbindung vom Surface (Port " +
                             std::to_wstring(kPortControl) + L")";
    }

    m_running = true;
    m_discoveryThread = std::thread(&ReceiverEngine::DiscoveryLoop, this);
    m_controlThread = std::thread(&ReceiverEngine::ControlAcceptLoop, this);
    Logger::Instance().Info("RCV-OK", L"Receiver listening on UDP " + std::to_wstring(kPortDiscovery) +
                            L" / TCP " + std::to_wstring(kPortControl));
    return true;
}

void ReceiverEngine::Stop() {
    m_stop = true;
    m_running = false;
    DisconnectSession();

    m_discoverySock.Close();
    m_controlListenSock.Close();

    if (m_discoveryThread.joinable()) m_discoveryThread.join();
    if (m_controlThread.joinable()) m_controlThread.join();

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_status.running = false;
    m_status.sessionActive = false;
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
        if (!ValidateHeader(hdr) || hdr.type != static_cast<uint16_t>(MsgType::DiscoverReq)) continue;

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

        struct {
            PacketHeader hdr;
            DiscoverRespPayload body;
        } reply{};
        reply.hdr = MakeHeader(MsgType::DiscoverResp, sizeof(DiscoverRespPayload));
        reply.body = resp;
        m_discoverySock.SendTo(senderIp, senderPort, &reply, sizeof(reply));
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
    DisconnectSession();
    m_sessionStop = false;

    clientSock.TuneLowLatency(1024 * 1024);
    clientSock.SetTimeouts(3000, 1000);

    PacketHeader hdr{};
    std::vector<uint8_t> payload;
    if (!clientSock.RecvPacket(hdr, payload) ||
        hdr.type != static_cast<uint16_t>(MsgType::HelloSender) ||
        payload.size() < sizeof(HelloSenderPayload)) {
        Logger::Instance().Warn("RCV-CTL", L"Expected HelloSender packet.");
        return;
    }

    HelloSenderPayload helloSender{};
    std::memcpy(&helloSender, payload.data(), sizeof(helloSender));
    helloSender.senderName[_countof(helloSender.senderName) - 1] = L'\0';

    auto mons = EnumerateLocalMonitors();
    HelloReceiverPayload helloResp{};
    CopyWideSafe(helloResp.pcName, _countof(helloResp.pcName), GetHostComputerName());
    helloResp.monitorCount = static_cast<uint32_t>(std::min<size_t>(mons.size(), kMaxMonitors));
    for (uint32_t i = 0; i < helloResp.monitorCount; ++i) {
        helloResp.monitors[i] = mons[i];
    }

    if (!clientSock.SendPacket(MsgType::HelloReceiver, &helloResp, sizeof(helloResp))) {
        return;
    }

    // Wait for StartSession
    if (!clientSock.RecvPacket(hdr, payload) ||
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
        w->port = static_cast<uint16_t>(kPortStreamBase + i);
        w->targetMonitorRect = RECT{ md.left, md.top, md.right, md.bottom };
        w->expectedWidth = spec.width ? spec.width : md.width;
        w->expectedHeight = spec.height ? spec.height : md.height;
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
        m_workers.push_back(std::move(w));
    }

    if (!allListening) {
        StopStreamWorkers();
        ack.accepted = 0;
        ack.errorCode = 2;
        CopyWideSafe(ack.message, _countof(ack.message), L"Stream-Port konnte nicht geoeffnet werden.");
        clientSock.SendPacket(MsgType::SessionAck, &ack, sizeof(ack));
        return;
    }

    ack.accepted = 1;
    CopyWideSafe(ack.message, _countof(ack.message), L"OK");
    if (!clientSock.SendPacket(MsgType::SessionAck, &ack, sizeof(ack))) {
        StopStreamWorkers();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
        m_activeCtrlSock = std::move(clientSock);
    }

    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        m_status.sessionActive = true;
        m_status.senderName = helloSender.senderName;
        m_status.senderIp = peerIp;
        m_status.localMonitors = mons;
        m_status.stateText = L"Verbunden mit " + m_status.senderName + L" (" + Utf8ToWide(peerIp) +
                             L") - " + std::to_wstring(streamCount) + L" Display(s)";
    }

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
            }
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT && err != 0) {
                Logger::Instance().Warn("RCV-CTL", L"Control connection closed, wsa=" + std::to_wstring(err));
                break;
            }
        }
    }

    DisconnectSession();
}

void ReceiverEngine::RequestKeyframe(uint32_t monitorIndex) {
    std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
    if (!m_activeCtrlSock.IsValid()) return;
    RequestKeyframePayload req{ monitorIndex };
    m_activeCtrlSock.SendPacket(MsgType::RequestKeyframe, &req, sizeof(req));
}

void ReceiverEngine::RunStreamWorker(StreamReceiverWorker* w) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    TcpSocket sock = w->listenSock.Accept(nullptr, 5000);
    w->listenSock.Close();
    if (!sock.IsValid()) {
        Logger::Instance().Warn("RCV-STRM", L"Timed out waiting for stream connection on port " + std::to_wstring(w->port));
        CoUninitialize();
        return;
    }
    sock.TuneLowLatency(4 * 1024 * 1024);
    sock.SetTimeouts(3000, 50); // short recv timeout so PumpMessages stays responsive

    D3D11Renderer renderer;
    if (!renderer.Create(w->monitorIndex, w->targetMonitorRect, w->expectedWidth, w->expectedHeight, w->startFullscreen)) {
        Logger::Instance().Error("RCV-RND", L"Failed to create D3D11Renderer for monitor " + std::to_wstring(w->monitorIndex));
        CoUninitialize();
        return;
    }

    H264Decoder h264;
    uint32_t h264W = 0, h264H = 0;
    DeltaDecoder delta;
    std::vector<uint8_t> cpuBgra;

    auto windowStart = std::chrono::steady_clock::now();
    uint64_t windowFrames = 0;
    uint64_t windowBytes = 0;
    uint64_t totalFrames = 0;
    bool needPresent = false;

    while (!m_stop && !m_sessionStop && !w->stop && !renderer.IsClosedByUser()) {
        renderer.PumpMessages();
        if (renderer.NeedsRepaint()) {
            renderer.ClearRepaint();
            needPresent = true;
        }

        PacketHeader hdr{};
        std::vector<uint8_t> payload;
        if (!sock.RecvPacket(hdr, payload)) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT || err == 0) {
                if (needPresent) {
                    renderer.Present();
                    needPresent = false;
                }
                continue;
            }
            break;
        }

        if (hdr.type == static_cast<uint16_t>(MsgType::StreamEnd)) {
            break;
        } else if (hdr.type == static_cast<uint16_t>(MsgType::CursorUpdate) &&
                   payload.size() >= sizeof(CursorUpdateHeader)) {
            CursorUpdateHeader ch{};
            std::memcpy(&ch, payload.data(), sizeof(ch));
            const uint8_t* shapePtr = (ch.dataSize > 0 && payload.size() >= sizeof(ch) + ch.dataSize)
                                          ? (payload.data() + sizeof(ch))
                                          : nullptr;
            renderer.UpdateCursor(ch.visible != 0, ch.x, ch.y, ch.shapeChanged != 0, ch.shapeType,
                                  ch.shapeWidth, ch.shapeHeight, ch.shapePitch, ch.hotX, ch.hotY,
                                  shapePtr, ch.dataSize);
            needPresent = true;
        } else if (hdr.type == static_cast<uint16_t>(MsgType::VideoFrame) &&
                   payload.size() >= sizeof(VideoFrameHeader)) {
            VideoFrameHeader vh{};
            std::memcpy(&vh, payload.data(), sizeof(vh));
            if (payload.size() < sizeof(vh) + vh.dataSize) continue;
            const uint8_t* frameData = payload.data() + sizeof(vh);

            bool decodedOk = false;
            std::wstring codecLabel;
            bool isDxva = false;

            if (vh.codec == static_cast<uint8_t>(CodecType::H264)) {
                if (!h264.IsInitialized() || h264W != vh.width || h264H != vh.height) {
                    h264.Init(renderer.Device(), renderer.Context(), vh.width, vh.height);
                    h264W = vh.width;
                    h264H = vh.height;
                    renderer.EnsureFrameTexture(vh.width, vh.height);
                }
                if (h264.IsInitialized()) {
                    renderer.EnsureFrameTexture(vh.width, vh.height);
                    uint32_t outW = vh.width, outH = vh.height;
                    if (h264.IsHardwareDxva()) {
                        decodedOk = h264.Decode(frameData, vh.dataSize, vh.timestampUs,
                                                renderer.FrameTexture(), nullptr, &outW, &outH);
                        codecLabel = L"H.264 (NVDEC/DXVA)";
                        isDxva = true;
                    } else {
                        decodedOk = h264.Decode(frameData, vh.dataSize, vh.timestampUs,
                                                nullptr, &cpuBgra, &outW, &outH);
                        if (decodedOk && !cpuBgra.empty()) {
                            renderer.UploadCpuBgra(cpuBgra.data(), outW * 4, outW, outH);
                        }
                        codecLabel = L"H.264 (CPU)";
                    }
                } else {
                    RequestKeyframe(w->monitorIndex);
                }
            } else if (vh.codec == static_cast<uint8_t>(CodecType::DeltaLossless)) {
                if (delta.Decode(frameData, vh.dataSize, vh.width, vh.height)) {
                    renderer.UploadCpuBgra(delta.FrameData(), delta.StrideBytes(), delta.Width(), delta.Height());
                    decodedOk = true;
                    codecLabel = L"Delta Lossless";
                } else {
                    RequestKeyframe(w->monitorIndex);
                }
            }

            if (decodedOk) {
                ++totalFrames;
                ++windowFrames;
                windowBytes += payload.size() + sizeof(PacketHeader);
                renderer.Present();
                needPresent = false;

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
                    windowStart = now;
                    windowFrames = 0;
                    windowBytes = 0;
                }
            }
        }
    }

    if (renderer.IsClosedByUser() && !m_sessionStop) {
        std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
        if (m_activeCtrlSock.IsValid()) {
            m_activeCtrlSock.SendPacket(MsgType::StopSession, nullptr, 0);
        }
        m_sessionStop = true;
    }

    h264.Shutdown();
    renderer.Destroy();
    sock.Close();
    CoUninitialize();
}

void ReceiverEngine::StopStreamWorkers() {
    for (auto& w : m_workers) {
        w->stop = true;
        w->listenSock.Close();
    }
    for (auto& w : m_workers) {
        if (w->thread.joinable()) w->thread.join();
    }
    m_workers.clear();
}

void ReceiverEngine::DisconnectSession() {
    m_sessionStop = true;
    {
        std::lock_guard<std::mutex> lock(m_ctrlSendMutex);
        if (m_activeCtrlSock.IsValid()) {
            m_activeCtrlSock.SendPacket(MsgType::StopSession, nullptr, 0);
            m_activeCtrlSock.Close();
        }
    }
    StopStreamWorkers();

    std::lock_guard<std::mutex> lock(m_stateMutex);
    m_status.sessionActive = false;
    m_status.senderName.clear();
    m_status.senderIp.clear();
    m_status.streams.clear();
    if (m_running) {
        m_status.stateText = L"Bereit - Warte auf Verbindung vom Surface (Port " +
                             std::to_wstring(kPortControl) + L")";
    }
}

ReceiverStatus ReceiverEngine::GetStatus() const {
    ReceiverStatus st;
    {
        std::lock_guard<std::mutex> lock(m_stateMutex);
        st = m_status;
    }
    st.streams.clear();
    for (const auto& w : m_workers) {
        std::lock_guard<std::mutex> sl(w->statsMutex);
        st.streams.push_back(w->stats);
    }
    return st;
}

} // namespace s2p
