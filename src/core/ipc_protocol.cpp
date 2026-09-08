#include "mitigator/ipc_protocol.hpp"
#include <cstring>

namespace mitigator::ipc {

namespace {

template <typename T>
std::vector<uint8_t> serialize_packet(PacketType type, const T& payload) {
    const size_t total_size = sizeof(Header) + sizeof(T);
    std::vector<uint8_t> buffer(total_size);

    Header hdr;
    hdr.magic = IPC_MAGIC;
    hdr.version = IPC_VERSION;
    hdr.type = static_cast<uint16_t>(type);
    hdr.payload_size = static_cast<uint32_t>(sizeof(T));

    std::memcpy(buffer.data(), &hdr, sizeof(Header));
    std::memcpy(buffer.data() + sizeof(Header), &payload, sizeof(T));

    return buffer;
}

template <typename T>
std::optional<T> deserialize_packet(std::span<const uint8_t> data) {
    if (data.size() < sizeof(T)) {
        return std::nullopt;
    }
    T payload{};
    std::memcpy(&payload, data.data(), sizeof(T));
    return payload;
}

} // anonymous namespace

std::vector<uint8_t> serialize_heartbeat(const HeartbeatPayload& payload) {
    return serialize_packet(PacketType::Heartbeat, payload);
}

std::vector<uint8_t> serialize_status(const StatusPayload& payload) {
    return serialize_packet(PacketType::Status, payload);
}

std::vector<uint8_t> serialize_telemetry(const TelemetryPayload& payload) {
    return serialize_packet(PacketType::Telemetry, payload);
}

std::vector<uint8_t> serialize_command(const CommandPayload& payload) {
    return serialize_packet(PacketType::Command, payload);
}

std::optional<Header> parse_header(std::span<const uint8_t> data) {
    if (data.size() < sizeof(Header)) {
        return std::nullopt;
    }

    Header hdr{};
    std::memcpy(&hdr, data.data(), sizeof(Header));

    if (hdr.magic != IPC_MAGIC || hdr.version != IPC_VERSION) {
        return std::nullopt;
    }

    return hdr;
}

std::optional<HeartbeatPayload> deserialize_heartbeat(std::span<const uint8_t> data) {
    return deserialize_packet<HeartbeatPayload>(data);
}

std::optional<StatusPayload> deserialize_status(std::span<const uint8_t> data) {
    return deserialize_packet<StatusPayload>(data);
}

std::optional<TelemetryPayload> deserialize_telemetry(std::span<const uint8_t> data) {
    return deserialize_packet<TelemetryPayload>(data);
}

std::optional<CommandPayload> deserialize_command(std::span<const uint8_t> data) {
    return deserialize_packet<CommandPayload>(data);
}

} // namespace mitigator::ipc
