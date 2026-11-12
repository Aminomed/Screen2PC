#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <iostream>
#include <string>
#include "../driver/DriverProtocol.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace {

bool FindExistingDevice(HDEVINFO devInfo, SP_DEVINFO_DATA& outData) {
    outData.cbSize = sizeof(SP_DEVINFO_DATA);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &outData); ++i) {
        wchar_t hwIds[512] = {};
        if (SetupDiGetDeviceRegistryPropertyW(devInfo, &outData, SPDRP_HARDWAREID, nullptr,
                                              reinterpret_cast<PBYTE>(hwIds), sizeof(hwIds), nullptr)) {
            for (const wchar_t* p = hwIds; *p; p += wcslen(p) + 1) {
                if (_wcsicmp(p, s2p::vdd::kHardwareId) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

int InstallDriver(const std::wstring& infPath) {
    wchar_t fullInf[MAX_PATH] = {};
    if (!GetFullPathNameW(infPath.c_str(), MAX_PATH, fullInf, nullptr)) {
        std::wcerr << L"Invalid INF path: " << infPath << L"\n";
        return 1;
    }

    HDEVINFO devInfo = SetupDiCreateDeviceInfoList(&GUID_DEVCLASS_DISPLAY, nullptr);
    if (devInfo == INVALID_HANDLE_VALUE) {
        std::wcerr << L"SetupDiCreateDeviceInfoList failed: " << GetLastError() << L"\n";
        return 2;
    }

    // Check if already exists across all devices
    HDEVINFO existingSet = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    bool exists = (existingSet != INVALID_HANDLE_VALUE) && FindExistingDevice(existingSet, devData);
    if (existingSet != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(existingSet);

    if (!exists) {
        if (!SetupDiCreateDeviceInfoW(devInfo, L"Screen2PCVDD", &GUID_DEVCLASS_DISPLAY,
                                      L"Screen2PC Virtual Display Adapter", nullptr, DICD_GENERATE_ID, &devData)) {
            std::wcerr << L"SetupDiCreateDeviceInfoW failed: " << GetLastError() << L"\n";
            SetupDiDestroyDeviceInfoList(devInfo);
            return 3;
        }

        // HardwareID must be multi-sz (double null terminated)
        wchar_t hwIdMulti[64] = {};
        wcsncpy_s(hwIdMulti, s2p::vdd::kHardwareId, 60);
        DWORD propLen = static_cast<DWORD>((wcslen(hwIdMulti) + 2) * sizeof(wchar_t));
        if (!SetupDiSetDeviceRegistryPropertyW(devInfo, &devData, SPDRP_HARDWAREID,
                                               reinterpret_cast<const BYTE*>(hwIdMulti), propLen)) {
            std::wcerr << L"SetupDiSetDeviceRegistryPropertyW failed: " << GetLastError() << L"\n";
            SetupDiDestroyDeviceInfoList(devInfo);
            return 4;
        }

        if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, devInfo, &devData)) {
            std::wcerr << L"SetupDiCallClassInstaller(DIF_REGISTERDEVICE) failed: " << GetLastError() << L"\n";
            SetupDiDestroyDeviceInfoList(devInfo);
            return 5;
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);

    BOOL rebootRequired = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, s2p::vdd::kHardwareId, fullInf, INSTALLFLAG_FORCE, &rebootRequired)) {
        DWORD err = GetLastError();
        std::wcerr << L"UpdateDriverForPlugAndPlayDevicesW failed: " << err << L" (0x" << std::hex << err << std::dec << L")\n";
        return 6;
    }

    std::wcout << L"Driver installed successfully." << (rebootRequired ? L" Reboot required." : L"") << L"\n";
    return 0;
}

int UninstallDriver() {
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    if (devInfo == INVALID_HANDLE_VALUE) return 1;

    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    int removed = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfo, i, &devData); ++i) {
        wchar_t hwIds[512] = {};
        if (SetupDiGetDeviceRegistryPropertyW(devInfo, &devData, SPDRP_HARDWAREID, nullptr,
                                              reinterpret_cast<PBYTE>(hwIds), sizeof(hwIds), nullptr)) {
            bool match = false;
            for (const wchar_t* p = hwIds; *p; p += wcslen(p) + 1) {
                if (_wcsicmp(p, s2p::vdd::kHardwareId) == 0) {
                    match = true;
                    break;
                }
            }
            if (match) {
                BOOL reboot = FALSE;
                if (DiUninstallDevice(nullptr, devInfo, &devData, 0, &reboot)) {
                    ++removed;
                } else {
                    SetupDiCallClassInstaller(DIF_REMOVE, devInfo, &devData);
                    ++removed;
                }
            }
        }
    }
    SetupDiDestroyDeviceInfoList(devInfo);
    std::wcout << L"Removed " << removed << L" Screen2PC virtual display device(s).\n";
    return 0;
}

