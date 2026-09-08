#pragma once

#include "mitigator/ipc_protocol.hpp"
#include <string>
#include <mutex>
#include <cstdint>

namespace mitigator::loader {

/**
 * @brief Thread-safe terminal dashboard renderer.
 *
 * Displays real-time telemetry metrics, action mitigation logs, and
 * hotkey controls in the console window.
 */
class UiRenderer {
public:
    UiRenderer();

    /// Renders the startup header and game connection banner.
    void render_header(uint32_t pid, uint32_t hook_count, double target_ping_ms, bool dry_run);

    /// Formats and prints an incoming action telemetry event.
    void log_action(const ipc::TelemetryPayload& t, bool verbose);

    /// Prints a system status / notification message.
    void log_status(const std::string& message, bool is_error = false);

    /// Updates and renders the bottom telemetry stats summary.
    void render_stats_summary();

    /// Renders available hotkeys.
    void render_hotkey_bar(bool dry_run, bool verbose);

    /// Resets all accumulated session stats.
    void reset_stats();

private:
    std::mutex m_render_mutex;
    uint64_t m_total_actions{0};
    uint64_t m_actions_mitigated{0};
    double m_cumulative_time_saved_ms{0.0};
    float m_last_smoothed_rtt{0.0f};
    float m_last_jitter{0.0f};
};

} // namespace mitigator::loader
