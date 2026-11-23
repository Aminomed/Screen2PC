#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <set>
#include <thread>
#include <vector>

#include "../common/Version.h"
#include "../common/AppIcon.h"
#include "../common/Autostart.h"
#include "../common/BitratePresets.h"
#include "../common/ColorProfile.h"
#include "../common/Pairing.h"
#include "../common/Updater.h"
#include "../common/Protocol.h"
#include "../common/Logger.h"
#include "../common/NetSocket.h"
#include "../common/CodecDelta.h"
#include "../common/CodecH264.h"
#include "../common/AmfEncoder.h"
#include "../common/UdpVideoFec.h"
#include "../common/ClipboardSync.h"
#include "../common/FileTransfer.h"
#include "../common/KvmInput.h"
#include "../sender/PowerPolicy.h"
#include "../driver/DriverProtocol.h"

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

    // Cursor shape buffer validation
    uint32_t actualH = 0;
    CHECK(s2p::ValidateCursorShapeBuffer(1, 16, 32, 2, 64, actualH) && actualH == 16,
          "Valid monochrome cursor buffer rejected");
    CHECK(!s2p::ValidateCursorShapeBuffer(1, 16, 1, 2, 64, actualH),
          "Height=1 monochrome cursor accepted");
    CHECK(s2p::ValidateCursorShapeBuffer(2, 32, 32, 128, 32 * 128, actualH) && actualH == 32,
          "Valid color cursor buffer rejected");
    CHECK(s2p::ValidateCursorShapeBuffer(4, 16, 16, 80, 15 * 80 + 16 * 4, actualH) && actualH == 16,
          "Valid masked color cursor with trailing pitch rejected");
    CHECK(!s2p::ValidateCursorShapeBuffer(4, 16, 16, 80, 15 * 80 + 16 * 4 - 1, actualH),
          "Truncated cursor buffer accepted");
    CHECK(!s2p::ValidateCursorShapeBuffer(2, 16, 16, 60, 16 * 60, actualH),
          "Undersized cursor pitch accepted");
    CHECK(!s2p::ValidateCursorShapeBuffer(2, s2p::kMaxCursorDimension + 1, 16, (s2p::kMaxCursorDimension + 1) * 4, 1024 * 1024, actualH),
          "Oversized cursor width accepted");
    CHECK(!s2p::ValidateCursorShapeBuffer(3, 16, 16, 64, 16 * 64, actualH),
          "Unknown cursor shapeType accepted");

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
    std::vector<s2p::DeltaRect> hints = { { 200, 100, 32, 32 }, { 0xFFFFFFFFu, 0, 20, 20 }, { 5000, 5000, 16, 16 } };
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

    // Corrupted payload must be rejected cleanly without out-of-bounds read/write
    std::vector<uint8_t> corrupt = payload2;
    if (corrupt.size() > 8) {
        corrupt.resize(corrupt.size() - 5);
        CHECK(!dec.Decode(corrupt.data(), corrupt.size(), w, h), "Truncated Delta payload unexpectedly accepted");
    }

    // Crafted Delta payload with 32-bit wraparound rect & zero rect must be rejected
    auto makeSingleRectDelta = [](uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh, uint32_t fw, uint32_t fh) {
        const size_t hdrBytes = 20 + 16;
        std::vector<uint8_t> p(hdrBytes + 64, 0);
        const uint32_t magic = 0x31544C44u; // 'DLT1'
        const uint32_t zero = 0;
        const uint32_t rectCount = 1;
        std::memcpy(p.data() + 0, &magic, 4);
        std::memcpy(p.data() + 4, &fw, 4);
        std::memcpy(p.data() + 8, &fh, 4);
        std::memcpy(p.data() + 12, &zero, 4);
        std::memcpy(p.data() + 16, &rectCount, 4);
        std::memcpy(p.data() + 20, &rx, 4);
        std::memcpy(p.data() + 24, &ry, 4);
        std::memcpy(p.data() + 28, &rw, 4);
        std::memcpy(p.data() + 32, &rh, 4);
        return p;
    };
    auto wrapPkt = makeSingleRectDelta(0xFFFFFFFFu, 0, 4, 4, w, h);
    CHECK(!dec.Decode(wrapPkt.data(), wrapPkt.size(), w, h), "32-bit wraparound Delta rect unexpectedly accepted");
    auto zeroPkt = makeSingleRectDelta(0, 0, 0, 4, w, h);
    CHECK(!dec.Decode(zeroPkt.data(), zeroPkt.size(), w, h), "Zero-width Delta rect unexpectedly accepted");

    // Oversized > 64MP dimensions must be rejected before allocation
    CHECK(!dec.Decode(payload2.data(), payload2.size(), 16384, 4097), "Oversized >64MP Delta decode accepted");
    CHECK(!enc.Encode(frame1.data(), 16384, 4097, 16384 * 4, {}, true, payload3, isKey3),
          "Oversized >64MP Delta encode accepted");

    // DLT2: MergeAndBoundRects + MoveRects + FastMotion 2x2 + Lossless Settle + LastBoundingRect
    {
        std::vector<s2p::DeltaRect> many;
        for (uint32_t i = 0; i < 80; ++i) {
            many.push_back(s2p::DeltaRect{ (i % 10) * 60, (i / 10) * 40, 20, 20 });
        }
        auto merged = s2p::DeltaEncoder::MergeAndBoundRects(many, 640, 360, 16);
        CHECK(!merged.empty() && merged.size() <= 16, "MergeAndBoundRects failed to bound rect count");

        s2p::DeltaEncoder enc2;
        s2p::DeltaDecoder dec2;
        std::vector<uint8_t> fA, fB, pA, pMove, pFast, pSettle;
        FillPatternBgra(fA, 640, 360, 640 * 4, 7);
        bool kA = false;
        CHECK(enc2.Encode(fA.data(), 640, 360, 640 * 4, {}, true, pA, kA), "DLT2 keyframe failed");
        CHECK(dec2.Decode(pA.data(), pA.size(), 640, 360), "DLT2 decode keyframe failed");

        // Scroll/Move 200x100 block from (40,40) to (120,140)
        fB = fA;
        for (uint32_t y = 0; y < 100; ++y) {
            std::memcpy(fB.data() + static_cast<size_t>(140 + y) * 640 * 4 + 120 * 4,
                        fA.data() + static_cast<size_t>(40 + y) * 640 * 4 + 40 * 4,
                        200 * 4);
        }
        std::vector<s2p::DeltaMoveRect> moves = { { 40, 40, 120, 140, 200, 100 } };
        bool kMove = false;
        CHECK(enc2.Encode(fB.data(), 640, 360, 640 * 4, {}, false, pMove, kMove, moves), "DLT2 MoveRect encode failed");
        CHECK(dec2.Decode(pMove.data(), pMove.size(), 640, 360), "DLT2 MoveRect decode failed");
        CHECK(std::memcmp(fB.data(), dec2.FrameData(), fB.size()) == 0, "DLT2 MoveRect round-trip mismatch");
        CHECK(dec2.LastBoundingRect().x == 120 && dec2.LastBoundingRect().y == 140 &&
              dec2.LastBoundingRect().w == 200 && dec2.LastBoundingRect().h == 100,
              "DLT2 LastBoundingRect mismatch after MoveRect");

        // Fast-motion 2x2 (>35% dirty area) followed by 100% lossless settle
        std::vector<uint8_t> fC;
        FillPatternBgra(fC, 640, 360, 640 * 4, 99);
        std::vector<s2p::DeltaRect> fullHint = { { 0, 0, 640, 360 } };
        bool kFast = false, usedFast = false;
        CHECK(enc2.Encode(fC.data(), 640, 360, 640 * 4, fullHint, false, pFast, kFast, {}, true, &usedFast),
              "DLT2 fast-motion encode failed");
        CHECK(usedFast, "DLT2 fast-motion flag not triggered on >35% dirty area");
        CHECK(dec2.Decode(pFast.data(), pFast.size(), 640, 360), "DLT2 fast-motion decode failed");
        CHECK(dec2.LastDecodeUsedFastMotion(), "DLT2 decoder did not report fast-motion");

        bool kSettle = false, usedFastSettle = false;
        CHECK(enc2.Encode(fC.data(), 640, 360, 640 * 4, fullHint, false, pSettle, kSettle, {}, false, &usedFastSettle),
              "DLT2 lossless settle encode failed");
        CHECK(!usedFastSettle, "DLT2 settle should not use fast-motion");
        CHECK(dec2.Decode(pSettle.data(), pSettle.size(), 640, 360), "DLT2 lossless settle decode failed");
        CHECK(std::memcmp(fC.data(), dec2.FrameData(), fC.size()) == 0,
              "DLT2 lossless settle did not restore 100% bit-exact frame");

        // Regression: fast-motion 2x2 followed by a small partial 1:1 frame (e.g. mouse move)
        // must still allow a subsequent settle pass to restore 100% bit-exactness across the full frame.
        std::vector<uint8_t> fD, pFast2, pMouse, pSettle2;
        FillPatternBgra(fD, 640, 360, 640 * 4, 155);
        bool kFast2 = false, usedFast2 = false;
        CHECK(enc2.Encode(fD.data(), 640, 360, 640 * 4, fullHint, false, pFast2, kFast2, {}, true, &usedFast2) && usedFast2,
              "DLT2 second fast-motion encode failed");
        CHECK(dec2.Decode(pFast2.data(), pFast2.size(), 640, 360), "DLT2 second fast-motion decode failed");

        for (uint32_t y = 10; y < 42; ++y) {
            for (uint32_t x = 10; x < 42; ++x) {
                size_t idx = static_cast<size_t>(y) * 640 * 4 + x * 4;
                fD[idx + 0] = 0xFE;
                fD[idx + 1] = 0x01;
                fD[idx + 2] = 0x77;
                fD[idx + 3] = 0xFF;
            }
        }
        std::vector<s2p::DeltaRect> smallHint = { { 10, 10, 32, 32 } };
        bool kMouse = false, usedFastMouse = true;
        CHECK(enc2.Encode(fD.data(), 640, 360, 640 * 4, smallHint, false, pMouse, kMouse, {}, true, &usedFastMouse) && !usedFastMouse,
              "DLT2 small mouse-area encode after fast-motion failed");
        CHECK(dec2.Decode(pMouse.data(), pMouse.size(), 640, 360), "DLT2 small mouse-area decode failed");

        bool kSettle2 = false, usedFastSettle2 = true;
        CHECK(enc2.Encode(fD.data(), 640, 360, 640 * 4, fullHint, false, pSettle2, kSettle2, {}, false, &usedFastSettle2) && !usedFastSettle2,
              "DLT2 post-mouse settle encode failed");
        CHECK(dec2.Decode(pSettle2.data(), pSettle2.size(), 640, 360), "DLT2 post-mouse settle decode failed");
        CHECK(std::memcmp(fD.data(), dec2.FrameData(), fD.size()) == 0,
              "DLT2 post-mouse settle did not restore 100% bit-exact frame");
    }

    std::cout << "[PASS] TestDeltaCodec\n";
}

