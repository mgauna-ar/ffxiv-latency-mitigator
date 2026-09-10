#pragma once

#include "mitigator/types.hpp"
#include <mutex>
#include <chrono>

namespace mitigator {

/**
 * @brief Thread-safe tracker for player casting states (spells / channels).
 *
 * Prevents premature animation lock reductions during active casts to maintain
 * game integrity and prevent cast-slide anomalies.
 */
class CastTracker {
public:
    CastTracker() = default;

    /// Called when the client initiates a cast (CastBegin detour).
    void on_cast_begin(ActionId action_id, float cast_time_seconds, TimePoint now = std::chrono::steady_clock::now());

    /// Called when a cast is interrupted (moving, stunned, cancelled).
    void on_cast_interrupt(TimePoint now = std::chrono::steady_clock::now());

    /// Called when a cast completes.
    void on_cast_end(TimePoint now = std::chrono::steady_clock::now());

    /// Returns true if the player is actively casting.
    [[nodiscard]] bool is_casting(TimePoint now = std::chrono::steady_clock::now(), double smoothed_rtt_ms = 0.0) const;

    /// Returns the ActionId currently being cast, or 0 if none.
    [[nodiscard]] ActionId current_cast_action_id() const;

    /// Returns the remaining cast time in seconds, or 0.0f if not casting.
    [[nodiscard]] float remaining_cast_time_seconds(TimePoint now = std::chrono::steady_clock::now()) const;

    /// Resets cast state.
    void reset();

private:
    mutable std::mutex m_mutex;
    mutable bool m_is_casting{false};
    mutable ActionId m_cast_action_id{0};
    TimePoint m_cast_start{std::chrono::steady_clock::now()};
    mutable float m_cast_duration_seconds{0.0f};
};

} // namespace mitigator
