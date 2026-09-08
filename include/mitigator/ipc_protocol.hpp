#pragma once

#include <cstdint>
#include <vector>
#include <span>
#include <optional>

namespace mitigator::ipc {

constexpr uint32_t IPC_MAGIC = 0x4646584D; // "FFXM"
constexpr uint16_t IPC_VERSION = 1;
constexpr const char* DEFAULT_PIPE_NAME = "\\\\.\\pipe\\ffxiv_mitigator_ipc";

enum class PacketType : uint16_t {
    Heartbeat = 1,
    Status = 2,
    Telemetry = 3,
    Command = 4
};

enum class CommandType : uint32_t {
    UnhookAndExit = 1,
    SetDryRun = 2,
    SetVerbose = 3,
    SetTargetPing = 4,
    ResetStats = 5,
    SetMinAnimationLock = 6
};

#pragma pack(push, 1)

struct Header {
    uint32_t magic{IPC_MAGIC};
    uint16_t version{IPC_VERSION};
    uint16_t type{0};
    uint32_t payload_size{0};
};

struct HeartbeatPayload {
    uint64_t timestamp_ms{0};
    uint32_t sequence{0};
};

struct StatusPayload {
    uint32_t game_pid{0};
    uint32_t hooks_installed{0};
    uint16_t version_major{1};
    uint16_t version_minor{0};
    char status_message[64]{0};
};

struct TelemetryPayload {
    uint32_t action_id{0};
    uint32_t sequence{0};
    float original_lock_ms{0.0f};
    float adjusted_lock_ms{0.0f};
    float delay_reduced_ms{0.0f};
    float measured_rtt_ms{0.0f};
    float smoothed_rtt_ms{0.0f};
    float jitter_ms{0.0f};
    uint8_t clamped_floor{0};
    uint8_t dry_run{0};
    uint8_t applied{0};
    uint8_t cast_active{0};
    uint64_t timestamp_ms{0};
};

struct CommandPayload {
    uint32_t command_type{0};
    float param_float{0.0f};
    uint32_t param_uint{0};
};

#pragma pack(pop)

/// Serializes any packet into a byte vector including Header.
[[nodiscard]] std::vector<uint8_t> serialize_heartbeat(const HeartbeatPayload& payload);
[[nodiscard]] std::vector<uint8_t> serialize_status(const StatusPayload& payload);
[[nodiscard]] std::vector<uint8_t> serialize_telemetry(const TelemetryPayload& payload);
[[nodiscard]] std::vector<uint8_t> serialize_command(const CommandPayload& payload);

/// Validates header and returns payload size if valid.
[[nodiscard]] std::optional<Header> parse_header(std::span<const uint8_t> data);

/// Deserializes payloads from byte spans with strict bounds checking.
[[nodiscard]] std::optional<HeartbeatPayload> deserialize_heartbeat(std::span<const uint8_t> data);
[[nodiscard]] std::optional<StatusPayload> deserialize_status(std::span<const uint8_t> data);
[[nodiscard]] std::optional<TelemetryPayload> deserialize_telemetry(std::span<const uint8_t> data);
[[nodiscard]] std::optional<CommandPayload> deserialize_command(std::span<const uint8_t> data);

} // namespace mitigator::ipc
