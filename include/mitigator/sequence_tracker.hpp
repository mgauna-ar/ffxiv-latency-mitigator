#pragma once

#include "mitigator/types.hpp"
#include <deque>
#include <optional>
#include <mutex>
#include <chrono>

namespace mitigator {

/**
 * @brief Thread-safe tracker correlating action requests with server effect responses.
 *
 * Handles sequence number matching, FIFO fallback for unsequenced actions,
 * out-of-order packet arrival, and automatic stale entry eviction.
 */
class SequenceTracker {
public:
    explicit SequenceTracker(std::chrono::milliseconds stale_timeout = constants::DEFAULT_STALE_TIMEOUT);

    /**
     * @brief Records an outgoing action invocation.
     */
    void record_request(ActionId action_id, SequenceId sequence, TimePoint timestamp = std::chrono::steady_clock::now());

    /**
     * @brief Attempts to correlate an incoming action effect with a recorded request.
     * Matches by exact sequence number if non-zero; otherwise matches the oldest
     * matching action_id in the FIFO queue.
     */
    [[nodiscard]] std::optional<ActionRequestInfo> match_response(
        ActionId action_id,
        SequenceId sequence,
        TimePoint timestamp = std::chrono::steady_clock::now()
    );

    /**
     * @brief Evicts all tracked requests older than the stale timeout.
     * @return Number of requests pruned.
     */
    size_t prune_stale(TimePoint now = std::chrono::steady_clock::now());

    /// Returns the number of currently pending action requests.
    [[nodiscard]] size_t pending_count() const;

    /// Clears all pending requests.
    void clear();

    /// Sets the stale timeout duration.
    void set_stale_timeout(std::chrono::milliseconds timeout);

private:
    mutable std::mutex m_mutex;
    std::deque<ActionRequestInfo> m_pending;
    std::chrono::milliseconds m_stale_timeout{5000};
    static constexpr size_t MAX_PENDING_ENTRIES = 256;
};

} // namespace mitigator
