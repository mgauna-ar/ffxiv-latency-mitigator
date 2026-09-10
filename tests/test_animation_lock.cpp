#include "test_framework.hpp"
#include "mitigator/animation_lock.hpp"
#include <thread>
#include <atomic>
#include <vector>

TEST_CASE(AnimationLock, StandardPingMitigation) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;
    cfg.dry_run = false;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Player presses action (e.g. Fleche, 600ms standard lock)
    engine.record_action_request(0x1A4F, 100, t0);

    // Server responds after 150ms round-trip network time
    const auto t_recv = t0 + std::chrono::milliseconds(150);
    const auto res = engine.calculate_mitigation(0x1A4F, 100, 600.0, t_recv);

    // Latency delta = 150ms - 15ms = 135ms
    // Adjusted lock = 600ms - 135ms = 465ms
    TEST_ASSERT_EQ(res.action_id, 0x1A4F);
    TEST_ASSERT_EQ(res.sequence, 100);
    TEST_ASSERT_NEAR(res.measured_rtt_ms, 150.0, 0.5);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 135.0, 0.5);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 465.0, 0.5);
    TEST_ASSERT(res.applied);
    TEST_ASSERT(!res.clamped_by_floor);
    TEST_ASSERT(!res.spike_filtered);
    TEST_ASSERT(!res.cold_start_guard);
}

TEST_CASE(AnimationLock, LowPingNoMitigationNeeded) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    engine.record_action_request(0x1A50, 101, t0);

    // Server responds in 10ms (faster than target ping of 15ms)
    const auto t_recv = t0 + std::chrono::milliseconds(10);
    const auto res = engine.calculate_mitigation(0x1A50, 101, 600.0, t_recv);

    // No reduction applied; lock remains 600ms
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 0.0, 0.01);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 600.0, 0.01);
    TEST_ASSERT(!res.applied);
}

TEST_CASE(AnimationLock, SafetyFloorAntiCheatClamping) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 30.0; // 30ms hard floor

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Short original lock of 100ms with high RTT of 200ms
    engine.record_action_request(0x1A51, 102, t0);
    const auto t_recv = t0 + std::chrono::milliseconds(200);
    const auto res = engine.calculate_mitigation(0x1A51, 102, 100.0, t_recv);

    // Without clamping: 100ms - (200ms - 15ms) = -85ms (dangerous/invalid!)
    // Clamping MUST clamp to exactly min_animation_lock_ms (30.0ms)
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 30.0, 0.001);
    TEST_ASSERT(res.clamped_by_floor);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 70.0, 0.001); // 100 - 30 = 70ms saved
    TEST_ASSERT(res.applied);

    // Verify session telemetry recorded the floor clamp
    const auto stats = engine.get_session_stats();
    TEST_ASSERT_EQ(stats.total_floor_clamps, 1);
}

TEST_CASE(AnimationLock, DryRunMode) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.dry_run = true; // Observe only

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    engine.record_action_request(0x1A52, 103, t0);
    const auto t_recv = t0 + std::chrono::milliseconds(120);
    const auto res = engine.calculate_mitigation(0x1A52, 103, 600.0, t_recv);

    // Calculations are performed for metrics, but applied must be false
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 105.0, 0.5);
    TEST_ASSERT(!res.applied);

    // Dry-run mode must NOT accumulate session telemetry stats
    const auto stats = engine.get_session_stats();
    TEST_ASSERT_EQ(stats.total_actions_mitigated, 0);
    TEST_ASSERT_NEAR(stats.cumulative_time_saved_ms, 0.0, 0.001);
}

TEST_CASE(AnimationLock, UntrackedFallbackToSmoothedRtt) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Prime the rolling tracker with a 100ms sample
    engine.record_action_request(0x1000, 1, t0);
    (void)engine.calculate_mitigation(0x1000, 1, 600.0, t0 + std::chrono::milliseconds(100));

    // Now an untracked server effect arrives (no matching request)
    const auto res = engine.calculate_mitigation(0x9999, 999, 600.0, t0 + std::chrono::milliseconds(500));

    // Untracked actions (e.g. from party member/enemy) must NOT modify game memory
    TEST_ASSERT(!res.applied);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 600.0, 0.001);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 0.0, 0.001);
}

