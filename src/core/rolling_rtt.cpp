#include "mitigator/rolling_rtt.hpp"
#include <algorithm>
#include <numeric>
#include <cmath>

namespace mitigator {

RollingRttTracker::RollingRttTracker(size_t window_size, double initial_rtt_ms)
    : m_window_size(window_size > 0 ? window_size : 10),
      m_smoothed_rtt(initial_rtt_ms > 0.0 ? initial_rtt_ms : 50.0) {}

void RollingRttTracker::add_sample(double rtt_ms) {
    // Sanity filter: Ignore negative or physically impossible values (> 5000ms)
    if (rtt_ms < 0.5 || rtt_ms > 5000.0) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);

    m_samples.push_back(rtt_ms);
    if (m_samples.size() > m_window_size) {
        m_samples.pop_front();
    }

    ++m_total_samples;

    // EMA calculation: alpha adapts as samples accumulate
    const double n = static_cast<double>(std::min(m_total_samples, m_window_size));
    const double alpha = 2.0 / (n + 1.0);

    if (m_total_samples == 1) {
        m_smoothed_rtt = rtt_ms;
        m_jitter = 0.0;
    } else {
        const double diff = std::abs(rtt_ms - m_smoothed_rtt);
        m_smoothed_rtt = alpha * rtt_ms + (1.0 - alpha) * m_smoothed_rtt;
        // Jitter smoothed via EMA
        m_jitter = alpha * diff + (1.0 - alpha) * m_jitter;
    }
}

double RollingRttTracker::get_smoothed_rtt_ms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_smoothed_rtt;
}

double RollingRttTracker::get_median_rtt_ms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_samples.empty()) {
        return m_smoothed_rtt;
    }

    std::vector<double> sorted(m_samples.begin(), m_samples.end());
    std::sort(sorted.begin(), sorted.end());

    const size_t mid = sorted.size() / 2;
    if (sorted.size() % 2 == 0) {
        return (sorted[mid - 1] + sorted[mid]) / 2.0;
    }
    return sorted[mid];
}

double RollingRttTracker::get_average_rtt_ms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_samples.empty()) {
        return m_smoothed_rtt;
    }
    const double sum = std::accumulate(m_samples.begin(), m_samples.end(), 0.0);
    return sum / static_cast<double>(m_samples.size());
}

double RollingRttTracker::get_jitter_ms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_jitter;
}

size_t RollingRttTracker::sample_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_total_samples;
}

void RollingRttTracker::set_window_size(size_t window_size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_window_size = window_size > 0 ? window_size : 1;
    while (m_samples.size() > m_window_size) {
        m_samples.pop_front();
    }
}

void RollingRttTracker::reset(double initial_rtt_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_samples.clear();
    m_smoothed_rtt = initial_rtt_ms > 0.0 ? initial_rtt_ms : 50.0;
    m_jitter = 0.0;
    m_total_samples = 0;
}

} // namespace mitigator
