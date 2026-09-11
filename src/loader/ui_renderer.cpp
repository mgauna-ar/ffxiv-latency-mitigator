#include "loader/ui_renderer.hpp"
#include "mitigator/types.hpp"
#include "mitigator/game_definitions.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cmath>
#include <ctime>
#include <cstdio>

namespace mitigator::loader {

// ANSI color codes
namespace color {
    constexpr const char* RESET       = "\033[0m";
    constexpr const char* BOLD        = "\033[1m";

    // Modern Cyber-Sleek TrueColor Palette
    constexpr const char* BORDER      = "\033[38;2;75;85;105m";   // Slate steel borders
    constexpr const char* TITLE       = "\033[38;2;125;195;255m"; // Ice blue bold title
    constexpr const char* TARGET      = "\033[38;2;100;220;200m"; // Bright teal target
    constexpr const char* MINT        = "\033[38;2;85;225;145m";  // Mint green for mitigations & savings
    constexpr const char* AMBER       = "\033[38;2;245;185;75m";  // Warm amber for warnings & standby
    constexpr const char* CORAL       = "\033[38;2;250;105;125m"; // Soft coral red for errors & spikes
    constexpr const char* PURPLE      = "\033[38;2;180;140;255m"; // Lavender for dry-run
    constexpr const char* MUTED       = "\033[38;2;120;130;145m"; // Graphite gray for timestamps & dividers
    constexpr const char* TEXT        = "\033[38;2;225;230;240m"; // Crisp white
    constexpr const char* ACCENT      = "\033[38;2;70;160;255m";  // Bright cyan accent

    // Legacy ANSI fallbacks
    constexpr const char* RED         = "\033[38;2;250;105;125m";
    constexpr const char* GREEN       = "\033[38;2;85;225;145m";
    constexpr const char* YELLOW      = "\033[38;2;245;185;75m";
    constexpr const char* BLUE        = "\033[38;2;70;160;255m";
    constexpr const char* CYAN        = "\033[38;2;125;195;255m";
    constexpr const char* GRAY        = "\033[38;2;120;130;145m";

    // Cursor controls
    constexpr const char* HIDE_CURSOR = "\033[?25l";
    constexpr const char* SHOW_CURSOR = "\033[?25h";
}

namespace {

namespace box {
    constexpr const char* TL = "╭";
    constexpr const char* TR = "╮";
    constexpr const char* BL = "╰";
    constexpr const char* BR = "╯";
    constexpr const char* H  = "─";
    constexpr const char* V  = "│";
    constexpr const char* LT = "├";
    constexpr const char* RT = "┤";
}

std::string repeat_str(std::string_view s, size_t count) {
    std::string out;
    out.reserve(s.size() * count);
    for (size_t i = 0; i < count; ++i) {
        out += s;
    }
    return out;
}

std::string box_top_line(size_t width = UiRenderer::DEFAULT_DASHBOARD_WIDTH) {
    return std::string(color::BORDER) + box::TL + repeat_str(box::H, width - 2) + box::TR + color::RESET;
}

std::string box_separator_line(size_t width = UiRenderer::DEFAULT_DASHBOARD_WIDTH) {
    return std::string(color::BORDER) + box::LT + repeat_str(box::H, width - 2) + box::RT + color::RESET;
}

std::string box_bottom_line(size_t width = UiRenderer::DEFAULT_DASHBOARD_WIDTH) {
    return std::string(color::BORDER) + box::BL + repeat_str(box::H, width - 2) + box::BR + color::RESET;
}

std::string make_box_row(std::string_view content, size_t inner_width) {
    const size_t vw = UiRenderer::visible_width(content);
    std::string line;
    line += color::BORDER;
    line += box::V;
    line += color::RESET;
    line += " ";
    line += content;
    if (vw + 2 < inner_width) {
        line.append(inner_width - vw - 2, ' ');
    }
    line += " ";
    line += color::BORDER;
    line += box::V;
    line += color::RESET;
    return line;
}

const char* get_quality_tier(float rtt, float jitter, const char*& out_color) {
    if (rtt <= 0.0f) {
        out_color = color::MUTED;
        return "[INITIALIZING]";
    }
    if (rtt <= 80.0f && jitter <= 5.0f) {
        out_color = color::MINT;
        return "[EXCELLENT]";
    }
    if (rtt <= 150.0f && jitter <= 15.0f) {
        out_color = color::ACCENT;
        return "[GOOD]";
    }
    if (rtt <= 220.0f && jitter <= 30.0f) {
        out_color = color::AMBER;
        return "[FAIR]";
    }
    out_color = color::CORAL;
    return "[POOR]";
}

} // anonymous namespace

UiRenderer::UiRenderer() = default;

void UiRenderer::set_session_info(uint32_t pid, uint32_t hook_count, double target_ping_ms, bool dry_run) {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    m_pid = pid;
    m_hook_count = hook_count;
    m_target_ping_ms = target_ping_ms;
    m_dry_run = dry_run;
    m_dirty = true;
}

void UiRenderer::set_connection_status(const std::string& status) {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    m_connection_status = status;
    m_dirty = true;
}

std::string UiRenderer::connection_status() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_connection_status;
}

