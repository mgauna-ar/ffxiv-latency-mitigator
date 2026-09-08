#include "test_framework.hpp"
#include "mitigator/animation_lock.hpp"

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

    // Effective RTT should fallback to smoothed RTT (~100ms)
    TEST_ASSERT(res.smoothed_rtt_ms > 0.0);
    TEST_ASSERT(res.delay_reduced_ms > 0.0);
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
