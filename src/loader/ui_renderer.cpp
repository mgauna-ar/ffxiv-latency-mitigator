#include "loader/ui_renderer.hpp"

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/table.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>

#include <iostream>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <cmath>

namespace mitigator::loader {

using namespace ftxui;

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
    if (m_rows <= 16) return 6;
    size_t calculated = m_rows - 16;
    return std::clamp<size_t>(calculated, 6, RING_BUFFER_CAPACITY);
}

void UiRenderer::set_dashboard_mode(bool enabled) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    m_dashboard_mode = enabled;
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
    m_slider_min_lock = static_cast<int>(m_config.min_animation_lock_ms);
    m_slider_target_ping = static_cast<int>(m_config.target_ping_ms);
    m_slider_margin = static_cast<int>(m_config.safety_margin_ms);
    m_dry_run = m_config.dry_run;
    m_target_ping_ms = m_config.target_ping_ms;
    m_dirty = true;
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
    set_session_info(pid, hook_count, target_ping_ms, dry_run);
}

void UiRenderer::log_action(const ipc::TelemetryPayload& t, bool /*verbose*/) {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    record_action_internal(t);
}

void UiRenderer::record_action_internal(const ipc::TelemetryPayload& t) {
    m_total_actions++;
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
        m_actions_mitigated++;
        m_cumulative_time_saved_ms += static_cast<double>(t.delay_reduced_ms);
        m_delay_saved_samples.push_back(t.delay_reduced_ms);
    }

    if (t.measured_rtt_ms > 0.0f) {
        m_rtt_samples.push_back(t.measured_rtt_ms);
        m_rtt_history.push_back(t.measured_rtt_ms);
        while (m_rtt_history.size() > SPARKLINE_HISTORY_CAPACITY) {
            m_rtt_history.pop_front();
        }
    }

    m_last_smoothed_rtt = t.smoothed_rtt_ms;
    m_last_jitter = t.jitter_ms;

    if (t.clamped_floor) m_guards.floor_clamps++;
    if (t.spike_filtered) m_guards.spike_filtered++;
    if (t.cold_start_guard) m_guards.cold_start_guards++;
    if (t.cast_active) m_guards.cast_locks_preserved++;

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
    entry.clamped_floor = t.clamped_floor;
    entry.dry_run = t.dry_run;
    entry.applied = t.applied;
    entry.cast_active = t.cast_active;
    entry.spike_filtered = t.spike_filtered;
    entry.cold_start_guard = t.cold_start_guard;
    entry.timestamp_str = current_time_hhmmss();

    m_action_ring_buffer.push_back(entry);
    while (m_action_ring_buffer.size() > RING_BUFFER_CAPACITY) {
        m_action_ring_buffer.pop_front();
    }

    m_dirty = true;
}

void UiRenderer::log_status(const std::string& message, bool /*is_error*/) {
    set_connection_status(message);
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
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_session_start_time);
}

// -----------------------------------------------------------------------------
// FTXUI DOM Element Builders
// -----------------------------------------------------------------------------

Element UiRenderer::build_hero_banner() const {
    bool connected = (m_pid > 0);
    std::string pid_str = connected ? ("ffxiv_dx11.exe (PID: " + std::to_string(m_pid) + ")") : "ffxiv_dx11.exe";
    std::string mode_str = m_dry_run ? "DRY-RUN (TEST)" : "ACTIVE";
    auto mode_color = m_dry_run ? Color::RGB(190, 140, 255) : Color::RGB(85, 225, 145);

    return vbox({
        hbox({
            text(" ⚔ FFXIV STANDALONE LATENCY MITIGATOR (C++20) ") | bold | color(Color::RGB(130, 200, 255)),
            filler(),
            text(connected ? "● CONNECTED " : "● STANDBY ") | bold | color(connected ? Color::RGB(85, 225, 145) : Color::RGB(255, 195, 75)),
        }),
        hbox({
            text(" Target: ") | color(Color::Grey70),
            text(pid_str) | bold | color(Color::White),
            text("  │  Status: ") | color(Color::Grey50),
            text(m_connection_status) | color(Color::RGB(180, 210, 240)),
            filler(),
            text("Mode: ") | color(Color::Grey50),
            text(mode_str) | bold | color(mode_color),
            text(" "),
        }),
    });
}

