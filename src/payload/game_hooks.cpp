#include "payload/game_hooks.hpp"
#include "mitigator/game_structures.hpp"
#include "mitigator/game_definitions.hpp"
#include "mitigator/sigscan.hpp"
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "MinHook.h"
#endif

namespace mitigator::payload {

namespace {

#if defined(_MSC_VER) || (defined(_WIN32) && defined(__clang__))
#define MITIGATOR_SEH_TRY __try
#define MITIGATOR_SEH_EXCEPT __except (EXCEPTION_EXECUTE_HANDLER)
#else
#define MITIGATOR_SEH_TRY if (true)
#define MITIGATOR_SEH_EXCEPT else
#endif

#if defined(_WIN32)
#define FFXIV_FASTCALL __fastcall

// Atomic in-flight detour invocation counter to prevent uninstall race conditions
std::atomic<int32_t> g_in_flight_detours{0};

// RAII counter increment/decrement for detour invocations
struct DetourScope {
    DetourScope() { g_in_flight_detours.fetch_add(1, std::memory_order_acquire); }
    ~DetourScope() { g_in_flight_detours.fetch_sub(1, std::memory_order_release); }
    DetourScope(const DetourScope&) = delete;
    DetourScope& operator=(const DetourScope&) = delete;
};

// Global pointers managed safely by HookManager
std::atomic<AnimationLockMitigator*> s_mitigator{nullptr};
std::atomic<PayloadIpcClient*> s_ipc{nullptr};
std::atomic<game::ActionManager*> s_action_manager{nullptr};

static game::ActionManager* safe_read_action_manager_ptr(game::ActionManager** pp_mgr) {
    MITIGATOR_SEH_TRY {
        if (pp_mgr != nullptr) {
            return *pp_mgr;
        }
    }
    MITIGATOR_SEH_EXCEPT {
        return nullptr;
    }
    return nullptr;
}
#else
#define FFXIV_FASTCALL
#endif

// Function pointer typedefs
using FnUseActionLocation = bool(FFXIV_FASTCALL*)(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    uint32_t extra_param,
    uint32_t use_type,
    int32_t pvp,
    game::Vector3* target_location
);

using FnReceiveActionEffect = void(FFXIV_FASTCALL*)(
    uint32_t source_id,
    void* source_character,
    game::Vector3* pos,
    game::ActionEffectHeader* effect_header,
    void* effect_data,
    void* targets
);

using FnCastBegin = void(FFXIV_FASTCALL*)(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    float cast_time,
    float current_cast_time
);

using FnCastInterrupt = void(FFXIV_FASTCALL*)(
    game::ActionManager* self
);

#if defined(_WIN32)
// Trampolines
FnUseActionLocation fp_original_use_action_location = nullptr;
FnReceiveActionEffect fp_original_receive_action_effect = nullptr;
FnCastBegin fp_original_cast_begin = nullptr;
FnCastInterrupt fp_original_cast_interrupt = nullptr;

// Detour implementations with SEH and RAII scope protection
static bool DetourUseActionLocationProtected(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    uint32_t extra_param,
    uint32_t use_type,
    int32_t pvp,
    game::Vector3* target_location
) {
    MITIGATOR_SEH_TRY {
        if (self != nullptr) {
            s_action_manager.store(self, std::memory_order_relaxed);
        }

        if (!fp_original_use_action_location) {
            return false;
        }

        const bool ret = fp_original_use_action_location(
            self, action_type, action_id, target_id, extra_param, use_type, pvp, target_location
        );

        // Check if action was accepted and dispatched. If rejected, or if the action was queued
        // in client buffer (self->is_queued), do not record timestamp now to avoid RTT distortion.
        const bool is_queued = (self != nullptr && self->is_queued);
        auto* mitigator = s_mitigator.load(std::memory_order_acquire);
        if (ret && !is_queued && mitigator != nullptr) {
            const uint32_t seq = (self != nullptr) ? static_cast<uint32_t>(self->current_sequence) : 0;
            mitigator->record_action_request(action_id, seq);
        }

        return ret;
    }
    MITIGATOR_SEH_EXCEPT {
        return false;
    }
}

bool FFXIV_FASTCALL DetourUseActionLocation(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    uint32_t extra_param,
    uint32_t use_type,
    int32_t pvp,
    game::Vector3* target_location
) {
    DetourScope scope;
    return DetourUseActionLocationProtected(
        self, action_type, action_id, target_id, extra_param, use_type, pvp, target_location
    );
}

static void DetourReceiveActionEffectProtected(
    uint32_t source_id,
    void* source_character,
    game::Vector3* pos,
    game::ActionEffectHeader* effect_header,
    void* effect_data,
    void* targets
) {
    MITIGATOR_SEH_TRY {
        if (!fp_original_receive_action_effect) {
            return;
        }

        auto* mgr = s_action_manager.load(std::memory_order_acquire);
        const float old_lock = (mgr != nullptr) ? mgr->animation_lock : 0.0f;

        // Call original game function to process effect and assign normal animationLock
        fp_original_receive_action_effect(
            source_id, source_character, pos, effect_header, effect_data, targets
        );

        mgr = s_action_manager.load(std::memory_order_acquire);
        const float new_lock = (mgr != nullptr) ? mgr->animation_lock : 0.0f;

        // Zone-wide action effect isolation:
        // Only mitigate if animation lock was actually increased for local player and is valid
        if (mgr == nullptr || !(new_lock > old_lock && new_lock > game::definitions::MIN_ACTION_EFFECT_LOCK_SECONDS && std::isfinite(new_lock))) {
            return;
        }

        auto* mitigator = s_mitigator.load(std::memory_order_acquire);
        if (effect_header == nullptr || mitigator == nullptr) {
            return;
        }

        const double original_lock_ms = static_cast<double>(new_lock) * constants::MS_PER_SECOND;
        const auto result = mitigator->calculate_mitigation(
            effect_header->action_id,
            effect_header->global_sequence,
            original_lock_ms
        );

        if (result.applied) {
            const float new_lock_seconds = static_cast<float>(result.adjusted_lock_ms / constants::MS_PER_SECOND);
            mgr->animation_lock = new_lock_seconds;
        }

        auto* ipc = s_ipc.load(std::memory_order_acquire);
        if (ipc != nullptr && ipc->is_connected()) {
            ipc::TelemetryPayload payload{};
            payload.action_id = result.action_id;
            payload.sequence = result.sequence;
            payload.original_lock_ms = static_cast<float>(result.original_lock_ms);
            payload.adjusted_lock_ms = static_cast<float>(result.adjusted_lock_ms);
            payload.delay_reduced_ms = static_cast<float>(result.delay_reduced_ms);
            payload.measured_rtt_ms = static_cast<float>(result.measured_rtt_ms);
            payload.smoothed_rtt_ms = static_cast<float>(result.smoothed_rtt_ms);
            payload.jitter_ms = static_cast<float>(mitigator->rtt_tracker().get_jitter_ms());
            payload.clamped_floor = result.clamped_by_floor ? 1 : 0;
            payload.dry_run = mitigator->get_config().dry_run ? 1 : 0;
            payload.applied = result.applied ? 1 : 0;
            payload.cast_active = result.cast_active ? 1 : 0;
            payload.timestamp_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()
                ).count()
            );
            ipc->send_telemetry(payload);
        }
    }
    MITIGATOR_SEH_EXCEPT {
        return;
    }
}

