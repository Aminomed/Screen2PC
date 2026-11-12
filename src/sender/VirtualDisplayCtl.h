#pragma once
#include <vector>
#include <cstdint>
#include "../driver/DriverProtocol.h"

namespace s2p {

class VirtualDisplayCtl {
public:
    static bool IsDriverActive();
    static bool SetMonitors(const std::vector<vdd::VddMonitorSpec>& specs);
    static bool ClearMonitors();

private:
    static bool SendCommand(const vdd::VddCommand& cmd, vdd::VddReply& outReply);
    static void SaveConfigFile(const vdd::VddCommand& cmd);
    static void DeleteConfigFile();
};

} // namespace s2p
