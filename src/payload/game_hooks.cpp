#include "payload/game_hooks.hpp"
#include "payload/payload_logger.hpp"
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
using FnUseActionLocation = uint8_t(FFXIV_FASTCALL*)(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    game::Vector3* target_location,
    uint32_t extra_param,
    uint8_t a7
);

using FnReceiveActionEffect = void(FFXIV_FASTCALL*)(
    uint32_t source_id,
    void* source_character,
    game::Vector3* pos,
    game::ActionEffectHeader* effect_header,
    void* effect_data,
    void* targets
);

#if defined(_WIN32)
// Trampolines
FnUseActionLocation fp_original_use_action_location = nullptr;
FnReceiveActionEffect fp_original_receive_action_effect = nullptr;

static void OnActionDispatched(uint32_t action_id, uint32_t seq) {
    auto* mitigator = s_mitigator.load(std::memory_order_acquire);
    if (mitigator != nullptr) {
        mitigator->record_action_request(action_id, seq);
        log_debug("UseActionLocation: accepted action=" + std::to_string(action_id) + " seq=" + std::to_string(seq));
    }
}

// Detour implementations with SEH and RAII scope protection
static uint8_t DetourUseActionLocationProtected(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    game::Vector3* target_location,
    uint32_t extra_param,
    uint8_t a7
) {
    if (self != nullptr) {
        s_action_manager.store(self, std::memory_order_release);
    }

    if (!fp_original_use_action_location) {
        return 0;
    }

    uint8_t ret = 0;
    MITIGATOR_SEH_TRY {
        ret = fp_original_use_action_location(
            self, action_type, action_id, target_id, target_location, extra_param, a7
        );
    }
    MITIGATOR_SEH_EXCEPT {
        return 0;
    }

    // Check if action was accepted and dispatched.
    if (ret != 0) {
        const uint32_t seq = (self != nullptr) ? static_cast<uint32_t>(self->current_sequence) : 0;
        OnActionDispatched(action_id, seq);
    }

    return ret;
}

uint8_t FFXIV_FASTCALL DetourUseActionLocation(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    game::Vector3* target_location,
    uint32_t extra_param,
    uint8_t a7
) {
    DetourScope scope;
    return DetourUseActionLocationProtected(
        self, action_type, action_id, target_id, target_location, extra_param, a7
    );
}

