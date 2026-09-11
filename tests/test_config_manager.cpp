#include "test_framework.hpp"
#include "mitigator/config_manager.hpp"
#include <cstdio>
#include <filesystem>

using namespace mitigator;

TEST_CASE(ConfigManager, DefaultConfigValues) {
    auto config = ConfigManager::default_config();
    TEST_ASSERT_NEAR(config.target_ping_ms, 15.0, 0.001);
    TEST_ASSERT_NEAR(config.min_animation_lock_ms, 25.0, 0.001);
    TEST_ASSERT_NEAR(config.max_animation_lock_ms, 2500.0, 0.001);
    TEST_ASSERT_EQ(config.rtt_sample_window, static_cast<size_t>(10));
    TEST_ASSERT(!config.dry_run);
    TEST_ASSERT(!config.verbose);
    TEST_ASSERT_NEAR(config.safety_margin_ms, 0.0, 0.001);
}

TEST_CASE(ConfigManager, ClampSafetyFloorAntiCheat) {
    MitigationConfig dangerous;
    dangerous.min_animation_lock_ms = 0.0; // Dangerous 0ms floor
    auto clamped = ConfigManager::clamp_and_validate(dangerous);
    TEST_ASSERT(clamped.min_animation_lock_ms >= constants::ABSOLUTE_MIN_ANIMATION_LOCK_FLOOR_MS);
    TEST_ASSERT_NEAR(clamped.min_animation_lock_ms, 20.0, 0.001);

    dangerous.min_animation_lock_ms = -50.0;
    clamped = ConfigManager::clamp_and_validate(dangerous);
    TEST_ASSERT_NEAR(clamped.min_animation_lock_ms, 20.0, 0.001);

    dangerous.target_ping_ms = -10.0;
    clamped = ConfigManager::clamp_and_validate(dangerous);
    TEST_ASSERT_NEAR(clamped.target_ping_ms, 0.0, 0.001);

    dangerous.rtt_sample_window = 1; // Too small
    clamped = ConfigManager::clamp_and_validate(dangerous);
    TEST_ASSERT_EQ(clamped.rtt_sample_window, static_cast<size_t>(3));
}

TEST_CASE(ConfigManager, JsonSerializationDeserializationRoundtrip) {
    MitigationConfig original;
    original.target_ping_ms = 22.5;
    original.min_animation_lock_ms = 35.0;
    original.max_animation_lock_ms = 1800.0;
    original.rtt_sample_window = 15;
    original.safety_margin_ms = 5.0;
    original.dry_run = true;
    original.verbose = true;

    std::string json = ConfigManager::serialize_json(original);
    TEST_ASSERT(json.find("\"target_ping_ms\": 22.50") != std::string::npos);
    TEST_ASSERT(json.find("\"dry_run\": true") != std::string::npos);
    TEST_ASSERT(json.find("\"verbose\": true") != std::string::npos);

    MitigationConfig loaded = ConfigManager::deserialize_json(json);
    TEST_ASSERT_NEAR(loaded.target_ping_ms, 22.5, 0.01);
    TEST_ASSERT_NEAR(loaded.min_animation_lock_ms, 35.0, 0.01);
    TEST_ASSERT_NEAR(loaded.max_animation_lock_ms, 1800.0, 0.01);
    TEST_ASSERT_EQ(loaded.rtt_sample_window, static_cast<size_t>(15));
    TEST_ASSERT_NEAR(loaded.safety_margin_ms, 5.0, 0.01);
    TEST_ASSERT(loaded.dry_run);
    TEST_ASSERT(loaded.verbose);
}

TEST_CASE(ConfigManager, DeserializationMalformedJsonFallsBackSafely) {
    std::string bad_json = "{ invalid json content: 123 }";
    auto config = ConfigManager::deserialize_json(bad_json);
    TEST_ASSERT_NEAR(config.target_ping_ms, 15.0, 0.001);
    TEST_ASSERT_NEAR(config.min_animation_lock_ms, 25.0, 0.001);
    TEST_ASSERT(!config.dry_run);
}

TEST_CASE(ConfigManager, FilePersistenceRoundtrip) {
    std::string temp_file = "test_mitigator_config_temp.json";

    MitigationConfig cfg;
    cfg.target_ping_ms = 18.0;
    cfg.min_animation_lock_ms = 30.0;
    cfg.dry_run = true;

    TEST_ASSERT(ConfigManager::save_to_file(temp_file, cfg));

    auto loaded = ConfigManager::load_from_file(temp_file);
    TEST_ASSERT_NEAR(loaded.target_ping_ms, 18.0, 0.01);
    TEST_ASSERT_NEAR(loaded.min_animation_lock_ms, 30.0, 0.01);
    TEST_ASSERT(loaded.dry_run);

    std::remove(temp_file.c_str());
}

TEST_CASE(ConfigManager, LoadFromNonExistentFileReturnsDefault) {
    auto config = ConfigManager::load_from_file("non_existent_file_xyz_123.json");
    TEST_ASSERT_NEAR(config.target_ping_ms, 15.0, 0.001);
    TEST_ASSERT_NEAR(config.min_animation_lock_ms, 25.0, 0.001);
}