Element UiRenderer::build_tab_live_combat() const {
    // 1. KPI Metric Bar
    float rtt = m_last_smoothed_rtt;
    float ping_ratio = std::clamp(rtt / 150.0f, 0.0f, 1.0f);
    std::string rtt_tier = (rtt <= 0.0f) ? "INITIAL" : (rtt < 30.0f) ? "EXCELLENT" : (rtt < 70.0f) ? "GOOD" : (rtt < 120.0f) ? "FAIR" : "POOR";
    auto rtt_tier_color = (rtt <= 0.0f) ? Color::Grey70 : (rtt < 30.0f) ? Color::RGB(85, 225, 145) : (rtt < 70.0f) ? Color::RGB(130, 200, 255) : (rtt < 120.0f) ? Color::RGB(255, 195, 75) : Color::RGB(255, 100, 100);

    double percent_mit = (m_total_actions > 0) ? (static_cast<double>(m_actions_mitigated) * 100.0 / m_total_actions) : 0.0;
    double saved_sec = m_cumulative_time_saved_ms / 1000.0;
    double avg_saved_ms = (m_actions_mitigated > 0) ? (m_cumulative_time_saved_ms / m_actions_mitigated) : 0.0;
    double apm = static_cast<double>(m_recent_action_times.size());
    std::string uptime_str = format_time_hhmmss(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_session_start_time));

    auto kpi_bar = vbox({
        hbox({
            text(" RTT: ") | color(Color::Grey70),
            text([&] {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << rtt << "ms";
                return ss.str();
            }()) | bold | color(Color::White),
            text(" [" + rtt_tier + "] ") | bold | color(rtt_tier_color),
            gauge(ping_ratio) | color(rtt_tier_color) | size(WIDTH, EQUAL, 12),
            text(" ±" + [&] {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << m_last_jitter << "ms";
                return ss.str();
            }() + " jitter  │  ") | color(Color::Grey70),
            text("Mitigated: ") | color(Color::Grey70),
            text([&] {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(0) << percent_mit << "% ("
                   << m_actions_mitigated << "/" << m_total_actions << ")";
                return ss.str();
            }()) | bold | color(Color::RGB(85, 225, 145)),
            text(" · Saved: ") | color(Color::Grey70),
            text([&] {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(2) << saved_sec << "s (Avg: "
                   << std::setprecision(1) << avg_saved_ms << "ms)";
                return ss.str();
            }()) | bold | color(Color::RGB(85, 225, 145)),
            filler(),
        }),
        hbox({
            text(" APM: ") | color(Color::Grey70),
            text([&] {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << apm;
                return ss.str();
            }()) | bold | color(Color::White),
            text("  │  Uptime: ") | color(Color::Grey70),
            text(uptime_str) | color(Color::White),
            text("  │  Guards: ") | color(Color::Grey70),
            text(std::to_string(m_guards.floor_clamps) + " floor · " +
                 std::to_string(m_guards.spike_filtered) + " spike · " +
                 std::to_string(m_guards.cold_start_guards) + " cold · " +
                 std::to_string(m_guards.cast_locks_preserved) + " cast") | color(Color::RGB(180, 210, 240)),
            filler(),
        }),
    });

    // 2. Action Table Header & Rows
    std::vector<std::vector<Element>> table_data;
    table_data.push_back({
        text(" TIME ") | bold | color(Color::Grey70),
        text(" SEQ · ACTION ") | bold | color(Color::Grey70),
        text(" ANIMATION LOCK TRANSITION ") | bold | color(Color::Grey70),
        text(" LATENCY (RTT) ") | bold | color(Color::Grey70),
        text(" STATUS ") | bold | color(Color::Grey70),
    });

    if (m_action_ring_buffer.empty()) {
        table_data.push_back({
            text(" --:--:-- ") | color(Color::Grey50),
            text("  -- · --  ") | color(Color::Grey50),
            text(" [Waiting for game process and combat actions...] ") | color(Color::RGB(130, 200, 255)),
            text("    --    ") | color(Color::Grey50),
            text("    --    ") | color(Color::Grey50),
        });
    } else {
        size_t display_count = dashboard_display_rows();
        size_t start_idx = (m_action_ring_buffer.size() > display_count) ? (m_action_ring_buffer.size() - display_count) : 0;
        for (size_t i = start_idx; i < m_action_ring_buffer.size(); ++i) {
            const auto& a = m_action_ring_buffer[i];

            std::ostringstream ss_seq;
            ss_seq << "#" << std::setw(4) << std::setfill('0') << a.sequence
                   << " · 0x" << std::hex << std::uppercase << a.action_id << std::dec;

            std::ostringstream ss_trans;
            ss_trans << std::fixed << std::setprecision(1) << std::setw(5) << a.original_lock_ms << "ms ➔ "
                     << std::setw(5) << a.adjusted_lock_ms << "ms ";
            if (a.delay_reduced_ms > 0.0f) {
                ss_trans << "(-" << std::setw(5) << a.delay_reduced_ms << "ms)";
            } else {
                ss_trans << "(  +0.0ms)";
            }

            std::ostringstream ss_rtt;
            ss_rtt << std::fixed << std::setprecision(1) << std::setw(5) << a.measured_rtt_ms << "ms ("
                   << std::setw(5) << a.smoothed_rtt_ms << "ms)";

            // Status Pill
            Element pill = text("[?]");
            if (a.dry_run) {
                pill = text("[🧪 DRY-RUN]") | bold | color(Color::RGB(190, 140, 255));
            } else if (a.cast_active) {
                pill = text("[🛡 CAST-LOCK]") | bold | color(Color::RGB(130, 200, 255));
            } else if (a.clamped_floor) {
                pill = text("[⚠ FLOOR]") | bold | color(Color::RGB(255, 195, 75));
            } else if (a.spike_filtered) {
                pill = text("[⚡ SPIKE]") | bold | color(Color::RGB(255, 100, 120));
            } else if (a.cold_start_guard) {
                pill = text("[❄ COLD]") | bold | color(Color::RGB(150, 220, 240));
            } else if (a.applied) {
                pill = text("[✔ MITIGATED]") | bold | color(Color::RGB(85, 225, 145));
            } else {
                pill = text("[--]") | color(Color::Grey50);
            }

            table_data.push_back({
                text(" " + a.timestamp_str + " ") | color(Color::Grey70),
                text(" " + ss_seq.str() + " ") | color(Color::White),
                text(" " + ss_trans.str() + " ") | color(a.applied ? Color::RGB(85, 225, 145) : Color::White),
                text(" " + ss_rtt.str() + " ") | color(Color::Grey70),
                hbox({ text(" "), pill, text(" ") }),
            });
        }
    }

    auto action_table = Table(table_data);
    action_table.SelectAll().Border(LIGHT);
    action_table.SelectRow(0).Decorate(bold);
    action_table.SelectRow(0).Border(LIGHT);

    return vbox({
        kpi_bar,
        separatorLight(),
        text(" LIVE COMBAT ACTION STREAM ") | bold | color(Color::RGB(130, 200, 255)),
        action_table.Render() | flex,
    });
}

