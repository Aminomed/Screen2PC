#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>
#include <vector>

#include "../common/Version.h"
#include "../common/Autostart.h"
#include "../common/ColorProfile.h"
#include "../common/Updater.h"
#include "../common/Protocol.h"
#include "../common/Logger.h"
#include "../common/NetSocket.h"
#include "../common/CodecDelta.h"
#include "../common/CodecH264.h"
#include "../common/ClipboardSync.h"
#include "../common/FileTransfer.h"
#include "../common/KvmInput.h"

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

    // Corrupted payload must be rejected cleanly without out-of-bounds read/write
    std::vector<uint8_t> corrupt = payload2;
    if (corrupt.size() > 8) {
        corrupt.resize(corrupt.size() - 5);
        CHECK(!dec.Decode(corrupt.data(), corrupt.size(), w, h), "Truncated Delta payload unexpectedly accepted");
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
    TestClipboardSync();
    TestFileTransfer();
    TestKvmInput();
    TestAutostartAndCli();
    TestHdrColorPipeline();
    TestUpdaterParsing();

    CoUninitialize();

    if (g_failures > 0) {
        std::cerr << "\nTOTAL FAILURES: " << g_failures << "\n";
        return 1;
    }
    std::cout << "\nALL SCREEN2PC TESTS PASSED!\n";
    return 0;
}
