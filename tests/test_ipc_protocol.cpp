#include "test_framework.hpp"
#include "mitigator/ipc_protocol.hpp"

TEST_CASE(IpcProtocol, TelemetryRoundtrip) {
    mitigator::ipc::TelemetryPayload original{};
    original.action_id = 0x1A4F;
    original.sequence = 42;
    original.original_lock_ms = 600.0f;
    original.adjusted_lock_ms = 465.0f;
    original.delay_reduced_ms = 135.0f;
    original.measured_rtt_ms = 150.0f;
    original.smoothed_rtt_ms = 148.5f;
    original.jitter_ms = 2.1f;
    original.clamped_floor = 0;
    original.dry_run = 0;
    original.applied = 1;
    original.cast_active = 0;
    original.spike_filtered = 1;
    original.cold_start_guard = 0;
    original.timestamp_ms = 123456789;

    static_assert(sizeof(mitigator::ipc::TelemetryPayload) == 48, "TelemetryPayload size must be 48 bytes");

    const auto buffer = mitigator::ipc::serialize_telemetry(original);
    TEST_ASSERT(buffer.size() > sizeof(mitigator::ipc::Header));

    const auto hdr = mitigator::ipc::parse_header(buffer);
    TEST_ASSERT(hdr.has_value());
    TEST_ASSERT_EQ(hdr->magic, mitigator::ipc::IPC_MAGIC);
    TEST_ASSERT_EQ(hdr->version, mitigator::ipc::IPC_VERSION);
    TEST_ASSERT_EQ(hdr->type, static_cast<uint16_t>(mitigator::ipc::PacketType::Telemetry));

    std::span<const uint8_t> payload_span(buffer.data() + sizeof(mitigator::ipc::Header), hdr->payload_size);
    const auto decoded = mitigator::ipc::deserialize_telemetry(payload_span);
    TEST_ASSERT(decoded.has_value());
    TEST_ASSERT_EQ(decoded->action_id, 0x1A4F);
    TEST_ASSERT_EQ(decoded->sequence, 42);
    TEST_ASSERT_NEAR(decoded->delay_reduced_ms, 135.0f, 0.001f);
    TEST_ASSERT_EQ(decoded->applied, 1);
    TEST_ASSERT_EQ(decoded->spike_filtered, 1);
    TEST_ASSERT_EQ(decoded->cold_start_guard, 0);
}

TEST_CASE(IpcProtocol, CommandRoundtrip) {
    mitigator::ipc::CommandPayload original{};
    original.command_type = static_cast<uint32_t>(mitigator::ipc::CommandType::SetTargetPing);
    original.param_float = 18.5f;
    original.param_uint = 1;

    const auto buffer = mitigator::ipc::serialize_command(original);
    const auto hdr = mitigator::ipc::parse_header(buffer);
    TEST_ASSERT(hdr.has_value());
    TEST_ASSERT_EQ(hdr->type, static_cast<uint16_t>(mitigator::ipc::PacketType::Command));

    std::span<const uint8_t> payload_span(buffer.data() + sizeof(mitigator::ipc::Header), hdr->payload_size);
    const auto decoded = mitigator::ipc::deserialize_command(payload_span);
    TEST_ASSERT(decoded.has_value());
    TEST_ASSERT_EQ(decoded->command_type, static_cast<uint32_t>(mitigator::ipc::CommandType::SetTargetPing));
    TEST_ASSERT_NEAR(decoded->param_float, 18.5f, 0.001f);
    TEST_ASSERT_EQ(decoded->param_uint, 1);
}

TEST_CASE(IpcProtocol, StatusRoundtrip) {
    mitigator::ipc::StatusPayload original{};
    original.game_pid = 9876;
    original.hooks_installed = 4;
    original.version_major = 1;
    original.version_minor = 0;
    const char* msg = "All detours active";
    std::memcpy(original.status_message, msg, std::strlen(msg) + 1);

    const auto buffer = mitigator::ipc::serialize_status(original);
    const auto hdr = mitigator::ipc::parse_header(buffer);
    TEST_ASSERT(hdr.has_value());
    TEST_ASSERT_EQ(hdr->type, static_cast<uint16_t>(mitigator::ipc::PacketType::Status));

    std::span<const uint8_t> payload_span(buffer.data() + sizeof(mitigator::ipc::Header), hdr->payload_size);
    const auto decoded = mitigator::ipc::deserialize_status(payload_span);
    TEST_ASSERT(decoded.has_value());
    TEST_ASSERT_EQ(decoded->game_pid, 9876);
    TEST_ASSERT_EQ(decoded->hooks_installed, 4);
    TEST_ASSERT_EQ(std::string(decoded->status_message), std::string(msg));
}