TEST_CASE(AnimationLock, MedianSpikeRejection) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Prime with several stable 50ms samples
    for (int i = 1; i <= 5; ++i) {
        engine.record_action_request(0x3000 + i, i, t0);
        (void)engine.calculate_mitigation(0x3000 + i, i, 600.0, t0 + std::chrono::milliseconds(50));
    }

    // Now an extreme 450ms packet hitch occurs
    engine.record_action_request(0x3010, 10, t0);
    const auto res = engine.calculate_mitigation(0x3010, 10, 600.0, t0 + std::chrono::milliseconds(450));

    // Spike filter should replace effective RTT with median RTT (~50ms)
    // Delay reduced = 50ms - 15ms = 35ms, adjusted lock = 600ms - 35ms = 565ms
    TEST_ASSERT_NEAR(res.measured_rtt_ms, 450.0, 0.5);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 35.0, 2.0);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 565.0, 2.0);
    TEST_ASSERT(res.applied);
    TEST_ASSERT(res.spike_filtered);
    TEST_ASSERT(!res.cold_start_guard);
}

TEST_CASE(AnimationLock, ConsecutiveSpikesDoNotInflateJitterOrBypassFilter) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // 1. Prime with 5 stable 50ms samples
    for (int i = 1; i <= 5; ++i) {
        engine.record_action_request(0x5000 + i, i, t0);
        (void)engine.calculate_mitigation(0x5000 + i, i, 600.0, t0 + std::chrono::milliseconds(50));
    }
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_jitter_ms(), 0.0, 1.0);

    // 2. Spike 1: Extreme 450ms packet hitch
    engine.record_action_request(0x5010, 10, t0);
    const auto res1 = engine.calculate_mitigation(0x5010, 10, 600.0, t0 + std::chrono::milliseconds(450));
    TEST_ASSERT_NEAR(res1.delay_reduced_ms, 35.0, 2.0); // Clamped to median (50 - 15 = 35)
    TEST_ASSERT(res1.spike_filtered);

    // Jitter must NOT be heavily inflated by the rejected spike
    TEST_ASSERT(engine.rtt_tracker().get_jitter_ms() < 20.0);

    // 3. Spike 2: Second consecutive 350ms spike immediately follows
    engine.record_action_request(0x5011, 11, t0);
    const auto res2 = engine.calculate_mitigation(0x5011, 11, 600.0, t0 + std::chrono::milliseconds(350));

    // Spike 2 must ALSO be caught by the filter and replaced with median RTT (~50ms)
    // rather than bypassing the filter and over-reducing the lock
    TEST_ASSERT_NEAR(res2.delay_reduced_ms, 35.0, 2.0);
    TEST_ASSERT_NEAR(res2.adjusted_lock_ms, 565.0, 2.0);
    TEST_ASSERT(res2.applied);
    TEST_ASSERT(res2.spike_filtered);
}

TEST_CASE(AnimationLock, AbsoluteAntiCheatFloorEnforcement) {
    mitigator::MitigationConfig cfg{};
    cfg.min_animation_lock_ms = 5.0; // Attempt invalid low floor
    mitigator::AnimationLockMitigator engine(cfg);

    // Should be clamped to at least 20.0ms
    TEST_ASSERT(engine.get_config().min_animation_lock_ms >= 20.0);

    engine.set_min_animation_lock_ms(0.0);
    TEST_ASSERT_NEAR(engine.get_config().min_animation_lock_ms, 20.0, 0.001);

    engine.set_min_animation_lock_ms(35.0);
    TEST_ASSERT_NEAR(engine.get_config().min_animation_lock_ms, 35.0, 0.001);
}

TEST_CASE(AnimationLock, CumulativeStatistics) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    for (int i = 1; i <= 5; ++i) {
        engine.record_action_request(0x2000 + i, i, t0);
        (void)engine.calculate_mitigation(0x2000 + i, i, 500.0, t0 + std::chrono::milliseconds(115));
    }

    const auto stats = engine.get_session_stats();
    TEST_ASSERT_EQ(stats.total_actions_requested, 5);
    TEST_ASSERT_EQ(stats.total_actions_mitigated, 5);
    // Each action saved 100ms (115 - 15 = 100ms) -> total 500ms
    TEST_ASSERT_NEAR(stats.cumulative_time_saved_ms, 500.0, 2.0);
}