void UiRenderer::set_terminal_dimensions(size_t cols, size_t rows) {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    m_cols = std::max(MIN_DASHBOARD_WIDTH, cols);
    m_rows = std::max(MIN_DASHBOARD_ROWS, rows);
    m_dirty = true;
}

size_t UiRenderer::terminal_cols() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_cols;
}

size_t UiRenderer::terminal_rows() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_rows;
}

size_t UiRenderer::dashboard_display_rows() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    const size_t available = m_rows > 17 ? (m_rows - 17) : 6;
    return std::clamp(available, size_t{6}, size_t{20});
}

void UiRenderer::set_dashboard_mode(bool enabled) {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    m_dashboard_mode = enabled;
    m_dirty = true;
}

bool UiRenderer::is_dashboard_mode() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_dashboard_mode;
}

bool UiRenderer::needs_redraw() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_dirty;
}

void UiRenderer::render_header(uint32_t pid, uint32_t hook_count, double target_ping_ms, bool dry_run) {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    m_pid = pid;
    m_hook_count = hook_count;
    m_target_ping_ms = target_ping_ms;
    m_dry_run = dry_run;

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

void UiRenderer::record_action_internal(const ipc::TelemetryPayload& t) {
    if (m_session_start_time == std::chrono::steady_clock::time_point{}) {
        m_session_start_time = std::chrono::steady_clock::now();
    }

    ++m_total_actions;
    if (t.applied) {
        ++m_actions_mitigated;
        m_cumulative_time_saved_ms += t.delay_reduced_ms;
        m_delay_saved_samples.push_back(t.delay_reduced_ms);
    }
    m_last_smoothed_rtt = t.smoothed_rtt_ms;
    m_last_jitter = t.jitter_ms;
    m_rtt_samples.push_back(t.measured_rtt_ms);

    if (t.clamped_floor) {
        ++m_guards.floor_clamps;
    }
    if (t.spike_filtered) {
        ++m_guards.spike_filtered;
    }
    if (t.cold_start_guard) {
        ++m_guards.cold_start_guards;
    }
    if (t.cast_active) {
        ++m_guards.cast_locks_preserved;
    }

    ActionLogEntry entry{};
    entry.index = m_total_actions;
    entry.action_id = t.action_id;
    entry.sequence = t.sequence;
    entry.original_lock_ms = t.original_lock_ms;
    entry.adjusted_lock_ms = t.adjusted_lock_ms;
    entry.delay_reduced_ms = t.delay_reduced_ms;
    entry.measured_rtt_ms = t.measured_rtt_ms;
    entry.smoothed_rtt_ms = t.smoothed_rtt_ms;
    entry.jitter_ms = t.jitter_ms;
    entry.clamped_floor = (t.clamped_floor != 0);
    entry.dry_run = (t.dry_run != 0);
    entry.applied = (t.applied != 0);
    entry.cast_active = (t.cast_active != 0);
    entry.spike_filtered = (t.spike_filtered != 0);
    entry.cold_start_guard = (t.cold_start_guard != 0);
    entry.timestamp_str = current_time_hhmmss();

    if (m_action_ring_buffer.size() >= RING_BUFFER_CAPACITY) {
        m_action_ring_buffer.pop_front();
    }
    m_action_ring_buffer.push_back(entry);

    m_dirty = true;
}

void UiRenderer::log_action(const ipc::TelemetryPayload& t, bool verbose) {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    record_action_internal(t);

    if (!m_dashboard_mode) {
        if (!verbose && t.delay_reduced_ms <= 0.0f) {
            return; // In non-verbose mode, skip actions that required no mitigation
        }
        std::cout << format_action_line(m_action_ring_buffer.back()) << "\n";
    }
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

    const char* quality_color = color::GRAY;
    const char* quality_str = get_quality_tier(
        (m_total_actions > 0) ? m_last_smoothed_rtt : 0.0f,
        m_last_jitter,
        quality_color
    );

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

void UiRenderer::render_dashboard(bool dry_run, bool verbose) {
    std::lock_guard<std::mutex> lock(m_render_mutex);

    const size_t width = m_cols;
    const size_t inner_w = width - 2;
    const size_t display_rows = m_rows > 17 ? std::clamp(m_rows - 17, size_t{6}, size_t{20}) : 6;

    // ANSI cursor hide & home: reposition cursor to top-left of the console buffer
    std::cout << color::HIDE_CURSOR << "\033[H";

    // Top border
    std::cout << box_top_line(width) << "\033[K\n";

    // Hero title banner
    const std::string header_title = std::string(color::TITLE) + color::BOLD +
        "⚡ FFXIV STANDALONE LATENCY MITIGATOR (C++20) — LIVE COMBAT DASHBOARD" + color::RESET;
    std::cout << make_box_row(header_title, inner_w) << "\033[K\n";

    // Target process metadata
    std::ostringstream meta_ss;
    if (m_pid == 0) {
        meta_ss << color::MUTED << "Target: " << color::TARGET << game::definitions::DEFAULT_GAME_PROCESS_NAME << color::RESET
                << color::MUTED << " │ Status: " << color::AMBER << m_connection_status << color::RESET
                << color::MUTED << " │ Mode: " << (dry_run ? std::string(color::PURPLE) + "DRY-RUN" : std::string(color::MINT) + "ACTIVE") << color::RESET;
    } else {
        meta_ss << color::MUTED << "Target: " << color::TARGET << game::definitions::DEFAULT_GAME_PROCESS_NAME << color::RESET
                << color::TEXT << " (PID: " << m_pid << ")" << color::RESET
                << color::MUTED << " │ Detours: " << color::MINT << m_hook_count << "/" << game::definitions::TOTAL_AVAILABLE_HOOKS << " Active" << color::RESET
                << color::MUTED << " │ Mode: " << (dry_run ? std::string(color::PURPLE) + "DRY-RUN" : std::string(color::MINT) + "ACTIVE") << color::RESET;
    }
    std::cout << make_box_row(meta_ss.str(), inner_w) << "\033[K\n";

    // Separator to KPI block
    std::cout << box_separator_line(width) << "\033[K\n";

    // KPI Row 1: Latency & Target
    const char* quality_color = color::MUTED;
    const char* quality_str = get_quality_tier(
        (m_total_actions > 0) ? m_last_smoothed_rtt : 0.0f,
        m_last_jitter,
        quality_color
    );
    const std::string ping_bar = make_bar(m_last_smoothed_rtt, 200.0f, 8, color::ACCENT);
    const auto rtt_dist = compute_distribution(m_rtt_samples);

    std::ostringstream kpi1;
    kpi1 << color::BOLD << color::TEXT << "NETWORK & LATENCY: " << color::RESET
         << "Ping " << ping_bar << " " << color::TEXT << std::fixed << std::setprecision(1)
         << std::setw(5) << m_last_smoothed_rtt << "ms" << color::RESET
         << " " << quality_color << quality_str << color::RESET
         << color::MUTED << " (±" << std::fixed << std::setprecision(1) << m_last_jitter << "ms jitter)"
         << " │ Target: " << color::TEXT << std::fixed << std::setprecision(0) << m_target_ping_ms << "ms"
         << color::MUTED << " │ RTT Med/P95/Max: " << color::TEXT << std::fixed << std::setprecision(0)
         << rtt_dist.median_val << "/" << rtt_dist.p95_val << "/" << rtt_dist.max_val << "ms" << color::RESET;
    std::cout << make_box_row(kpi1.str(), inner_w) << "\033[K\n";

    // KPI Row 2: Mitigation & Throughput
    const float mit_ratio = (m_total_actions > 0) ?
        static_cast<float>(m_actions_mitigated) / static_cast<float>(m_total_actions) : 0.0f;
    const std::string mit_bar = make_bar(mit_ratio, 1.0f, 8, color::MINT);
    const double avg_reduction = m_actions_mitigated > 0 ?
        (m_cumulative_time_saved_ms / static_cast<double>(m_actions_mitigated)) : 0.0;

    std::ostringstream kpi2;
    kpi2 << color::BOLD << color::TEXT << "MITIGATION & THROUGHPUT: " << color::RESET
         << "Mitigated " << mit_bar << " " << color::MINT << std::fixed << std::setprecision(0)
         << (mit_ratio * 100.0f) << "%" << color::RESET
         << color::MUTED << " (" << m_actions_mitigated << "/" << m_total_actions << ")"
         << " │ Saved: " << color::MINT << color::BOLD << std::fixed << std::setprecision(2)
         << (m_cumulative_time_saved_ms / constants::MS_PER_SECOND) << "s" << color::RESET
         << color::MUTED << " (Avg: " << color::TEXT << std::fixed << std::setprecision(1) << avg_reduction << "ms" << color::MUTED << ")"
         << " │ Total Actions: " << color::TEXT << m_total_actions << color::RESET;
    std::cout << make_box_row(kpi2.str(), inner_w) << "\033[K\n";

    // KPI Row 3: Pacing & Guards
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_secs = (m_session_start_time != std::chrono::steady_clock::time_point{}) ?
        std::chrono::duration_cast<std::chrono::seconds>(now - m_session_start_time) :
        std::chrono::seconds{0};
    const double apm = (elapsed_secs.count() >= 3 && m_total_actions > 0) ?
        (static_cast<double>(m_total_actions) / static_cast<double>(elapsed_secs.count())) * 60.0 : 0.0;

    std::ostringstream kpi3;
    kpi3 << color::BOLD << color::TEXT << "SAFETY GUARDS & DIAGNOSTICS: " << color::RESET
         << "APM: " << color::TEXT << std::fixed << std::setprecision(1) << apm << color::RESET
         << color::MUTED << " │ Uptime: " << color::TEXT << format_time_hhmmss(elapsed_secs) << color::RESET
         << color::MUTED << " │ Floor: " << color::AMBER << m_guards.floor_clamps << color::RESET
         << color::MUTED << " │ Spike: " << color::CORAL << m_guards.spike_filtered << color::RESET
         << color::MUTED << " │ Cold: " << color::ACCENT << m_guards.cold_start_guards << color::RESET
         << color::MUTED << " │ Cast: " << color::TARGET << m_guards.cast_locks_preserved << color::RESET;
    std::cout << make_box_row(kpi3.str(), inner_w) << "\033[K\n";

    // Separator to Action Log
    std::cout << box_separator_line(width) << "\033[K\n";
    std::cout << make_box_row(std::string(color::BOLD) + color::TITLE + "RECENT ACTION LOG & COMBAT STREAM" + color::RESET, inner_w) << "\033[K\n";

    std::ostringstream tbl_hdr;
    if (inner_w >= 90) {
        tbl_hdr << color::MUTED
                << "TIME     │ #SEQ  │ ACTION   │ ANIMATION LOCK         │ SAVED     │ RTT (SMOOTH)   │ STATUS"
                << color::RESET;
    } else {
        tbl_hdr << color::MUTED
                << "TIME    │#SEQ │ACTION│LOCK BEFORE->AFTER│SAVED  │RTT (SMOOTH)│STATUS"
                << color::RESET;
    }
    std::cout << make_box_row(tbl_hdr.str(), inner_w) << "\033[K\n";

    // Render newest actions
    const size_t entries_count = m_action_ring_buffer.size();
    const size_t display_count = (std::min)(entries_count, display_rows);
    const size_t start_idx = entries_count > display_rows ? (entries_count - display_rows) : 0;

    for (size_t i = 0; i < display_rows; ++i) {
        if (i < display_count) {
            const auto& entry = m_action_ring_buffer[start_idx + i];
            std::ostringstream row_ss;
            if (inner_w >= 90) {
                row_ss << color::MUTED << entry.timestamp_str << " │ "
                       << color::TEXT << "#" << std::setw(4) << std::setfill('0') << entry.index << std::setfill(' ') << color::MUTED << " │ "
                       << color::TARGET << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << entry.action_id << std::setfill(' ') << std::dec << color::MUTED << " │ "
                       << color::TEXT << std::fixed << std::setprecision(1) << std::setw(5) << entry.original_lock_ms << "ms ➔ "
                       << color::MINT << std::setw(5) << entry.adjusted_lock_ms << "ms" << color::MUTED << " │ ";

                if (entry.delay_reduced_ms > 0) {
                    row_ss << color::MINT << color::BOLD << "-" << std::fixed << std::setprecision(1) << std::setw(5) << entry.delay_reduced_ms << "ms" << color::RESET << color::MUTED << " │ ";
                } else {
                    row_ss << color::MUTED << "  +0.0ms  │ ";
                }

                row_ss << color::TEXT << std::setw(4) << static_cast<int>(entry.measured_rtt_ms) << "ms ("
                       << std::setw(3) << static_cast<int>(entry.smoothed_rtt_ms) << "ms)" << color::MUTED << " │ ";
            } else {
                row_ss << color::MUTED << entry.timestamp_str << "│"
                       << color::TEXT << "#" << std::setw(4) << std::setfill('0') << entry.index << std::setfill(' ') << color::MUTED << "│"
                       << color::TARGET << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << entry.action_id << std::setfill(' ') << std::dec << color::MUTED << "│"
                       << color::TEXT << std::fixed << std::setprecision(1) << std::setw(5) << entry.original_lock_ms << "m->"
                       << color::MINT << std::setw(5) << entry.adjusted_lock_ms << "m" << color::MUTED << "│";

                if (entry.delay_reduced_ms > 0) {
                    row_ss << color::MINT << "-" << std::fixed << std::setprecision(1) << std::setw(5) << entry.delay_reduced_ms << "m" << color::MUTED << "│";
                } else {
                    row_ss << color::MUTED << " +0.0m │";
                }

                row_ss << color::TEXT << std::setw(3) << static_cast<int>(entry.measured_rtt_ms) << "m("
                       << std::setw(3) << static_cast<int>(entry.smoothed_rtt_ms) << "m)" << color::MUTED << "│";
            }

            std::string tags;
            if (entry.dry_run) tags += std::string(color::PURPLE) + "[🧪 DRY-RUN] ";
            else if (entry.applied) tags += std::string(color::MINT) + color::BOLD + "[✔ MITIGATED] ";
            else if (entry.cast_active) tags += std::string(color::TARGET) + "[🛡 CAST-LOCK] ";
            else tags += std::string(color::MUTED) + "[PASS] ";

            if (entry.clamped_floor) tags += std::string(color::AMBER) + "[⚠ FLOOR] ";
            if (entry.spike_filtered) tags += std::string(color::CORAL) + "[⚡ SPIKE] ";
            if (entry.cold_start_guard) tags += std::string(color::ACCENT) + "[❄ COLD] ";
            tags += color::RESET;

            row_ss << tags;
            std::cout << make_box_row(row_ss.str(), inner_w) << "\033[K\n";
        } else {
            if (entries_count == 0 && i == 0) {
                std::cout << make_box_row(std::string(color::MUTED) + "   --    │  --   │   --     │  [Waiting for game process & actions...]  │" + color::RESET, inner_w) << "\033[K\n";
            } else {
                std::cout << make_box_row(std::string(color::MUTED) + "   --    │  --   │   --     │          --            │    --     │       --       │   --" + color::RESET, inner_w) << "\033[K\n";
            }
        }
    }

    // Hotkey footer
    std::cout << box_separator_line(width) << "\033[K\n";
    std::ostringstream ctrl_ss;
    ctrl_ss << color::MUTED << "Controls: "
            << color::BOLD << color::TEXT << "[Q]" << color::MUTED << " Exit   "
            << color::BOLD << color::TEXT << "[D]" << color::MUTED << " Dry-Run (" << (dry_run ? std::string(color::AMBER) + "ON" : std::string(color::TEXT) + "OFF") << color::MUTED << ")   "
            << color::BOLD << color::TEXT << "[L]" << color::MUTED << " Verbose (" << (verbose ? std::string(color::AMBER) + "ON" : std::string(color::TEXT) + "OFF") << color::MUTED << ")   "
            << color::BOLD << color::TEXT << "[C]" << color::MUTED << " Clear Stats   "
            << color::BOLD << color::TEXT << "[S]" << color::MUTED << " Refresh View"
            << color::RESET;
    std::cout << make_box_row(ctrl_ss.str(), inner_w) << "\033[K\n";

    // Bottom border
    std::cout << box_bottom_line(width) << "\033[K\n\033[J" << std::flush;

    m_dirty = false;
}

void UiRenderer::render_final_report() {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    std::cout << color::SHOW_CURSOR; // Ensure cursor is visible on exit

    const auto rtt_dist = compute_distribution(m_rtt_samples);
    const auto saved_dist = compute_distribution(m_delay_saved_samples);

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_secs = (m_session_start_time != std::chrono::steady_clock::time_point{}) ?
        std::chrono::duration_cast<std::chrono::seconds>(now - m_session_start_time) :
        std::chrono::seconds{0};

    const double apm = (elapsed_secs.count() >= 3 && m_total_actions > 0) ?
        (static_cast<double>(m_total_actions) / static_cast<double>(elapsed_secs.count())) * 60.0 : 0.0;

    const double mit_pct = (m_total_actions > 0) ?
        (static_cast<double>(m_actions_mitigated) / static_cast<double>(m_total_actions)) * 100.0 : 0.0;

    const double avg_reduction = (m_actions_mitigated > 0) ?
        (m_cumulative_time_saved_ms / static_cast<double>(m_actions_mitigated)) : 0.0;

    const char* quality_color = color::MUTED;
    const char* quality_str = get_quality_tier(
        (m_total_actions > 0) ? m_last_smoothed_rtt : 0.0f,
        m_last_jitter,
        quality_color
    );

    const size_t width = m_cols;
    const size_t inner_w = width - 2;

    std::cout << "\n" << box_top_line(width) << "\n";
    std::cout << make_box_row(std::string(color::TITLE) + color::BOLD + "FINAL SESSION TELEMETRY REPORT" + color::RESET, inner_w) << "\n";
    std::cout << box_separator_line(width) << "\n";

    std::ostringstream ss;
    ss << "Session Duration:    " << format_time_hhmmss(elapsed_secs);
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Total Actions:       " << m_total_actions << " actions (" << std::fixed << std::setprecision(1) << apm << " APM)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Mitigated Actions:   " << color::MINT << m_actions_mitigated << "/" << m_total_actions << color::RESET
       << " (" << std::fixed << std::setprecision(1) << mit_pct << "%)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Total Time Saved:    " << color::MINT << color::BOLD << std::fixed << std::setprecision(2)
       << (m_cumulative_time_saved_ms / constants::MS_PER_SECOND) << "s" << color::RESET
       << " (Avg: " << std::fixed << std::setprecision(1) << avg_reduction << " ms / action)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    std::cout << box_separator_line(width) << "\n";
    std::cout << make_box_row(std::string(color::BOLD) + "LATENCY & SAVINGS DISTRIBUTION" + color::RESET, inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "RTT Distribution:    Min: " << std::fixed << std::setprecision(0) << rtt_dist.min_val
       << "ms │ Med: " << rtt_dist.median_val << "ms │ P95: " << rtt_dist.p95_val
       << "ms │ Max: " << rtt_dist.max_val << "ms";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Delay Reduction:     Min: " << std::fixed << std::setprecision(0) << saved_dist.min_val
       << "ms │ Med: " << saved_dist.median_val << "ms │ P95: " << saved_dist.p95_val
       << "ms │ Max: " << saved_dist.max_val << "ms";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Final Jitter:        ± " << std::fixed << std::setprecision(1) << m_last_jitter << " ms";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Network Rating:      " << quality_color << quality_str << color::RESET;
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    std::cout << box_separator_line(width) << "\n";
    std::cout << make_box_row(std::string(color::BOLD) + "SAFETY GUARD DIAGNOSTICS" + color::RESET, inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Floor Clamps:        " << m_guards.floor_clamps << " (actions bounded by safety floor)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Spike Rejections:    " << m_guards.spike_filtered << " (outlier spikes filtered by median filter)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Cold-Start Guards:   " << m_guards.cold_start_guards << " (initial packet burst protections)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Cast Locks Active:   " << m_guards.cast_locks_preserved << " (cast animation locks preserved)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    std::cout << box_bottom_line(width) << "\n" << std::flush;
}

void UiRenderer::reset_stats() {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    m_total_actions = 0;
    m_actions_mitigated = 0;
    m_cumulative_time_saved_ms = 0.0;
    m_last_smoothed_rtt = 0.0f;
    m_last_jitter = 0.0f;
    m_guards = GuardCounters{};
    m_rtt_samples.clear();
    m_delay_saved_samples.clear();
    m_action_ring_buffer.clear();
    m_session_start_time = std::chrono::steady_clock::time_point{};
    m_dirty = true;
}

uint64_t UiRenderer::total_actions() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_total_actions;
}

uint64_t UiRenderer::actions_mitigated() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_actions_mitigated;
}

double UiRenderer::cumulative_time_saved_ms() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_cumulative_time_saved_ms;
}

float UiRenderer::last_smoothed_rtt() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_last_smoothed_rtt;
}

