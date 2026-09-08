#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <optional>
#include "mitigator/game_definitions.hpp"

namespace mitigator::loader {

struct ProcessInfo {
    uint32_t pid{0};
    std::string name;
    bool is_64_bit{false};
    void* handle{nullptr};
    uint32_t last_error{0};
};

/**
 * @brief Locates running game processes using Win32 Toolhelp32 snapshot.
 */
class ProcessFinder {
public:
    /// Enables SeDebugPrivilege in the current process token (requires Administrator).
    static bool enable_debug_privilege();

    /// Searches for a process with the specified image name (e.g. "ffxiv_dx11.exe").
    [[nodiscard]] static std::optional<ProcessInfo> find_process(
        std::string_view process_name = game::definitions::DEFAULT_GAME_PROCESS_NAME
    );

    /// Checks if a process handle is 64-bit architecture.
    [[nodiscard]] static bool is_process_64_bit(void* process_handle);

    /// Waits until the game process starts or timeout expires (0 = wait indefinitely).
    [[nodiscard]] static std::optional<ProcessInfo> wait_for_process(
        std::string_view process_name = game::definitions::DEFAULT_GAME_PROCESS_NAME,
        uint32_t timeout_seconds = 0
    );
};

} // namespace mitigator::loader
