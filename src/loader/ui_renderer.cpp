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
    if (rtt <= 40.0f && jitter <= 3.0f) {
        out_color = color::MINT;
        return "[EXCELLENT]";
    }
    if (rtt <= 80.0f && jitter <= 10.0f) {
        out_color = color::ACCENT;
        return "[GOOD]";
    }
    if (rtt <= 130.0f && jitter <= 20.0f) {
        out_color = color::AMBER;
        return "[FAIR]";
    }
    out_color = color::CORAL;
    return "[POOR]";
}

} // anonymous namespace

UiRenderer::UiRenderer()
    : m_session_start_time(std::chrono::steady_clock::now()) {}

void UiRenderer::set_session_info(uint32_t pid, uint32_t hook_count, double target_ping_ms, bool dry_run) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_pid = pid;
    m_hook_count = hook_count;
    m_target_ping_ms = target_ping_ms;
    m_dry_run = dry_run;
    m_dirty = true;
}

void UiRenderer::set_connection_status(const std::string& status) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_connection_status = status;
    m_dirty = true;
}

std::string UiRenderer::connection_status() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_connection_status;
}

void UiRenderer::set_terminal_dimensions(size_t cols, size_t rows) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_cols = std::max(cols, MIN_DASHBOARD_WIDTH);
    m_rows = std::max(rows, MIN_DASHBOARD_ROWS);
    m_dirty = true;
}

size_t UiRenderer::terminal_cols() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_cols;
}

size_t UiRenderer::terminal_rows() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_rows;
}

size_t UiRenderer::dashboard_display_rows() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    if (m_rows <= 18) return 6;
    size_t calculated = m_rows - 18;
    return std::clamp<size_t>(calculated, 6, RING_BUFFER_CAPACITY);
}

void UiRenderer::set_dashboard_mode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_dashboard_mode = enabled;
    m_dirty = true;
}

bool UiRenderer::is_dashboard_mode() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_dashboard_mode;
}

bool UiRenderer::needs_redraw() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_dirty;
}

int UiRenderer::active_tab() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_active_tab;
}

void UiRenderer::set_active_tab(int tab_index) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_active_tab = std::clamp(tab_index, 0, 2);
    m_dirty = true;
}

MitigationConfig UiRenderer::config() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_config;
}

void UiRenderer::set_config(const MitigationConfig& config) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_config = ConfigManager::clamp_and_validate(config);
    m_dry_run = m_config.dry_run;
    m_target_ping_ms = m_config.target_ping_ms;
    m_dirty = true;
    if (m_on_config_changed) {
        m_on_config_changed(m_config);
    }
}

void UiRenderer::set_on_config_changed(std::function<void(const MitigationConfig&)> callback) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_on_config_changed = std::move(callback);
}

void UiRenderer::set_on_reset_stats_callback(std::function<void()> callback) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_on_reset_stats = std::move(callback);
}

std::vector<float> UiRenderer::rtt_history() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return {m_rtt_history.begin(), m_rtt_history.end()};
}

void UiRenderer::render_header(uint32_t pid, uint32_t hook_count, double target_ping_ms, bool dry_run) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
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
    m_recent_action_times.push_back(std::chrono::steady_clock::now());
    while (!m_recent_action_times.empty()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - m_recent_action_times.front()).count();
        if (age > 60) {
            m_recent_action_times.pop_front();
        } else {
            break;
        }
    }

    if (t.applied) {
        ++m_actions_mitigated;
        m_cumulative_time_saved_ms += static_cast<double>(t.delay_reduced_ms);
        m_delay_saved_samples.push_back(t.delay_reduced_ms);
    }
    m_last_smoothed_rtt = t.smoothed_rtt_ms;
    m_last_jitter = t.jitter_ms;
    m_rtt_samples.push_back(t.measured_rtt_ms);

    m_rtt_history.push_back(t.measured_rtt_ms);
    if (m_rtt_history.size() > SPARKLINE_HISTORY_CAPACITY) {
        m_rtt_history.pop_front();
    }

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
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    record_action_internal(t);

    if (!m_dashboard_mode) {
        if (!verbose && t.delay_reduced_ms <= 0.0f) {
            return;
        }
        std::cout << format_action_line(m_action_ring_buffer.back()) << "\n";
    }
}

