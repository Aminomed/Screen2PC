#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/Protocol.h"
#include "../common/Logger.h"
#include "../common/NetSocket.h"
#include "../common/CodecDelta.h"
#include "../common/CodecH264.h"

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::cerr << "[FAIL] " << msg << " (line " << __LINE__ << ")\n"; \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

void TestProtocol() {
    auto hdr = s2p::MakeHeader(s2p::MsgType::VideoFrame, 1234, 7);
    CHECK(s2p::ValidateHeader(hdr), "Valid header rejected");

    auto badMagic = hdr;
    badMagic.magic = 0x12345678;
    CHECK(!s2p::ValidateHeader(badMagic), "Bad magic accepted");

    auto badVer = hdr;
    badVer.version = 99;
    CHECK(!s2p::ValidateHeader(badVer), "Bad version accepted");

    auto badSize = hdr;
    badSize.payloadSize = static_cast<uint32_t>(s2p::kMaxFrameBytes + 1);
    CHECK(!s2p::ValidateHeader(badSize), "Oversized payload accepted");

    std::cout << "[PASS] TestProtocol\n";
}

void FillPatternBgra(std::vector<uint8_t>& buf, uint32_t w, uint32_t h, uint32_t stride, uint8_t seed) {
    buf.assign(static_cast<size_t>(stride) * h, 0);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = buf.data() + static_cast<size_t>(y) * stride;
        for (uint32_t x = 0; x < w; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>((x * 3 + seed) & 0xFF);
            row[x * 4 + 1] = static_cast<uint8_t>((y * 5 + seed) & 0xFF);
            row[x * 4 + 2] = static_cast<uint8_t>(((x ^ y) + seed) & 0xFF);
            row[x * 4 + 3] = 255;
        }
    }
}

void TestDeltaCodec() {
    const uint32_t w = 640, h = 360, stride = w * 4;
    std::vector<uint8_t> frame1, frame2;
    FillPatternBgra(frame1, w, h, stride, 10);

    s2p::DeltaEncoder enc;
    s2p::DeltaDecoder dec;

    std::vector<uint8_t> payload1;
    bool isKey = false;
    CHECK(enc.Encode(frame1.data(), w, h, stride, {}, false, payload1, isKey), "Delta encode keyframe failed");
    CHECK(isKey, "First Delta frame must be keyframe");
    CHECK(dec.Decode(payload1.data(), payload1.size(), w, h), "Delta decode keyframe failed");
    CHECK(std::memcmp(frame1.data(), dec.FrameData(), frame1.size()) == 0, "Delta keyframe round-trip mismatch");

    // Modify small 32x32 rectangle only
    frame2 = frame1;
    for (uint32_t y = 100; y < 132; ++y) {
        for (uint32_t x = 200; x < 232; ++x) {
            size_t idx = static_cast<size_t>(y) * stride + x * 4;
            frame2[idx + 0] = 0x11;
            frame2[idx + 1] = 0xAA;
            frame2[idx + 2] = 0xEE;
            frame2[idx + 3] = 0xFF;
        }
    }

    std::vector<uint8_t> payload2;
    bool isKey2 = true;
    std::vector<s2p::DeltaRect> hints = { { 200, 100, 32, 32 } };
    CHECK(enc.Encode(frame2.data(), w, h, stride, hints, false, payload2, isKey2), "Delta encode partial failed");
    CHECK(!isKey2, "Partial update unexpectedly flagged as keyframe");
    CHECK(payload2.size() < payload1.size() / 4, "Partial delta payload was not significantly smaller");
    CHECK(dec.Decode(payload2.data(), payload2.size(), w, h), "Delta decode partial failed");
    CHECK(std::memcmp(frame2.data(), dec.FrameData(), frame2.size()) == 0, "Delta partial round-trip mismatch");

    // Resize forces keyframe
    std::vector<uint8_t> frameSmall;
    FillPatternBgra(frameSmall, 320, 240, 320 * 4, 42);
    std::vector<uint8_t> payload3;
    bool isKey3 = false;
    CHECK(enc.Encode(frameSmall.data(), 320, 240, 320 * 4, {}, false, payload3, isKey3), "Delta resize encode failed");
    CHECK(isKey3, "Resize should trigger keyframe");
    CHECK(dec.Decode(payload3.data(), payload3.size(), 320, 240), "Delta resize decode failed");
    CHECK(std::memcmp(frameSmall.data(), dec.FrameData(), frameSmall.size()) == 0, "Delta resize round-trip mismatch");

    std::cout << "[PASS] TestDeltaCodec\n";
}

