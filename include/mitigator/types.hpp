#pragma once

#include <cstdint>
#include <chrono>
#include <string>

namespace mitigator {

using ActionId = uint32_t;
using SequenceId = uint32_t;
using TimePoint = std::chrono::steady_clock::time_point;
using Milliseconds = std::chrono::duration<double, std::milli>;

/// Named constants and domain defaults across the mitigation engine
namespace constants {
    /// Milliseconds in one second
    constexpr double MS_PER_SECOND = 1000.0;
    /// Seconds in one millisecond
    constexpr double SECONDS_PER_MS = 0.001;

    /// Default simulated target ping in milliseconds
    constexpr double DEFAULT_TARGET_PING_MS = 15.0;
    /// Default safety floor for animation lock in milliseconds
    constexpr double DEFAULT_MIN_ANIMATION_LOCK_MS = 25.0;
    /// Default sanity upper ceiling for animation lock in milliseconds
    constexpr double DEFAULT_MAX_ANIMATION_LOCK_MS = 2500.0;
    /// Default window size for rolling RTT sample queue
    constexpr size_t DEFAULT_RTT_SAMPLE_WINDOW = 10;
    /// Default initial estimate for RTT in milliseconds
    constexpr double DEFAULT_INITIAL_RTT_MS = 50.0;
    /// Default conservative safety margin buffer in milliseconds
    constexpr double DEFAULT_SAFETY_MARGIN_MS = 0.0;

    /// Minimum plausible RTT sample in milliseconds (filters 0 or negative measurements)
    constexpr double MIN_PLAUSIBLE_RTT_MS = 0.5;
    /// Maximum plausible RTT sample in milliseconds (filters anomalous network disconnects)
    constexpr double MAX_PLAUSIBLE_RTT_MS = 5000.0;

    /// Default timeout for pruning stale action requests
    constexpr auto DEFAULT_STALE_TIMEOUT = std::chrono::milliseconds(5000);

    /// Absolute hard anti-cheat safety limit: never allow setting animation lock floor below this value
    constexpr double ABSOLUTE_MIN_ANIMATION_LOCK_FLOOR_MS = 20.0;

    /// Grace window in seconds after cast completion to allow server packet ack
    constexpr float CAST_COMPLETION_GRACE_WINDOW_SECONDS = 0.1f;
    /// Base buffer added to dynamic cast grace window in seconds
    constexpr float CAST_GRACE_BASE_BUFFER_SECONDS = 0.050f;
    /// Ratio of RTT representing one-way client-to-server or server-to-client latency
    constexpr double ONE_WAY_LATENCY_RATIO = 0.5;

    /// Minimum samples needed before activating moving median spike rejection
    constexpr size_t MIN_SAMPLES_FOR_MEDIAN_FILTER = 3;
    /// Minimum latency deviation tolerance in ms before considering an RTT spike an outlier
    constexpr double MIN_OUTLIER_TOLERANCE_MS = 50.0;
    /// Multiplier on measured jitter to calculate outlier rejection threshold
    constexpr double JITTER_SPIKE_MULTIPLIER = 3.0;

    /// Maximum duration in milliseconds to wait for in-flight detours to drain during unhooking
    constexpr uint32_t HOOK_DRAIN_TIMEOUT_MS = 2000;
    /// Polling sleep interval in milliseconds while waiting for detours to drain
    constexpr uint32_t HOOK_DRAIN_POLL_INTERVAL_MS = 10;
}

/// Configuration parameters for latency mitigation.
struct MitigationConfig {
    /// Simulated target ping (RTT) in milliseconds (e.g. 10.0 - 20.0ms).
    double target_ping_ms{constants::DEFAULT_TARGET_PING_MS};

    /// Hard safety floor for animation lock in milliseconds (25.0 - 40.0ms).
    /// Server anomaly detection guards prevent setting animation lock to 0.
    double min_animation_lock_ms{constants::DEFAULT_MIN_ANIMATION_LOCK_MS};

    /// Sanity upper bound for animation lock in milliseconds.
    double max_animation_lock_ms{constants::DEFAULT_MAX_ANIMATION_LOCK_MS};

    /// Number of samples to retain for rolling RTT smoothing.
    size_t rtt_sample_window{constants::DEFAULT_RTT_SAMPLE_WINDOW};

    /// If true, calculate and log telemetry without modifying game memory.
    bool dry_run{false};

    /// If true, emit detailed diagnostics for every action event.
    bool verbose{false};

    /// Extra buffer added to RTT adjustment to compensate for local frame pacing.
    double safety_margin_ms{constants::DEFAULT_SAFETY_MARGIN_MS};
};

/// Information recorded when an action is requested by the client.
struct ActionRequestInfo {
    ActionId action_id{0};
    SequenceId sequence{0};
    TimePoint timestamp{std::chrono::steady_clock::now()};
    bool is_cast{false};
    float cast_duration_seconds{0.0f};
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
