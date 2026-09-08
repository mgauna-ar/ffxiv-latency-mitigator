#include "test_framework.hpp"
#include "mitigator/rolling_rtt.hpp"
#include <thread>
#include <vector>

TEST_CASE(RollingRtt, InitialState) {
    mitigator::RollingRttTracker tracker(10, 45.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 45.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 45.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_jitter_ms(), 0.0, 0.001);
}

TEST_CASE(RollingRtt, SingleSample) {
    mitigator::RollingRttTracker tracker(10, 50.0);
    tracker.add_sample(120.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 1);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 120.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 120.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_average_rtt_ms(), 120.0, 0.001);
}

TEST_CASE(RollingRtt, ExponentialMovingAverage) {
    mitigator::RollingRttTracker tracker(5, 50.0);
    tracker.add_sample(100.0);
    tracker.add_sample(100.0);
    tracker.add_sample(100.0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 100.0, 0.1);
    TEST_ASSERT_NEAR(tracker.get_average_rtt_ms(), 100.0, 0.001);

    // Add a single higher sample: smoothed RTT increases smoothly
    tracker.add_sample(130.0);
    TEST_ASSERT(tracker.get_smoothed_rtt_ms() > 100.0);
    TEST_ASSERT(tracker.get_smoothed_rtt_ms() < 130.0);
    TEST_ASSERT(tracker.get_jitter_ms() > 0.0);
}

TEST_CASE(RollingRtt, MedianSpikeRejection) {
    mitigator::RollingRttTracker tracker(5, 50.0);
    tracker.add_sample(50.0);
    tracker.add_sample(52.0);
    tracker.add_sample(48.0);
    tracker.add_sample(51.0);
    // Severe latency spike (packet queue delay)
    tracker.add_sample(450.0);

    // Median should still reflect the stable cluster around ~51ms
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 51.0, 1.0);
    // Average is heavily affected by 450ms spike
    TEST_ASSERT(tracker.get_average_rtt_ms() > 100.0);
}

TEST_CASE(RollingRtt, OutlierFiltering) {
    mitigator::RollingRttTracker tracker(5, 50.0);
    // Negative or near-zero samples are invalid
    tracker.add_sample(-10.0);
    tracker.add_sample(0.1);
    // Unreasonably huge sample (> 5000ms) is invalid
    tracker.add_sample(9999.0);

    TEST_ASSERT_EQ(tracker.sample_count(), 0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 50.0, 0.001);
}

TEST_CASE(RollingRtt, WindowResizeAndReset) {
    mitigator::RollingRttTracker tracker(10, 50.0);
    for (int i = 0; i < 15; ++i) {
        tracker.add_sample(60.0);
    }
    TEST_ASSERT_EQ(tracker.sample_count(), 15);

    tracker.set_window_size(3);
    tracker.reset(75.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 75.0, 0.001);
}

TEST_CASE(RollingRtt, MultithreadedConcurrentSamples) {
    mitigator::RollingRttTracker tracker(20, 50.0);
    std::vector<std::thread> threads;
    constexpr int NUM_THREADS = 4;
    constexpr int SAMPLES_PER_THREAD = 100;

    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&tracker, t]() {
            for (int i = 0; i < SAMPLES_PER_THREAD; ++i) {
                tracker.add_sample(60.0 + static_cast<double>(t));
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }

    TEST_ASSERT_EQ(tracker.sample_count(), NUM_THREADS * SAMPLES_PER_THREAD);
    TEST_ASSERT(tracker.get_smoothed_rtt_ms() >= 60.0 && tracker.get_smoothed_rtt_ms() <= 64.0);
}
