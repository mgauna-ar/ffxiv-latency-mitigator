#include "test_framework.hpp"
#include "mitigator/sequence_tracker.hpp"
#include <thread>
#include <vector>

TEST_CASE(SequenceTracker, ExactSequenceMatch) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    tracker.record_request(0x1001, 42, now);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    auto matched = tracker.match_response(0x1001, 42, now + std::chrono::milliseconds(80));
    TEST_ASSERT(matched.has_value());
    TEST_ASSERT_EQ(matched->action_id, 0x1001);
    TEST_ASSERT_EQ(matched->sequence, 42);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, ActionIdFallbackWhenSequenceZero) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Client uses action without sequence (sequence = 0)
    tracker.record_request(0x2001, 0, now);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Server sends effect with sequence = 0
    auto matched = tracker.match_response(0x2001, 0, now + std::chrono::milliseconds(110));
    TEST_ASSERT(matched.has_value());
    TEST_ASSERT_EQ(matched->action_id, 0x2001);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, OutOfOrderMatching) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Two actions dispatched in quick succession (double weaving)
    tracker.record_request(0x3001, 10, now);
    tracker.record_request(0x3002, 11, now + std::chrono::milliseconds(5));
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    // Server responds to action 11 first (out of order packet arrival)
    auto matched_second = tracker.match_response(0x3002, 11, now + std::chrono::milliseconds(80));
    TEST_ASSERT(matched_second.has_value());
    TEST_ASSERT_EQ(matched_second->action_id, 0x3002);
    TEST_ASSERT_EQ(matched_second->sequence, 11);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Server responds to action 10 second
    auto matched_first = tracker.match_response(0x3001, 10, now + std::chrono::milliseconds(95));
    TEST_ASSERT(matched_first.has_value());
    TEST_ASSERT_EQ(matched_first->action_id, 0x3001);
    TEST_ASSERT_EQ(matched_first->sequence, 10);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, PruneStaleRequests) {
    mitigator::SequenceTracker tracker(std::chrono::milliseconds(200));
    const auto t0 = std::chrono::steady_clock::now();

    tracker.record_request(0x4001, 1, t0);
    tracker.record_request(0x4002, 2, t0 + std::chrono::milliseconds(50));
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    // Advance clock past the 200ms timeout
    const auto t_later = t0 + std::chrono::milliseconds(300);
    const size_t pruned = tracker.prune_stale(t_later);
    TEST_ASSERT_EQ(pruned, 2);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, NonExistentResponse) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    tracker.record_request(0x5001, 99, now);

    // Non-existent action or sequence should return nullopt
    auto res = tracker.match_response(0x9999, 999, now);
    TEST_ASSERT(!res.has_value());
    TEST_ASSERT_EQ(tracker.pending_count(), 1);
}

TEST_CASE(SequenceTracker, ConcurrentRequestsAndResponses) {
    mitigator::SequenceTracker tracker(std::chrono::milliseconds(5000));
    constexpr int NUM_PAIRS = 50;

    std::thread sender([&tracker]() {
        for (int i = 1; i <= NUM_PAIRS; ++i) {
            tracker.record_request(0x1000 + i, i);
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    });

    std::thread receiver([&tracker]() {
        for (int i = 1; i <= NUM_PAIRS; ++i) {
            // Poll for response
            bool matched = false;
            for (int retry = 0; retry < 50; ++retry) {
                if (tracker.match_response(0x1000 + i, i).has_value()) {
                    matched = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(150));
            }
            (void)matched;
        }
    });

    sender.join();
    receiver.join();
}

TEST_CASE(SequenceTracker, NonZeroServerSequenceWithRealClientSequence) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Two actions with real sequence numbers (as read from ActionManager->current_sequence)
    tracker.record_request(0x1001, 101, now);
    tracker.record_request(0x1002, 102, now + std::chrono::milliseconds(2));

    // Response for second action arrives first
    auto m2 = tracker.match_response(0x1002, 102, now + std::chrono::milliseconds(50));
    TEST_ASSERT(m2.has_value());
    TEST_ASSERT_EQ(m2->sequence, 102);
    TEST_ASSERT_EQ(m2->action_id, 0x1002);

    // Response for first action arrives second
    auto m1 = tracker.match_response(0x1001, 101, now + std::chrono::milliseconds(70));
    TEST_ASSERT(m1.has_value());
    TEST_ASSERT_EQ(m1->sequence, 101);
    TEST_ASSERT_EQ(m1->action_id, 0x1001);

    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, ActionIdMismatchWithZeroSequenceDoesNotMatch) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Client records request for action 0x1000
    tracker.record_request(0x1000, 0, now);
    tracker.record_request(0x2000, 0, now + std::chrono::milliseconds(5));

    // Response arrives for action 0x3000 (different action) with sequence 0
    auto m = tracker.match_response(0x3000, 0, now + std::chrono::milliseconds(50));
    TEST_ASSERT(!m.has_value());
    TEST_ASSERT_EQ(tracker.pending_count(), 2);
}

