#pragma once

#include "loader/process_finder.hpp"
#include <string>
#include <vector>
#include <span>

namespace mitigator::loader {

/**
 * @brief Injects the embedded payload DLL into the running game process.
 */
class DllInjector {
public:
    DllInjector() = default;
    ~DllInjector();

    /**
     * @brief Injects embedded payload DLL bytes into target process.
     * @param proc ProcessInfo containing valid handle and pid.
     * @param dll_bytes Span of raw DLL file bytes.
     * @return true if injection succeeded and payload is running.
     */
    bool inject(const ProcessInfo& proc, std::span<const uint8_t> dll_bytes);

    /**
     * @brief Injects an existing DLL file on disk.
     */
    bool inject_from_file(const ProcessInfo& proc, const std::wstring& dll_path);

    /**
     * @brief Cleans up temporary files and remote references.
     */
    void cleanup();

    /**
     * @brief Checks if a mitigator payload DLL is already loaded in the target process.
     */
    [[nodiscard]] static bool is_payload_already_loaded(const ProcessInfo& proc);

    /// Returns remote HMODULE in target process.
    [[nodiscard]] uintptr_t remote_module_handle() const { return m_remote_hmodule; }

    /// Returns the temporary file path used for injection.
    [[nodiscard]] const std::wstring& temp_dll_path() const { return m_temp_path; }

private:
    std::wstring write_temp_dll(std::span<const uint8_t> dll_bytes, uint32_t pid);

    uintptr_t m_remote_hmodule{0};
    std::wstring m_temp_path;
    [[maybe_unused]] void* m_target_process_handle{nullptr};
};

} // namespace mitigator::loader
