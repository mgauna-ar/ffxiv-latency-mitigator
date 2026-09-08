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
