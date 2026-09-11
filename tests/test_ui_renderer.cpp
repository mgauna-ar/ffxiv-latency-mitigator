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
    TEST_ASSERT(renderer.active_tab() == 0);
    TEST_ASSERT(renderer.total_actions() == 0);
    TEST_ASSERT(renderer.actions_mitigated() == 0);

    const std::string out = renderer.render_snapshot_to_string(100, 30);
    TEST_ASSERT(out.find("0/0") != std::string::npos);
    TEST_ASSERT(out.find("STANDBY") != std::string::npos);
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
    t.applied = 0; // dry-run
    t.dry_run = 1;

    renderer.log_action(t, true);

    TEST_ASSERT(renderer.total_actions() == 1);
    TEST_ASSERT(renderer.actions_mitigated() == 0);
    TEST_ASSERT(renderer.cumulative_time_saved_ms() == 0.0);

    std::string out = renderer.render_snapshot_to_string(100, 30);
    TEST_ASSERT(out.find("0/1") != std::string::npos);
    TEST_ASSERT(out.find("0.00s") != std::string::npos);

    // Now apply an action with applied = 1
    t.applied = 1;
    t.dry_run = 0;
    renderer.log_action(t, true);

    TEST_ASSERT(renderer.total_actions() == 2);
    TEST_ASSERT(renderer.actions_mitigated() == 1);
    TEST_ASSERT_NEAR(renderer.cumulative_time_saved_ms(), 135.0, 0.1);

    out = renderer.render_snapshot_to_string(100, 30);
    TEST_ASSERT(out.find("1/2") != std::string::npos);
}