TEST_CASE(AnimationLock, ActiveCastPreservesAnimationLock) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Start casting a spell (e.g. Fire IV, 2.8s cast time)
    engine.record_cast_begin(0x0E05, 2.8f, t0);

    // Record request for the spell
    engine.record_action_request(0x0E05, 50, t0);

    // 100ms later during cast, an action effect arrives with standard 100ms cast lock
    const auto t_recv = t0 + std::chrono::milliseconds(100);
    const auto res = engine.calculate_mitigation(0x0E05, 50, 100.0, t_recv);

    // Active cast must NOT have its animation lock reduced (prevents slide-cast clipping)
    TEST_ASSERT(res.cast_active);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 100.0, 0.001);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 0.0, 0.001);
    TEST_ASSERT(!res.applied);
}

TEST_CASE(AnimationLock, CastGraceWindowProtectsLateServerAck) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Cast 2.0s spell
    engine.record_cast_begin(0x0E06, 2.0f, t0);
    engine.record_action_request(0x0E06, 51, t0, true, 2.0f);

    // Server ack arrives at 2.06s (cast duration elapsed, but inside dynamic grace window)
    const auto t_recv = t0 + std::chrono::milliseconds(2060);
    const auto res = engine.calculate_mitigation(0x0E06, 51, 100.0, t_recv);

    // Lock must be preserved (not reduced) because grace window protects it
    TEST_ASSERT(res.cast_active);
    TEST_ASSERT(!res.applied);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 100.0, 0.001);

    // Subsequent instant action at 2.15s should now be clear to mitigate
    const auto t_instant = t0 + std::chrono::milliseconds(2150);
    engine.record_action_request(0x0E07, 52, t_instant, false, 0.0f);
    const auto res_instant = engine.calculate_mitigation(0x0E07, 52, 600.0, t_instant + std::chrono::milliseconds(80));
    TEST_ASSERT(!res_instant.cast_active);
    TEST_ASSERT(res_instant.applied);
}

TEST_CASE(AnimationLock, MaxAnimationLockCeilingClamping) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.max_animation_lock_ms = 2500.0; // 2.5s ceiling

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // 1. High incoming animation lock (Limit Break 3: 8000ms) with small 40ms RTT
    engine.record_action_request(0x0ABC, 60, t0);
    const auto t_recv = t0 + std::chrono::milliseconds(40);
    const auto res = engine.calculate_mitigation(0x0ABC, 60, 8000.0, t_recv);

    // Target lock = 8000 - (40 - 15) = 7975ms
    // Relative latency invariant: Extended locks (LB 3.8s-8.0s, potions 1.2s) must NOT be truncated to 2500ms
    TEST_ASSERT(!res.clamped_by_ceiling);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 25.0, 0.001);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 7975.0, 0.001);
    TEST_ASSERT(res.applied);

    // 2. Limit Break 1/2 (3800ms lock)
    const auto t1 = t0 + std::chrono::milliseconds(1000);
    engine.record_action_request(0x0ABD, 61, t1);
    const auto res_lb1 = engine.calculate_mitigation(0x0ABD, 61, 3800.0, t1 + std::chrono::milliseconds(40));
    TEST_ASSERT(!res_lb1.clamped_by_ceiling);
    TEST_ASSERT_NEAR(res_lb1.delay_reduced_ms, 25.0, 0.001);
    TEST_ASSERT_NEAR(res_lb1.adjusted_lock_ms, 3775.0, 0.001);
    TEST_ASSERT(res_lb1.applied);
}

