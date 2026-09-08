#pragma once

#include "mitigator/game_definitions.hpp"

namespace mitigator::game {

#pragma pack(push, 1)

struct Vector3 {
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
};

/// Header for incoming action effect packets in FFXIV dx11 client
struct ActionEffectHeader {
    uint32_t animation_target_id{0};
    uint32_t action_id{0};
    uint32_t global_sequence{0};
    float animation_lock_time{0.0f};
    uint32_t some_target_id{0};
    uint16_t hidden_animation{0};
    uint16_t rotation{0};
    uint16_t action_animation_id{0};
    uint8_t variation{0};
    uint8_t effect_display_type{0};
    uint8_t effect_count{0};
    uint8_t padding{0};
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

} // namespace mitigator::game
