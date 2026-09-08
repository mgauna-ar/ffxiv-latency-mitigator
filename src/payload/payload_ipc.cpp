#include "payload/payload_ipc.hpp"
#include "payload/payload_logger.hpp"
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

    log_debug("PayloadIpcClient::connect: connecting to pipe " + std::string(m_pipe_name));
    const auto start_time = std::chrono::steady_clock::now();
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    DWORD last_logged_err = 0;

    m_stop_event = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop_event) {
        return false;
    }

    while (true) {
        hPipe = CreateFileA(
            m_pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr
        );

        if (hPipe != INVALID_HANDLE_VALUE) {
            break;
        }

        const DWORD err = GetLastError();
        if (err != last_logged_err) {
            log_debug("PayloadIpcClient::connect: CreateFileA failed, Win32 Error: " + std::to_string(err));
            last_logged_err = err;
        }

        const auto elapsed_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time
            ).count()
        );

        if (elapsed_ms >= timeout_ms) {
            log_debug("PayloadIpcClient::connect: timeout expired (" + std::to_string(timeout_ms) + "ms), aborting.");
            if (m_stop_event) {
                CloseHandle(static_cast<HANDLE>(m_stop_event));
                m_stop_event = nullptr;
            }
            return false;
        }

        const uint32_t remaining_ms = timeout_ms - elapsed_ms;

        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeA(m_pipe_name, (std::min)(remaining_ms, 200u));
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    log_debug("PayloadIpcClient::connect: CreateFileA succeeded! Setting message-read mode...");

    // Set message-read mode
    DWORD mode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr)) {
        const DWORD err = GetLastError();
        log_debug("PayloadIpcClient::connect: SetNamedPipeHandleState failed, Win32 Error: " + std::to_string(err));
        CloseHandle(hPipe);
        if (m_stop_event) {
            CloseHandle(static_cast<HANDLE>(m_stop_event));
            m_stop_event = nullptr;
        }
        return false;
    }

    log_debug("PayloadIpcClient::connect: SetNamedPipeHandleState succeeded. IPC channel open!");
    m_pipe_handle = hPipe;
    m_connected = true;
    m_running = true;

    return true;
#else
    (void)timeout_ms;
    return false;
#endif
}

void PayloadIpcClient::start_worker_threads() {
#if defined(_WIN32)
    if (!m_reader_thread.joinable()) {
        m_reader_thread = std::thread(&PayloadIpcClient::reader_thread_func, this);
    }
    if (!m_writer_thread.joinable()) {
        m_writer_thread = std::thread(&PayloadIpcClient::writer_thread_func, this);
    }
#endif
}

void PayloadIpcClient::disconnect() {
    m_running = false;
    m_queue_cv.notify_all();

#if defined(_WIN32)
    if (m_stop_event) {
        SetEvent(static_cast<HANDLE>(m_stop_event));
    }

    // 1. Drain remaining queued telemetry packets on writer thread before closing the handle
    if (m_writer_thread.joinable()) {
        m_writer_thread.join();
    }

    // 2. Cancel any pending overlapped read on reader thread
    if (m_reader_thread.joinable()) {
        if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
            CancelIoEx(static_cast<HANDLE>(m_pipe_handle), nullptr);
        }
        m_reader_thread.join();
    }

    // 3. Close pipe handle safely under m_send_mutex
    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(static_cast<HANDLE>(m_pipe_handle));
            m_pipe_handle = nullptr;
        }
    }
    if (m_stop_event) {
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
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
    // Synchronously send status packet via overlapped I/O so loader immediately receives
    // handshake without depending on writer thread scheduling or blocking on kernel mutexes.
    std::lock_guard<std::mutex> lock(m_send_mutex);
    if (!m_pipe_handle || m_pipe_handle == INVALID_HANDLE_VALUE) return false;

    OVERLAPPED ov_write{};
    ov_write.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ov_write.hEvent) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(
        static_cast<HANDLE>(m_pipe_handle),
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &written,
        &ov_write
    );
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = GetOverlappedResult(static_cast<HANDLE>(m_pipe_handle), &ov_write, &written, TRUE);
    }
    CloseHandle(ov_write.hEvent);
    log_debug("PayloadIpcClient::send_status: WriteFile ok=" + std::to_string(ok) +
              ", written=" + std::to_string(written) + "/" + std::to_string(buffer.size()));
    return (ok && written == buffer.size());
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
                OVERLAPPED ov_write{};
                ov_write.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
                if (ov_write.hEvent) {
                    DWORD written = 0;
                    BOOL ok = WriteFile(
                        static_cast<HANDLE>(m_pipe_handle),
                        packet.data(),
                        static_cast<DWORD>(packet.size()),
                        &written,
                        &ov_write
                    );
                    if (!ok && GetLastError() == ERROR_IO_PENDING) {
                        ok = GetOverlappedResult(static_cast<HANDLE>(m_pipe_handle), &ov_write, &written, TRUE);
                    }
                    CloseHandle(ov_write.hEvent);
                    if (!ok) {
                        m_connected = false;
                        break;
                    }
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
    const auto h_stop = static_cast<HANDLE>(m_stop_event);

    OVERLAPPED ov_read{};
    ov_read.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!ov_read.hEvent) {
        m_connected = false;
        return;
    }

    while (m_running.load()) {
        ResetEvent(ov_read.hEvent);
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(
            static_cast<HANDLE>(m_pipe_handle),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_read,
            &ov_read
        );

        if (!ok) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                HANDLE wait_events[2] = { ov_read.hEvent, h_stop };
                const DWORD wait_res = WaitForMultipleObjects(2, wait_events, FALSE, INFINITE);
                if (wait_res == WAIT_OBJECT_0) {
                    ok = GetOverlappedResult(static_cast<HANDLE>(m_pipe_handle), &ov_read, &bytes_read, FALSE);
                } else {
                    CancelIoEx(static_cast<HANDLE>(m_pipe_handle), &ov_read);
                    break;
                }
            } else {
                // Pipe broken or disconnected
                break;
            }
        }

        if (!ok || bytes_read == 0) {
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

    CloseHandle(ov_read.hEvent);
    m_connected = false;
#endif
}

} // namespace mitigator::payload