void TestColorAndH264() {
    const uint32_t w = 640, h = 360;
    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4, 0);
    // Solid bright frame (B=200, G=220, R=210)
    for (size_t i = 0; i < w * h; ++i) {
        bgra[i * 4 + 0] = 200;
        bgra[i * 4 + 1] = 220;
        bgra[i * 4 + 2] = 210;
        bgra[i * 4 + 3] = 255;
    }

    std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2);
    std::vector<uint8_t> roundTrip(static_cast<size_t>(w) * h * 4);
    s2p::CpuBgraToNv12(bgra.data(), w * 4, w, h, nv12.data());
    s2p::CpuNv12ToBgra(nv12.data(), w, nv12.data() + static_cast<size_t>(w) * h, w, w, h, roundTrip.data());
    CHECK(std::abs(static_cast<int>(roundTrip[1]) - 220) < 20, "CPU BGRA<->NV12 conversion accuracy check failed");

    s2p::H264Encoder enc;
    s2p::H264Decoder dec;
    bool encInit = enc.Init(nullptr, nullptr, w, h, 30, 10);
    bool decInit = dec.Init(nullptr, nullptr, w, h);
    CHECK(encInit, "H264Encoder CPU Init failed");
    CHECK(decInit, "H264Decoder CPU Init failed");

    if (encInit && decInit) {
        bool gotEncoded = false;
        bool gotDecoded = false;
        std::vector<uint8_t> outBgra;

        for (int f = 0; f < 10; ++f) {
            std::vector<uint8_t> annexB;
            bool isKey = false;
            if (f == 0) enc.RequestKeyframe();
            if (enc.EncodeBgra(bgra.data(), w * 4, static_cast<uint64_t>(f) * 33333ULL, annexB, isKey) && !annexB.empty()) {
                gotEncoded = true;
                uint32_t outW = 0, outH = 0;
                if (dec.Decode(annexB.data(), annexB.size(), static_cast<uint64_t>(f) * 33333ULL,
                               nullptr, &outBgra, &outW, &outH) && !outBgra.empty()) {
                    CHECK(outW == w && outH == h, "H264 decoded dimensions mismatch");
                    // Center pixel should be bright (>150)
                    size_t center = (static_cast<size_t>(h / 2) * w + (w / 2)) * 4;
                    CHECK(outBgra[center + 1] > 150, "H264 decoded center pixel luma unexpectedly dark");
                    gotDecoded = true;
                    break;
                }
            }
        }
        CHECK(gotEncoded, "H264Encoder produced no Annex-B output across 10 frames");
        CHECK(gotDecoded, "H264Decoder produced no decoded frame across 10 frames");
    }

    enc.Shutdown();
    dec.Shutdown();
    std::cout << "[PASS] TestColorAndH264\n";
}

void TestLoopbackNetworking() {
    const uint16_t testTcpPort = 48299;
    const uint16_t testUdpPort = 48298;

    // 1. TCP Packet framing roundtrip
    s2p::TcpSocket listener;
    CHECK(listener.Listen(testTcpPort, 2), "TcpSocket::Listen failed on loopback test port");

    std::thread serverThread([&]() {
        s2p::TcpSocket srv = listener.Accept(nullptr, 3000);
        if (!srv.IsValid()) return;
        s2p::PacketHeader hdr{};
        std::vector<uint8_t> payload;
        if (srv.RecvPacket(hdr, payload) && hdr.type == static_cast<uint16_t>(s2p::MsgType::VideoFrame)) {
            srv.SendPacket(s2p::MsgType::SessionAck, payload.data(), static_cast<uint32_t>(payload.size()), 77);
        }
        srv.Close();
    });

    s2p::TcpSocket client;
    CHECK(client.Connect("127.0.0.1", testTcpPort, 2000), "TcpSocket::Connect 127.0.0.1 failed");
    const std::vector<uint8_t> sample = { 1, 2, 3, 4, 5, 42, 99 };
    CHECK(client.SendPacket(s2p::MsgType::VideoFrame, sample.data(), static_cast<uint32_t>(sample.size())),
          "TcpSocket::SendPacket failed");

    s2p::PacketHeader ackHdr{};
    std::vector<uint8_t> ackPayload;
    CHECK(client.RecvPacket(ackHdr, ackPayload), "TcpSocket::RecvPacket failed");
    CHECK(ackHdr.type == static_cast<uint16_t>(s2p::MsgType::SessionAck), "Wrong echo MsgType");
    CHECK(ackHdr.reserved == 77, "Wrong header reserved field");
    CHECK(ackPayload == sample, "Echoed TCP payload corrupted");

    client.Close();
    listener.Close();
    serverThread.join();

    // 2. UDP Loopback packet roundtrip
    s2p::UdpSocket udpSrv;
    s2p::UdpSocket udpCli;
    CHECK(udpSrv.Bind(testUdpPort, true), "UdpSocket::Bind failed");
    CHECK(udpCli.CreateForBroadcast(), "UdpSocket::CreateForBroadcast failed");

    const char msg[] = "S2P_PING";
    CHECK(udpCli.SendTo("127.0.0.1", testUdpPort, msg, sizeof(msg)), "UdpSocket::SendTo failed");
    char recvBuf[64]{};
    std::string fromIp;
    uint16_t fromPort = 0;
    int recvd = udpSrv.RecvFrom(recvBuf, sizeof(recvBuf), fromIp, fromPort, 1000);
    CHECK(recvd == sizeof(msg) && std::memcmp(recvBuf, msg, sizeof(msg)) == 0, "UDP loopback mismatch");

    std::cout << "[PASS] TestLoopbackNetworking\n";
}

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    s2p::Logger::Instance().Init(L"Screen2PC_Tests.log");
    s2p::WinsockGuard wsa;
    CHECK(wsa.Ok(), "WinsockGuard initialization failed");

    TestProtocol();
    TestDeltaCodec();
    TestColorAndH264();
    TestLoopbackNetworking();

    CoUninitialize();

    if (g_failures > 0) {
        std::cerr << "\nTOTAL FAILURES: " << g_failures << "\n";
        return 1;
    }
    std::cout << "\nALL SCREEN2PC TESTS PASSED!\n";
    return 0;
}
