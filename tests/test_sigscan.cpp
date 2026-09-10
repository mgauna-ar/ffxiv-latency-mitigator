#include "test_framework.hpp"
#include "mitigator/sigscan.hpp"
#include "mitigator/game_definitions.hpp"
#include "mitigator/game_structures.hpp"
#include <vector>
#include <limits>

TEST_CASE(SigScan, PatternParsing) {
    const auto sig = mitigator::memory::Signature::parse("48 89 5C 24 ? 48 89 6C");
    TEST_ASSERT_EQ(sig.size(), 8);
    TEST_ASSERT(sig.mask[0]);
    TEST_ASSERT_EQ(sig.bytes[0], 0x48);
    TEST_ASSERT(sig.mask[1]);
    TEST_ASSERT_EQ(sig.bytes[1], 0x89);
    TEST_ASSERT(!sig.mask[4]); // Wildcard '?'
    TEST_ASSERT(sig.mask[7]);
    TEST_ASSERT_EQ(sig.bytes[7], 0x6C);
}

TEST_CASE(SigScan, FindPatternExact) {
    std::vector<uint8_t> buffer = { 0x90, 0x90, 0x48, 0x89, 0x5C, 0x24, 0x20, 0xCC };
    const auto match = mitigator::memory::find_pattern(buffer.data(), buffer.size(), "48 89 5C 24 20");
    TEST_ASSERT(match != nullptr);
    TEST_ASSERT_EQ(match - buffer.data(), 2);
}

TEST_CASE(SigScan, FindPatternWildcard) {
    std::vector<uint8_t> buffer = { 0xAA, 0xBB, 0x48, 0x89, 0xFF, 0x55, 0xEE };
    // Wildcard '?' matches 0xFF
    const auto match = mitigator::memory::find_pattern(buffer.data(), buffer.size(), "48 89 ? 55");
    TEST_ASSERT(match != nullptr);
    TEST_ASSERT_EQ(match - buffer.data(), 2);
}

TEST_CASE(SigScan, PatternNotFound) {
    std::vector<uint8_t> buffer = { 0x11, 0x22, 0x33, 0x44 };
    const auto match = mitigator::memory::find_pattern(buffer.data(), buffer.size(), "99 88 77");
    TEST_ASSERT(match == nullptr);
}

TEST_CASE(SigScan, ResolveRelativeCall) {
    // E8 <disp32>: CALL rel32
    // If instruction is at address 0x1000, size is 5 (next instruction RIP = 0x1005)
    // Target is 0x1050 -> disp32 = 0x1050 - 0x1005 = 0x4B
    uint8_t instruction[5] = { 0xE8, 0x4B, 0x00, 0x00, 0x00 };
    const uintptr_t base_addr = reinterpret_cast<uintptr_t>(instruction);
    const uintptr_t target = mitigator::memory::resolve_call_relative(base_addr);

    TEST_ASSERT_EQ(target, base_addr + 5 + 0x4B);
}

TEST_CASE(SigScan, ResolveRipRelativeNegativeDisplacement) {
    // 48 8B 0D <disp32>: MOV rcx, [rip + disp32] (7 bytes)
    // Negative disp32 = -0x10 = 0xFFFFFFF0
    uint8_t instruction[7] = { 0x48, 0x8B, 0x0D, 0xF0, 0xFF, 0xFF, 0xFF };
    const uintptr_t base_addr = reinterpret_cast<uintptr_t>(instruction);
    const uintptr_t target = mitigator::memory::resolve_rip_relative(base_addr, 3, 7);

    TEST_ASSERT_EQ(target, base_addr + 7 - 0x10);
}

TEST_CASE(SigScan, ResolveRipRelativeNullAddr) {
    const uintptr_t target = mitigator::memory::resolve_rip_relative(0, 3, 7);
    TEST_ASSERT_EQ(target, 0);

    const uintptr_t call_target = mitigator::memory::resolve_call_relative(0);
    TEST_ASSERT_EQ(call_target, 0);
}