TEST_CASE(SequenceTracker, ZeroSequenceAndZeroActionIdDoesNotMatch) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Client records single pending request
    tracker.record_request(0x1000, 10, now);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Generic effect arrives with both sequence == 0 and action_id == 0
    // Must NOT match or consume the legitimate pending action
    auto m = tracker.match_response(0, 0, now + std::chrono::milliseconds(50));
    TEST_ASSERT(!m.has_value());
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Subsequent arrival of the real action response should still match successfully
    auto real_match = tracker.match_response(0x1000, 10, now + std::chrono::milliseconds(70));
    TEST_ASSERT(real_match.has_value());
    TEST_ASSERT_EQ(real_match->action_id, 0x1000);
    TEST_ASSERT_EQ(real_match->sequence, 10);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, DuplicateSequenceReplacesStaleRequest) {
    mitigator::SequenceTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // Request 1: Action 0x1001 dispatched with sequence 50
    tracker.record_request(0x1001, 50, t0);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Request 2: Re-dispatch or sequence reuse with same sequence 50 200ms later
    const auto t1 = t0 + std::chrono::milliseconds(200);
    tracker.record_request(0x1002, 50, t1);

    // Queue must NOT contain duplicate entries for the same non-zero sequence
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Response arrives at t0 + 260ms (60ms after second request)
    const auto t_recv = t0 + std::chrono::milliseconds(260);
    auto matched = tracker.match_response(0x1002, 50, t_recv);

    TEST_ASSERT(matched.has_value());
    TEST_ASSERT_EQ(matched->action_id, 0x1002);
    TEST_ASSERT_EQ(matched->sequence, 50);

    // Elapsed should be 60ms (against t1), NOT 260ms (against t0)
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t_recv - matched->timestamp).count();
    TEST_ASSERT_EQ(elapsed, 60);

    // No ghost entry left behind
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, ZeroSequenceAllowsMultiplePending) {
    mitigator::SequenceTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // Multiple distinct unsequenced actions (sequence = 0) must coexist
    tracker.record_request(0x2001, 0, t0);
    tracker.record_request(0x2002, 0, t0 + std::chrono::milliseconds(10));
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    // Matching 0x2001 leaves 0x2002
    auto m1 = tracker.match_response(0x2001, 0, t0 + std::chrono::milliseconds(50));
    TEST_ASSERT(m1.has_value());
    TEST_ASSERT_EQ(m1->action_id, 0x2001);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Matching 0x2002 empties queue
    auto m2 = tracker.match_response(0x2002, 0, t0 + std::chrono::milliseconds(60));
    TEST_ASSERT(m2.has_value());
    TEST_ASSERT_EQ(m2->action_id, 0x2002);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, ActionIdFallbackPrefersNewerRequestWhenOldestIsStale) {
    mitigator::SequenceTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();
    const auto t1 = t0 + std::chrono::milliseconds(500);
    const auto t_recv = t0 + std::chrono::milliseconds(560);

    // Two unsequenced requests for the same action_id (e.g. spammed GCD/oGCD or dropped first attempt)
    tracker.record_request(0x3000, 0, t0);
    tracker.record_request(0x3000, 0, t1);
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    // Expected RTT is 50ms. Plausibility threshold is max(100, 100) = 100ms.
    // Request 1 has elapsed 560ms (> 100ms), while Request 2 has elapsed 60ms (<= 100ms).
    auto matched = tracker.match_response(0x3000, 0, t_recv, 50.0);
    TEST_ASSERT(matched.has_value());
    TEST_ASSERT_EQ(matched->action_id, 0x3000);
    TEST_ASSERT(matched->timestamp == t1);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t_recv - matched->timestamp).count();
    TEST_ASSERT_EQ(elapsed, 60);

    // Stale Request 1 was evicted and matched Request 2 was consumed
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, ActionIdFallbackPreservesOldestWhenWithinPlausibilityThreshold) {
    mitigator::SequenceTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();
    const auto t1 = t0 + std::chrono::milliseconds(20);
    const auto t_recv = t0 + std::chrono::milliseconds(70);

    // Rapid double-press where the first request is still recent (elapsed 70ms <= 100ms threshold)
    tracker.record_request(0x3000, 0, t0);
    tracker.record_request(0x3000, 0, t1);
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    auto matched = tracker.match_response(0x3000, 0, t_recv, 50.0);
    TEST_ASSERT(matched.has_value());
    TEST_ASSERT(matched->timestamp == t0);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t_recv - matched->timestamp).count();
    TEST_ASSERT_EQ(elapsed, 70);

    // FIFO preserved: Request 2 remains in queue
    TEST_ASSERT_EQ(tracker.pending_count(), 1);
}

TEST_CASE(SequenceTracker, ActionIdFallbackWithCastingAccountsForCastDuration) {
    mitigator::SequenceTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();
    const auto t1 = t0 + std::chrono::milliseconds(2500);
    const auto t_recv = t0 + std::chrono::milliseconds(2560);

    // Two casted requests with 2.0s cast duration
    tracker.record_request(0x4000, 0, t0, true, 2.0f);
    tracker.record_request(0x4000, 0, t1, true, 2.0f);
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    // For Request 1: elapsed is 2560ms. Threshold is 2000ms + 100ms = 2100ms.
    // 2560ms > 2100ms -> Request 1 is stale.
    // For Request 2: elapsed is 60ms <= 2100ms -> matches Request 2.
    auto matched = tracker.match_response(0x4000, 0, t_recv, 50.0);
    TEST_ASSERT(matched.has_value());
    TEST_ASSERT(matched->timestamp == t1);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, ActionIdFallbackSingleMatchPreservedEvenIfElapsedExceedsThreshold) {
    mitigator::SequenceTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();
    const auto t_recv = t0 + std::chrono::milliseconds(400);

    // Single request experiencing a temporary lag spike
    tracker.record_request(0x5000, 0, t0);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Even though 400ms > 100ms threshold, with NO collision (no newer request),
    // the single match must NOT be discarded
    auto matched = tracker.match_response(0x5000, 0, t_recv, 50.0);
    TEST_ASSERT(matched.has_value());
    TEST_ASSERT_EQ(matched->action_id, 0x5000);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

