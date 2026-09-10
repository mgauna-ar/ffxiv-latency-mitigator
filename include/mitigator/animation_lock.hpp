#pragma once

#include "mitigator/types.hpp"
#include "mitigator/rolling_rtt.hpp"
#include "mitigator/sequence_tracker.hpp"
#include "mitigator/cast_tracker.hpp"
#include <mutex>

namespace mitigator {

/**
 * @brief Core adaptive latency mitigation engine.
 *
 * Implements adaptive ping smoothing: calculates network RTT, offsets server
 * animation lock delays to simulate low-ping play (~10-20ms), and applies
 * safety clamping floors to prevent anti-cheat / server anomaly triggers.
 */
class AnimationLockMitigator {
public:
    explicit AnimationLockMitigator(const MitigationConfig& config = {});

    /// Registers an outgoing action request from the client.
    void record_action_request(
        ActionId action_id,
        SequenceId sequence,
        TimePoint timestamp = std::chrono::steady_clock::now(),
        bool is_cast = false,
        float cast_duration_seconds = 0.0f,
        bool is_queued = false
    );

    /**
     * @brief Computes the adjusted animation lock when a server action effect arrives.
     * @param action_id Game action identifier.
     * @param sequence Sequence number from server effect header.
     * @param original_lock_ms Animation lock duration received from the game/server.
     * @param now Current timestamp.
     * @return MitigationResult containing adjusted lock, time saved, and telemetry.
     */
    [[nodiscard]] MitigationResult calculate_mitigation(
        ActionId action_id,
        SequenceId sequence,
        double original_lock_ms,
        TimePoint now = std::chrono::steady_clock::now()
    );

    /// Records the initiation of a casted ability.
    void record_cast_begin(ActionId action_id, float cast_time_seconds, TimePoint now = std::chrono::steady_clock::now());

    /// Records interruption of an active cast.
    void record_cast_interrupt(TimePoint now = std::chrono::steady_clock::now());

    /// Records completion of an active cast.
    void record_cast_end(TimePoint now = std::chrono::steady_clock::now());

    /// Returns whether the player is currently considered casting.
    [[nodiscard]] bool is_casting(TimePoint now = std::chrono::steady_clock::now()) const;

    /// Returns current configuration copy.
    [[nodiscard]] MitigationConfig get_config() const;

    /// Updates configuration.
    void set_config(const MitigationConfig& config);

    /// Sets dry-run mode.
    void set_dry_run(bool dry_run);

    /// Sets simulated target ping in milliseconds.
    void set_target_ping_ms(double target_ping_ms);

    /// Sets hard minimum animation lock floor in milliseconds.
    void set_min_animation_lock_ms(double min_lock_ms);

    /// Returns cumulative session statistics.
    [[nodiscard]] SessionStats get_session_stats() const;

    /// Resets all stats and tracking state.
    void reset();

    /// Direct access to rolling RTT tracker for inspections.
    [[nodiscard]] const RollingRttTracker& rtt_tracker() const { return m_rtt_tracker; }
    [[nodiscard]] const SequenceTracker& sequence_tracker() const { return m_seq_tracker; }
    [[nodiscard]] const CastTracker& cast_tracker() const { return m_cast_tracker; }
    enum class OutlierDirection { None, Up, Down };

    /// Returns consecutive outlier sample count in the current outlier direction.
    [[nodiscard]] size_t consecutive_outliers() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_consecutive_outliers;
    }

    /// Returns the current active outlier direction.
    [[nodiscard]] OutlierDirection outlier_direction() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_outlier_direction;
    }

private:
    mutable std::mutex m_mutex;
    MitigationConfig m_config;
    RollingRttTracker m_rtt_tracker;
    SequenceTracker m_seq_tracker;
    CastTracker m_cast_tracker;

    // Outlier shift detection
    OutlierDirection m_outlier_direction{OutlierDirection::None};
    size_t m_consecutive_outliers{0};

    // Session telemetry counters
    uint64_t m_total_actions_requested{0};
    uint64_t m_total_actions_mitigated{0};
    double m_cumulative_time_saved_ms{0.0};
    uint64_t m_total_floor_clamps{0};
};

} // namespace mitigator
