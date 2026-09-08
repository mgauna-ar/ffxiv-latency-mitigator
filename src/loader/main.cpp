#include "mitigator/types.hpp"
#include "mitigator/game_definitions.hpp"
#include "loader/process_finder.hpp"
#include "loader/injector.hpp"
#include "loader/loader_ipc.hpp"
#include "loader/ui_renderer.hpp"
#include "loader/embedded_payload.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <conio.h>
#endif

namespace {

std::atomic<bool> g_keep_running{true};

#if defined(_WIN32)
BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_keep_running = false;
        return TRUE;
    }
    return FALSE;
}

void wait_for_user_exit() {
    HWND console_wnd = GetConsoleWindow();
    if (console_wnd) {
        DWORD proc_id = 0;
        GetWindowThreadProcessId(console_wnd, &proc_id);
        if (proc_id == GetCurrentProcessId()) {
            std::cout << "\nPress any key to exit...\n";
            _getch();
        }
    }
}

void print_payload_log(mitigator::loader::UiRenderer& ui) {
    wchar_t temp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp_dir) != 0) {
        const std::wstring log_path = std::wstring(temp_dir) + L"ffxiv_mitigator_payload.log";
        std::ifstream f(log_path);
        if (f.is_open()) {
            ui.log_status("--- Injected Payload Diagnostic Log ---", false);
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty()) {
                    ui.log_status("  " + line, false);
                }
            }
            ui.log_status("---------------------------------------", false);
        } else {
            ui.log_status("No payload diagnostic log was found in %TEMP%.", false);
            ui.log_status("This suggests DllMain did not execute (remote thread may have been blocked).", false);
        }
    }
}
#else
void wait_for_user_exit() {}
void print_payload_log(mitigator::loader::UiRenderer&) {}
#endif

constexpr size_t MIN_EMBEDDED_PAYLOAD_SIZE = 100;
constexpr int MAX_HANDSHAKE_WAIT_TICKS = 150;
constexpr auto HANDSHAKE_POLL_INTERVAL = std::chrono::milliseconds(100);
constexpr auto HOTKEY_POLL_INTERVAL = std::chrono::milliseconds(50);
constexpr auto UNHOOK_DRAIN_DELAY = std::chrono::milliseconds(300);

void print_help(const char* exe_name) {
    std::cout << "Usage: " << exe_name << " [options]\n\n"
              << "Options:\n"
              << "  --target-ping <ms>    Simulated low-ping target RTT in ms (default: 15.0)\n"
              << "  --min-lock <ms>       Hard floor animation lock in ms (default: 25.0)\n"
              << "  --dry-run             Monitor and display calculations without modifying game memory\n"
              << "  --verbose             Log all actions including non-mitigated ones\n"
              << "  --help, -h            Show this help text\n\n"
              << "Hotkeys during execution:\n"
              << "  [Q]                   Cleanly unhook detours and exit\n"
              << "  [D]                   Toggle dry-run mode on/off\n"
              << "  [L]                   Toggle verbose logging on/off\n"
              << "  [C]                   Clear telemetry counters\n";
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    double target_ping_ms = mitigator::constants::DEFAULT_TARGET_PING_MS;
    double min_lock_ms = mitigator::constants::DEFAULT_MIN_ANIMATION_LOCK_MS;
    bool dry_run = false;
    bool verbose = false;

    // Parse command line arguments
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--target-ping" && i + 1 < argc) {
            target_ping_ms = std::stod(argv[++i]);
        } else if (arg == "--min-lock" && i + 1 < argc) {
            min_lock_ms = std::stod(argv[++i]);
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        }
    }