int CheckStatus() {
    HDEVINFO devInfo = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, 0);
    if (devInfo == INVALID_HANDLE_VALUE) return 1;

    SP_DEVINFO_DATA devData{};
    devData.cbSize = sizeof(devData);
    bool found = FindExistingDevice(devInfo, devData);
    if (!found) {
        SetupDiDestroyDeviceInfoList(devInfo);
        std::wcout << L"STATUS=NOT_INSTALLED\n";
        return 2;
    }

    ULONG status = 0, prob = 0;
    CONFIGRET cr = CM_Get_DevNode_Status(&status, &prob, devData.DevInst, 0);
    SetupDiDestroyDeviceInfoList(devInfo);
    if (cr != CR_SUCCESS) {
        std::wcout << L"STATUS=UNKNOWN CR=" << cr << L"\n";
        return 3;
    }
    std::wcout << L"STATUS=" << (prob == 0 ? L"OK" : L"PROBLEM") << L" PROBLEM_CODE=" << prob << L"\n";
    return prob == 0 ? 0 : 4;
}

int PingPipe(uint32_t setCount = 99, uint32_t w1 = 2560, uint32_t h1 = 1440, uint32_t w2 = 1920, uint32_t h2 = 1080) {
    HANDLE hPipe = CreateFileW(s2p::vdd::kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) {
        std::wcerr << L"Pipe open failed: " << GetLastError() << L"\n";
        return 1;
    }
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    s2p::vdd::VddCommand cmd{};
    cmd.magic = s2p::vdd::kVddMagic;
    if (setCount <= 2) {
        cmd.op = static_cast<uint32_t>(s2p::vdd::VddOp::SetMonitors);
        cmd.monitorCount = setCount;
        cmd.monitors[0] = { w1, h1, 60 };
        cmd.monitors[1] = { w2, h2, 60 };
    } else {
        cmd.op = static_cast<uint32_t>(s2p::vdd::VddOp::Ping);
    }

    DWORD written = 0, readBytes = 0;
    s2p::vdd::VddReply reply{};
    if (!TransactNamedPipe(hPipe, &cmd, sizeof(cmd), &reply, sizeof(reply), &readBytes, nullptr)) {
        // Fallback WriteFile + ReadFile
        if (!WriteFile(hPipe, &cmd, sizeof(cmd), &written, nullptr) ||
            !ReadFile(hPipe, &reply, sizeof(reply), &readBytes, nullptr)) {
            std::wcerr << L"Pipe transaction failed: " << GetLastError() << L"\n";
            CloseHandle(hPipe);
            return 2;
        }
    }
    CloseHandle(hPipe);
    std::wcout << L"PIPE_OK status=" << reply.status << L" activeMonitors=" << reply.activeMonitors << L"\n";
    return reply.status == 0 ? 0 : 3;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::wcout << L"Usage:\n"
                   << L"  Screen2PC.DriverSetup.exe --install <Screen2PCDriver.inf>\n"
                   << L"  Screen2PC.DriverSetup.exe --uninstall\n"
                   << L"  Screen2PC.DriverSetup.exe --status\n"
                   << L"  Screen2PC.DriverSetup.exe --ping\n"
                   << L"  Screen2PC.DriverSetup.exe --set <0|1|2>\n";
        return 0;
    }
    std::wstring arg1 = argv[1];
    if (arg1 == L"--install" && argc >= 3) return InstallDriver(argv[2]);
    if (arg1 == L"--uninstall") return UninstallDriver();
    if (arg1 == L"--status") return CheckStatus();
    if (arg1 == L"--ping") return PingPipe();
    if (arg1 == L"--set" && argc >= 3) return PingPipe(static_cast<uint32_t>(_wtoi(argv[2])));
    return 1;
}