Element UiRenderer::build_tab_latency_analytics() const {
    // 1. Latency Sparkline Waveform Graph
    std::vector<float> hist = m_rtt_history.empty() ? std::vector<float>{0.0f} : std::vector<float>(m_rtt_history.begin(), m_rtt_history.end());
    float max_sample = 50.0f;
    float min_sample = 1000.0f;
    for (float v : hist) {
        if (v > max_sample) max_sample = v;
        if (v < min_sample && v > 0.0f) min_sample = v;
    }
    if (min_sample > max_sample) min_sample = 0.0f;

    auto graph_fn = [hist, max_sample](int width, int height) -> std::vector<int> {
        std::vector<int> output(width, 0);
        if (hist.empty() || height <= 0) return output;
        for (int x = 0; x < width; ++x) {
            float sample_idx = static_cast<float>(x) / static_cast<float>(width) * static_cast<float>(hist.size() - 1);
            size_t idx = static_cast<size_t>(sample_idx);
            if (idx >= hist.size()) idx = hist.size() - 1;
            float val = hist[idx];
            float normalized = (max_sample > 0.0f) ? (val / max_sample) : 0.0f;
            output[x] = std::clamp(static_cast<int>(normalized * (height - 1)), 0, height - 1);
        }
        return output;
    };

    auto graph_view = vbox({
        hbox({
            text(" REAL-TIME LATENCY WAVEFORM (Last 60 Samples) ") | bold | color(Color::RGB(130, 200, 255)),
            filler(),
            text("Peak: " + [&] {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << max_sample << "ms";
                return ss.str();
            }() + " │ Min: " + [&] {
                std::ostringstream ss;
                ss << std::fixed << std::setprecision(1) << min_sample << "ms";
                return ss.str();
            }()) | color(Color::Grey70),
        }),
        separatorLight(),
        graph(graph_fn) | color(Color::RGB(85, 225, 145)) | size(HEIGHT, EQUAL, 8) | borderRounded,
    });

    // 2. Statistics Cards
    auto rtt_dist = compute_distribution(m_rtt_samples);
    auto card_rtt = vbox({
        text(" LATENCY PERCENTILES ") | bold | color(Color::RGB(130, 200, 255)),
        separatorLight(),
        text(" Minimum:  " + std::to_string(static_cast<int>(rtt_dist.min_val)) + " ms") | color(Color::White),
        text(" Median:   " + std::to_string(static_cast<int>(rtt_dist.median_val)) + " ms") | bold | color(Color::RGB(85, 225, 145)),
        text(" 95th Pct: " + std::to_string(static_cast<int>(rtt_dist.p95_val)) + " ms") | color(Color::RGB(255, 195, 75)),
        text(" Maximum:  " + std::to_string(static_cast<int>(rtt_dist.max_val)) + " ms") | color(Color::RGB(255, 100, 100)),
        text(" Jitter:   ±" + [&] {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1) << m_last_jitter << " ms";
            return ss.str();
        }()) | color(Color::Grey70),
    }) | borderRounded | flex;

    auto saved_dist = compute_distribution(m_delay_saved_samples);
    double percent_mit = (m_total_actions > 0) ? (static_cast<double>(m_actions_mitigated) * 100.0 / m_total_actions) : 0.0;
    auto card_savings = vbox({
        text(" TIME SAVED SUMMARY ") | bold | color(Color::RGB(130, 200, 255)),
        separatorLight(),
        text(" Total Saved:   " + [&] {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(2) << (m_cumulative_time_saved_ms / 1000.0) << " s";
            return ss.str();
        }()) | bold | color(Color::RGB(85, 225, 145)),
        text(" Average Saved: " + [&] {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1)
               << (m_actions_mitigated > 0 ? (m_cumulative_time_saved_ms / m_actions_mitigated) : 0.0) << " ms";
            return ss.str();
        }()) | color(Color::White),
        text(" P95 Saved:     " + std::to_string(static_cast<int>(saved_dist.p95_val)) + " ms") | color(Color::Grey70),
        text(" Mitigated:     " + [&] {
            std::ostringstream ss;
            ss << std::fixed << std::setprecision(1) << percent_mit << "%";
            return ss.str();
        }()) | color(Color::RGB(85, 225, 145)),
        text(" Action Count:  " + std::to_string(m_total_actions)) | color(Color::Grey70),
    }) | borderRounded | flex;

    auto card_guards = vbox({
        text(" SAFETY & GUARDS ") | bold | color(Color::RGB(130, 200, 255)),
        separatorLight(),
        text(" Floor Clamps:      " + std::to_string(m_guards.floor_clamps)) | color(m_guards.floor_clamps > 0 ? Color::RGB(255, 195, 75) : Color::White),
        text(" Spike Filtered:    " + std::to_string(m_guards.spike_filtered)) | color(m_guards.spike_filtered > 0 ? Color::RGB(255, 100, 100) : Color::White),
        text(" Cold Start Guard:  " + std::to_string(m_guards.cold_start_guards)) | color(Color::White),
        text(" Cast Lock Kept:    " + std::to_string(m_guards.cast_locks_preserved)) | color(Color::RGB(130, 200, 255)),
        text(" Mode:              " + std::string(m_dry_run ? "DRY-RUN" : "ACTIVE")) | bold | color(m_dry_run ? Color::RGB(190, 140, 255) : Color::RGB(85, 225, 145)),
    }) | borderRounded | flex;

    return vbox({
        graph_view,
        hbox({ card_rtt, card_savings, card_guards }),
    });
}