#if defined(_WIN32)
    mitigator::loader::ProcessFinder::enable_debug_privilege();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Enable virtual terminal processing for ANSI color codes
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode)) {
        SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#endif

    mitigator::loader::UiRenderer ui;
    mitigator::loader::LoaderIpcServer ipc_server;

    std::cout << "[*] Starting IPC server...\n";
    if (!ipc_server.start()) {
        ui.log_status("Failed to initialize Named Pipe server", true);
        wait_for_user_exit();
        return 1;
    }

    // Telemetry callback
    ipc_server.set_telemetry_callback([&](const mitigator::ipc::TelemetryPayload& t) {
        ui.log_action(t, verbose);
    });

    // Status callback
    ipc_server.set_status_callback([&](const mitigator::ipc::StatusPayload& s) {
        ui.render_header(s.game_pid, s.hooks_installed, target_ping_ms, dry_run);
        if (s.hooks_installed < mitigator::game::definitions::MIN_REQUIRED_PRIMARY_HOOKS) {
            ui.log_status(
                "Game update detected! Signature scan failed (" +
                std::string(s.status_message) + ").",
                true
            );
            ui.log_status("Game memory is safe and untouched. Payload automatically self-unloaded.", false);
            ui.log_status("Update signatures in include/mitigator/game_definitions.hpp to support this patch.", false);
        } else {
            ui.render_hotkey_bar(dry_run, verbose);
        }
    });

    std::cout << "[*] Searching for " << mitigator::game::definitions::DEFAULT_GAME_PROCESS_NAME << "...\n";
    auto proc = mitigator::loader::ProcessFinder::find_process();
    if (!proc.has_value()) {
        std::cout << "[*] Waiting for " << mitigator::game::definitions::DEFAULT_GAME_PROCESS_NAME << " to launch (Ctrl+C to abort)...\n";
        proc = mitigator::loader::ProcessFinder::wait_for_process();
    }

    if (!proc.has_value()) {
        ui.log_status("Game process not found.", true);
        ipc_server.stop();
        wait_for_user_exit();
        return 1;
    }

    if (!proc->handle) {
        ui.log_status(
            "Access denied opening game process (PID: " + std::to_string(proc->pid) +
            ", Win32 Error: " + std::to_string(proc->last_error) + ").",
            true
        );
        ui.log_status(
            "FFXIV is running with Administrator privileges. Please re-run ffxiv-mitigator as Administrator (or launch FFXIV via XIVLauncher without Admin).",
            false
        );
        ipc_server.stop();
        wait_for_user_exit();
        return 1;
    }

    if (!proc->is_64_bit) {
        ui.log_status(
            std::string("Detected process is not a 64-bit executable. Only 64-bit FFXIV (") +
            std::string(mitigator::game::definitions::DEFAULT_GAME_PROCESS_NAME) + ") is supported.",
            true
        );
        ipc_server.stop();
#if defined(_WIN32)
        if (proc->handle) CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
        wait_for_user_exit();
        return 1;
    }

    std::cout << "[+] Found game process! PID: " << proc->pid << "\n";

    if (mitigator::loader::DllInjector::is_payload_already_loaded(*proc)) {
        ui.log_status(
            "An existing mitigator payload DLL is ALREADY loaded in game process (PID " +
            std::to_string(proc->pid) + ").",
            true
        );
        ui.log_status(
            "Windows cannot reload updated code into an already-injected game process.",
            true
        );
        ui.log_status(
            "Please completely CLOSE and REOPEN Final Fantasy XIV, then run ffxiv-mitigator again.",
            true
        );
        ipc_server.stop();
#if defined(_WIN32)
        if (proc->handle) CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
        wait_for_user_exit();
        return 1;
    }

#if defined(_WIN32)
    // Clean up any stale diagnostic log from a previous session
    wchar_t temp_dir[MAX_PATH];
    if (GetTempPathW(MAX_PATH, temp_dir) != 0) {
        const std::wstring log_path = std::wstring(temp_dir) + L"ffxiv_mitigator_payload.log";
        DeleteFileW(log_path.c_str());
    }
#endif

    // Inject payload DLL
    mitigator::loader::DllInjector injector;
    const auto embedded_dll = mitigator::loader::get_embedded_payload();

    // Check if mitigator_payload.dll is present next to the executable
    std::filesystem::path disk_payload_path;
#if defined(_WIN32)
    wchar_t module_file[MAX_PATH];
    if (GetModuleFileNameW(nullptr, module_file, MAX_PATH) != 0) {
        const std::filesystem::path exe_dir = std::filesystem::path(module_file).parent_path();
        const auto candidate = exe_dir / "mitigator_payload.dll";
        if (std::filesystem::exists(candidate)) {
            disk_payload_path = candidate;
        }
    }
#endif
    if (disk_payload_path.empty() && std::filesystem::exists("mitigator_payload.dll")) {
        disk_payload_path = std::filesystem::absolute("mitigator_payload.dll");
    }

    bool injected = false;
    if (!disk_payload_path.empty()) {
        std::cout << "[*] Injecting payload from disk: " << disk_payload_path.string() << "...\n";
        injected = injector.inject_from_file(*proc, disk_payload_path.wstring());
    } else if (embedded_dll.size() > MIN_EMBEDDED_PAYLOAD_SIZE) {
        std::cout << "[*] Injecting embedded payload (" << embedded_dll.size() << " bytes)...\n";
        injected = injector.inject(*proc, embedded_dll);
    } else {
        ui.log_status("No payload DLL found (neither embedded nor on disk).", true);
    }

    if (!injected) {
        ui.log_status("Failed to inject payload DLL into game process.", true);
        ipc_server.stop();
#if defined(_WIN32)
        if (proc->handle) CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
        wait_for_user_exit();
        return 1;
    }

    std::cout << "[+] Payload successfully injected. Awaiting IPC telemetry handshake...\n";

    // Wait for payload to connect to pipe and complete handshake
    int wait_ticks = 0;
    while (!ipc_server.has_received_status()
           && wait_ticks++ < MAX_HANDSHAKE_WAIT_TICKS
           && g_keep_running.load()) {
        std::this_thread::sleep_for(HANDSHAKE_POLL_INTERVAL);
    }

    const auto last_status = ipc_server.last_status();
    if (last_status.has_value() && last_status->hooks_installed < mitigator::game::definitions::MIN_REQUIRED_PRIMARY_HOOKS) {
        // Detour installation failed in game; status callback already displayed diagnostic
        print_payload_log(ui);
        ipc_server.stop();
#if defined(_WIN32)
        if (proc->handle) CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
        wait_for_user_exit();
        return 1;
    }

    if (!ipc_server.has_received_status()) {
        ui.log_status("Handshake timed out. Injected payload did not establish IPC telemetry.", true);
        print_payload_log(ui);
        ui.log_status("Possible causes:", true);
        ui.log_status("  1. Privilege mismatch: Ensure both game and mitigator are run with matching privileges (e.g. Run as administrator).", false);
        ui.log_status("  2. Antivirus or security software blocked remote thread execution.", false);
        ui.log_status("  3. Third-party overlay or hook conflict.", false);
        ipc_server.stop();
#if defined(_WIN32)
        if (proc->handle) CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
        wait_for_user_exit();
        return 1;
    }

    // Set initial configuration parameters
    ipc_server.set_dry_run(dry_run);
    ipc_server.set_target_ping(static_cast<float>(target_ping_ms));
    ipc_server.set_min_lock(static_cast<float>(min_lock_ms));

    // Interactive hotkey input loop
    while (g_keep_running.load()) {
#if defined(_WIN32)
        if (proc->handle && WaitForSingleObject(static_cast<HANDLE>(proc->handle), 0) == WAIT_OBJECT_0) {
            std::cout << "\n[!] Game process terminated unexpectedly.\n";
            g_keep_running = false;
            break;
        }

        if (_kbhit()) {
            const int key = _getch();
            switch (key) {
                case 'q':
                case 'Q':
                    std::cout << "\n[!] 'Q' pressed. Sending clean unhook command to game...\n";
                    g_keep_running = false;
                    break;
                case 'd':
                case 'D':
                    dry_run = !dry_run;
                    ipc_server.set_dry_run(dry_run);
                    ui.log_status(std::string("Dry-Run toggled: ") + (dry_run ? "ENABLED" : "DISABLED"));
                    ui.render_hotkey_bar(dry_run, verbose);
                    break;
                case 'l':
                case 'L':
                    verbose = !verbose;
                    ipc_server.set_verbose(verbose);
                    ui.log_status(std::string("Verbose logging: ") + (verbose ? "ENABLED" : "DISABLED"));
                    ui.render_hotkey_bar(dry_run, verbose);
                    break;
                case 'c':
                case 'C':
                    ui.reset_stats();
                    ipc_server.reset_stats();
                    ui.log_status("Session statistics cleared.");
                    break;
                case 's':
                case 'S':
                    ui.render_stats_summary();
                    break;
                default:
                    break;
            }
        }
#endif
        std::this_thread::sleep_for(HOTKEY_POLL_INTERVAL);
    }

    // Clean unhooking sequence
    std::cout << "[*] Detaching from game process and restoring detours...\n";
    ipc_server.request_unhook();

    // Allow remote thread to unhook and call FreeLibraryAndExitThread
    std::this_thread::sleep_for(UNHOOK_DRAIN_DELAY);

    ipc_server.stop();
    injector.cleanup();

#if defined(_WIN32)
    if (proc->handle) {
        CloseHandle(static_cast<HANDLE>(proc->handle));
    }
#endif

    ui.render_stats_summary();
    std::cout << "[+] Done. Clean exit completed.\n";

    wait_for_user_exit();
    return 0;
}