TEST_CASE(UiRenderer, QualityScoringTiers) {
    // 1. Excellent (RTT < 30ms)
    {
        mitigator::loader::UiRenderer renderer;
        mitigator::ipc::TelemetryPayload t{};
        t.smoothed_rtt_ms = 25.0f;
        t.jitter_ms = 1.0f;
        t.delay_reduced_ms = 10.0f;
        t.applied = 1;
        renderer.log_action(t, true);

        std::string out = renderer.render_snapshot_to_string(100, 30);
        TEST_ASSERT(out.find("EXCELLENT") != std::string::npos);
    }

    // 2. Good (RTT < 70ms)
    {
        mitigator::loader::UiRenderer renderer;
        mitigator::ipc::TelemetryPayload t{};
        t.smoothed_rtt_ms = 55.0f;
        t.jitter_ms = 3.0f;
        t.delay_reduced_ms = 10.0f;
        t.applied = 1;
        renderer.log_action(t, true);

        std::string out = renderer.render_snapshot_to_string(100, 30);
        TEST_ASSERT(out.find("GOOD") != std::string::npos);
    }

    // 3. Fair (RTT < 120ms)
    {
        mitigator::loader::UiRenderer renderer;
        mitigator::ipc::TelemetryPayload t{};
        t.smoothed_rtt_ms = 95.0f;
        t.jitter_ms = 6.0f;
        t.delay_reduced_ms = 10.0f;
        t.applied = 1;
        renderer.log_action(t, true);

        std::string out = renderer.render_snapshot_to_string(100, 30);
        TEST_ASSERT(out.find("FAIR") != std::string::npos);
    }

    // 4. Poor (RTT >= 120ms)
    {
        mitigator::loader::UiRenderer renderer;
        mitigator::ipc::TelemetryPayload t{};
        t.smoothed_rtt_ms = 160.0f;
        t.jitter_ms = 12.0f;
        t.delay_reduced_ms = 10.0f;
        t.applied = 1;
        renderer.log_action(t, true);

        std::string out = renderer.render_snapshot_to_string(100, 30);
        TEST_ASSERT(out.find("POOR") != std::string::npos);
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
    t.spike_filtered = 0;
    t.cold_start_guard = 0;
    t.applied = 1;

    renderer.log_action(t, true);

    std::string out = renderer.render_snapshot_to_string(100, 30);
    TEST_ASSERT(out.find("FLOOR") != std::string::npos);
}

TEST_CASE(UiRenderer, ResetStatsClearsState) {
    mitigator::loader::UiRenderer renderer;

    mitigator::ipc::TelemetryPayload t{};
    t.smoothed_rtt_ms = 50.0f;
    t.jitter_ms = 2.0f;
    t.delay_reduced_ms = 100.0f;
    t.measured_rtt_ms = 50.0f;
    t.applied = 1;
    renderer.log_action(t, true);

    TEST_ASSERT(renderer.total_actions() == 1);
    TEST_ASSERT(renderer.rtt_history().size() == 1);

    renderer.reset_stats();

    TEST_ASSERT(renderer.total_actions() == 0);
    TEST_ASSERT(renderer.actions_mitigated() == 0);
    TEST_ASSERT(renderer.rtt_history().empty());

    std::string out = renderer.render_snapshot_to_string(100, 30);
    TEST_ASSERT(out.find("0/0") != std::string::npos);
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

    for (uint32_t i = 1; i <= 30; ++i) {
        mitigator::ipc::TelemetryPayload t{};
        t.action_id = 0x1000 + i;
        t.original_lock_ms = 600.0f;
        t.adjusted_lock_ms = 465.0f;
        t.delay_reduced_ms = 135.0f;
        t.measured_rtt_ms = 50.0f;
        t.smoothed_rtt_ms = 50.0f;
        t.applied = 1;

        renderer.log_action(t, true);
    }

    // Capacity must be capped at RING_BUFFER_CAPACITY (24)
    TEST_ASSERT(renderer.ring_buffer_size() == mitigator::loader::UiRenderer::RING_BUFFER_CAPACITY);
    TEST_ASSERT(renderer.total_actions() == 30);
    TEST_ASSERT(renderer.actions_mitigated() == 30);
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

    renderer.log_action(t1, true);
    renderer.log_action(t2, true);

    auto guards = renderer.guard_counters();
    TEST_ASSERT(guards.floor_clamps == 1);
    TEST_ASSERT(guards.spike_filtered == 1);
    TEST_ASSERT(guards.cold_start_guards == 1);
    TEST_ASSERT(guards.cast_locks_preserved == 1);
}

TEST_CASE(UiRenderer, VisibleWidthHelper) {
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("Hello") == 5);
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("") == 0);
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("\033[32m[EXCELLENT]\033[0m") == 11);
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("┌─┐") == 3);
    TEST_ASSERT(mitigator::loader::UiRenderer::visible_width("█░") == 2);
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
    TEST_ASSERT(out.find("FFXIV STANDALONE LATENCY MITIGATOR") != std::string::npos);
    TEST_ASSERT(out.find("4321") != std::string::npos);
    TEST_ASSERT(out.find("LIVE COMBAT ACTION STREAM") != std::string::npos);
    TEST_ASSERT(out.find("0x1A4F") != std::string::npos);
    TEST_ASSERT(out.find("MITIGATED") != std::string::npos);
    TEST_ASSERT(out.find("╭") != std::string::npos);
    TEST_ASSERT(out.find("╰") != std::string::npos);
    TEST_ASSERT(out.find("\033[?25l") != std::string::npos);
}

TEST_CASE(UiRenderer, DashboardStandbyModeBeforeGameLaunches) {
    mitigator::loader::UiRenderer renderer;
    renderer.set_session_info(0, 0, 15.0, false);
    renderer.set_dashboard_mode(true);
    renderer.set_connection_status("Searching for ffxiv_dx11.exe...");

    CoutRedirect redirect;
    renderer.render_dashboard(false, false);
    const std::string out = redirect.str();

    TEST_ASSERT(out.find("\033[H") != std::string::npos);
    TEST_ASSERT(out.find("FFXIV STANDALONE LATENCY MITIGATOR") != std::string::npos);
    TEST_ASSERT(out.find("ffxiv_dx11.exe") != std::string::npos);
    TEST_ASSERT(out.find("Searching for ffxiv_dx11.exe...") != std::string::npos);
    TEST_ASSERT(out.find("STANDBY") != std::string::npos);
    TEST_ASSERT(renderer.connection_status() == "Searching for ffxiv_dx11.exe...");
}

