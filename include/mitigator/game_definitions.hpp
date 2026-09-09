#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>

namespace mitigator::game {

/// Target game version and executable metadata
namespace definitions {
    /// Default image name of the 64-bit DirectX 11 game process
    constexpr std::string_view DEFAULT_GAME_PROCESS_NAME = "ffxiv_dx11.exe";

    /// Current game client release supported by these signatures and offsets
    constexpr std::string_view SUPPORTED_GAME_VERSION = "7.x (Dawntrail)";

    /// Instruction displacement and length for RIP-relative LEA/MOV rcx, [rip + disp32] (48 8D 0D [disp32])
    constexpr size_t ACTION_MGR_RIP_DISP_OFFSET = 3;
    constexpr size_t ACTION_MGR_RIP_INSN_LEN = 7;

    /// Total number of detours managed by HookManager (UseActionLocation, ReceiveActionEffect, CastBegin, CastInterrupt)
    constexpr uint32_t TOTAL_AVAILABLE_HOOKS = 4;

    /// Minimum number of primary hooks required to perform latency mitigation
    constexpr uint32_t MIN_REQUIRED_PRIMARY_HOOKS = 4;

    /// Minimum animation lock threshold in seconds for local player action effect detection
    constexpr float MIN_ACTION_EFFECT_LOCK_SECONDS = 0.01f;
} // namespace definitions

/// Memory offsets within the game's ActionManager structure (FFXIV dx11 x64)
namespace offsets {
    /// Active animation lock timer in seconds (float)
    constexpr size_t ACTION_MANAGER_ANIMATION_LOCK = 0x08;

    /// Active spell or channeled cast flag (bool)
    constexpr size_t ACTION_MANAGER_IS_CASTING = 0x28;

    /// Elapsed cast time in seconds (float)
    constexpr size_t ACTION_MANAGER_ELAPSED_CAST_TIME = 0x30;

    /// Total cast duration in seconds (float)
    constexpr size_t ACTION_MANAGER_CAST_TIME = 0x34;

    /// Remaining combo expiration timer in seconds (float)
    constexpr size_t ACTION_MANAGER_COMBO_TIME = 0x60;

    /// Client-side queued action flag (bool)
    constexpr size_t ACTION_MANAGER_IS_QUEUED = 0x68;

    /// Rolling client action sequence counter (uint16_t)
    constexpr size_t ACTION_MANAGER_CURRENT_SEQUENCE = 0x120;
} // namespace offsets

/// IDA-style AOB (Array of Bytes) pattern signatures for memory scanning
namespace signatures {
    // 1. Client action dispatch function: ActionManager::UseActionLocation
    // Dawntrail 7.x (FFXIVClientStructs call-site): E8 ?? ?? ?? ?? 48 8B BC 24 ?? ?? ?? ?? 44 0F B6 F8 B0
    constexpr std::string_view USE_ACTION_LOCATION_PRIMARY =
        "E8 ? ? ? ? 48 8B BC 24 ? ? ? ? 44 0F B6 F8 B0";
    // Dawntrail 7.0 - 7.1 call-site fallback:
    constexpr std::string_view USE_ACTION_LOCATION_FALLBACK =
        "E8 ? ? ? ? 40 3A C7 0F 85 ? ? ? ?";
    // Legacy function prologue fallback:
    constexpr std::string_view USE_ACTION_LOCATION_LEGACY =
        "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B F9 41 8B F1";

    // 2. Zone action effect processing: ActionEffectHandler::ReceiveActionEffect
    // Dawntrail 7.x (FFXIVClientStructs call-site): E8 ?? ?? ?? ?? 48 8B 8D ?? ?? ?? ?? 48 33 CC E8 ?? ?? ?? ?? 48 81 C4 00 05 00 00
    constexpr std::string_view RECEIVE_ACTION_EFFECT_PRIMARY =
        "E8 ? ? ? ? 48 8B 8D ? ? ? ? 48 33 CC E8 ? ? ? ? 48 81 C4 00 05 00 00";
    // Dawntrail 7.0 - 7.1 function prologue fallback:
    constexpr std::string_view RECEIVE_ACTION_EFFECT_FALLBACK =
        "40 55 56 57 41 54 41 55 41 56 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05";
    // Legacy prologue fallback:
    constexpr std::string_view RECEIVE_ACTION_EFFECT_LEGACY =
        "48 89 5C 24 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24";

    // 3. Spell cast begin: ActionManager::CastBegin (reference / test verification)
    constexpr std::string_view CAST_BEGIN_PRIMARY =
        "40 53 57 48 81 EC ? ? ? ? 48 8B FA 8B D1";
    constexpr std::string_view CAST_BEGIN_FALLBACK =
        "40 53 48 83 EC ? 48 8B D9 89 91 ? ? ? ? 89 91";

    // 4. Spell cast interrupt/cancel: ActionManager::CastInterrupt (reference / test verification)
    constexpr std::string_view CAST_INTERRUPT_PRIMARY =
        "48 83 EC ? 48 8B 01 BA ? ? ? ? FF 50";
    constexpr std::string_view CAST_INTERRUPT_FALLBACK =
        "48 8B C4 48 83 EC 48 48 89 58 08";

    // 5. ActionManager static instance resolution instruction
    // Dawntrail 7.x (FFXIVClientStructs): LEA rcx, [rip + disp32] followed by movss xmm2, [rbx]
    constexpr std::string_view ACTION_MANAGER_INSTANCE_PRIMARY =
        "48 8D 0D ? ? ? ? F3 0F 10 13";
    // Dawntrail fallback:
    constexpr std::string_view ACTION_MANAGER_INSTANCE_FALLBACK =
        "48 8D 0D ? ? ? ? 88";
    // Legacy Endwalker 6.x: MOV rcx, [rip + disp32]
    constexpr std::string_view ACTION_MANAGER_INSTANCE_LEGACY =
        "48 8B 0D ? ? ? ? 48 85 C9 74 ? 48 8B 01 FF 50 ? 48 85 C0";
} // namespace signatures

} // namespace mitigator::game
