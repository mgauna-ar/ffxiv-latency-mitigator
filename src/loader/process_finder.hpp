#pragma once

#include <cstdint>
#include <string>
#include <optional>

namespace mitigator::loader {

struct ProcessInfo {
    uint32_t pid{0};
    std::string name;
    bool is_64_bit{false};
    void* handle{nullptr};
};

/**
 * @brief Locates running game processes using Win32 Toolhelp32 snapshot.
 */
class ProcessFinder {
public:
    /// Searches for a process with the specified image name (e.g. "ffxiv_dx11.exe").
    [[nodiscard]] static std::optional<ProcessInfo> find_process(const std::string& process_name = "ffxiv_dx11.exe");

    /// Checks if a process handle is 64-bit architecture.
    [[nodiscard]] static bool is_process_64_bit(void* process_handle);

    /// Waits until the game process starts or timeout expires (0 = wait indefinitely).
    [[nodiscard]] static std::optional<ProcessInfo> wait_for_process(
        const std::string& process_name = "ffxiv_dx11.exe",
        uint32_t timeout_seconds = 0
    );
};

} // namespace mitigator::loader
