#include "test_framework.hpp"
#include "mitigator/sigscan.hpp"
#include <vector>

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
