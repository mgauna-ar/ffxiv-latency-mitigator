#pragma once

#include "mitigator/ipc_protocol.hpp"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <queue>
#include <vector>
#include <condition_variable>

namespace mitigator::payload {

using CommandHandler = std::function<void(const ipc::CommandPayload&)>;

/**
 * @brief Named Pipe IPC client running inside the injected DLL payload.
 *
 * Streams live telemetry to the console loader and receives real-time hotkey
 * commands (dry-run, target ping changes, clean unhook).
 */
class PayloadIpcClient {
public:
    explicit PayloadIpcClient(const char* pipe_name = ipc::DEFAULT_PIPE_NAME);
    ~PayloadIpcClient();

    /// Connects to the loader's Named Pipe server and starts reader and writer threads.
    bool connect(uint32_t timeout_ms = 3000);

    /// Disconnects and shuts down worker threads.
    void disconnect();

    /// Enqueues a telemetry packet to be sent non-blockingly to the loader.
    /// Drops packets if the queue is full to prevent freezing the game thread.
    bool send_telemetry(const ipc::TelemetryPayload& payload);

    /// Enqueues a status packet to be sent non-blockingly to the loader.
    bool send_status(const ipc::StatusPayload& payload);

    /// Starts reader and writer worker threads after initial handshake.
    void start_worker_threads();

    /// Sets callback invoked when a command is received from the loader.
    void set_command_handler(CommandHandler handler);

    /// Returns true if currently connected to pipe.
    [[nodiscard]] bool is_connected() const { return m_connected.load(); }

private:
    void reader_thread_func();
    void writer_thread_func();
    bool enqueue_packet(std::vector<uint8_t>&& packet);

    [[maybe_unused]] const char* m_pipe_name;
    [[maybe_unused]] void* m_pipe_handle{nullptr}; // HANDLE
    [[maybe_unused]] void* m_stop_event{nullptr};  // HANDLE
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};
    std::thread m_reader_thread;
    std::thread m_writer_thread;
    std::mutex m_send_mutex;
    std::mutex m_queue_mutex;
    std::condition_variable m_queue_cv;
    std::queue<std::vector<uint8_t>> m_send_queue;
    static constexpr size_t MAX_QUEUE_SIZE = 256;
    CommandHandler m_command_handler;
};

} // namespace mitigator::payload