void TestColorAndH264() {
    auto testRes = [](uint32_t w, uint32_t h) {
        std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4, 0);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
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
        s2p::H264Decoder decCpuOnly;
        bool encInit = enc.Init(nullptr, nullptr, w, h, 30, 15);
        bool decInit = dec.Init(nullptr, nullptr, w, h);
        bool decCpuInit = decCpuOnly.Init(nullptr, nullptr, w, h, false);
        CHECK(encInit, "H264Encoder CPU Init failed");
        CHECK(decInit, "H264Decoder CPU Init failed");
        CHECK(decCpuInit && !decCpuOnly.IsHardwareDxva(), "H264Decoder allowHardware=false check failed");

        if (encInit && decInit) {
            bool gotEncoded = false;
            bool gotDecoded = false;
            bool gotCpuDecoded = false;
            std::vector<uint8_t> outBgra;

            for (int f = 0; f < 10; ++f) {
                std::vector<uint8_t> annexB;
                bool isKey = false;
                if (f == 0) enc.RequestKeyframe();
                if (enc.EncodeBgra(bgra.data(), w * 4, static_cast<uint64_t>(f) * 33333ULL, annexB, isKey) && !annexB.empty()) {
                    gotEncoded = true;
                    uint32_t outW = 0, outH = 0;
                    if (!gotDecoded &&
                        dec.Decode(annexB.data(), annexB.size(), static_cast<uint64_t>(f) * 33333ULL,
                                   nullptr, &outBgra, &outW, &outH) && !outBgra.empty()) {
                        CHECK(outW == w && outH == h, "H264 decoded dimensions mismatch");
                        CHECK(outBgra.size() == static_cast<size_t>(w) * h * 4, "H264 decoded buffer byte count mismatch");
                        size_t center = (static_cast<size_t>(h / 2) * w + (w / 2)) * 4;
                        CHECK(outBgra[center + 1] > 150, "H264 decoded center pixel luma unexpectedly dark");
                        gotDecoded = true;
                    }
                    if (!gotCpuDecoded && decCpuInit) {
                        std::vector<uint8_t> cpuOut;
                        uint32_t cw = 0, ch = 0;
                        if (decCpuOnly.Decode(annexB.data(), annexB.size(), static_cast<uint64_t>(f) * 33333ULL,
                                              nullptr, &cpuOut, &cw, &ch) && !cpuOut.empty()) {
                            CHECK(cw == w && ch == h, "H264Decoder CPU-fallback decoded dimensions mismatch");
                            gotCpuDecoded = true;
                        }
                    }
                    if (gotDecoded && (!decCpuInit || gotCpuDecoded)) {
                        break;
                    }
                }
            }
            CHECK(gotEncoded, "H264Encoder produced no Annex-B output");
            CHECK(gotDecoded, "H264Decoder produced no decoded frame");
            CHECK(gotCpuDecoded, "H264Decoder allowHardware=false produced no decoded frame");
        }

        enc.Shutdown();
        dec.Shutdown();
        decCpuOnly.Shutdown();
    };

    // Verify EnsureLowLatencyH264Stream patches Baseline SPS (profile_idc=66) with constraint_set0|1 (0xC0)
    {
        std::vector<uint8_t> sps = { 0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x28, 0x95, 0xA0, 0x1E, 0x00, 0x89, 0xF9, 0x61 };
        s2p::EnsureLowLatencyH264Stream(sps);
        CHECK((sps[6] & 0xC0u) == 0xC0u, "EnsureLowLatencyH264Stream did not set constraint_set0|1 on Baseline SPS");
    }

    // Neutral gray (128, 128, 128) BT.709 Studio round-trip must have zero chromatic cast
    {
        const uint32_t gw = 16, gh = 16;
        std::vector<uint8_t> grayBgra(gw * gh * 4, 0);
        for (size_t i = 0; i < gw * gh; ++i) {
            grayBgra[i * 4 + 0] = 128;
            grayBgra[i * 4 + 1] = 128;
            grayBgra[i * 4 + 2] = 128;
            grayBgra[i * 4 + 3] = 255;
        }
        std::vector<uint8_t> grayNv12(gw * gh * 3 / 2);
        std::vector<uint8_t> grayRt(gw * gh * 4);
        s2p::CpuBgraToNv12(grayBgra.data(), gw * 4, gw, gh, grayNv12.data());
        s2p::CpuNv12ToBgra(grayNv12.data(), gw, grayNv12.data() + gw * gh, gw, gw, gh, grayRt.data());
        CHECK(std::abs(static_cast<int>(grayRt[0]) - 128) <= 2 &&
              std::abs(static_cast<int>(grayRt[1]) - 128) <= 2 &&
              std::abs(static_cast<int>(grayRt[2]) - 128) <= 2,
              "BT.709 Studio neutral gray BGRA<->NV12 round-trip shifted");
        CHECK(grayRt[0] == grayRt[1] && grayRt[1] == grayRt[2],
              "BT.709 Studio neutral gray introduced color tint");
    }

    CHECK(std::strcmp(SCREEN2PC_VERSION_RC_STR, SCREEN2PC_VERSION_STR) == 0,
          "Version.h RC string macro diverges from C++ version string");

    testRes(640, 360);
    testRes(1920, 1080); // Macroblock-padded 1088 -> cropped 1080 check
    std::cout << "[PASS] TestColorAndH264\n";
}

void TestLoopbackNetworking() {
    const uint16_t testTcpPort = 48299;
    const uint16_t testUdpPort = 48298;

    // 1. TCP Packet framing roundtrip (small + large > 1536 bytes)
    s2p::TcpSocket listener;
    CHECK(listener.Listen(testTcpPort, 2), "TcpSocket::Listen failed on loopback test port");

    std::thread serverThread([&]() {
        s2p::TcpSocket srv = listener.Accept(nullptr, 3000);
        if (!srv.IsValid()) return;
        for (int i = 0; i < 2; ++i) {
            s2p::PacketHeader hdr{};
            std::vector<uint8_t> payload;
            if (srv.WaitReadable(2000) == 1 && srv.RecvPacket(hdr, payload) &&
                hdr.type == static_cast<uint16_t>(s2p::MsgType::VideoFrame)) {
                srv.SendPacket(s2p::MsgType::SessionAck, payload.data(), static_cast<uint32_t>(payload.size()), hdr.reserved);
            }
        }
        srv.Close();
    });

    s2p::TcpSocket client;
    CHECK(client.Connect("127.0.0.1", testTcpPort, 2000), "TcpSocket::Connect 127.0.0.1 failed");
    const std::vector<uint8_t> sample = { 1, 2, 3, 4, 5, 42, 99 };
    CHECK(client.SendPacket(s2p::MsgType::VideoFrame, sample.data(), static_cast<uint32_t>(sample.size()), 77),
          "TcpSocket::SendPacket small failed");

    s2p::PacketHeader ackHdr{};
    std::vector<uint8_t> ackPayload;
    CHECK(client.RecvPacket(ackHdr, ackPayload), "TcpSocket::RecvPacket small failed");
    CHECK(ackHdr.type == static_cast<uint16_t>(s2p::MsgType::SessionAck), "Wrong echo MsgType");
    CHECK(ackHdr.reserved == 77, "Wrong header reserved field");
    CHECK(ackPayload == sample, "Echoed small TCP payload corrupted");

    std::vector<uint8_t> largeSample(16384);
    for (size_t i = 0; i < largeSample.size(); ++i) largeSample[i] = static_cast<uint8_t>((i * 13) & 0xFF);
    CHECK(client.SendPacket(s2p::MsgType::VideoFrame, largeSample.data(), static_cast<uint32_t>(largeSample.size()), 88),
          "TcpSocket::SendPacket large failed");
    CHECK(client.RecvPacket(ackHdr, ackPayload), "TcpSocket::RecvPacket large failed");
    CHECK(ackHdr.reserved == 88 && ackPayload == largeSample, "Echoed large TCP payload corrupted");

    client.Close();
    listener.Close();
    serverThread.join();

    // 1b. Cancelable TcpSocket::Connect aborts cleanly
    {
        std::atomic<bool> cancel{ true };
        s2p::TcpSocket canceledCli;
        CHECK(!canceledCli.Connect("127.0.0.1", testTcpPort, 5000, &cancel),
              "Canceled TcpSocket::Connect should return false immediately");
    }

    // 2. UDP Loopback packet roundtrip + bounds check
    s2p::UdpSocket udpSrv;
    s2p::UdpSocket udpCli;
    CHECK(udpSrv.Bind(testUdpPort, true), "UdpSocket::Bind failed");
    CHECK(udpCli.CreateForBroadcast(), "UdpSocket::CreateForBroadcast failed");

    const char msg[] = "S2P_PING";
    CHECK(udpCli.SendTo("127.0.0.1", testUdpPort, msg, sizeof(msg)), "UdpSocket::SendTo failed");
    CHECK(!udpCli.SendTo("127.0.0.1", testUdpPort, msg, 70000), "Oversized UDP SendTo accepted");
    char recvBuf[64]{};
    std::string fromIp;
    uint16_t fromPort = 0;
    int recvd = udpSrv.RecvFrom(recvBuf, sizeof(recvBuf), fromIp, fromPort, 1000);
    CHECK(recvd == sizeof(msg) && std::memcmp(recvBuf, msg, sizeof(msg)) == 0, "UDP loopback mismatch");

    std::cout << "[PASS] TestLoopbackNetworking\n";
}

void TestClipboardSync() {
    const wchar_t text1[] = L"Hallo Screen2PC Zwischenablage!";
    const wchar_t text2[] = L"Anderer Text";

    uint64_t h1 = s2p::ClipboardSync::ComputeHash64(text1, sizeof(text1));
    uint64_t h1Again = s2p::ClipboardSync::ComputeHash64(text1, sizeof(text1));
    uint64_t h2 = s2p::ClipboardSync::ComputeHash64(text2, sizeof(text2));

    CHECK(h1 != 0, "Clipboard hash must be non-zero");
    CHECK(h1 == h1Again, "Clipboard hash must be deterministic");
    CHECK(h1 != h2, "Different clipboard contents collided unexpectedly");

    // Anti-echo loop test
    CHECK(!s2p::ClipboardSync::ShouldEmit(h1, h1), "Remote-applied clipboard hash echoed back");
    CHECK(!s2p::ClipboardSync::ShouldEmit(0, h1), "Zero hash must not emit");
    CHECK(s2p::ClipboardSync::ShouldEmit(h2, h1), "New local clipboard content was suppressed");

    // ClipboardHeader struct serialization check
    s2p::ClipboardHeader ch{};
    ch.kind = s2p::kClipKindText;
    ch.hash64 = h1;
    ch.dataSize = sizeof(text1);
    CHECK(sizeof(ch) == 16, "Unexpected ClipboardHeader packed size");

    // NormalizeTextPayload hardening checks
    std::vector<uint8_t> normOut;
    const uint8_t oddNonNull[3] = { 0x41, 0x00, 0x42 };
    CHECK(s2p::ClipboardSync::NormalizeTextPayload(oddNonNull, sizeof(oddNonNull), normOut),
          "NormalizeTextPayload failed on odd non-null buffer");
    CHECK(normOut.size() == 6 && normOut[4] == 0 && normOut[5] == 0,
          "NormalizeTextPayload did not align odd text and append UTF-16 NUL");

    const uint8_t nullTerm[4] = { 0x41, 0x00, 0x00, 0x00 };
    CHECK(s2p::ClipboardSync::NormalizeTextPayload(nullTerm, sizeof(nullTerm), normOut) && normOut.size() == 4,
          "NormalizeTextPayload altered already NUL-terminated UTF-16 text");
    CHECK(!s2p::ClipboardSync::NormalizeTextPayload(nullptr, 4, normOut),
          "NormalizeTextPayload accepted nullptr");
    CHECK(!s2p::ClipboardSync::NormalizeTextPayload(nullTerm, 0, normOut),
          "NormalizeTextPayload accepted empty size");
    CHECK(!s2p::ClipboardSync::NormalizeTextPayload(nullTerm, s2p::kMaxClipboardTextBytes + 2, normOut),
          "NormalizeTextPayload accepted >kMaxClipboardTextBytes");

    std::cout << "[PASS] TestClipboardSync\n";
}

