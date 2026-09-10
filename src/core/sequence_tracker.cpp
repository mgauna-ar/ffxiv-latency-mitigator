#include "mitigator/sequence_tracker.hpp"
#include <algorithm>

namespace mitigator {

SequenceTracker::SequenceTracker(std::chrono::milliseconds stale_timeout)
    : m_stale_timeout(stale_timeout) {}

void SequenceTracker::record_request(
    ActionId action_id,
    SequenceId sequence,
    TimePoint timestamp,
    bool is_cast,
    float cast_duration_seconds
) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Prune stale entries to keep queue compact
    const auto cutoff = timestamp - m_stale_timeout;
    while (!m_pending.empty() && m_pending.front().timestamp < cutoff) {
        m_pending.pop_front();
    }

    // Duplicate sequence detection (2C):
    // If a request with the exact same non-zero sequence already exists in the queue
    // (e.g. client re-dispatch or sequence wraparound before server ack),
    // remove the stale duplicate so it doesn't linger as a ghost entry.
    if (sequence != 0) {
        auto it = std::find_if(m_pending.begin(), m_pending.end(),
            [sequence](const ActionRequestInfo& req) {
                return req.sequence == sequence;
            });
        if (it != m_pending.end()) {
            m_pending.erase(it);
        }
    }

    // Guard against unbounded queue growth under abnormal conditions
    if (m_pending.size() >= MAX_PENDING_ENTRIES) {
        m_pending.pop_front();
    }

    m_pending.push_back(ActionRequestInfo{
        .action_id = action_id,
        .sequence = sequence,
        .timestamp = timestamp,
        .is_cast = is_cast,
        .cast_duration_seconds = cast_duration_seconds
    });
}

std::optional<ActionRequestInfo> SequenceTracker::match_response(
    ActionId action_id,
    SequenceId sequence,
    TimePoint timestamp,
    double expected_rtt_ms
) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Prune stale requests first
    const auto cutoff = timestamp - m_stale_timeout;
    while (!m_pending.empty() && m_pending.front().timestamp < cutoff) {
        m_pending.pop_front();
    }

    if (m_pending.empty()) {
        return std::nullopt;
    }

    // 1. Primary Strategy: Match by exact non-zero sequence
    if (sequence != 0) {
        auto it = std::find_if(m_pending.begin(), m_pending.end(),
            [sequence](const ActionRequestInfo& req) {
                return req.sequence == sequence;
            });

        if (it != m_pending.end()) {
            ActionRequestInfo matched = *it;
            m_pending.erase(it);
            return matched;
        }
    }

    // 2. Secondary Strategy: Match pending request with matching action_id
    if (action_id != 0) {
        auto it = std::find_if(m_pending.begin(), m_pending.end(),
            [action_id](const ActionRequestInfo& req) {
                return req.action_id == action_id;
            });

        if (it != m_pending.end()) {
            // Plausibility check for repeated actions (2A):
            // If expected_rtt_ms is provided and multiple requests share the same action_id,
            // prune any older match whose elapsed duration is implausibly large compared to expected RTT.
            if (expected_rtt_ms > 0.0) {
                const double base_threshold_ms = (std::max)(
                    2.0 * expected_rtt_ms,
                    expected_rtt_ms + constants::MIN_OUTLIER_TOLERANCE_MS
                );

                while (it != m_pending.end()) {
                    const double elapsed_ms = std::chrono::duration_cast<Milliseconds>(
                        timestamp - it->timestamp
                    ).count();

                    const double cast_time_ms = it->is_cast
                        ? (static_cast<double>(it->cast_duration_seconds) * constants::MS_PER_SECOND)
                        : 0.0;

                    if (elapsed_ms > (cast_time_ms + base_threshold_ms)) {
                        auto next = std::find_if(std::next(it), m_pending.end(),
                            [action_id](const ActionRequestInfo& req) {
                                return req.action_id == action_id;
                            });

                        if (next != m_pending.end()) {
                            // The older entry is stale; erase it and continue checking from next match
                            it = m_pending.erase(it);
                            it = std::find_if(it, m_pending.end(),
                                [action_id](const ActionRequestInfo& req) {
                                    return req.action_id == action_id;
                                });
                            continue;
                        }
                    }
                    break;
                }
            }

            if (it != m_pending.end()) {
                ActionRequestInfo matched = *it;
                m_pending.erase(it);
                return matched;
            }
        }
    }

    return std::nullopt;
}

size_t SequenceTracker::prune_stale(TimePoint now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto cutoff = now - m_stale_timeout;
    const size_t initial_size = m_pending.size();

    while (!m_pending.empty() && m_pending.front().timestamp < cutoff) {
        m_pending.pop_front();
    }

    return initial_size - m_pending.size();
}

size_t SequenceTracker::pending_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pending.size();
}

void SequenceTracker::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.clear();
}

void SequenceTracker::set_stale_timeout(std::chrono::milliseconds timeout) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_stale_timeout = timeout;
}

} // namespace mitigator
