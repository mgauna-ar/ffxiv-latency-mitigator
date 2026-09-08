#pragma once

#include <cstdint>
#include <cstddef>
#include <span>

#if __has_include("embedded_payload_generated.hpp")
#include "embedded_payload_generated.hpp"
#else
namespace mitigator::loader {
inline const uint8_t g_embedded_payload_dll[] = { 0 };
inline const size_t g_embedded_payload_dll_size = 0;
} // namespace mitigator::loader
#endif

namespace mitigator::loader {

inline std::span<const uint8_t> get_embedded_payload() {
    return std::span<const uint8_t>(g_embedded_payload_dll, g_embedded_payload_dll_size);
}

} // namespace mitigator::loader
