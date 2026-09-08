#pragma once

#include "mitigator/game_definitions.hpp"

namespace mitigator::game {

#pragma pack(push, 1)

struct Vector3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

/// Header for incoming action effect packets in FFXIV dx11 client (0x28 bytes)
struct ActionEffectHeader {
    uint64_t animation_target_id{0};         // 0x00: Primary target of action (GameObjectId)
    uint32_t action_id{0};                   // 0x08: Action ID
    uint32_t global_sequence{0};             // 0x0C: Unique server global sequence ID
    float animation_lock{0.0f};              // 0x10: Server-assigned animation lock in seconds
    uint32_t ballista_entity_id{0};          // 0x14: Ballista / artillery cannon entity ID
    uint16_t source_sequence{0};             // 0x18: Client-initiated action sequence counter
    uint16_t rotation{0};                    // 0x1A: Quantized rotation (0 -> -pi, 65535 -> pi)
    uint16_t spell_id{0};                    // 0x1C: Spell ID
    uint8_t animation_variation{0};          // 0x1E: Animation variation
    uint8_t action_type{0};                  // 0x1F: Action type
    uint8_t flags{0};                        // 0x20: Flags (bit 0: ShowInLog, bit 1: ForceAnimationLock)
    uint8_t num_targets{0};                  // 0x21: Number of targets affected
    uint8_t pad_22[6]{0};                    // 0x22 - 0x28: Structure padding
};

/// Structure representing the game's ActionManager
/// In FFXIV dx11, ActionManager holds the current animation lock timer and sequence counter
struct ActionManager {
    void* vtable{nullptr};                   // 0x00
    float animation_lock{0.0f};              // 0x08: Current active animation lock in seconds
    uint8_t pad_0c[0x1C]{0};                 // 0x0C - 0x28
    bool is_casting{false};                  // 0x28: Active spell/channel cast flag
    uint8_t pad_29[0x07]{0};                 // 0x29 - 0x30
    float elapsed_cast_time{0.0f};           // 0x30: Elapsed cast time in seconds
    float cast_time{0.0f};                   // 0x34: Total cast duration in seconds
    uint8_t pad_38[0x28]{0};                 // 0x38 - 0x60
    float remaining_combo_time{0.0f};        // 0x60: Combo expiration timer
    uint8_t pad_64[0x04]{0};                 // 0x64 - 0x68
    bool is_queued{false};                   // 0x68: Action queuing flag
    uint8_t pad_69[0xB7]{0};                 // 0x69 - 0x120
    uint16_t current_sequence{0};            // 0x120: Rolling action sequence counter
};

#pragma pack(pop)

// Compile-time verification of ActionManager memory layout against client offsets defined in game_definitions.hpp

// Compile-time verification of ActionManager memory layout against client offsets
static_assert(offsetof(ActionManager, animation_lock) == offsets::ACTION_MANAGER_ANIMATION_LOCK,
    "ActionManager::animation_lock offset mismatch");
static_assert(offsetof(ActionManager, is_casting) == offsets::ACTION_MANAGER_IS_CASTING,
    "ActionManager::is_casting offset mismatch");
static_assert(offsetof(ActionManager, elapsed_cast_time) == offsets::ACTION_MANAGER_ELAPSED_CAST_TIME,
    "ActionManager::elapsed_cast_time offset mismatch");
static_assert(offsetof(ActionManager, cast_time) == offsets::ACTION_MANAGER_CAST_TIME,
    "ActionManager::cast_time offset mismatch");
static_assert(offsetof(ActionManager, remaining_combo_time) == offsets::ACTION_MANAGER_COMBO_TIME,
    "ActionManager::remaining_combo_time offset mismatch");
static_assert(offsetof(ActionManager, is_queued) == offsets::ACTION_MANAGER_IS_QUEUED,
    "ActionManager::is_queued offset mismatch");
static_assert(offsetof(ActionManager, current_sequence) == offsets::ACTION_MANAGER_CURRENT_SEQUENCE,
    "ActionManager::current_sequence offset mismatch");

// Compile-time verification of ActionEffectHeader memory layout
static_assert(sizeof(ActionEffectHeader) == 0x28,
    "ActionEffectHeader size mismatch (must be 0x28 bytes)");
static_assert(offsetof(ActionEffectHeader, animation_target_id) == 0x00,
    "ActionEffectHeader::animation_target_id offset mismatch");
static_assert(offsetof(ActionEffectHeader, action_id) == 0x08,
    "ActionEffectHeader::action_id offset mismatch");
static_assert(offsetof(ActionEffectHeader, global_sequence) == 0x0C,
    "ActionEffectHeader::global_sequence offset mismatch");
static_assert(offsetof(ActionEffectHeader, animation_lock) == 0x10,
    "ActionEffectHeader::animation_lock offset mismatch");
static_assert(offsetof(ActionEffectHeader, source_sequence) == 0x18,
    "ActionEffectHeader::source_sequence offset mismatch");

} // namespace mitigator::game
