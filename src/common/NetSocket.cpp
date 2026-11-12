#include "NetSocket.h"
#include "Logger.h"
#include <iphlpapi.h>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

namespace s2p {

WinsockGuard::WinsockGuard() {
    WSADATA wsa{};
    m_ok = (WSAStartup(MAKEWORD(2, 2), &wsa) == 0);
}

WinsockGuard::~WinsockGuard() {
    if (m_ok) {
        WSACleanup();
    }
}

TcpSocket::~TcpSocket() {
    Close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept : m_sock(other.m_sock) {
    other.m_sock = INVALID_SOCKET;
}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        Close();
        m_sock = other.m_sock;
        other.m_sock = INVALID_SOCKET;
    }
    return *this;
}

void TcpSocket::Close() {
    if (m_sock != INVALID_SOCKET) {
        shutdown(m_sock, SD_BOTH);
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
}

bool TcpSocket::Connect(const std::string& host, uint16_t port, int timeoutMs) {
    Close();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    char portStr[16]{};
    snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), portStr, &hints, &res) != 0 || !res) {
        return false;
    }

    SOCKET s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(res);
        return false;
    }

    // Non-blocking connect with timeout
    u_long nonBlock = 1;
    ioctlsocket(s, FIONBIO, &nonBlock);

    int rc = connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen));
    freeaddrinfo(res);

    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            closesocket(s);
            return false;
        }
        fd_set writeSet, errSet;
        FD_ZERO(&writeSet);
        FD_ZERO(&errSet);
        FD_SET(s, &writeSet);
        FD_SET(s, &errSet);

        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;

        int sel = select(0, nullptr, &writeSet, &errSet, &tv);
        if (sel <= 0 || !FD_ISSET(s, &writeSet) || FD_ISSET(s, &errSet)) {
            closesocket(s);
            return false;
        }
        int soErr = 0;
        int optLen = sizeof(soErr);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soErr), &optLen);
        if (soErr != 0) {
            closesocket(s);
            return false;
        }
    }

    nonBlock = 0;
    ioctlsocket(s, FIONBIO, &nonBlock);
    m_sock = s;
    TuneLowLatency();
    return true;
}

bool TcpSocket::Listen(uint16_t port, int backlog) {
    Close();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;

    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }
    if (listen(s, backlog) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }
    m_sock = s;
    return true;
}

TcpSocket TcpSocket::Accept(std::string* outPeerIp, int timeoutMs) {
    if (m_sock == INVALID_SOCKET) return TcpSocket();

    if (timeoutMs >= 0) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(m_sock, &readSet);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel <= 0) return TcpSocket();
    }

    sockaddr_in peer{};
    int peerLen = sizeof(peer);
    SOCKET client = accept(m_sock, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    if (client == INVALID_SOCKET) return TcpSocket();

    if (outPeerIp) {
        char buf[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof(buf));
        *outPeerIp = buf;
    }

    TcpSocket sock(client);
    sock.TuneLowLatency();
    return sock;
}

void TcpSocket::TuneLowLatency(int bufBytes) {
    if (m_sock == INVALID_SOCKET) return;
    BOOL nodelay = TRUE;
    setsockopt(m_sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    setsockopt(m_sock, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&bufBytes), sizeof(bufBytes));
    setsockopt(m_sock, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bufBytes), sizeof(bufBytes));
}

void TcpSocket::SetTimeouts(int sendTimeoutMs, int recvTimeoutMs) {
    if (m_sock == INVALID_SOCKET) return;
    DWORD snd = static_cast<DWORD>(sendTimeoutMs);
    DWORD rcv = static_cast<DWORD>(recvTimeoutMs);
    setsockopt(m_sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&snd), sizeof(snd));
    setsockopt(m_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcv), sizeof(rcv));
}

int TcpSocket::WaitReadable(int timeoutMs) {
    if (m_sock == INVALID_SOCKET) return -1;
    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(m_sock, &readSet);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    int sel = select(0, &readSet, nullptr, nullptr, (timeoutMs >= 0) ? &tv : nullptr);
    if (sel > 0) return 1;
    if (sel == 0) return 0;
    return -1;
}

