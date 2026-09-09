#pragma once

#include "mitigator/types.hpp"
#include "mitigator/animation_lock.hpp"
#include "payload/payload_ipc.hpp"
#include <atomic>
#include <cstdint>

namespace mitigator::payload {

/**
 * @brief Manages MinHook detours in the FFXIV client process.
 *
 * Hooks:
 * 1. UseActionLocation (Client action dispatch & cast initiation)
 * 2. ReceiveActionEffect (Server action response & animation lock assignment)
 */
class HookManager {
public:
    static HookManager& instance();

    /// Scans memory patterns and installs all MinHook detours.
    bool install(AnimationLockMitigator* mitigator, PayloadIpcClient* ipc);

    /// Safely disables and uninstalls all detours.
    void uninstall();

    /// Returns true if hooks are currently active.
    [[nodiscard]] bool is_installed() const { return m_installed.load(); }

    /// Returns the number of successfully hooked target functions.
    [[nodiscard]] uint32_t active_hook_count() const { return m_hook_count.load(); }

    /// Returns diagnostic description of the last error or signature mismatch.
    [[nodiscard]] const char* last_error() const { return m_last_error; }

private:
    HookManager() = default;
    ~HookManager() { uninstall(); }

    HookManager(const HookManager&) = delete;
    HookManager& operator=(const HookManager&) = delete;

    std::atomic<bool> m_installed{false};
    std::atomic<uint32_t> m_hook_count{0};
    const char* m_last_error{"OK"};
};

} // namespace mitigator::payload