static void ProcessActionEffect(game::ActionEffectHeader* effect_header, float old_lock) {
    auto* mgr = s_action_manager.load(std::memory_order_acquire);
    if (mgr == nullptr || effect_header == nullptr) {
        return;
    }

    const float new_lock = mgr->animation_lock;
    const bool lock_changed = (new_lock != old_lock);
    const bool is_our_sequence = (effect_header->source_sequence != 0);

    // Zone-wide action effect isolation:
    // Only mitigate if animation lock was actually changed or belongs to our sequence,
    // and new lock is positive and finite
    if ((!lock_changed && !is_our_sequence) || new_lock <= game::definitions::MIN_ACTION_EFFECT_LOCK_SECONDS || !std::isfinite(new_lock)) {
        return;
    }

    auto* mitigator = s_mitigator.load(std::memory_order_acquire);
    if (mitigator == nullptr) {
        return;
    }

    const uint32_t action_id = effect_header->action_id;
    const uint32_t sequence = static_cast<uint32_t>(effect_header->source_sequence);
    const double original_lock_ms = static_cast<double>(new_lock) * constants::MS_PER_SECOND;

    const auto result = mitigator->calculate_mitigation(
        action_id,
        sequence,
        original_lock_ms
    );

    if (result.applied) {
        const float new_lock_seconds = static_cast<float>(result.adjusted_lock_ms / constants::MS_PER_SECOND);
        mgr->animation_lock = new_lock_seconds;
    }

    log_debug("ReceiveActionEffect: action=" + std::to_string(action_id) +
              " seq=" + std::to_string(sequence) +
              " old_lock=" + std::to_string(old_lock) +
              " new_lock=" + std::to_string(new_lock) +
              " adjusted=" + std::to_string(result.adjusted_lock_ms / constants::MS_PER_SECOND) +
              " applied=" + (result.applied ? "true" : "false"));

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

static void DetourReceiveActionEffectProtected(
    uint32_t source_id,
    void* source_character,
    game::Vector3* pos,
    game::ActionEffectHeader* effect_header,
    void* effect_data,
    void* targets
) {
    float old_lock = 0.0f;
    game::ActionManager* mgr = s_action_manager.load(std::memory_order_acquire);
    if (mgr != nullptr) {
        old_lock = mgr->animation_lock;
    }

    MITIGATOR_SEH_TRY {
        if (fp_original_receive_action_effect != nullptr) {
            fp_original_receive_action_effect(
                source_id, source_character, pos, effect_header, effect_data, targets
            );
        }
    }
    MITIGATOR_SEH_EXCEPT {
        return;
    }

    ProcessActionEffect(effect_header, old_lock);
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

    const auto mh_init = MH_Initialize();
    if (mh_init != MH_OK && mh_init != MH_ERROR_ALREADY_INITIALIZED) {
        m_last_error = "MinHook initialization failed";
        return false;
    }

    uint32_t hooked = 0;

    // 1. Hook UseActionLocation
    const auto sig_use_action = memory::Signature::parse(game::signatures::USE_ACTION_LOCATION_PRIMARY);
    uintptr_t addr_use_action = memory::scan_module_section(nullptr, sig_use_action);
    if (addr_use_action == 0) {
        const auto sig_fallback = memory::Signature::parse(game::signatures::USE_ACTION_LOCATION_FALLBACK);
        addr_use_action = memory::scan_module_section(nullptr, sig_fallback);
    }
    if (addr_use_action == 0) {
        const auto sig_legacy = memory::Signature::parse(game::signatures::USE_ACTION_LOCATION_LEGACY);
        addr_use_action = memory::scan_module_section(nullptr, sig_legacy);
    }
    log_debug("HookManager: UseActionLocation sig addr=" + (addr_use_action ? std::to_string(addr_use_action) : "NOT FOUND"));

    if (addr_use_action != 0 && *reinterpret_cast<const uint8_t*>(addr_use_action) == 0xE8) {
        const uintptr_t target = memory::resolve_call_relative(addr_use_action);
        log_debug("HookManager: UseActionLocation call-site resolved -> " + std::to_string(target));
        addr_use_action = target;
    }

    if (addr_use_action != 0) {
        const auto status = MH_CreateHook(
            reinterpret_cast<LPVOID>(addr_use_action),
            reinterpret_cast<LPVOID>(&DetourUseActionLocation),
            reinterpret_cast<LPVOID*>(&fp_original_use_action_location)
        );
        log_debug("HookManager: MH_CreateHook(UseActionLocation) result: " + std::to_string(status));
        if (status == MH_OK) {
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
    if (addr_recv_effect == 0) {
        const auto sig_legacy = memory::Signature::parse(game::signatures::RECEIVE_ACTION_EFFECT_LEGACY);
        addr_recv_effect = memory::scan_module_section(nullptr, sig_legacy);
    }
    log_debug("HookManager: ReceiveActionEffect sig addr=" + (addr_recv_effect ? std::to_string(addr_recv_effect) : "NOT FOUND"));

    if (addr_recv_effect != 0 && *reinterpret_cast<const uint8_t*>(addr_recv_effect) == 0xE8) {
        const uintptr_t target = memory::resolve_call_relative(addr_recv_effect);
        log_debug("HookManager: ReceiveActionEffect call-site resolved -> " + std::to_string(target));
        addr_recv_effect = target;
    }

    if (addr_recv_effect != 0) {
        const auto status = MH_CreateHook(
            reinterpret_cast<LPVOID>(addr_recv_effect),
            reinterpret_cast<LPVOID>(&DetourReceiveActionEffect),
            reinterpret_cast<LPVOID*>(&fp_original_receive_action_effect)
        );
        log_debug("HookManager: MH_CreateHook(ReceiveActionEffect) result: " + std::to_string(status));
        if (status == MH_OK) {
            ++hooked;
        }
    }

    // 3. Attempt initial static pointer acquisition for ActionManager
    uintptr_t addr_action_mgr_insn = 0;
    const auto sig_action_mgr_prim = memory::Signature::parse(game::signatures::ACTION_MANAGER_INSTANCE_PRIMARY);
    addr_action_mgr_insn = memory::scan_module_section(nullptr, sig_action_mgr_prim);
    if (addr_action_mgr_insn == 0) {
        const auto sig_action_mgr_fb = memory::Signature::parse(game::signatures::ACTION_MANAGER_INSTANCE_FALLBACK);
        addr_action_mgr_insn = memory::scan_module_section(nullptr, sig_action_mgr_fb);
    }
    if (addr_action_mgr_insn == 0) {
        const auto sig_action_mgr_leg = memory::Signature::parse(game::signatures::ACTION_MANAGER_INSTANCE_LEGACY);
        addr_action_mgr_insn = memory::scan_module_section(nullptr, sig_action_mgr_leg);
    }
    log_debug("HookManager: ActionManager sig addr=" + (addr_action_mgr_insn ? std::to_string(addr_action_mgr_insn) : "NOT FOUND"));
    if (addr_action_mgr_insn != 0) {
        const uint8_t op = *reinterpret_cast<const uint8_t*>(addr_action_mgr_insn + 1);
        const uintptr_t p_static_mgr = memory::resolve_rip_relative(
            addr_action_mgr_insn,
            game::definitions::ACTION_MGR_RIP_DISP_OFFSET,
            game::definitions::ACTION_MGR_RIP_INSN_LEN
        );
        log_debug("HookManager: ActionManager RIP resolved=" + std::to_string(p_static_mgr) + " (op=" + std::to_string(static_cast<int>(op)) + ")");
        if (p_static_mgr != 0) {
            game::ActionManager* mgr = nullptr;
            if (op == 0x8D) {
                // LEA rcx, [rip + disp32] (Dawntrail 7.x): direct static struct address
                mgr = reinterpret_cast<game::ActionManager*>(p_static_mgr);
            } else if (op == 0x8B) {
                // MOV rcx, [rip + disp32] (Legacy 6.x): pointer-to-pointer
                mgr = safe_read_action_manager_ptr(reinterpret_cast<game::ActionManager**>(p_static_mgr));
            }
            if (mgr != nullptr) {
                s_action_manager.store(mgr, std::memory_order_release);
                log_debug("HookManager: ActionManager instance acquired: " + std::to_string(reinterpret_cast<uintptr_t>(mgr)));
            }
        }
    }

    // Both UseActionLocation and ReceiveActionEffect are strictly required for latency mitigation
    const bool primary_hooks_ok = (addr_use_action != 0 && addr_recv_effect != 0 && hooked >= game::definitions::MIN_REQUIRED_PRIMARY_HOOKS);
    if (primary_hooks_ok) {
        MH_EnableHook(MH_ALL_HOOKS);
        m_hook_count = hooked;
        m_installed = true;
        m_last_error = "OK";
        log_debug("HookManager: All primary hooks enabled successfully! Hooked count: " + std::to_string(hooked));
        return true;
    }

    if (addr_use_action == 0 && addr_recv_effect == 0) {
        m_last_error = "Signatures not found: UseActionLocation & ReceiveActionEffect";
    } else if (addr_use_action == 0) {
        m_last_error = "Signature not found: UseActionLocation";
    } else if (addr_recv_effect == 0) {
        m_last_error = "Signature not found: ReceiveActionEffect";
    } else if (hooked < game::definitions::MIN_REQUIRED_PRIMARY_HOOKS) {
        m_last_error = "MinHook failed to install primary hooks";
    } else {
        m_last_error = "Hook installation failed";
    }
    log_debug("HookManager: installation failed: " + std::string(m_last_error));

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
    s_action_manager.store(nullptr, std::memory_order_release);
    s_mitigator.store(nullptr, std::memory_order_release);
    s_ipc.store(nullptr, std::memory_order_release);

    m_hook_count = 0;
#endif
}

} // namespace mitigator::payload
