#pragma once

#include "mitigator/ipc_protocol.hpp"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>

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

    /// Connects to the loader's Named Pipe server and starts reader thread.
    bool connect(uint32_t timeout_ms = 3000);

    /// Disconnects and shuts down reader thread.
    void disconnect();

    /// Sends a telemetry packet to the loader.
    bool send_telemetry(const ipc::TelemetryPayload& payload);

    /// Sends a status packet to the loader.
    bool send_status(const ipc::StatusPayload& payload);

    /// Sets callback invoked when a command is received from the loader.
    void set_command_handler(CommandHandler handler);

    /// Returns true if currently connected to pipe.
    [[nodiscard]] bool is_connected() const { return m_connected.load(); }

private:
    void reader_thread_func();

    [[maybe_unused]] const char* m_pipe_name;
    [[maybe_unused]] void* m_pipe_handle{nullptr}; // HANDLE
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_running{false};
    std::thread m_reader_thread;
    std::mutex m_send_mutex;
    CommandHandler m_command_handler;
};

} // namespace mitigator::payload