void TestFileTransfer() {
    // 1. Path traversal / illegal char sanitization
    CHECK(s2p::FileTransfer::SanitizeFilename(L"..\\..\\Windows\\System32\\cmd.exe") == L"cmd.exe",
          "Backslash path traversal not stripped");
    CHECK(s2p::FileTransfer::SanitizeFilename(L"../../etc/passwd") == L"passwd",
          "Slash path traversal not stripped");
    CHECK(s2p::FileTransfer::SanitizeFilename(L"C:evil:name*?.txt") == L"evil_name__.txt",
          "Illegal Windows chars not sanitized");
    CHECK(s2p::FileTransfer::SanitizeFilename(L"..") == L"received_file.bin",
          "Dotdot filename not replaced");

    // 2. CRC32 check ("123456789" -> 0xCBF43926)
    const char digits[] = "123456789";
    uint32_t crc = s2p::FileTransfer::ComputeCrc32(digits, 9);
    CHECK(crc == 0xCBF43926u, "CRC32 standard check vector failed");

    // 3. End-to-end multi-chunk (200 KB) bidirectional loopback TCP transfer
    wchar_t tempBase[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempBase);
    std::wstring srcDir = std::wstring(tempBase) + L"s2p_test_src_" + std::to_wstring(GetCurrentProcessId());
    std::wstring dstDir = std::wstring(tempBase) + L"s2p_test_dst_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(srcDir.c_str(), nullptr);
    CreateDirectoryW(dstDir.c_str(), nullptr);

    std::wstring srcFile = srcDir + L"\\sample_payload.bin";
    std::wstring revSrcFile = dstDir + L"\\reverse_payload.bin";
    const size_t fileBytes = 200 * 1024 + 137; // > 3 chunks (64 KB) + unaligned tail
    const size_t revBytes = 95 * 1024 + 41;
    std::vector<uint8_t> expected(fileBytes);
    for (size_t i = 0; i < fileBytes; ++i) {
        expected[i] = static_cast<uint8_t>((i * 29 + 7) & 0xFF);
    }
    std::vector<uint8_t> revExpected(revBytes);
    for (size_t i = 0; i < revBytes; ++i) {
        revExpected[i] = static_cast<uint8_t>((i * 17 + 3) & 0xFF);
    }

    auto writeTestFile = [](const std::wstring& path, const std::vector<uint8_t>& data) {
        HANDLE hOut = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(hOut != INVALID_HANDLE_VALUE, "Could not create temp source file");
        if (hOut != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(hOut, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
            CloseHandle(hOut);
        }
    };
    writeTestFile(srcFile, expected);
    writeTestFile(revSrcFile, revExpected);

    const uint16_t ftPort = 48297;
    s2p::TcpSocket listener;
    CHECK(listener.Listen(ftPort, 2), "FileTransfer loopback listener failed");

    s2p::TcpSocket srvSock;
    std::thread acceptThread([&]() {
        srvSock = listener.Accept(nullptr, 3000);
    });

    s2p::TcpSocket cliSock;
    CHECK(cliSock.Connect("127.0.0.1", ftPort, 2000), "FileTransfer loopback connect failed");
    acceptThread.join();
    listener.Close();
    CHECK(srvSock.IsValid(), "FileTransfer server accept invalid");

    s2p::FileTransfer senderFt;
    s2p::FileTransfer receiverFt;
    senderFt.Start(std::move(cliSock), srcDir);
    receiverFt.Start(std::move(srvSock), dstDir);

    // Simultaneous full-duplex transfer in both directions at once
    CHECK(senderFt.QueueFiles({ srcFile }) == 1, "QueueFiles failed to enqueue test file");
    CHECK(receiverFt.QueueFiles({ revSrcFile }) == 1, "QueueFiles failed to enqueue reverse test file");

    bool done = false;
    for (int i = 0; i < 80; ++i) {
        if (receiverFt.GetStats().filesReceived >= 1 && senderFt.GetStats().filesReceived >= 1) {
            done = true;
            break;
        }
        Sleep(50);
    }
    CHECK(done, "Bidirectional loopback file transfer did not finish in time");
    CHECK(senderFt.GetStats().filesSent == 1 && receiverFt.GetStats().filesSent == 1, "filesSent count mismatch");
    CHECK(receiverFt.GetStats().bytesReceived == fileBytes, "Receiver bytesReceived mismatch");
    CHECK(senderFt.GetStats().bytesReceived == revBytes, "Sender reverse bytesReceived mismatch");

    // Verify exact file bytes on disk for both directions
    auto verifyDiskFile = [](const std::wstring& path, const std::vector<uint8_t>& exp) {
        HANDLE hIn = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(hIn != INVALID_HANDLE_VALUE, "Received destination file missing");
        if (hIn != INVALID_HANDLE_VALUE) {
            std::vector<uint8_t> actual(exp.size());
            DWORD readBytes = 0;
            ReadFile(hIn, actual.data(), static_cast<DWORD>(actual.size()), &readBytes, nullptr);
            CloseHandle(hIn);
            CHECK(readBytes == exp.size() && actual == exp, "Received file content corrupted");
        }
    };
    std::wstring dstFile = dstDir + L"\\sample_payload.bin";
    std::wstring revDstFile = srcDir + L"\\reverse_payload.bin";
    verifyDiskFile(dstFile, expected);
    verifyDiskFile(revDstFile, revExpected);

    // Also test unique destination collision naming when same filename already exists
    std::wstring collisionPath = s2p::FileTransfer::ResolveUniqueDestPath(dstDir, L"sample_payload.bin");
    CHECK(collisionPath.find(L"sample_payload (1).bin") != std::wstring::npos, "Unique collision suffix (1) failed");

    // Verify clean peer-initiated TCP EOF exits RecvWorker without spinning/hanging
    senderFt.Stop();
    bool peerClosedDetected = false;
    for (int i = 0; i < 40; ++i) {
        if (!receiverFt.IsConnected() && !receiverFt.GetStats().active) {
            peerClosedDetected = true;
            break;
        }
        Sleep(25);
    }
    CHECK(peerClosedDetected, "FileTransfer did not detect peer TCP EOF cleanly");

    receiverFt.Stop();
    DeleteFileW(srcFile.c_str());
    DeleteFileW(revSrcFile.c_str());
    DeleteFileW(dstFile.c_str());
    DeleteFileW(revDstFile.c_str());
    RemoveDirectoryW(srcDir.c_str());
    RemoveDirectoryW(dstDir.c_str());

    std::cout << "[PASS] TestFileTransfer\n";
}

void TestKvmInput() {
    float nx = -1.0f, ny = -1.0f;

    // 1. Exact 16:9 inside 16:9
    CHECK(s2p::KvmInput::MapWindowClientToDisplayNorm(960, 540, 1920, 1080, 1920, 1080, nx, ny),
          "16:9 center should be inside");
    CHECK(std::abs(nx - 0.5f) < 0.001f && std::abs(ny - 0.5f) < 0.001f, "16:9 center norm mismatch");

    // 2. Pillarbox: 16:9 (1600x900) inside ultrawide 2560x900 -> 480px black bars left & right
    CHECK(!s2p::KvmInput::MapWindowClientToDisplayNorm(200, 450, 2560, 900, 1920, 1080, nx, ny),
          "Left pillarbox bar should return inside=false");
    CHECK(nx == 0.0f && std::abs(ny - 0.5f) < 0.001f, "Left pillarbox clamp mismatch");
    CHECK(s2p::KvmInput::MapWindowClientToDisplayNorm(1280, 450, 2560, 900, 1920, 1080, nx, ny),
          "Pillarbox center should return inside=true");
    CHECK(std::abs(nx - 0.5f) < 0.001f && std::abs(ny - 0.5f) < 0.001f, "Pillarbox center mismatch");

    // 3. Letterbox: 16:9 (1600x900) inside 1600x1200 -> 150px black bars top & bottom
    CHECK(!s2p::KvmInput::MapWindowClientToDisplayNorm(800, 50, 1600, 1200, 1920, 1080, nx, ny),
          "Top letterbox bar should return inside=false");
    CHECK(ny == 0.0f && std::abs(nx - 0.5f) < 0.001f, "Top letterbox clamp mismatch");

    // 4. Virtual desktop multi-monitor mapping (Surface 2256x1504 + Virtual Mon1 2560x1440 + Virtual Mon2 1920x1080)
    int32_t dx = -1, dy = -1;
    RECT rightMon{ 2256, 0, 2256 + 2560, 1440 };
    const int32_t totalW = 2256 + 2560 + 1920; // 6736
    const int32_t totalH = 1504;
    s2p::KvmInput::ComputeVirtualDesktopAbs(rightMon, 0.0f, 0.0f, 0, 0, totalW, totalH, dx, dy);
    CHECK(dx > 21000 && dx < 23000 && dy == 0, "Virtual 2K monitor top-left mapping out of expected range");

    RECT mon2{ 2256 + 2560, 0, totalW, 1080 };
    s2p::KvmInput::ComputeVirtualDesktopAbs(mon2, 1.0f, 1.0f, 0, 0, totalW, totalH, dx, dy);
    CHECK(dx == 65535 && dy > 46000 && dy < 48000, "Virtual 1080p bottom-right mapping mismatch");

    // 5. Negative left monitor mapping (-1920..0 left of primary 0..1920)
    RECT leftMon{ -1920, 0, 0, 1080 };
    s2p::KvmInput::ComputeVirtualDesktopAbs(leftMon, 0.0f, 0.5f, -1920, 0, 3840, 1080, dx, dy);
    CHECK(dx == 0 && std::abs(dy - 32768) <= 32, "Negative-left monitor mapping mismatch");

    // 6. Full-desktop relative roaming across Surface (-2256..0, 1504h) + Mon1 (0..2560, 1440h) + Mon2 (2560..4480, 1080h)
    const RECT roamMons[3] = {
        RECT{ -2256, 0, 0, 1504 },
        RECT{ 0, 0, 2560, 1440 },
        RECT{ 2560, 0, 4480, 1080 }
    };
    const int32_t rLeft = -2256, rTop = 0, rW = 2256 + 2560 + 1920, rH = 1504;
    int32_t nxPx = 0, nyPx = 0;

    // Crossing left from Monitor 1 onto physical Surface monitor
    s2p::KvmInput::StepVirtualDesktopPoint(10, 700, -30, 5, roamMons, 3, rLeft, rTop, rW, rH, nxPx, nyPx, dx, dy);
    CHECK(nxPx == -20 && nyPx == 705, "Crossing left from PC monitor onto physical Surface monitor failed");
    CHECK(dx >= 0 && dx < 65535 && dy > 0 && dy < 65535, "Virtual desktop normalized coord out of range on Surface");

    // Crossing right from bottom of 1440p Monitor 1 onto shorter 1080p Monitor 2 snaps cleanly to y=1079
    s2p::KvmInput::StepVirtualDesktopPoint(2550, 1300, 30, 0, roamMons, 3, rLeft, rTop, rW, rH, nxPx, nyPx, dx, dy);
    CHECK(nxPx == 2580 && nyPx == 1079, "Snapping onto shorter adjacent monitor height failed");

    // Bumping bottom edge of Monitor 2 right near the seam stays on Monitor 2 instead of warping sideways
    s2p::KvmInput::StepVirtualDesktopPoint(2565, 1075, 0, 20, roamMons, 3, rLeft, rTop, rW, rH, nxPx, nyPx, dx, dy);
    CHECK(nxPx == 2565 && nyPx == 1079, "Bottom edge push near seam wrongly jumped sideways");

    // Crossing right from bottom of 1504p Surface onto 1440p Monitor 1 snaps cleanly to y=1439
    s2p::KvmInput::StepVirtualDesktopPoint(-10, 1490, 30, 0, roamMons, 3, rLeft, rTop, rW, rH, nxPx, nyPx, dx, dy);
    CHECK(nxPx == 20 && nyPx == 1439, "Crossing right from Surface bottom onto Monitor 1 failed");

    // Vertical stack: crossing down from wider 1440p top monitor onto narrower 1080p bottom monitor
    const RECT stackedMons[2] = {
        RECT{ 0, 0, 2560, 1440 },
        RECT{ 0, 1440, 1920, 2520 }
    };
    s2p::KvmInput::StepVirtualDesktopPoint(2200, 1430, 0, 20, stackedMons, 2, 0, 0, 2560, 2520, nxPx, nyPx, dx, dy);
    CHECK(nxPx == 1919 && nyPx == 1450, "Vertical uneven-width monitor crossing failed");

    // Clamping at outer left edge of physical Surface monitor
    s2p::KvmInput::StepVirtualDesktopPoint(-2240, 100, -100, -200, roamMons, 3, rLeft, rTop, rW, rH, nxPx, nyPx, dx, dy);
    CHECK(nxPx == -2256 && nyPx == 0 && dx == 0 && dy == 0, "Outer top-left virtual desktop clamping failed");

    // Clamping at outer right edge of Monitor 2
    s2p::KvmInput::StepVirtualDesktopPoint(4460, 1060, 100, 100, roamMons, 3, rLeft, rTop, rW, rH, nxPx, nyPx, dx, dy);
    CHECK(nxPx == 4479 && nyPx == 1079 && dx == 65535, "Outer bottom-right Monitor 2 clamping failed");

    // Extreme INT32 deltas & NaN/Inf inputs must stay bounded cleanly without overflow
    s2p::KvmInput::StepVirtualDesktopPoint(-2000, 500, INT32_MIN, INT32_MAX, roamMons, 3, rLeft, rTop, rW, rH, nxPx, nyPx, dx, dy);
    CHECK(nxPx >= rLeft && nxPx < 4480 && nyPx >= 0 && nyPx < 1504 && dx >= 0 && dx <= 65535 && dy >= 0 && dy <= 65535,
          "Extreme INT32 deltas caused out-of-range virtual desktop point");

    s2p::KvmInput::ComputeVirtualDesktopAbs(rightMon,
                                            std::numeric_limits<float>::quiet_NaN(),
                                            std::numeric_limits<float>::infinity(),
                                            0, 0, totalW, totalH, dx, dy);
    CHECK(dx >= 0 && dx <= 65535 && dy >= 0 && dy <= 65535, "Non-finite norm coords produced out-of-range abs coord");

    s2p::KvmMouseEvent nanEv{};
    nanEv.action = s2p::kMouseMove;
    nanEv.isRelative = 1;
    nanEv.normX = std::numeric_limits<float>::quiet_NaN();
    nanEv.normY = -std::numeric_limits<float>::infinity();
    CHECK(!s2p::KvmInput::InjectMouse(nanEv, rightMon), "Non-finite InjectMouse event unexpectedly succeeded");

    // 5. Independent KVM mouse & scroll speed scaling (25..500%)
    CHECK(s2p::KvmInput::ClampSpeedPct(0) == s2p::KvmInput::kMinSpeedPct &&
          s2p::KvmInput::ClampSpeedPct(10) == s2p::KvmInput::kMinSpeedPct &&
          s2p::KvmInput::ClampSpeedPct(999) == s2p::KvmInput::kMaxSpeedPct &&
          s2p::KvmInput::ClampSpeedPct(175) == 175,
          "ClampSpeedPct bounds failed");

    double ax = 0.0, ay = 0.0;
    int32_t sdx = 0, sdy = 0;
    s2p::KvmInput::ScaleRelativeDelta(3, -4, 100, ax, ay, sdx, sdy);
    CHECK(sdx == 3 && sdy == -4 && ax == 0.0 && ay == 0.0, "ScaleRelativeDelta 100% failed");

    // 150% fractional accumulation: (1,1) -> (1,1) rem 0.5; (1,1) -> (2,2) rem 0.0
    s2p::KvmInput::ScaleRelativeDelta(1, 1, 150, ax, ay, sdx, sdy);
    CHECK(sdx == 1 && sdy == 1, "ScaleRelativeDelta 150% step 1 failed");
    s2p::KvmInput::ScaleRelativeDelta(1, 1, 150, ax, ay, sdx, sdy);
    CHECK(sdx == 2 && sdy == 2 && std::abs(ax) < 1e-9 && std::abs(ay) < 1e-9,
          "ScaleRelativeDelta 150% step 2 accumulation failed");

    // Negative fractional accumulation at 150%
    s2p::KvmInput::ScaleRelativeDelta(-1, 0, 150, ax, ay, sdx, sdy);
    CHECK(sdx == -1 && sdy == 0, "ScaleRelativeDelta negative 150% step 1 failed");
    s2p::KvmInput::ScaleRelativeDelta(-1, 0, 150, ax, ay, sdx, sdy);
    CHECK(sdx == -2 && sdy == 0 && std::abs(ax) < 1e-9,
          "ScaleRelativeDelta negative 150% step 2 failed");

    // Slow 25% accumulation: 4x +1 raw -> 0, 0, 0, 1
    int sumSlow = 0;
    for (int i = 0; i < 4; ++i) {
        s2p::KvmInput::ScaleRelativeDelta(1, 0, 25, ax, ay, sdx, sdy);
        sumSlow += sdx;
    }
    CHECK(sumSlow == 1, "ScaleRelativeDelta 25% accumulation failed");

    // Scroll scaling
    double wAcc = 0.0;
    CHECK(s2p::KvmInput::ScaleScrollDelta(120, 200, wAcc) == 240, "ScaleScrollDelta 200% positive failed");
    CHECK(s2p::KvmInput::ScaleScrollDelta(-120, 200, wAcc) == -240, "ScaleScrollDelta 200% negative failed");
    CHECK(s2p::KvmInput::ScaleScrollDelta(120, 150, wAcc) == 180, "ScaleScrollDelta 150% failed");
    CHECK(s2p::KvmInput::ScaleScrollDelta(30, 50, wAcc) == 15, "ScaleScrollDelta 50% high-res failed");
    CHECK(s2p::KvmInput::ScaleScrollDelta(2, 25, wAcc) == 0, "ScaleScrollDelta 25% step 1 should accumulate");
    CHECK(s2p::KvmInput::ScaleScrollDelta(2, 25, wAcc) == 1, "ScaleScrollDelta 25% step 2 should emit 1");
    CHECK(s2p::KvmInput::ScaleScrollDelta(0, 200, wAcc) == 0, "ScaleScrollDelta 0 should emit 0");

    std::cout << "[PASS] TestKvmInput\n";
}

void TestAutostartAndCli() {
    CHECK(s2p::HasCliFlag(L"\"C:\\Program Files\\Screen2PC\\Screen2PC.Receiver.exe\" --TRAY", L"--tray"),
          "HasCliFlag should match case-insensitively");
    CHECK(!s2p::HasCliFlag(L"Screen2PC.Receiver.exe --other", L"--tray"),
          "HasCliFlag should not match absent flag");
    CHECK(s2p::HasTrayLaunchFlag(L"\"C:\\My App\\Screen2PC.Receiver.exe\" --tray"),
          "HasTrayLaunchFlag failed on --tray");
    CHECK(s2p::HasTrayLaunchFlag(L"Screen2PC.Receiver.exe -Minimized"),
          "HasTrayLaunchFlag failed on -Minimized");
    CHECK(!s2p::HasTrayLaunchFlag(L"Screen2PC.Receiver.exe"),
          "HasTrayLaunchFlag unexpectedly matched bare command");
    CHECK(!s2p::HasTrayLaunchFlag(nullptr),
          "HasTrayLaunchFlag should handle nullptr safely");

    std::wstring cmd = s2p::BuildAutostartCommand(L"C:\\Tools\\Screen 2 PC\\Screen2PC.Receiver.exe");
    CHECK(cmd == L"\"C:\\Tools\\Screen 2 PC\\Screen2PC.Receiver.exe\" --tray",
          "BuildAutostartCommand quoting mismatch");
    CHECK(s2p::BuildAutostartCommand(L"").empty(), "Empty exe path must build empty command");

    CHECK(s2p::ExtractExePathFromCommand(cmd) == L"C:\\Tools\\Screen 2 PC\\Screen2PC.Receiver.exe",
          "ExtractExePathFromCommand failed on quoted command with args");
    CHECK(s2p::ExtractExePathFromCommand(L"\"C:\\App\\rec.exe\"") == L"C:\\App\\rec.exe",
          "ExtractExePathFromCommand failed on quoted bare command");
    CHECK(s2p::ExtractExePathFromCommand(L"C:\\App\\rec.exe --tray") == L"C:\\App\\rec.exe",
          "ExtractExePathFromCommand failed on unquoted command with args");
    CHECK(s2p::ExtractExePathFromCommand(L"  C:\\App\\rec.exe  ") == L"C:\\App\\rec.exe",
          "ExtractExePathFromCommand failed on trimmed bare command");
    CHECK(s2p::ExtractExePathFromCommand(L"").empty(),
          "ExtractExePathFromCommand failed on empty command");

    CHECK(std::wstring(s2p::kAutostartValueName) == L"Screen2PC Receiver" &&
          std::wstring(s2p::kAutostartSenderValueName) == L"Screen2PC Sender" &&
          std::wstring(s2p::kAutostartValueName) != std::wstring(s2p::kAutostartSenderValueName),
          "Autostart registry value names for Receiver and Sender must be non-empty and distinct");

    std::cout << "[PASS] TestAutostartAndCli\n";
}

void TestHdrColorPipeline() {
    s2p::MonitorColorConfig defCfg{};

    // 1. SDR identity (osSdrWhiteScale ignored in SDR mode)
    auto sdrCb = s2p::ComputeColorShaderConstants(defCfg, false, 3.0f, false);
    CHECK(std::abs(sdrCb.params0[0] - 1.0f) < 1e-5f &&
          std::abs(sdrCb.params0[3] - 1.0f) < 1e-5f &&
          sdrCb.params1[3] < 0.5f,
          "SDR default shader constants mismatch");
    float r = 0.0f, g = 0.0f, b = 0.0f;
    s2p::EvaluateColorPixelCpu(0.25f, 0.50f, 0.80f, sdrCb, r, g, b);
    CHECK(std::abs(r - 0.25f) < 1e-4f && std::abs(g - 0.50f) < 1e-4f && std::abs(b - 0.80f) < 1e-4f,
          "SDR identity color transform altered pixel values");

    // 2. HDR scRGB + pure Gamma 2.2 + OS SDR White Scale (e.g. 240 nits -> 3.0x 80 nits)
    auto hdrCb = s2p::ComputeColorShaderConstants(defCfg, true, 3.0f, false);
    CHECK(std::abs(hdrCb.params0[0] - 2.20f) < 1e-4f &&
          std::abs(hdrCb.params0[3] - 3.0f) < 1e-4f &&
          hdrCb.params1[3] > 0.5f,
          "HDR scRGB shader constants mismatch");

    s2p::EvaluateColorPixelCpu(0.0f, 0.0f, 0.0f, hdrCb, r, g, b);
    CHECK(std::abs(r) < 1e-5f && std::abs(g) < 1e-5f && std::abs(b) < 1e-5f,
          "HDR black level did not stay at 0");

    s2p::EvaluateColorPixelCpu(1.0f, 1.0f, 1.0f, hdrCb, r, g, b);
    CHECK(std::abs(r - 3.0f) < 1e-4f && std::abs(g - 3.0f) < 1e-4f && std::abs(b - 3.0f) < 1e-4f,
          "HDR peak SDR white did not scale to OS SDR white level");

    const float expectedMid = std::pow(0.5f, 2.20f) * 3.0f;
    s2p::EvaluateColorPixelCpu(0.5f, 0.5f, 0.5f, hdrCb, r, g, b);
    CHECK(std::abs(r - expectedMid) < 1e-4f && std::abs(g - expectedMid) < 1e-4f && std::abs(b - expectedMid) < 1e-4f,
          "HDR mid-gray Gamma 2.2 EOTF mismatch");

    // 3. Cursor white-level parity (forces contrast/saturation = 1.0, keeps brightness & gains)
    s2p::MonitorColorConfig tunedCfg = defCfg;
    tunedCfg.brightnessPct = 120.0f;
    tunedCfg.contrastPct = 130.0f;
    tunedCfg.saturationPct = 50.0f;
    auto cursorCb = s2p::ComputeColorShaderConstants(tunedCfg, true, 3.0f, true);
    CHECK(std::abs(cursorCb.params0[1] - 1.0f) < 1e-5f &&
          std::abs(cursorCb.params0[2] - 1.0f) < 1e-5f &&
          std::abs(cursorCb.params0[3] - 3.6f) < 1e-4f,
          "Cursor shader constants did not force neutral contrast/saturation");
    s2p::EvaluateColorPixelCpu(1.0f, 0.0f, 0.0f, cursorCb, r, g, b);
    CHECK(std::abs(r - 3.6f) < 1e-4f && std::abs(g) < 1e-5f && std::abs(b) < 1e-5f,
          "Cursor chromaticity/brightness parity mismatch");

    // 4. ClampColorConfig bounds
    s2p::MonitorColorConfig lowCfg{ false, 10.0f, 0.5f, 10.0f, 0.0f, 10.0f, 10.0f, 10.0f };
    s2p::ClampColorConfig(lowCfg);
    CHECK(lowCfg.brightnessPct == 50.0f && std::abs(lowCfg.gamma - 1.60f) < 1e-4f &&
          lowCfg.contrastPct == 60.0f && lowCfg.saturationPct == 50.0f &&
          lowCfg.redGainPct == 70.0f && lowCfg.greenGainPct == 70.0f && lowCfg.blueGainPct == 70.0f,
          "Lower ClampColorConfig bounds failed");

    s2p::MonitorColorConfig highCfg{ true, 500.0f, 5.0f, 300.0f, 300.0f, 300.0f, 300.0f, 300.0f };
    s2p::ClampColorConfig(highCfg);
    CHECK(highCfg.brightnessPct == 200.0f && std::abs(highCfg.gamma - 2.80f) < 1e-4f &&
          highCfg.contrastPct == 140.0f && highCfg.saturationPct == 160.0f &&
          highCfg.redGainPct == 130.0f && highCfg.greenGainPct == 130.0f && highCfg.blueGainPct == 130.0f,
          "Upper ClampColorConfig bounds failed");

    const float qnan = std::numeric_limits<float>::quiet_NaN();
    const float pinf = std::numeric_limits<float>::infinity();
    s2p::MonitorColorConfig nanCfg{ true, qnan, pinf, -pinf, qnan, qnan, pinf, -pinf };
    s2p::ClampColorConfig(nanCfg);
    CHECK(nanCfg.brightnessPct == 100.0f && std::abs(nanCfg.gamma - 2.20f) < 1e-4f &&
          nanCfg.contrastPct == 100.0f && nanCfg.saturationPct == 100.0f &&
          nanCfg.redGainPct == 100.0f && nanCfg.greenGainPct == 100.0f && nanCfg.blueGainPct == 100.0f,
          "Non-finite ClampColorConfig values were not reset to defaults");
    auto nanCb = s2p::ComputeColorShaderConstants(nanCfg, true, qnan, false);
    CHECK(std::isfinite(nanCb.params0[0]) && std::isfinite(nanCb.params0[3]) &&
          std::abs(nanCb.params0[3] - 1.0f) < 1e-4f,
          "Non-finite osSdrWhiteScale produced invalid shader constants");

    // 5. Direct ColorShaderConstants desaturation (saturation = 0 -> BT.709 luma)
    s2p::ColorShaderConstants grayCb{};
    grayCb.params0[0] = 1.0f;
    grayCb.params0[1] = 1.0f;
    grayCb.params0[2] = 0.0f;
    grayCb.params0[3] = 1.0f;
    grayCb.params1[0] = 1.0f;
    grayCb.params1[1] = 1.0f;
    grayCb.params1[2] = 1.0f;
    grayCb.params1[3] = 0.0f;
    s2p::EvaluateColorPixelCpu(0.8f, 0.4f, 0.2f, grayCb, r, g, b);
    const float expectedLuma = 0.2126f * 0.8f + 0.7152f * 0.4f + 0.0722f * 0.2f;
    CHECK(std::abs(r - expectedLuma) < 1e-4f && std::abs(r - g) < 1e-5f && std::abs(g - b) < 1e-5f,
          "BT.709 luma desaturation failed");

    // 6. INI persistence round-trip in %TEMP%
    wchar_t tempDir[MAX_PATH]{};
    DWORD tn = GetTempPathW(MAX_PATH, tempDir);
    CHECK(tn > 0 && tn < MAX_PATH, "GetTempPathW failed");
    std::wstring iniPath = std::wstring(tempDir) + L"s2p_color_test_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(iniPath.c_str());

    s2p::MonitorColorConfig saved{};
    saved.hdrOutputEnabled = false;
    saved.brightnessPct = 135.0f;
    saved.gamma = 2.35f;
    saved.contrastPct = 112.0f;
    saved.saturationPct = 85.0f;
    saved.redGainPct = 95.0f;
    saved.greenGainPct = 102.0f;
    saved.blueGainPct = 108.0f;
    s2p::SaveMonitorColorConfigIni(iniPath, 0, saved);

    s2p::MonitorColorConfig loaded = s2p::LoadMonitorColorConfigIni(iniPath, 0);
    CHECK(loaded.hdrOutputEnabled == false &&
          std::abs(loaded.brightnessPct - 135.0f) < 0.01f &&
          std::abs(loaded.gamma - 2.35f) < 0.01f &&
          std::abs(loaded.contrastPct - 112.0f) < 0.01f &&
          std::abs(loaded.saturationPct - 85.0f) < 0.01f &&
          std::abs(loaded.redGainPct - 95.0f) < 0.01f &&
          std::abs(loaded.greenGainPct - 102.0f) < 0.01f &&
          std::abs(loaded.blueGainPct - 108.0f) < 0.01f,
          "MonitorColorConfig INI round-trip mismatch");

    s2p::MonitorColorConfig otherMon = s2p::LoadMonitorColorConfigIni(iniPath, 1);
    CHECK(otherMon == defCfg, "Unconfigured monitor INI section did not return defaults");

    DeleteFileW(iniPath.c_str());
    std::cout << "[PASS] TestHdrColorPipeline\n";
}

void TestUpdaterParsing() {
    s2p::AppVersion v{};
    CHECK(s2p::ParseVersionString("v1.5.0.25", v) &&
          v.major == 1 && v.minor == 5 && v.patch == 0 && v.build == 25,
          "ParseVersionString failed on v1.5.0.25");
    CHECK(s2p::ParseVersionString("V2.10.3-b42", v) &&
          v.major == 2 && v.minor == 10 && v.patch == 3 && v.build == 42,
          "ParseVersionString failed on V2.10.3-b42");
    CHECK(s2p::ParseVersionString("1.5", v) &&
          v.major == 1 && v.minor == 5 && v.patch == 0 && v.build == 0,
          "ParseVersionString failed on short 1.5");
    CHECK(!s2p::ParseVersionString("abc", v) && !s2p::ParseVersionString("1", v),
          "ParseVersionString unexpectedly accepted invalid strings");

    s2p::AppVersion v25{ 1, 5, 0, 25 };
    s2p::AppVersion v24{ 1, 5, 0, 24 };
    s2p::AppVersion vNextMajor{ 2, 0, 0, 0 };
    s2p::AppVersion vNextPatch{ 1, 5, 1, 0 };
    CHECK(s2p::CompareVersions(v25, v24) > 0, "CompareVersions build > failed");
    CHECK(s2p::CompareVersions(v24, v25) < 0, "CompareVersions build < failed");
    CHECK(s2p::CompareVersions(v25, v25) == 0, "CompareVersions equal failed");
    CHECK(s2p::CompareVersions(vNextMajor, v25) > 0, "CompareVersions major failed");
    CHECK(s2p::CompareVersions(vNextPatch, v25) > 0, "CompareVersions patch failed");

    const std::string releaseJson = R"JSON({
        "url": "https://api.github.com/repos/Aminomed/Screen2PC/releases/1",
        "author": {
            "login": "github-actions[bot]",
            "tag_name": "v99.9.9.9",
            "name": "Wrong Nested Name",
            "draft": true
        },
        "tag_name": "v1.5.0.30",
        "name": null,
        "draft": false,
        "prerelease": false,
        "html_url": "https://github.com/Aminomed/Screen2PC/releases/tag/v1.5.0.30",
        "body": null,
        "assets": [
            {
                "name": "checksums.txt",
                "size": 128,
                "browser_download_url": "https://github.com/Aminomed/Screen2PC/releases/download/v1.5.0.30/checksums.txt"
            },
            {
                "name": "Screen2PC-Windows-x64.zip",
                "size": 1234567,
                "browser_download_url": "https://github.com/Aminomed/Screen2PC/releases/download/v1.5.0.30/Screen2PC-Windows-x64.zip"
            }
        ]
    })JSON";

    s2p::UpdateReleaseInfo info{};
    CHECK(s2p::ParseLatestReleaseJson(releaseJson, v25, info), "ParseLatestReleaseJson failed on valid release");
    CHECK(info.valid && info.updateAvailable, "Expected valid available update from v1.5.0.25 -> v1.5.0.30");
    CHECK(info.tagName == L"v1.5.0.30", "Depth-aware parser picked nested author.tag_name instead of root tag_name");
    CHECK(info.releaseName == L"v1.5.0.30", "Null name should fall back to tagName");
    CHECK(info.assetSize == 1234567ULL, "Asset size mismatch");
    CHECK(info.downloadUrl == L"https://github.com/Aminomed/Screen2PC/releases/download/v1.5.0.30/Screen2PC-Windows-x64.zip",
          "Asset ZIP selection mismatch");

    s2p::UpdateReleaseInfo sameInfo{};
    CHECK(s2p::ParseLatestReleaseJson(releaseJson, s2p::AppVersion{ 1, 5, 0, 30 }, sameInfo) &&
          sameInfo.valid && !sameInfo.updateAvailable,
          "Equal version must report updateAvailable=false");

    s2p::UpdateReleaseInfo newerLocal{};
    CHECK(s2p::ParseLatestReleaseJson(releaseJson, s2p::AppVersion{ 1, 5, 0, 31 }, newerLocal) &&
          newerLocal.valid && !newerLocal.updateAvailable,
          "Newer local build must report updateAvailable=false");

    const std::string draftJson = R"JSON({
        "tag_name": "v1.5.0.35",
        "draft": true,
        "assets": [
            { "name": "Screen2PC-Windows-x64.zip", "size": 100000, "browser_download_url": "https://example.com/a.zip" }
        ]
    })JSON";
    s2p::UpdateReleaseInfo draftInfo{};
    CHECK(!s2p::ParseLatestReleaseJson(draftJson, v25, draftInfo), "Draft release must be rejected");

    const std::string noAssetJson = R"JSON({
        "tag_name": "v1.5.0.35",
        "draft": false,
        "assets": []
    })JSON";
    s2p::UpdateReleaseInfo noAssetInfo{};
    CHECK(!s2p::ParseLatestReleaseJson(noAssetJson, v25, noAssetInfo), "Release without ZIP asset must be rejected");

    std::cout << "[PASS] TestUpdaterParsing\n";
}