void FFXIV_FASTCALL DetourReceiveActionEffect(
    uint32_t source_id,
    void* source_character,
    game::Vector3* pos,
    game::ActionEffectHeader* effect_header,
    void* effect_data,
    void* targets
) {
    DetourScope scope;
    DetourReceiveActionEffectProtected(
        source_id, source_character, pos, effect_header, effect_data, targets
    );
}

static void DetourCastBeginProtected(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    float cast_time,
    float current_cast_time
) {
    MITIGATOR_SEH_TRY {
        if (self != nullptr) {
            s_action_manager.store(self, std::memory_order_relaxed);
        }
        auto* mitigator = s_mitigator.load(std::memory_order_acquire);
        if (mitigator != nullptr) {
            mitigator->record_cast_begin(action_id, cast_time);
        }
        if (fp_original_cast_begin) {
            fp_original_cast_begin(self, action_type, action_id, cast_time, current_cast_time);
        }
    }
    MITIGATOR_SEH_EXCEPT {
        return;
    }
}

void FFXIV_FASTCALL DetourCastBegin(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    float cast_time,
    float current_cast_time
) {
    DetourScope scope;
    DetourCastBeginProtected(self, action_type, action_id, cast_time, current_cast_time);
}

static void DetourCastInterruptProtected(game::ActionManager* self) {
    MITIGATOR_SEH_TRY {
        if (self != nullptr) {
            s_action_manager.store(self, std::memory_order_relaxed);
        }
        auto* mitigator = s_mitigator.load(std::memory_order_acquire);
        if (mitigator != nullptr) {
            mitigator->record_cast_interrupt();
        }
        if (fp_original_cast_interrupt) {
            fp_original_cast_interrupt(self);
        }
    }
    MITIGATOR_SEH_EXCEPT {
        return;
    }
}