bool TcpSocket::SendAll(const void* data, size_t len) {
    if (m_sock == INVALID_SOCKET) return false;
    const char* ptr = static_cast<const char*>(data);
    size_t sent = 0;
    while (sent < len) {
        int chunk = static_cast<int>(std::min<size_t>(len - sent, 1 << 20));
        int n = send(m_sock, ptr + sent, chunk, 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool TcpSocket::RecvAll(void* data, size_t len) {
    if (m_sock == INVALID_SOCKET) return false;
    char* ptr = static_cast<char*>(data);
    size_t recvd = 0;
    while (recvd < len) {
        int chunk = static_cast<int>(std::min<size_t>(len - recvd, 1 << 20));
        int n = recv(m_sock, ptr + recvd, chunk, 0);
        if (n <= 0) return false;
        recvd += static_cast<size_t>(n);
    }
    return true;
}

bool TcpSocket::SendPacket(MsgType type, const void* payload, uint32_t payloadSize, uint32_t reserved) {
    PacketHeader hdr = MakeHeader(type, payloadSize, reserved);
    if (payloadSize == 0 || !payload) {
        return SendAll(&hdr, sizeof(hdr));
    }

    const size_t total = sizeof(hdr) + static_cast<size_t>(payloadSize);
    if (total <= 1536) {
        uint8_t stackBuf[1536];
        std::memcpy(stackBuf, &hdr, sizeof(hdr));
        std::memcpy(stackBuf + sizeof(hdr), payload, payloadSize);
        return SendAll(stackBuf, total);
    }

    thread_local std::vector<uint8_t> sendBuf;
    sendBuf.resize(total);
    std::memcpy(sendBuf.data(), &hdr, sizeof(hdr));
    std::memcpy(sendBuf.data() + sizeof(hdr), payload, payloadSize);
    return SendAll(sendBuf.data(), total);
}

bool TcpSocket::RecvPacket(PacketHeader& outHeader, std::vector<uint8_t>& outPayload) {
    if (!RecvAll(&outHeader, sizeof(outHeader))) return false;
    if (!ValidateHeader(outHeader)) return false;
    outPayload.resize(outHeader.payloadSize);
    if (outHeader.payloadSize > 0) {
        if (!RecvAll(outPayload.data(), outHeader.payloadSize)) return false;
    }
    return true;
}

std::string TcpSocket::PeerIp() const {
    if (m_sock == INVALID_SOCKET) return {};
    sockaddr_in addr{};
    int len = sizeof(addr);
    if (getpeername(m_sock, reinterpret_cast<sockaddr*>(&addr), &len) != 0) return {};
    char buf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
    return buf;
}

UdpSocket::~UdpSocket() {
    Close();
}

void UdpSocket::Close() {
    if (m_sock != INVALID_SOCKET) {
        closesocket(m_sock);
        m_sock = INVALID_SOCKET;
    }
}

bool UdpSocket::Bind(uint16_t port, bool reuseAddr) {
    Close();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;

    if (reuseAddr) {
        BOOL reuse = TRUE;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    }
    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }
    m_sock = s;
    return true;
}

bool UdpSocket::CreateForBroadcast() {
    Close();
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;

    BOOL bcast = TRUE;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&bcast), sizeof(bcast));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = 0;
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return false;
    }
    m_sock = s;
    return true;
}

bool UdpSocket::SendTo(const std::string& ip, uint16_t port, const void* data, size_t len) {
    if (m_sock == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return false;
    int sent = sendto(m_sock, static_cast<const char*>(data), static_cast<int>(len), 0,
                      reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return sent == static_cast<int>(len);
}

bool UdpSocket::Broadcast(uint16_t port, const void* data, size_t len) {
    bool anyOk = false;
    for (const auto& bcastIp : GetLocalBroadcastAddresses()) {
        if (SendTo(bcastIp, port, data, len)) anyOk = true;
    }
    if (SendTo("255.255.255.255", port, data, len)) anyOk = true;
    return anyOk;
}

int UdpSocket::RecvFrom(void* buf, size_t bufCap, std::string& outSenderIp, uint16_t& outSenderPort, int timeoutMs) {
    if (m_sock == INVALID_SOCKET) return -1;

    if (timeoutMs >= 0) {
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(m_sock, &readSet);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel == 0) return 0;
        if (sel < 0) return -1;
    }

    sockaddr_in from{};
    int fromLen = sizeof(from);
    int n = recvfrom(m_sock, static_cast<char*>(buf), static_cast<int>(bufCap), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromLen);
    if (n <= 0) return -1;

    char ipBuf[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &from.sin_addr, ipBuf, sizeof(ipBuf));
    outSenderIp = ipBuf;
    outSenderPort = ntohs(from.sin_port);
    return n;
}

std::vector<std::string> GetLocalBroadcastAddresses() {
    std::vector<std::string> result;
    ULONG size = 0;
    GetIpAddrTable(nullptr, &size, FALSE);
    if (size == 0) return result;

    std::vector<uint8_t> buf(size);
    auto* table = reinterpret_cast<MIB_IPADDRTABLE*>(buf.data());
    if (GetIpAddrTable(table, &size, FALSE) != NO_ERROR) return result;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwAddr == 0 || row.dwMask == 0) continue;
        if ((row.dwAddr & 0xFF) == 127) continue; // skip 127.x.x.x
        uint32_t bcast = row.dwAddr | (~row.dwMask);
        in_addr ina{};
        ina.S_un.S_addr = bcast;
        char str[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &ina, str, sizeof(str));
        if (std::find(result.begin(), result.end(), str) == result.end()) {
            result.emplace_back(str);
        }
    }
    return result;
}

std::vector<std::string> GetLocalIPv4Addresses() {
    std::vector<std::string> result;
    ULONG size = 0;
    GetIpAddrTable(nullptr, &size, FALSE);
    if (size == 0) return result;

    std::vector<uint8_t> buf(size);
    auto* table = reinterpret_cast<MIB_IPADDRTABLE*>(buf.data());
    if (GetIpAddrTable(table, &size, FALSE) != NO_ERROR) return result;

    for (DWORD i = 0; i < table->dwNumEntries; ++i) {
        const auto& row = table->table[i];
        if (row.dwAddr == 0) continue;
        if ((row.dwAddr & 0xFF) == 127) continue;
        in_addr ina{};
        ina.S_un.S_addr = row.dwAddr;
        char str[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &ina, str, sizeof(str));
        result.emplace_back(str);
    }
    return result;
}

std::wstring GetHostComputerName() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD len = ARRAYSIZE(name);
    if (GetComputerNameW(name, &len)) {
        return std::wstring(name, len);
    }
    return L"WindowsPC";
}

} // namespace s2p
