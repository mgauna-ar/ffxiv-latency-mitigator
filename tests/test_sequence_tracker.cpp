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

TEST_CASE(SequenceTracker, SequenceWraparoundAtUint16Boundary) {
    mitigator::SequenceTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // Request 1: Dispatched at the very edge of uint16_t (65535 / 0xFFFF)
    tracker.record_request(0x1001, 65535, t0);

    // Request 2: Next dispatch rolls over past 65535 to 1 (0x0001)
    tracker.record_request(0x1002, 1, t0 + std::chrono::milliseconds(50));
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    // Response 1 arrives for sequence 65535
    auto m1 = tracker.match_response(0x1001, 65535, t0 + std::chrono::milliseconds(80));
    TEST_ASSERT(m1.has_value());
    TEST_ASSERT_EQ(m1->action_id, 0x1001);
    TEST_ASSERT_EQ(m1->sequence, 65535);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Response 2 arrives for sequence 1
    auto m2 = tracker.match_response(0x1002, 1, t0 + std::chrono::milliseconds(120));
    TEST_ASSERT(m2.has_value());
    TEST_ASSERT_EQ(m2->action_id, 0x1002);
    TEST_ASSERT_EQ(m2->sequence, 1);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, OutOfOrderAcrossWraparoundBoundary) {
    mitigator::SequenceTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // Requests dispatched across wraparound boundary
    tracker.record_request(0x2001, 65535, t0);
    tracker.record_request(0x2002, 1, t0 + std::chrono::milliseconds(20));

    // Response for sequence 1 arrives BEFORE response for sequence 65535 (out-of-order)
    auto m2 = tracker.match_response(0x2002, 1, t0 + std::chrono::milliseconds(60));
    TEST_ASSERT(m2.has_value());
    TEST_ASSERT_EQ(m2->action_id, 0x2002);
    TEST_ASSERT_EQ(m2->sequence, 1);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Response for sequence 65535 arrives later
    auto m1 = tracker.match_response(0x2001, 65535, t0 + std::chrono::milliseconds(100));
    TEST_ASSERT(m1.has_value());
    TEST_ASSERT_EQ(m1->action_id, 0x2001);
    TEST_ASSERT_EQ(m1->sequence, 65535);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, GenericZeroSequenceFallbackMatchesUntrackedAction) {
    mitigator::SequenceTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    // Action recorded with sequence 0 (e.g. untracked client dispatch)
    tracker.record_request(0x00FF, 0, t0);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Generic effect arrives with sequence 0 and action_id 0 within 1500ms
    auto matched = tracker.match_response(0, 0, t0 + std::chrono::milliseconds(75));
    TEST_ASSERT(matched.has_value());
    TEST_ASSERT_EQ(matched->action_id, 0x00FF);
    TEST_ASSERT_EQ(matched->sequence, 0);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, ConflictingSequencesDoNotMatchOnActionId) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Client records request for action 0x1234 with sequence 10
    tracker.record_request(0x1234, 10, now);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Foreign player packet or mismatched sequence arrives for action 0x1234 with sequence 20
    // Because both sequences are non-zero and conflicting (10 != 20), Strategy 2 MUST NOT match
    auto m_mismatch = tracker.match_response(0x1234, 20, now + std::chrono::milliseconds(50));
    TEST_ASSERT(!m_mismatch.has_value());
    TEST_ASSERT_EQ(tracker.pending_count(), 1); // Request was NOT stolen

    // Correct response with sequence 10 arrives and successfully matches
    auto m_correct = tracker.match_response(0x1234, 10, now + std::chrono::milliseconds(80));
    TEST_ASSERT(m_correct.has_value());
    TEST_ASSERT_EQ(m_correct->action_id, 0x1234);
    TEST_ASSERT_EQ(m_correct->sequence, 10);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, UnsequencedActionIdFallbackMatches) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Case 1: Pending request has sequence 0 (unsequenced action), response has non-zero sequence 100
    tracker.record_request(0x2001, 0, now);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    auto m1 = tracker.match_response(0x2001, 100, now + std::chrono::milliseconds(50));
    TEST_ASSERT(m1.has_value());
    TEST_ASSERT_EQ(m1->action_id, 0x2001);
    TEST_ASSERT_EQ(m1->sequence, 0);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);

    // Case 2: Pending request has non-zero sequence 200, response arrives with sequence 0
    tracker.record_request(0x2002, 200, now);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    auto m2 = tracker.match_response(0x2002, 0, now + std::chrono::milliseconds(50));
    TEST_ASSERT(m2.has_value());
    TEST_ASSERT_EQ(m2->action_id, 0x2002);
    TEST_ASSERT_EQ(m2->sequence, 200);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, UnsequencedServerResponseMatchesUnsequencedRequest) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Both client request and server response are unsequenced (sequence = 0)
    tracker.record_request(0x3333, 0, now);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    auto matched = tracker.match_response(0x3333, 0, now + std::chrono::milliseconds(65));
    TEST_ASSERT(matched.has_value());
    TEST_ASSERT_EQ(matched->action_id, 0x3333);
    TEST_ASSERT_EQ(matched->sequence, 0);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, SequencedResponseRefusesMismatchOnSameActionId) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Pending request has sequence 100
    tracker.record_request(0x5555, 100, now);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Conflicting sequenced response arrives for sequence 200
    auto m_conflict = tracker.match_response(0x5555, 200, now + std::chrono::milliseconds(50));
    TEST_ASSERT(!m_conflict.has_value());
    TEST_ASSERT_EQ(tracker.pending_count(), 1); // Not stolen!

    // Exactly matching response arrives
    auto m_correct = tracker.match_response(0x5555, 100, now + std::chrono::milliseconds(80));
    TEST_ASSERT(m_correct.has_value());
    TEST_ASSERT_EQ(m_correct->action_id, 0x5555);
    TEST_ASSERT_EQ(m_correct->sequence, 100);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, DisambiguationWhenMultipleRequestsForSameActionIdExist) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // Spamming the same action twice:
    tracker.record_request(0x7777, 10, now);
    tracker.record_request(0x7777, 20, now + std::chrono::milliseconds(10));
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    // Conflicting non-zero sequence 30 arrives -> refuses match
    auto m_rogue = tracker.match_response(0x7777, 30, now + std::chrono::milliseconds(50));
    TEST_ASSERT(!m_rogue.has_value());
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    // Response 2 arrives out-of-order first
    auto m2 = tracker.match_response(0x7777, 20, now + std::chrono::milliseconds(70));
    TEST_ASSERT(m2.has_value());
    TEST_ASSERT_EQ(m2->sequence, 20);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // Response 1 arrives second
    auto m1 = tracker.match_response(0x7777, 10, now + std::chrono::milliseconds(90));
    TEST_ASSERT(m1.has_value());
    TEST_ASSERT_EQ(m1->sequence, 10);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}

