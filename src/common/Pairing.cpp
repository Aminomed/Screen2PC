#include "Pairing.h"
#include "Logger.h"

#include <iphlpapi.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <mutex>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "bcrypt.lib")

namespace s2p {

namespace {

std::mutex g_trustIniMutex;
constexpr const wchar_t* kTrustedSection = L"TrustedDevices";

int HexCharValue(wchar_t ch) {
    if (ch >= L'0' && ch <= L'9') return ch - L'0';
    if (ch >= L'a' && ch <= L'f') return 10 + (ch - L'a');
    if (ch >= L'A' && ch <= L'F') return 10 + (ch - L'A');
    return -1;
}

std::wstring SanitizeField(const std::wstring& raw) {
    std::wstring out;
    out.reserve(raw.size());
    for (wchar_t ch : raw) {
        if (ch == L'|' || ch == L'\r' || ch == L'\n' || ch < 32) {
            out.push_back(L' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::wstring CurrentUtcIsoString() {
    SYSTEMTIME st{};
    GetSystemTime(&st);
    wchar_t buf[32]{};
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02uZ",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::vector<std::wstring> SplitPipe(const std::wstring& line) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (start <= line.size()) {
        size_t pos = line.find(L'|', start);
        if (pos == std::wstring::npos) {
            parts.push_back(line.substr(start));
            break;
        }
        parts.push_back(line.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

std::wstring ToUpperHexString(std::wstring s) {
    for (wchar_t& ch : s) {
        ch = static_cast<wchar_t>(std::towupper(ch));
    }
    return s;
}

void FillRandomBytes(uint8_t* dst, size_t len) {
    if (!dst || len == 0) return;
    if (BCRYPT_SUCCESS(BCryptGenRandom(nullptr, dst, static_cast<ULONG>(len), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return;
    }
    // Fallback deterministic/clock-seeded fill only if BCrypt fails unexpectedly
    uint64_t seed = GetTickCount64() ^ (static_cast<uint64_t>(GetCurrentProcessId()) << 32);
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    seed ^= static_cast<uint64_t>(qpc.QuadPart);
    for (size_t i = 0; i < len; ++i) {
        seed ^= seed >> 12;
        seed ^= seed << 25;
        seed ^= seed >> 27;
        uint64_t val = seed * 0x2545F4914F6CDD1DULL;
        dst[i] = static_cast<uint8_t>((val >> ((i & 7) * 8)) & 0xFFu);
    }
}

bool QueryMacByIfIndex(ULONG bestIfIndex, uint8_t outMac[6]) {
    std::memset(outMac, 0, 6);
    ULONG bufLen = 16384;
    std::vector<uint8_t> buf(bufLen);
    const ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST |
                        GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                    reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &bufLen);
    if (rc == ERROR_BUFFER_OVERFLOW && bufLen > 0 && bufLen <= 1024 * 1024) {
        buf.resize(bufLen);
        rc = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                  reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &bufLen);
    }
    if (rc != NO_ERROR) {
        return false;
    }

    auto* first = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());

    // First pass: exact interface index match
    if (bestIfIndex != 0) {
        for (auto* aa = first; aa; aa = aa->Next) {
            if (aa->IfIndex == bestIfIndex &&
                aa->PhysicalAddressLength == 6 &&
                !IsAllZero(aa->PhysicalAddress, 6)) {
                std::memcpy(outMac, aa->PhysicalAddress, 6);
                return true;
            }
        }
    }

    // Second pass: active Ethernet or Wi-Fi adapter
    for (auto* aa = first; aa; aa = aa->Next) {
        if (aa->OperStatus == IfOperStatusUp &&
            (aa->IfType == IF_TYPE_ETHERNET_CSMACD || aa->IfType == IF_TYPE_IEEE80211) &&
            aa->PhysicalAddressLength == 6 &&
            !IsAllZero(aa->PhysicalAddress, 6)) {
            std::memcpy(outMac, aa->PhysicalAddress, 6);
            return true;
        }
    }

    // Third pass: any non-loopback adapter with a 6-byte MAC
    for (auto* aa = first; aa; aa = aa->Next) {
        if (aa->IfType != IF_TYPE_SOFTWARE_LOOPBACK &&
            aa->PhysicalAddressLength == 6 &&
            !IsAllZero(aa->PhysicalAddress, 6)) {
            std::memcpy(outMac, aa->PhysicalAddress, 6);
            return true;
        }
    }

    return false;
}

} // namespace

bool IsAllZero(const uint8_t* data, size_t len) {
    if (!data || len == 0) return true;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}

std::wstring FormatHex(const uint8_t* data, size_t len) {
    if (!data || len == 0) return L"";
    static const wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[(data[i] >> 4) & 0xF]);
        out.push_back(kHex[data[i] & 0xF]);
    }
    return out;
}

bool ParseHex(const std::wstring& hex, uint8_t* out, size_t outLen) {
    if (!out || outLen == 0) return false;
    std::wstring clean;
    clean.reserve(hex.size());
    for (wchar_t ch : hex) {
        if (ch == L':' || ch == L'-' || ch == L' ') continue;
        clean.push_back(ch);
    }
    if (clean.size() != outLen * 2) return false;
    for (size_t i = 0; i < outLen; ++i) {
        int hi = HexCharValue(clean[i * 2]);
        int lo = HexCharValue(clean[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return true;
}

std::wstring FormatMacAddress(const uint8_t mac[6]) {
    if (!mac || IsAllZero(mac, 6)) return L"";
    wchar_t buf[24]{};
    swprintf_s(buf, L"%02X:%02X:%02X:%02X:%02X:%02X",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
}

std::wstring MakeTrustedDeviceKey(const uint8_t mac[6], const uint8_t deviceId[16]) {
    if (mac && !IsAllZero(mac, 6)) {
        return L"MAC_" + FormatHex(mac, 6);
    }
    if (deviceId && !IsAllZero(deviceId, 16)) {
        return L"ID_" + FormatHex(deviceId, 16);
    }
    return L"";
}

std::string GeneratePin6() {
    uint32_t pin = 0;
    const uint32_t limit = 4294000000u; // Largest multiple of 1,000,000 <= UINT32_MAX
    bool ok = false;
    for (int attempt = 0; attempt < 32; ++attempt) {
        uint32_t rnd = 0;
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr,
                                            reinterpret_cast<PUCHAR>(&rnd),
                                            sizeof(rnd),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            break;
        }
        if (rnd < limit) {
            pin = rnd % 1000000u;
            ok = true;
            break;
        }
    }
    if (!ok) {
        uint8_t bytes[4]{};
        FillRandomBytes(bytes, sizeof(bytes));
        uint32_t rnd = 0;
        std::memcpy(&rnd, bytes, sizeof(rnd));
        pin = rnd % 1000000u;
    }
    char buf[8]{};
    snprintf(buf, sizeof(buf), "%06u", pin);
    return std::string(buf);
}

bool ConstantTimePinEquals(const char* a, const char* b, size_t exactLen) {
    if (!a || !b || exactLen == 0) return false;
    size_t la = strnlen(a, exactLen + 1);
    size_t lb = strnlen(b, exactLen + 1);
    if (la != exactLen || lb != exactLen) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < exactLen; ++i) {
        diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

bool GetLocalPrimaryMac(uint8_t outMac[6]) {
    if (!outMac) return false;
    return QueryMacByIfIndex(0, outMac);
}

bool GetMacForTargetIp(const std::string& targetIp, uint8_t outMac[6]) {
    if (!outMac) return false;
    std::memset(outMac, 0, 6);
    if (!targetIp.empty()) {
        in_addr addr{};
        if (inet_pton(AF_INET, targetIp.c_str(), &addr) == 1) {
            DWORD bestIf = 0;
            if (GetBestInterface(addr.S_un.S_addr, &bestIf) == NO_ERROR && bestIf != 0) {
                if (QueryMacByIfIndex(bestIf, outMac)) {
                    return true;
                }
            }
        }
    }
    return GetLocalPrimaryMac(outMac);
}

bool ResolvePeerMacFromIp(const std::string& peerIp, uint8_t outMac[6]) {
    if (!outMac) return false;
    std::memset(outMac, 0, 6);
    if (peerIp.empty()) return false;

    in_addr dest{};
    if (inet_pton(AF_INET, peerIp.c_str(), &dest) != 1) {
        return false;
    }
    // Loopback (127.0.0.0/8) does not have a hardware ARP MAC address
    uint32_t hostOrder = ntohl(dest.S_un.S_addr);
    if ((hostOrder >> 24) == 127 || hostOrder == 0) {
        return false;
    }

    ULONG macAddr[2]{};
    ULONG physLen = 6;
    DWORD rc = SendARP(dest.S_un.S_addr, 0, macAddr, &physLen);
    if (rc == NO_ERROR && physLen == 6) {
        const auto* bytes = reinterpret_cast<const uint8_t*>(macAddr);
        if (!IsAllZero(bytes, 6)) {
            std::memcpy(outMac, bytes, 6);
            return true;
        }
    }
    return false;
}

bool LoadOrCreateLocalDeviceId(const std::wstring& iniPath, uint8_t outId[16], const wchar_t* section) {
    if (!outId) return false;
    std::memset(outId, 0, 16);
    if (iniPath.empty()) return false;
    const wchar_t* sec = (section && *section) ? section : L"Sender";

    std::lock_guard<std::mutex> lock(g_trustIniMutex);
    wchar_t buf[64]{};
    GetPrivateProfileStringW(sec, L"DeviceId", L"", buf, _countof(buf), iniPath.c_str());
    if (ParseHex(buf, outId, 16) && !IsAllZero(outId, 16)) {
        return true;
    }

    FillRandomBytes(outId, 16);
    outId[6] = static_cast<uint8_t>((outId[6] & 0x0Fu) | 0x40u); // UUID v4
    outId[8] = static_cast<uint8_t>((outId[8] & 0x3Fu) | 0x80u);
    if (IsAllZero(outId, 16)) {
        outId[0] = 0x53;
        outId[15] = 0x32;
    }
    std::wstring hex = FormatHex(outId, 16);
    WritePrivateProfileStringW(sec, L"DeviceId", hex.c_str(), iniPath.c_str());
    return true;
}

std::vector<TrustedDeviceEntry> LoadTrustedDevices(const std::wstring& iniPath) {
    std::vector<TrustedDeviceEntry> list;
    if (iniPath.empty()) return list;

    std::lock_guard<std::mutex> lock(g_trustIniMutex);
    DWORD cap = 8192;
    std::vector<wchar_t> buf(cap, L'\0');
    for (;;) {
        DWORD n = GetPrivateProfileSectionW(kTrustedSection, buf.data(), cap, iniPath.c_str());
        if (n < cap - 2) {
            break;
        }
        if (cap >= 65536) break;
        cap *= 2;
        buf.assign(cap, L'\0');
    }

    const wchar_t* p = buf.data();
    while (p && *p != L'\0') {
        std::wstring item(p);
        p += item.size() + 1;

        size_t eq = item.find(L'=');
        if (eq == std::wstring::npos || eq == 0) continue;

        TrustedDeviceEntry entry;
        entry.key = ToUpperHexString(item.substr(0, eq));
        std::wstring val = item.substr(eq + 1);
        auto parts = SplitPipe(val);
        if (parts.size() >= 1) entry.name = parts[0];
        if (parts.size() >= 2) entry.macStr = ToUpperHexString(parts[1]);
        if (parts.size() >= 3) entry.deviceIdHex = ToUpperHexString(parts[2]);
        if (parts.size() >= 4) entry.lastIp = parts[3];
        if (parts.size() >= 5) entry.addedUtc = parts[4];
        list.push_back(std::move(entry));
    }
    return list;
}

bool IsDeviceTrusted(const std::wstring& iniPath,
                     const uint8_t mac[6],
                     const uint8_t deviceId[16],
                     TrustedDeviceEntry* outMatch) {
    if (iniPath.empty()) return false;
    auto list = LoadTrustedDevices(iniPath);
    if (list.empty()) return false;

    const bool hasMac = (mac && !IsAllZero(mac, 6));
    const bool hasId = (deviceId && !IsAllZero(deviceId, 16));

    if (hasMac) {
        std::wstring macStr = FormatMacAddress(mac);
        std::wstring macKey = L"MAC_" + FormatHex(mac, 6);
        for (const auto& entry : list) {
            if ((!entry.macStr.empty() && _wcsicmp(entry.macStr.c_str(), macStr.c_str()) == 0) ||
                _wcsicmp(entry.key.c_str(), macKey.c_str()) == 0) {
                if (outMatch) *outMatch = entry;
                return true;
            }
        }
        return false;
    }

    if (hasId) {
        std::wstring idHex = FormatHex(deviceId, 16);
        std::wstring idKey = L"ID_" + idHex;
        for (const auto& entry : list) {
            if ((!entry.deviceIdHex.empty() && _wcsicmp(entry.deviceIdHex.c_str(), idHex.c_str()) == 0) ||
                _wcsicmp(entry.key.c_str(), idKey.c_str()) == 0) {
                if (outMatch) *outMatch = entry;
                return true;
            }
        }
    }

    return false;
}

bool SaveTrustedDevice(const std::wstring& iniPath,
                       const std::wstring& name,
                       const uint8_t mac[6],
                       const uint8_t deviceId[16],
                       const std::string& ip) {
    if (iniPath.empty()) return false;
    std::wstring key = MakeTrustedDeviceKey(mac, deviceId);
    if (key.empty()) return false;

    std::wstring cleanName = SanitizeField(name.empty() ? L"Surface" : name);
    std::wstring macStr = (mac && !IsAllZero(mac, 6)) ? FormatMacAddress(mac) : L"";
    std::wstring idHex = (deviceId && !IsAllZero(deviceId, 16)) ? FormatHex(deviceId, 16) : L"";
    std::wstring ipW = SanitizeField(Utf8ToWide(ip));
    std::wstring utc = CurrentUtcIsoString();

    std::wstring val = cleanName + L"|" + macStr + L"|" + idHex + L"|" + ipW + L"|" + utc;
    std::lock_guard<std::mutex> lock(g_trustIniMutex);
    return WritePrivateProfileStringW(kTrustedSection, key.c_str(), val.c_str(), iniPath.c_str()) != FALSE;
}

bool RemoveTrustedDevice(const std::wstring& iniPath, const std::wstring& key) {
    if (iniPath.empty() || key.empty()) return false;
    std::lock_guard<std::mutex> lock(g_trustIniMutex);
    return WritePrivateProfileStringW(kTrustedSection, key.c_str(), nullptr, iniPath.c_str()) != FALSE;
}

bool ClearTrustedDevices(const std::wstring& iniPath) {
    if (iniPath.empty()) return false;
    std::lock_guard<std::mutex> lock(g_trustIniMutex);
    return WritePrivateProfileStringW(kTrustedSection, nullptr, nullptr, iniPath.c_str()) != FALSE;
}

} // namespace s2p
