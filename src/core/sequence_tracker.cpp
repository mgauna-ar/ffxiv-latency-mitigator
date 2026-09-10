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
    float cast_duration_seconds,
    bool is_queued
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
        .cast_duration_seconds = cast_duration_seconds,
        .is_queued = is_queued
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

    // 2. Secondary Strategy: Match by action_id
    if (action_id != 0) {
        if (sequence == 0) {
            // Unsequenced server response (e.g. potion, duty action, sprint):
            // First prefer an unsequenced pending request
            auto it = std::find_if(m_pending.begin(), m_pending.end(),
                [action_id](const ActionRequestInfo& req) {
                    return (req.sequence == 0) && (req.action_id == action_id);
                });

            // If no unsequenced request exists, fall back to matching the oldest pending request for this action
            if (it == m_pending.end()) {
                it = std::find_if(m_pending.begin(), m_pending.end(),
                    [action_id](const ActionRequestInfo& req) {
                        return req.action_id == action_id;
                    });
            }

            if (it != m_pending.end()) {
                ActionRequestInfo matched = *it;
                m_pending.erase(it);
                return matched;
            }
        } else {
            // Sequenced server response where exact sequence did not match:
            // Match if:
            // a) Pending request was unsequenced (req.sequence == 0)
            // b) Pending request was queued (in FFXIV, client sequence at queue time is N while server sequence is N+1)
            // c) Server sequence is adjacent to request sequence (sequence == req.sequence + 1 or uint16 wraparound)
            auto it = std::find_if(m_pending.begin(), m_pending.end(),
                [action_id, sequence](const ActionRequestInfo& req) {
                    if (req.action_id != action_id) {
                        return false;
                    }
                    if (req.sequence == 0) {
                        return true;
                    }
                    if (req.is_queued) {
                        return true;
                    }
                    const uint16_t expected_next = static_cast<uint16_t>(req.sequence + 1);
                    if (static_cast<uint16_t>(sequence) == expected_next) {
                        return true;
                    }
                    return false;
                });

            if (it != m_pending.end()) {
                ActionRequestInfo matched = *it;
                m_pending.erase(it);
                return matched;
            }
        }
    }

    // 3. Fallback: If sequence and action_id are unspecified (generic response)
    // and exactly 1 pending action exists with sequence == 0, and was recent (< 1500ms), match it
    if (sequence == 0 && action_id == 0 && m_pending.size() == 1 && m_pending.front().sequence == 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp - m_pending.front().timestamp
        );
        if (elapsed >= std::chrono::milliseconds(0) && elapsed < constants::GENERIC_FALLBACK_MAX_ELAPSED) {
            ActionRequestInfo matched = m_pending.front();
            m_pending.pop_front();
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
