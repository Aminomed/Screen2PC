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
        bool encInit = enc.Init(nullptr, nullptr, w, h, 30, 15);
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
                        CHECK(outBgra.size() == static_cast<size_t>(w) * h * 4, "H264 decoded buffer byte count mismatch");
                        size_t center = (static_cast<size_t>(h / 2) * w + (w / 2)) * 4;
                        CHECK(outBgra[center + 1] > 150, "H264 decoded center pixel luma unexpectedly dark");
                        gotDecoded = true;
                        break;
                    }
                }
            }
            CHECK(gotEncoded, "H264Encoder produced no Annex-B output");
            CHECK(gotDecoded, "H264Decoder produced no decoded frame");
        }

        enc.Shutdown();
        dec.Shutdown();
    };

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

    senderFt.Stop();
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

    std::cout << "[PASS] TestKvmInput\n";
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

    CoUninitialize();

    if (g_failures > 0) {
        std::cerr << "\nTOTAL FAILURES: " << g_failures << "\n";
        return 1;
    }
    std::cout << "\nALL SCREEN2PC TESTS PASSED!\n";
    return 0;
}
