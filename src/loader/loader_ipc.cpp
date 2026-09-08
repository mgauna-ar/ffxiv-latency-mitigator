#include "loader/loader_ipc.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace mitigator::loader {

namespace {
#if defined(_WIN32)
    constexpr DWORD PIPE_BUFFER_SIZE = 4096;
    constexpr DWORD PIPE_DEFAULT_TIMEOUT_MS = 0;
    constexpr size_t IPC_READ_BUFFER_SIZE = 2048;
#endif
}

LoaderIpcServer::LoaderIpcServer(const char* pipe_name)
    : m_pipe_name(pipe_name) {}

LoaderIpcServer::~LoaderIpcServer() {
    stop();
}

bool LoaderIpcServer::start() {
#if defined(_WIN32)
    if (m_running.load()) {
        return true;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = FALSE;

    // 1. Programmatically initialize a Security Descriptor with an explicit NULL DACL.
    // A NULL DACL grants unrestricted access to all users and processes regardless of privilege.
    SECURITY_DESCRIPTOR sd{};
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE);

    // 2. Also attempt to apply a Low Mandatory Integrity Label SACL:
    // S:(ML;;NW;;;LW) allows Low & Medium integrity processes (e.g. non-elevated game)
    // to write to a High integrity (Administrator) server pipe.
    PSECURITY_DESCRIPTOR p_ml_sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorA(
            "S:(ML;;NW;;;LW)",
            SDDL_REVISION_1,
            &p_ml_sd,
            nullptr)) {
        PACL p_sacl = nullptr;
        BOOL sacl_present = FALSE;
        BOOL sacl_defaulted = FALSE;
        if (GetSecurityDescriptorSacl(p_ml_sd, &sacl_present, &p_sacl, &sacl_defaulted) && sacl_present && p_sacl) {
            SetSecurityDescriptorSacl(&sd, TRUE, p_sacl, FALSE);
        }
    }

    sa.lpSecurityDescriptor = &sd;

    HANDLE h_pipe = CreateNamedPipeA(
        m_pipe_name,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
        PIPE_UNLIMITED_INSTANCES,
        PIPE_BUFFER_SIZE,
        PIPE_BUFFER_SIZE,
        PIPE_DEFAULT_TIMEOUT_MS,
        &sa
    );

    if (h_pipe == INVALID_HANDLE_VALUE) {
        // If creating with the SACL failed (e.g. OS restricted SACL assignment),
        // retry with NULL DACL alone.
        SECURITY_DESCRIPTOR sd_dacl_only{};
        InitializeSecurityDescriptor(&sd_dacl_only, SECURITY_DESCRIPTOR_REVISION);
        SetSecurityDescriptorDacl(&sd_dacl_only, TRUE, nullptr, FALSE);
        sa.lpSecurityDescriptor = &sd_dacl_only;

        h_pipe = CreateNamedPipeA(
            m_pipe_name,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_BUFFER_SIZE,
            PIPE_BUFFER_SIZE,
            PIPE_DEFAULT_TIMEOUT_MS,
            &sa
        );
    }

    if (p_ml_sd) {
        LocalFree(p_ml_sd);
    }

    if (h_pipe == INVALID_HANDLE_VALUE) {
        return false;
    }

    m_pipe_handle = h_pipe;
    m_running = true;

    m_worker_thread = std::thread(&LoaderIpcServer::server_worker_thread, this);
    return true;
#else
    return false;
#endif
}

