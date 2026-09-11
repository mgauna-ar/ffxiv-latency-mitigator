#include "mitigator/config_manager.hpp"
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>

namespace mitigator {

MitigationConfig ConfigManager::default_config() noexcept {
    return MitigationConfig{};
}

MitigationConfig ConfigManager::clamp_and_validate(const MitigationConfig& config) noexcept {
    MitigationConfig validated = config;

    // Safety floor anti-cheat constraint: never below ABSOLUTE_MIN_ANIMATION_LOCK_FLOOR_MS (20.0ms)
    validated.min_animation_lock_ms = std::clamp(
        validated.min_animation_lock_ms,
        constants::ABSOLUTE_MIN_ANIMATION_LOCK_FLOOR_MS,
        150.0
    );

    // Target ping: plausible 0ms to 250ms
    validated.target_ping_ms = std::clamp(validated.target_ping_ms, 0.0, 250.0);

    // Max animation lock ceiling
    validated.max_animation_lock_ms = std::clamp(validated.max_animation_lock_ms, 500.0, 5000.0);

    // Safety margin
    validated.safety_margin_ms = std::clamp(validated.safety_margin_ms, 0.0, 100.0);

    // Sample window
    validated.rtt_sample_window = std::clamp<size_t>(validated.rtt_sample_window, 3, 100);

    return validated;
}

std::string ConfigManager::serialize_json(const MitigationConfig& config) {
    auto c = clamp_and_validate(config);
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(2);
    ss << "{\n";
    ss << "  \"target_ping_ms\": " << c.target_ping_ms << ",\n";
    ss << "  \"min_animation_lock_ms\": " << c.min_animation_lock_ms << ",\n";
    ss << "  \"max_animation_lock_ms\": " << c.max_animation_lock_ms << ",\n";
    ss << "  \"rtt_sample_window\": " << c.rtt_sample_window << ",\n";
    ss << "  \"safety_margin_ms\": " << c.safety_margin_ms << ",\n";
    ss << "  \"dry_run\": " << (c.dry_run ? "true" : "false") << ",\n";
    ss << "  \"verbose\": " << (c.verbose ? "true" : "false") << "\n";
    ss << "}\n";
    return ss.str();
}

namespace {

std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool extract_value(const std::string& json, const std::string& key, std::string& out_val) {
    std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return false;

    size_t colon = json.find(':', pos + needle.size());
    if (colon == std::string::npos) return false;

    size_t start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t' || json[start] == '\r' || json[start] == '\n')) {
        ++start;
    }
    if (start >= json.size()) return false;

    size_t end = json.find_first_of(",}\r\n", start);
    if (end == std::string::npos) end = json.size();

    out_val = trim(json.substr(start, end - start));
    // Remove trailing comma if captured
    if (!out_val.empty() && out_val.back() == ',') {
        out_val.pop_back();
        out_val = trim(out_val);
    }
    return !out_val.empty();
}

} // namespace

MitigationConfig ConfigManager::deserialize_json(const std::string& json_str) {
    MitigationConfig config = default_config();
    std::string val;

    if (extract_value(json_str, "target_ping_ms", val)) {
        try { config.target_ping_ms = std::stod(val); } catch (...) {}
    }
    if (extract_value(json_str, "min_animation_lock_ms", val)) {
        try { config.min_animation_lock_ms = std::stod(val); } catch (...) {}
    }
    if (extract_value(json_str, "max_animation_lock_ms", val)) {
        try { config.max_animation_lock_ms = std::stod(val); } catch (...) {}
    }
    if (extract_value(json_str, "rtt_sample_window", val)) {
        try { config.rtt_sample_window = static_cast<size_t>(std::stoul(val)); } catch (...) {}
    }
    if (extract_value(json_str, "safety_margin_ms", val)) {
        try { config.safety_margin_ms = std::stod(val); } catch (...) {}
    }
    if (extract_value(json_str, "dry_run", val)) {
        config.dry_run = (val == "true" || val == "1");
    }
    if (extract_value(json_str, "verbose", val)) {
        config.verbose = (val == "true" || val == "1");
    }

    return clamp_and_validate(config);
}

MitigationConfig ConfigManager::load_from_file(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        return default_config();
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return deserialize_json(buffer.str());
}

bool ConfigManager::save_to_file(const std::string& filepath, const MitigationConfig& config) {
    std::ofstream file(filepath, std::ios::trunc);
    if (!file.is_open()) {
        return false;
    }
    file << serialize_json(config);
    return file.good();
}

} // namespace mitigator
