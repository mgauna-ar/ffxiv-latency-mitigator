#include "loader/ui_renderer.hpp"
#include "mitigator/types.hpp"
#include "mitigator/game_definitions.hpp"
#include <iostream>
#include <iomanip>

namespace mitigator::loader {

// ANSI color codes
namespace color {
    constexpr const char* RESET   = "\033[0m";
    constexpr const char* BOLD    = "\033[1m";
    constexpr const char* RED     = "\033[31m";
    constexpr const char* GREEN   = "\033[32m";
    constexpr const char* YELLOW  = "\033[33m";
    constexpr const char* BLUE    = "\033[34m";
    constexpr const char* CYAN    = "\033[36m";
    constexpr const char* GRAY    = "\033[90m";
}

UiRenderer::UiRenderer() = default;

void UiRenderer::render_header(uint32_t pid, uint32_t hook_count, double target_ping_ms, bool dry_run) {
    std::lock_guard<std::mutex> lock(m_render_mutex);

    std::cout << "\n"
              << color::CYAN << color::BOLD
              << "===================================================================\n"
              << "          FFXIV STANDALONE LATENCY MITIGATOR (C++20)\n"
              << "          Zero-Dependency In-Memory Detour Architecture\n"
              << "===================================================================\n"
              << color::RESET;

    std::cout << color::BOLD << "Target Process: " << color::GREEN << game::definitions::DEFAULT_GAME_PROCESS_NAME
              << color::RESET << " (PID: " << pid << ", Game: " << color::CYAN << game::definitions::SUPPORTED_GAME_VERSION << color::RESET << ")\n";
    std::cout << color::BOLD << "Active Detours: " << color::GREEN << hook_count << "/" << game::definitions::TOTAL_AVAILABLE_HOOKS << " hooks active\n"
              << color::RESET;
    std::cout << color::BOLD << "Target Ping:    " << color::YELLOW << std::fixed << std::setprecision(1) << target_ping_ms << " ms\n"
              << color::RESET;
    std::cout << color::BOLD << "Operating Mode: "
              << (dry_run ? std::string(color::YELLOW) + "DRY-RUN (Monitoring Only)" : std::string(color::GREEN) + "ACTIVE (Live Animation Lock Mitigation)")
              << color::RESET << "\n";
    std::cout << color::CYAN
              << "-------------------------------------------------------------------\n"
              << color::RESET;
}

void UiRenderer::log_action(const ipc::TelemetryPayload& t, bool verbose) {
    std::lock_guard<std::mutex> lock(m_render_mutex);

    ++m_total_actions;
    if (t.applied) {
        ++m_actions_mitigated;
        m_cumulative_time_saved_ms += t.delay_reduced_ms;
    }
    m_last_smoothed_rtt = t.smoothed_rtt_ms;
    m_last_jitter = t.jitter_ms;

    if (!verbose && t.delay_reduced_ms <= 0.0f) {
        return; // In non-verbose mode, skip actions that required no mitigation
    }

    std::cout << color::GRAY << "[#" << std::setw(4) << std::setfill('0') << m_total_actions << std::setfill(' ') << "] " << color::RESET
              << color::BOLD << "Action: " << color::CYAN << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << t.action_id << std::setfill(' ') << std::dec << color::RESET
              << " | "
              << "Orig: " << std::fixed << std::setprecision(1) << t.original_lock_ms << "ms -> "
              << color::GREEN << t.adjusted_lock_ms << "ms" << color::RESET
              << " | Saved: " << color::YELLOW << color::BOLD << "+" << t.delay_reduced_ms << "ms" << color::RESET
              << " | RTT: " << t.measured_rtt_ms << "ms (smooth: " << t.smoothed_rtt_ms << "ms)";

    if (t.clamped_floor) {
        std::cout << " " << color::YELLOW << "[Floor Clamp]" << color::RESET;
    }
    if (t.spike_filtered) {
        std::cout << " " << color::YELLOW << "[Spike Filtered]" << color::RESET;
    }
    if (t.cold_start_guard) {
        std::cout << " " << color::YELLOW << "[Cold Start]" << color::RESET;
    }
    if (t.dry_run) {
        std::cout << " " << color::BLUE << "[Dry Run]" << color::RESET;
    }
    if (t.cast_active) {
        std::cout << " " << color::GRAY << "[Cast]" << color::RESET;
    }

    std::cout << "\n";
}

void UiRenderer::log_status(const std::string& message, bool is_error) {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    if (is_error) {
        std::cout << color::RED << color::BOLD << "[ERROR] " << message << color::RESET << "\n";
    } else {
        std::cout << color::GREEN << "[INFO] " << message << color::RESET << "\n";
    }
}

void UiRenderer::render_stats_summary() {
    std::lock_guard<std::mutex> lock(m_render_mutex);

    const double avg_reduction = m_actions_mitigated > 0 ?
        (m_cumulative_time_saved_ms / static_cast<double>(m_actions_mitigated)) : 0.0;

    const char* quality_str = "[INITIALIZING]";
    const char* quality_color = color::GRAY;
    if (m_total_actions > 0 && m_last_smoothed_rtt > 0.0f) {
        if (m_last_smoothed_rtt <= 80.0f && m_last_jitter <= 5.0f) {
            quality_str = "[EXCELLENT]";
            quality_color = color::GREEN;
        } else if (m_last_smoothed_rtt <= 150.0f && m_last_jitter <= 15.0f) {
            quality_str = "[GOOD]";
            quality_color = color::CYAN;
        } else if (m_last_smoothed_rtt <= 220.0f && m_last_jitter <= 30.0f) {
            quality_str = "[FAIR]";
            quality_color = color::YELLOW;
        } else {
            quality_str = "[POOR]";
            quality_color = color::RED;
        }
    }

    std::cout << color::CYAN << "--- Telemetry Summary ---------------------------------------------\n" << color::RESET;
    std::cout << "Mitigated: " << color::GREEN << m_actions_mitigated << "/" << m_total_actions << color::RESET
              << " | Total Saved: " << color::YELLOW << color::BOLD << std::fixed << std::setprecision(2) << (m_cumulative_time_saved_ms / constants::MS_PER_SECOND) << "s" << color::RESET
              << " | Avg/Action: " << color::CYAN << std::fixed << std::setprecision(1) << avg_reduction << "ms" << color::RESET
              << " | Ping: " << m_last_smoothed_rtt << "ms (jitter: " << m_last_jitter << "ms)"
              << " " << quality_color << quality_str << color::RESET << "\n";
}

void UiRenderer::render_hotkey_bar(bool dry_run, bool verbose) {
    std::lock_guard<std::mutex> lock(m_render_mutex);

    std::cout << color::GRAY << "\nControls: "
              << color::BOLD << "[Q]" << color::RESET << color::GRAY << " Exit & Unhook | "
              << color::BOLD << "[D]" << color::RESET << color::GRAY << " Dry-Run (" << (dry_run ? "ON" : "OFF") << ") | "
              << color::BOLD << "[L]" << color::RESET << color::GRAY << " Verbose (" << (verbose ? "ON" : "OFF") << ") | "
              << color::BOLD << "[C]" << color::RESET << color::GRAY << " Clear Stats\n"
              << color::RESET;
}

void UiRenderer::reset_stats() {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    m_total_actions = 0;
    m_actions_mitigated = 0;
    m_cumulative_time_saved_ms = 0.0;
    m_last_smoothed_rtt = 0.0f;
    m_last_jitter = 0.0f;
}

} // namespace mitigator::loader