void UiRenderer::log_status(const std::string& message, bool is_error) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    if (is_error) {
        std::cout << color::RED << color::BOLD << "[ERROR] " << message << color::RESET << "\n";
    } else {
        std::cout << color::GREEN << "[INFO] " << message << color::RESET << "\n";
    }
}

void UiRenderer::render_stats_summary() {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);

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
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);

    std::cout << color::GRAY << "\nControls: "
              << color::BOLD << "[1..3]" << color::RESET << color::GRAY << " Tabs | "
              << color::BOLD << "[Q]" << color::RESET << color::GRAY << " Exit | "
              << color::BOLD << "[D]" << color::RESET << color::GRAY << " Dry-Run (" << (dry_run ? "ON" : "OFF") << ") | "
              << color::BOLD << "[L]" << color::RESET << color::GRAY << " Verbose (" << (verbose ? "ON" : "OFF") << ") | "
              << color::BOLD << "[S]" << color::RESET << color::GRAY << " Save Settings | "
              << color::BOLD << "[C]" << color::RESET << color::GRAY << " Clear Stats\n"
              << color::RESET;
}

std::string UiRenderer::render_sparkline_bar(const std::vector<float>& samples, size_t width) {
    static constexpr const char* BLOCKS[] = {
        " ", "▂", "▃", "▄", "▅", "▆", "▇", "█"
    };
    if (samples.empty() || width == 0) {
        return "";
    }
    const size_t count = (std::min)(samples.size(), width);
    const size_t start = samples.size() - count;

    float min_val = 0.0f;
    float max_val = 150.0f;
    for (size_t i = start; i < samples.size(); ++i) {
        if (samples[i] > max_val) {
            max_val = samples[i];
        }
    }
    if (max_val <= min_val) {
        max_val = min_val + 1.0f;
    }

    std::string out;
    for (size_t i = start; i < samples.size(); ++i) {
        const float val = samples[i];
        const char* col = color::MINT;
        if (val > 150.0f) {
            col = color::CORAL;
        } else if (val > 100.0f) {
            col = color::AMBER;
        } else if (val > 60.0f) {
            col = color::ACCENT;
        }

        size_t idx = static_cast<size_t>((val - min_val) / (max_val - min_val) * 7.99f);
        idx = std::clamp(idx, size_t{0}, size_t{7});
        out += col;
        out += BLOCKS[idx];
    }
    out += color::RESET;
    return out;
}