TEST_CASE(SequenceTracker, GenericFallbackEdgeCases) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // 1. Multiple pending unsequenced requests -> generic fallback refuses (ambiguous)
    tracker.record_request(0x1111, 0, now);
    tracker.record_request(0x2222, 0, now + std::chrono::milliseconds(5));
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    auto m_ambiguous = tracker.match_response(0, 0, now + std::chrono::milliseconds(50));
    TEST_ASSERT(!m_ambiguous.has_value());
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    tracker.clear();

    // 2. Single pending sequenced request (sequence != 0) -> generic fallback refuses
    tracker.record_request(0x3333, 42, now);
    auto m_sequenced = tracker.match_response(0, 0, now + std::chrono::milliseconds(50));
    TEST_ASSERT(!m_sequenced.has_value());
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    tracker.clear();

    // 3. Single pending unsequenced request, but exceeds max generic elapsed (1500ms)
    tracker.record_request(0x4444, 0, now);
    auto m_timeout = tracker.match_response(0, 0, now + std::chrono::milliseconds(1600));
    TEST_ASSERT(!m_timeout.has_value());
}

TEST_CASE(SequenceTracker, UnsequencedResponsePrefersUnsequencedRequestWhenSequencedRequestPending) {
    mitigator::SequenceTracker tracker;
    const auto now = std::chrono::steady_clock::now();

    // 1. Client dispatches a sequenced request first (e.g. sequence = 10)
    tracker.record_request(0x8888, 10, now);
    // 2. Client dispatches an unsequenced request second for the same action_id (e.g. sequence = 0)
    tracker.record_request(0x8888, 0, now + std::chrono::milliseconds(5));
    TEST_ASSERT_EQ(tracker.pending_count(), 2);

    // 3. Server unsequenced response arrives (sequence = 0)
    auto m_unseq = tracker.match_response(0x8888, 0, now + std::chrono::milliseconds(50));
    TEST_ASSERT(m_unseq.has_value());
    // Crucial: Must match the unsequenced request (seq = 0), NOT steal the sequenced request (seq = 10)!
    TEST_ASSERT_EQ(m_unseq->sequence, 0);
    TEST_ASSERT_EQ(tracker.pending_count(), 1);

    // 4. Server sequenced response arrives (sequence = 10)
    auto m_seq = tracker.match_response(0x8888, 10, now + std::chrono::milliseconds(80));
    TEST_ASSERT(m_seq.has_value());
    TEST_ASSERT_EQ(m_seq->sequence, 10);
    TEST_ASSERT_EQ(tracker.pending_count(), 0);
}