TEST_CASE(AnimationLock, StandardActionRunawayCeilingClamping) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.max_animation_lock_ms = 1000.0;
    cfg.min_animation_lock_ms = 1200.0; // Floor configured higher than ceiling forces runaway target_lock

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Standard action (original_lock_ms <= max_animation_lock_ms)
    engine.record_action_request(0x0ABD, 61, t0);
    const auto t_recv = t0 + std::chrono::milliseconds(40);
    const auto res = engine.calculate_mitigation(0x0ABD, 61, 800.0, t_recv);

    // Floor clamp pushed target_lock to 1200ms (> 1000ms max).
    // Because original_lock_ms (800ms) <= max_animation_lock_ms (1000ms), ceiling clamp must trigger.
    TEST_ASSERT(res.clamped_by_ceiling);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 1000.0, 0.001);
}

TEST_CASE(AnimationLock, ConservativeSafetyMargin) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.safety_margin_ms = 10.0; // 10ms conservative buffer to prevent over-reduction

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    engine.record_action_request(0x1234, 70, t0);
    const auto t_recv = t0 + std::chrono::milliseconds(150);
    const auto res = engine.calculate_mitigation(0x1234, 70, 600.0, t_recv);

    // Latency delta = (150 - 15) - 10 = 125ms reduction
    // Adjusted lock = 600 - 125 = 475ms
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 125.0, 0.01);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 475.0, 0.01);
}

TEST_CASE(AnimationLock, SubsequentInstantActionAfterCastIsMitigated) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // 1. Cast a spell with 2.5s cast time
    engine.record_cast_begin(0x4001, 2.5f, t0);
    engine.record_action_request(0x4001, 80, t0, true, 2.5f);

    // 2. Cast completes after 2.5s + 60ms latency = 2560ms
    const auto t_cast_done = t0 + std::chrono::milliseconds(2560);
    const auto res_cast = engine.calculate_mitigation(0x4001, 80, 100.0, t_cast_done);

    // Cast lock must be preserved without reduction
    TEST_ASSERT(res_cast.cast_active);
    TEST_ASSERT(!res_cast.applied);
    TEST_ASSERT_NEAR(res_cast.adjusted_lock_ms, 100.0, 0.001);
    TEST_ASSERT_NEAR(res_cast.delay_reduced_ms, 0.0, 0.001);

    // Cast duration must NOT be sampled into RTT tracker (RTT remains baseline, not 2560ms)
    TEST_ASSERT(engine.rtt_tracker().get_smoothed_rtt_ms() < 200.0);

    // 3. Immediately dispatch an instant action after cast completes
    const auto t_instant = t_cast_done + std::chrono::milliseconds(50);
    engine.record_action_request(0x4002, 81, t_instant, false, 0.0f);

    // 4. Server responds 70ms later with 500ms animation lock
    const auto t_instant_recv = t_instant + std::chrono::milliseconds(70);
    const auto res_instant = engine.calculate_mitigation(0x4002, 81, 500.0, t_instant_recv);

    // Instant action MUST be mitigated and NOT blocked by leftover cast state
    TEST_ASSERT(!res_instant.cast_active);
    TEST_ASSERT(res_instant.applied);
    // Latency delta: 70ms RTT - 15ms target = 55ms reduction
    TEST_ASSERT_NEAR(res_instant.delay_reduced_ms, 55.0, 1.0);
    TEST_ASSERT_NEAR(res_instant.adjusted_lock_ms, 445.0, 1.0);
}