float UiRenderer::last_jitter() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_last_jitter;
}

LatencyDistribution UiRenderer::rtt_distribution() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return compute_distribution(m_rtt_samples);
}

LatencyDistribution UiRenderer::delay_saved_distribution() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return compute_distribution(m_delay_saved_samples);
}

GuardCounters UiRenderer::guard_counters() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_guards;
}

double UiRenderer::calculate_apm() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    if (m_total_actions == 0 || m_session_start_time == std::chrono::steady_clock::time_point{}) {
        return 0.0;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_sec = std::chrono::duration_cast<std::chrono::seconds>(now - m_session_start_time).count();
    if (elapsed_sec < 3) {
        return 0.0;
    }
    return (static_cast<double>(m_total_actions) / static_cast<double>(elapsed_sec)) * 60.0;
}

size_t UiRenderer::ring_buffer_size() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    return m_action_ring_buffer.size();
}

std::chrono::seconds UiRenderer::uptime() const {
    std::lock_guard<std::mutex> lock(m_render_mutex);
    if (m_session_start_time == std::chrono::steady_clock::time_point{}) {
        return std::chrono::seconds{0};
    }
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now - m_session_start_time);
}

size_t UiRenderer::visible_width(std::string_view s) {
    size_t width = 0;
    bool in_ansi = false;
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\033') {
            in_ansi = true;
            ++i;
            continue;
        }
        if (in_ansi) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                in_ansi = false;
            }
            ++i;
            continue;
        }

        if (c < 0x80) {
            ++width;
            ++i;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8
            ++width;
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8
            if (i + 2 < s.size()) {
                const uint32_t cp = ((c & 0x0F) << 12) |
                                    ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                                    (static_cast<unsigned char>(s[i + 2]) & 0x3F);
                if (cp == 0x26A0 || cp == 0x26A1) {
                    width += 2;
                } else {
                    width += 1;
                }
            } else {
                width += 1;
            }
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8 (Emoji like 🛡 U+1F6E1, 🧪 U+1F9EA)
            width += 2;
            i += 4;
        } else {
            ++i;
        }
    }
    return width;
}