void TestAppIcons() {
    // Fallback helpers must always return a non-null icon handle even without EXE resources.
    CHECK(s2p::LoadAppLargeIcon() != nullptr, "LoadAppLargeIcon fallback returned null");
    CHECK(s2p::LoadAppSmallIcon() != nullptr, "LoadAppSmallIcon fallback returned null");

    wchar_t modulePath[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    CHECK(n > 0 && n < MAX_PATH, "GetModuleFileNameW failed");
    std::wstring exeDir(modulePath);
    size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        exeDir.resize(slash + 1);
    } else {
        exeDir.clear();
    }

    const wchar_t* kTargets[] = { L"Screen2PC.Sender.exe", L"Screen2PC.Receiver.exe" };
    const int kExpectedSizes[] = { 16, 20, 24, 32, 48, 64, 256 };

    int verifiedTargets = 0;
    for (const wchar_t* exeName : kTargets) {
        std::wstring targetPath = exeDir + exeName;
        if (GetFileAttributesW(targetPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        ++verifiedTargets;

        HMODULE hMod = LoadLibraryExW(
            targetPath.c_str(), nullptr,
            LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
        CHECK(hMod != nullptr, "LoadLibraryExW failed on target EXE for icon check");
        if (!hMod) continue;

        CHECK(FindResourceW(hMod, MAKEINTRESOURCEW(IDI_SCREEN2PC_APP), RT_GROUP_ICON) != nullptr,
              "IDI_SCREEN2PC_APP RT_GROUP_ICON missing from EXE resource table");

        for (int sz : kExpectedSizes) {
            HICON hIcon = static_cast<HICON>(LoadImageW(
                hMod, MAKEINTRESOURCEW(IDI_SCREEN2PC_APP), IMAGE_ICON, sz, sz, LR_DEFAULTCOLOR));
            CHECK(hIcon != nullptr, "LoadImageW failed to decode icon size from EXE");
            if (hIcon) {
                ICONINFO ii{};
                BOOL okInfo = GetIconInfo(hIcon, &ii);
                CHECK(okInfo != FALSE, "GetIconInfo failed on decoded EXE icon");
                if (okInfo) {
                    BITMAP bmp{};
                    CHECK(ii.hbmColor != nullptr && GetObjectW(ii.hbmColor, sizeof(bmp), &bmp) == sizeof(bmp),
                          "GetObjectW failed on icon color bitmap");
                    CHECK(bmp.bmWidth == sz && bmp.bmHeight == sz,
                          "Decoded icon bitmap dimensions do not match requested size");
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                }
                DestroyIcon(hIcon);
            }
        }
        FreeLibrary(hMod);
    }

    CHECK(verifiedTargets == 2, "Expected both Screen2PC.Sender.exe and Screen2PC.Receiver.exe beside Tests.exe");
    std::cout << "[PASS] TestAppIcons\n";
}

void TestBitratePresets() {
    CHECK(s2p::kBitratePresetCount == 10, "Expected 10 bitrate presets");
    CHECK(s2p::BitrateFromPresetIndex(s2p::kDefaultBitratePresetIdx) == 80, "Default bitrate must be 80 Mbit/s");
    CHECK(s2p::BitrateFromPresetIndex(-1) == 80 && s2p::BitrateFromPresetIndex(999) == 80,
          "Out-of-bounds preset index must safely fall back to 80 Mbit/s");

    for (size_t i = 1; i < s2p::kBitratePresetCount; ++i) {
        CHECK(s2p::kBitratePresetsMbps[i] > s2p::kBitratePresetsMbps[i - 1],
              "Bitrate presets must be strictly ascending");
    }

    // Requested 60 and 100 Mbit/s presets must exist with clean labels
    int idx60 = s2p::FindBitratePresetIndex(60);
    int idx100 = s2p::FindBitratePresetIndex(100);
    CHECK(s2p::BitrateFromPresetIndex(idx60) == 60, "60 Mbit/s preset missing");
    CHECK(s2p::BitrateFromPresetIndex(idx100) == 100, "100 Mbit/s preset missing");
    CHECK(s2p::FormatBitrateLabel(60) == L"60 Mbit/s", "60 Mbit/s label mismatch");
    CHECK(s2p::FormatBitrateLabel(100) == L"100 Mbit/s", "100 Mbit/s label mismatch");

    // Legacy 5-step INI migration (v1.5.0.28 and earlier: 25, 50, 80, 120, 200)
    CHECK(s2p::BitrateFromPresetIndex(s2p::ResolveSavedBitratePresetIndex(0, 0)) == 25, "Legacy idx 0 -> 25 failed");
    CHECK(s2p::BitrateFromPresetIndex(s2p::ResolveSavedBitratePresetIndex(0, 1)) == 50, "Legacy idx 1 -> 50 failed");
    CHECK(s2p::BitrateFromPresetIndex(s2p::ResolveSavedBitratePresetIndex(0, 2)) == 80, "Legacy idx 2 -> 80 failed");
    CHECK(s2p::BitrateFromPresetIndex(s2p::ResolveSavedBitratePresetIndex(0, 3)) == 120, "Legacy idx 3 -> 120 failed");
    CHECK(s2p::BitrateFromPresetIndex(s2p::ResolveSavedBitratePresetIndex(0, 4)) == 200, "Legacy idx 4 -> 200 failed");
    CHECK(s2p::BitrateFromPresetIndex(s2p::ResolveSavedBitratePresetIndex(0, -1)) == 80, "Fresh INI default -> 80 failed");

    // Explicit BitrateMbps always wins over legacy index and snaps nearest
    CHECK(s2p::BitrateFromPresetIndex(s2p::ResolveSavedBitratePresetIndex(60, 4)) == 60, "Explicit 60 Mbit/s ignored");
    CHECK(s2p::BitrateFromPresetIndex(s2p::ResolveSavedBitratePresetIndex(100, 0)) == 100, "Explicit 100 Mbit/s ignored");
    CHECK(s2p::BitrateFromPresetIndex(s2p::ResolveSavedBitratePresetIndex(95, 0)) == 100, "Nearest snap 95 -> 100 failed");

    std::cout << "[PASS] TestBitratePresets\n";
}

void TestVersionProtocolAndPresets() {
    // 1. Header version & raw mismatch detection
    auto v2Hdr = s2p::MakeHeader(s2p::MsgType::HelloSender, 16);
    uint16_t remoteVer = 0;
    CHECK(s2p::CheckHeaderVersion(v2Hdr, remoteVer) && remoteVer == s2p::kProtocolVersion,
          "CheckHeaderVersion failed on current protocol header");

    auto v1Hdr = v2Hdr;
    v1Hdr.version = 1;
    CHECK(!s2p::CheckHeaderVersion(v1Hdr, remoteVer) && remoteVer == 1,
          "CheckHeaderVersion failed to detect v1 mismatch");

    auto badMagicHdr = v2Hdr;
    badMagicHdr.magic = 0x11223344u;
    CHECK(!s2p::CheckHeaderVersion(badMagicHdr, remoteVer) && remoteVer == 0,
          "CheckHeaderVersion did not reset version on invalid magic");

    // 2. Loopback RecvPacketRaw detects raw v1 mismatch & delivers v2 VersionMismatch packet
    const uint16_t testPort = 48296;
    s2p::TcpSocket listener;
    CHECK(listener.Listen(testPort, 2), "Version test TcpSocket::Listen failed");

    s2p::TcpSocket srvSock;
    std::thread acceptThread([&]() {
        srvSock = listener.Accept(nullptr, 2500);
    });
    s2p::TcpSocket cliSock;
    CHECK(cliSock.Connect("127.0.0.1", testPort, 2000), "Version test TcpSocket::Connect failed");
    acceptThread.join();
    listener.Close();
    CHECK(srvSock.IsValid(), "Version test server accept failed");

    struct {
        s2p::PacketHeader hdr;
        uint32_t body;
    } legacyPkt{};
    legacyPkt.hdr = s2p::MakeHeader(s2p::MsgType::HelloSender, sizeof(uint32_t));
    legacyPkt.hdr.version = 1;
    legacyPkt.body = 0x12345678u;
    CHECK(cliSock.SendAll(&legacyPkt, sizeof(legacyPkt)), "Send legacy v1 packet failed");

    s2p::PacketHeader recvHdr{};
    std::vector<uint8_t> recvPayload;
    uint16_t detectedVer = 0;
    bool verMismatch = false;
    CHECK(!srvSock.RecvPacketRaw(recvHdr, recvPayload, detectedVer, verMismatch),
          "RecvPacketRaw should return false on protocol version mismatch");
    CHECK(verMismatch && detectedVer == 1, "RecvPacketRaw failed to report remote v1 mismatch");

    s2p::VersionMismatchPayload vm{};
    vm.protocolVersion = s2p::kProtocolVersion;
    vm.appVersion = s2p::CurrentAppVersionTuple();
    wcscpy_s(vm.message, L"Bitte aktualisieren");
    CHECK(srvSock.SendPacket(s2p::MsgType::VersionMismatch, &vm, sizeof(vm)), "Send VersionMismatch failed");

    CHECK(cliSock.RecvPacketRaw(recvHdr, recvPayload, detectedVer, verMismatch) &&
          !verMismatch &&
          recvHdr.type == static_cast<uint16_t>(s2p::MsgType::VersionMismatch) &&
          recvPayload.size() == sizeof(vm),
          "Client failed to receive v2 VersionMismatch packet");

    cliSock.Close();
    srvSock.Close();

    // 3. FPS presets & effective FPS calculation
    CHECK(s2p::kFpsPresetCount == 4, "Expected 4 FPS presets (Auto, 60, 90, 120)");
    CHECK(s2p::FpsFromPresetIndex(0) == 0 &&
          s2p::FpsFromPresetIndex(1) == 60 &&
          s2p::FpsFromPresetIndex(2) == 90 &&
          s2p::FpsFromPresetIndex(3) == 120,
          "FPS preset order mismatch");
    CHECK(s2p::FindFpsPresetIndex(0) == 0 && s2p::FindFpsPresetIndex(90) == 2 && s2p::FindFpsPresetIndex(144) == 3,
          "FindFpsPresetIndex mismatch");
    CHECK(s2p::ResolveEffectiveFps(0, 144) == 120 &&
          s2p::ResolveEffectiveFps(0, 60) == 60 &&
          s2p::ResolveEffectiveFps(90, 144) == 90,
          "ResolveEffectiveFps mismatch");
    CHECK(s2p::FormatFpsLabel(0) == L"Auto (Monitor-Hz)" && s2p::FormatFpsLabel(120) == L"120 FPS",
          "FormatFpsLabel mismatch");

    std::cout << "[PASS] TestVersionProtocolAndPresets\n";
}

void TestPairingSecurity() {
    // 1. Cryptographic 6-digit PIN generation + constant-time verify
    std::set<std::string> pins;
    for (int i = 0; i < 12; ++i) {
        std::string p = s2p::GeneratePin6();
        CHECK(p.size() == 6, "Generated PIN is not 6 chars");
        for (char c : p) {
            CHECK(c >= '0' && c <= '9', "Generated PIN contains non-digit");
        }
        pins.insert(p);
    }
    CHECK(pins.size() >= 2, "GeneratePin6 returned identical values across 12 samples");

    CHECK(s2p::ConstantTimePinEquals("482910", "482910", 6), "ConstantTimePinEquals failed on exact match");
    CHECK(!s2p::ConstantTimePinEquals("482910", "482911", 6), "ConstantTimePinEquals accepted mismatch");
    CHECK(!s2p::ConstantTimePinEquals("48291", "48291", 6), "ConstantTimePinEquals accepted short PIN");
    CHECK(!s2p::ConstantTimePinEquals(nullptr, "482910", 6), "ConstantTimePinEquals failed nullptr check");

    // 2. MAC formatting, trusted key derivation & INI persistence
    const uint8_t macA[6] = { 0xAA, 0xBB, 0xCC, 0x11, 0x22, 0x33 };
    const uint8_t zeroMac[6]{};
    const uint8_t devIdA[16] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };
    const uint8_t zeroId[16]{};

    CHECK(s2p::FormatMacAddress(macA) == L"AA:BB:CC:11:22:33", "FormatMacAddress mismatch");
    CHECK(s2p::MakeTrustedDeviceKey(macA, devIdA) == L"MAC_AABBCC112233", "MAC trusted key mismatch");
    CHECK(s2p::MakeTrustedDeviceKey(zeroMac, devIdA) == L"ID_0102030405060708090A0B0C0D0E0F10",
          "DeviceId fallback trusted key mismatch");

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring iniPath = std::wstring(tempDir) + L"s2p_pair_test_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(iniPath.c_str());

    uint8_t loadedId1[16]{};
    uint8_t loadedId2[16]{};
    CHECK(s2p::LoadOrCreateLocalDeviceId(iniPath, loadedId1) && !s2p::IsAllZero(loadedId1, 16),
          "LoadOrCreateLocalDeviceId failed");
    CHECK(s2p::LoadOrCreateLocalDeviceId(iniPath, loadedId2) && std::memcmp(loadedId1, loadedId2, 16) == 0,
          "LoadOrCreateLocalDeviceId not persistent");

    CHECK(!s2p::IsDeviceTrusted(iniPath, macA, devIdA), "Fresh INI unexpectedly trusted unknown device");
    CHECK(s2p::SaveTrustedDevice(iniPath, L"Surface Laptop|Test", macA, devIdA, "192.168.1.45"),
          "SaveTrustedDevice failed");
    s2p::TrustedDeviceEntry match{};
    CHECK(s2p::IsDeviceTrusted(iniPath, macA, zeroId, &match) &&
          match.key == L"MAC_AABBCC112233" &&
          match.macStr == L"AA:BB:CC:11:22:33" &&
          match.lastIp == L"192.168.1.45",
          "IsDeviceTrusted by MAC failed");
    CHECK(s2p::IsDeviceTrusted(iniPath, zeroMac, devIdA, &match) && match.key == L"MAC_AABBCC112233",
          "IsDeviceTrusted fallback by deviceId failed");

    // 3. Loopback PIN exchange simulation -> trusted remember -> auto-verify
    const uint16_t pairPort = 48295;
    s2p::TcpSocket listener;
    CHECK(listener.Listen(pairPort, 2), "Pairing loopback Listen failed");

    const std::string expectedPin = s2p::GeneratePin6();
    std::thread pcAuthThread([&]() {
        s2p::TcpSocket s = listener.Accept(nullptr, 2500);
        if (!s.IsValid()) return;
        s2p::PairChallengePayload ch{};
        wcscpy_s(ch.pcName, L"Gaming-PC");
        ch.pcVersion = s2p::CurrentAppVersionTuple();
        s.SendPacket(s2p::MsgType::PairChallenge, &ch, sizeof(ch));

        for (int attempt = 0; attempt < 3; ++attempt) {
            s2p::PacketHeader ph{};
            std::vector<uint8_t> pp;
            if (!s.RecvPacket(ph, pp) || pp.size() < sizeof(s2p::PairVerifyPayload)) break;
            s2p::PairVerifyPayload pv{};
            std::memcpy(&pv, pp.data(), sizeof(pv));
            s2p::PairResultPayload pr{};
            if (s2p::ConstantTimePinEquals(pv.pinCode, expectedPin.c_str(), 6)) {
                pr.accepted = 1;
                pr.remembered = s2p::SaveTrustedDevice(iniPath, L"Surface Pro", pv.senderMac, pv.senderDeviceId, "127.0.0.1") ? 1 : 0;
                pr.remainingAttempts = static_cast<uint8_t>(3 - attempt);
                s.SendPacket(s2p::MsgType::PairResult, &pr, sizeof(pr));
                break;
            } else {
                pr.accepted = 0;
                pr.remainingAttempts = static_cast<uint8_t>(2 - attempt);
                s.SendPacket(s2p::MsgType::PairResult, &pr, sizeof(pr));
            }
        }
        s.Close();
    });

    s2p::TcpSocket surfSock;
    CHECK(surfSock.Connect("127.0.0.1", pairPort, 2000), "Pairing loopback Connect failed");
    s2p::PacketHeader h{};
    std::vector<uint8_t> pl;
    CHECK(surfSock.RecvPacket(h, pl) && h.type == static_cast<uint16_t>(s2p::MsgType::PairChallenge),
          "Missing PairChallenge");

    const uint8_t macB[6] = { 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC };
    s2p::PairVerifyPayload pvBad{};
    strncpy_s(pvBad.pinCode, "000000", _TRUNCATE);
    if (expectedPin == "000000") strncpy_s(pvBad.pinCode, "111111", _TRUNCATE);
    std::memcpy(pvBad.senderMac, macB, 6);
    CHECK(surfSock.SendPacket(s2p::MsgType::PairVerify, &pvBad, sizeof(pvBad)), "Send bad PairVerify failed");
    CHECK(surfSock.RecvPacket(h, pl) && h.type == static_cast<uint16_t>(s2p::MsgType::PairResult) &&
          pl.size() >= sizeof(s2p::PairResultPayload), "Missing bad PairResult");
    s2p::PairResultPayload prBad{};
    std::memcpy(&prBad, pl.data(), sizeof(prBad));
    CHECK(prBad.accepted == 0 && prBad.remainingAttempts == 2, "Wrong PIN should fail with 2 attempts left");

    s2p::PairVerifyPayload pvGood{};
    strncpy_s(pvGood.pinCode, expectedPin.c_str(), _TRUNCATE);
    std::memcpy(pvGood.senderMac, macB, 6);
    CHECK(surfSock.SendPacket(s2p::MsgType::PairVerify, &pvGood, sizeof(pvGood)), "Send good PairVerify failed");
    CHECK(surfSock.RecvPacket(h, pl) && h.type == static_cast<uint16_t>(s2p::MsgType::PairResult) &&
          pl.size() >= sizeof(s2p::PairResultPayload), "Missing good PairResult");
    s2p::PairResultPayload prGood{};
    std::memcpy(&prGood, pl.data(), sizeof(prGood));
    CHECK(prGood.accepted == 1 && prGood.remembered == 1, "Valid PIN should be accepted and remembered");

    surfSock.Close();
    pcAuthThread.join();
    listener.Close();

    CHECK(s2p::IsDeviceTrusted(iniPath, macB, zeroId), "Second device not found in trusted list after PIN auth");
    CHECK(s2p::LoadTrustedDevices(iniPath).size() == 2, "Expected 2 trusted entries");
    CHECK(s2p::RemoveTrustedDevice(iniPath, L"MAC_AABBCC112233") && s2p::LoadTrustedDevices(iniPath).size() == 1,
          "RemoveTrustedDevice failed");
    CHECK(s2p::ClearTrustedDevices(iniPath) && s2p::LoadTrustedDevices(iniPath).empty(),
          "ClearTrustedDevices failed");

    DeleteFileW(iniPath.c_str());
    std::cout << "[PASS] TestPairingSecurity\n";
}

void TestFileTransferRecursiveAndCancel() {
    // 1. Relative subfolder sanitization & traversal blocking
    CHECK(s2p::FileTransfer::SanitizeRelativePath(L"Projekte/2026\\Daten") == L"Projekte\\2026\\Daten",
          "Valid relative path altered");
    CHECK(s2p::FileTransfer::SanitizeRelativePath(L"..\\..\\Windows\\System32").empty(),
          "Parent directory traversal must return empty path");
    CHECK(s2p::FileTransfer::SanitizeRelativePath(L"C:\\Users\\Public").empty(),
          "Drive-qualified relative path must be rejected");
    CHECK(s2p::FileTransfer::SanitizeRelativePath(L"\\\\server\\share\\file").empty(),
          "UNC relative path must be rejected");
    CHECK(s2p::FileTransfer::SanitizeRelativePath(L"CON\\PRN") == L"_CON\\_PRN",
          "Reserved Windows folder names not escaped");

    // 2. Recursive directory tree transfer over loopback
    wchar_t tempBase[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempBase);
    std::wstring baseDir = std::wstring(tempBase) + L"s2p_ft_rec_" + std::to_wstring(GetCurrentProcessId());
    std::wstring srcRoot = baseDir + L"\\src";
    std::wstring folderA = srcRoot + L"\\OrdnerA";
    std::wstring folderDeep = folderA + L"\\Unter";
    std::wstring dstDir = baseDir + L"\\dst";
    CreateDirectoryW(baseDir.c_str(), nullptr);
    CreateDirectoryW(srcRoot.c_str(), nullptr);
    CreateDirectoryW(folderA.c_str(), nullptr);
    CreateDirectoryW(folderDeep.c_str(), nullptr);
    CreateDirectoryW(dstDir.c_str(), nullptr);

    auto writeBytes = [](const std::wstring& path, const std::vector<uint8_t>& data) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE, "Failed creating temp file");
        if (h != INVALID_HANDLE_VALUE) {
            DWORD w = 0;
            WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &w, nullptr);
            CloseHandle(h);
        }
    };
    auto readBytes = [](const std::wstring& path, std::vector<uint8_t>& out) -> bool {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz{};
        GetFileSizeEx(h, &sz);
        out.resize(static_cast<size_t>(sz.QuadPart));
        DWORD r = 0;
        BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &r, nullptr);
        CloseHandle(h);
        return ok && r == out.size();
    };

    const std::vector<uint8_t> d1 = { 'H','e','l','l','o','1' };
    const std::vector<uint8_t> d2(128 * 1024 + 9, 0x5A);
    std::wstring f1 = folderA + L"\\top.txt";
    std::wstring f2 = folderDeep + L"\\deep.bin";
    writeBytes(f1, d1);
    writeBytes(f2, d2);

    const uint16_t recPort = 48294;
    s2p::TcpSocket listener;
    CHECK(listener.Listen(recPort, 2), "Recursive FileTransfer listen failed");

    s2p::TcpSocket srvSock;
    std::thread acc([&]() { srvSock = listener.Accept(nullptr, 2500); });
    s2p::TcpSocket cliSock;
    CHECK(cliSock.Connect("127.0.0.1", recPort, 2000), "Recursive FileTransfer connect failed");
    acc.join();
    listener.Close();

    s2p::FileTransfer tx, rx;
    tx.Start(std::move(cliSock), srcRoot);
    rx.Start(std::move(srvSock), dstDir);

    CHECK(tx.QueuePaths({ folderA }) == 2, "QueuePaths did not collect 2 files recursively");
    bool recDone = false;
    for (int i = 0; i < 80; ++i) {
        if (rx.GetStats().filesReceived == 2 && tx.GetStats().filesSent == 2) {
            recDone = true;
            break;
        }
        Sleep(25);
    }
    CHECK(recDone, "Recursive folder transfer did not complete");

    std::vector<uint8_t> out1, out2;
    std::wstring rx1 = dstDir + L"\\OrdnerA\\top.txt";
    std::wstring rx2 = dstDir + L"\\OrdnerA\\Unter\\deep.bin";
    CHECK(readBytes(rx1, out1) && out1 == d1, "Transferred top.txt missing or corrupted");
    CHECK(readBytes(rx2, out2) && out2 == d2, "Transferred deep.bin missing or corrupted");

    // 3. CancelCurrent clears queue & stops active transfer cleanly
    std::wstring big1 = srcRoot + L"\\big1.bin";
    std::wstring big2 = srcRoot + L"\\big2.bin";
    std::vector<uint8_t> bigData(24 * 1024 * 1024, 0x3C); // ~24 MB takes ~150+ ms due to per-byte CRC32
    std::vector<uint8_t> smallAfter = { 1, 2, 3, 4 };
    writeBytes(big1, bigData);
    writeBytes(big2, smallAfter);

    CHECK(tx.QueuePaths({ big1, big2 }) == 2, "Failed queueing cancel test files");
    bool canceledDuringRun = false;
    for (int i = 0; i < 200; ++i) {
        auto st = tx.GetStats();
        if (st.transferring) {
            tx.CancelCurrent();
            canceledDuringRun = true;
            break;
        }
        if (st.filesSent >= 4) break;
        Sleep(1);
    }
    for (int i = 0; i < 60; ++i) {
        if (!tx.GetStats().transferring && !rx.GetStats().transferring) break;
        Sleep(25);
    }
    CHECK(tx.GetStats().queuedFiles == 0, "CancelCurrent left queued files behind");
    if (canceledDuringRun) {
        CHECK(rx.GetStats().filesReceived < 4, "Second queued file transferred despite CancelCurrent");
    }

    tx.Stop();
    rx.Stop();

    DeleteFileW(f1.c_str());
    DeleteFileW(f2.c_str());
    DeleteFileW(big1.c_str());
    DeleteFileW(big2.c_str());
    DeleteFileW(rx1.c_str());
    DeleteFileW(rx2.c_str());
    DeleteFileW((dstDir + L"\\big1.bin").c_str());
    DeleteFileW((dstDir + L"\\big2.bin").c_str());
    RemoveDirectoryW((dstDir + L"\\OrdnerA\\Unter").c_str());
    RemoveDirectoryW((dstDir + L"\\OrdnerA").c_str());
    RemoveDirectoryW(dstDir.c_str());
    RemoveDirectoryW(folderDeep.c_str());
    RemoveDirectoryW(folderA.c_str());
    RemoveDirectoryW(srcRoot.c_str());
    RemoveDirectoryW(baseDir.c_str());

    std::cout << "[PASS] TestFileTransferRecursiveAndCancel\n";
}