TEST_CASE(AnimationLock, ColdStartSpikeRejectionPreventsFloorClamp) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Sample 1: Action 1 establishes baseline of 80ms RTT
    engine.record_action_request(0x1001, 1, t0);
    const auto res1 = engine.calculate_mitigation(0x1001, 1, 600.0, t0 + std::chrono::milliseconds(80));
    TEST_ASSERT(res1.applied);
    TEST_ASSERT(!res1.clamped_by_floor);
    TEST_ASSERT(!res1.cold_start_guard);
    TEST_ASSERT(!res1.spike_filtered);
    TEST_ASSERT_NEAR(res1.adjusted_lock_ms, 535.0, 1.0);

    // Sample 2: Action 2 suffers an extreme 550ms opening burst / queuing delay
    const auto t1 = t0 + std::chrono::milliseconds(600);
    engine.record_action_request(0x1002, 2, t1);
    const auto res2 = engine.calculate_mitigation(0x1002, 2, 600.0, t1 + std::chrono::milliseconds(550));

    // With cold-start protection, effective RTT is capped to baseline (80ms) + 50ms = 130ms
    // Adjusted lock = 600 - (130 - 15) = 485ms (well above 25ms floor)
    TEST_ASSERT(!res2.clamped_by_floor);
    TEST_ASSERT(res2.cold_start_guard);
    TEST_ASSERT(!res2.spike_filtered);
    TEST_ASSERT_NEAR(res2.adjusted_lock_ms, 485.0, 2.0);
    TEST_ASSERT(res2.adjusted_lock_ms > 400.0);

    // Verify session telemetry: zero floor clamps occurred
    const auto stats = engine.get_session_stats();
    TEST_ASSERT_EQ(stats.total_floor_clamps, 0);

    // Sample 3 & 4: Ingest stable samples to bring sample count to 4
    const auto t2 = t1 + std::chrono::milliseconds(600);
    engine.record_action_request(0x1003, 3, t2);
    (void)engine.calculate_mitigation(0x1003, 3, 600.0, t2 + std::chrono::milliseconds(80));

    const auto t3 = t2 + std::chrono::milliseconds(600);
    engine.record_action_request(0x1004, 4, t3);
    (void)engine.calculate_mitigation(0x1004, 4, 600.0, t3 + std::chrono::milliseconds(80));

    // Sample 5: samples_before = 4 (< 5), still guarded by cold_start_guard
    const auto t4 = t3 + std::chrono::milliseconds(600);
    engine.record_action_request(0x1005, 5, t4);
    const auto res5 = engine.calculate_mitigation(0x1005, 5, 600.0, t4 + std::chrono::milliseconds(500));
    TEST_ASSERT(res5.cold_start_guard);
    TEST_ASSERT(!res5.spike_filtered);

    // Sample 6: samples_before = 5 (>= MIN_SAMPLES_FOR_MEDIAN_FILTER = 5), switches to full median filter
    const auto t5 = t4 + std::chrono::milliseconds(600);
    engine.record_action_request(0x1006, 6, t5);
    const auto res6 = engine.calculate_mitigation(0x1006, 6, 600.0, t5 + std::chrono::milliseconds(500));
    TEST_ASSERT(!res6.cold_start_guard);
    TEST_ASSERT(res6.spike_filtered);
}

TEST_CASE(AnimationLock, ConcurrentAccessStressTest) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    constexpr int NUM_ACTIONS = 200;
    std::atomic<bool> start_flag{false};
    std::atomic<int> completed_mitigations{0};

    // Thread 1: Dispatches action requests
    std::thread t_dispatch([&]() {
        while (!start_flag.load()) { std::this_thread::yield(); }
        for (int i = 1; i <= NUM_ACTIONS; ++i) {
            engine.record_action_request(0x3000 + (i % 10), i, t0 + std::chrono::milliseconds(i * 5));
        }
    });

    // Thread 2: Processes incoming action effects
    std::thread t_effects([&]() {
        while (!start_flag.load()) { std::this_thread::yield(); }
        for (int i = 1; i <= NUM_ACTIONS; ++i) {
            const auto res = engine.calculate_mitigation(
                0x3000 + (i % 10),
                i,
                600.0,
                t0 + std::chrono::milliseconds(i * 5 + 60)
            );
            if (res.applied) {
                completed_mitigations.fetch_add(1);
            }
        }
    });

    // Thread 3: Dynamic config changes during live mitigation
    std::thread t_config([&]() {
        while (!start_flag.load()) { std::this_thread::yield(); }
        for (int i = 0; i < 50; ++i) {
            engine.set_target_ping_ms(10.0 + (i % 15));
            engine.set_dry_run((i % 2) == 0);
            (void)engine.get_config();
        }
    });

    // Thread 4: Telemetry reader thread
    std::thread t_reader([&]() {
        while (!start_flag.load()) { std::this_thread::yield(); }
        for (int i = 0; i < 50; ++i) {
            const auto stats = engine.get_session_stats();
            (void)stats;
        }
    });

    start_flag.store(true);
    t_dispatch.join();
    t_effects.join();
    t_config.join();
    t_reader.join();

    // Verify system remained stable, no deadlocks occurred, and all requested actions counted
    const auto final_stats = engine.get_session_stats();
    TEST_ASSERT_EQ(final_stats.total_actions_requested, NUM_ACTIONS);
}

