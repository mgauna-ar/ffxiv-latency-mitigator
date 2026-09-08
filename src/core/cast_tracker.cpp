#include "mitigator/cast_tracker.hpp"
#include <algorithm>

namespace mitigator {

void CastTracker::on_cast_begin(ActionId action_id, float cast_time_seconds, TimePoint now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_casting = true;
    m_cast_action_id = action_id;
    m_cast_duration_seconds = std::max(0.0f, cast_time_seconds);
    m_cast_start = now;
}

void CastTracker::on_cast_interrupt(TimePoint /*now*/) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_casting = false;
    m_cast_action_id = 0;
    m_cast_duration_seconds = 0.0f;
}

void CastTracker::on_cast_end(TimePoint /*now*/) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_casting = false;
    m_cast_action_id = 0;
    m_cast_duration_seconds = 0.0f;
}

bool CastTracker::is_casting(TimePoint now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_casting) {
        return false;
    }

    const auto elapsed = std::chrono::duration<float>(now - m_cast_start).count();
    // Allow a small grace window after cast completes for server ack
    return elapsed < (m_cast_duration_seconds + constants::CAST_COMPLETION_GRACE_WINDOW_SECONDS);
}

ActionId CastTracker::current_cast_action_id() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cast_action_id;
}

float CastTracker::remaining_cast_time_seconds(TimePoint now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_casting) {
        return 0.0f;
    }

    const auto elapsed = std::chrono::duration<float>(now - m_cast_start).count();
    const float remaining = m_cast_duration_seconds - elapsed;
    return std::max(0.0f, remaining);
}

void CastTracker::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_casting = false;
    m_cast_action_id = 0;
    m_cast_duration_seconds = 0.0f;
}

} // namespace mitigator
