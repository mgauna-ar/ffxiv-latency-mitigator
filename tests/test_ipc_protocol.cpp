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
    original.timestamp_ms = 123456789;

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
