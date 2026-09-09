#include "loader/process_finder.hpp"
#include <thread>
#include <chrono>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace mitigator::loader {

bool ProcessFinder::enable_debug_privilege() {
    HANDLE h_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &h_token)) {
        return false;
    }

    auto enable_priv = [&](LPCWSTR priv_name) -> bool {
        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, priv_name, &luid)) {
            return false;
        }
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        const BOOL ok = AdjustTokenPrivileges(h_token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
        return (ok && GetLastError() != ERROR_NOT_ALL_ASSIGNED);
    };

    const bool debug_ok = enable_priv(L"SeDebugPrivilege");
    enable_priv(L"SeSecurityPrivilege"); // Best-effort for SACL assignment

    CloseHandle(h_token);
    return debug_ok;
}

std::optional<ProcessInfo> ProcessFinder::find_process(std::string_view process_name) {
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
    std::optional<ProcessInfo> fallback_proc;

    do {
        if (_wcsicmp(entry.szExeFile, target_name_w.c_str()) == 0) {
            HANDLE h_process = OpenProcess(
                PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                FALSE,
                entry.th32ProcessID
            );

            const DWORD err = h_process ? 0 : GetLastError();
            const bool is_64 = h_process ? is_process_64_bit(h_process) : false;

            ProcessInfo info{
                .pid = entry.th32ProcessID,
                .name = std::string(process_name),
                .is_64_bit = is_64,
                .handle = h_process,
                .last_error = err
            };

            if (h_process != nullptr) {
                CloseHandle(snapshot);
                return info;
            }

            // Save first failure as fallback in case no other matching process succeeds
            if (!fallback_proc.has_value()) {
                fallback_proc = info;
            }
        }
    } while (Process32NextW(snapshot, &entry));

    CloseHandle(snapshot);
    return fallback_proc;
}

bool ProcessFinder::is_process_64_bit(void* process_handle) {
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
}

std::optional<ProcessInfo> ProcessFinder::wait_for_process(
    std::string_view process_name,
    uint32_t timeout_seconds
) {
    const auto start = std::chrono::steady_clock::now();
    int denied_retries = 0;
    constexpr int MAX_DENIED_RETRIES = 10; // Retry for up to 5 seconds if process is still spawning

    while (true) {
        auto proc = find_process(process_name);
        if (proc.has_value()) {
            if (proc->handle != nullptr) {
                return proc;
            }
            // Process exists but handle couldn't be opened yet.
            // When FFXIV is launched, parent launcher holds initialization locks briefly.
            // Retry for a few seconds before giving up.
            if (++denied_retries >= MAX_DENIED_RETRIES) {
                return proc;
            }
        } else {
            denied_retries = 0;
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
