#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>

namespace s2p {

enum class LogLevel {
    Info,
    Warn,
    Error
};

class Logger {
public:
    static Logger& Instance();

    void Init(const std::wstring& logFileName);
    void Log(LogLevel level, const char* code, const std::wstring& msg);
    void Info(const char* code, const std::wstring& msg);
    void Warn(const char* code, const std::wstring& msg);
    void Error(const char* code, const std::wstring& msg);

    std::vector<std::wstring> GetRecentLines(size_t maxLines = 100) const;

private:
    Logger() = default;
    mutable std::mutex m_mutex;
    std::vector<std::wstring> m_ring;
    std::wstring m_logPath;
    bool m_initialized = false;
};

std::wstring FormatHr(long hr);
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& w);

} // namespace s2p