void LoaderIpcServer::stop() {
    m_running = false;
    m_connected = false;
    m_status_received = false;

#if defined(_WIN32)
    // If the worker thread is blocking in ConnectNamedPipe, wake it up with a dummy client connection
    if (m_pipe_name && m_worker_thread.joinable()) {
        HANDLE h_wake = CreateFileA(
            m_pipe_name,
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (h_wake != INVALID_HANDLE_VALUE) {
            CloseHandle(h_wake);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_send_mutex);
        if (m_pipe_handle && m_pipe_handle != INVALID_HANDLE_VALUE) {
            DisconnectNamedPipe(static_cast<HANDLE>(m_pipe_handle));
            CloseHandle(static_cast<HANDLE>(m_pipe_handle));
            m_pipe_handle = nullptr;
        }
    }
#endif

    if (m_worker_thread.joinable()) {
        m_worker_thread.join();
    }
}

bool LoaderIpcServer::send_command(const ipc::CommandPayload& cmd) {
    if (!m_connected.load()) return false;

    const auto buffer = ipc::serialize_command(cmd);

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

bool LoaderIpcServer::request_unhook() {
    ipc::CommandPayload cmd{};
    cmd.command_type = static_cast<uint32_t>(ipc::CommandType::UnhookAndExit);
    return send_command(cmd);
}

bool LoaderIpcServer::set_dry_run(bool enabled) {
    ipc::CommandPayload cmd{};
    cmd.command_type = static_cast<uint32_t>(ipc::CommandType::SetDryRun);
    cmd.param_uint = enabled ? 1 : 0;
    return send_command(cmd);
}

bool LoaderIpcServer::set_verbose(bool enabled) {
    ipc::CommandPayload cmd{};
    cmd.command_type = static_cast<uint32_t>(ipc::CommandType::SetVerbose);
    cmd.param_uint = enabled ? 1 : 0;
    return send_command(cmd);
}

bool LoaderIpcServer::set_target_ping(float target_ping_ms) {
    ipc::CommandPayload cmd{};
    cmd.command_type = static_cast<uint32_t>(ipc::CommandType::SetTargetPing);
    cmd.param_float = target_ping_ms;
    return send_command(cmd);
}

bool LoaderIpcServer::set_min_lock(float min_lock_ms) {
    ipc::CommandPayload cmd{};
    cmd.command_type = static_cast<uint32_t>(ipc::CommandType::SetMinAnimationLock);
    cmd.param_float = min_lock_ms;
    return send_command(cmd);
}

bool LoaderIpcServer::reset_stats() {
    ipc::CommandPayload cmd{};
    cmd.command_type = static_cast<uint32_t>(ipc::CommandType::ResetStats);
    return send_command(cmd);
}

void LoaderIpcServer::server_worker_thread() {
#if defined(_WIN32)
    const auto h_pipe = static_cast<HANDLE>(m_pipe_handle);

    // Wait for the injected DLL to connect to our named pipe
    const BOOL client_connected = ConnectNamedPipe(h_pipe, nullptr) ?
        TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

    if (!client_connected) {
        m_running = false;
        return;
    }

    m_connected = true;
    std::vector<uint8_t> buffer(IPC_READ_BUFFER_SIZE);

    while (m_running.load()) {
        DWORD bytes_read = 0;
        BOOL ok = ReadFile(
            h_pipe,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytes_read,
            nullptr
        );

        if (!ok || bytes_read == 0) {
            // Client closed connection or unhooked
            m_connected = false;
            break;
        }

        std::span<const uint8_t> data_span(buffer.data(), bytes_read);
        const auto hdr = ipc::parse_header(data_span);
        if (!hdr.has_value()) {
            continue;
        }

        const auto payload_span = data_span.subspan(sizeof(ipc::Header));

        if (hdr->type == static_cast<uint16_t>(ipc::PacketType::Telemetry)) {
            const auto telemetry = ipc::deserialize_telemetry(payload_span);
            if (telemetry.has_value() && m_on_telemetry) {
                m_on_telemetry(*telemetry);
            }
        } else if (hdr->type == static_cast<uint16_t>(ipc::PacketType::Status)) {
            const auto status = ipc::deserialize_status(payload_span);
            if (status.has_value()) {
                {
                    std::lock_guard<std::mutex> lock(m_status_mutex);
                    m_last_status = *status;
                }
                m_status_received = true;
                if (m_on_status) {
                    m_on_status(*status);
                }
            }
        }
    }

    m_connected = false;
#endif
}

} // namespace mitigator::loader
