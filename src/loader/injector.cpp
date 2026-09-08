#include "loader/injector.hpp"
#include <fstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mitigator::loader {

DllInjector::~DllInjector() {
    cleanup();
}

bool DllInjector::inject(const ProcessInfo& proc, std::span<const uint8_t> dll_bytes) {
    if (dll_bytes.empty() || proc.handle == nullptr) {
        return false;
    }

    const std::wstring temp_dll = write_temp_dll(dll_bytes, proc.pid);
    if (temp_dll.empty()) {
        return false;
    }

    m_temp_path = temp_dll;
    return inject_from_file(proc, temp_dll);
}

bool DllInjector::inject_from_file(const ProcessInfo& proc, const std::wstring& dll_path) {
#if defined(_WIN32)
    if (!proc.handle || dll_path.empty()) {
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
        return false;
    }

    // 2. Write DLL path to remote process memory
    SIZE_T bytes_written = 0;
    if (!WriteProcessMemory(h_process, p_remote_path, dll_path.c_str(), path_size_bytes, &bytes_written) ||
        bytes_written != path_size_bytes) {
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    // 3. Locate LoadLibraryW in kernel32.dll (identical VA across 64-bit processes)
    HMODULE h_kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!h_kernel32) {
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    auto pfn_load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(h_kernel32, "LoadLibraryW")
    );
    if (!pfn_load_library) {
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
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    // 5. Wait for injection to complete
    WaitForSingleObject(h_remote_thread, 10000);

    DWORD remote_exit_code = 0;
    GetExitCodeThread(h_remote_thread, &remote_exit_code);

    CloseHandle(h_remote_thread);
    VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);

    m_remote_hmodule = static_cast<uintptr_t>(remote_exit_code);
    return (m_remote_hmodule != 0);
#else
    (void)proc;
    (void)dll_path;
    return false;
#endif
}

std::wstring DllInjector::write_temp_dll(std::span<const uint8_t> dll_bytes, uint32_t pid) {
#if defined(_WIN32)
    wchar_t temp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp_dir) == 0) {
        return L"";
    }

    std::wstring dll_file_path = std::wstring(temp_dir) + L"ffxiv_mitigator_payload_" +
                                 std::to_wstring(pid) + L".dll";

    HANDLE h_file = CreateFileW(
        dll_file_path.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
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
    CloseHandle(h_file);

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
        DeleteFileW(m_temp_path.c_str());
        m_temp_path.clear();
    }
#endif
}

} // namespace mitigator::loader
