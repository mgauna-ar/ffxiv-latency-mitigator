#include "loader/injector.hpp"
#include <fstream>
#include <chrono>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace mitigator::loader {

namespace {
#if defined(_WIN32)
    // Timeout for LoadLibraryW execution in the target process
    constexpr DWORD INJECTION_THREAD_TIMEOUT_MS = 10000;
    // Number of retry attempts when deleting temp payload DLL on exit
    constexpr int MAX_TEMP_CLEANUP_RETRIES = 5;
    // Delay between file deletion attempts in milliseconds
    constexpr DWORD CLEANUP_RETRY_INTERVAL_MS = 50;
#endif
}

DllInjector::~DllInjector() {
    cleanup();
}

bool DllInjector::inject(const ProcessInfo& proc, std::span<const uint8_t> dll_bytes) {
    if (dll_bytes.empty() || proc.handle == nullptr) {
        m_last_error = "Embedded payload is empty or process handle is invalid";
        return false;
    }

    const std::wstring temp_dll = write_temp_dll(dll_bytes, proc.pid);
    if (temp_dll.empty()) {
        m_last_error = "Failed to write embedded payload DLL to %TEMP%";
        return false;
    }

    m_temp_path = temp_dll;
    return inject_from_file(proc, temp_dll);
}

bool DllInjector::inject_from_file(const ProcessInfo& proc, const std::wstring& dll_path) {
#if defined(_WIN32)
    if (!proc.handle || dll_path.empty()) {
        m_last_error = "Invalid process handle or empty DLL path.";
        return false;
    }

    m_target_process_handle = proc.handle;
    const auto h_process = static_cast<HANDLE>(proc.handle);

    const size_t path_size_bytes = (dll_path.length() + 1) * sizeof(wchar_t);

    // 1. Allocate remote memory in game process for DLL path
    LPVOID p_remote_path = VirtualAllocEx(
        h_process,
        nullptr,
        path_size_bytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );
    if (!p_remote_path) {
        m_last_error = "VirtualAllocEx failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        return false;
    }

    // 2. Write DLL path to remote process memory
    SIZE_T bytes_written = 0;
    if (!WriteProcessMemory(h_process, p_remote_path, dll_path.c_str(), path_size_bytes, &bytes_written) ||
        bytes_written != path_size_bytes) {
        m_last_error = "WriteProcessMemory failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    // 3. Locate LoadLibraryW in kernel32.dll (identical VA across 64-bit processes)
    HMODULE h_kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!h_kernel32) {
        m_last_error = "GetModuleHandleW(kernel32.dll) failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    auto pfn_load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(h_kernel32, "LoadLibraryW")
    );
    if (!pfn_load_library) {
        m_last_error = "GetProcAddress(LoadLibraryW) failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    // 4. Create remote thread calling LoadLibraryW(p_remote_path)
    DWORD thread_id = 0;
    HANDLE h_remote_thread = CreateRemoteThread(
        h_process,
        nullptr,
        0,
        pfn_load_library,
        p_remote_path,
        0,
        &thread_id
    );

    if (!h_remote_thread) {
        m_last_error = "CreateRemoteThread failed (Win32 Error: " + std::to_string(GetLastError()) + "). Possible antivirus or elevation mismatch.";
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    // 5. Wait for injection to complete
    const DWORD wait_res = WaitForSingleObject(h_remote_thread, INJECTION_THREAD_TIMEOUT_MS);
    if (wait_res != WAIT_OBJECT_0) {
        m_last_error = "Remote thread execution timed out or failed (wait result: " + std::to_string(wait_res) + ")";
        CloseHandle(h_remote_thread);
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    DWORD remote_exit_code = 0;
    if (!GetExitCodeThread(h_remote_thread, &remote_exit_code)) {
        m_last_error = "GetExitCodeThread failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        CloseHandle(h_remote_thread);
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    if (remote_exit_code == 0 || remote_exit_code == STILL_ACTIVE) {
        m_last_error = "LoadLibraryW failed in game process (remote exit code: 0). Target path: " +
                       std::string(dll_path.begin(), dll_path.end());
        CloseHandle(h_remote_thread);
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    CloseHandle(h_remote_thread);
    VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);

    m_remote_hmodule = static_cast<uintptr_t>(remote_exit_code);
    m_last_error = "OK";
    return true;
#else
    (void)proc;
    (void)dll_path;
    m_last_error = "Platform not supported";
    return false;
#endif
}

std::wstring DllInjector::write_temp_dll(std::span<const uint8_t> dll_bytes, uint32_t pid) {
#if defined(_WIN32)
    wchar_t temp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp_dir) == 0) {
        return L"";
    }

    const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();

    std::wstring dll_file_path = std::wstring(temp_dir) + L"ffxiv_mitigator_payload_" +
                                 std::to_wstring(pid) + L"_" + std::to_wstring(now_ms) + L".dll";

    // Initialize an open NULL DACL so the game process can map and read the DLL
    // regardless of whether game and loader run under different integrity levels.
    SECURITY_DESCRIPTOR sd{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    HANDLE h_file = CreateFileW(
        dll_file_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        &sa,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (h_file == INVALID_HANDLE_VALUE) {
        return L"";
    }

    DWORD written = 0;
    const BOOL ok = WriteFile(
        h_file,
        dll_bytes.data(),
        static_cast<DWORD>(dll_bytes.size()),
        &written,
        nullptr
    );

    if (ok) {
        FlushFileBuffers(h_file);
    }
    CloseHandle(h_file);

    // Brief delay to ensure filesystem cache and filter drivers release any inspection handles
    Sleep(20);

    return (ok && written == dll_bytes.size()) ? dll_file_path : L"";
#else
    (void)dll_bytes;
    (void)pid;
    return L"";
#endif
}

void DllInjector::cleanup() {
#if defined(_WIN32)
    if (!m_temp_path.empty()) {
        for (int i = 0; i < MAX_TEMP_CLEANUP_RETRIES; ++i) {
            if (DeleteFileW(m_temp_path.c_str())) {
                break;
            }
            Sleep(CLEANUP_RETRY_INTERVAL_MS);
        }
        m_temp_path.clear();
    }
#endif
}

bool DllInjector::is_payload_already_loaded(const ProcessInfo& proc) {
#if defined(_WIN32)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, proc.pid);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(MODULEENTRY32W);
    bool found = false;

    if (Module32FirstW(snap, &me)) {
        do {
            std::wstring mod = me.szModule;
            for (auto& c : mod) {
                c = static_cast<wchar_t>(towlower(c));
            }
            if (mod.find(L"mitigator_payload") != std::wstring::npos) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }

    CloseHandle(snap);
    return found;
#else
    (void)proc;
    return false;
#endif
}

} // namespace mitigator::loader
