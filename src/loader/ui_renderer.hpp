#pragma once

#include "mitigator/ipc_protocol.hpp"
#include "mitigator/config_manager.hpp"

#include <string>
#include <string_view>
#include <mutex>
#include <cstdint>
#include <deque>
#include <vector>
#include <chrono>
#include <functional>

namespace mitigator::loader {

/**
 * @brief Record representing an individual action telemetry event in the rolling buffer.
 */
struct ActionLogEntry {
    uint64_t index{0};
    uint32_t action_id{0};
    uint32_t sequence{0};
    float original_lock_ms{0.0f};
    float adjusted_lock_ms{0.0f};
    float delay_reduced_ms{0.0f};
    float measured_rtt_ms{0.0f};
    float smoothed_rtt_ms{0.0f};
    float jitter_ms{0.0f};
    bool clamped_floor{false};
    bool dry_run{false};
    bool applied{false};
    bool cast_active{false};
    bool spike_filtered{false};
    bool cold_start_guard{false};
    std::string timestamp_str; // HH:MM:SS
};

/**
 * @brief Statistical summary of latency or delay reduction percentiles.
 */
struct LatencyDistribution {
    float min_val{0.0f};
    float median_val{0.0f};
    float p95_val{0.0f};
    float max_val{0.0f};
};

/**
 * @brief Cumulative counters for safety guard and anomaly filter activations.
 */
struct GuardCounters {
    uint64_t floor_clamps{0};
    uint64_t spike_filtered{0};
    uint64_t cold_start_guards{0};
    uint64_t cast_locks_preserved{0};
};

/**
 * @brief High-performance, zero-dependency ANSI/TrueColor terminal dashboard and telemetry renderer.
 *
 * Provides:
 * - Tab 1: Live Combat Stream, KPI gauges, and hero status banner.
 * - Tab 2: Latency Analytics with real-time 60-sample Unicode sparkline waveform graph.
 * - Tab 3: Interactive Settings & Safety Controls (persisted to disk).
 */
class UiRenderer {
public:
    static constexpr size_t DEFAULT_DASHBOARD_WIDTH = 100;
    static constexpr size_t MIN_DASHBOARD_WIDTH = 78;
    static constexpr size_t DEFAULT_DASHBOARD_ROWS = 30;
    static constexpr size_t MIN_DASHBOARD_ROWS = 24;
    static constexpr size_t DASHBOARD_WIDTH = 78;
    static constexpr size_t INNER_WIDTH = DASHBOARD_WIDTH - 2;
    static constexpr size_t RING_BUFFER_CAPACITY = 24;
    static constexpr size_t DASHBOARD_DISPLAY_ROWS = 6;
    static constexpr size_t SPARKLINE_HISTORY_CAPACITY = 60;
    static constexpr size_t MAX_DISTRIBUTION_SAMPLES = 1000;

    UiRenderer();

    /// Configures session metadata for dashboard rendering.
    void set_session_info(uint32_t pid, uint32_t hook_count, double target_ping_ms, bool dry_run);

    /// Sets the dynamic connection/standby status message displayed on the dashboard.
    void set_connection_status(const std::string& status);

    /// Gets the current connection status message.
    [[nodiscard]] std::string connection_status() const;

    /// Updates the terminal window dimensions for responsive layout rendering.
    void set_terminal_dimensions(size_t cols, size_t rows);

    /// Gets current terminal width in columns.
    [[nodiscard]] size_t terminal_cols() const;

    /// Gets current terminal height in rows.
    [[nodiscard]] size_t terminal_rows() const;

    /// Gets the calculated number of action rows displayed in the live combat feed.
    [[nodiscard]] size_t dashboard_display_rows() const;

    /// Enables or disables in-place split-screen dashboard mode.
    void set_dashboard_mode(bool enabled);

    /// Returns true if dashboard mode is currently enabled.
    [[nodiscard]] bool is_dashboard_mode() const;

    /// Checks if new telemetry has arrived since the last dashboard render.
    [[nodiscard]] bool needs_redraw() const;

    /// Renders the startup header and game connection banner.
    void render_header(uint32_t pid, uint32_t hook_count, double target_ping_ms, bool dry_run);

    /// Records an incoming action telemetry event into metrics and ring buffer.
    void log_action(const ipc::TelemetryPayload& t, bool verbose);

    /// Prints a system status / notification message.
    void log_status(const std::string& message, bool is_error = false);