TEST_CASE(UiRenderer, ResponsiveTerminalDimensions) {
    mitigator::loader::UiRenderer renderer;
    TEST_ASSERT(renderer.terminal_cols() == mitigator::loader::UiRenderer::DEFAULT_DASHBOARD_WIDTH);
    TEST_ASSERT(renderer.terminal_rows() == mitigator::loader::UiRenderer::DEFAULT_DASHBOARD_ROWS);

    renderer.set_terminal_dimensions(120, 35);
    TEST_ASSERT(renderer.terminal_cols() == 120);
    TEST_ASSERT(renderer.terminal_rows() == 35);

    CoutRedirect redirect;
    renderer.render_dashboard(false, false);
    const std::string out = redirect.str();
    TEST_ASSERT(out.find("╭") != std::string::npos);
    TEST_ASSERT(out.find("╰") != std::string::npos);

    // Test minimum clamping
    renderer.set_terminal_dimensions(60, 15);
    TEST_ASSERT(renderer.terminal_cols() == mitigator::loader::UiRenderer::MIN_DASHBOARD_WIDTH);
    TEST_ASSERT(renderer.terminal_rows() == mitigator::loader::UiRenderer::MIN_DASHBOARD_ROWS);
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

    renderer.log_action(t, true);

    CoutRedirect redirect;
    renderer.render_final_report();
    const std::string report = redirect.str();

    TEST_ASSERT(report.find("FINAL COMBAT SESSION REPORT") != std::string::npos);
    TEST_ASSERT(report.find("Session Duration:") != std::string::npos);
    TEST_ASSERT(report.find("Total Actions:") != std::string::npos);
    TEST_ASSERT(report.find("Actions Mitigated:") != std::string::npos);
    TEST_ASSERT(report.find("Total Time Saved:") != std::string::npos);
    TEST_ASSERT(report.find("RTT (Min/Med/P95):") != std::string::npos);
    TEST_ASSERT(report.find("Safety Floor Clamps:") != std::string::npos);
}

TEST_CASE(UiRenderer, TabSwitchingAndAnalyticsTab) {
    mitigator::loader::UiRenderer renderer;

    mitigator::ipc::TelemetryPayload t{};
    t.action_id = 0x1A4F;
    t.original_lock_ms = 600.0f;
    t.adjusted_lock_ms = 465.0f;
    t.delay_reduced_ms = 135.0f;
    t.measured_rtt_ms = 48.0f;
    t.smoothed_rtt_ms = 48.0f;
    t.jitter_ms = 1.8f;
    t.applied = 1;
    renderer.log_action(t, true);

    // Tab 0: Live Combat
    TEST_ASSERT(renderer.active_tab() == 0);
    std::string tab0 = renderer.render_snapshot_to_string(100, 30);
    TEST_ASSERT(tab0.find("LIVE COMBAT ACTION STREAM") != std::string::npos);

    // Switch to Tab 1: Latency Analytics
    renderer.set_active_tab(1);
    TEST_ASSERT(renderer.active_tab() == 1);
    std::string tab1 = renderer.render_snapshot_to_string(100, 30);
    TEST_ASSERT(tab1.find("REAL-TIME LATENCY WAVEFORM") != std::string::npos);
    TEST_ASSERT(tab1.find("LATENCY PERCENTILES") != std::string::npos);
    TEST_ASSERT(tab1.find("TIME SAVED SUMMARY") != std::string::npos);
    TEST_ASSERT(tab1.find("SAFETY & GUARDS") != std::string::npos);

    // Switch to Tab 2: Settings
    renderer.set_active_tab(2);
    TEST_ASSERT(renderer.active_tab() == 2);
    std::string tab2 = renderer.render_snapshot_to_string(100, 30);
    TEST_ASSERT(tab2.find("INTERACTIVE SETTINGS & CONFIGURATION") != std::string::npos);
    TEST_ASSERT(tab2.find("Min Animation Lock Floor") != std::string::npos);
}

