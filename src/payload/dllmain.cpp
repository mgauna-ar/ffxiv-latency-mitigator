#include "payload/game_hooks.hpp"
#include "payload/payload_ipc.hpp"
#include "payload/payload_logger.hpp"
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

constexpr uint32_t IPC_CONNECT_TIMEOUT_MS = 10000;
constexpr auto LIFECYCLE_POLL_INTERVAL = std::chrono::milliseconds(50);
constexpr auto UNHOOK_DRAIN_DELAY = std::chrono::milliseconds(150);
constexpr uint16_t PAYLOAD_VERSION_MAJOR = 1;
constexpr uint16_t PAYLOAD_VERSION_MINOR = 0;

DWORD WINAPI PayloadMain(LPVOID module_handle) {
    const auto h_module = static_cast<HMODULE>(module_handle);
    mitigator::payload::log_debug("PayloadMain: thread started (PID: " + std::to_string(GetCurrentProcessId()) + ")");

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
        mitigator::payload::log_debug("PayloadMain: connecting to IPC pipe...");
        const bool ipc_ok = ipc_client.connect(IPC_CONNECT_TIMEOUT_MS);
        if (!ipc_ok) {
            mitigator::payload::log_debug("PayloadMain: IPC connect failed after timeout. Aborting and self-unloading.");
            FreeLibraryAndExitThread(h_module, 0);
            return 0;
        }
        mitigator::payload::log_debug("PayloadMain: IPC connected! Installing detours...");

        // 3. Install detours
        const bool hooks_ok = mitigator::payload::HookManager::instance().install(
            &mitigator_engine,
            &ipc_client
        );
        mitigator::payload::log_debug(
            std::string("PayloadMain: hooks result ok=") + (hooks_ok ? "true" : "false") +
            ", count=" + std::to_string(mitigator::payload::HookManager::instance().active_hook_count()) +
            ", last_error=" + mitigator::payload::HookManager::instance().last_error()
        );

        // 4. Report initial status
        mitigator::ipc::StatusPayload status{};
        status.game_pid = GetCurrentProcessId();
        status.hooks_installed = mitigator::payload::HookManager::instance().active_hook_count();
        status.version_major = PAYLOAD_VERSION_MAJOR;
        status.version_minor = PAYLOAD_VERSION_MINOR;
        const char* msg = hooks_ok ? "Detours installed successfully" : mitigator::payload::HookManager::instance().last_error();
        strncpy_s(status.status_message, sizeof(status.status_message), msg, _TRUNCATE);
        mitigator::payload::log_debug("PayloadMain: sending status packet...");
        const bool status_sent = ipc_client.send_status(status);

        // If hook installation or status send failed, abort and self-unload cleanly
        if (!hooks_ok || !status_sent) {
            mitigator::payload::log_debug("PayloadMain: initialization failed (hooks_ok=" +
                std::string(hooks_ok ? "true" : "false") + ", status_sent=" +
                std::string(status_sent ? "true" : "false") + "), self-unloading.");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            mitigator::payload::HookManager::instance().uninstall();
            ipc_client.disconnect();
            FreeLibraryAndExitThread(h_module, 0);
            return 0;
        }

        // 5. Start background worker threads (reader & writer)
        mitigator::payload::log_debug("PayloadMain: starting background worker threads.");
        ipc_client.start_worker_threads();

        // 6. Main payload lifecycle loop - exit if shutdown requested OR loader disconnects
        mitigator::payload::log_debug("PayloadMain: entering main lifecycle loop.");
        while (!g_shutdown_requested.load() && ipc_client.is_connected()) {
            std::this_thread::sleep_for(LIFECYCLE_POLL_INTERVAL);
        }

        // 6. Graceful shutdown: Cleanly unhook detours
        mitigator::payload::log_debug("PayloadMain: shutdown requested or IPC disconnected, uninstalling detours.");
        mitigator::payload::HookManager::instance().uninstall();

        // Small delay to ensure any in-flight detoured threads have safely completed
        std::this_thread::sleep_for(UNHOOK_DRAIN_DELAY);

        ipc_client.disconnect();
    }

    // 7. Unload payload DLL from game process
    mitigator::payload::log_debug("PayloadMain: clean exit, unloading DLL.");
    FreeLibraryAndExitThread(h_module, 0);
    return 0;
}
#endif

} // anonymous namespace

#if defined(_WIN32)
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH: {
            mitigator::payload::log_debug("DllMain: DLL_PROCESS_ATTACH received");
            g_dll_module = hModule;
            DisableThreadLibraryCalls(hModule);
            HANDLE h_thread = CreateThread(nullptr, 0, &PayloadMain, hModule, 0, nullptr);
            if (!h_thread) {
                mitigator::payload::log_debug("DllMain: CreateThread failed!");
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