Element UiRenderer::build_dashboard_document() const {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    Element hero = build_hero_banner();
    Element body;
    if (m_active_tab == 0) {
        body = build_tab_live_combat();
    } else if (m_active_tab == 1) {
        body = build_tab_latency_analytics();
    } else {
        body = vbox({
            text(" INTERACTIVE SETTINGS & CONFIGURATION ") | bold | color(Color::RGB(130, 200, 255)),
            separatorLight(),
            text(" Min Animation Lock Floor: " + std::to_string(m_slider_min_lock) + " ms") | bold,
            text(" Target Simulated Ping:    " + std::to_string(m_slider_target_ping) + " ms"),
            text(" Conservative Margin:      " + std::to_string(m_slider_margin) + " ms"),
            text(" Dry-Run Mode:             " + std::string(m_config.dry_run ? "ENABLED (No memory edits)" : "DISABLED (Live mitigation)")),
            text(" Verbose Logging:          " + std::string(m_config.verbose ? "ENABLED" : "DISABLED")),
            separatorLight(),
            text(m_settings_feedback.empty() ? " Use arrow keys to adjust sliders. Changes save to mitigator_config.json." : m_settings_feedback) | color(Color::RGB(85, 225, 145)),
        });
    }

    // Bottom Navigation Bar
    std::string tab0_label = (m_active_tab == 0) ? "[1] LIVE COMBAT ●" : " [1] Live Combat ";
    std::string tab1_label = (m_active_tab == 1) ? "[2] LATENCY ANALYTICS ●" : " [2] Latency Analytics ";
    std::string tab2_label = (m_active_tab == 2) ? "[3] SETTINGS & SAFETY ●" : " [3] Settings & Safety ";

    auto nav_bar = hbox({
        text(" ") | color(Color::Grey50),
        text(tab0_label) | (m_active_tab == 0 ? (bold | color(Color::RGB(130, 200, 255))) : color(Color::Grey70)),
        text(" │ ") | color(Color::Grey50),
        text(tab1_label) | (m_active_tab == 1 ? (bold | color(Color::RGB(130, 200, 255))) : color(Color::Grey70)),
        text(" │ ") | color(Color::Grey50),
        text(tab2_label) | (m_active_tab == 2 ? (bold | color(Color::RGB(130, 200, 255))) : color(Color::Grey70)),
        filler(),
        text("Hotkeys: [1-3] Switch Tab  [D] Dry-Run  [C] Clear  [Q] Exit ") | color(Color::Grey50),
    });

    return vbox({
        hero,
        separatorLight(),
        body | flex,
        separatorLight(),
        nav_bar,
    }) | borderRounded;
}

