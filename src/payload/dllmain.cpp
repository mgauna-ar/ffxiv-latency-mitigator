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

void PayloadMain(void* module_handle) {
    const auto h_module = static_cast<HMODULE>(module_handle);

    // 1. Initialize core mitigator engine
    mitigator::MitigationConfig config{};
    config.target_ping_ms = 15.0;
    config.min_animation_lock_ms = 25.0;
    config.dry_run = false;
    config.verbose = false;

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

    // Connect to loader Named Pipe (try for up to 5 seconds)
    const bool ipc_ok = ipc_client.connect(5000);

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
        status.version_major = 1;
        status.version_minor = 0;
        const char* msg = hooks_ok ? "Detours installed successfully" : "Failed to hook all functions";
        strncpy_s(status.status_message, sizeof(status.status_message), msg, _TRUNCATE);
        ipc_client.send_status(status);
    }

    // 5. Main payload lifecycle loop
    while (!g_shutdown_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 6. Graceful shutdown: Cleanly unhook detours
    mitigator::payload::HookManager::instance().uninstall();

    // Small delay to ensure any in-flight detoured threads have safely completed
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    ipc_client.disconnect();

    // 7. Unload payload DLL from game process
    FreeLibraryAndExitThread(h_module, 0);
}
#endif

} // anonymous namespace

#if defined(_WIN32)
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            g_dll_module = hModule;
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(&PayloadMain), hModule, 0, nullptr);
            break;
        case DLL_PROCESS_DETACH:
            mitigator::payload::HookManager::instance().uninstall();
            break;
        default:
            break;
    }
    return TRUE;
}
#endif