void TestPowerPolicyIniAndGuid() {
    const GUID sampleGuid = { 0x381B4222, 0xF694, 0x41F0, { 0x96, 0x85, 0xFF, 0x5B, 0xB2, 0x60, 0xDF, 0x2E } };
    std::wstring s = s2p::LidCloseGuard::FormatGuidString(sampleGuid);
    GUID parsed{};
    CHECK(s2p::LidCloseGuard::ParseGuidString(s, parsed) &&
          std::memcmp(&sampleGuid, &parsed, sizeof(GUID)) == 0,
          "GUID Format/Parse roundtrip with braces failed");

    std::wstring noBraces = s.substr(1, s.size() - 2);
    GUID parsed2{};
    CHECK(s2p::LidCloseGuard::ParseGuidString(noBraces, parsed2) &&
          std::memcmp(&sampleGuid, &parsed2, sizeof(GUID)) == 0,
          "GUID Parse without braces failed");

    GUID bad{};
    CHECK(!s2p::LidCloseGuard::ParseGuidString(L"not-a-guid", bad), "Invalid GUID unexpectedly parsed");

    wchar_t tempDir[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring iniPath = std::wstring(tempDir) + L"s2p_power_test_" + std::to_wstring(GetCurrentProcessId()) + L".ini";
    DeleteFileW(iniPath.c_str());

    s2p::PowerRestoreState out{};
    CHECK(!s2p::LidCloseGuard::LoadPowerRestoreIni(iniPath, out), "Empty INI should report no pending restore");
    CHECK(!s2p::LidCloseGuard::RecoverStaleStateOnStartup(iniPath), "RecoverStaleStateOnStartup should no-op on clean INI");

    s2p::PowerRestoreState st{};
    st.pending = true;
    st.schemeGuid = sampleGuid;
    st.savedAc = 2;
    st.savedDc = 1;
    CHECK(s2p::LidCloseGuard::SavePowerRestoreIni(iniPath, st), "SavePowerRestoreIni failed");
    CHECK(s2p::LidCloseGuard::LoadPowerRestoreIni(iniPath, out) &&
          out.pending &&
          std::memcmp(&out.schemeGuid, &sampleGuid, sizeof(GUID)) == 0 &&
          out.savedAc == 2 &&
          out.savedDc == 1,
          "LoadPowerRestoreIni roundtrip mismatch");

    s2p::LidCloseGuard::ClearPowerRestoreIni(iniPath);
    CHECK(!s2p::LidCloseGuard::LoadPowerRestoreIni(iniPath, out), "ClearPowerRestoreIni did not clear state");

    DeleteFileW(iniPath.c_str());
    std::cout << "[PASS] TestPowerPolicyIniAndGuid\n";
}

void TestH264BitrateAndFpsInit() {
    s2p::H264Encoder enc;
    CHECK(enc.Init(nullptr, nullptr, 640, 360, 90, 60), "H264Encoder init at 90 FPS / 60 Mbps failed");
    CHECK(enc.BitrateMbps() == 60, "Initial BitrateMbps mismatch");
    CHECK(enc.SetBitrate(100) && enc.BitrateMbps() == 100, "H264Encoder::SetBitrate(100) failed");
    CHECK(enc.SetBitrate(40) && enc.BitrateMbps() == 40, "H264Encoder::SetBitrate(40) failed");

    std::vector<uint8_t> bgra(640 * 360 * 4, 160);
    std::vector<uint8_t> annexB;
    bool isKey = false;
    bool produced = false;
    for (int f = 0; f < 6; ++f) {
        if (enc.EncodeBgra(bgra.data(), 640 * 4, static_cast<uint64_t>(f) * 11111ULL, annexB, isKey) &&
            !annexB.empty()) {
            produced = true;
            break;
        }
    }
    CHECK(produced, "H264Encoder at 90 FPS produced no Annex-B after SetBitrate");
    enc.Shutdown();

    s2p::H264Decoder dec;
    CHECK(dec.Init(nullptr, nullptr, 640, 360, false, 90), "H264Decoder::Init(..., fps=90) failed");
    dec.Shutdown();

    std::cout << "[PASS] TestH264BitrateAndFpsInit\n";
}

void TestVddDeterministicGuid() {
    GUID g0a = s2p::vdd::MakeDeterministicMonitorGuid(0);
    GUID g0b = s2p::vdd::MakeDeterministicMonitorGuid(0);
    GUID g1 = s2p::vdd::MakeDeterministicMonitorGuid(1);
    GUID g2 = s2p::vdd::MakeDeterministicMonitorGuid(2);

    CHECK(std::memcmp(&g0a, &g0b, sizeof(GUID)) == 0, "Deterministic monitor GUID 0 is not stable");
    CHECK(std::memcmp(&g0a, &g1, sizeof(GUID)) != 0, "Monitor GUID 0 and 1 collided");
    CHECK(std::memcmp(&g1, &g2, sizeof(GUID)) != 0, "Monitor GUID 1 and 2 collided");

    std::cout << "[PASS] TestVddDeterministicGuid\n";
}

void TestDeltaQoiHybrid() {
    const uint32_t W = 640, H = 360;
    std::vector<uint8_t> fb(static_cast<size_t>(W) * H * 4, 0);
    for (uint32_t y = 0; y < H; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = fb.data() + (static_cast<size_t>(y) * W + x) * 4;
            if ((x / 16 + y / 16) % 2 == 0) {
                p[0] = static_cast<uint8_t>((x * 3) & 0xFF);
                p[1] = static_cast<uint8_t>((y * 5) & 0xFF);
                p[2] = static_cast<uint8_t>((x + y) & 0xFF);
                p[3] = 255;
            } else if (x < 100) {
                // Solid runs -> QOI_OP_RUN
                p[0] = 30; p[1] = 60; p[2] = 90; p[3] = 255;
            } else {
                // Slight delta + varying alpha -> QOI_OP_DIFF / QOI_OP_LUMA / QOI_OP_RGBA
                p[0] = static_cast<uint8_t>(30 + (x % 3));
                p[1] = static_cast<uint8_t>(60 + (y % 4));
                p[2] = static_cast<uint8_t>(90 + ((x ^ y) % 5));
                p[3] = (y % 17 == 0) ? 128 : 255;
            }
        }
    }

    s2p::DeltaEncoder enc;
    s2p::DeltaDecoder dec;
    std::vector<uint8_t> payload;
    bool isKey = false;
    CHECK(enc.Encode(fb.data(), W, H, W * 4, {}, true, payload, isKey), "DLT3 QOI keyframe encode failed");
    CHECK(isKey && payload.size() >= 4, "DLT3 keyframe flags missing");
    uint32_t magic = 0;
    std::memcpy(&magic, payload.data(), 4);
    CHECK(magic == 0x444C5433u, "Expected DLT3 magic on QOI keyframe");
    CHECK(dec.Decode(payload.data(), payload.size(), W, H), "DLT3 QOI keyframe decode failed");
    CHECK(std::memcmp(dec.FrameData(), fb.data(), fb.size()) == 0, "DLT3 QOI keyframe pixel mismatch");

    // Large 40% update -> QOI hybrid delta
    for (uint32_t y = 0; y < H / 2; ++y) {
        for (uint32_t x = 0; x < W; ++x) {
            uint8_t* p = fb.data() + (static_cast<size_t>(y) * W + x) * 4;
            p[0] = static_cast<uint8_t>(255 - (x & 0xFF));
            p[1] = static_cast<uint8_t>((y * 7) & 0xFF);
            p[2] = static_cast<uint8_t>((x * 11) & 0xFF);
            p[3] = 255;
        }
    }
    std::vector<s2p::DeltaRect> bigHint{ { 0, 0, W, H / 2 } };
    CHECK(enc.Encode(fb.data(), W, H, W * 4, bigHint, false, payload, isKey), "DLT3 QOI delta encode failed");
    CHECK(!isKey, "Delta frame unexpectedly flagged as keyframe");
    std::memcpy(&magic, payload.data(), 4);
    CHECK(magic == 0x444C5433u, "Expected DLT3 magic on large >=20% delta");
    CHECK(dec.Decode(payload.data(), payload.size(), W, H), "DLT3 QOI delta decode failed");
    CHECK(std::memcmp(dec.FrameData(), fb.data(), fb.size()) == 0, "DLT3 QOI delta pixel mismatch");

    // Tiny 32x32 update -> XOR+LZ4 DLT2 path
    for (uint32_t y = 200; y < 232; ++y) {
        for (uint32_t x = 200; x < 232; ++x) {
            uint8_t* p = fb.data() + (static_cast<size_t>(y) * W + x) * 4;
            p[0] ^= 0x5Au; p[1] ^= 0x3Cu; p[2] ^= 0x7Eu;
        }
    }
    std::vector<s2p::DeltaRect> smallHint{ { 200, 200, 32, 32 } };
    CHECK(enc.Encode(fb.data(), W, H, W * 4, smallHint, false, payload, isKey), "DLT2 small XOR delta encode failed");
    CHECK(dec.Decode(payload.data(), payload.size(), W, H), "DLT2 small XOR delta decode failed");
    CHECK(std::memcmp(dec.FrameData(), fb.data(), fb.size()) == 0, "DLT2 small XOR delta pixel mismatch");

    std::cout << "[PASS] TestDeltaQoiHybrid\n";
}

