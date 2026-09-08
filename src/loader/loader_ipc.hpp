#pragma once

#include "mitigator/ipc_protocol.hpp"
#include <functional>
#include <atomic>
#include <thread>
#include <mutex>
#include <optional>

namespace mitigator::loader {

using TelemetryCallback = std::function<void(const ipc::TelemetryPayload&)>;
using StatusCallback = std::function<void(const ipc::StatusPayload&)>;

/**
 * @brief Named Pipe IPC server running inside the console loader.
 *
 * Listens for connection from the injected DLL, reads telemetry stream,
 * and transmits hotkey commands to the game process.
 */
class LoaderIpcServer {
public:
    explicit LoaderIpcServer(const char* pipe_name = ipc::DEFAULT_PIPE_NAME);
    ~LoaderIpcServer();

    /// Starts the server and listens for client connection asynchronously.
    bool start();

    /// Stops server and closes connection.
    void stop();

    /// Sends a command to the injected payload DLL.
    bool send_command(const ipc::CommandPayload& cmd);

    /// Convenience command helpers
    bool request_unhook();
    bool set_dry_run(bool enabled);
    bool set_verbose(bool enabled);
    bool set_target_ping(float target_ping_ms);
    bool set_min_lock(float min_lock_ms);
    bool reset_stats();

    /// Registers event callbacks
    void set_telemetry_callback(TelemetryCallback cb) { m_on_telemetry = std::move(cb); }
    void set_status_callback(StatusCallback cb) { m_on_status = std::move(cb); }

    [[nodiscard]] bool is_connected() const { return m_connected.load(); }
    [[nodiscard]] bool is_running() const { return m_running.load(); }
    [[nodiscard]] bool has_received_status() const { return m_status_received.load(); }
    [[nodiscard]] std::optional<ipc::StatusPayload> last_status() const {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        return m_last_status;
    }

private:
    void server_worker_thread();

    [[maybe_unused]] const char* m_pipe_name;
    [[maybe_unused]] void* m_pipe_handle{nullptr}; // HANDLE
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_status_received{false};
    std::thread m_worker_thread;
    std::mutex m_send_mutex;
    mutable std::mutex m_status_mutex;
    std::optional<ipc::StatusPayload> m_last_status;

    TelemetryCallback m_on_telemetry;
    StatusCallback m_on_status;
};

} // namespace mitigator::loader
