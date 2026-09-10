#include "test_framework.hpp"
#include "mitigator/rolling_rtt.hpp"
#include "mitigator/types.hpp"
#include <thread>
#include <vector>
#include <atomic>
#include <limits>

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

TEST_CASE(RollingRtt, GetSamplesSnapshot) {
    mitigator::RollingRttTracker tracker(3, 50.0);
    // Initially empty
    auto samples = tracker.get_samples();
    TEST_ASSERT(samples.empty());

    tracker.add_sample(40.0);
    tracker.add_sample(50.0);
    samples = tracker.get_samples();
    TEST_ASSERT_EQ(samples.size(), 2);
    TEST_ASSERT_NEAR(samples[0], 40.0, 0.001);
    TEST_ASSERT_NEAR(samples[1], 50.0, 0.001);

    // Overflow rolling window of 3
    tracker.add_sample(60.0);
    tracker.add_sample(70.0);
    samples = tracker.get_samples();
    TEST_ASSERT_EQ(samples.size(), 3);
    TEST_ASSERT_NEAR(samples[0], 50.0, 0.001);
    TEST_ASSERT_NEAR(samples[1], 60.0, 0.001);
    TEST_ASSERT_NEAR(samples[2], 70.0, 0.001);

    // Reset clears samples
    tracker.reset(50.0);
    samples = tracker.get_samples();
    TEST_ASSERT(samples.empty());
}

TEST_CASE(RollingRtt, ResetReseedsMedianAndEma) {
    mitigator::RollingRttTracker tracker(10, 40.0);
    for (int i = 0; i < 5; ++i) {
        tracker.add_sample(40.0);
    }
    TEST_ASSERT_EQ(tracker.sample_count(), 5);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 40.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 40.0, 0.001);

    // Reseed to a route shift baseline of 120ms
    tracker.reset(120.0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 120.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 120.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_jitter_ms(), 0.0, 0.001);
    TEST_ASSERT_EQ(tracker.sample_count(), 0);
    TEST_ASSERT(tracker.get_samples().empty());

    // Subsequent sample adapts cleanly from new 120ms baseline
    tracker.add_sample(125.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 1);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 125.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 125.0, 0.001);

    // Add 2nd sample: n=2, alpha = 2/(2+1) = 2/3
    tracker.add_sample(125.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 2);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 125.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 125.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_jitter_ms(), 0.0, 0.001);

    // Sample with jitter: n=3, alpha = 2/(3+1) = 0.5
    // diff against previous smoothed (125.0): |135.0 - 125.0| = 10.0
    // smoothed = 0.5 * 135.0 + 0.5 * 125.0 = 130.0
    // jitter = 0.5 * 10.0 + 0.5 * 0.0 = 5.0
    tracker.add_sample(135.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 3);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 130.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 125.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_jitter_ms(), 5.0, 0.001);
}

TEST_CASE(RollingRtt, ResetPlausibilityBounds) {
    mitigator::RollingRttTracker tracker(10, 50.0);
    tracker.add_sample(60.0);

    // Negative sample should fall back to default initial RTT
    tracker.reset(-5.0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // Zero should fall back to default
    tracker.reset(0.0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // Just below minimum plausible RTT (0.5ms) should fall back to default
    tracker.reset(0.4);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // Exact minimum plausible boundary (0.5ms) must be accepted
    tracker.reset(mitigator::constants::MIN_PLAUSIBLE_RTT_MS);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::MIN_PLAUSIBLE_RTT_MS, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), mitigator::constants::MIN_PLAUSIBLE_RTT_MS, 0.001);

    // Exact maximum plausible boundary (5000.0ms) must be accepted
    tracker.reset(mitigator::constants::MAX_PLAUSIBLE_RTT_MS);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::MAX_PLAUSIBLE_RTT_MS, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), mitigator::constants::MAX_PLAUSIBLE_RTT_MS, 0.001);

    // Just above maximum plausible RTT (5000.1ms) should fall back to default
    tracker.reset(5000.1);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // Extreme sample (> MAX_PLAUSIBLE_RTT_MS) should fall back to default
    tracker.reset(10000.0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // Infinity should fall back to default
    tracker.reset(std::numeric_limits<double>::infinity());
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    tracker.reset(-std::numeric_limits<double>::infinity());
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // NaN should fall back to default
    tracker.reset(std::numeric_limits<double>::quiet_NaN());
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);
}