TEST_CASE(AnimationLock, InitialActionQueueDelayProtectedByColdStartGuard) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // The very first action (sample 0) suffers an opening 500ms delay (e.g. queued before pull)
    engine.record_action_request(0x1000, 1, t0);
    const auto res = engine.calculate_mitigation(0x1000, 1, 600.0, t0 + std::chrono::milliseconds(500));

    // Must be capped by cold_start_guard to 200ms rather than causing a 25ms floor clamp
    TEST_ASSERT(res.cold_start_guard);
    TEST_ASSERT(!res.clamped_by_floor);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 415.0, 2.0); // 600 - (200 - 15) = 415ms
    TEST_ASSERT_EQ(engine.get_session_stats().total_floor_clamps, 0);
}

TEST_CASE(AnimationLock, QueuedActionAppliesBaselineWithoutPoisoningRtt) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Prime the tracker with stable 40ms samples so baseline RTT is firmly established
    for (int i = 1; i <= 5; ++i) {
        engine.record_action_request(0x2000 + i, i, t0);
        (void)engine.calculate_mitigation(0x2000 + i, i, 600.0, t0 + std::chrono::milliseconds(40));
    }
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_smoothed_rtt_ms(), 40.0, 1.0);
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_median_rtt_ms(), 40.0, 1.0);
    const size_t samples_before = engine.rtt_tracker().sample_count();

    // Dispatch a queued action (held in client buffer, 400ms dwell time before server response)
    const auto t_queued = t0 + std::chrono::milliseconds(500);
    engine.record_action_request(0x3001, 50, t_queued, false, 0.0f, true /* is_queued */);

    // Server responds 400ms after client queue submission
    const auto t_recv = t_queued + std::chrono::milliseconds(400);
    const auto res = engine.calculate_mitigation(0x3001, 50, 600.0, t_recv);

    // Queued action must use smoothed baseline RTT (40ms) rather than 400ms queue elapsed time
    // Latency delta: 40ms - 15ms = 25ms reduction -> adjusted lock 575ms
    TEST_ASSERT(res.queued_action);
    TEST_ASSERT(res.applied);
    TEST_ASSERT_NEAR(res.measured_rtt_ms, 400.0, 0.5);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 25.0, 1.0);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 575.0, 1.0);

    // Tracker must NOT be poisoned by the 400ms queue dwell time
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_smoothed_rtt_ms(), 40.0, 1.0);
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_median_rtt_ms(), 40.0, 1.0);
    TEST_ASSERT_EQ(engine.rtt_tracker().sample_count(), samples_before);
    TEST_ASSERT(!res.spike_filtered);
    TEST_ASSERT(!res.route_shift_reseeded);
}

