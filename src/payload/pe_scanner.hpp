#pragma once

#include "mitigator/sigscan.hpp"
#include <cstdint>

namespace mitigator::memory {

/**
 * @brief Scans the specified PE section (default .text) of a loaded Windows module.
 * Protected by Win32 Structured Exception Handling (SEH).
 *
 * @param module_handle Handle to the module (nullptr for host exe).
 * @param sig Pre-parsed Signature to find.
 * @param section_name Name of the PE section to scan (e.g. ".text").
 * @return Absolute virtual address of match, or 0 if not found / access violation.
 */
[[nodiscard]] uintptr_t scan_module_section(
    void* module_handle,
    const Signature& sig,
    const char* section_name = ".text"
);

} // namespace mitigator::memory