std::string UiRenderer::render_snapshot_to_string(int width, int height) const {
    auto document = build_dashboard_document();
    auto screen = Screen::Create(
        Dimension::Fixed(std::max(width, 78)),
        Dimension::Fixed(std::max(height, 20))
    );
    Render(screen, document);
    return screen.ToString();
}

Component UiRenderer::create_interactive_component() {
    static const std::vector<std::string> tab_entries = {
        "[1] Live Combat",
        "[2] Latency Analytics",
        "[3] Settings & Safety",
    };
    auto tab_toggle = Toggle(&tab_entries, &m_active_tab);

    auto slider_min_lock = Slider("Safety Floor (ms)", &m_slider_min_lock, 20, 100, 1);
    auto slider_target_ping = Slider("Target Ping (ms)", &m_slider_target_ping, 0, 50, 1);
    auto slider_margin = Slider("Safety Margin (ms)", &m_slider_margin, 0, 30, 1);
    auto checkbox_dry = Checkbox("Dry-Run Mode (Simulation only, no memory edits)", &m_config.dry_run);
    auto checkbox_verb = Checkbox("Verbose Diagnostic Logging", &m_config.verbose);

    auto btn_save = Button("Save Configuration to mitigator_config.json", [this] {
        std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
        m_config.min_animation_lock_ms = static_cast<double>(m_slider_min_lock);
        m_config.target_ping_ms = static_cast<double>(m_slider_target_ping);
        m_config.safety_margin_ms = static_cast<double>(m_slider_margin);
        m_dry_run = m_config.dry_run;
        m_target_ping_ms = m_config.target_ping_ms;
        bool ok = ConfigManager::save_to_file(ConfigManager::DEFAULT_CONFIG_FILENAME, m_config);
        m_settings_feedback = ok ? "✔ Successfully saved configuration to mitigator_config.json!" : "✖ Error writing configuration file.";
        if (m_on_config_changed) m_on_config_changed(m_config);
        m_dirty = true;
    });

    auto btn_reset_stats = Button("Reset Statistics Counters", [this] {
        reset_stats();
        if (m_on_reset_stats) m_on_reset_stats();
    });

    auto btn_restore_defaults = Button("Restore Safe Defaults", [this] {
        std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
        m_config = ConfigManager::default_config();
        m_slider_min_lock = static_cast<int>(m_config.min_animation_lock_ms);
        m_slider_target_ping = static_cast<int>(m_config.target_ping_ms);
        m_slider_margin = static_cast<int>(m_config.safety_margin_ms);
        m_dry_run = m_config.dry_run;
        m_target_ping_ms = m_config.target_ping_ms;
        m_settings_feedback = "Restored safe default values.";
        if (m_on_config_changed) m_on_config_changed(m_config);
        m_dirty = true;
    });

    auto settings_container = Container::Vertical({
        slider_min_lock,
        slider_target_ping,
        slider_margin,
        checkbox_dry,
        checkbox_verb,
        Container::Horizontal({ btn_save, btn_reset_stats, btn_restore_defaults }),
    });

    auto tab_container = Container::Tab({
        Renderer([this] { return build_tab_live_combat(); }),
        Renderer([this] { return build_tab_latency_analytics(); }),
        Renderer(settings_container, [this, settings_container] {
            return vbox({
                text(" INTERACTIVE SETTINGS & CONFIGURATION ") | bold | color(Color::RGB(130, 200, 255)),
                separatorLight(),
                settings_container->Render(),
                separatorLight(),
                text(m_settings_feedback.empty() ? " Changes can be saved to mitigator_config.json to persist across sessions." : m_settings_feedback) | color(Color::RGB(85, 225, 145)),
            });
        }),
    }, &m_active_tab);

    auto main_container = Container::Vertical({
        tab_toggle,
        tab_container,
    });

    return Renderer(main_container, [this, tab_toggle, tab_container] {
        std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
        return vbox({
            build_hero_banner(),
            separatorLight(),
            tab_container->Render() | flex,
            separatorLight(),
            hbox({
                tab_toggle->Render(),
                filler(),
                text("Hotkeys: [1-3] Switch Tab  [D] Dry-Run  [C] Clear  [Q] Exit ") | color(Color::Grey50),
            }),
        }) | borderRounded;
    });
}