TEST_CASE(SigScan, GameDefinitionsSignaturesParseSuccessfully) {
    using namespace mitigator::game;

    const auto sig_use_primary = mitigator::memory::Signature::parse(signatures::USE_ACTION_LOCATION_PRIMARY);
    TEST_ASSERT(!sig_use_primary.empty());
    TEST_ASSERT_EQ(sig_use_primary.bytes.size(), sig_use_primary.mask.size());

    const auto sig_use_fallback = mitigator::memory::Signature::parse(signatures::USE_ACTION_LOCATION_FALLBACK);
    TEST_ASSERT(!sig_use_fallback.empty());

    const auto sig_recv_primary = mitigator::memory::Signature::parse(signatures::RECEIVE_ACTION_EFFECT_PRIMARY);
    TEST_ASSERT(!sig_recv_primary.empty());

    const auto sig_recv_fallback = mitigator::memory::Signature::parse(signatures::RECEIVE_ACTION_EFFECT_FALLBACK);
    TEST_ASSERT(!sig_recv_fallback.empty());

    const auto sig_action_mgr = mitigator::memory::Signature::parse(signatures::ACTION_MANAGER_INSTANCE_PRIMARY);
    TEST_ASSERT(!sig_action_mgr.empty());

    const auto sig_action_mgr_fb = mitigator::memory::Signature::parse(signatures::ACTION_MANAGER_INSTANCE_FALLBACK);
    TEST_ASSERT(!sig_action_mgr_fb.empty());

    // Compile-time validation of definitions invariants
    static_assert(definitions::TOTAL_AVAILABLE_HOOKS == 2);
    static_assert(definitions::MIN_REQUIRED_PRIMARY_HOOKS == 2);
    static_assert(definitions::MIN_ACTION_EFFECT_LOCK_SECONDS > 0.0f);
    static_assert(definitions::MAX_ACTION_EFFECT_LOCK_SECONDS >= 5.0f);
    static_assert(definitions::MAX_ACTION_EFFECT_TARGETS == 32);
}

TEST_CASE(GameStructures, ActionEffectHeaderValidation) {
    using namespace mitigator::game;

    // 1. Null pointer rejection
    TEST_ASSERT(!is_valid_action_effect_header(nullptr));

    // 2. Normal valid header
    ActionEffectHeader valid_header{};
    valid_header.action_id = 1234;
    valid_header.source_sequence = 42;
    valid_header.animation_lock = 0.5f;
    valid_header.num_targets = 1;
    TEST_ASSERT(is_valid_action_effect_header(&valid_header));

    // 3. Boundary values: 0.0f lock, exactly max targets (32), exactly max lock (60.0f)
    ActionEffectHeader boundary_header{};
    boundary_header.animation_lock = 0.0f;
    boundary_header.num_targets = definitions::MAX_ACTION_EFFECT_TARGETS;
    TEST_ASSERT(is_valid_action_effect_header(&boundary_header));

    boundary_header.animation_lock = definitions::MAX_ACTION_EFFECT_LOCK_SECONDS;
    TEST_ASSERT(is_valid_action_effect_header(&boundary_header));

    // 4. Negative animation lock rejection
    ActionEffectHeader negative_lock = valid_header;
    negative_lock.animation_lock = -0.001f;
    TEST_ASSERT(!is_valid_action_effect_header(&negative_lock));

    // 5. Exceeded animation lock ceiling rejection
    ActionEffectHeader excessive_lock = valid_header;
    excessive_lock.animation_lock = 60.001f;
    TEST_ASSERT(!is_valid_action_effect_header(&excessive_lock));

    // 6. Non-finite animation lock rejection (NaN, +Inf, -Inf)
    ActionEffectHeader nan_lock = valid_header;
    nan_lock.animation_lock = std::numeric_limits<float>::quiet_NaN();
    TEST_ASSERT(!is_valid_action_effect_header(&nan_lock));

    ActionEffectHeader inf_lock = valid_header;
    inf_lock.animation_lock = std::numeric_limits<float>::infinity();
    TEST_ASSERT(!is_valid_action_effect_header(&inf_lock));

    ActionEffectHeader neg_inf_lock = valid_header;
    neg_inf_lock.animation_lock = -std::numeric_limits<float>::infinity();
    TEST_ASSERT(!is_valid_action_effect_header(&neg_inf_lock));

    // 7. Excessive num_targets rejection (> 32)
    ActionEffectHeader too_many_targets = valid_header;
    too_many_targets.num_targets = 33;
    TEST_ASSERT(!is_valid_action_effect_header(&too_many_targets));

    too_many_targets.num_targets = 255;
    TEST_ASSERT(!is_valid_action_effect_header(&too_many_targets));
}

