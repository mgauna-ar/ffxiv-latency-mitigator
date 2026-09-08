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