void FFXIV_FASTCALL DetourCastInterrupt(game::ActionManager* self) {
    DetourScope scope;
    DetourCastInterruptProtected(self);
}
#endif // defined(_WIN32)

} // anonymous namespace

HookManager& HookManager::instance() {
    static HookManager s_instance;
    return s_instance;
}

bool HookManager::install(AnimationLockMitigator* mitigator, PayloadIpcClient* ipc) {
#if defined(_WIN32)
    if (m_installed.load()) {
        return true;
    }

    s_mitigator.store(mitigator, std::memory_order_release);
    s_ipc.store(ipc, std::memory_order_release);

    if (MH_Initialize() != MH_OK) {
        return false;
    }

    uint32_t hooked = 0;

    // 1. Hook UseActionLocation
    const auto sig_use_action = memory::Signature::parse(game::signatures::USE_ACTION_LOCATION_PRIMARY);
    uintptr_t addr_use_action = memory::scan_module_section(nullptr, sig_use_action);
    if (addr_use_action == 0) {
        // Fallback pattern
        const auto sig_fallback = memory::Signature::parse(game::signatures::USE_ACTION_LOCATION_FALLBACK);
        addr_use_action = memory::scan_module_section(nullptr, sig_fallback);
    }

    if (addr_use_action != 0) {
        if (MH_CreateHook(
                reinterpret_cast<LPVOID>(addr_use_action),
                reinterpret_cast<LPVOID>(&DetourUseActionLocation),
                reinterpret_cast<LPVOID*>(&fp_original_use_action_location)
            ) == MH_OK) {
            ++hooked;
        }
    }

    // 2. Hook ReceiveActionEffect
    const auto sig_recv_effect = memory::Signature::parse(game::signatures::RECEIVE_ACTION_EFFECT_PRIMARY);
    uintptr_t addr_recv_effect = memory::scan_module_section(nullptr, sig_recv_effect);
    if (addr_recv_effect == 0) {
        const auto sig_fallback = memory::Signature::parse(game::signatures::RECEIVE_ACTION_EFFECT_FALLBACK);
        addr_recv_effect = memory::scan_module_section(nullptr, sig_fallback);
    }

    if (addr_recv_effect != 0) {
        if (MH_CreateHook(
                reinterpret_cast<LPVOID>(addr_recv_effect),
                reinterpret_cast<LPVOID>(&DetourReceiveActionEffect),
                reinterpret_cast<LPVOID*>(&fp_original_receive_action_effect)
            ) == MH_OK) {
            ++hooked;
        }
    }

    // 3. Hook CastBegin
    const auto sig_cast_begin = memory::Signature::parse(game::signatures::CAST_BEGIN_PRIMARY);
    const uintptr_t addr_cast_begin = memory::scan_module_section(nullptr, sig_cast_begin);
    if (addr_cast_begin != 0) {
        if (MH_CreateHook(
                reinterpret_cast<LPVOID>(addr_cast_begin),
                reinterpret_cast<LPVOID>(&DetourCastBegin),
                reinterpret_cast<LPVOID*>(&fp_original_cast_begin)
            ) == MH_OK) {
            ++hooked;
        }
    }

    // 4. Hook CastInterrupt
    const auto sig_cast_interrupt = memory::Signature::parse(game::signatures::CAST_INTERRUPT_PRIMARY);
    const uintptr_t addr_cast_interrupt = memory::scan_module_section(nullptr, sig_cast_interrupt);
    if (addr_cast_interrupt != 0) {
        if (MH_CreateHook(
                reinterpret_cast<LPVOID>(addr_cast_interrupt),
                reinterpret_cast<LPVOID>(&DetourCastInterrupt),
                reinterpret_cast<LPVOID*>(&fp_original_cast_interrupt)
            ) == MH_OK) {
            ++hooked;
        }
    }

    // 5. Attempt initial static pointer acquisition for ActionManager
    const auto sig_action_mgr = memory::Signature::parse(game::signatures::ACTION_MANAGER_INSTANCE_PRIMARY);
    const uintptr_t addr_action_mgr_insn = memory::scan_module_section(nullptr, sig_action_mgr);
    if (addr_action_mgr_insn != 0) {
        const uintptr_t p_static_mgr = memory::resolve_rip_relative(
            addr_action_mgr_insn,
            game::definitions::ACTION_MGR_RIP_DISP_OFFSET,
            game::definitions::ACTION_MGR_RIP_INSN_LEN
        );
        if (p_static_mgr != 0) {
            auto pp_mgr = reinterpret_cast<game::ActionManager**>(p_static_mgr);
            auto* mgr = safe_read_action_manager_ptr(pp_mgr);
            if (mgr != nullptr) {
                s_action_manager.store(mgr, std::memory_order_release);
            }
        }
    }

    // Both UseActionLocation and ReceiveActionEffect are strictly required for latency mitigation
    const bool primary_hooks_ok = (addr_use_action != 0 && addr_recv_effect != 0 && hooked >= game::definitions::MIN_REQUIRED_PRIMARY_HOOKS);
    if (primary_hooks_ok) {
        MH_EnableHook(MH_ALL_HOOKS);
        m_hook_count = hooked;
        m_installed = true;
        return true;
    }

    MH_Uninitialize();
    s_mitigator.store(nullptr, std::memory_order_release);
    s_ipc.store(nullptr, std::memory_order_release);
    return false;
#else
    (void)mitigator;
    (void)ipc;
    return false;
#endif
}

void HookManager::uninstall() {
#if defined(_WIN32)
    if (!m_installed.exchange(false)) {
        return;
    }

    // 1. Disable all MinHook hooks first so execution falls back to original code
    MH_DisableHook(MH_ALL_HOOKS);

    // 2. Wait until all in-flight detours have safely completed
    const auto start = std::chrono::steady_clock::now();
    while (g_in_flight_detours.load(std::memory_order_acquire) > 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
        ).count();
        if (elapsed >= constants::HOOK_DRAIN_TIMEOUT_MS) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(constants::HOOK_DRAIN_POLL_INTERVAL_MS));
    }

    // 3. Uninitialize MinHook and clear trampolines only after all detours have drained
    MH_Uninitialize();

    fp_original_use_action_location = nullptr;
    fp_original_receive_action_effect = nullptr;
    fp_original_cast_begin = nullptr;
    fp_original_cast_interrupt = nullptr;
    s_action_manager.store(nullptr, std::memory_order_release);
    s_mitigator.store(nullptr, std::memory_order_release);
    s_ipc.store(nullptr, std::memory_order_release);

    m_hook_count = 0;
#endif
}

} // namespace mitigator::payload