void TestUdpVideoFec() {
    s2p::UdpVideoFecSender sender;
    s2p::UdpVideoFecReceiver receiver;

    // 1) Multi-group keyframe (~48 KB) with dropped shards recovered by RS-FEC
    std::vector<uint8_t> keyBytes(49152);
    for (size_t i = 0; i < keyBytes.size(); ++i) {
        keyBytes[i] = static_cast<uint8_t>((i * 37 + 13) & 0xFF);
    }
    s2p::VideoFrameHeader vh{};
    vh.monitorIndex = 0;
    vh.frameIndex = 101;
    vh.timestampUs = 987654321ULL;
    vh.width = 2560;
    vh.height = 1440;
    vh.codec = static_cast<uint8_t>(s2p::CodecType::HEVC);
    vh.flags = s2p::kFrameFlagKeyframe;
    vh.dataSize = static_cast<uint32_t>(keyBytes.size());

    std::vector<std::vector<uint8_t>> pkts;
    size_t parityCnt = 0;
    CHECK(sender.BuildPackets(0, vh.frameIndex, true, vh, keyBytes.data(), keyBytes.size(), pkts, &parityCnt),
          "UdpVideoFecSender::BuildPackets failed on keyframe");
    CHECK(pkts.size() > 20 && parityCnt >= 2, "Expected multi-group FEC packets on 48KB keyframe");

    s2p::VideoFrameHeader outHdr{};
    std::vector<uint8_t> outPayload;
    bool needKey = false;
    bool delivered = false;
    // Drop shard index 1 and 5 in group 0 (<= 3 parity shards for keyframes -> fully recoverable!)
    for (size_t i = 0; i < pkts.size(); ++i) {
        if (i == 1 || i == 5) continue;
        if (receiver.OnPacket(pkts[i].data(), pkts[i].size(), 1000ULL, outHdr, outPayload, needKey)) {
            delivered = true;
        }
    }
    CHECK(delivered, "RS-FEC failed to recover keyframe with 2 erased shards");
    CHECK(outHdr.frameIndex == vh.frameIndex && outHdr.codec == static_cast<uint8_t>(s2p::CodecType::HEVC) &&
          outPayload.size() == keyBytes.size() &&
          std::memcmp(outPayload.data(), keyBytes.data(), keyBytes.size()) == 0,
          "Recovered RS-FEC keyframe payload mismatch");

    // 2) Small P-frame (~5 KB) with 1 erased shard recovered
    std::vector<uint8_t> pBytes(5120, 0xA5);
    vh.frameIndex = 102;
    vh.flags = s2p::kFrameFlagNone;
    vh.dataSize = static_cast<uint32_t>(pBytes.size());
    CHECK(sender.BuildPackets(0, vh.frameIndex, false, vh, pBytes.data(), pBytes.size(), pkts, &parityCnt),
          "BuildPackets P-frame failed");
    delivered = false;
    for (size_t i = 0; i < pkts.size(); ++i) {
        if (i == 0) continue; // drop first data shard
        if (receiver.OnPacket(pkts[i].data(), pkts[i].size(), 1005ULL, outHdr, outPayload, needKey)) {
            delivered = true;
        }
    }
    CHECK(delivered && outPayload.size() == pBytes.size() &&
          std::memcmp(outPayload.data(), pBytes.data(), pBytes.size()) == 0,
          "RS-FEC failed to recover P-frame with 1 erased shard");

    // 3) Unrecoverable erasure expires cleanly and requests keyframe
    vh.frameIndex = 103;
    CHECK(sender.BuildPackets(0, vh.frameIndex, false, vh, pBytes.data(), pBytes.size(), pkts, &parityCnt),
          "BuildPackets expiration test failed");
    delivered = false;
    for (size_t i = 3; i < pkts.size(); ++i) { // drop 3 shards > 1 P-frame parity shard
        if (receiver.OnPacket(pkts[i].data(), pkts[i].size(), 1010ULL, outHdr, outPayload, needKey)) {
            delivered = true;
        }
    }
    CHECK(!delivered, "Unrecoverable frame unexpectedly succeeded");
    CHECK(receiver.PruneExpired(1050ULL, 25), "PruneExpired did not trigger keyframe recovery");

    std::cout << "[PASS] TestUdpVideoFec\n";
}

