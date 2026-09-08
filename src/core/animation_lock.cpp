#include "mitigator/animation_lock.hpp"
#include <algorithm>
#include <cmath>

namespace mitigator {

AnimationLockMitigator::AnimationLockMitigator(const MitigationConfig& config)
    : m_config(config),
      m_rtt_tracker(config.rtt_sample_window, config.target_ping_ms > 0 ? config.target_ping_ms * 3.0 : 50.0) {}

void AnimationLockMitigator::record_action_request(
    ActionId action_id,
    SequenceId sequence,
    TimePoint timestamp
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_total_actions_requested;
    m_seq_tracker.record_request(action_id, sequence, timestamp);
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

    double measured_rtt = 0.0;
    if (matched_req.has_value()) {
        const auto elapsed = std::chrono::duration_cast<Milliseconds>(
            now - matched_req->timestamp
        ).count();
        if (elapsed > 0.0 && elapsed < 5000.0) {
            measured_rtt = elapsed;
            m_rtt_tracker.add_sample(measured_rtt);
        }
    }

    res.measured_rtt_ms = measured_rtt;
    res.smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms();

    // Use measured RTT if valid; otherwise fallback to current smoothed RTT estimate
    const double effective_rtt = (measured_rtt > 0.0) ? measured_rtt : res.smoothed_rtt_ms;

    // Check if active cast is in progress for this action
    res.cast_active = m_cast_tracker.is_casting(now);

    // 2. Compute latency delta to mitigate: Delta = RTT - TargetPing + SafetyMargin
    double latency_delta = (effective_rtt - m_config.target_ping_ms) + m_config.safety_margin_ms;
    if (latency_delta < 0.0) {
        latency_delta = 0.0; // Already faster than target ping, no need to reduce
    }

    // 3. Compute raw target animation lock
    double target_lock = original_lock_ms - latency_delta;

    // 4. Apply safety floors and ceilings (Anti-cheat & server anomaly protection)
    if (target_lock < m_config.min_animation_lock_ms) {
        target_lock = m_config.min_animation_lock_ms;
        res.clamped_by_floor = true;
        ++m_total_floor_clamps;
    }

    if (target_lock > m_config.max_animation_lock_ms) {
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

    // 6. Update session telemetry
    if (res.delay_reduced_ms > 0.0) {
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

MitigationConfig AnimationLockMitigator::get_config() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void AnimationLockMitigator::set_config(const MitigationConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
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
    m_config.min_animation_lock_ms = std::max(0.0, min_lock_ms);
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
}

} // namespace mitigator
