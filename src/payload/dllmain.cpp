#include "payload/game_hooks.hpp"
#include "payload/payload_ipc.hpp"
#include "mitigator/animation_lock.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <thread>

namespace {

#if defined(_WIN32)
HMODULE g_dll_module = nullptr;
std::atomic<bool> g_shutdown_requested{false};

constexpr uint32_t IPC_CONNECT_TIMEOUT_MS = 5000;
constexpr auto LIFECYCLE_POLL_INTERVAL = std::chrono::milliseconds(50);
constexpr auto UNHOOK_DRAIN_DELAY = std::chrono::milliseconds(150);
constexpr uint16_t PAYLOAD_VERSION_MAJOR = 1;
constexpr uint16_t PAYLOAD_VERSION_MINOR = 0;

DWORD WINAPI PayloadMain(LPVOID module_handle) {
    const auto h_module = static_cast<HMODULE>(module_handle);

    {
        // 1. Initialize core mitigator engine with default configuration
        mitigator::MitigationConfig config{};
        mitigator::AnimationLockMitigator mitigator_engine(config);

        // 2. Initialize IPC client
        mitigator::payload::PayloadIpcClient ipc_client;
        ipc_client.set_command_handler([&](const mitigator::ipc::CommandPayload& cmd) {
            switch (static_cast<mitigator::ipc::CommandType>(cmd.command_type)) {
                case mitigator::ipc::CommandType::UnhookAndExit:
                    g_shutdown_requested = true;
                    break;
                case mitigator::ipc::CommandType::SetDryRun:
                    mitigator_engine.set_dry_run(cmd.param_uint != 0);
                    break;
                case mitigator::ipc::CommandType::SetTargetPing:
                    mitigator_engine.set_target_ping_ms(static_cast<double>(cmd.param_float));
                    break;
                case mitigator::ipc::CommandType::SetMinAnimationLock:
                    mitigator_engine.set_min_animation_lock_ms(static_cast<double>(cmd.param_float));
                    break;
                case mitigator::ipc::CommandType::ResetStats:
                    mitigator_engine.reset();
                    break;
                default:
                    break;
            }
        });

        // Connect to loader Named Pipe
        const bool ipc_ok = ipc_client.connect(IPC_CONNECT_TIMEOUT_MS);

        // 3. Install detours
        const bool hooks_ok = mitigator::payload::HookManager::instance().install(
            &mitigator_engine,
            &ipc_client
        );

        // 4. Report initial status
        if (ipc_ok) {
            mitigator::ipc::StatusPayload status{};
            status.game_pid = GetCurrentProcessId();
            status.hooks_installed = mitigator::payload::HookManager::instance().active_hook_count();
            status.version_major = PAYLOAD_VERSION_MAJOR;
            status.version_minor = PAYLOAD_VERSION_MINOR;
            const char* msg = hooks_ok ? "Detours installed successfully" : "Failed to hook all functions";
            strncpy_s(status.status_message, sizeof(status.status_message), msg, _TRUNCATE);
            ipc_client.send_status(status);
        }

        // 5. Main payload lifecycle loop
        while (!g_shutdown_requested.load()) {
            std::this_thread::sleep_for(LIFECYCLE_POLL_INTERVAL);
        }

        // 6. Graceful shutdown: Cleanly unhook detours
        mitigator::payload::HookManager::instance().uninstall();

        // Small delay to ensure any in-flight detoured threads have safely completed
        std::this_thread::sleep_for(UNHOOK_DRAIN_DELAY);

        ipc_client.disconnect();
    }

    // 7. Unload payload DLL from game process
    FreeLibraryAndExitThread(h_module, 0);
    return 0;
}
#endif

} // anonymous namespace

#if defined(_WIN32)
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH: {
            g_dll_module = hModule;
            DisableThreadLibraryCalls(hModule);
            HANDLE h_thread = CreateThread(nullptr, 0, &PayloadMain, hModule, 0, nullptr);
            if (!h_thread) {
                return FALSE;
            }
            CloseHandle(h_thread);
            break;
        }
        case DLL_PROCESS_DETACH:
            // Check lpReserved == nullptr before calling uninstall to prevent
            // OS Loader-Lock deadlocks on game process exit.
            if (lpReserved == nullptr) {
                mitigator::payload::HookManager::instance().uninstall();
            }
            break;
        default:
            break;
    }
    return TRUE;
}
#endif