LatencyDistribution UiRenderer::compute_distribution(const std::vector<float>& samples) {
    if (samples.empty()) {
        return LatencyDistribution{};
    }
    std::vector<float> sorted = samples;
    std::sort(sorted.begin(), sorted.end());

    LatencyDistribution dist;
    dist.min_val = sorted.front();
    dist.median_val = sorted[sorted.size() / 2];
    dist.p95_val = sorted[static_cast<size_t>(static_cast<double>(sorted.size() - 1) * 0.95)];
    dist.max_val = sorted.back();
    return dist;
}

std::string UiRenderer::make_bar(float value, float max_val, size_t bar_width, const char* bar_color) {
    if (max_val <= 0.0f) {
        max_val = 1.0f;
    }
    const float ratio = std::clamp(value / max_val, 0.0f, 1.0f);
    const size_t filled = static_cast<size_t>(std::round(ratio * static_cast<float>(bar_width)));
    const size_t clamped_filled = (std::min)(filled, bar_width);
    const size_t empty = bar_width - clamped_filled;

    std::string res;
    res += bar_color;
    for (size_t i = 0; i < clamped_filled; ++i) {
        res += "█";
    }
    res += color::GRAY;
    for (size_t i = 0; i < empty; ++i) {
        res += "░";
    }
    res += color::RESET;
    return res;
}