void UiRenderer::render_dashboard(bool dry_run, bool /*verbose*/) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
        m_dry_run = dry_run;
    }
    std::cout << "\033[H\033[?25l" << render_snapshot_to_string(static_cast<int>(m_cols), static_cast<int>(m_rows)) << "\033[J" << std::flush;
}

void UiRenderer::render_stats_summary() {
    // Handled reactively by FTXUI
}

void UiRenderer::render_hotkey_bar(bool /*dry_run*/, bool /*verbose*/) {
    // Handled reactively in bottom dock
}

void UiRenderer::render_final_report() {
    std::lock_guard<std::recursive_mutex> lock(m_render_mutex);
    std::cout << "\033[?25h\n";

    auto rtt_dist = compute_distribution(m_rtt_samples);
    double percent_mit = (m_total_actions > 0) ? (static_cast<double>(m_actions_mitigated) * 100.0 / m_total_actions) : 0.0;
    auto up = uptime();

    std::cout << "==================================================================================================\n";
    std::cout << "                  FINAL COMBAT SESSION REPORT — FFXIV LATENCY MITIGATOR                           \n";
    std::cout << "==================================================================================================\n";
    std::cout << "  Session Duration:    " << format_time_hhmmss(up) << "\n";
    std::cout << "  Total Actions:       " << m_total_actions << "\n";
    std::cout << "  Actions Mitigated:   " << m_actions_mitigated << " (" << std::fixed << std::setprecision(1) << percent_mit << "%)\n";
    std::cout << "  Total Time Saved:    " << std::fixed << std::setprecision(2) << (m_cumulative_time_saved_ms / 1000.0) << " seconds\n";
    std::cout << "  Avg Delay Saved:     " << (m_actions_mitigated > 0 ? (m_cumulative_time_saved_ms / m_actions_mitigated) : 0.0) << " ms\n";
    std::cout << "  RTT (Min/Med/P95):   " << rtt_dist.min_val << " / " << rtt_dist.median_val << " / " << rtt_dist.p95_val << " ms\n";
    std::cout << "  Safety Floor Clamps: " << m_guards.floor_clamps << "\n";
    std::cout << "  Spikes Filtered:     " << m_guards.spike_filtered << "\n";
    std::cout << "  Cast Locks Kept:     " << m_guards.cast_locks_preserved << "\n";
    std::cout << "==================================================================================================\n" << std::flush;
}