TEST_CASE(AnimationLock, SustainedRouteShiftReseedsOutlierWindow) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // 1. Prime tracker with stable 40ms baseline (5 samples)
    for (int i = 1; i <= 5; ++i) {
        engine.record_action_request(0x4000 + i, i, t0);
        (void)engine.calculate_mitigation(0x4000 + i, i, 600.0, t0 + std::chrono::milliseconds(40));
    }
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_median_rtt_ms(), 40.0, 1.0);
    TEST_ASSERT_EQ(engine.consecutive_outliers(), 0);

    // 2. Ingest 3 outlier samples (120ms ping vs 40ms median + 50ms tolerance = 90ms threshold)
    // Verify spike filtering remains active for transient spikes (< 4)
    for (int i = 1; i <= 3; ++i) {
        const auto t_req = t0 + std::chrono::milliseconds(i * 1000);
        engine.record_action_request(0x5000 + i, 10 + i, t_req);
        const auto res = engine.calculate_mitigation(0x5000 + i, 10 + i, 600.0, t_req + std::chrono::milliseconds(120));

        TEST_ASSERT(res.spike_filtered);
        TEST_ASSERT(!res.route_shift_reseeded);
        // Mitigated using clamped median RTT (40ms - 15ms = 25ms)
        TEST_ASSERT_NEAR(res.delay_reduced_ms, 25.0, 2.0);
        TEST_ASSERT_EQ(engine.consecutive_outliers(), static_cast<size_t>(i));
    }

    // 3. Ingest 4th outlier sample -> reaches CONSECUTIVE_OUTLIER_RESEED_THRESHOLD (4)
    const auto t_shift = t0 + std::chrono::milliseconds(4000);
    engine.record_action_request(0x5004, 14, t_shift);
    const auto res4 = engine.calculate_mitigation(0x5004, 14, 600.0, t_shift + std::chrono::milliseconds(120));

    // Window must reseed to new route baseline (120ms)
    TEST_ASSERT(res4.route_shift_reseeded);
    TEST_ASSERT(!res4.spike_filtered);
    TEST_ASSERT_EQ(engine.consecutive_outliers(), 0);
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_median_rtt_ms(), 120.0, 1.0);
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_smoothed_rtt_ms(), 120.0, 1.0);

    // Reseeded action applies reduction according to new 120ms baseline (120 - 15 = 105ms)
    TEST_ASSERT_NEAR(res4.delay_reduced_ms, 105.0, 2.0);
    TEST_ASSERT_NEAR(res4.adjusted_lock_ms, 495.0, 2.0);

    // 4. Subsequent sample at 120ms adapts cleanly from new baseline
    const auto t_subsequent = t0 + std::chrono::milliseconds(5000);
    engine.record_action_request(0x5005, 15, t_subsequent);
    const auto res5 = engine.calculate_mitigation(0x5005, 15, 600.0, t_subsequent + std::chrono::milliseconds(120));

    TEST_ASSERT(!res5.route_shift_reseeded);
    TEST_ASSERT(!res5.spike_filtered);
    TEST_ASSERT_NEAR(res5.adjusted_lock_ms, 495.0, 2.0);
}

TEST_CASE(AnimationLock, TransientSpikesResetConsecutiveOutlierCounter) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // 1. Prime tracker with 5 stable 40ms samples
    for (int i = 1; i <= 5; ++i) {
        engine.record_action_request(0x6000 + i, i, t0);
        (void)engine.calculate_mitigation(0x6000 + i, i, 600.0, t0 + std::chrono::milliseconds(40));
    }
    TEST_ASSERT_EQ(engine.consecutive_outliers(), 0);

    // 2. Ingest 2 transient spikes
    for (int i = 1; i <= 2; ++i) {
        const auto t_spike = t0 + std::chrono::milliseconds(i * 1000);
        engine.record_action_request(0x6010 + i, 10 + i, t_spike);
        const auto res = engine.calculate_mitigation(0x6010 + i, 10 + i, 600.0, t_spike + std::chrono::milliseconds(120));
        TEST_ASSERT(res.spike_filtered);
        TEST_ASSERT_EQ(engine.consecutive_outliers(), static_cast<size_t>(i));
    }

    // 3. Normal 40ms sample arrives -> consecutive outliers must reset to 0
    const auto t_normal = t0 + std::chrono::milliseconds(3000);
    engine.record_action_request(0x6020, 20, t_normal);
    const auto res_normal = engine.calculate_mitigation(0x6020, 20, 600.0, t_normal + std::chrono::milliseconds(40));
    TEST_ASSERT(!res_normal.spike_filtered);
    TEST_ASSERT(!res_normal.route_shift_reseeded);
    TEST_ASSERT_EQ(engine.consecutive_outliers(), 0);

    // 4. One more spike occurs -> counter starts over at 1 (not 3)
    const auto t_spike3 = t0 + std::chrono::milliseconds(4000);
    engine.record_action_request(0x6021, 21, t_spike3);
    const auto res_spike3 = engine.calculate_mitigation(0x6021, 21, 600.0, t_spike3 + std::chrono::milliseconds(120));
    TEST_ASSERT(res_spike3.spike_filtered);
    TEST_ASSERT(!res_spike3.route_shift_reseeded);
    TEST_ASSERT_EQ(engine.consecutive_outliers(), 1);

    // 5. Engine reset clears consecutive outliers counter
    engine.reset();
    TEST_ASSERT_EQ(engine.consecutive_outliers(), 0);
}

