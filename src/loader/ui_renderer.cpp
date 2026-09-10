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
    constexpr const char* RESET   = "\033[0m";
    constexpr const char* BOLD    = "\033[1m";
    constexpr const char* RED     = "\033[31m";
    constexpr const char* GREEN   = "\033[32m";
    constexpr const char* YELLOW  = "\033[33m";
    constexpr const char* BLUE    = "\033[34m";
    constexpr const char* CYAN    = "\033[36m";
    constexpr const char* GRAY    = "\033[90m";
}

namespace {

namespace box {
    constexpr const char* TL = "┌";
    constexpr const char* TR = "┐";
    constexpr const char* BL = "└";
    constexpr const char* BR = "┘";
    constexpr const char* H  = "─";
    constexpr const char* V  = "│";
    constexpr const char* LT = "├";
    constexpr const char* RT = "┤";
    constexpr const char* TT = "┬";
    constexpr const char* BT = "┴";
}

std::string repeat_str(std::string_view s, size_t count) {
    std::string out;
    out.reserve(s.size() * count);
    for (size_t i = 0; i < count; ++i) {
        out += s;
    }
    return out;
}

std::string box_top_line(size_t width = UiRenderer::DASHBOARD_WIDTH) {
    return std::string(box::TL) + repeat_str(box::H, width - 2) + box::TR;
}

std::string box_separator_line(size_t width = UiRenderer::DASHBOARD_WIDTH) {
    return std::string(box::LT) + repeat_str(box::H, width - 2) + box::RT;
}

std::string box_bottom_line(size_t width = UiRenderer::DASHBOARD_WIDTH) {
    return std::string(box::BL) + repeat_str(box::H, width - 2) + box::BR;
}

std::string box_split_separator_line(size_t left_inner_w, size_t right_inner_w) {
    return std::string(box::LT) + repeat_str(box::H, left_inner_w) + box::TT + repeat_str(box::H, right_inner_w) + box::RT;
}

std::string box_split_rejoin_line(size_t left_inner_w, size_t right_inner_w) {
    return std::string(box::LT) + repeat_str(box::H, left_inner_w) + box::BT + repeat_str(box::H, right_inner_w) + box::RT;
}

std::string make_box_row(std::string_view content, size_t inner_width = UiRenderer::INNER_WIDTH) {
    const size_t vw = UiRenderer::visible_width(content);
    std::string line;
    line += box::V;
    line += " ";
    line += content;
    if (vw + 2 < inner_width) {
        line.append(inner_width - vw - 2, ' ');
    }
    line += " ";
    line += box::V;
    return line;
}

std::string make_box_split_row(std::string_view left, size_t left_inner_w,
                               std::string_view right, size_t right_inner_w) {
    const size_t l_vw = UiRenderer::visible_width(left);
    const size_t r_vw = UiRenderer::visible_width(right);

    std::string line;
    line += box::V;
    line += " ";
    line += left;
    if (l_vw + 2 < left_inner_w) {
        line.append(left_inner_w - l_vw - 2, ' ');
    }
    line += " ";
    line += box::V;
    line += " ";
    line += right;
    if (r_vw + 2 < right_inner_w) {
        line.append(right_inner_w - r_vw - 2, ' ');
    }
    line += " ";
    line += box::V;
    return line;
}

const char* get_quality_tier(float rtt, float jitter, const char*& out_color) {
    if (rtt <= 0.0f) {
        out_color = color::GRAY;
        return "[INITIALIZING]";
    }
    if (rtt <= 80.0f && jitter <= 5.0f) {
        out_color = color::GREEN;
        return "[EXCELLENT]";
    }
    if (rtt <= 150.0f && jitter <= 15.0f) {
        out_color = color::CYAN;
        return "[GOOD]";
    }
    if (rtt <= 220.0f && jitter <= 30.0f) {
        out_color = color::YELLOW;
        return "[FAIR]";
    }
    out_color = color::RED;
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

    // ANSI cursor home: reposition cursor to top-left of the console buffer
    std::cout << "\033[H";

    // Top border
    std::cout << box_top_line() << "\033[K\n";

    // Main header title
    const std::string header_title = std::string(color::CYAN) + color::BOLD +
        "FFXIV STANDALONE LATENCY MITIGATOR (C++20) — LIVE COMBAT DASHBOARD" + color::RESET;
    std::cout << make_box_row(header_title, INNER_WIDTH) << "\033[K\n";

    // Target process metadata
    std::ostringstream meta_ss;
    if (m_pid == 0) {
        meta_ss << "Target: " << color::YELLOW << game::definitions::DEFAULT_GAME_PROCESS_NAME << color::RESET
                << " │ Status: " << color::CYAN << m_connection_status << color::RESET
                << " │ Mode: " << (dry_run ? std::string(color::YELLOW) + "DRY-RUN" : std::string(color::GREEN) + "ACTIVE") << color::RESET;
    } else {
        meta_ss << "Target: " << color::GREEN << game::definitions::DEFAULT_GAME_PROCESS_NAME << color::RESET
                << " (PID: " << m_pid << ") │ Detours: " << color::GREEN << m_hook_count << "/" << game::definitions::TOTAL_AVAILABLE_HOOKS << " Active" << color::RESET
                << " │ Mode: " << (dry_run ? std::string(color::YELLOW) + "DRY-RUN" : std::string(color::GREEN) + "ACTIVE") << color::RESET;
    }
    std::cout << make_box_row(meta_ss.str(), INNER_WIDTH) << "\033[K\n";

    // Two-column split separator (37 left, 38 right -> 78 total with 3 borders)
    std::cout << box_split_separator_line(37, 38) << "\033[K\n";

    // Row 1: Titles
    const std::string left_t1 = std::string(color::BOLD) + "NETWORK & LATENCY" + color::RESET;
    const std::string right_t1 = std::string(color::BOLD) + "MITIGATION & THROUGHPUT" + color::RESET;
    std::cout << make_box_split_row(left_t1, 37, right_t1, 38) << "\033[K\n";

    // Network quality tier
    const char* quality_color = color::GRAY;
    const char* quality_str = get_quality_tier(
        (m_total_actions > 0) ? m_last_smoothed_rtt : 0.0f,
        m_last_jitter,
        quality_color
    );

    // Row 2: Ping Bar & Mitigated Bar
    std::ostringstream left_s2;
    const std::string ping_bar = make_bar(m_last_smoothed_rtt, 200.0f, 8, color::CYAN);
    left_s2 << "Ping: " << ping_bar << " " << std::fixed << std::setprecision(1)
            << std::setw(5) << m_last_smoothed_rtt << "ms " << quality_color << quality_str << color::RESET;

    std::ostringstream right_s2;
    const float mit_ratio = (m_total_actions > 0) ?
        static_cast<float>(m_actions_mitigated) / static_cast<float>(m_total_actions) : 0.0f;
    const std::string mit_bar = make_bar(mit_ratio, 1.0f, 8, color::GREEN);
    right_s2 << "Mitigated: " << mit_bar << " " << std::fixed << std::setprecision(0)
             << (mit_ratio * 100.0f) << "% (" << m_actions_mitigated << "/" << m_total_actions << ")";
    std::cout << make_box_split_row(left_s2.str(), 37, right_s2.str(), 38) << "\033[K\n";

    // Row 3: Jitter & Total Saved
    std::ostringstream left_s3;
    left_s3 << "Jitter: ±" << std::fixed << std::setprecision(1) << m_last_jitter << "ms │ Target: "
            << std::fixed << std::setprecision(0) << m_target_ping_ms << "ms";

    const double avg_reduction = m_actions_mitigated > 0 ?
        (m_cumulative_time_saved_ms / static_cast<double>(m_actions_mitigated)) : 0.0;
    std::ostringstream right_s3;
    right_s3 << "Saved: " << color::YELLOW << color::BOLD << std::fixed << std::setprecision(2)
             << (m_cumulative_time_saved_ms / constants::MS_PER_SECOND) << "s" << color::RESET
             << " (Avg: " << std::fixed << std::setprecision(1) << avg_reduction << "ms)";
    std::cout << make_box_split_row(left_s3.str(), 37, right_s3.str(), 38) << "\033[K\n";

    // Row 4: Percentiles & Uptime/APM
    const auto rtt_dist = compute_distribution(m_rtt_samples);
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_secs = (m_session_start_time != std::chrono::steady_clock::time_point{}) ?
        std::chrono::duration_cast<std::chrono::seconds>(now - m_session_start_time) :
        std::chrono::seconds{0};
    const double apm = (elapsed_secs.count() >= 3 && m_total_actions > 0) ?
        (static_cast<double>(m_total_actions) / static_cast<double>(elapsed_secs.count())) * 60.0 : 0.0;

    std::ostringstream left_s4;
    left_s4 << "RTT Med/P95/Max: " << std::fixed << std::setprecision(0)
            << rtt_dist.median_val << "/" << rtt_dist.p95_val << "/" << rtt_dist.max_val << "ms";

    std::ostringstream right_s4;
    right_s4 << "Uptime: " << format_time_hhmmss(elapsed_secs)
             << " │ APM: " << std::fixed << std::setprecision(1) << apm;
    std::cout << make_box_split_row(left_s4.str(), 37, right_s4.str(), 38) << "\033[K\n";

    // Rejoin split columns
    std::cout << box_split_rejoin_line(37, 38) << "\033[K\n";

    // Safety Guards & Diagnostics Card
    std::cout << make_box_row(std::string(color::BOLD) + "SAFETY GUARDS & DIAGNOSTICS" + color::RESET, INNER_WIDTH) << "\033[K\n";
    std::ostringstream guards_ss;
    guards_ss << "Floor: " << color::YELLOW << m_guards.floor_clamps << color::RESET
              << " │ Spike: " << color::YELLOW << m_guards.spike_filtered << color::RESET
              << " │ Cold: " << color::YELLOW << m_guards.cold_start_guards << color::RESET
              << " │ Cast: " << color::CYAN << m_guards.cast_locks_preserved << color::RESET;
    std::cout << make_box_row(guards_ss.str(), INNER_WIDTH) << "\033[K\n";

    // Action Log Header
    std::cout << box_separator_line() << "\033[K\n";
    std::cout << make_box_row(std::string(color::BOLD) + "RECENT ACTION LOG (Last 6 Actions)" + color::RESET, INNER_WIDTH) << "\033[K\n";

    std::ostringstream tbl_hdr;
    tbl_hdr << color::GRAY
            << "TIME    │#SEQ │ACTION│LOCK BEFORE->AFTER│SAVED  │RTT (SMOOTH)│TAGS"
            << color::RESET;
    std::cout << make_box_row(tbl_hdr.str(), INNER_WIDTH) << "\033[K\n";

    // Render up to DASHBOARD_DISPLAY_ROWS (6) newest actions
    const size_t entries_count = m_action_ring_buffer.size();
    const size_t display_count = (std::min)(entries_count, DASHBOARD_DISPLAY_ROWS);
    const size_t start_idx = entries_count > DASHBOARD_DISPLAY_ROWS ? (entries_count - DASHBOARD_DISPLAY_ROWS) : 0;

    for (size_t i = 0; i < DASHBOARD_DISPLAY_ROWS; ++i) {
        if (i < display_count) {
            const auto& entry = m_action_ring_buffer[start_idx + i];
            std::ostringstream row_ss;
            row_ss << color::GRAY << entry.timestamp_str << "│"
                   << "#" << std::setw(4) << std::setfill('0') << entry.index << std::setfill(' ') << "│"
                   << color::CYAN << "0x" << std::hex << std::uppercase << std::setw(4) << std::setfill('0') << entry.action_id << std::setfill(' ') << std::dec << color::RESET
                   << "│"
                   << std::fixed << std::setprecision(1) << std::setw(5) << entry.original_lock_ms << "m->"
                   << color::GREEN << std::setw(5) << entry.adjusted_lock_ms << "m" << color::RESET
                   << "│"
                   << color::YELLOW << (entry.delay_reduced_ms > 0 ? "+" : " ")
                   << std::fixed << std::setprecision(1) << std::setw(5) << entry.delay_reduced_ms << "m" << color::RESET
                   << "│"
                   << std::setw(4) << static_cast<int>(entry.measured_rtt_ms) << "m("
                   << std::setw(3) << static_cast<int>(entry.smoothed_rtt_ms) << "m)│";

            std::string tags;
            if (entry.dry_run) tags += "[Dry Run] ";
            else if (entry.applied) tags += "[Mitigated] ";
            if (entry.clamped_floor) tags += "[Floor] ";
            if (entry.spike_filtered) tags += "[Spike] ";
            if (entry.cold_start_guard) tags += "[Cold] ";
            if (entry.cast_active) tags += "[Cast] ";
            if (tags.empty()) tags = "[Pass]";

            row_ss << color::YELLOW << tags << color::RESET;
            std::cout << make_box_row(row_ss.str(), INNER_WIDTH) << "\033[K\n";
        } else {
            if (entries_count == 0 && i == 0) {
                std::cout << make_box_row(std::string(color::GRAY) + "   --   │ --  │  --  │  [Waiting for game process & actions...]  │" + color::RESET, INNER_WIDTH) << "\033[K\n";
            } else {
                std::cout << make_box_row(std::string(color::GRAY) + "   --   │ --  │  --  │        --        │  --   │     --     │    --" + color::RESET, INNER_WIDTH) << "\033[K\n";
            }
        }
    }

    // Hotkey footer
    std::cout << box_separator_line() << "\033[K\n";
    std::ostringstream ctrl_ss;
    ctrl_ss << color::GRAY << "Controls: "
            << color::BOLD << "[Q]" << color::RESET << color::GRAY << " Exit  "
            << color::BOLD << "[D]" << color::RESET << color::GRAY << " Dry (" << (dry_run ? "ON" : "OFF") << ")  "
            << color::BOLD << "[L]" << color::RESET << color::GRAY << " Verb (" << (verbose ? "ON" : "OFF") << ")  "
            << color::BOLD << "[C]" << color::RESET << color::GRAY << " Clear  "
            << color::BOLD << "[S]" << color::RESET << color::GRAY << " View"
            << color::RESET;
    std::cout << make_box_row(ctrl_ss.str(), INNER_WIDTH) << "\033[K\n";

    // Bottom border with line clearing and screen clear to bottom
    std::cout << box_bottom_line() << "\033[K\n\033[J" << std::flush;

    m_dirty = false;
}

void UiRenderer::render_final_report() {
    std::lock_guard<std::mutex> lock(m_render_mutex);

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

    const char* quality_color = color::GRAY;
    const char* quality_str = get_quality_tier(
        (m_total_actions > 0) ? m_last_smoothed_rtt : 0.0f,
        m_last_jitter,
        quality_color
    );

    std::cout << "\n" << box_top_line() << "\n";
    std::cout << make_box_row(std::string(color::CYAN) + color::BOLD + "FINAL SESSION TELEMETRY REPORT" + color::RESET, INNER_WIDTH) << "\n";
    std::cout << box_separator_line() << "\n";

    std::ostringstream ss;
    ss << "Session Duration:    " << format_time_hhmmss(elapsed_secs);
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Total Actions:       " << m_total_actions << " actions (" << std::fixed << std::setprecision(1) << apm << " APM)";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Mitigated Actions:   " << color::GREEN << m_actions_mitigated << "/" << m_total_actions << color::RESET
       << " (" << std::fixed << std::setprecision(1) << mit_pct << "%)";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Total Time Saved:    " << color::YELLOW << color::BOLD << std::fixed << std::setprecision(2)
       << (m_cumulative_time_saved_ms / constants::MS_PER_SECOND) << "s" << color::RESET
       << " (Avg: " << std::fixed << std::setprecision(1) << avg_reduction << " ms / action)";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    std::cout << box_separator_line() << "\n";
    std::cout << make_box_row(std::string(color::BOLD) + "LATENCY & SAVINGS DISTRIBUTION" + color::RESET, INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "RTT Distribution:    Min: " << std::fixed << std::setprecision(0) << rtt_dist.min_val
       << "ms │ Med: " << rtt_dist.median_val << "ms │ P95: " << rtt_dist.p95_val
       << "ms │ Max: " << rtt_dist.max_val << "ms";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Delay Reduction:     Min: " << std::fixed << std::setprecision(0) << saved_dist.min_val
       << "ms │ Med: " << saved_dist.median_val << "ms │ P95: " << saved_dist.p95_val
       << "ms │ Max: " << saved_dist.max_val << "ms";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Final Jitter:        ± " << std::fixed << std::setprecision(1) << m_last_jitter << " ms";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Network Rating:      " << quality_color << quality_str << color::RESET;
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    std::cout << box_separator_line() << "\n";
    std::cout << make_box_row(std::string(color::BOLD) + "SAFETY GUARD DIAGNOSTICS" + color::RESET, INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Floor Clamps:        " << m_guards.floor_clamps << " (actions bounded by safety floor)";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Spike Rejections:    " << m_guards.spike_filtered << " (outlier spikes filtered by median filter)";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Cold-Start Guards:   " << m_guards.cold_start_guards << " (initial packet burst protections)";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    ss.str(""); ss.clear();
    ss << "Cast Locks Active:   " << m_guards.cast_locks_preserved << " (cast animation locks preserved)";
    std::cout << make_box_row(ss.str(), INNER_WIDTH) << "\n";

    std::cout << box_bottom_line() << "\n" << std::flush;
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
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\033') {
            in_ansi = true;
            continue;
        }
        if (in_ansi) {
            if ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z')) {
                in_ansi = false;
            }
            continue;
        }
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            ++width;
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
