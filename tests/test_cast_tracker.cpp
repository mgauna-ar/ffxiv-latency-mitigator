#include "test_framework.hpp"
#include "mitigator/cast_tracker.hpp"

TEST_CASE(CastTracker, InitialState) {
    mitigator::CastTracker tracker;
    TEST_ASSERT(!tracker.is_casting());
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0);
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(), 0.0f, 0.001f);
}

TEST_CASE(CastTracker, CastBeginAndRemaining) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    tracker.on_cast_begin(0x00FF, 2.5f, t0);
    TEST_ASSERT(tracker.is_casting(t0));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0x00FF);
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t0), 2.5f, 0.01f);

    // 1 second later
    const auto t1 = t0 + std::chrono::milliseconds(1000);
    TEST_ASSERT(tracker.is_casting(t1));
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t1), 1.5f, 0.01f);
}

TEST_CASE(CastTracker, CastInterrupt) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    tracker.on_cast_begin(0x00AA, 3.0f, t0);
    TEST_ASSERT(tracker.is_casting(t0));

    tracker.on_cast_interrupt(t0 + std::chrono::milliseconds(500));
    TEST_ASSERT(!tracker.is_casting(t0 + std::chrono::milliseconds(500)));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0);
}

TEST_CASE(CastTracker, CastEnd) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    tracker.on_cast_begin(0x00BB, 1.5f, t0);
    tracker.on_cast_end(t0 + std::chrono::milliseconds(1500));
    TEST_ASSERT(!tracker.is_casting(t0 + std::chrono::milliseconds(1500)));
}

TEST_CASE(CastTracker, DynamicGraceWindowWithHighPing) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // 2.0s cast
    tracker.on_cast_begin(0x00CC, 2.0f, t0);

    // At 2.15s: elapsed = 2.15s.
    // Default 100ms grace window would expire at 2.10s!
    // With 250ms smoothed RTT, grace window = max(0.100, 250/2000 + 0.050) = 0.175s -> expires at 2.175s.
    const auto t_check = t0 + std::chrono::milliseconds(2150);
    TEST_ASSERT(!tracker.is_casting(t_check, 0.0)); // Expired with default 100ms window
    TEST_ASSERT(tracker.is_casting(t_check, 250.0)); // Still protected with dynamic RTT grace
}

TEST_CASE(CastTracker, AbsoluteCastTimeoutEvictsStaleState) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // Player begins 5.0s Teleport cast, then enters loading screen / zone transition
    tracker.on_cast_begin(0x00DD, 5.0f, t0);
    TEST_ASSERT(tracker.is_casting(t0));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0x00DD);

    // 35 seconds later (well past ABSOLUTE_MAX_CAST_DURATION_SECONDS = 30s)
    const auto t_zone = t0 + std::chrono::seconds(35);
    TEST_ASSERT(!tracker.is_casting(t_zone));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0);
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t_zone), 0.0f, 0.001f);
}

TEST_CASE(CastTracker, NegativeElapsedDoesNotEvaluateAsCasting) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    tracker.on_cast_begin(0x00EE, 2.5f, t0);

    // Simulated clock jump backwards
    const auto t_before = t0 - std::chrono::seconds(5);
    TEST_ASSERT(!tracker.is_casting(t_before));
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t_before), 0.0f, 0.001f);
}

TEST_CASE(CastTracker, RapidConsecutiveCastsTransitionCleanly) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // Cast 1: 2.0s spell
    tracker.on_cast_begin(0x1001, 2.0f, t0);
    TEST_ASSERT(tracker.is_casting(t0));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0x1001);

    // Cast 1 finishes at 2.0s
    const auto t1 = t0 + std::chrono::milliseconds(2000);
    tracker.on_cast_end(t1);
    TEST_ASSERT(!tracker.is_casting(t1));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0);

    // Cast 2: begins 50ms later (e.g. chained spell)
    const auto t2 = t1 + std::chrono::milliseconds(50);
    tracker.on_cast_begin(0x1002, 1.5f, t2);
    TEST_ASSERT(tracker.is_casting(t2));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0x1002);
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t2), 1.5f, 0.01f);

    // Mid-cast check for Cast 2
    const auto t3 = t2 + std::chrono::milliseconds(500);
    TEST_ASSERT(tracker.is_casting(t3));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0x1002);
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t3), 1.0f, 0.01f);
}

