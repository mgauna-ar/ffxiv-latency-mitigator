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

TEST_CASE(UiRenderer, PercentileCalculations) {
    // Empty vector
    std::vector<float> empty_samples;
    auto empty_dist = mitigator::loader::UiRenderer::compute_distribution(empty_samples);
    TEST_ASSERT(empty_dist.min_val == 0.0f);
    TEST_ASSERT(empty_dist.median_val == 0.0f);
    TEST_ASSERT(empty_dist.p95_val == 0.0f);
    TEST_ASSERT(empty_dist.max_val == 0.0f);

    // Known distribution
    std::vector<float> samples = {100.0f, 20.0f, 50.0f, 10.0f, 80.0f, 90.0f, 30.0f, 70.0f, 40.0f, 60.0f};
    auto dist = mitigator::loader::UiRenderer::compute_distribution(samples);
    TEST_ASSERT_NEAR(dist.min_val, 10.0f, 0.01f);
    TEST_ASSERT_NEAR(dist.median_val, 60.0f, 0.01f);
    TEST_ASSERT_NEAR(dist.p95_val, 90.0f, 0.01f);
    TEST_ASSERT_NEAR(dist.max_val, 100.0f, 0.01f);
}

TEST_CASE(UiRenderer, ActionRingBufferEviction) {
    mitigator::loader::UiRenderer renderer;

    for (uint32_t i = 1; i <= 20; ++i) {
        mitigator::ipc::TelemetryPayload t{};
        t.action_id = 0x1000 + i;
        t.original_lock_ms = 600.0f;
        t.adjusted_lock_ms = 465.0f;
        t.delay_reduced_ms = 135.0f;
        t.measured_rtt_ms = 50.0f;
        t.smoothed_rtt_ms = 50.0f;
        t.applied = 1;

        CoutRedirect sink;
        renderer.log_action(t, true);
    }

    // Capacity must be capped at RING_BUFFER_CAPACITY (12)
    TEST_ASSERT(renderer.ring_buffer_size() == mitigator::loader::UiRenderer::RING_BUFFER_CAPACITY);
    TEST_ASSERT(renderer.total_actions() == 20);
    TEST_ASSERT(renderer.actions_mitigated() == 20);
}

TEST_CASE(UiRenderer, SafetyGuardCounters) {
    mitigator::loader::UiRenderer renderer;

    mitigator::ipc::TelemetryPayload t1{};
    t1.clamped_floor = 1;
    t1.applied = 1;

    mitigator::ipc::TelemetryPayload t2{};
    t2.spike_filtered = 1;
    t2.cold_start_guard = 1;
    t2.cast_active = 1;
    t2.applied = 0;

    {
        CoutRedirect sink;
        renderer.log_action(t1, true);
        renderer.log_action(t2, true);
    }

    auto guards = renderer.guard_counters();
    TEST_ASSERT(guards.floor_clamps == 1);
    TEST_ASSERT(guards.spike_filtered == 1);
    TEST_ASSERT(guards.cold_start_guards == 1);
    TEST_ASSERT(guards.cast_locks_preserved == 1);
}

TEST_CASE(UiRenderer, VisibleWidthHelper) {
    // Pure ASCII
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("Hello") == 5);
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("") == 0);

    // ANSI codes have 0 visible width
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("\033[32m[EXCELLENT]\033[0m") == 11);
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("\033[1m\033[31mERROR\033[0m") == 5);

    // UTF-8 box characters have 1 visible column each
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("┌─┐") == 3);
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("█░") == 2);
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("±") == 1);
}

TEST_CASE(UiRenderer, MakeBarHelper) {
    const std::string bar = mitigator::loader::UiRenderer::make_bar(50.0f, 100.0f, 10, "");
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width(bar) == 10);

    const std::string full_bar = mitigator::loader::UiRenderer::make_bar(100.0f, 100.0f, 8, "");
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width(full_bar) == 8);

    const std::string zero_bar = mitigator::loader::UiRenderer::make_bar(0.0f, 100.0f, 8, "");
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width(zero_bar) == 8);
}

TEST_CASE(UiRenderer, FormatTimeHhmmss) {
    TEST_ASSERT(mitigator::loader::UiRenderer::format_time_hhmmss(std::chrono::seconds(0)) == "00:00:00");
    TEST_ASSERT(mitigator::loader::UiRenderer::format_time_hhmmss(std::chrono::seconds(59)) == "00:00:59");
    TEST_ASSERT(mitigator::loader::UiRenderer::format_time_hhmmss(std::chrono::seconds(3661)) == "01:01:01");
    TEST_ASSERT(mitigator::loader::UiRenderer::format_time_hhmmss(std::chrono::seconds(86400)) == "24:00:00");
}

