#pragma once

#include <cstdint>
#include <chrono>
#include <string>

namespace mitigator {

using ActionId = uint32_t;
using SequenceId = uint32_t;
using TimePoint = std::chrono::steady_clock::time_point;
using Milliseconds = std::chrono::duration<double, std::milli>;

/// Configuration parameters for latency mitigation.
struct MitigationConfig {
    /// Simulated target ping (RTT) in milliseconds (e.g. 10.0 - 20.0ms).
    double target_ping_ms{15.0};

    /// Hard safety floor for animation lock in milliseconds (25.0 - 40.0ms).
    /// Server anomaly detection guards prevent setting animation lock to 0.
    double min_animation_lock_ms{25.0};

    /// Sanity upper bound for animation lock in milliseconds.
    double max_animation_lock_ms{2500.0};

    /// Number of samples to retain for rolling RTT smoothing.
    size_t rtt_sample_window{10};

    /// If true, calculate and log telemetry without modifying game memory.
    bool dry_run{false};

    /// If true, emit detailed diagnostics for every action event.
    bool verbose{false};

    /// Extra buffer added to RTT adjustment to compensate for local frame pacing.
    double safety_margin_ms{0.0};
};

/// Information recorded when an action is requested by the client.
struct ActionRequestInfo {
    ActionId action_id{0};
    SequenceId sequence{0};
    TimePoint timestamp{std::chrono::steady_clock::now()};
};

/// Detailed outcome of an animation lock mitigation calculation.
struct MitigationResult {
    ActionId action_id{0};
    SequenceId sequence{0};

    /// Original animation lock sent by server / client in milliseconds.
    double original_lock_ms{0.0};

    /// Adjusted animation lock in milliseconds after mitigation.
    double adjusted_lock_ms{0.0};

    /// Net time saved in milliseconds (original - adjusted).
    double delay_reduced_ms{0.0};

    /// Measured RTT for this specific action in milliseconds.
    double measured_rtt_ms{0.0};

    /// Current smoothed RTT estimate in milliseconds.
    double smoothed_rtt_ms{0.0};

    /// Whether the calculated lock was clamped by min_animation_lock_ms.
    bool clamped_by_floor{false};

    /// Whether the lock was clamped by max_animation_lock_ms.
    bool clamped_by_ceiling{false};

    /// Whether the mitigation was applied to game memory (false in dry-run mode).
    bool applied{false};

    /// Whether this action was executed during an active cast.
    bool cast_active{false};
};

/// Real-time session metrics and telemetry summary.
struct SessionStats {
    uint64_t total_actions_requested{0};
    uint64_t total_actions_mitigated{0};
    double cumulative_time_saved_ms{0.0};
    double current_smoothed_rtt_ms{0.0};
    double current_jitter_ms{0.0};
    uint64_t total_floor_clamps{0};
};

} // namespace mitigator