TEST_CASE(CastTracker, InterruptedCastAllowsImmediateNewCast) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // Cast 1: 3.0s spell begins
    tracker.on_cast_begin(0x2001, 3.0f, t0);
    TEST_ASSERT(tracker.is_casting(t0));

    // Player moves / interrupted at 800ms
    const auto t_int = t0 + std::chrono::milliseconds(800);
    tracker.on_cast_interrupt(t_int);
    TEST_ASSERT(!tracker.is_casting(t_int));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0);

    // Immediately starts new instant action / cast at 850ms
    const auto t_new = t_int + std::chrono::milliseconds(50);
    tracker.on_cast_begin(0x2002, 1.0f, t_new);
    TEST_ASSERT(tracker.is_casting(t_new));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0x2002);
}

TEST_CASE(CastTracker, ZeroAndNegativeDurationCastDoesNotEvaluateAsCasting) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // 0.0s cast (instant action erroneously passed to cast tracker)
    tracker.on_cast_begin(0x3001, 0.0f, t0);
    TEST_ASSERT(!tracker.is_casting(t0));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0);
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t0), 0.0f, 0.001f);

    // Negative cast duration
    tracker.on_cast_begin(0x3002, -2.5f, t0);
    TEST_ASSERT(!tracker.is_casting(t0));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0);
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t0), 0.0f, 0.001f);

    // Non-finite cast duration (NaN)
    tracker.on_cast_begin(0x3003, std::numeric_limits<float>::quiet_NaN(), t0);
    TEST_ASSERT(!tracker.is_casting(t0));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 0);
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t0), 0.0f, 0.001f);
}

TEST_CASE(CastTracker, DynamicGraceWindowBoundaryConditions) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // 2.0s cast
    tracker.on_cast_begin(0x4001, 2.0f, t0);

    // Base grace window without RTT: 100ms -> expires at 2100ms
    const auto t_before_base = t0 + std::chrono::milliseconds(2090);
    const auto t_after_base = t0 + std::chrono::milliseconds(2110);
    TEST_ASSERT(tracker.is_casting(t_before_base, 0.0));
    TEST_ASSERT(!tracker.is_casting(t_after_base, 0.0));

    // Dynamic grace window with 200ms RTT: max(0.100, 200/2000 + 0.050) = 0.150s (150ms) -> expires at 2150ms
    const auto t_before_dyn = t0 + std::chrono::milliseconds(2140);
    const auto t_after_dyn = t0 + std::chrono::milliseconds(2160);
    TEST_ASSERT(tracker.is_casting(t_before_dyn, 200.0));
    TEST_ASSERT(!tracker.is_casting(t_after_dyn, 200.0));
}

TEST_CASE(CastTracker, ExtremeCastDurationClampedToAbsoluteMax) {
    mitigator::CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // Corrupted 999.0s cast duration from memory
    tracker.on_cast_begin(0x4002, 999.0f, t0);
    TEST_ASSERT(tracker.is_casting(t0));
    // Must be clamped to ABSOLUTE_MAX_CAST_DURATION_SECONDS (30.0s)
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t0), mitigator::constants::ABSOLUTE_MAX_CAST_DURATION_SECONDS, 0.001f);

    // After 30s + grace window, expires cleanly
    const auto t_after_max = t0 + std::chrono::seconds(31);
    TEST_ASSERT(!tracker.is_casting(t_after_max));
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t_after_max), 0.0f, 0.001f);
}



