#include "payload/pe_scanner.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstring>

namespace mitigator::memory {

namespace {
    // Standard PE section header name length (fixed 8 bytes, not null-terminated if 8 chars)
    constexpr size_t PE_SECTION_NAME_MAX_LEN = 8;
}

uintptr_t scan_module_section(
    void* module_handle,
    const Signature& sig,
    const char* section_name
) {
    __try {
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
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

} // namespace mitigator::memory
