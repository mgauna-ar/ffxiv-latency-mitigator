#include "payload/payload_ipc.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mitigator::payload {

PayloadIpcClient::PayloadIpcClient(const char* pipe_name)
    : m_pipe_name(pipe_name) {}

PayloadIpcClient::~PayloadIpcClient() {
    disconnect();
}

bool PayloadIpcClient::connect(uint32_t timeout_ms) {
#if defined(_WIN32)
    if (m_connected.load()) {
        return true;
    }

    if (!WaitNamedPipeA(m_pipe_name, timeout_ms)) {
        return false;
    }

    HANDLE hPipe = CreateFileA(
        m_pipe_name,
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );

    if (hPipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    // Set message-read mode
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);

    m_pipe_handle = hPipe;
    m_connected = true;
    m_running = true;

    m_reader_thread = std::thread(&PayloadIpcClient::reader_thread_func, this);
    return true;
#else
    (void)timeout_ms;
    return false;
#endif
}

void PayloadIpcClient::disconnect() {
    m_running = false;
    m_connected = false;

#if defined(_WIN32)
    if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(static_cast<HANDLE>(m_pipe_handle));
        m_pipe_handle = nullptr;
    }
#endif

    if (m_reader_thread.joinable()) {
        m_reader_thread.join();
    }
}

bool PayloadIpcClient::send_telemetry(const ipc::TelemetryPayload& payload) {
    if (!m_connected.load()) return false;

    const auto buffer = ipc::serialize_telemetry(payload);

#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(m_send_mutex);
    if (!m_pipe_handle || m_pipe_handle == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(
        static_cast<HANDLE>(m_pipe_handle),
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &written,
        nullptr
    );
    return (ok && written == buffer.size());
#else
    (void)buffer;
    return false;
#endif
}

bool PayloadIpcClient::send_status(const ipc::StatusPayload& payload) {
    if (!m_connected.load()) return false;

    const auto buffer = ipc::serialize_status(payload);

#if defined(_WIN32)
    std::lock_guard<std::mutex> lock(m_send_mutex);
    if (!m_pipe_handle || m_pipe_handle == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = WriteFile(
        static_cast<HANDLE>(m_pipe_handle),
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &written,
        nullptr
    );
    return (ok && written == buffer.size());
#else
    (void)buffer;
    return false;
#endif
}

void PayloadIpcClient::set_command_handler(CommandHandler handler) {
    m_command_handler = std::move(handler);
}

void PayloadIpcClient::reader_thread_func() {
#if defined(_WIN32)
    std::vector<uint8_t> buffer(1024);

    while (m_running.load()) {
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(
            static_cast<HANDLE>(m_pipe_handle),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_read,
            nullptr
        );

        if (!ok || bytes_read == 0) {
            // Pipe broken or disconnected
            m_connected = false;
            break;
        }

        std::span<const uint8_t> data_span(buffer.data(), bytes_read);
        const auto hdr = ipc::parse_header(data_span);
        if (!hdr.has_value()) {
            continue;
        }

        if (hdr->type == static_cast<uint16_t>(ipc::PacketType::Command)) {
            const auto payload_span = data_span.subspan(sizeof(ipc::Header));
            const auto cmd = ipc::deserialize_command(payload_span);
            if (cmd.has_value() && m_command_handler) {
                m_command_handler(*cmd);
            }
        }
    }
#endif
}

} // namespace mitigator::payload