std::string UiRenderer::render_snapshot_to_string(int width, int height) const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);

    const size_t cols = std::max(static_cast<size_t>(width), MIN_DASHBOARD_WIDTH);
    const size_t rows = std::max(static_cast<size_t>(height), MIN_DASHBOARD_ROWS);
    const size_t inner_w = cols - 2;

    std::ostringstream doc;

    // Top border
    doc << box_top_line(cols) << "\033[K\n";

    // Hero title banner
    const std::string header_title = std::string(color::TITLE) + color::BOLD +
        "⚡ FFXIV STANDALONE LATENCY MITIGATOR (C++20) — LIVE COMBAT DASHBOARD" + color::RESET;
    doc << make_box_row(header_title, inner_w) << "\033[K\n";

    // Target process metadata
    std::ostringstream meta_ss;
    if (m_pid == 0) {
        meta_ss << color::MUTED << "Target: " << color::TARGET << game::definitions::DEFAULT_GAME_PROCESS_NAME << color::RESET
                << color::MUTED << " │ Status: " << color::AMBER << "STANDBY (" << m_connection_status << ")" << color::RESET
                << color::MUTED << " │ Mode: " << (m_dry_run ? std::string(color::PURPLE) + "DRY-RUN" : std::string(color::MINT) + "ACTIVE") << color::RESET;
    } else {
        meta_ss << color::MUTED << "Target: " << color::TARGET << game::definitions::DEFAULT_GAME_PROCESS_NAME << color::RESET
                << color::TEXT << " (PID: " << m_pid << ")" << color::RESET
                << color::MUTED << " │ Detours: " << color::MINT << m_hook_count << "/" << game::definitions::TOTAL_AVAILABLE_HOOKS << " Active" << color::RESET
                << color::MUTED << " │ Mode: " << (m_dry_run ? std::string(color::PURPLE) + "DRY-RUN" : std::string(color::MINT) + "ACTIVE") << color::RESET;
    }
    doc << make_box_row(meta_ss.str(), inner_w) << "\033[K\n";

    // Separator to Tab bar
    doc << box_separator_line(cols) << "\033[K\n";

    // 3-Tab selector bar
    std::ostringstream tab_ss;
    auto make_tab = [&](int idx, std::string_view label, std::string_view num) {
        if (m_active_tab == idx) {
            return std::string(color::BOLD) + color::TARGET + "▶ [" + std::string(num) + "] " + std::string(label) + " " + color::RESET;
        } else {
            return std::string(color::MUTED) + "  [" + std::string(num) + "] " + std::string(label) + " " + color::RESET;
        }
    };
    tab_ss << make_tab(0, "Live Combat", "1")
           << color::BORDER << "│" << color::RESET << " "
           << make_tab(1, "Latency Analytics", "2")
           << color::BORDER << "│" << color::RESET << " "
           << make_tab(2, "Settings & Safety", "3");
    doc << make_box_row(tab_ss.str(), inner_w) << "\033[K\n";

    // Separator to KPI block
    doc << box_separator_line(cols) << "\033[K\n";

    // KPI Row 1: Network & Latency
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
    doc << make_box_row(kpi1.str(), inner_w) << "\033[K\n";

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
    doc << make_box_row(kpi2.str(), inner_w) << "\033[K\n";

    // KPI Row 3: Pacing & Guards
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_secs = (m_session_start_time != std::chrono::steady_clock::time_point{}) ?
        std::chrono::duration_cast<std::chrono::seconds>(now - m_session_start_time) :
        std::chrono::seconds{0};
    const double apm = calculate_apm();

    std::ostringstream kpi3;
    kpi3 << color::BOLD << color::TEXT << "SAFETY GUARDS & DIAGNOSTICS: " << color::RESET
         << "APM: " << color::TEXT << std::fixed << std::setprecision(1) << apm << color::RESET
         << color::MUTED << " │ Uptime: " << color::TEXT << format_time_hhmmss(elapsed_secs) << color::RESET
         << color::MUTED << " │ Floor: " << color::AMBER << m_guards.floor_clamps << color::RESET
         << color::MUTED << " │ Spike: " << color::CORAL << m_guards.spike_filtered << color::RESET
         << color::MUTED << " │ Cold: " << color::ACCENT << m_guards.cold_start_guards << color::RESET
         << color::MUTED << " │ Cast: " << color::TARGET << m_guards.cast_locks_preserved << color::RESET;
    doc << make_box_row(kpi3.str(), inner_w) << "\033[K\n";

    // Separator to Active Tab View
    doc << box_separator_line(cols) << "\033[K\n";

    size_t content_rows_used = 0;

    // -------------------------------------------------------------------------
    // TAB 0: LIVE COMBAT ACTION STREAM
    // -------------------------------------------------------------------------
    if (m_active_tab == 0) {
        doc << make_box_row(std::string(color::BOLD) + color::TITLE + "LIVE COMBAT ACTION STREAM" + color::RESET, inner_w) << "\033[K\n";
        content_rows_used++;

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
        doc << make_box_row(tbl_hdr.str(), inner_w) << "\033[K\n";
        content_rows_used++;

        const size_t display_rows = (rows > 18) ? std::clamp<size_t>(rows - 18, 6, RING_BUFFER_CAPACITY) : 6;
        const size_t entries_count = m_action_ring_buffer.size();
        const size_t display_count = (std::min)(entries_count, display_rows);
        const size_t start_idx = entries_count > display_rows ? (entries_count - display_rows) : 0;

        if (entries_count == 0) {
            doc << make_box_row(std::string(color::MUTED) + "Waiting for combat actions... (Cast spells or weaponskills in game)" + color::RESET, inner_w) << "\033[K\n";
            content_rows_used++;
            for (size_t i = 1; i < display_rows; ++i) {
                doc << make_box_row("", inner_w) << "\033[K\n";
                content_rows_used++;
            }
        } else {
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
                    if (entry.dry_run) tags += std::string(color::PURPLE) + "[DRY-RUN] ";
                    else if (entry.applied) tags += std::string(color::MINT) + color::BOLD + "[MITIGATED] ";
                    else if (entry.cast_active) tags += std::string(color::TARGET) + "[CAST-LOCK] ";
                    else tags += std::string(color::MUTED) + "[PASS] ";

                    if (entry.clamped_floor) tags += std::string(color::AMBER) + "[FLOOR] ";
                    if (entry.spike_filtered) tags += std::string(color::CORAL) + "[SPIKE] ";
                    if (entry.cold_start_guard) tags += std::string(color::ACCENT) + "[COLD] ";

                    row_ss << tags << color::RESET;
                    doc << make_box_row(row_ss.str(), inner_w) << "\033[K\n";
                } else {
                    doc << make_box_row("", inner_w) << "\033[K\n";
                }
                content_rows_used++;
            }
        }
    }
    // -------------------------------------------------------------------------
    // TAB 1: LATENCY ANALYTICS & WAVEFORM
    // -------------------------------------------------------------------------
    else if (m_active_tab == 1) {
        doc << make_box_row(std::string(color::BOLD) + color::TITLE + "REAL-TIME LATENCY WAVEFORM (Last 60 Actions)" + color::RESET, inner_w) << "\033[K\n";
        content_rows_used++;

        std::vector<float> hist(m_rtt_history.begin(), m_rtt_history.end());
        const size_t sparkline_width = (inner_w >= 30) ? std::min<size_t>(inner_w - 20, SPARKLINE_HISTORY_CAPACITY) : 20;

        if (hist.empty()) {
            doc << make_box_row(std::string(color::MUTED) + "  No latency samples collected yet. Awaiting action responses from game." + color::RESET, inner_w) << "\033[K\n";
            content_rows_used++;
        } else {
            // Waveform visual chart
            float max_rtt = 150.0f;
            for (float val : hist) {
                if (val > max_rtt) max_rtt = val;
            }


            // Row 3: 150ms+
            std::string row3 = "  150ms+ ┤ ";
            for (size_t i = (hist.size() > sparkline_width ? hist.size() - sparkline_width : 0); i < hist.size(); ++i) {
                if (hist[i] >= 150.0f) row3 += std::string(color::CORAL) + "█" + color::RESET;
                else if (hist[i] >= 120.0f) row3 += std::string(color::AMBER) + "▆" + color::RESET;
                else row3 += " ";
            }
            doc << make_box_row(row3, inner_w) << "\033[K\n";
            content_rows_used++;

            // Row 2: 80ms - 150ms
            std::string row2 = "   80ms  ┤ ";
            for (size_t i = (hist.size() > sparkline_width ? hist.size() - sparkline_width : 0); i < hist.size(); ++i) {
                if (hist[i] >= 80.0f) row2 += std::string(color::ACCENT) + "█" + color::RESET;
                else if (hist[i] >= 60.0f) row2 += std::string(color::MINT) + "▄" + color::RESET;
                else row2 += " ";
            }
            doc << make_box_row(row2, inner_w) << "\033[K\n";
            content_rows_used++;

            // Row 1: 30ms - 80ms
            std::string row1 = "   30ms  ┤ ";
            for (size_t i = (hist.size() > sparkline_width ? hist.size() - sparkline_width : 0); i < hist.size(); ++i) {
                if (hist[i] >= 30.0f) row1 += std::string(color::MINT) + "█" + color::RESET;
                else row1 += std::string(color::MINT) + "▂" + color::RESET;
            }
            doc << make_box_row(row1, inner_w) << "\033[K\n";
            content_rows_used++;

            // Sparkline bar line
            std::string sparkline_row = "  Waveform: " + render_sparkline_bar(hist, sparkline_width);
            doc << make_box_row(sparkline_row, inner_w) << "\033[K\n";
            content_rows_used++;
        }

        doc << box_separator_line(cols) << "\033[K\n";

        // Section: Latency Percentiles
        doc << make_box_row(std::string(color::BOLD) + color::TITLE + "LATENCY PERCENTILES" + color::RESET, inner_w) << "\033[K\n";
        content_rows_used++;

        std::ostringstream p_ss;
        p_ss << "  Min: " << color::TEXT << std::fixed << std::setprecision(1) << rtt_dist.min_val << "ms" << color::RESET
             << " │ Median: " << color::TEXT << rtt_dist.median_val << "ms" << color::RESET
             << " │ P95: " << color::TEXT << rtt_dist.p95_val << "ms" << color::RESET
             << " │ Max: " << color::TEXT << rtt_dist.max_val << "ms" << color::RESET
             << " │ Jitter: " << color::TEXT << "±" << m_last_jitter << "ms" << color::RESET
             << " │ Quality: " << quality_color << quality_str << color::RESET;
        doc << make_box_row(p_ss.str(), inner_w) << "\033[K\n";
        content_rows_used++;

        doc << box_separator_line(cols) << "\033[K\n";

        // Section: Time Saved Summary
        doc << make_box_row(std::string(color::BOLD) + color::TITLE + "TIME SAVED SUMMARY" + color::RESET, inner_w) << "\033[K\n";
        content_rows_used++;

        auto saved_dist = compute_distribution(m_delay_saved_samples);
        std::ostringstream s_ss;
        s_ss << "  Total Saved: " << color::MINT << color::BOLD << std::fixed << std::setprecision(2)
             << (m_cumulative_time_saved_ms / 1000.0) << "s" << color::RESET
             << " │ Avg Reduction: " << color::TEXT << std::fixed << std::setprecision(1) << avg_reduction << "ms" << color::RESET
             << " │ Saved P95: " << color::TEXT << saved_dist.p95_val << "ms" << color::RESET
             << " │ Saved Max: " << color::TEXT << saved_dist.max_val << "ms" << color::RESET;
        doc << make_box_row(s_ss.str(), inner_w) << "\033[K\n";
        content_rows_used++;

        doc << box_separator_line(cols) << "\033[K\n";

        // Section: Safety & Guards
        doc << make_box_row(std::string(color::BOLD) + color::TITLE + "SAFETY & GUARDS" + color::RESET, inner_w) << "\033[K\n";
        content_rows_used++;

        std::ostringstream g_ss;
        g_ss << "  Floor Clamps: " << color::AMBER << m_guards.floor_clamps << color::RESET
             << " │ Spike Filtered: " << color::CORAL << m_guards.spike_filtered << color::RESET
             << " │ Cold-Start Guards: " << color::ACCENT << m_guards.cold_start_guards << color::RESET
             << " │ Cast-Locks Preserved: " << color::TARGET << m_guards.cast_locks_preserved << color::RESET;
        doc << make_box_row(g_ss.str(), inner_w) << "\033[K\n";
        content_rows_used++;
    }
    // -------------------------------------------------------------------------
    // TAB 2: INTERACTIVE SETTINGS & CONFIGURATION
    // -------------------------------------------------------------------------
    else if (m_active_tab == 2) {
        doc << make_box_row(std::string(color::BOLD) + color::TITLE + "INTERACTIVE SETTINGS & CONFIGURATION" + color::RESET, inner_w) << "\033[K\n";
        content_rows_used++;

        doc << make_box_row("  Settings are automatically read from and saved to mitigator_config.json.", inner_w) << "\033[K\n";
        content_rows_used++;

        doc << box_separator_line(cols) << "\033[K\n";

        std::ostringstream cfg1;
        cfg1 << "  Min Animation Lock Floor : [" << color::BOLD << color::TEXT << std::setw(5) << std::fixed << std::setprecision(1)
             << m_config.min_animation_lock_ms << " ms" << color::RESET << "]"
             << color::MUTED << "  (Hotkeys: [F] -5ms  /  [Shift+F] +5ms)" << color::RESET;
        doc << make_box_row(cfg1.str(), inner_w) << "\033[K\n";
        content_rows_used++;

        std::ostringstream cfg2;
        cfg2 << "  Simulated Target Ping    : [" << color::BOLD << color::TEXT << std::setw(5) << std::fixed << std::setprecision(1)
             << m_config.target_ping_ms << " ms" << color::RESET << "]"
             << color::MUTED << "  (Hotkeys: [P] -5ms  /  [Shift+P] +5ms)" << color::RESET;
        doc << make_box_row(cfg2.str(), inner_w) << "\033[K\n";
        content_rows_used++;

        std::ostringstream cfg3;
        cfg3 << "  Mitigation Safety Margin : [" << color::BOLD << color::TEXT << std::setw(5) << std::fixed << std::setprecision(1)
             << m_config.safety_margin_ms << " ms" << color::RESET << "]"
             << color::MUTED << "  (Hotkeys: [M] -1ms  /  [Shift+M] +1ms)" << color::RESET;
        doc << make_box_row(cfg3.str(), inner_w) << "\033[K\n";
        content_rows_used++;

        std::ostringstream cfg4;
        cfg4 << "  Operating Mode           : [" << (m_dry_run ? std::string(color::PURPLE) + "DRY-RUN (Monitoring Only)" : std::string(color::MINT) + "ACTIVE (Live Animation Lock Mitigation)") << color::RESET << "]"
             << color::MUTED << "  (Hotkey: [D] Toggle)" << color::RESET;
        doc << make_box_row(cfg4.str(), inner_w) << "\033[K\n";
        content_rows_used++;

        std::ostringstream cfg5;
        cfg5 << "  Logging Verbosity        : [" << (m_config.verbose ? std::string(color::ACCENT) + "VERBOSE (All Actions)" : std::string(color::MUTED) + "CONCISE (Mitigations Only)") << color::RESET << "]"
             << color::MUTED << "  (Hotkey: [L] Toggle)" << color::RESET;
        doc << make_box_row(cfg5.str(), inner_w) << "\033[K\n";
        content_rows_used++;

        doc << box_separator_line(cols) << "\033[K\n";

        std::ostringstream cfg6;
        cfg6 << "  Configuration Persistence: " << color::TARGET << ConfigManager::DEFAULT_CONFIG_FILENAME << color::RESET
             << color::MUTED << "  (Hotkey: [S] Save configuration immediately)" << color::RESET;
        doc << make_box_row(cfg6.str(), inner_w) << "\033[K\n";
        content_rows_used++;

        std::ostringstream cfg7;
        cfg7 << "  Telemetry Reset          : " << color::MUTED << "Clear counters and statistics (Hotkey: [C])" << color::RESET;
        doc << make_box_row(cfg7.str(), inner_w) << "\033[K\n";
        content_rows_used++;
    }

    // Fill remaining rows if terminal height allows
    const size_t target_total_rows = (rows > 20) ? (rows - 4) : rows;
    const size_t fixed_header_rows = 9;
    if (content_rows_used + fixed_header_rows < target_total_rows) {
        size_t pad = target_total_rows - (content_rows_used + fixed_header_rows);
        for (size_t i = 0; i < pad; ++i) {
            doc << make_box_row("", inner_w) << "\033[K\n";
        }
    }

    // Bottom Separator
    doc << box_separator_line(cols) << "\033[K\n";

    // Bottom hotkey toolbar
    std::ostringstream bar_ss;
    bar_ss << color::GRAY << "Controls: "
           << color::BOLD << "[1..3]" << color::RESET << color::GRAY << " Tabs │ "
           << color::BOLD << "[Q]" << color::RESET << color::GRAY << " Exit │ "
           << color::BOLD << "[D]" << color::RESET << color::GRAY << " Dry-Run │ "
           << color::BOLD << "[L]" << color::RESET << color::GRAY << " Verbose │ "
           << color::BOLD << "[S]" << color::RESET << color::GRAY << " Save Settings │ "
           << color::BOLD << "[C]" << color::RESET << color::GRAY << " Reset Stats" << color::RESET;
    doc << make_box_row(bar_ss.str(), inner_w) << "\033[K\n";

    // Bottom border
    doc << box_bottom_line(cols) << "\033[K\n";

    return doc.str();
}

