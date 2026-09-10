#pragma once

#include <vector>
#include <deque>
#include <mutex>
#include <cstddef>

namespace mitigator {

/**
 * @brief Thread-safe rolling Round-Trip-Time (RTT) tracker.
 *
 * Implements exponential moving average (EMA), moving median for spike resistance,
 * and jitter calculation to adaptively smooth network fluctuations.
 */
class RollingRttTracker {
public:
    explicit RollingRttTracker(size_t window_size = 10, double initial_rtt_ms = 50.0);

    /**
     * @brief Ingests a new RTT measurement in milliseconds.
     * Rejects invalid (negative or extreme) outliers.
     */
    void add_sample(double rtt_ms);

    /// Returns the Exponential Moving Average (EMA) RTT in milliseconds.
    [[nodiscard]] double get_smoothed_rtt_ms() const;

    /// Returns the moving median RTT in milliseconds over the current window.
    [[nodiscard]] double get_median_rtt_ms() const;

    /// Returns the simple moving average RTT in milliseconds.
    [[nodiscard]] double get_average_rtt_ms() const;

    /// Returns the network jitter (mean deviation) in milliseconds.
    [[nodiscard]] double get_jitter_ms() const;

    /// Returns the total number of valid samples recorded.
    [[nodiscard]] size_t sample_count() const;

    /// Returns a snapshot copy of the raw RTT samples in the current rolling window.
    [[nodiscard]] std::vector<double> get_samples() const;

    /// Adjusts the rolling window size.
    void set_window_size(size_t window_size);

    /// Resets all statistics.
    void reset(double initial_rtt_ms = 50.0);

private:
    mutable std::mutex m_mutex;
    std::deque<double> m_samples;
    size_t m_window_size{10};
    double m_smoothed_rtt{50.0};
    double m_jitter{0.0};
    size_t m_total_samples{0};
};

} // namespace mitigator
