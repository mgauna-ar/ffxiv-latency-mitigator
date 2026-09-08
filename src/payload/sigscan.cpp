#include "mitigator/sigscan.hpp"
#include <cstring>
#include <sstream>
#include <iomanip>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace mitigator::memory {

Signature Signature::parse(std::string_view pattern) {
    Signature sig;
    size_t i = 0;
    const size_t len = pattern.length();

    while (i < len) {
        // Skip whitespace
        while (i < len && (pattern[i] == ' ' || pattern[i] == '\t')) {
            ++i;
        }
        if (i >= len) break;

        if (pattern[i] == '?') {
            sig.bytes.push_back(0);
            sig.mask.push_back(false);
            ++i;
            if (i < len && pattern[i] == '?') {
                ++i;
            }
        } else if (i + 1 < len) {
            char hex[3] = { pattern[i], pattern[i + 1], '\0' };
            char* end = nullptr;
            const auto val = static_cast<uint8_t>(std::strtoul(hex, &end, 16));
            if (end != hex) {
                sig.bytes.push_back(val);
                sig.mask.push_back(true);
                i += 2;
            } else {
                ++i; // Invalid character, skip
            }
        } else {
            ++i;
        }
    }

    return sig;
}

const uint8_t* find_pattern(
    const uint8_t* base,
    size_t size,
    const Signature& sig
) {
    if (!base || size == 0 || sig.empty() || sig.size() > size) {
        return nullptr;
    }

    const size_t sig_len = sig.size();
    const size_t scan_len = size - sig_len;

    const uint8_t first_byte = sig.bytes[0];
    const bool first_masked = sig.mask[0];

    for (size_t i = 0; i <= scan_len; ++i) {
        // Quick check first byte if it is not wildcard
        if (first_masked && base[i] != first_byte) {
            continue;
        }

        bool match = true;
        for (size_t j = 1; j < sig_len; ++j) {
            if (sig.mask[j] && base[i + j] != sig.bytes[j]) {
                match = false;
                break;
            }
        }

        if (match) {
            return base + i;
        }
    }

    return nullptr;
}

const uint8_t* find_pattern(
    const uint8_t* base,
    size_t size,
    std::string_view pattern
) {
    const auto sig = Signature::parse(pattern);
    return find_pattern(base, size, sig);
}

#if defined(_WIN32)
#if defined(_MSC_VER) || (defined(_WIN32) && defined(__clang__))
#define MITIGATOR_SEH_TRY __try
#define MITIGATOR_SEH_EXCEPT __except (EXCEPTION_EXECUTE_HANDLER)
#else
#define MITIGATOR_SEH_TRY if (true)
#define MITIGATOR_SEH_EXCEPT else
#endif
#endif

uintptr_t resolve_rip_relative(
    uintptr_t instruction_addr,
    size_t disp_offset,
    size_t instruction_size
) {
    if (instruction_addr == 0) return 0;
#if defined(_WIN32)
    MITIGATOR_SEH_TRY {
        int32_t disp = 0;
        std::memcpy(&disp, reinterpret_cast<const void*>(instruction_addr + disp_offset), sizeof(int32_t));
        const uintptr_t rip = instruction_addr + instruction_size;
        return rip + static_cast<intptr_t>(disp);
    }
    MITIGATOR_SEH_EXCEPT {
        return 0;
    }
#else
    int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(instruction_addr + disp_offset), sizeof(int32_t));
    const uintptr_t rip = instruction_addr + instruction_size;
    return rip + static_cast<intptr_t>(disp);
#endif
}

namespace {
    // x86-64 CALL rel32 instruction layout: opcode (1 byte: 0xE8) + displacement (4 bytes int32)
    constexpr size_t CALL_REL32_DISP_OFFSET = 1;
    constexpr size_t CALL_REL32_INSN_SIZE = 5;
}

uintptr_t resolve_call_relative(uintptr_t call_addr) {
    if (call_addr == 0) return 0;
    return resolve_rip_relative(call_addr, CALL_REL32_DISP_OFFSET, CALL_REL32_INSN_SIZE);
}

#if defined(_WIN32)

namespace {
    // Standard PE section header name length (fixed 8 bytes, not null-terminated if 8 chars)
    constexpr size_t PE_SECTION_NAME_MAX_LEN = 8;
}

uintptr_t scan_module_section(
    void* module_handle,
    const Signature& sig,
    const char* section_name
) {
    MITIGATOR_SEH_TRY {
        auto h_mod = static_cast<HMODULE>(module_handle);
        if (!h_mod) {
            h_mod = GetModuleHandleW(nullptr);
        }
        if (!h_mod) return 0;

        auto dos_header = reinterpret_cast<PIMAGE_DOS_HEADER>(h_mod);
        if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) return 0;

        auto nt_headers = reinterpret_cast<PIMAGE_NT_HEADERS>(
            reinterpret_cast<uintptr_t>(h_mod) + dos_header->e_lfanew
        );
        if (nt_headers->Signature != IMAGE_NT_SIGNATURE) return 0;

        auto section = IMAGE_FIRST_SECTION(nt_headers);
        for (WORD i = 0; i < nt_headers->FileHeader.NumberOfSections; ++i, ++section) {
            char name[PE_SECTION_NAME_MAX_LEN + 1] = {0};
            std::memcpy(name, section->Name, PE_SECTION_NAME_MAX_LEN);

            if (std::strcmp(name, section_name) == 0) {
                const auto sec_base = reinterpret_cast<const uint8_t*>(
                    reinterpret_cast<uintptr_t>(h_mod) + section->VirtualAddress
                );
                const size_t sec_size = section->Misc.VirtualSize;
                const uint8_t* result = find_pattern(sec_base, sec_size, sig);
                return result ? reinterpret_cast<uintptr_t>(result) : 0;
            }
        }

        return 0;
    }
    MITIGATOR_SEH_EXCEPT {
        return 0;
    }
}
#endif

} // namespace mitigator::memory