void TestAmfFallbackAndHevcHeaders() {
    static_assert(s2p::kProtocolVersion == 3, "Protocol v3 required for HEVC + UDP-FEC");
    static_assert(s2p::kPortVideoUdpBase == 48216, "UDP video port base mismatch");

    s2p::AmfEncoder amf;
    CHECK(!amf.Initialize(nullptr, nullptr, 1920, 1080, 60, 25000000, s2p::CodecType::HEVC),
          "AmfEncoder with null D3D device must return false cleanly");
    amf.Shutdown();

    s2p::H264Encoder enc;
    // Software HEVC fallback test: on machines without HEVC MFT it must fail gracefully without crashing
    bool hevcOk = enc.Init(nullptr, nullptr, 640, 360, 60, 20, false, s2p::CodecType::HEVC);
    if (hevcOk) {
        CHECK(enc.ActiveCodec() == s2p::CodecType::HEVC, "ActiveCodec mismatch for HEVC");
        enc.Shutdown();
    }

    std::cout << "[PASS] TestAmfFallbackAndHevcHeaders\n";
}

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    s2p::Logger::Instance().Init(L"Screen2PC_Tests.log");
    s2p::WinsockGuard wsa;
    CHECK(wsa.Ok(), "WinsockGuard initialization failed");

    TestProtocol();
    TestDeltaCodec();
    TestDeltaQoiHybrid();
    TestUdpVideoFec();
    TestColorAndH264();
    TestLoopbackNetworking();
    TestClipboardSync();
    TestFileTransfer();
    TestKvmInput();
    TestAutostartAndCli();
    TestHdrColorPipeline();
    TestUpdaterParsing();
    TestAppIcons();
    TestBitratePresets();
    TestVersionProtocolAndPresets();
    TestPairingSecurity();
    TestFileTransferRecursiveAndCancel();
    TestPowerPolicyIniAndGuid();
    TestH264BitrateAndFpsInit();
    TestAmfFallbackAndHevcHeaders();
    TestVddDeterministicGuid();

    CoUninitialize();

    if (g_failures > 0) {
        std::cerr << "\nTOTAL FAILURES: " << g_failures << "\n";
        return 1;
    }
    std::cout << "\nALL SCREEN2PC TESTS PASSED!\n";
    return 0;
}
