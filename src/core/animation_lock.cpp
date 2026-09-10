#include "mitigator/animation_lock.hpp"
#include <algorithm>
#include <cmath>

namespace mitigator {

AnimationLockMitigator::AnimationLockMitigator(const MitigationConfig& config)
    : m_config(config),
      m_rtt_tracker(config.rtt_sample_window, config.target_ping_ms > 0 ? config.target_ping_ms * 3.0 : 50.0) {
    // Enforce absolute anti-cheat safety floor of at least 20.0ms
    m_config.min_animation_lock_ms = std::max(20.0, m_config.min_animation_lock_ms);
}

void AnimationLockMitigator::record_action_request(
    ActionId action_id,
    SequenceId sequence,
    TimePoint timestamp,
    bool is_cast,
    float cast_duration_seconds,
    bool is_queued
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_total_actions_requested;
    m_seq_tracker.record_request(action_id, sequence, timestamp, is_cast, cast_duration_seconds, is_queued);
}

MitigationResult AnimationLockMitigator::calculate_mitigation(
    ActionId action_id,
    SequenceId sequence,
    double original_lock_ms,
    TimePoint now
) {
    std::lock_guard<std::mutex> lock(m_mutex);

    MitigationResult res{};
    res.action_id = action_id;
    res.sequence = sequence;
    res.original_lock_ms = original_lock_ms;

    // 1. Try to correlate with recorded outgoing action request
    const auto matched_req = m_seq_tracker.match_response(action_id, sequence, now);
    if (!matched_req.has_value()) {
        // Untracked server effect (party member, enemy, or zone-wide effect):
        // Safely pass through without modifying game memory to prevent lock corruption.
        res.adjusted_lock_ms = std::isfinite(original_lock_ms) ? original_lock_ms : 0.0;
        res.delay_reduced_ms = 0.0;
        res.applied = false;
        res.measured_rtt_ms = 0.0;
        res.smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms();
        return res;
    }

    if (!std::isfinite(original_lock_ms)) {
        if (matched_req->is_cast) {
            m_cast_tracker.on_cast_end(now);
        }
        res.cast_active = matched_req->is_cast || m_cast_tracker.is_casting(now, m_rtt_tracker.get_smoothed_rtt_ms());
        res.adjusted_lock_ms = 0.0;
        res.delay_reduced_ms = 0.0;
        res.applied = false;
        res.measured_rtt_ms = 0.0;
        res.smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms();
        return res;
    }

    res.queued_action = matched_req->is_queued;

    // Check if active cast is in progress or if this effect corresponds to a casted action
    const bool is_cast_effect = matched_req->is_cast;
    const bool is_currently_casting = m_cast_tracker.is_casting(now, m_rtt_tracker.get_smoothed_rtt_ms());
    res.cast_active = is_cast_effect || is_currently_casting;

    // If casting is active or this effect was for a casted spell, preserve cast lock
    // (e.g. caster tax / slide-cast duration). Do NOT reduce animation lock, and do NOT
    // sample RTT (elapsed includes cast duration, which would corrupt ping tracking).
    if (res.cast_active) {
        if (is_cast_effect) {
            m_cast_tracker.on_cast_end(now);
        }
        res.adjusted_lock_ms = original_lock_ms;
        res.delay_reduced_ms = 0.0;
        res.applied = false;
        res.measured_rtt_ms = 0.0;
        res.smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms();
        return res;
    }

    const double baseline_rtt = m_rtt_tracker.get_smoothed_rtt_ms();
    const size_t samples_before = m_rtt_tracker.sample_count();
    const bool is_cold_start = (samples_before < constants::MIN_SAMPLES_FOR_MEDIAN_FILTER);

    double measured_rtt = 0.0;
    const auto elapsed = std::chrono::duration_cast<Milliseconds>(
        now - matched_req->timestamp
    ).count();

    double effective_rtt = baseline_rtt;

    if (matched_req->is_queued) {
        res.queued_action = true;
        effective_rtt = baseline_rtt;
        if (elapsed > 0.0 && elapsed < constants::MAX_PLAUSIBLE_RTT_MS) {
            measured_rtt = elapsed;
            // Queued actions: if measured_rtt is a clean sample (not delayed by queue dwell time),
            // or if the tracker is completely cold (0 samples), ingest it so the tracker is never starved.
            // Otherwise, do not ingest the queue dwell delay into the tracker.
            const double max_allowed_sample = (samples_before >= constants::MIN_SAMPLES_FOR_MEDIAN_FILTER)
                ? (m_rtt_tracker.get_median_rtt_ms() + std::max(constants::MIN_OUTLIER_TOLERANCE_MS, constants::JITTER_SPIKE_MULTIPLIER * m_rtt_tracker.get_jitter_ms()))
                : std::max(200.0, baseline_rtt + constants::MIN_OUTLIER_TOLERANCE_MS);
            if (measured_rtt <= max_allowed_sample) {
                m_rtt_tracker.add_sample(measured_rtt);
            } else if (samples_before == 0) {
                m_rtt_tracker.add_sample(baseline_rtt);
            }
        }
    } else if (elapsed > 0.0 && elapsed < constants::MAX_PLAUSIBLE_RTT_MS) {
        measured_rtt = elapsed;
        effective_rtt = measured_rtt;
        OutlierDirection current_sample_dir = OutlierDirection::None;

        // Apply moving median spike filter to reject extreme latency anomalies
        if (samples_before >= constants::MIN_SAMPLES_FOR_MEDIAN_FILTER) {
            const double median_rtt = m_rtt_tracker.get_median_rtt_ms();
            const double jitter = m_rtt_tracker.get_jitter_ms();
            const double tolerance = std::max(
                constants::MIN_OUTLIER_TOLERANCE_MS,
                constants::JITTER_SPIKE_MULTIPLIER * jitter
            );
            const double upper_threshold = median_rtt + tolerance;
            const double lower_threshold = median_rtt - tolerance;

            if (effective_rtt > upper_threshold) {
                current_sample_dir = OutlierDirection::Up;
            } else if (effective_rtt < lower_threshold) {
                current_sample_dir = OutlierDirection::Down;
            }
        } else if (is_cold_start) {
            // Cold-start protection: prior to having median filtering samples,
            // guard against initial handshake jitter, hitching, opening burst, or queue delays
            const double cold_tolerance = std::max(constants::MIN_OUTLIER_TOLERANCE_MS, baseline_rtt * 0.5);
            const double cold_start_cap = (samples_before == 0)
                ? std::max(200.0, baseline_rtt + cold_tolerance)
                : (baseline_rtt + cold_tolerance);
            const double cold_start_floor = baseline_rtt - cold_tolerance;

            if (effective_rtt > cold_start_cap) {
                current_sample_dir = OutlierDirection::Up;
            } else if (effective_rtt < cold_start_floor) {
                current_sample_dir = OutlierDirection::Down;
            }
        }

        if (current_sample_dir != OutlierDirection::None) {
            if (m_outlier_direction == current_sample_dir) {
                ++m_consecutive_outliers;
            } else {
                m_outlier_direction = current_sample_dir;
                m_consecutive_outliers = 1;
            }

            if (m_consecutive_outliers >= constants::CONSECUTIVE_OUTLIER_RESEED_THRESHOLD) {
                m_rtt_tracker.reset(effective_rtt);
                res.route_shift_reseeded = true;
                m_consecutive_outliers = 0;
                m_outlier_direction = OutlierDirection::None;
            } else {
                if (current_sample_dir == OutlierDirection::Up) {
                    if (samples_before >= constants::MIN_SAMPLES_FOR_MEDIAN_FILTER) {
                        effective_rtt = m_rtt_tracker.get_median_rtt_ms();
                        res.spike_filtered = true;
                    } else {
                        const double cold_tolerance = std::max(constants::MIN_OUTLIER_TOLERANCE_MS, baseline_rtt * 0.5);
                        const double cold_start_cap = (samples_before == 0)
                            ? std::max(200.0, baseline_rtt + cold_tolerance)
                            : (baseline_rtt + cold_tolerance);
                        effective_rtt = cold_start_cap;
                        res.cold_start_guard = true;
                    }
                }
                // Downward outlier: do NOT clamp effective_rtt upwards to median.
                // Using measured_rtt is strictly anti-cheat safe because smaller latency reduction means longer animation lock.
            }
        } else {
            m_consecutive_outliers = 0;
            m_outlier_direction = OutlierDirection::None;
        }

        // Ingest sample into tracker unless route shift reseed just reset it with effective_rtt
        if (!res.route_shift_reseeded) {
            const double sample_to_ingest = (current_sample_dir != OutlierDirection::None)
                ? (samples_before >= constants::MIN_SAMPLES_FOR_MEDIAN_FILTER ? m_rtt_tracker.get_median_rtt_ms() : baseline_rtt)
                : measured_rtt;
            m_rtt_tracker.add_sample(sample_to_ingest);
        }
    }

    res.measured_rtt_ms = measured_rtt;
    res.smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms();

    // 2. Compute latency delta to mitigate: Delta = RTT - TargetPing - SafetyMargin
    // Relative latency invariant: reduction must never exceed (effective_rtt - target_ping_ms)
    const double max_allowed_reduction = std::max(0.0, (effective_rtt - m_config.target_ping_ms) - m_config.safety_margin_ms);

    // 3. Compute raw target animation lock
    double target_lock = original_lock_ms - max_allowed_reduction;

    // 4. Apply safety floors and ceilings (Anti-cheat & server anomaly protection)
    if (target_lock < m_config.min_animation_lock_ms) {
        target_lock = m_config.min_animation_lock_ms;
        res.clamped_by_floor = true;
        ++m_total_floor_clamps;
    }

    // Ceiling clamp: only apply to standard actions (original_lock_ms <= max_animation_lock_ms).
    // Extended locks like Limit Breaks (3.8s–8.0s) or Potions must NOT be truncated to max ceiling.
    if (original_lock_ms <= m_config.max_animation_lock_ms && target_lock > m_config.max_animation_lock_ms) {
        target_lock = m_config.max_animation_lock_ms;
        res.clamped_by_ceiling = true;
    }

    // Never increase original lock beyond server's intent unless original was below min floor
    if (target_lock > original_lock_ms && original_lock_ms >= m_config.min_animation_lock_ms) {
        target_lock = original_lock_ms;
    }

    res.adjusted_lock_ms = target_lock;
    res.delay_reduced_ms = std::max(0.0, original_lock_ms - res.adjusted_lock_ms);

    // 5. Check dry-run mode
    if (m_config.dry_run) {
        res.applied = false;
    } else {
        res.applied = (res.delay_reduced_ms > 0.0);
    }

    // 6. Update session telemetry (only if mitigation was actually applied to game memory)
    if (res.applied) {
        ++m_total_actions_mitigated;
        m_cumulative_time_saved_ms += res.delay_reduced_ms;
    }

    return res;
}

void AnimationLockMitigator::record_cast_begin(ActionId action_id, float cast_time_seconds, TimePoint now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cast_tracker.on_cast_begin(action_id, cast_time_seconds, now);
}

void AnimationLockMitigator::record_cast_interrupt(TimePoint now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cast_tracker.on_cast_interrupt(now);
}

void AnimationLockMitigator::record_cast_end(TimePoint now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cast_tracker.on_cast_end(now);
}

bool AnimationLockMitigator::is_casting(TimePoint now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cast_tracker.is_casting(now, m_rtt_tracker.get_smoothed_rtt_ms());
}

MitigationConfig AnimationLockMitigator::get_config() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void AnimationLockMitigator::set_config(const MitigationConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
    m_config.min_animation_lock_ms = std::max(20.0, m_config.min_animation_lock_ms);
    m_rtt_tracker.set_window_size(config.rtt_sample_window);
}

void AnimationLockMitigator::set_dry_run(bool dry_run) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.dry_run = dry_run;
}