    /// Renders the complete live split-screen dashboard in-place to stdout.
    void render_dashboard(bool dry_run, bool verbose);

    /// Updates and renders the telemetry stats summary card.
    void render_stats_summary();

    /// Renders available hotkeys.
    void render_hotkey_bar(bool dry_run, bool verbose);

    /// Prints a persistent ASCII session summary report card upon shutdown.
    void render_final_report();

    /// Resets all accumulated session stats, distributions, and ring buffer.
    void reset_stats();

    // -------------------------------------------------------------------------
    // Dashboard Tab & Snapshot Methods
    // -------------------------------------------------------------------------

    /// Renders the current dashboard to a string buffer for tests & headless captures
    [[nodiscard]] std::string render_snapshot_to_string(int width = 100, int height = 30) const;

    // Tab selection management
    [[nodiscard]] int active_tab() const;
    void set_active_tab(int tab_index);
    void cycle_tab(int delta = 1);

    // Configuration management
    [[nodiscard]] MitigationConfig config() const;
    void set_config(const MitigationConfig& config);
    void set_on_config_changed(std::function<void(const MitigationConfig&)> callback);
    void set_on_reset_stats_callback(std::function<void()> callback);

    // Latency sparkline history
    [[nodiscard]] std::vector<float> rtt_history() const;

    // Inspection getters for unit testing and diagnostics
    [[nodiscard]] uint64_t total_actions() const;
    [[nodiscard]] uint64_t actions_mitigated() const;
    [[nodiscard]] double cumulative_time_saved_ms() const;
    [[nodiscard]] float last_smoothed_rtt() const;
    [[nodiscard]] float last_jitter() const;
    [[nodiscard]] LatencyDistribution rtt_distribution() const;
    [[nodiscard]] LatencyDistribution delay_saved_distribution() const;
    [[nodiscard]] GuardCounters guard_counters() const;
    [[nodiscard]] double calculate_apm(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) const;
    [[nodiscard]] size_t ring_buffer_size() const;
    [[nodiscard]] std::chrono::seconds uptime() const;

    // Static formatting and utility helpers
    [[nodiscard]] static size_t visible_width(std::string_view s);
    [[nodiscard]] static LatencyDistribution compute_distribution(const std::vector<float>& samples);
    [[nodiscard]] static LatencyDistribution compute_distribution(const std::deque<float>& samples);
    [[nodiscard]] static std::string make_bar(float value, float max_val, size_t bar_width, const char* bar_color);
    [[nodiscard]] static std::string format_time_hhmmss(std::chrono::seconds total_secs);
    [[nodiscard]] static std::string current_time_hhmmss();
    [[nodiscard]] static std::string format_action_line(const ActionLogEntry& entry);
    [[nodiscard]] static std::string render_sparkline_bar(const std::vector<float>& samples, size_t width);

private:
    void record_action_internal(const ipc::TelemetryPayload& t);

    mutable std::recursive_mutex m_render_mutex;

    uint64_t m_total_actions{0};
    uint64_t m_actions_mitigated{0};
    double m_cumulative_time_saved_ms{0.0};
    float m_last_smoothed_rtt{0.0f};
    float m_last_jitter{0.0f};

    GuardCounters m_guards{};
    std::deque<float> m_rtt_samples;
    std::deque<float> m_delay_saved_samples;
    std::deque<ActionLogEntry> m_action_ring_buffer;
    mutable std::deque<std::chrono::steady_clock::time_point> m_recent_action_times;
    std::deque<float> m_rtt_history;

    std::chrono::steady_clock::time_point m_session_start_time;
    uint32_t m_pid{0};
    uint32_t m_hook_count{0};
    double m_target_ping_ms{15.0};
    bool m_dry_run{false};
    bool m_dashboard_mode{false};
    mutable bool m_dirty{true};
    std::string m_connection_status{"Waiting for game to launch..."};
    size_t m_cols{DEFAULT_DASHBOARD_WIDTH};
    size_t m_rows{DEFAULT_DASHBOARD_ROWS};

    // Tab state
    int m_active_tab{0};
    MitigationConfig m_config{ConfigManager::default_config()};
    std::function<void(const MitigationConfig&)> m_on_config_changed;
    std::function<void()> m_on_reset_stats;
    std::string m_settings_feedback;
};

} // namespace mitigator::loader

