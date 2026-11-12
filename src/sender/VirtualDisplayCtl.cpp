#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "VirtualDisplayCtl.h"
#include "../common/Logger.h"
#include <algorithm>
#include <cstring>

namespace s2p {

bool VirtualDisplayCtl::SendCommand(const vdd::VddCommand& cmd, vdd::VddReply& outReply) {
    if (!WaitNamedPipeW(vdd::kPipeName, 500)) {
        return false;
    }
    HANDLE hPipe = CreateFileW(vdd::kPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    DWORD readBytes = 0;
    BOOL ok = TransactNamedPipe(hPipe, const_cast<vdd::VddCommand*>(&cmd), sizeof(cmd),
                                &outReply, sizeof(outReply), &readBytes, nullptr);
    if (!ok) {
        DWORD written = 0;
        ok = WriteFile(hPipe, &cmd, sizeof(cmd), &written, nullptr) &&
             ReadFile(hPipe, &outReply, sizeof(outReply), &readBytes, nullptr);
    }
    CloseHandle(hPipe);
    return ok && readBytes == sizeof(outReply) && outReply.magic == vdd::kVddMagic;
}

void VirtualDisplayCtl::SaveConfigFile(const vdd::VddCommand& cmd) {
    CreateDirectoryW(vdd::kConfigDir, nullptr);
    HANDLE hFile = CreateFileW(vdd::kConfigFile, GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(hFile, &cmd, sizeof(cmd), &written, nullptr);
        CloseHandle(hFile);
    }
}

void VirtualDisplayCtl::DeleteConfigFile() {
    DeleteFileW(vdd::kConfigFile);
}

bool VirtualDisplayCtl::IsDriverActive() {
    vdd::VddCommand cmd{};
    cmd.magic = vdd::kVddMagic;
    cmd.op = static_cast<uint32_t>(vdd::VddOp::Ping);
    vdd::VddReply reply{};
    return SendCommand(cmd, reply) && reply.status == 0;
}

bool VirtualDisplayCtl::SetMonitors(const std::vector<vdd::VddMonitorSpec>& specs) {
    vdd::VddCommand cmd{};
    cmd.magic = vdd::kVddMagic;
    cmd.op = static_cast<uint32_t>(vdd::VddOp::SetMonitors);
    cmd.monitorCount = static_cast<uint32_t>(std::min<size_t>(specs.size(), vdd::kMaxVirtualMonitors));
    for (uint32_t i = 0; i < cmd.monitorCount; ++i) {
        cmd.monitors[i] = specs[i];
    }

    if (cmd.monitorCount > 0) {
        SaveConfigFile(cmd);
    } else {
        DeleteConfigFile();
    }

    vdd::VddReply reply{};
    if (!SendCommand(cmd, reply)) {
        Logger::Instance().Error("VDD_PIPE", L"Virtual Display Driver not reachable on NamedPipe.");
        return false;
    }
    if (reply.status != 0) {
        Logger::Instance().Error("VDD_STATUS", L"Driver returned error status: " + std::to_wstring(reply.status));
        return false;
    }
    return reply.activeMonitors == cmd.monitorCount;
}

bool VirtualDisplayCtl::ClearMonitors() {
    DeleteConfigFile();
    vdd::VddCommand cmd{};
    cmd.magic = vdd::kVddMagic;
    cmd.op = static_cast<uint32_t>(vdd::VddOp::SetMonitors);
    cmd.monitorCount = 0;
    vdd::VddReply reply{};
    return SendCommand(cmd, reply);
}

} // namespace s2p
