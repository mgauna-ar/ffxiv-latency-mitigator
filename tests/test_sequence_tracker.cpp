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