// -----------------------------------------------------------------------------
// Utilities
// -----------------------------------------------------------------------------

size_t UiRenderer::visible_width(std::string_view s) {
    size_t width = 0;
    bool in_escape = false;
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (in_escape) {
            if (c == 'm' || c == 'H' || c == 'J' || c == 'K' || c == 'h' || c == 'l') {
                in_escape = false;
            }
            continue;
        }
        if (c == '\033') {
            in_escape = true;
            continue;
        }
        if ((c & 0xC0) == 0x80) continue; // UTF-8 continuation
        if ((c & 0xF8) == 0xF0) { // 4-byte UTF-8 emoji
            width += 2;
            continue;
        }
        if (c == 0xE2) { // Special UTF-8 symbol (e.g. ⚡, ⚠)
            if (i + 2 < s.size()) {
                unsigned char c2 = static_cast<unsigned char>(s[i + 1]);
                unsigned char c3 = static_cast<unsigned char>(s[i + 2]);
                if (c2 == 0x9A && (c3 == 0xA1 || c3 == 0xA0)) {
                    width += 2;
                    i += 2;
                    continue;
                }
            }
        }
        width++;
    }
    return width;
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
    for (size_t i = 0; i < filled; ++i) out += "\u2588";
    out += "\033[0m";
    for (size_t i = filled; i < bar_width; ++i) out += "\u2591";
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
    ss << entry.timestamp_str << " | #" << entry.sequence << " | 0x" << std::hex << entry.action_id << std::dec;
    return ss.str();
}

} // namespace mitigator::loader