TEST_CASE(UiRenderer, SparklineHistoryRingBuffer) {
    mitigator::loader::UiRenderer renderer;

    for (int i = 1; i <= 70; ++i) {
        mitigator::ipc::TelemetryPayload t{};
        t.action_id = 0x01;
        t.measured_rtt_ms = static_cast<float>(i);
        t.smoothed_rtt_ms = static_cast<float>(i);
        t.applied = 1;
        renderer.log_action(t, false);
    }

    // Sparkline history must be capped at SPARKLINE_HISTORY_CAPACITY (60)
    auto history = renderer.rtt_history();
    TEST_ASSERT(history.size() == mitigator::loader::UiRenderer::SPARKLINE_HISTORY_CAPACITY);
    TEST_ASSERT_NEAR(history.front(), 11.0f, 0.01f);
    TEST_ASSERT_NEAR(history.back(), 70.0f, 0.01f);

    // Test Unicode sparkline rendering helper
    std::string sparkline = mitigator::loader::UiRenderer::render_sparkline_bar(history, 60);
    TEST_ASSERT(!sparkline.empty());
}

TEST_CASE(UiRenderer, SettingsAndConfigurationCallbacks) {
    mitigator::loader::UiRenderer renderer;

    bool config_callback_called = false;
    renderer.set_on_config_changed([&](const mitigator::MitigationConfig&) {
        config_callback_called = true;
    });

    auto cfg = renderer.config();
    cfg.min_animation_lock_ms = 40.0;
    renderer.set_config(cfg);
    TEST_ASSERT_NEAR(renderer.config().min_animation_lock_ms, 40.0, 0.01);
    TEST_ASSERT(config_callback_called);
}

TEST_CASE(UiRenderer, CycleTabNavigation) {
    mitigator::loader::UiRenderer renderer;
    TEST_ASSERT(renderer.active_tab() == 0);

    // Forward cycle
    renderer.cycle_tab(1);
    TEST_ASSERT(renderer.active_tab() == 1);
    renderer.cycle_tab(1);
    TEST_ASSERT(renderer.active_tab() == 2);
    renderer.cycle_tab(1);
    TEST_ASSERT(renderer.active_tab() == 0);

    // Backward cycle
    renderer.cycle_tab(-1);
    TEST_ASSERT(renderer.active_tab() == 2);
    renderer.cycle_tab(-1);
    TEST_ASSERT(renderer.active_tab() == 1);
    renderer.cycle_tab(-1);
    TEST_ASSERT(renderer.active_tab() == 0);
}

TEST_CASE(UiRenderer, ExactRowCountBudgetAndNoTrailingNewline) {
    mitigator::loader::UiRenderer renderer;

    const std::vector<int> test_heights = {24, 25, 30, 45};

    for (int tab = 0; tab < 3; ++tab) {
        renderer.set_active_tab(tab);

        for (int h : test_heights) {
            std::string snapshot = renderer.render_snapshot_to_string(100, h);

            // Count lines: number of newlines must be exactly h - 1 (meaning h lines, no trailing \n)
            size_t newline_count = 0;
            for (char c : snapshot) {
                if (c == '\n') {
                    newline_count++;
                }
            }

            TEST_ASSERT(newline_count == static_cast<size_t>(h - 1));
            TEST_ASSERT(!snapshot.empty());
            TEST_ASSERT(snapshot.back() != '\n');
        }
    }
}

TEST_CASE(UiRenderer, SynchronizedUpdateModeFrameSwapping) {
    mitigator::loader::UiRenderer renderer;
    renderer.set_dashboard_mode(true);

    CoutRedirect redirect;
    renderer.render_dashboard(false, false);
    const std::string out = redirect.str();

    // Must bracket the render with mode 2026 for atomic GPU screen buffer presentation
    TEST_ASSERT(out.find("\033[?2026h") != std::string::npos);
    TEST_ASSERT(out.find("\033[?2026l") != std::string::npos);
}



