#include "payload/game_hooks.hpp"
#include "mitigator/game_structures.hpp"
#include "mitigator/sigscan.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "MinHook.h"
#endif

namespace mitigator::payload {

namespace {

#if defined(_WIN32)
#define FFXIV_FASTCALL __fastcall
// Global pointers managed safely by HookManager
AnimationLockMitigator* s_mitigator = nullptr;
PayloadIpcClient* s_ipc = nullptr;
game::ActionManager* s_action_manager = nullptr;
#else
#define FFXIV_FASTCALL
#endif

// Function pointer typedefs
using FnUseActionLocation = int64_t(FFXIV_FASTCALL*)(
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

// Detour implementations
int64_t FFXIV_FASTCALL DetourUseActionLocation(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    uint64_t target_id,
    uint32_t extra_param,
    uint32_t use_type,
    int32_t pvp,
    game::Vector3* target_location
) {
    if (self != nullptr) {
        s_action_manager = self;
    }

    if (s_mitigator != nullptr) {
        s_mitigator->record_action_request(action_id, 0);
    }

    return fp_original_use_action_location(
        self, action_type, action_id, target_id, extra_param, use_type, pvp, target_location
    );
}

void FFXIV_FASTCALL DetourReceiveActionEffect(
    uint32_t source_id,
    void* source_character,
    game::Vector3* pos,
    game::ActionEffectHeader* effect_header,
    void* effect_data,
    void* targets
) {
    MitigationResult result{};
    bool should_mitigate = false;

    if (effect_header != nullptr && s_mitigator != nullptr) {
        const double original_lock_ms = static_cast<double>(effect_header->animation_lock_time) * 1000.0;
        result = s_mitigator->calculate_mitigation(
            effect_header->action_id,
            effect_header->global_sequence,
            original_lock_ms
        );
        should_mitigate = true;
    }

    // Call the original game function so it processes effects and sets normal animationLock
    fp_original_receive_action_effect(
        source_id, source_character, pos, effect_header, effect_data, targets
    );

    // If mitigation should be applied, adjust ActionManager->animation_lock immediately
    if (should_mitigate && result.applied && s_action_manager != nullptr) {
        const float new_lock_seconds = static_cast<float>(result.adjusted_lock_ms / 1000.0);
        s_action_manager->animation_lock = new_lock_seconds;
    }

    // Transmit telemetry to console via IPC
    if (should_mitigate && s_ipc != nullptr && s_ipc->is_connected()) {
        ipc::TelemetryPayload payload{};
        payload.action_id = result.action_id;
        payload.sequence = result.sequence;
        payload.original_lock_ms = static_cast<float>(result.original_lock_ms);
        payload.adjusted_lock_ms = static_cast<float>(result.adjusted_lock_ms);
        payload.delay_reduced_ms = static_cast<float>(result.delay_reduced_ms);
        payload.measured_rtt_ms = static_cast<float>(result.measured_rtt_ms);
        payload.smoothed_rtt_ms = static_cast<float>(result.smoothed_rtt_ms);
        payload.clamped_floor = result.clamped_by_floor ? 1 : 0;
        payload.dry_run = s_mitigator->get_config().dry_run ? 1 : 0;
        payload.applied = result.applied ? 1 : 0;
        payload.cast_active = result.cast_active ? 1 : 0;
        payload.timestamp_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
        s_ipc->send_telemetry(payload);
    }
}

void FFXIV_FASTCALL DetourCastBegin(
    game::ActionManager* self,
    uint32_t action_type,
    uint32_t action_id,
    float cast_time,
    float current_cast_time
) {
    if (self != nullptr) {
        s_action_manager = self;
    }
    if (s_mitigator != nullptr) {
        s_mitigator->record_cast_begin(action_id, cast_time);
    }
    fp_original_cast_begin(self, action_type, action_id, cast_time, current_cast_time);
}

void FFXIV_FASTCALL DetourCastInterrupt(game::ActionManager* self) {
    if (self != nullptr) {
        s_action_manager = self;
    }
    if (s_mitigator != nullptr) {
        s_mitigator->record_cast_interrupt();
    }
    fp_original_cast_interrupt(self);
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

    s_mitigator = mitigator;
    s_ipc = ipc;

    if (MH_Initialize() != MH_OK) {
        return false;
    }

    uint32_t hooked = 0;

    // 1. Hook UseActionLocation
    const auto sig_use_action = memory::Signature::parse("48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B F9 41 8B F1");
    uintptr_t addr_use_action = memory::scan_module_section(nullptr, sig_use_action);
    if (addr_use_action == 0) {
        // Fallback pattern
        const auto sig_fallback = memory::Signature::parse("40 53 55 57 41 54 41 57 48 83 EC 60");
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
    const auto sig_recv_effect = memory::Signature::parse("40 55 56 57 41 54 41 55 41 56 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05");
    uintptr_t addr_recv_effect = memory::scan_module_section(nullptr, sig_recv_effect);
    if (addr_recv_effect == 0) {
        const auto sig_fallback = memory::Signature::parse("48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24");
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
    const auto sig_cast_begin = memory::Signature::parse("40 53 48 83 EC ? 48 8B D9 89 91 ? ? ? ? 89 91");
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
    const auto sig_cast_interrupt = memory::Signature::parse("48 83 EC ? 48 8B 01 BA ? ? ? ? FF 50");
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

    if (hooked > 0) {
        MH_EnableHook(MH_ALL_HOOKS);
        m_hook_count = hooked;
        m_installed = true;
        return true;
    }

    MH_Uninitialize();
    return false;
#else
    (void)mitigator;
    (void)ipc;
    return false;
#endif
}

void HookManager::uninstall() {
#if defined(_WIN32)
    if (!m_installed.load()) {
        return;
    }

    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();

    fp_original_use_action_location = nullptr;
    fp_original_receive_action_effect = nullptr;
    fp_original_cast_begin = nullptr;
    fp_original_cast_interrupt = nullptr;
    s_action_manager = nullptr;
    s_mitigator = nullptr;
    s_ipc = nullptr;

    m_hook_count = 0;
    m_installed = false;
#endif
}

} // namespace mitigator::payload
