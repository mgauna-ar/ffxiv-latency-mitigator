#pragma once

#include "mitigator/types.hpp"
#include <string>

namespace mitigator {

/// Manages persistence, validation, and JSON serialization for MitigationConfig.
/// Pure C++20, zero external library dependencies.
class ConfigManager {
public:
    /// Default configuration filename placed alongside the application binary
    static constexpr const char* DEFAULT_CONFIG_FILENAME = "mitigator_config.json";

    /// Returns default safe configuration parameters
    [[nodiscard]] static MitigationConfig default_config() noexcept;

    /// Validates and clamps configuration values against safety invariants
    [[nodiscard]] static MitigationConfig clamp_and_validate(const MitigationConfig& config) noexcept;

    /// Serializes a MitigationConfig into a formatted JSON string
    [[nodiscard]] static std::string serialize_json(const MitigationConfig& config);

    /// Deserializes a JSON string into a MitigationConfig, falling back to defaults for missing keys
    [[nodiscard]] static MitigationConfig deserialize_json(const std::string& json_str);

    /// Loads configuration from file at the given path.
    /// Returns default_config() if file does not exist or parsing fails.
    [[nodiscard]] static MitigationConfig load_from_file(const std::string& filepath);

    /// Saves configuration to file at the given path. Returns true on success.
    static bool save_to_file(const std::string& filepath, const MitigationConfig& config);
};

} // namespace mitigator