TEST_CASE(UiRenderer, DashboardRenderLayoutAndBorders) {
    mitigator::loader::UiRenderer renderer;
    renderer.set_session_info(4321, 2, 15.0, false);
    renderer.set_dashboard_mode(true);

    mitigator::ipc::TelemetryPayload t{};
    t.action_id = 0x1A4F;
    t.original_lock_ms = 600.0f;
    t.adjusted_lock_ms = 465.0f;
    t.delay_reduced_ms = 135.0f;
    t.measured_rtt_ms = 52.0f;
    t.smoothed_rtt_ms = 50.0f;
    t.jitter_ms = 2.0f;
    t.applied = 1;

    renderer.log_action(t, true);

    CoutRedirect redirect;
    renderer.render_dashboard(false, false);
    const std::string out = redirect.str();

    TEST_ASSERT(out.find("\033[H") != std::string::npos);
    TEST_ASSERT(out.find("LIVE COMBAT DASHBOARD") != std::string::npos);
    TEST_ASSERT(out.find("4321") != std::string::npos);
    TEST_ASSERT(out.find("NETWORK & LATENCY") != std::string::npos);
    TEST_ASSERT(out.find("MITIGATION & THROUGHPUT") != std::string::npos);
    TEST_ASSERT(out.find("SAFETY GUARDS & DIAGNOSTICS") != std::string::npos);
    TEST_ASSERT(out.find("RECENT ACTION LOG") != std::string::npos);
    TEST_ASSERT(out.find("0x1A4F") != std::string::npos);
    TEST_ASSERT(out.find("[Mitigated]") != std::string::npos);
    TEST_ASSERT(out.find("Controls:") != std::string::npos);
}

TEST_CASE(UiRenderer, FinalSessionReportCard) {
    mitigator::loader::UiRenderer renderer;
    renderer.set_session_info(1234, 2, 15.0, false);

    mitigator::ipc::TelemetryPayload t{};
    t.action_id = 0x001B;
    t.original_lock_ms = 500.0f;
    t.adjusted_lock_ms = 400.0f;
    t.delay_reduced_ms = 100.0f;
    t.measured_rtt_ms = 45.0f;
    t.smoothed_rtt_ms = 45.0f;
    t.jitter_ms = 1.5f;
    t.applied = 1;
    t.clamped_floor = 1;

    {
        CoutRedirect sink;
        renderer.log_action(t, true);
    }

    CoutRedirect redirect;
    renderer.render_final_report();
    const std::string report = redirect.str();

    TEST_ASSERT(report.find("FINAL SESSION TELEMETRY REPORT") != std::string::npos);
    TEST_ASSERT(report.find("Session Duration:") != std::string::npos);
    TEST_ASSERT(report.find("Total Actions:") != std::string::npos);
    TEST_ASSERT(report.find("Mitigated Actions:") != std::string::npos);
    TEST_ASSERT(report.find("Total Time Saved:") != std::string::npos);
    TEST_ASSERT(report.find("LATENCY & SAVINGS DISTRIBUTION") != std::string::npos);
    TEST_ASSERT(report.find("RTT Distribution:") != std::string::npos);
    TEST_ASSERT(report.find("SAFETY GUARD DIAGNOSTICS") != std::string::npos);
    TEST_ASSERT(report.find("Floor Clamps:") != std::string::npos);
}

TEST_CASE(UiRenderer, ResetStatsClearsExtendedMetrics) {
    mitigator::loader::UiRenderer renderer;

    mitigator::ipc::TelemetryPayload t{};
    t.action_id = 0x1111;
    t.original_lock_ms = 600.0f;
    t.adjusted_lock_ms = 450.0f;
    t.delay_reduced_ms = 150.0f;
    t.measured_rtt_ms = 60.0f;
    t.smoothed_rtt_ms = 60.0f;
    t.jitter_ms = 3.0f;
    t.applied = 1;
    t.clamped_floor = 1;
    t.spike_filtered = 1;
    t.cold_start_guard = 1;
    t.cast_active = 1;

    {
        CoutRedirect sink;
        renderer.log_action(t, true);
    }

    TEST_ASSERT(renderer.total_actions() == 1);
    TEST_ASSERT(renderer.actions_mitigated() == 1);
    TEST_ASSERT(renderer.guard_counters().floor_clamps == 1);
    TEST_ASSERT(renderer.ring_buffer_size() == 1);

    renderer.reset_stats();

    TEST_ASSERT(renderer.total_actions() == 0);
    TEST_ASSERT(renderer.actions_mitigated() == 0);
    TEST_ASSERT(renderer.cumulative_time_saved_ms() == 0.0);
    TEST_ASSERT(renderer.guard_counters().floor_clamps == 0);
    TEST_ASSERT(renderer.guard_counters().spike_filtered == 0);
    TEST_ASSERT(renderer.guard_counters().cold_start_guards == 0);
    TEST_ASSERT(renderer.guard_counters().cast_locks_preserved == 0);
    TEST_ASSERT(renderer.ring_buffer_size() == 0);
    TEST_ASSERT(renderer.rtt_distribution().min_val == 0.0f);
    TEST_ASSERT(renderer.delay_saved_distribution().min_val == 0.0f);
}
