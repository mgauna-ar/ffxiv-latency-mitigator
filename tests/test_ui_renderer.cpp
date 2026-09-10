#include "test_framework.hpp"
#include "loader/ui_renderer.hpp"
#include "mitigator/types.hpp"
#include <sstream>
#include <iostream>

namespace {
    struct CoutRedirect {
        std::stringstream buffer;
        std::streambuf* old_buf{nullptr};

        CoutRedirect() : old_buf(std::cout.rdbuf(buffer.rdbuf())) {}
        ~CoutRedirect() { std::cout.rdbuf(old_buf); }

        std::string str() const { return buffer.str(); }
    };
}

TEST_CASE(UiRenderer, InitialSummaryShowsInitializing) {
    mitigator::loader::UiRenderer renderer;
    CoutRedirect redirect;

    renderer.render_stats_summary();
    const std::string out = redirect.str();

    TEST_ASSERT(out.find("Mitigated:") != std::string::npos);
    TEST_ASSERT(out.find("0/0") != std::string::npos);
    TEST_ASSERT(out.find("[INITIALIZING]") != std::string::npos);
}

TEST_CASE(UiRenderer, AppliedGatePreventsDryRunCounterInflation) {
    mitigator::loader::UiRenderer renderer;

    mitigator::ipc::TelemetryPayload t{};
    t.action_id = 0x1A4F;
    t.original_lock_ms = 600.0f;
    t.adjusted_lock_ms = 465.0f;
    t.delay_reduced_ms = 135.0f;
    t.smoothed_rtt_ms = 50.0f;
    t.jitter_ms = 2.0f;
    t.applied = 0; // e.g. dry-run
    t.dry_run = 1;

    {
        CoutRedirect redirect;
        renderer.log_action(t, true);
        const std::string log_out = redirect.str();
        TEST_ASSERT(log_out.find("[Dry Run]") != std::string::npos);
    }

    {
        CoutRedirect redirect;
        renderer.render_stats_summary();
        const std::string sum_out = redirect.str();
        // Total actions is 1, but mitigated MUST be 0 because applied was false
        TEST_ASSERT(sum_out.find("Mitigated:") != std::string::npos);
        TEST_ASSERT(sum_out.find("0/1") != std::string::npos);
        TEST_ASSERT(sum_out.find("Total Saved:") != std::string::npos);
        TEST_ASSERT(sum_out.find("0.00s") != std::string::npos);
    }

    // Now apply an action with applied = 1
    t.applied = 1;
    t.dry_run = 0;
    {
        CoutRedirect redirect;
        renderer.log_action(t, true);
    }

    {
        CoutRedirect redirect;
        renderer.render_stats_summary();
        const std::string sum_out = redirect.str();
        // Now mitigated is 1 / 2
        TEST_ASSERT(sum_out.find("Mitigated:") != std::string::npos);
        TEST_ASSERT(sum_out.find("1/2") != std::string::npos);
        TEST_ASSERT(sum_out.find("Total Saved:") != std::string::npos);
        TEST_ASSERT(sum_out.find("0.14s") != std::string::npos);
    }
}

TEST_CASE(UiRenderer, QualityScoringTiers) {
    // 1. Excellent (RTT <= 80ms, jitter <= 5ms)
    {
        mitigator::loader::UiRenderer renderer;
        mitigator::ipc::TelemetryPayload t{};
        t.smoothed_rtt_ms = 45.0f;
        t.jitter_ms = 3.0f;
        t.delay_reduced_ms = 10.0f;
        t.applied = 1;
        {
            CoutRedirect sink;
            renderer.log_action(t, true);
        }

        CoutRedirect redirect;
        renderer.render_stats_summary();
        TEST_ASSERT(redirect.str().find("[EXCELLENT]") != std::string::npos);
    }

    // 2. Good (RTT <= 150ms, jitter <= 15ms)
    {
        mitigator::loader::UiRenderer renderer;
        mitigator::ipc::TelemetryPayload t{};
        t.smoothed_rtt_ms = 120.0f;
        t.jitter_ms = 8.0f;
        t.delay_reduced_ms = 10.0f;
        t.applied = 1;
        {
            CoutRedirect sink;
            renderer.log_action(t, true);
        }

        CoutRedirect redirect;
        renderer.render_stats_summary();
        TEST_ASSERT(redirect.str().find("[GOOD]") != std::string::npos);
    }

    // 3. Fair (RTT <= 220ms, jitter <= 30ms)
    {
        mitigator::loader::UiRenderer renderer;
        mitigator::ipc::TelemetryPayload t{};
        t.smoothed_rtt_ms = 190.0f;
        t.jitter_ms = 22.0f;
        t.delay_reduced_ms = 10.0f;
        t.applied = 1;
        {
            CoutRedirect sink;
            renderer.log_action(t, true);
        }

        CoutRedirect redirect;
        renderer.render_stats_summary();
        TEST_ASSERT(redirect.str().find("[FAIR]") != std::string::npos);
    }

    // 4. Poor (RTT > 220ms or jitter > 30ms)
    {
        mitigator::loader::UiRenderer renderer;
        mitigator::ipc::TelemetryPayload t{};
        t.smoothed_rtt_ms = 260.0f;
        t.jitter_ms = 10.0f;
        t.delay_reduced_ms = 10.0f;
        t.applied = 1;
        {
            CoutRedirect sink;
            renderer.log_action(t, true);
        }

        CoutRedirect redirect;
        renderer.render_stats_summary();
        TEST_ASSERT(redirect.str().find("[POOR]") != std::string::npos);
    }
}

TEST_CASE(UiRenderer, DecisionTagsRendered) {
    mitigator::loader::UiRenderer renderer;

    mitigator::ipc::TelemetryPayload t{};
    t.action_id = 0x2001;
    t.original_lock_ms = 600.0f;
    t.adjusted_lock_ms = 400.0f;
    t.delay_reduced_ms = 200.0f;
    t.smoothed_rtt_ms = 70.0f;
    t.jitter_ms = 4.0f;
    t.clamped_floor = 1;
    t.spike_filtered = 1;
    t.cold_start_guard = 1;
    t.applied = 1;

    CoutRedirect redirect;
    renderer.log_action(t, true);
    const std::string out = redirect.str();

    TEST_ASSERT(out.find("[Floor Clamp]") != std::string::npos);
    TEST_ASSERT(out.find("[Spike Filtered]") != std::string::npos);
    TEST_ASSERT(out.find("[Cold Start]") != std::string::npos);
}

TEST_CASE(UiRenderer, ResetStatsClearsState) {
    mitigator::loader::UiRenderer renderer;

    mitigator::ipc::TelemetryPayload t{};
    t.smoothed_rtt_ms = 50.0f;
    t.jitter_ms = 2.0f;
    t.delay_reduced_ms = 100.0f;
    t.applied = 1;
    {
        CoutRedirect sink;
        renderer.log_action(t, true);
    }

    renderer.reset_stats();

    CoutRedirect redirect;
    renderer.render_stats_summary();
    const std::string out = redirect.str();

    TEST_ASSERT(out.find("Mitigated:") != std::string::npos);
    TEST_ASSERT(out.find("0/0") != std::string::npos);
    TEST_ASSERT(out.find("[INITIALIZING]") != std::string::npos);
}