TEST_CASE(IpcProtocol, HeaderValidationRejections) {
    // 1. Truncated header (< sizeof(Header))
    std::vector<uint8_t> truncated(sizeof(mitigator::ipc::Header) - 1, 0);
    TEST_ASSERT(!mitigator::ipc::parse_header(truncated).has_value());

    // 2. Corrupt magic number
    mitigator::ipc::Header bad_magic_hdr{};
    bad_magic_hdr.magic = 0xDEADBEEF;
    bad_magic_hdr.version = mitigator::ipc::IPC_VERSION;
    std::vector<uint8_t> bad_buf(sizeof(bad_magic_hdr));
    std::memcpy(bad_buf.data(), &bad_magic_hdr, sizeof(bad_magic_hdr));
    TEST_ASSERT(!mitigator::ipc::parse_header(bad_buf).has_value());

    // 3. Corrupt version number
    mitigator::ipc::Header bad_ver_hdr{};
    bad_ver_hdr.magic = mitigator::ipc::IPC_MAGIC;
    bad_ver_hdr.version = 999;
    std::memcpy(bad_buf.data(), &bad_ver_hdr, sizeof(bad_ver_hdr));
    TEST_ASSERT(!mitigator::ipc::parse_header(bad_buf).has_value());
}

TEST_CASE(IpcProtocol, DeserializationRobustnessAndFuzzing) {
    // 1. Empty spans must safely reject without out-of-bounds access
    std::vector<uint8_t> empty_vec{};
    std::span<const uint8_t> empty_span(empty_vec);
    TEST_ASSERT(!mitigator::ipc::deserialize_telemetry(empty_span).has_value());
    TEST_ASSERT(!mitigator::ipc::deserialize_command(empty_span).has_value());
    TEST_ASSERT(!mitigator::ipc::deserialize_status(empty_span).has_value());

    // 2. Truncated payloads (1 byte less than required struct size)
    std::vector<uint8_t> trunc_telemetry(sizeof(mitigator::ipc::TelemetryPayload) - 1, 0xAA);
    TEST_ASSERT(!mitigator::ipc::deserialize_telemetry(trunc_telemetry).has_value());

    std::vector<uint8_t> trunc_cmd(sizeof(mitigator::ipc::CommandPayload) - 1, 0xBB);
    TEST_ASSERT(!mitigator::ipc::deserialize_command(trunc_cmd).has_value());

    std::vector<uint8_t> trunc_status(sizeof(mitigator::ipc::StatusPayload) - 1, 0xCC);
    TEST_ASSERT(!mitigator::ipc::deserialize_status(trunc_status).has_value());

    // 3. Trailing byte tolerance (span >= sizeof(T) safely extracts without overrun)
    std::vector<uint8_t> extra_telemetry(sizeof(mitigator::ipc::TelemetryPayload) + 32, 0);
    mitigator::ipc::TelemetryPayload original_t{};
    original_t.action_id = 0x9999;
    original_t.sequence = 77;
    std::memcpy(extra_telemetry.data(), &original_t, sizeof(original_t));

    auto decoded_t = mitigator::ipc::deserialize_telemetry(extra_telemetry);
    TEST_ASSERT(decoded_t.has_value());
    TEST_ASSERT_EQ(decoded_t->action_id, 0x9999);
    TEST_ASSERT_EQ(decoded_t->sequence, 77);

    // 4. Garbage bit patterns with all 0xFF across entire buffer
    std::vector<uint8_t> ff_telemetry(sizeof(mitigator::ipc::TelemetryPayload), 0xFF);
    auto decoded_ff = mitigator::ipc::deserialize_telemetry(ff_telemetry);
    TEST_ASSERT(decoded_ff.has_value());
    // Safe memory extraction without crash or UB
    TEST_ASSERT_EQ(decoded_ff->action_id, 0xFFFFFFFF);

    // 5. Header fuzzing across odd buffer sizes (1 to 64 bytes of pseudo-random data)
    for (size_t len = 1; len <= 64; ++len) {
        std::vector<uint8_t> fuzz_buf(len);
        for (size_t i = 0; i < len; ++i) {
            fuzz_buf[i] = static_cast<uint8_t>((i * 37 + len) & 0xFF);
        }
        // Must never crash, throw, or access out-of-bounds
        auto hdr_res = mitigator::ipc::parse_header(fuzz_buf);
        if (len < sizeof(mitigator::ipc::Header)) {
            TEST_ASSERT(!hdr_res.has_value());
        }
    }
}

