#include "mitigator/types.hpp"
#include "loader/process_finder.hpp"
#include "loader/injector.hpp"
#include "loader/loader_ipc.hpp"
#include "loader/ui_renderer.hpp"
#include "loader/embedded_payload.hpp"

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>

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
#endif

constexpr size_t MIN_EMBEDDED_PAYLOAD_SIZE = 100;
constexpr int MAX_HANDSHAKE_WAIT_TICKS = 50;
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
        return 1;
    }

    // Telemetry callback
    ipc_server.set_telemetry_callback([&](const mitigator::ipc::TelemetryPayload& t) {
        ui.log_action(t, verbose);
    });

    // Status callback
    ipc_server.set_status_callback([&](const mitigator::ipc::StatusPayload& s) {
        ui.render_header(s.game_pid, s.hooks_installed, target_ping_ms, dry_run);
        ui.render_hotkey_bar(dry_run, verbose);
    });

    std::cout << "[*] Searching for ffxiv_dx11.exe...\n";
    auto proc = mitigator::loader::ProcessFinder::find_process("ffxiv_dx11.exe");
    if (!proc.has_value()) {
        std::cout << "[*] Waiting for ffxiv_dx11.exe to launch (Ctrl+C to abort)...\n";
        proc = mitigator::loader::ProcessFinder::wait_for_process("ffxiv_dx11.exe", 0);
    }

    if (!proc.has_value() || !proc->handle) {
        ui.log_status("Game process not found or access denied.", true);
        ipc_server.stop();
        return 1;
    }

    if (!proc->is_64_bit) {
        ui.log_status("Detected process is not a 64-bit executable. Only 64-bit FFXIV (ffxiv_dx11.exe) is supported.", true);
        ipc_server.stop();
#if defined(_WIN32)
        if (proc->handle) CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
        return 1;
    }

    std::cout << "[+] Found game process! PID: " << proc->pid << "\n";

    // Inject payload DLL
    mitigator::loader::DllInjector injector;
    const auto embedded_dll = mitigator::loader::get_embedded_payload();

    bool injected = false;
    if (embedded_dll.size() > MIN_EMBEDDED_PAYLOAD_SIZE) {
        std::cout << "[*] Injecting embedded payload (" << embedded_dll.size() << " bytes)...\n";
        injected = injector.inject(*proc, embedded_dll);
    } else {
        // Fallback: look for mitigator_payload.dll in the current working directory
        std::cout << "[*] Searching for mitigator_payload.dll on disk...\n";
        injected = injector.inject_from_file(*proc, L"mitigator_payload.dll");
    }

    if (!injected) {
        ui.log_status("Failed to inject payload DLL into game process.", true);
        ipc_server.stop();
#if defined(_WIN32)
        if (proc->handle) CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
        return 1;
    }

    std::cout << "[+] Payload successfully injected. Awaiting IPC telemetry handshake...\n";

    // Wait for payload to connect to pipe
    int wait_ticks = 0;
    while (!ipc_server.is_connected() && wait_ticks++ < MAX_HANDSHAKE_WAIT_TICKS && g_keep_running.load()) {
        std::this_thread::sleep_for(HANDSHAKE_POLL_INTERVAL);
    }

    if (!ipc_server.is_connected()) {
        ui.log_status("Handshake timed out. Game may be closing or hooks failed.", true);
    }

    // Set initial configuration parameters
    ipc_server.set_dry_run(dry_run);
    ipc_server.set_target_ping(static_cast<float>(target_ping_ms));
    ipc_server.set_min_lock(static_cast<float>(min_lock_ms));

    // Interactive hotkey input loop
    while (g_keep_running.load()) {
#if defined(_WIN32)
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

    return 0;
}
