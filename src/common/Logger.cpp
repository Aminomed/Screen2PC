#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "Logger.h"
#include <cstdio>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace s2p {

Logger& Logger::Instance() {
    static Logger inst;
    return inst;
}

void Logger::Init(const std::wstring& logFileName) {
    std::lock_guard<std::mutex> lock(m_mutex);
    wchar_t tempDir[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempDir);
    if (len > 0 && len < MAX_PATH) {
        m_logPath = std::wstring(tempDir) + logFileName;
    } else {
        m_logPath = logFileName;
    }
    m_initialized = true;
}

void Logger::Log(LogLevel level, const char* code, const std::wstring& msg) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tmLocal{};
    localtime_s(&tmLocal, &t);

    const wchar_t* lvlStr = L"INF";
    if (level == LogLevel::Warn) lvlStr = L"WRN";
    else if (level == LogLevel::Error) lvlStr = L"ERR";

    std::wostringstream oss;
    oss << std::setfill(L'0')
        << std::setw(2) << tmLocal.tm_hour << L":"
        << std::setw(2) << tmLocal.tm_min << L":"
        << std::setw(2) << tmLocal.tm_sec << L"."
        << std::setw(3) << ms.count()
        << L" [" << lvlStr << L"] [" << Utf8ToWide(code ? code : "S2P") << L"] " << msg;

    std::wstring line = oss.str();

    std::lock_guard<std::mutex> lock(m_mutex);
    m_ring.push_back(line);
    if (m_ring.size() > 300) {
        m_ring.erase(m_ring.begin());
    }

    OutputDebugStringW((line + L"\n").c_str());

    if (m_initialized && !m_logPath.empty()) {
        FILE* f = nullptr;
        if (_wfopen_s(&f, m_logPath.c_str(), L"a, ccs=UTF-8") == 0 && f) {
            fwprintf(f, L"%ls\n", line.c_str());
            fclose(f);
        }
    }
}

void Logger::Info(const char* code, const std::wstring& msg) {
    Log(LogLevel::Info, code, msg);
}

void Logger::Warn(const char* code, const std::wstring& msg) {
    Log(LogLevel::Warn, code, msg);
}

void Logger::Error(const char* code, const std::wstring& msg) {
    Log(LogLevel::Error, code, msg);
}

std::vector<std::wstring> Logger::GetRecentLines(size_t maxLines) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_ring.size() <= maxLines) return m_ring;
    return std::vector<std::wstring>(m_ring.end() - maxLines, m_ring.end());
}

std::wstring FormatHr(long hr) {
    wchar_t buf[64]{};
    swprintf_s(buf, L"0x%08X", static_cast<unsigned int>(hr));
    return buf;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring w(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), needed);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string s(needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), needed, nullptr, nullptr);
    return s;
}

} // namespace s2p
