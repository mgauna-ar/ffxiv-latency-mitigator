#pragma once

#include <cstdint>
#include <cstddef>
#include <string_view>
#include <vector>
#include <optional>

namespace mitigator::memory {

/**
 * @brief Represents a compiled byte pattern with mask for fast signature scanning.
 */
struct Signature {
    std::vector<uint8_t> bytes;
    std::vector<bool> mask; // true = match exact byte, false = wildcard (?)

    /// Constructs a Signature from an IDA-style string (e.g. "48 89 5C 24 ? 48 89 6C")
    static Signature parse(std::string_view pattern);

    [[nodiscard]] bool empty() const { return bytes.empty(); }
    [[nodiscard]] size_t size() const { return bytes.size(); }
};

/**
 * @brief Scans a memory range for the first occurrence of a Signature.
 * @param base Starting address of the memory buffer.
 * @param size Size of the buffer in bytes.
 * @param sig The compiled Signature to find.
 * @return Pointer to the match, or nullptr if not found.
 */
[[nodiscard]] const uint8_t* find_pattern(
    const uint8_t* base,
    size_t size,
    const Signature& sig
);

/**
 * @brief Scans a memory range using an IDA-style pattern string.
 */
[[nodiscard]] const uint8_t* find_pattern(
    const uint8_t* base,
    size_t size,
    std::string_view pattern
);

/**
 * @brief Resolves a 32-bit RIP-relative displacement (e.g. `lea rcx, [rip + 0x1234]`).
 * @param instruction_addr Address of the instruction.
 * @param disp_offset Offset of the 4-byte displacement within the instruction.
 * @param instruction_size Total size of the instruction in bytes.
 * @return Absolute target address.
 */
[[nodiscard]] uintptr_t resolve_rip_relative(
    uintptr_t instruction_addr,
    size_t disp_offset,
    size_t instruction_size
);

/**
 * @brief Resolves a 5-byte relative CALL or JMP instruction (E8/E9 xx xx xx xx).
 */
[[nodiscard]] uintptr_t resolve_call_relative(uintptr_t call_addr);

} // namespace mitigator::memory
