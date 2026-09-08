#include "loader/process_finder.hpp"
#include <thread>
#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace mitigator::loader {

std::optional<ProcessInfo> ProcessFinder::find_process(std::string_view process_name) {
#if defined(_WIN32)
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(snapshot, &entry)) {
        CloseHandle(snapshot);
        return std::nullopt;
    }

    std::wstring target_name_w(process_name.begin(), process_name.end());

    do {
        if (_wcsicmp(entry.szExeFile, target_name_w.c_str()) == 0) {
            HANDLE h_process = OpenProcess(
                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                FALSE,
                entry.th32ProcessID
            );

            const bool is_64 = h_process ? is_process_64_bit(h_process) : false;
            CloseHandle(snapshot);

            return ProcessInfo{
                .pid = entry.th32ProcessID,
                .name = std::string(process_name),
                .is_64_bit = is_64,
                .handle = h_process
            };
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return std::nullopt;
#else
    (void)process_name;
    return std::nullopt;
#endif
}

bool ProcessFinder::is_process_64_bit(void* process_handle) {
#if defined(_WIN32)
    if (!process_handle) return false;

    BOOL is_wow64 = FALSE;
    if (!IsWow64Process(static_cast<HANDLE>(process_handle), &is_wow64)) {
        return false;
    }

    // On 64-bit Windows, a 64-bit process returns FALSE for IsWow64Process
    SYSTEM_INFO sys_info{};
    GetNativeSystemInfo(&sys_info);
    const bool is_os_64 = (sys_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64);

    return is_os_64 && !is_wow64;
#else
    (void)process_handle;
    return false;
#endif
}

std::optional<ProcessInfo> ProcessFinder::wait_for_process(
    std::string_view process_name,
    uint32_t timeout_seconds
) {
    const auto start = std::chrono::steady_clock::now();

    while (true) {
        auto proc = find_process(process_name);
        if (proc.has_value()) {
            return proc;
        }

        if (timeout_seconds > 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start
            ).count();
            if (elapsed >= timeout_seconds) {
                return std::nullopt;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

} // namespace mitigator::loader
