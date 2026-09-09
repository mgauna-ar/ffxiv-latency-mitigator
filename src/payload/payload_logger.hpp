#pragma once

#include <string_view>
#include <fstream>
#include <chrono>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace mitigator::payload {

inline void log_debug(std::string_view msg) {
    wchar_t temp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp_dir) == 0) return;
    const std::wstring log_path = std::wstring(temp_dir) + L"ffxiv_mitigator_payload.log";

    std::ofstream f(log_path, std::ios::app);
    if (f.is_open()) {
        const auto now = std::chrono::system_clock::now();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        f << "[" << ms << "] " << msg << "\n";
        f.flush();
    }
}

} // namespace mitigator::payload