void UiRenderer::render_dashboard(bool dry_run, bool verbose) {
    (void)dry_run;
    (void)verbose;
    std::string snapshot = render_snapshot_to_string(static_cast<int>(m_cols), static_cast<int>(m_rows));
    std::cout << color::HIDE_CURSOR << "\033[H" << snapshot << std::flush;
}

void UiRenderer::render_final_report() {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    std::cout << color::SHOW_CURSOR;

    const auto rtt_dist = compute_distribution(m_rtt_samples);
    const auto saved_dist = compute_distribution(m_delay_saved_samples);

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_secs = (m_session_start_time != std::chrono::steady_clock::time_point{}) ?
        std::chrono::duration_cast<std::chrono::seconds>(now - m_session_start_time) :
        std::chrono::seconds{0};

    const double apm = calculate_apm();
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
    std::cout << make_box_row(std::string(color::TITLE) + color::BOLD + "FINAL COMBAT SESSION REPORT" + color::RESET, inner_w) << "\n";
    std::cout << box_separator_line(width) << "\n";

    std::ostringstream ss;
    ss << "Session Duration:    " << format_time_hhmmss(elapsed_secs);
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Total Actions:       " << m_total_actions << " actions (" << std::fixed << std::setprecision(1) << apm << " APM)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Actions Mitigated:   " << m_actions_mitigated << " (" << std::fixed << std::setprecision(1) << mit_pct << "%)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Total Time Saved:    " << color::MINT << color::BOLD << std::fixed << std::setprecision(2)
       << (m_cumulative_time_saved_ms / constants::MS_PER_SECOND) << "s" << color::RESET
       << " (Avg: " << std::fixed << std::setprecision(1) << avg_reduction << " ms / action)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    std::cout << box_separator_line(width) << "\n";
    std::cout << make_box_row(std::string(color::BOLD) + "LATENCY & SAVINGS DISTRIBUTION" + color::RESET, inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "RTT (Min/Med/P95):   " << std::fixed << std::setprecision(0) << rtt_dist.min_val
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
    ss << "Safety Floor Clamps: " << m_guards.floor_clamps << " (actions bounded by safety floor)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Spike Rejections:    " << m_guards.spike_filtered << " (outlier spikes filtered by median filter)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Cold-Start Guards:   " << m_guards.cold_start_guards << " (initial packet burst protections)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    ss.str(""); ss.clear();
    ss << "Cast Locks Kept:     " << m_guards.cast_locks_preserved << " (cast animation locks preserved)";
    std::cout << make_box_row(ss.str(), inner_w) << "\n";

    std::cout << box_bottom_line(width) << "\n" << std::flush;
}

void UiRenderer::reset_stats() {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_total_actions = 0;
    m_actions_mitigated = 0;
    m_cumulative_time_saved_ms = 0.0;
    m_last_smoothed_rtt = 0.0f;
    m_last_jitter = 0.0f;
    m_guards = GuardCounters{};
    m_rtt_samples.clear();
    m_delay_saved_samples.clear();
    m_action_ring_buffer.clear();
    m_recent_action_times.clear();
    m_rtt_history.clear();
    m_session_start_time = std::chrono::steady_clock::now();
    m_dirty = true;
}

uint64_t UiRenderer::total_actions() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_total_actions;
}

