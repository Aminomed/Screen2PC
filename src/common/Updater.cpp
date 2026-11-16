#include "Updater.h"
#include "Version.h"
#include "Logger.h"

#include <winhttp.h>
#include <shellapi.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

namespace s2p {

namespace {

std::string EscapePsSingleQuoted(const std::wstring& ws) {
    std::string u8 = WideToUtf8(ws);
    std::string out;
    out.reserve(u8.size() + 8);
    for (char c : u8) {
        if (c == '\'') out += "''";
        else out.push_back(c);
    }
    return out;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

void AppendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class JsonCursor {
public:
    explicit JsonCursor(const std::string& src) : m_s(src) {}

    void SkipWs() {
        while (m_pos < m_s.size() &&
               (m_s[m_pos] == ' ' || m_s[m_pos] == '\t' || m_s[m_pos] == '\r' || m_s[m_pos] == '\n')) {
            ++m_pos;
        }
    }

    char Peek() {
        SkipWs();
        return m_pos < m_s.size() ? m_s[m_pos] : '\0';
    }

    bool Consume(char expected) {
        SkipWs();
        if (m_pos < m_s.size() && m_s[m_pos] == expected) {
            ++m_pos;
            return true;
        }
        return false;
    }

    bool ParseString(std::string& out) {
        SkipWs();
        if (m_pos >= m_s.size() || m_s[m_pos] != '"') return false;
        ++m_pos;
        out.clear();
        while (m_pos < m_s.size()) {
            char c = m_s[m_pos++];
            if (c == '"') return true;
            if (c == '\\') {
                if (m_pos >= m_s.size()) return false;
                char esc = m_s[m_pos++];
                switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (m_pos + 4 > m_s.size()) return false;
                    uint32_t cp = 0;
                    for (int i = 0; i < 4; ++i) {
                        char h = m_s[m_pos++];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<uint32_t>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<uint32_t>(h - 'A' + 10);
                        else return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && m_pos + 6 <= m_s.size() &&
                        m_s[m_pos] == '\\' && m_s[m_pos + 1] == 'u') {
                        uint32_t low = 0;
                        bool okLow = true;
                        for (int i = 0; i < 4; ++i) {
                            char h = m_s[m_pos + 2 + i];
                            low <<= 4;
                            if (h >= '0' && h <= '9') low |= static_cast<uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') low |= static_cast<uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') low |= static_cast<uint32_t>(h - 'A' + 10);
                            else { okLow = false; break; }
                        }
                        if (okLow && low >= 0xDC00 && low <= 0xDFFF) {
                            m_pos += 6;
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                        }
                    }
                    AppendUtf8Codepoint(out, cp);
                    break;
                }
                default:
                    out.push_back(esc);
                    break;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool ParseBool(bool& out) {
        SkipWs();
        if (m_s.compare(m_pos, 4, "true") == 0) {
            m_pos += 4;
            out = true;
            return true;
        }
        if (m_s.compare(m_pos, 5, "false") == 0) {
            m_pos += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool ParseUint64(uint64_t& out) {
        SkipWs();
        if (m_pos >= m_s.size() || !std::isdigit(static_cast<unsigned char>(m_s[m_pos]))) return false;
        uint64_t v = 0;
        while (m_pos < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_pos]))) {
            v = v * 10 + static_cast<uint64_t>(m_s[m_pos++] - '0');
        }
        out = v;
        return true;
    }

    bool SkipValue(int depth = 0) {
        if (depth > 64) return false;
        char c = Peek();
        if (c == '\0') return false;
        if (c == '"') {
            std::string dummy;
            return ParseString(dummy);
        }
        if (c == '{') {
            Consume('{');
            if (Consume('}')) return true;
            while (m_pos < m_s.size()) {
                std::string k;
                if (!ParseString(k)) return false;
                if (!Consume(':')) return false;
                if (!SkipValue(depth + 1)) return false;
                if (Consume(',')) continue;
                return Consume('}');
            }
            return false;
        }
        if (c == '[') {
            Consume('[');
            if (Consume(']')) return true;
            while (m_pos < m_s.size()) {
                if (!SkipValue(depth + 1)) return false;
                if (Consume(',')) continue;
                return Consume(']');
            }
            return false;
        }
        while (m_pos < m_s.size()) {
            char ch = m_s[m_pos];
            if (ch == ',' || ch == '}' || ch == ']' ||
                ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                return true;
            }
            ++m_pos;
        }
        return true;
    }

private:
    const std::string& m_s;
    size_t m_pos = 0;
};

struct WinHttpHandle {
    HINTERNET h = nullptr;
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET v) : h(v) {}
    ~WinHttpHandle() { if (h) WinHttpCloseHandle(h); }
    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;
    operator HINTERNET() const { return h; }
};

bool OpenHttpsRequest(const wchar_t* url,
                      const wchar_t* extraHeaders,
                      WinHttpHandle& hSession,
                      WinHttpHandle& hConnect,
                      WinHttpHandle& hRequest,
                      uint32_t& outStatus,
                      std::wstring& outErr) {
    outStatus = 0;
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256]{};
    wchar_t path[2048]{};
    wchar_t extra[1024]{};
    uc.lpszHostName = host;
    uc.dwHostNameLength = _countof(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = _countof(path);
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = _countof(extra);

    if (!WinHttpCrackUrl(url, 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS || host[0] == L'\0') {
        outErr = L"Ungueltige HTTPS-URL.";
        return false;
    }

    std::wstring ua = L"Screen2PC-Updater/" + FormatAppVersion(CurrentAppVersion());
    hSession.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession.h) {
        hSession.h = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    if (!hSession.h) {
        outErr = L"WinHTTP-Sitzung konnte nicht geoeffnet werden (" + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    WinHttpSetTimeouts(hSession.h, 5000, 8000, 10000, 30000);

    hConnect.h = WinHttpConnect(hSession.h, host, uc.nPort, 0);
    if (!hConnect.h) {
        outErr = L"Verbindung zu GitHub fehlgeschlagen (" + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    std::wstring fullPath = std::wstring(path) + std::wstring(extra);
    if (fullPath.empty()) fullPath = L"/";

    hRequest.h = WinHttpOpenRequest(hConnect.h, L"GET", fullPath.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    WINHTTP_FLAG_SECURE);
    if (!hRequest.h) {
        outErr = L"HTTPS-Anfrage konnte nicht erstellt werden (" + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hRequest.h, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    if (!WinHttpSendRequest(hRequest.h,
                            extraHeaders ? extraHeaders : WINHTTP_NO_ADDITIONAL_HEADERS,
                            extraHeaders ? static_cast<DWORD>(-1L) : 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest.h, nullptr)) {
        DWORD err = GetLastError();
        outErr = L"Netzwerkfehler bei GitHub-Abfrage (Code " + std::to_wstring(err) + L").";
        return false;
    }

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (WinHttpQueryHeaders(hRequest.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX)) {
        outStatus = status;
    }
    return true;
}

bool HttpDownloadToFile(const wchar_t* url,
                        const std::wstring& destPath,
                        uint64_t expectedSize,
                        uint64_t& outBytes,
                        std::wstring& outErr) {
    outBytes = 0;
    constexpr uint64_t kMaxDownloadBytes = 128ULL * 1024ULL * 1024ULL;
    if (expectedSize > kMaxDownloadBytes) {
        outErr = L"Update-Archiv ueberschreitet Sicherheitslimit (128 MB).";
        return false;
    }

    WinHttpHandle hSession, hConnect, hRequest;
    uint32_t status = 0;
    if (!OpenHttpsRequest(url, nullptr, hSession, hConnect, hRequest, status, outErr)) {
        return false;
    }
    if (status != 200) {
        outErr = L"Download fehlgeschlagen (HTTP " + std::to_wstring(status) + L").";
        return false;
    }

    std::wstring partPath = destPath + L".part";
    HANDLE hFile = CreateFileW(partPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        outErr = L"Temporaere Update-Datei konnte nicht erstellt werden (" + std::to_wstring(GetLastError()) + L").";
        return false;
    }

    std::vector<char> buf(65536);
    uint8_t magic[4]{};
    DWORD avail = 0;
    bool ok = true;

    for (;;) {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest.h, &avail)) {
            ok = false;
            outErr = L"Verbindungsfehler waehrend Update-Download (" + std::to_wstring(GetLastError()) + L").";
            break;
        }
        if (avail == 0) break;

        DWORD toRead = std::min<DWORD>(avail, static_cast<DWORD>(buf.size()));
        DWORD readBytes = 0;
        if (!WinHttpReadData(hRequest.h, buf.data(), toRead, &readBytes)) {
            ok = false;
            outErr = L"Lesefehler waehrend Update-Download (" + std::to_wstring(GetLastError()) + L").";
            break;
        }
        if (readBytes == 0) break;
        if (outBytes + readBytes > kMaxDownloadBytes) {
            ok = false;
            outErr = L"Update-Archiv ueberschreitet Sicherheitslimit (128 MB).";
            break;
        }
        for (DWORD i = 0; i < readBytes && outBytes + i < 4; ++i) {
            magic[outBytes + i] = static_cast<uint8_t>(buf[i]);
        }
        DWORD written = 0;
        if (!WriteFile(hFile, buf.data(), readBytes, &written, nullptr) || written != readBytes) {
            ok = false;
            outErr = L"Schreibfehler bei Update-Download (" + std::to_wstring(GetLastError()) + L").";
            break;
        }
        outBytes += readBytes;
    }

    CloseHandle(hFile);

    if (!ok) {
        DeleteFileW(partPath.c_str());
        return false;
    }

    if (expectedSize > 0 && outBytes != expectedSize) {
        DeleteFileW(partPath.c_str());
        outErr = L"Update-Download unvollstaendig (" + std::to_wstring(outBytes) +
                 L" von " + std::to_wstring(expectedSize) + L" Bytes erhalten).";
        return false;
    }

    if (outBytes < 65536 || magic[0] != 'P' || magic[1] != 'K' || magic[2] != 0x03 || magic[3] != 0x04) {
        DeleteFileW(partPath.c_str());
        outErr = L"Heruntergeladenes Paket ist kein gueltiges ZIP-Archiv.";
        return false;
    }

    if (!MoveFileExW(partPath.c_str(), destPath.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(partPath.c_str());
        outErr = L"Update-ZIP konnte nicht verschoben werden (" + std::to_wstring(GetLastError()) + L").";
        return false;
    }
    return true;
}

bool IsDirectoryWritable(const std::wstring& dir) {
    std::wstring probe = dir + L"\\.s2p_write_" + std::to_wstring(GetCurrentProcessId()) + L".tmp";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(h);
    DeleteFileW(probe.c_str());
    return true;
}

} // namespace

AppVersion CurrentAppVersion() {
    AppVersion v{};
    v.major = SCREEN2PC_VERSION_MAJOR;
    v.minor = SCREEN2PC_VERSION_MINOR;
    v.patch = SCREEN2PC_VERSION_PATCH;
    v.build = SCREEN2PC_BUILD_NUMBER;
    return v;
}

std::wstring FormatAppVersion(const AppVersion& v) {
    return std::to_wstring(v.major) + L"." + std::to_wstring(v.minor) + L"." +
           std::to_wstring(v.patch) + L"." + std::to_wstring(v.build);
}

bool ParseVersionString(const std::string& raw, AppVersion& out) {
    out = AppVersion{};
    size_t i = 0;
    while (i < raw.size() && std::isspace(static_cast<unsigned char>(raw[i]))) ++i;
    if (i < raw.size() && (raw[i] == 'v' || raw[i] == 'V')) ++i;

    int parts[4]{};
    int count = 0;
    while (i < raw.size() && count < 4) {
        if (!std::isdigit(static_cast<unsigned char>(raw[i]))) {
            if (raw[i] == '.' || raw[i] == '-' || raw[i] == '+') {
                ++i;
                // Skip non-digit label prefix such as "-b42" or "-build.42"
                while (i < raw.size() && !std::isdigit(static_cast<unsigned char>(raw[i])) &&
                       raw[i] != '.' && raw[i] != '-') {
                    ++i;
                }
                continue;
            }
            break;
        }
        int val = 0;
        while (i < raw.size() && std::isdigit(static_cast<unsigned char>(raw[i]))) {
            int digit = raw[i] - '0';
            if (val > 1000000) return false;
            val = val * 10 + digit;
            ++i;
        }
        parts[count++] = val;
        if (i < raw.size() && (raw[i] == '.' || raw[i] == '-' || raw[i] == '+')) {
            ++i;
            while (i < raw.size() && !std::isdigit(static_cast<unsigned char>(raw[i])) &&
                   raw[i] != '.' && raw[i] != '-') {
                ++i;
            }
        } else {
            break;
        }
    }

    if (count < 2) return false;
    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    out.build = parts[3];
    return true;
}

int CompareVersions(const AppVersion& a, const AppVersion& b) {
    if (a.major != b.major) return (a.major < b.major) ? -1 : 1;
    if (a.minor != b.minor) return (a.minor < b.minor) ? -1 : 1;
    if (a.patch != b.patch) return (a.patch < b.patch) ? -1 : 1;
    if (a.build != b.build) return (a.build < b.build) ? -1 : 1;
    return 0;
}

bool ParseLatestReleaseJson(const std::string& json,
                            const AppVersion& currentVersion,
                            UpdateReleaseInfo& out) {
    out = UpdateReleaseInfo{};
    JsonCursor cur(json);
    if (!cur.Consume('{')) {
        out.error = L"Ungueltige JSON-Antwort von GitHub.";
        return false;
    }
    if (cur.Consume('}')) {
        out.error = L"Leere GitHub Release-Antwort.";
        return false;
    }

    bool isDraft = false;
    std::string tagStr, nameStr, htmlStr, bodyStr;
    std::string chosenAssetUrl, fallbackAssetUrl;
    uint64_t chosenAssetSize = 0, fallbackAssetSize = 0;

    while (true) {
        std::string key;
        if (!cur.ParseString(key)) {
            out.error = L"JSON-Schluessel fehlerhaft.";
            return false;
        }
        if (!cur.Consume(':')) {
            out.error = L"JSON-Doppelpunkt erwartet.";
            return false;
        }

        auto parseOptStr = [&](std::string& dst) -> bool {
            if (cur.Peek() == '"') return cur.ParseString(dst);
            dst.clear();
            return cur.SkipValue();
        };

        bool stepOk = true;
        if (key == "tag_name") {
            stepOk = parseOptStr(tagStr);
        } else if (key == "name") {
            stepOk = parseOptStr(nameStr);
        } else if (key == "html_url") {
            stepOk = parseOptStr(htmlStr);
        } else if (key == "body") {
            stepOk = parseOptStr(bodyStr);
        } else if (key == "draft") {
            if (cur.Peek() == 't' || cur.Peek() == 'f') stepOk = cur.ParseBool(isDraft);
            else stepOk = cur.SkipValue();
        } else if (key == "prerelease") {
            if (cur.Peek() == 't' || cur.Peek() == 'f') stepOk = cur.ParseBool(out.prerelease);
            else stepOk = cur.SkipValue();
        } else if (key == "assets") {
            if (cur.Consume('[')) {
                if (!cur.Consume(']')) {
                    while (true) {
                        if (cur.Consume('{')) {
                            std::string assetName, assetUrl;
                            uint64_t assetSz = 0;
                            if (!cur.Consume('}')) {
                                while (true) {
                                    std::string ak;
                                    if (!cur.ParseString(ak) || !cur.Consume(':')) {
                                        stepOk = false;
                                        break;
                                    }
                                    if (ak == "name") {
                                        stepOk = parseOptStr(assetName);
                                    } else if (ak == "browser_download_url") {
                                        stepOk = parseOptStr(assetUrl);
                                    } else if (ak == "size") {
                                        if (!cur.ParseUint64(assetSz)) stepOk = cur.SkipValue();
                                    } else {
                                        stepOk = cur.SkipValue();
                                    }
                                    if (!stepOk) break;
                                    if (cur.Consume(',')) continue;
                                    stepOk = cur.Consume('}');
                                    break;
                                }
                            }
                            if (!stepOk) break;
                            std::string lowerName = ToLowerAscii(assetName);
                            if (lowerName == "screen2pc-windows-x64.zip") {
                                chosenAssetUrl = assetUrl;
                                chosenAssetSize = assetSz;
                            } else if (fallbackAssetUrl.empty() && lowerName.size() > 4 &&
                                       lowerName.compare(lowerName.size() - 4, 4, ".zip") == 0) {
                                fallbackAssetUrl = assetUrl;
                                fallbackAssetSize = assetSz;
                            }
                        } else {
                            stepOk = cur.SkipValue();
                            if (!stepOk) break;
                        }
                        if (cur.Consume(',')) continue;
                        stepOk = cur.Consume(']');
                        break;
                    }
                }
            } else {
                stepOk = cur.SkipValue();
            }
        } else {
            stepOk = cur.SkipValue();
        }

        if (!stepOk) {
            out.error = L"Unerwartetes JSON-Format in GitHub Release.";
            return false;
        }

        if (cur.Consume(',')) continue;
        if (cur.Consume('}')) break;
        out.error = L"JSON-Objekt nicht sauber abgeschlossen.";
        return false;
    }

    if (chosenAssetUrl.empty()) {
        chosenAssetUrl = fallbackAssetUrl;
        chosenAssetSize = fallbackAssetSize;
    }

    out.tagName = Utf8ToWide(tagStr);
    out.releaseName = Utf8ToWide(nameStr.empty() ? tagStr : nameStr);
    out.htmlUrl = Utf8ToWide(htmlStr);
    out.downloadUrl = Utf8ToWide(chosenAssetUrl);
    out.releaseNotes = Utf8ToWide(bodyStr);
    out.assetSize = chosenAssetSize;

    if (isDraft) {
        out.error = L"Release ist noch ein Entwurf.";
        return false;
    }
    if (!ParseVersionString(tagStr, out.version)) {
        out.error = L"Release-Tag enthaelt keine gueltige Versionsnummer: " + out.tagName;
        return false;
    }
    if (out.downloadUrl.empty()) {
        out.error = L"Im GitHub-Release fehlt das Download-Paket Screen2PC-Windows-x64.zip.";
        return false;
    }

    out.valid = true;
    out.updateAvailable = CompareVersions(out.version, currentVersion) > 0;
    return true;
}

UpdateReleaseInfo CheckForGitHubUpdate(const wchar_t* apiUrl) {
    UpdateReleaseInfo info{};
    WinHttpHandle hSession, hConnect, hRequest;
    std::wstring err;
    uint32_t status = 0;

    if (!OpenHttpsRequest(apiUrl, L"Accept: application/vnd.github+json\r\n",
                          hSession, hConnect, hRequest, status, err)) {
        info.error = err;
        return info;
    }
    info.httpStatus = status;

    if (status == 404) {
        info.error = L"Noch kein GitHub-Release veroeffentlicht.";
        return info;
    }
    if (status == 403 || status == 429) {
        info.error = L"GitHub API-Anfragelimit erreicht. Bitte spaeter erneut versuchen.";
        return info;
    }
    if (status != 200) {
        info.error = L"GitHub API meldete HTTP " + std::to_wstring(status) + L".";
        return info;
    }

    constexpr size_t kMaxJsonBytes = 2 * 1024 * 1024;
    std::string body;
    DWORD avail = 0;
    for (;;) {
        avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest.h, &avail)) {
            info.error = L"Verbindungsfehler bei GitHub API-Antwort (" + std::to_wstring(GetLastError()) + L").";
            return info;
        }
        if (avail == 0) break;
        if (avail > kMaxJsonBytes - body.size()) {
            info.error = L"GitHub-Antwort ist unerwartet gross.";
            return info;
        }
        size_t oldSz = body.size();
        body.resize(oldSz + avail);
        DWORD readBytes = 0;
        if (!WinHttpReadData(hRequest.h, body.data() + oldSz, avail, &readBytes)) {
            info.error = L"Lesefehler bei GitHub API-Antwort.";
            return info;
        }
        body.resize(oldSz + readBytes);
        if (readBytes == 0) break;
    }

    ParseLatestReleaseJson(body, CurrentAppVersion(), info);
    return info;
}

bool DownloadAndLaunchSelfUpdate(const UpdateReleaseInfo& info,
                                 bool relaunchMinimizedToTray,
                                 bool checkDriverUpdate,
                                 std::wstring& outError) {
    if (!info.valid || info.downloadUrl.empty()) {
        outError = L"Keine gueltige Update-URL vorhanden.";
        return false;
    }

    std::vector<wchar_t> exeBuf(32768, L'\0');
    DWORD exeLen = GetModuleFileNameW(nullptr, exeBuf.data(), static_cast<DWORD>(exeBuf.size()));
    if (exeLen == 0 || exeLen >= exeBuf.size()) {
        outError = L"Programmpfad konnte nicht ermittelt werden.";
        return false;
    }
    std::wstring exePath(exeBuf.data(), exeLen);
    size_t slash = exePath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        outError = L"Ungueltiger Programmordner.";
        return false;
    }
    std::wstring appDir = exePath.substr(0, slash);