TEST_CASE(RollingRtt, ConstructorPlausibilityBounds) {
    // Negative initial RTT defaults
    mitigator::RollingRttTracker t1(10, -10.0);
    TEST_ASSERT_NEAR(t1.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // Zero initial RTT defaults
    mitigator::RollingRttTracker t2(10, 0.0);
    TEST_ASSERT_NEAR(t2.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // Below min bound defaults
    mitigator::RollingRttTracker t3(10, 0.4);
    TEST_ASSERT_NEAR(t3.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // Exact min bound accepted
    mitigator::RollingRttTracker t4(10, mitigator::constants::MIN_PLAUSIBLE_RTT_MS);
    TEST_ASSERT_NEAR(t4.get_smoothed_rtt_ms(), mitigator::constants::MIN_PLAUSIBLE_RTT_MS, 0.001);

    // Exact max bound accepted
    mitigator::RollingRttTracker t5(10, mitigator::constants::MAX_PLAUSIBLE_RTT_MS);
    TEST_ASSERT_NEAR(t5.get_smoothed_rtt_ms(), mitigator::constants::MAX_PLAUSIBLE_RTT_MS, 0.001);

    // Above max bound defaults
    mitigator::RollingRttTracker t6(10, 10000.0);
    TEST_ASSERT_NEAR(t6.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    // Non-finite values default
    mitigator::RollingRttTracker t7(10, std::numeric_limits<double>::infinity());
    TEST_ASSERT_NEAR(t7.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);

    mitigator::RollingRttTracker t8(10, std::numeric_limits<double>::quiet_NaN());
    TEST_ASSERT_NEAR(t8.get_smoothed_rtt_ms(), mitigator::constants::DEFAULT_INITIAL_RTT_MS, 0.001);
}

TEST_CASE(RollingRtt, MultithreadedConcurrentResetAndSamples) {
    mitigator::RollingRttTracker tracker(20, 50.0);
    std::atomic<bool> running{true};
    std::vector<std::thread> threads;

    // Worker threads adding samples
    for (int t = 0; t < 3; ++t) {
        threads.emplace_back([&tracker, &running, t]() {
            while (running.load(std::memory_order_relaxed)) {
                tracker.add_sample(50.0 + static_cast<double>(t));
            }
        });
    }

    // Reader thread reading stats
    threads.emplace_back([&tracker, &running]() {
        while (running.load(std::memory_order_relaxed)) {
            const double smoothed = tracker.get_smoothed_rtt_ms();
            const double median = tracker.get_median_rtt_ms();
            const double jitter = tracker.get_jitter_ms();
            (void)smoothed;
            (void)median;
            (void)jitter;
        }
    });

    // Resetter thread calling reset periodically
    threads.emplace_back([&tracker, &running]() {
        for (int i = 0; i < 50; ++i) {
            tracker.reset(60.0 + static_cast<double>(i % 5));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        running.store(false, std::memory_order_relaxed);
    });

    for (auto& th : threads) {
        th.join();
    }

    // After all threads finish, tracker state must remain well within valid plausible bounds
    TEST_ASSERT(tracker.get_smoothed_rtt_ms() >= mitigator::constants::MIN_PLAUSIBLE_RTT_MS);
    TEST_ASSERT(tracker.get_smoothed_rtt_ms() <= mitigator::constants::MAX_PLAUSIBLE_RTT_MS);
    TEST_ASSERT(tracker.get_median_rtt_ms() >= mitigator::constants::MIN_PLAUSIBLE_RTT_MS);
    TEST_ASSERT(tracker.get_median_rtt_ms() <= mitigator::constants::MAX_PLAUSIBLE_RTT_MS);
    TEST_ASSERT(tracker.get_jitter_ms() >= 0.0);
}

TEST_CASE(RollingRtt, DomainTypesPhase1Defaults) {
    mitigator::ActionRequestInfo req{};
    TEST_ASSERT_EQ(req.is_queued, false);
    req.is_queued = true;
    TEST_ASSERT_EQ(req.is_queued, true);

    mitigator::MitigationResult res{};
    TEST_ASSERT_EQ(res.queued_action, false);
    TEST_ASSERT_EQ(res.route_shift_reseeded, false);
    res.queued_action = true;
    res.route_shift_reseeded = true;
    TEST_ASSERT_EQ(res.queued_action, true);
    TEST_ASSERT_EQ(res.route_shift_reseeded, true);

    TEST_ASSERT_EQ(mitigator::constants::CONSECUTIVE_OUTLIER_RESEED_THRESHOLD, 4);
}

TEST_CASE(RollingRtt, DownwardLatencyTransitionAdaptsMedianAndSmoothed) {
    // Window size 10
    mitigator::RollingRttTracker tracker(10, 200.0);
    for (int i = 0; i < 10; ++i) {
        tracker.add_sample(200.0);
    }
    TEST_ASSERT_EQ(tracker.sample_count(), 10);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 200.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 200.0, 0.001);

    // Latency drops suddenly from 200ms to 40ms (e.g. routing restored)
    // Ingest 10 samples of 40ms
    for (int i = 1; i <= 10; ++i) {
        tracker.add_sample(40.0);
        TEST_ASSERT_EQ(tracker.sample_count(), 10 + static_cast<size_t>(i));
    }

    // After 10 samples of 40ms, all 10 slots in sliding window are 40ms
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 40.0, 0.001);
    // EMA smoothed RTT has decayed heavily towards 40ms (from 200ms down to ~61.5ms with alpha=2/11)
    TEST_ASSERT(tracker.get_smoothed_rtt_ms() < 65.0);
    TEST_ASSERT(tracker.get_smoothed_rtt_ms() >= 40.0);

    // After 5 more samples (15 total downward samples), smoothed RTT decays below 50.0ms
    for (int i = 0; i < 5; ++i) {
        tracker.add_sample(40.0);
    }
    TEST_ASSERT(tracker.get_smoothed_rtt_ms() < 50.0);
    TEST_ASSERT(tracker.get_smoothed_rtt_ms() >= 40.0);
}

TEST_CASE(RollingRtt, ResetReseedsDownwardShift) {
    mitigator::RollingRttTracker tracker(10, 200.0);
    for (int i = 0; i < 10; ++i) {
        tracker.add_sample(200.0);
    }
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 200.0, 0.001);

    // Immediate downward reseed to 40.0ms
    tracker.reset(40.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 40.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 40.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_jitter_ms(), 0.0, 0.001);
    TEST_ASSERT(tracker.get_samples().empty());

    // Next sample at 42ms adapts cleanly
    tracker.add_sample(42.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 1);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 42.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 42.0, 0.001);
}

TEST_CASE(RollingRtt, NonFiniteAddSampleRejected) {
    mitigator::RollingRttTracker tracker(10, 50.0);

    tracker.add_sample(std::numeric_limits<double>::quiet_NaN());
    tracker.add_sample(std::numeric_limits<double>::infinity());
    tracker.add_sample(-std::numeric_limits<double>::infinity());

    // None of these invalid samples should be accepted
    TEST_ASSERT_EQ(tracker.sample_count(), 0);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 50.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 50.0, 0.001);
    TEST_ASSERT_NEAR(tracker.get_jitter_ms(), 0.0, 0.001);
}