uint64_t UiRenderer::actions_mitigated() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_actions_mitigated;
}

double UiRenderer::cumulative_time_saved_ms() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_cumulative_time_saved_ms;
}

float UiRenderer::last_smoothed_rtt() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_last_smoothed_rtt;
}

float UiRenderer::last_jitter() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_last_jitter;
}

LatencyDistribution UiRenderer::rtt_distribution() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return compute_distribution(m_rtt_samples);
}

LatencyDistribution UiRenderer::delay_saved_distribution() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return compute_distribution(m_delay_saved_samples);
}

GuardCounters UiRenderer::guard_counters() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_guards;
}

double UiRenderer::calculate_apm() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return static_cast<double>(m_recent_action_times.size());
}

size_t UiRenderer::ring_buffer_size() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    return m_action_ring_buffer.size();
}

std::chrono::seconds UiRenderer::uptime() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    if (m_session_start_time == std::chrono::steady_clock::time_point{}) {
        return std::chrono::seconds{0};
    }
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - m_session_start_time);
}

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------

size_t UiRenderer::visible_width(std::string_view s) {
    size_t count = 0;
    bool in_escape = false;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (s[i] == '\033') {
            in_escape = true;
            continue;
        }
        if (in_escape) {
            if (s[i] == 'm' || s[i] == 'K' || s[i] == 'H' || s[i] == 'J' || s[i] == 'h' || s[i] == 'l') {
                in_escape = false;
            }
            continue;
        }
        // Count only UTF-8 start bytes (exclude 10xxxxxx)
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

LatencyDistribution UiRenderer::compute_distribution(const std::vector<float>& samples) {
    if (samples.empty()) return LatencyDistribution{};
    std::vector<float> sorted = samples;
    std::sort(sorted.begin(), sorted.end());
    size_t n = sorted.size();
    LatencyDistribution dist{};
    dist.min_val = sorted.front();
    dist.max_val = sorted.back();
    dist.median_val = sorted[n / 2];
    size_t p95_idx = (n > 1) ? static_cast<size_t>(std::floor(static_cast<double>(n - 1) * 0.95)) : 0;
    dist.p95_val = sorted[p95_idx];
    return dist;
}

std::string UiRenderer::make_bar(float value, float max_val, size_t bar_width, const char* bar_color) {
    float ratio = (max_val > 0.0f) ? std::clamp(value / max_val, 0.0f, 1.0f) : 0.0f;
    size_t filled = static_cast<size_t>(std::round(ratio * static_cast<float>(bar_width)));
    std::string out = "[";
    if (bar_color) out += bar_color;
    for (size_t i = 0; i < filled; ++i) out += "█";
    out += color::RESET;
    for (size_t i = filled; i < bar_width; ++i) out += "░";
    out += "]";
    return out;
}

std::string UiRenderer::format_time_hhmmss(std::chrono::seconds total_secs) {
    int s = static_cast<int>(total_secs.count());
    int h = s / 3600;
    int m = (s % 3600) / 60;
    int sec = s % 60;
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(2) << h << ":"
       << std::setw(2) << m << ":"
       << std::setw(2) << sec;
    return ss.str();
}

std::string UiRenderer::current_time_hhmmss() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
#if defined(_WIN32)
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    std::ostringstream ss;
    ss << std::setfill('0') << std::setw(2) << tm_buf.tm_hour << ":"
       << std::setw(2) << tm_buf.tm_min << ":"
       << std::setw(2) << tm_buf.tm_sec;
    return ss.str();
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