std::string UiRenderer::format_time_hhmmss(std::chrono::seconds total_secs) {
    const auto s = total_secs.count();
    const auto hours = s / 3600;
    const auto mins = (s % 3600) / 60;
    const auto secs = s % 60;

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(mins),
                  static_cast<long long>(secs));
    return std::string(buf);
}

std::string UiRenderer::current_time_hhmmss() {
    const auto now = std::chrono::system_clock::now();
    const auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_buf);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

std::string UiRenderer::format_action_line(const ActionLogEntry& entry) {
    std::ostringstream ss;
    ss << color::GRAY << "[#" << std::setw(4) << std::setfill('0') << entry.index << std::setfill(' ') << "] " << color::RESET
       << color::BOLD << "Action: " << color::CYAN << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << entry.action_id << std::setfill(' ') << std::dec << color::RESET
       << " | "
       << "Orig: " << std::fixed << std::setprecision(1) << entry.original_lock_ms << "ms -> "
       << color::GREEN << entry.adjusted_lock_ms << "ms" << color::RESET
       << " | Saved: " << color::YELLOW << color::BOLD << "+" << entry.delay_reduced_ms << "ms" << color::RESET
       << " | RTT: " << entry.measured_rtt_ms << "ms (smooth: " << entry.smoothed_rtt_ms << "ms)";

    if (entry.clamped_floor) {
        ss << " " << color::YELLOW << "[Floor Clamp]" << color::RESET;
    }
    if (entry.spike_filtered) {
        ss << " " << color::YELLOW << "[Spike Filtered]" << color::RESET;
    }
    if (entry.cold_start_guard) {
        ss << " " << color::YELLOW << "[Cold Start]" << color::RESET;
    }
    if (entry.dry_run) {
        ss << " " << color::BLUE << "[Dry Run]" << color::RESET;
    }
    if (entry.cast_active) {
        ss << " " << color::GRAY << "[Cast]" << color::RESET;
    }

    return ss.str();
}

} // namespace mitigator::loader
