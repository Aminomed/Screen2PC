#pragma once
#include <cstdint>
#include <guiddef.h>

namespace s2p::vdd {

constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\Screen2PC_VDD";
constexpr wchar_t kConfigDir[] = L"C:\\ProgramData\\Screen2PC";
constexpr wchar_t kConfigFile[] = L"C:\\ProgramData\\Screen2PC\\monitors.cfg";
constexpr wchar_t kHardwareId[] = L"Root\\Screen2PCVDD";

constexpr uint32_t kVddMagic = 0x56444431; // "VDD1"
constexpr uint32_t kMaxVirtualMonitors = 2;

enum class VddOp : uint32_t {
    Ping        = 1,
    SetMonitors = 2
};

#pragma pack(push, 1)

struct VddMonitorSpec {
    uint32_t width;
    uint32_t height;
    uint32_t refreshHz;
};

struct VddCommand {
    uint32_t magic;
    uint32_t op;           // VddOp
    uint32_t monitorCount; // 0..2
    VddMonitorSpec monitors[kMaxVirtualMonitors];
};

struct VddReply {
    uint32_t magic;
    uint32_t status;         // 0 = OK, nonzero = Win32/NTSTATUS error
    uint32_t activeMonitors; // Currently attached virtual monitors
};

#pragma pack(pop)

inline GUID MakeDeterministicMonitorGuid(uint32_t connectorIndex) {
    // Fixed Screen2PC virtual display container UUID base with connectorIndex in last 4 bytes.
    GUID g{ 0x53325043, 0x5644, 0x4430, { 0x80, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } };
    g.Data4[4] = static_cast<unsigned char>((connectorIndex >> 24) & 0xFFu);
    g.Data4[5] = static_cast<unsigned char>((connectorIndex >> 16) & 0xFFu);
    g.Data4[6] = static_cast<unsigned char>((connectorIndex >> 8) & 0xFFu);
    g.Data4[7] = static_cast<unsigned char>(connectorIndex & 0xFFu);
    return g;
}

} // namespace s2p::vdd
