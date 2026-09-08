#pragma once

#include <cstdint>
#include <cstddef>

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
/// In FFXIV dx11, ActionManager holds the current animation lock timer
struct ActionManager {
    void* vtable{nullptr};                   // 0x00
    float animation_lock{0.0f};              // 0x08: Current active animation lock in seconds
    uint8_t pad_0c[0x18]{0};                 // 0x0C - 0x24
    uint32_t cast_action_type{0};            // 0x24
    uint32_t cast_action_id{0};              // 0x28
    float current_cast_time{0.0f};           // 0x2C
    float max_cast_time{0.0f};               // 0x30
    uint32_t combo_action_id{0};             // 0x34
    float combo_timer{0.0f};                 // 0x38
    bool is_casting{false};                  // 0x3C
};

#pragma pack(pop)

/// Known default byte offsets within ActionManager (in case struct layout shifts across patches)
namespace offsets {
    constexpr size_t ACTION_MANAGER_ANIMATION_LOCK = 0x08;
    constexpr size_t ACTION_MANAGER_IS_CASTING = 0x3C;
    constexpr size_t ACTION_MANAGER_CAST_ACTION_ID = 0x28;
    constexpr size_t ACTION_MANAGER_CURRENT_CAST_TIME = 0x2C;
}

} // namespace mitigator::game
