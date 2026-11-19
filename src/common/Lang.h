#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <atomic>

namespace s2p::lang {

enum class LangMode : int {
    Auto = 0,
    De   = 1,
    En   = 2
};

inline std::atomic<int>& ModeStorage() {
    static std::atomic<int> mode{ 0 };
    return mode;
}

inline bool DetectSystemGerman() {
    LANGID lid = GetUserDefaultUILanguage();
    return (PRIMARYLANGID(lid) == LANG_GERMAN);
}

inline void SetLangMode(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    ModeStorage().store(mode, std::memory_order_relaxed);
}

inline int GetLangMode() {
    return ModeStorage().load(std::memory_order_relaxed);
}

inline bool IsEnglish() {
    int m = GetLangMode();
    if (m == 1) return false;
    if (m == 2) return true;
    return !DetectSystemGerman();
}

inline const wchar_t* Tr(const wchar_t* de, const wchar_t* en) {
    return IsEnglish() ? en : de;
}

} // namespace s2p::lang
