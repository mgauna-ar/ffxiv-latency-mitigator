#include "payload/payload_ipc.hpp"
#include <chrono>
#include <algorithm>

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

    const auto start_time = std::chrono::steady_clock::now();
    HANDLE hPipe = INVALID_HANDLE_VALUE;

    while (true) {
        hPipe = CreateFileA(
            m_pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hPipe != INVALID_HANDLE_VALUE) {
            break;
        }

        const DWORD err = GetLastError();
        const auto elapsed_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time
            ).count()
        );

        if (elapsed_ms >= timeout_ms) {
            return false;
        }

        const uint32_t remaining_ms = timeout_ms - elapsed_ms;

        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeA(m_pipe_name, (std::min)(remaining_ms, 200u));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    // Set message-read mode
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr)) {
        CloseHandle(hPipe);
        return false;
    }

    m_pipe_handle = hPipe;
    m_connected = true;
    m_running = true;

    m_reader_thread = std::thread(&PayloadIpcClient::reader_thread_func, this);
    m_writer_thread = std::thread(&PayloadIpcClient::writer_thread_func, this);
    return true;
#else
    (void)timeout_ms;
    return false;
#endif
}

void PayloadIpcClient::disconnect() {
    m_running = false;
    m_queue_cv.notify_all();

#if defined(_WIN32)
    // 1. Drain remaining queued telemetry packets on writer thread before closing the handle
    if (m_writer_thread.joinable()) {
        m_writer_thread.join();
    }

    // 2. Cancel any blocking synchronous ReadFile on reader thread
    if (m_reader_thread.joinable()) {
        CancelSynchronousIo(static_cast<HANDLE>(m_reader_thread.native_handle()));
        m_reader_thread.join();
    }

    // 3. Flush and close pipe handle safely under m_send_mutex
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(static_cast<HANDLE>(m_pipe_handle));
            CloseHandle(static_cast<HANDLE>(m_pipe_handle));
            m_pipe_handle = nullptr;
        }
    }
    m_connected = false;
#endif

    // Clear residual queue
    std::lock_guard<std::mutex> lock(m_queue_mutex);
    std::queue<std::vector<uint8_t>> empty;
    std::swap(m_send_queue, empty);
}

bool PayloadIpcClient::enqueue_packet(std::vector<uint8_t>&& packet) {
    if (!m_connected.load()) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_queue_mutex);
        if (m_send_queue.size() >= MAX_QUEUE_SIZE) {
            // Drop oldest packet to prevent unbounded queue growth and game thread stalls
            m_send_queue.pop();
        }
        m_send_queue.push(std::move(packet));
    }
    m_queue_cv.notify_one();
    return true;
}

bool PayloadIpcClient::send_telemetry(const ipc::TelemetryPayload& payload) {
    if (!m_connected.load()) return false;
    auto buffer = ipc::serialize_telemetry(payload);
    return enqueue_packet(std::move(buffer));
}

bool PayloadIpcClient::send_status(const ipc::StatusPayload& payload) {
    if (!m_connected.load()) return false;
    auto buffer = ipc::serialize_status(payload);

#if defined(_WIN32)
    // Synchronously send and flush status packet so loader immediately receives
    // handshake and diagnostic reports without depending on writer thread scheduling
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
    if (ok && written == buffer.size()) {
        FlushFileBuffers(static_cast<HANDLE>(m_pipe_handle));
        return true;
    }
    return false;
#else
    return enqueue_packet(std::move(buffer));
#endif
}

void PayloadIpcClient::set_command_handler(CommandHandler handler) {
    m_command_handler = std::move(handler);
}

void PayloadIpcClient::writer_thread_func() {
#if defined(_WIN32)
    while (true) {
        std::vector<uint8_t> packet;
        {
            std::unique_lock<std::mutex> lock(m_queue_mutex);
            m_queue_cv.wait(lock, [this]() {
                return !m_running.load() || !m_send_queue.empty();
            });

            if (!m_running.load() && m_send_queue.empty()) {
                break;
            }

            if (!m_send_queue.empty()) {
                packet = std::move(m_send_queue.front());
                m_send_queue.pop();
            }
        }

        if (!packet.empty()) {
            std::lock_guard<std::mutex> lock(m_send_mutex);
            if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                const BOOL ok = WriteFile(
                    static_cast<HANDLE>(m_pipe_handle),
                    packet.data(),
                    static_cast<DWORD>(packet.size()),
                    &written,
                    nullptr
                );
                if (!ok) {
                    m_connected = false;
                    break;
                }
            }
        }
    }
#endif
}

void PayloadIpcClient::reader_thread_func() {
#if defined(_WIN32)
    constexpr size_t IPC_READ_BUFFER_SIZE = 1024;
    std::vector<uint8_t> buffer(IPC_READ_BUFFER_SIZE);

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
