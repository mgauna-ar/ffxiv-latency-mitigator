#include "mitigator/types.hpp"
#include "mitigator/game_definitions.hpp"
#include "mitigator/config_manager.hpp"
#include "loader/process_finder.hpp"
#include "loader/injector.hpp"
#include "loader/loader_ipc.hpp"
#include "loader/ui_renderer.hpp"

#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <algorithm>
#include <optional>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <conio.h>

namespace {

std::atomic<bool> g_keep_running{true};

BOOL WINAPI ConsoleCtrlHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_CLOSE_EVENT) {
        std::cout << "\033[?2026l\033[?1049l\033[?25h" << std::flush;
        g_keep_running = false;
        return TRUE;
    }
    return FALSE;
}

void configure_fixed_console(HANDLE hOut, SHORT default_cols = 100, SHORT default_rows = 30) {
    if (!hOut || hOut == INVALID_HANDLE_VALUE) {
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    SHORT target_cols = default_cols;
    SHORT target_rows = default_rows;

    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        const SHORT cur_cols = static_cast<SHORT>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
        const SHORT cur_rows = static_cast<SHORT>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
        target_cols = (std::max)(default_cols, cur_cols);
        target_rows = (std::max)(default_rows, cur_rows);
    }

    SMALL_RECT temp_rect = {0, 0, 1, 1};
    SetConsoleWindowInfo(hOut, TRUE, &temp_rect);

    COORD buffer_size = {target_cols, target_rows};
    SetConsoleScreenBufferSize(hOut, buffer_size);

    SMALL_RECT window_rect = {0, 0, static_cast<SHORT>(target_cols - 1), static_cast<SHORT>(target_rows - 1)};
    SetConsoleWindowInfo(hOut, TRUE, &window_rect);

    HWND hwnd = GetConsoleWindow();
    if (hwnd) {
        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        style &= ~(WS_MAXIMIZEBOX | WS_THICKFRAME);
        SetWindowLong(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
}

void sync_terminal_dimensions(HANDLE hOut, mitigator::loader::UiRenderer& ui) {
    if (!hOut || hOut == INVALID_HANDLE_VALUE) return;
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        const size_t cols = static_cast<size_t>(csbi.srWindow.Right - csbi.srWindow.Left + 1);
        const size_t rows = static_cast<size_t>(csbi.srWindow.Bottom - csbi.srWindow.Top + 1);
        ui.set_terminal_dimensions(cols, rows);
    }
}

void restore_scrollable_console(HANDLE hOut) {
    std::cout << "\033[?2026l\033[?1049l\033[?25h" << std::flush;
    if (!hOut || hOut == INVALID_HANDLE_VALUE) {
        return;
    }

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    SHORT cols = 100;
    if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
        cols = (std::max)(cols, static_cast<SHORT>(csbi.srWindow.Right - csbi.srWindow.Left + 1));
    }

    COORD buffer_size = {cols, 300};
    SetConsoleScreenBufferSize(hOut, buffer_size);

    HWND hwnd = GetConsoleWindow();
    if (hwnd) {
        LONG style = GetWindowLong(hwnd, GWL_STYLE);
        style |= (WS_MAXIMIZEBOX | WS_THICKFRAME);
        SetWindowLong(hwnd, GWL_STYLE, style);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
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

constexpr int MAX_HANDSHAKE_WAIT_TICKS = 150;
constexpr auto HANDSHAKE_POLL_INTERVAL = std::chrono::milliseconds(100);
constexpr auto HOTKEY_POLL_INTERVAL = std::chrono::milliseconds(15);
constexpr auto UNHOOK_DRAIN_DELAY = std::chrono::milliseconds(300);

void print_help(const char* exe_name) {
    std::cout << "Usage: " << exe_name << " [options]\n\n"
              << "Options:\n"
              << "  --target-ping <ms>    Simulated low-ping target RTT in ms (default: 15.0)\n"
              << "  --min-lock <ms>       Hard floor animation lock in ms (default: 25.0)\n"
              << "  --dry-run             Monitor and display calculations without modifying game memory\n"
              << "  --verbose             Log all actions including non-mitigated ones\n"
              << "  --watch               Auto-recover and re-inject if game process restarts\n"
              << "  --help, -h            Show this help text\n\n"
              << "Hotkeys during execution:\n"
              << "  [Q]                   Cleanly unhook detours and exit\n"
              << "  [1]/[2]/[3]           Switch between Overview, Analytics, and Settings tabs\n"
              << "  [Tab] / Arrows        Cycle dashboard tabs\n"
              << "  [D]                   Toggle dry-run mode on/off\n"
              << "  [L]                   Toggle verbose logging on/off\n"
              << "  [P] / [Shift+P]       Decrease / Increase target ping (-/+ 5ms)\n"
              << "  [F] / [Shift+F]       Decrease / Increase min animation lock floor (-/+ 5ms)\n"
              << "  [M] / [Shift+M]       Decrease / Increase safety margin (-/+ 1ms)\n"
              << "  [S]                   Save settings to mitigator_config.json\n"
              << "  [C]                   Clear telemetry counters\n";
}

/// RAII wrapper for Win32 HANDLE ensuring zero leaks in error or recovery paths (8C)
struct ScopedHandle {
    HANDLE m_handle{nullptr};

    ScopedHandle() = default;
    explicit ScopedHandle(HANDLE h) : m_handle(h) {}
    ~ScopedHandle() { reset(); }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept : m_handle(other.m_handle) {
        other.m_handle = nullptr;
    }
    ScopedHandle& operator=(ScopedHandle&& other) noexcept {
        if (this != &other) {
            reset();
            m_handle = other.m_handle;
            other.m_handle = nullptr;
        }
        return *this;
    }

    void reset(HANDLE new_h = nullptr) {
        if (m_handle && m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
        m_handle = new_h;
    }

    [[nodiscard]] HANDLE get() const { return m_handle; }
    explicit operator bool() const { return m_handle != nullptr && m_handle != INVALID_HANDLE_VALUE; }
};

/// Interruptible sleep that immediately returns early if exit is requested via Ctrl+C or g_keep_running
bool interruptible_sleep(std::chrono::milliseconds duration) {
    constexpr auto step = std::chrono::milliseconds(20);
    auto remaining = duration;
    while (remaining > std::chrono::milliseconds(0) && g_keep_running.load()) {
        const auto sleep_time = (std::min)(step, remaining);
        std::this_thread::sleep_for(sleep_time);
        remaining -= sleep_time;
    }
    return g_keep_running.load();
}

std::optional<mitigator::loader::ProcessInfo> find_target_process(uint32_t exclude_pid) {
    auto proc = mitigator::loader::ProcessFinder::find_process();
    if (proc.has_value()) {
        if (proc->pid != 0 && proc->pid != exclude_pid) {
            if (proc->handle != nullptr) {
                if (WaitForSingleObject(static_cast<HANDLE>(proc->handle), 0) == WAIT_OBJECT_0) {
                    CloseHandle(static_cast<HANDLE>(proc->handle));
                    proc->handle = nullptr;
                    return std::nullopt;
                }
            }
            return proc;
        }
        // If process was excluded or invalid, ensure opened handle is closed immediately
        if (proc->handle != nullptr) {
            CloseHandle(static_cast<HANDLE>(proc->handle));
            proc->handle = nullptr;
        }
    }
    return std::nullopt;
}

template <typename F>
std::optional<mitigator::loader::ProcessInfo> wait_for_target_process(uint32_t exclude_pid, F&& on_tick) {
    int denied_retries = 0;
    constexpr int MAX_DENIED_RETRIES = 10;
    constexpr auto PROCESS_SCAN_INTERVAL = std::chrono::milliseconds(250);
    constexpr auto TICK_INTERVAL = std::chrono::milliseconds(15);

    auto last_scan = std::chrono::steady_clock::now() - PROCESS_SCAN_INTERVAL;

    while (g_keep_running.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_scan >= PROCESS_SCAN_INTERVAL) {
            last_scan = now;
            auto proc = find_target_process(exclude_pid);
            if (proc.has_value()) {
                if (proc->handle != nullptr) {
                    return proc;
                }
                if (++denied_retries >= MAX_DENIED_RETRIES) {
                    return proc;
                }
            } else {
                denied_retries = 0;
            }
        }

        // Process keyboard inputs and UI animations with zero lag
        on_tick();

        if (!g_keep_running.load()) {
            return std::nullopt;
        }

        std::this_thread::sleep_for(TICK_INTERVAL);
    }
    return std::nullopt;
}

inline std::optional<mitigator::loader::ProcessInfo> wait_for_target_process(uint32_t exclude_pid) {
    return wait_for_target_process(exclude_pid, []() {});
}

void process_extended_key(
    int ext_key,
    mitigator::loader::UiRenderer& ui,
    bool dry_run,
    bool verbose
) {
    switch (ext_key) {
        case 75: // Left Arrow
        case 72: // Up Arrow
        case 15: // Shift+Tab
            ui.cycle_tab(-1);
            ui.render_dashboard(dry_run, verbose);
            break;

        case 77: // Right Arrow
        case 80: // Down Arrow
            ui.cycle_tab(1);
            ui.render_dashboard(dry_run, verbose);
            break;

        default:
            break;
    }
}

void process_hotkey(
    int key,
    mitigator::loader::UiRenderer& ui,
    mitigator::loader::LoaderIpcServer* ipc_server,
    double& target_ping_ms,
    double& min_lock_ms,
    bool& dry_run,
    bool& verbose
) {
    switch (key) {
        case 'q':
        case 'Q':
            g_keep_running = false;
            break;

        case '1':
            ui.set_active_tab(0);
            ui.render_dashboard(dry_run, verbose);
            break;

        case '2':
            ui.set_active_tab(1);
            ui.render_dashboard(dry_run, verbose);
            break;

        case '3':
            ui.set_active_tab(2);
            ui.render_dashboard(dry_run, verbose);
            break;

        case '\t': // Tab key: cycle forward
            ui.cycle_tab(1);
            ui.render_dashboard(dry_run, verbose);
            break;

        case 'd':
        case 'D':
            dry_run = !dry_run;
            if (ipc_server) {
                ipc_server->set_dry_run(dry_run);
            } else {
                ui.set_session_info(0, 0, target_ping_ms, dry_run);
            }
            if (ui.is_dashboard_mode()) {
                ui.render_dashboard(dry_run, verbose);
            } else {
                ui.log_status(std::string("Dry-Run toggled: ") + (dry_run ? "ENABLED" : "DISABLED"));
                ui.render_hotkey_bar(dry_run, verbose);
            }
            break;

        case 'l':
        case 'L':
            verbose = !verbose;
            if (ipc_server) {
                ipc_server->set_verbose(verbose);
            }
            if (ui.is_dashboard_mode()) {
                ui.render_dashboard(dry_run, verbose);
            } else {
                ui.log_status(std::string("Verbose logging: ") + (verbose ? "ENABLED" : "DISABLED"));
                ui.render_hotkey_bar(dry_run, verbose);
            }
            break;

        case 'c':
        case 'C':
            ui.reset_stats();
            if (ipc_server) {
                ipc_server->reset_stats();
            }
            if (ui.is_dashboard_mode()) {
                ui.render_dashboard(dry_run, verbose);
            } else {
                ui.log_status("Session statistics cleared.");
            }
            break;

        case 's':
        case 'S':
            mitigator::ConfigManager::save_to_file(mitigator::ConfigManager::DEFAULT_CONFIG_FILENAME, ui.config());
            if (ui.is_dashboard_mode()) {
                ui.set_connection_status("Configuration saved to mitigator_config.json");
                ui.render_dashboard(dry_run, verbose);
            } else {
                ui.log_status("Saved settings to mitigator_config.json");
            }
            break;

        case 'f': {
            auto cfg = ui.config();
            cfg.min_animation_lock_ms = (std::max)(10.0, cfg.min_animation_lock_ms - 5.0);
            ui.set_config(cfg);
            min_lock_ms = cfg.min_animation_lock_ms;
            if (ipc_server) {
                ipc_server->set_min_lock(static_cast<float>(min_lock_ms));
            }
            ui.render_dashboard(dry_run, verbose);
            break;
        }
        case 'F': {
            auto cfg = ui.config();
            cfg.min_animation_lock_ms = (std::min)(100.0, cfg.min_animation_lock_ms + 5.0);
            ui.set_config(cfg);
            min_lock_ms = cfg.min_animation_lock_ms;
            if (ipc_server) {
                ipc_server->set_min_lock(static_cast<float>(min_lock_ms));
            }
            ui.render_dashboard(dry_run, verbose);
            break;
        }
        case 'p': {
            auto cfg = ui.config();
            cfg.target_ping_ms = (std::max)(5.0, cfg.target_ping_ms - 5.0);
            ui.set_config(cfg);
            target_ping_ms = cfg.target_ping_ms;
            if (ipc_server) {
                ipc_server->set_target_ping(static_cast<float>(target_ping_ms));
            } else {
                ui.set_session_info(0, 0, target_ping_ms, dry_run);
            }
            ui.render_dashboard(dry_run, verbose);
            break;
        }
        case 'P': {
            auto cfg = ui.config();
            cfg.target_ping_ms = (std::min)(100.0, cfg.target_ping_ms + 5.0);
            ui.set_config(cfg);
            target_ping_ms = cfg.target_ping_ms;
            if (ipc_server) {
                ipc_server->set_target_ping(static_cast<float>(target_ping_ms));
            } else {
                ui.set_session_info(0, 0, target_ping_ms, dry_run);
            }
            ui.render_dashboard(dry_run, verbose);
            break;
        }
        case 'm': {
            auto cfg = ui.config();
            cfg.safety_margin_ms = (std::max)(0.0, cfg.safety_margin_ms - 1.0);
            ui.set_config(cfg);
            ui.render_dashboard(dry_run, verbose);
            break;
        }
        case 'M': {
            auto cfg = ui.config();
            cfg.safety_margin_ms = (std::min)(20.0, cfg.safety_margin_ms + 1.0);
            ui.set_config(cfg);
            ui.render_dashboard(dry_run, verbose);
            break;
        }
        default:
            break;
    }
}

void poll_keyboard_inputs(
    mitigator::loader::UiRenderer& ui,
    mitigator::loader::LoaderIpcServer* ipc_server,
    double& target_ping_ms,
    double& min_lock_ms,
    bool& dry_run,
    bool& verbose
) {
    while (_kbhit() && g_keep_running.load()) {
        const int key = _getch();
        if (key == 0 || key == 0xE0) {
            const int ext_key = _getch();
            process_extended_key(ext_key, ui, dry_run, verbose);
        } else {
            process_hotkey(key, ui, ipc_server, target_ping_ms, min_lock_ms, dry_run, verbose);
        }
    }
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    auto config = mitigator::ConfigManager::load_from_file(mitigator::ConfigManager::DEFAULT_CONFIG_FILENAME);
    double target_ping_ms = config.target_ping_ms;
    double min_lock_ms = config.min_animation_lock_ms;
    bool dry_run = config.dry_run;
    bool verbose = config.verbose;
    bool watch_mode = false;

    // Parse command line arguments (CLI arguments take precedence over saved config)
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--target-ping" && i + 1 < argc) {
            target_ping_ms = std::stod(argv[++i]);
            config.target_ping_ms = target_ping_ms;
        } else if (arg == "--min-lock" && i + 1 < argc) {
            min_lock_ms = std::stod(argv[++i]);
            config.min_animation_lock_ms = min_lock_ms;
        } else if (arg == "--dry-run") {
            dry_run = true;
            config.dry_run = true;
        } else if (arg == "--verbose") {
            verbose = true;
            config.verbose = true;
        } else if (arg == "--watch") {
            watch_mode = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        }
    }

    mitigator::loader::ProcessFinder::enable_debug_privilege();
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    // Enable virtual terminal processing for ANSI color codes and UTF-8 encoding
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    if (GetConsoleMode(hOut, &dwMode)) {
        SetConsoleMode(hOut, dwMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }

    // Disable QuickEdit mode on console input to prevent mouse clicks from freezing output
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_in_mode = 0;
    bool in_mode_saved = false;
    if (hIn && hIn != INVALID_HANDLE_VALUE && GetConsoleMode(hIn, &original_in_mode)) {
        in_mode_saved = true;
        DWORD in_mode = original_in_mode;
        in_mode &= ~ENABLE_QUICK_EDIT_MODE;
        in_mode |= ENABLE_EXTENDED_FLAGS;
        SetConsoleMode(hIn, in_mode);
    }

    configure_fixed_console(hOut, 100, 30);

    mitigator::loader::UiRenderer ui;
    ui.set_config(config);
    sync_terminal_dimensions(hOut, ui);
    mitigator::loader::LoaderIpcServer ipc_server;

    // Connect UI config change callback to update IPC server and live settings
    ui.set_on_config_changed([&](const mitigator::MitigationConfig& cfg) {
        dry_run = cfg.dry_run;
        verbose = cfg.verbose;
        target_ping_ms = cfg.target_ping_ms;
        min_lock_ms = cfg.min_animation_lock_ms;
        ipc_server.set_dry_run(dry_run);
        ipc_server.set_verbose(verbose);
        ipc_server.set_target_ping(static_cast<float>(target_ping_ms));
        ipc_server.set_min_lock(static_cast<float>(min_lock_ms));
    });

    ui.set_on_reset_stats_callback([&]() {
        ipc_server.reset_stats();
    });

    // Telemetry callback
    ipc_server.set_telemetry_callback([&](const mitigator::ipc::TelemetryPayload& t) {
        ui.log_action(t, verbose);
    });

    // Status callback
    ipc_server.set_status_callback([&](const mitigator::ipc::StatusPayload& s) {
        ui.set_session_info(s.game_pid, s.hooks_installed, target_ping_ms, dry_run);
        if (s.hooks_installed < mitigator::game::definitions::MIN_REQUIRED_PRIMARY_HOOKS) {
            restore_scrollable_console(hOut);
            ui.render_header(s.game_pid, s.hooks_installed, target_ping_ms, dry_run);
            ui.log_status(
                "Game update detected! Signature scan failed (" +
                std::string(s.status_message) + ").",
                true
            );
            ui.log_status("Game memory is safe and untouched. Payload automatically self-unloaded.", false);
            ui.log_status("Update signatures in include/mitigator/game_definitions.hpp to support this patch.", false);
        } else {
            ui.set_connection_status("Connected");
            ui.set_dashboard_mode(true);
            ui.render_dashboard(dry_run, verbose);
        }
    });

    // Locate payload DLL from directory adjacent to executable
    std::filesystem::path disk_payload_path;
    wchar_t module_file[MAX_PATH];
    if (GetModuleFileNameW(nullptr, module_file, MAX_PATH) != 0) {
        const std::filesystem::path exe_dir = std::filesystem::path(module_file).parent_path();
        const auto candidate = exe_dir / "mitigator_payload.dll";
        if (std::filesystem::exists(candidate)) {
            disk_payload_path = candidate;
        }
    }

    if (disk_payload_path.empty()) {
        restore_scrollable_console(hOut);
        ui.log_status("mitigator_payload.dll was not found!", true);
        ui.log_status("Please ensure 'mitigator_payload.dll' is placed in the same folder as ffxiv-mitigator.exe.", false);
        wait_for_user_exit();
        return 1;
    }

    mitigator::loader::DllInjector injector;
    uint32_t last_pid = 0;
    int consecutive_failures = 0;
    constexpr int MAX_BACKOFF_MS = 8000;

    ui.set_dashboard_mode(true);
    std::cout << "\033[?1049h" << std::flush;
    ui.set_session_info(0, 0, target_ping_ms, dry_run);
    ui.set_connection_status("Searching for " + std::string(mitigator::game::definitions::DEFAULT_GAME_PROCESS_NAME) + "...");
    ui.render_dashboard(dry_run, verbose);

    while (g_keep_running.load()) {
        ui.set_session_info(0, 0, target_ping_ms, dry_run);
        ui.set_connection_status("Searching for " + std::string(mitigator::game::definitions::DEFAULT_GAME_PROCESS_NAME) + "...");

        auto on_search_tick = [&]() {
            sync_terminal_dimensions(hOut, ui);
            poll_keyboard_inputs(ui, nullptr, target_ping_ms, min_lock_ms, dry_run, verbose);
        };

        auto proc = wait_for_target_process(last_pid, on_search_tick);
        if (!proc.has_value() || !g_keep_running.load()) {
            break;
        }

        ScopedHandle proc_handle(static_cast<HANDLE>(proc->handle));
        proc->handle = proc_handle.get();

        if (!proc_handle) {
            ui.set_connection_status("Access Denied (Admin Privileges Required)");
            ui.render_dashboard(dry_run, verbose);
            if (!watch_mode) {
                restore_scrollable_console(hOut);
                ui.log_status(
                    "Access denied opening game process (PID: " + std::to_string(proc->pid) +
                    ", Win32 Error: " + std::to_string(proc->last_error) + ").",
                    true
                );
                ui.log_status(
                    "FFXIV is running with Administrator privileges. Please re-run ffxiv-mitigator as Administrator (or launch FFXIV via XIVLauncher without Admin).",
                    false
                );
                wait_for_user_exit();
                return 1;
            }
            interruptible_sleep(std::chrono::milliseconds(2000));
            continue;
        }

        if (!proc->is_64_bit) {
            ui.set_connection_status("Unsupported Architecture (Not 64-bit)");
            ui.render_dashboard(dry_run, verbose);
            if (!watch_mode) {
                restore_scrollable_console(hOut);
                ui.log_status(
                    std::string("Detected process is not a 64-bit executable. Only 64-bit FFXIV (") +
                    std::string(mitigator::game::definitions::DEFAULT_GAME_PROCESS_NAME) + ") is supported.",
                    true
                );
                wait_for_user_exit();
                return 1;
            }
            interruptible_sleep(std::chrono::milliseconds(2000));
            continue;
        }

        if (mitigator::loader::DllInjector::is_payload_already_loaded(*proc)) {
            ui.set_connection_status("Payload Already Injected (Restart Game Required)");
            ui.render_dashboard(dry_run, verbose);
            if (!watch_mode) {
                restore_scrollable_console(hOut);
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
                wait_for_user_exit();
                return 1;
            }
            // In watch mode, wait for this specific process to terminate before re-scanning
            while (g_keep_running.load() && proc_handle) {
                if (WaitForSingleObject(proc_handle.get(), 0) == WAIT_OBJECT_0) {
                    break;
                }
                interruptible_sleep(std::chrono::milliseconds(1000));
            }
            last_pid = proc->pid;
            continue;
        }

        // Initialize Named Pipe server for this session
        ui.set_session_info(proc->pid, 0, target_ping_ms, dry_run);
        ui.set_connection_status("Starting IPC server...");
        ui.render_dashboard(dry_run, verbose);

        if (!ipc_server.start()) {
            ui.set_connection_status("Failed to initialize Named Pipe server");
            ui.render_dashboard(dry_run, verbose);
            if (!watch_mode) {
                restore_scrollable_console(hOut);
                ui.log_status("Failed to initialize Named Pipe server", true);
                wait_for_user_exit();
                return 1;
            }
            interruptible_sleep(std::chrono::milliseconds(2000));
            continue;
        }

        ui.set_connection_status("Injecting payload DLL...");
        ui.render_dashboard(dry_run, verbose);
        const bool injected = injector.inject(*proc, disk_payload_path);

        if (!injected) {
            ui.set_connection_status("Failed to inject payload DLL");
            ui.render_dashboard(dry_run, verbose);
            ipc_server.stop();
            if (!watch_mode) {
                restore_scrollable_console(hOut);
                ui.log_status("Failed to inject payload DLL into game process.", true);
                if (!injector.last_error().empty()) {
                    ui.log_status("Reason: " + injector.last_error(), false);
                }
                wait_for_user_exit();
                return 1;
            }
            ++consecutive_failures;
            const int backoff_ms = (std::min)(1000 * (1 << (consecutive_failures - 1)), MAX_BACKOFF_MS);
            ui.set_connection_status("Retrying injection in " + std::to_string(backoff_ms / 1000) + "s...");
            ui.render_dashboard(dry_run, verbose);
            interruptible_sleep(std::chrono::milliseconds(backoff_ms));
            continue;
        }

        ui.set_connection_status("Injected - Awaiting telemetry handshake...");
        ui.render_dashboard(dry_run, verbose);

        // Wait for payload to connect to pipe and complete handshake
        int wait_ticks = 0;
        while (!ipc_server.has_received_status()
               && wait_ticks++ < MAX_HANDSHAKE_WAIT_TICKS
               && g_keep_running.load()) {
            if (proc_handle && WaitForSingleObject(proc_handle.get(), 0) == WAIT_OBJECT_0) {
                break; // Process died during handshake
            }
            if (_kbhit()) {
                const int key = _getch();
                if (key == 'q' || key == 'Q') {
                    g_keep_running = false;
                    break;
                }
            }
            std::this_thread::sleep_for(HANDSHAKE_POLL_INTERVAL);
        }

        const auto last_status = ipc_server.last_status();
        if (last_status.has_value() && last_status->hooks_installed < mitigator::game::definitions::MIN_REQUIRED_PRIMARY_HOOKS) {
            // Detour installation failed in game; status callback already displayed diagnostic
            ipc_server.stop();
            if (!watch_mode) {
                restore_scrollable_console(hOut);
                ui.render_header(proc->pid, last_status->hooks_installed, target_ping_ms, dry_run);
                ui.log_status(
                    "Game update detected! Signature scan failed (" +
                    std::string(last_status->status_message) + ").",
                    true
                );
                ui.log_status("Game memory is safe and untouched. Payload automatically self-unloaded.", false);
                ui.log_status("Update signatures in include/mitigator/game_definitions.hpp to support this patch.", false);
                wait_for_user_exit();
                return 1;
            }
            last_pid = proc->pid;
            continue;
        }

        if (!ipc_server.has_received_status()) {
            ui.set_connection_status("Handshake timed out");
            ui.render_dashboard(dry_run, verbose);
            ipc_server.stop();
            if (!watch_mode) {
                restore_scrollable_console(hOut);
                ui.log_status("Handshake timed out. Injected payload did not establish IPC telemetry.", true);
                ui.log_status("Possible causes:", true);
                ui.log_status("  1. Privilege mismatch: Ensure both game and mitigator are run with matching privileges (e.g. Run as administrator).", false);
                ui.log_status("  2. Antivirus or security software blocked remote thread execution.", false);
                ui.log_status("  3. Third-party overlay or hook conflict.", false);
                wait_for_user_exit();
                return 1;
            }
            last_pid = proc->pid;
            continue;
        }

        // Handshake succeeded: reset consecutive failures and update last PID
        consecutive_failures = 0;
        last_pid = proc->pid;

        // Set initial configuration parameters
        ipc_server.set_dry_run(dry_run);
        ipc_server.set_target_ping(static_cast<float>(target_ping_ms));
        ipc_server.set_min_lock(static_cast<float>(min_lock_ms));

        ui.set_connection_status("Connected");
        ui.set_session_info(proc->pid, last_status->hooks_installed, target_ping_ms, dry_run);
        ui.render_dashboard(dry_run, verbose);

        // Interactive hotkey input loop with decoupled dashboard refresh
        auto last_dashboard_render = std::chrono::steady_clock::now();
        constexpr auto DASHBOARD_TICK_INTERVAL = std::chrono::milliseconds(250);
        constexpr auto DASHBOARD_HEARTBEAT_INTERVAL = std::chrono::milliseconds(1000);

        while (g_keep_running.load()) {
            if (proc_handle && WaitForSingleObject(proc_handle.get(), 0) == WAIT_OBJECT_0) {
                break;
            }

            // Detect if injected payload disconnected or pipe broke while game is running
            if (!ipc_server.is_connected()) {
                ui.set_connection_status("Telemetry Disconnected (Payload Detached)");
                ui.render_dashboard(dry_run, verbose);
                break;
            }

            sync_terminal_dimensions(hOut, ui);

            const auto now = std::chrono::steady_clock::now();
            const auto time_since_render = now - last_dashboard_render;

            if (ui.is_dashboard_mode()) {
                if ((ui.needs_redraw() && time_since_render >= DASHBOARD_TICK_INTERVAL) ||
                    (time_since_render >= DASHBOARD_HEARTBEAT_INTERVAL)) {
                    ui.render_dashboard(dry_run, verbose);
                    last_dashboard_render = now;
                }
            }

            if (_kbhit()) {
                poll_keyboard_inputs(ui, &ipc_server, target_ping_ms, min_lock_ms, dry_run, verbose);
                last_dashboard_render = std::chrono::steady_clock::now();
            }
            std::this_thread::sleep_for(HOTKEY_POLL_INTERVAL);
        }

        // Clean unhooking sequence if game process is still alive
        if (proc_handle && WaitForSingleObject(proc_handle.get(), 0) != WAIT_OBJECT_0) {
            ui.set_connection_status("Detaching and restoring detours...");
            ui.render_dashboard(dry_run, verbose);
            ipc_server.request_unhook();
            std::this_thread::sleep_for(UNHOOK_DRAIN_DELAY);
        }

        ipc_server.stop();
        proc_handle.reset();

        restore_scrollable_console(hOut);
        ui.render_final_report();

        if (!watch_mode || !g_keep_running.load()) {
            break;
        }

        ui.reset_stats();
        configure_fixed_console(hOut, 100, 30);
        sync_terminal_dimensions(hOut, ui);
    }

    restore_scrollable_console(hOut);
    mitigator::ConfigManager::save_to_file(mitigator::ConfigManager::DEFAULT_CONFIG_FILENAME, ui.config());
    std::cout << "[+] Done. Clean exit completed.\n";
    if (in_mode_saved && hIn && hIn != INVALID_HANDLE_VALUE) {
        SetConsoleMode(hIn, original_in_mode);
    }
    wait_for_user_exit();
    return 0;
}
