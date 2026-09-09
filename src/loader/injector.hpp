#pragma once

#include "loader/process_finder.hpp"
#include <string>
#include <filesystem>

namespace mitigator::loader {

/**
 * @brief Injects the mitigator payload DLL into the running game process.
 */
class DllInjector {
public:
    DllInjector() = default;
    ~DllInjector() = default;

    /**
     * @brief Injects a payload DLL file from disk into target process.
     * @param proc ProcessInfo containing valid handle and pid.
     * @param dll_path Path to the payload DLL file on disk.
     * @return true if injection succeeded and payload is running.
     */
    bool inject(const ProcessInfo& proc, const std::filesystem::path& dll_path);

    /**
     * @brief Checks if a mitigator payload DLL is already loaded in the target process.
     */
    [[nodiscard]] static bool is_payload_already_loaded(const ProcessInfo& proc);

    /// Returns remote HMODULE in target process.
    [[nodiscard]] uintptr_t remote_module_handle() const { return m_remote_hmodule; }

    /// Returns diagnostic description of last error.
    [[nodiscard]] const std::string& last_error() const { return m_last_error; }

private:
    std::string m_last_error;
    uintptr_t m_remote_hmodule{0};
    [[maybe_unused]] void* m_target_process_handle{nullptr};
};

} // namespace mitigator::loader