void AnimationLockMitigator::set_target_ping_ms(double target_ping_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.target_ping_ms = std::max(0.0, target_ping_ms);
}

void AnimationLockMitigator::set_min_animation_lock_ms(double min_lock_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.min_animation_lock_ms = std::max(constants::ABSOLUTE_MIN_ANIMATION_LOCK_FLOOR_MS, min_lock_ms);
}

SessionStats AnimationLockMitigator::get_session_stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return SessionStats{
        .total_actions_requested = m_total_actions_requested,
        .total_actions_mitigated = m_total_actions_mitigated,
        .cumulative_time_saved_ms = m_cumulative_time_saved_ms,
        .current_smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms(),
        .current_jitter_ms = m_rtt_tracker.get_jitter_ms(),
        .total_floor_clamps = m_total_floor_clamps
    };
}

void AnimationLockMitigator::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_seq_tracker.clear();
    m_cast_tracker.reset();
    m_rtt_tracker.reset(m_config.target_ping_ms * 3.0);
    m_total_actions_requested = 0;
    m_total_actions_mitigated = 0;
    m_cumulative_time_saved_ms = 0.0;
    m_total_floor_clamps = 0;
    m_consecutive_outliers = 0;
    m_outlier_direction = OutlierDirection::None;
}

} // namespace mitigator
