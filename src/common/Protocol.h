#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>

namespace s2p {

constexpr uint32_t kProtocolMagic   = 0x53325031; // "S2P1"
constexpr uint16_t kProtocolVersion = 1;

constexpr uint16_t kPortDiscovery    = 48210; // UDP Broadcast discovery
constexpr uint16_t kPortControl      = 48211; // TCP Control & Handshake
constexpr uint16_t kPortStreamBase   = 48212; // TCP Stream per monitor (48212 + monitorIndex)
constexpr uint16_t kPortFileTransfer = 48214; // Dedicated bidirectional TCP File Transfer

constexpr size_t   kMaxMonitors     = 2;
constexpr size_t   kMaxFrameBytes   = 64 * 1024 * 1024; // 64 MB safety cap

enum class CodecType : uint8_t {
    Auto       = 0,
    H264       = 1,
    DeltaLossless = 2
};

enum class MsgType : uint16_t {
    // UDP Discovery
    DiscoverReq      = 1,
    DiscoverResp     = 2,

    // TCP Control
    HelloSender      = 10,
    HelloReceiver    = 11,
    StartSession     = 12,
    StopSession      = 13,
    RequestKeyframe  = 14,
    Heartbeat        = 15,
    SessionAck       = 16,

    // TCP Video Stream (on stream socket)
    VideoFrame       = 20,
    CursorUpdate     = 21,
    StreamEnd        = 22,

    // TCP Control - Shared Clipboard & KVM (Phase 2/3)
    ClipboardSync    = 30,

    // Dedicated TCP File Transfer (port 48214)
    FileMeta         = 31,
    FileChunk        = 32,
    FileDone         = 33,

    // TCP Control - Pure User-Mode KVM Input (PC -> Surface)
    KvmMouse         = 40,
    KvmKey           = 41
};

enum KvmMouseAction : uint8_t {
    kMouseMove      = 0,
    kMouseLeftDown  = 1,
    kMouseLeftUp    = 2,
    kMouseRightDown = 3,
    kMouseRightUp   = 4,
    kMouseMidDown   = 5,
    kMouseMidUp     = 6,
    kMouseWheel     = 7,
    kMouseHWheel    = 8,
    kMouseX1Down    = 9,
    kMouseX1Up      = 10,
    kMouseX2Down    = 11,
    kMouseX2Up      = 12
};

enum FrameFlags : uint32_t {
    kFrameFlagNone     = 0,
    kFrameFlagKeyframe = 1 << 0,
    kFrameFlagFullRect = 1 << 1,
};

#pragma pack(push, 1)

struct PacketHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t type;       // MsgType
    uint32_t payloadSize;
    uint32_t reserved;
};

struct MonitorDesc {
    uint32_t index;
    uint32_t width;
    uint32_t height;
    uint32_t refreshHz;
    int32_t  left;
    int32_t  top;
    int32_t  right;
    int32_t  bottom;
    uint8_t  isPrimary;
    uint8_t  pad[3];
    wchar_t  friendlyName[32];
};

struct DiscoverReqPayload {
    uint32_t senderNonce;
    wchar_t  senderName[32];
};

struct DiscoverRespPayload {
    uint32_t senderNonce;
    uint16_t controlPort;
    uint16_t streamBasePort;
    uint32_t monitorCount;
    wchar_t  pcName[32];
    MonitorDesc monitors[kMaxMonitors];
};

struct HelloSenderPayload {
    wchar_t  senderName[32];
    uint8_t  preferredCodec; // CodecType
    uint8_t  pad[3];
    uint32_t targetBitrateMbps;
    uint32_t targetFps;
};

struct HelloReceiverPayload {
    wchar_t  pcName[32];
    uint32_t monitorCount;
    MonitorDesc monitors[kMaxMonitors];
};

struct StartStreamSpec {
    uint32_t monitorIndex;     // PC target monitor index
    uint32_t width;
    uint32_t height;
    uint32_t refreshHz;
    uint8_t  codec;            // CodecType
    uint8_t  pad[3];
    uint32_t bitrateMbps;
};

struct StartSessionPayload {
    uint32_t streamCount;
    StartStreamSpec streams[kMaxMonitors];
};

struct SessionAckPayload {
    uint32_t accepted; // 1 = OK, 0 = rejected
    uint32_t errorCode;
    wchar_t  message[64];
};

struct RequestKeyframePayload {
    uint32_t monitorIndex;
};

struct VideoFrameHeader {
    uint32_t monitorIndex;
    uint64_t frameIndex;
    uint64_t timestampUs;
    uint32_t width;
    uint32_t height;
    uint8_t  codec;   // CodecType
    uint8_t  pad[3];
    uint32_t flags;   // FrameFlags
    uint32_t dataSize;
};

struct CursorUpdateHeader {
    uint32_t monitorIndex;
    uint8_t  visible;
    uint8_t  shapeChanged;
    uint8_t  shapeType; // DXGI_OUTDUPL_POINTER_SHAPE_TYPE (1=Mono, 2=Color, 4=MaskedColor)
    uint8_t  pad;
    int32_t  x;
    int32_t  y;
    uint32_t shapeWidth;
    uint32_t shapeHeight;
    uint32_t shapePitch;
    uint32_t hotX;
    uint32_t hotY;
    uint32_t dataSize;
};

struct ClipboardHeader {
    uint32_t kind;     // 1 = UTF-16LE text, 2 = CF_DIB image
    uint64_t hash64;
    uint32_t dataSize;
};

struct FileMetaHeader {
    uint32_t transferId;
    uint64_t fileSize;
    uint32_t nameUtf8Bytes; // followed by UTF-8 sanitized filename
};

struct FileChunkHeader {
    uint32_t transferId;
    uint64_t offset;
    uint32_t chunkSize;
    uint32_t crc32; // CRC32 of chunk bytes
};

struct FileDonePayload {
    uint32_t transferId;
    uint64_t totalBytes;
    uint32_t totalCrc32;
    uint8_t  ok;
    uint8_t  pad[3];
};

struct KvmMouseEvent {
    uint32_t monitorIndex;
    uint8_t  action;     // KvmMouseAction
    uint8_t  isRelative; // 0 = normX/normY in [0..1] of monitorIndex; 1 = normX/normY are pixel deltas (dx, dy) across full Virtual Desktop
    uint8_t  pad[2];
    float    normX;      // 0.0 .. 1.0 inside streamed monitor (or deltaX in pixels when isRelative != 0)
    float    normY;      // 0.0 .. 1.0 inside streamed monitor (or deltaY in pixels when isRelative != 0)
    int32_t  wheelDelta;
};

struct KvmKeyEvent {
    uint16_t vk;
    uint16_t scanCode;
    uint8_t  isDown;
    uint8_t  isExtended;
    uint8_t  pad[2];
};

#pragma pack(pop)

inline PacketHeader MakeHeader(MsgType type, uint32_t payloadSize, uint32_t reserved = 0) {
    PacketHeader h{};
    h.magic = kProtocolMagic;
    h.version = kProtocolVersion;
    h.type = static_cast<uint16_t>(type);
    h.payloadSize = payloadSize;
    h.reserved = reserved;
    return h;
}

inline bool ValidateHeader(const PacketHeader& h) {
    return h.magic == kProtocolMagic &&
           h.version == kProtocolVersion &&
           h.payloadSize <= kMaxFrameBytes;
}

} // namespace s2p