    wchar_t tempBase[MAX_PATH]{};
    DWORD tn = GetTempPathW(MAX_PATH, tempBase);
    if (tn == 0 || tn >= MAX_PATH) {
        outError = L"Temporaerer Ordner nicht verfuegbar.";
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    std::wstring updateDir = std::wstring(tempBase) + L"Screen2PC_Update_" + std::to_wstring(pid);
    CreateDirectoryW(updateDir.c_str(), nullptr);

    std::wstring zipPath = updateDir + L"\\Screen2PC-Windows-x64.zip";
    uint64_t downloadedBytes = 0;
    if (!HttpDownloadToFile(info.downloadUrl.c_str(), zipPath, info.assetSize, downloadedBytes, outError)) {
        RemoveDirectoryW(updateDir.c_str());
        return false;
    }

    Logger::Instance().Info("UPD-DL", L"Downloaded update ZIP (" + std::to_wstring(downloadedBytes) + L" bytes) to " + zipPath);

    std::wstring psPath = updateDir + L"\\apply_update.ps1";
    std::wstring relaunchArgs = relaunchMinimizedToTray ? L"--tray" : L"";
    bool needAdmin = !IsDirectoryWritable(appDir);

    std::string ps;
    ps += "$ErrorActionPreference = 'Stop'\r\n";
    ps += "$ParentPid = " + std::to_string(pid) + "\r\n";
    ps += "$ZipPath = '" + EscapePsSingleQuoted(zipPath) + "'\r\n";
    ps += "$TargetDir = '" + EscapePsSingleQuoted(appDir) + "'\r\n";
    ps += "$RelaunchExe = '" + EscapePsSingleQuoted(exePath) + "'\r\n";
    ps += "$RelaunchArgs = '" + EscapePsSingleQuoted(relaunchArgs) + "'\r\n";
    ps += "$CheckDriver = " + std::string(checkDriverUpdate ? "1" : "0") + "\r\n";
    ps += R"PS(
try {
    Write-Host "Warte auf Beendigung von Screen2PC (PID $ParentPid)..." -ForegroundColor Cyan
    for ($i = 0; $i -lt 60; $i++) {
        $p = Get-Process -Id $ParentPid -ErrorAction SilentlyContinue
        if (-not $p) { break }
        Start-Sleep -Milliseconds 250
    }
    $p = Get-Process -Id $ParentPid -ErrorAction SilentlyContinue
    if ($p) {
        Stop-Process -Id $ParentPid -Force -ErrorAction SilentlyContinue
        Start-Sleep -Milliseconds 500
    }

    $StageDir = Join-Path (Split-Path -Parent $ZipPath) 'extracted'
    if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
    Unblock-File -Path $ZipPath -ErrorAction SilentlyContinue
    Write-Host "Entpacke Update..." -ForegroundColor Cyan
    Expand-Archive -Path $ZipPath -DestinationPath $StageDir -Force

    $DriverChanged = $false
    if ($CheckDriver -eq 1) {
        $OldDll = Join-Path $TargetDir 'driver\Screen2PCDriver.dll'
        $NewDll = Join-Path $StageDir 'driver\Screen2PCDriver.dll'
        $OldInf = Join-Path $TargetDir 'driver\Screen2PCDriver.inf'
        $NewInf = Join-Path $StageDir 'driver\Screen2PCDriver.inf'
        if ((Test-Path $OldDll) -and (Test-Path $NewDll)) {
            $h1 = (Get-FileHash $OldDll -Algorithm SHA256).Hash
            $h2 = (Get-FileHash $NewDll -Algorithm SHA256).Hash
            $i1 = if (Test-Path $OldInf) { (Get-FileHash $OldInf -Algorithm SHA256).Hash } else { '' }
            $i2 = if (Test-Path $NewInf) { (Get-FileHash $NewInf -Algorithm SHA256).Hash } else { '' }
            if (($h1 -ne $h2) -or ($i1 -ne $i2)) { $DriverChanged = $true }
        }
    }

    Write-Host "Installiere neue Dateien nach $TargetDir..." -ForegroundColor Cyan
    $StagePrefixLen = $StageDir.TrimEnd('\', '/').Length + 1
    for ($attempt = 1; $attempt -le 5; $attempt++) {
        try {
            Get-ChildItem -LiteralPath $StageDir -File -Recurse -Force | ForEach-Object {
                $relPath = $_.FullName.Substring($StagePrefixLen)
                $dstFile = Join-Path $TargetDir $relPath
                $dstParent = Split-Path -Parent $dstFile
                if (-not (Test-Path -LiteralPath $dstParent)) {
                    New-Item -ItemType Directory -Path $dstParent -Force | Out-Null
                }
                Copy-Item -LiteralPath $_.FullName -Destination $dstFile -Force
            }
            break
        } catch {
            if ($attempt -eq 5) { throw }
            Start-Sleep -Milliseconds 400
        }
    }
    Get-ChildItem -LiteralPath $TargetDir -Recurse -File -ErrorAction SilentlyContinue | Unblock-File -ErrorAction SilentlyContinue

    if ($DriverChanged) {
        try {
            $SetupExe = Join-Path $TargetDir 'Screen2PC.DriverSetup.exe'
            $InstallPs1 = Join-Path $TargetDir 'install_driver.ps1'
            if (-not (Test-Path $InstallPs1)) { $InstallPs1 = Join-Path $TargetDir 'scripts\install_driver.ps1' }
            if ((Test-Path $SetupExe) -and (Test-Path $InstallPs1)) {
                & $SetupExe --status > $null 2>&1
                if ($LASTEXITCODE -eq 0) {
                    Write-Host "Virtual Display Treiber wurde geaendert - aktualisiere Treiber (Admin)..." -ForegroundColor Yellow
                    Start-Process powershell.exe -ArgumentList ('-ExecutionPolicy Bypass -File "' + $InstallPs1 + '"') -Verb RunAs -Wait
                }
            }
        } catch {
            Write-Warning "Treiber-Update uebersprungen oder abgebrochen: $($_.Exception.Message)"
        }
    }

    if ($RelaunchExe -and (Test-Path $RelaunchExe)) {
        Write-Host "Starte Screen2PC neu..." -ForegroundColor Green
        if ($RelaunchArgs) {
            Start-Process -FilePath $RelaunchExe -ArgumentList $RelaunchArgs
        } else {
            Start-Process -FilePath $RelaunchExe
        }
    }

    Start-Sleep -Milliseconds 300
    Remove-Item -Path (Split-Path -Parent $ZipPath) -Recurse -Force -ErrorAction SilentlyContinue
} catch {
    Write-Host ""
    Write-Host "Update fehlgeschlagen: $($_.Exception.Message)" -ForegroundColor Red
    Read-Host "Druecke Enter zum Schliessen"
}
)PS";

    {
        std::ofstream ofs(psPath, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            DeleteFileW(zipPath.c_str());
            RemoveDirectoryW(updateDir.c_str());
            outError = L"Update-Skript konnte nicht geschrieben werden.";
            return false;
        }
        // Write UTF-8 BOM so Windows PowerShell 5.1 parses paths with non-ASCII characters accurately
        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        ofs.write(reinterpret_cast<const char*>(bom), 3);
        ofs.write(ps.data(), static_cast<std::streamsize>(ps.size()));
    }

    std::wstring psArgs = L"-NoProfile -ExecutionPolicy Bypass -File \"" + psPath + L"\"";
    HINSTANCE res = ShellExecuteW(
        nullptr,
        needAdmin ? L"runas" : L"open",
        L"powershell.exe",
        psArgs.c_str(),
        appDir.c_str(),
        SW_SHOWNORMAL);

    if (reinterpret_cast<INT_PTR>(res) <= 32) {
        DeleteFileW(psPath.c_str());
        DeleteFileW(zipPath.c_str());
        RemoveDirectoryW(updateDir.c_str());
        outError = L"Updater-Skript konnte nicht gestartet werden (Code " +
                   std::to_wstring(reinterpret_cast<INT_PTR>(res)) + L").";
        return false;
    }

    return true;
}

} // namespace s2p
