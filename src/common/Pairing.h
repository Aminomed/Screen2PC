#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace s2p {

struct TrustedDeviceEntry {
    std::wstring key;         // INI key: "MAC_AABBCCDDEEFF" or "ID_<32hex>"
    std::wstring name;
    std::wstring macStr;      // "AA:BB:CC:DD:EE:FF" or ""
    std::wstring deviceIdHex; // 32 uppercase hex chars or ""
    std::wstring lastIp;
    std::wstring addedUtc;
};

bool IsAllZero(const uint8_t* data, size_t len);
std::wstring FormatHex(const uint8_t* data, size_t len);
bool ParseHex(const std::wstring& hex, uint8_t* out, size_t outLen);
std::wstring FormatMacAddress(const uint8_t mac[6]);
std::wstring MakeTrustedDeviceKey(const uint8_t mac[6], const uint8_t deviceId[16]);

// Cryptographic 6-digit PIN generation + constant-time verification
std::string GeneratePin6();
bool ConstantTimePinEquals(const char* a, const char* b, size_t exactLen = 6);

// Network identity helpers (Windows IPHelper / ARP)
bool GetLocalPrimaryMac(uint8_t outMac[6]);
bool GetMacForTargetIp(const std::string& targetIp, uint8_t outMac[6]);
bool ResolvePeerMacFromIp(const std::string& peerIp, uint8_t outMac[6]);
bool LoadOrCreateLocalDeviceId(const std::wstring& iniPath, uint8_t outId[16], const wchar_t* section = L"Sender");

// Trusted-device persistence ([TrustedDevices] section in receiver.ini)
std::vector<TrustedDeviceEntry> LoadTrustedDevices(const std::wstring& iniPath);
bool IsDeviceTrusted(const std::wstring& iniPath,
                     const uint8_t mac[6],
                     const uint8_t deviceId[16],
                     TrustedDeviceEntry* outMatch = nullptr);
bool SaveTrustedDevice(const std::wstring& iniPath,
                       const std::wstring& name,
                       const uint8_t mac[6],
                       const uint8_t deviceId[16],
                       const std::string& ip);
bool RemoveTrustedDevice(const std::wstring& iniPath, const std::wstring& key);
bool ClearTrustedDevices(const std::wstring& iniPath);

} // namespace s2p
