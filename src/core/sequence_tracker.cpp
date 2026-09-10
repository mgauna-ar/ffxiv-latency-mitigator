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
    TimePoint timestamp
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

    // 2. Secondary Strategy: Match oldest pending request with matching action_id
    if (action_id != 0) {
        auto it = std::find_if(m_pending.begin(), m_pending.end(),
            [action_id](const ActionRequestInfo& req) {
                return req.action_id == action_id;
            });

        if (it != m_pending.end()) {
            ActionRequestInfo matched = *it;
            m_pending.erase(it);
            return matched;
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
