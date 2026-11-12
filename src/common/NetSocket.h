#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <string>
#include <vector>
#include "Protocol.h"

namespace s2p {

class WinsockGuard {
public:
    WinsockGuard();
    ~WinsockGuard();
    WinsockGuard(const WinsockGuard&) = delete;
    WinsockGuard& operator=(const WinsockGuard&) = delete;
    bool Ok() const { return m_ok; }
private:
    bool m_ok = false;
};

class TcpSocket {
public:
    TcpSocket() = default;
    explicit TcpSocket(SOCKET s) : m_sock(s) {}
    ~TcpSocket();

    TcpSocket(const TcpSocket&) = delete;
    TcpSocket& operator=(const TcpSocket&) = delete;
    TcpSocket(TcpSocket&& other) noexcept;
    TcpSocket& operator=(TcpSocket&& other) noexcept;

    bool IsValid() const { return m_sock != INVALID_SOCKET; }
    SOCKET Handle() const { return m_sock; }
    void Close();

    bool Connect(const std::string& host, uint16_t port, int timeoutMs = 3000);
    bool Listen(uint16_t port, int backlog = 8);
    TcpSocket Accept(std::string* outPeerIp = nullptr, int timeoutMs = -1);

    void TuneLowLatency(int bufBytes = 4 * 1024 * 1024);
    void SetTimeouts(int sendTimeoutMs, int recvTimeoutMs);

    bool SendAll(const void* data, size_t len);
    bool RecvAll(void* data, size_t len);

    bool SendPacket(MsgType type, const void* payload, uint32_t payloadSize, uint32_t reserved = 0);
    bool RecvPacket(PacketHeader& outHeader, std::vector<uint8_t>& outPayload);

    std::string PeerIp() const;

private:
    SOCKET m_sock = INVALID_SOCKET;
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool Bind(uint16_t port, bool reuseAddr = true);
    bool CreateForBroadcast();
    void Close();
    bool IsValid() const { return m_sock != INVALID_SOCKET; }

    bool SendTo(const std::string& ip, uint16_t port, const void* data, size_t len);
    bool Broadcast(uint16_t port, const void* data, size_t len);
    int  RecvFrom(void* buf, size_t bufCap, std::string& outSenderIp, uint16_t& outSenderPort, int timeoutMs);

private:
    SOCKET m_sock = INVALID_SOCKET;
};

struct DiscoveredPeer {
    std::string ip;
    std::wstring pcName;
    uint16_t controlPort = kPortControl;
    uint16_t streamBasePort = kPortStreamBase;
    std::vector<MonitorDesc> monitors;
};

std::vector<std::string> GetLocalBroadcastAddresses();
std::vector<std::string> GetLocalIPv4Addresses();
std::wstring GetHostComputerName();

} // namespace s2p