TEST_CASE(AnimationLock, HighPingRouteShiftAdaptsWithoutFalseColdStartClamping) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // 1. Prime tracker with stable 40ms baseline (5 samples)
    for (int i = 1; i <= 5; ++i) {
        engine.record_action_request(0x7000 + i, i, t0);
        (void)engine.calculate_mitigation(0x7000 + i, i, 600.0, t0 + std::chrono::milliseconds(40));
    }
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_median_rtt_ms(), 40.0, 1.0);

    // 2. Route shifts to high ping (250ms, e.g. international cross-region)
    // First 3 samples are spike filtered
    for (int i = 1; i <= 3; ++i) {
        const auto t_req = t0 + std::chrono::milliseconds(i * 1000);
        engine.record_action_request(0x7010 + i, 10 + i, t_req);
        const auto res = engine.calculate_mitigation(0x7010 + i, 10 + i, 600.0, t_req + std::chrono::milliseconds(250));
        TEST_ASSERT(res.spike_filtered);
        TEST_ASSERT(!res.route_shift_reseeded);
        TEST_ASSERT_EQ(engine.consecutive_outliers(), static_cast<size_t>(i));
    }

    // 3. 4th sample triggers route reseed to 250ms
    const auto t_shift = t0 + std::chrono::milliseconds(4000);
    engine.record_action_request(0x7014, 14, t_shift);
    const auto res4 = engine.calculate_mitigation(0x7014, 14, 600.0, t_shift + std::chrono::milliseconds(250));

    TEST_ASSERT(res4.route_shift_reseeded);
    TEST_ASSERT(!res4.spike_filtered);
    TEST_ASSERT_EQ(engine.consecutive_outliers(), 0);
    TEST_ASSERT_NEAR(engine.rtt_tracker().get_smoothed_rtt_ms(), 250.0, 1.0);

    // 4. Crucial verification: 5th sample arrives at 250ms (samples_before == 0 due to reset).
    // It MUST NOT be falsely clamped by cold_start_guard to 200ms!
    const auto t_subsequent = t0 + std::chrono::milliseconds(5000);
    engine.record_action_request(0x7015, 15, t_subsequent);
    const auto res5 = engine.calculate_mitigation(0x7015, 15, 600.0, t_subsequent + std::chrono::milliseconds(250));

    TEST_ASSERT(!res5.route_shift_reseeded);
    TEST_ASSERT(!res5.spike_filtered);
    TEST_ASSERT(!res5.cold_start_guard); // Must NOT be flagged as cold start outlier!
    // Expected reduction: 250ms - 15ms = 235ms -> adjusted lock: 600ms - 235ms = 365ms
    TEST_ASSERT_NEAR(res5.delay_reduced_ms, 235.0, 2.0);
    TEST_ASSERT_NEAR(res5.adjusted_lock_ms, 365.0, 2.0);
}

TEST_CASE(AnimationLock, QueuedCastPreservesLockAndReportsQueuedTelemetry) {
    mitigator::MitigationConfig cfg{};
    cfg.target_ping_ms = 15.0;

    mitigator::AnimationLockMitigator engine(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Begin cast for a 2.5s spell (e.g. Fire IV)
    engine.record_cast_begin(0x0E05, 2.5f, t0);

    // Record queued cast request (buffered during GCD)
    engine.record_action_request(0x0E05, 80, t0, true /* is_cast */, 2.5f, true /* is_queued */);

    // Server effect arrives
    const auto t_recv = t0 + std::chrono::milliseconds(100);
    const auto res = engine.calculate_mitigation(0x0E05, 80, 100.0, t_recv);

    // Cast lock must be preserved, and queued_action flag must be accurately reported
    TEST_ASSERT(res.cast_active);
    TEST_ASSERT(res.queued_action);
    TEST_ASSERT(!res.applied);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 100.0, 0.001);
}



