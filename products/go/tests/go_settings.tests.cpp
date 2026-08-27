/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_settings.h"

#include <cstddef>
#include <map>
#include <string>
#include <type_traits>
#include <variant>

#include <catch2/catch_test_macros.hpp>

// ============================================================================
// FakeConfigStore — in-memory ConfigStore for round-trip testing
// ============================================================================

class FakeConfigStore : public ConfigStore {
public:
  ConfigStoreResult get_int(const char *key, int &out) override {
    auto it = _ints.find(key);
    if (it == _ints.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult set_int(const char *key, int value) override {
    if (should_fail_write()) {
      return ConfigStoreResult::ERROR;
    }
    _ints[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult get_bool(const char *key, bool &out) override {
    auto it = _bools.find(key);
    if (it == _bools.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult set_bool(const char *key, bool value) override {
    if (should_fail_write()) {
      return ConfigStoreResult::ERROR;
    }
    _bools[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult get_string(const char *key, std::string &out) override {
    auto it = _strings.find(key);
    if (it == _strings.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult set_string(const char *key, const std::string &value) override {
    if (should_fail_write()) {
      return ConfigStoreResult::ERROR;
    }
    _strings[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult get_float(const char *key, float &out) override {
    auto it = _floats.find(key);
    if (it == _floats.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult set_float(const char *key, float value) override {
    if (should_fail_write()) {
      return ConfigStoreResult::ERROR;
    }
    _floats[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult erase(const char *key) override {
    _ints.erase(key);
    _bools.erase(key);
    _strings.erase(key);
    _floats.erase(key);
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult commit() override {
    if (_fail_writes) {
      return ConfigStoreResult::ERROR;
    }
    _committed = true;
    return ConfigStoreResult::OK;
  }

  bool committed() const { return _committed; }
  void set_fail_writes(bool fail) { _fail_writes = fail; }
  void set_fail_write_at(std::size_t write) { _fail_write_at = write; }
  std::size_t write_attempt_count() const { return _write_attempt_count; }
  bool has_int(const char *key) const { return _ints.count(key) > 0; }
  bool has_float(const char *key) const { return _floats.count(key) > 0; }

private:
  bool should_fail_write() {
    ++_write_attempt_count;
    return _fail_writes || (_fail_write_at != 0 && _write_attempt_count == _fail_write_at);
  }

  std::map<std::string, int> _ints;
  std::map<std::string, bool> _bools;
  std::map<std::string, std::string> _strings;
  std::map<std::string, float> _floats;
  bool _committed = false;
  bool _fail_writes = false;
  std::size_t _fail_write_at = 0;
  std::size_t _write_attempt_count = 0;
};

static constexpr std::size_t GO_SETTINGS_WRITE_COUNT = 31;

// ============================================================================
// Defaults — load from empty store returns struct defaults
// ============================================================================

TEST_CASE("load from empty store returns struct defaults", "[settings]") {
  FakeConfigStore store;
  GoSettings s = load_go_settings(store);

  REQUIRE(s.measure_interval_seconds == 10);
  REQUIRE(s.gps_mode == GpsMode::OnWhenTracking);
  REQUIRE(s.operating_mode == OperatingMode::Portable);
  REQUIRE(s.use_fahrenheit == false);
  REQUIRE(s.use_feet == false);
  REQUIRE(s.pm_use_usaqi == false);
  REQUIRE(s.auto_lock_seconds == 10);
  REQUIRE(s.disable_cloud == false);
  REQUIRE(s.configuration_control == ConfigurationControl::Both);
  REQUIRE(s.co2_abc_days == CO2_ABC_DAYS_DEFAULT);
  REQUIRE(s.tvoc_learning_offset == LEARNING_OFFSET_HOURS_DEFAULT);
  REQUIRE(s.nox_learning_offset == LEARNING_OFFSET_HOURS_DEFAULT);
  REQUIRE(s.static_ip.ip == 0);
  REQUIRE(s.static_ip.netmask == 0);
  REQUIRE(s.static_ip.gateway == 0);
  REQUIRE(s.static_ip.dns_primary == 0);
  REQUIRE(s.static_ip.dns_secondary == 0);
  REQUIRE(s.onboarding_done == false);
  REQUIRE(s.corrections.pm25.algorithm == Pm25CorrectionAlgorithm::None);
  REQUIRE(s.corrections.temperature.algorithm == LinearCorrectionAlgorithm::None);
  REQUIRE(s.corrections.humidity.algorithm == LinearCorrectionAlgorithm::None);
}

// ============================================================================
// First-boot onboarding flag — absent key defaults false; round-trips
// ============================================================================

TEST_CASE("onboarding_done absent key loads as false", "[settings][onboarding]") {
  FakeConfigStore store;
  REQUIRE(load_go_settings(store).onboarding_done == false);
}

TEST_CASE("onboarding_done round-trips true", "[settings][onboarding]") {
  FakeConfigStore store;

  GoSettings original;
  original.onboarding_done = true;
  REQUIRE(save_go_settings(store, original));

  REQUIRE(load_go_settings(store).onboarding_done == true);
}

TEST_CASE("onboarding_done round-trips false", "[settings][onboarding]") {
  FakeConfigStore store;

  // Persist true first, then re-save false to confirm it overwrites.
  GoSettings on;
  on.onboarding_done = true;
  REQUIRE(save_go_settings(store, on));

  GoSettings off;
  off.onboarding_done = false;
  REQUIRE(save_go_settings(store, off));

  REQUIRE(load_go_settings(store).onboarding_done == false);
}

// ============================================================================
// Round-trip — save then load preserves every field
// ============================================================================

TEST_CASE("save then load round-trips all fields", "[settings]") {
  FakeConfigStore store;

  GoSettings original;
  original.measure_interval_seconds = 60;
  original.gps_mode = GpsMode::AlwaysOn;
  original.operating_mode = OperatingMode::Offline;
  original.use_fahrenheit = true;
  original.use_feet = true;
  original.pm_use_usaqi = true;
  original.auto_lock_seconds = 30;
  original.configuration_control = ConfigurationControl::Local;
  original.co2_abc_days = 200;
  original.tvoc_learning_offset = 24;
  original.nox_learning_offset = 48;

  REQUIRE(save_go_settings(store, original));
  REQUIRE(store.committed());

  GoSettings loaded = load_go_settings(store);

  REQUIRE(loaded.measure_interval_seconds == original.measure_interval_seconds);
  REQUIRE(loaded.gps_mode == original.gps_mode);
  REQUIRE(loaded.operating_mode == original.operating_mode);
  REQUIRE(loaded.use_fahrenheit == original.use_fahrenheit);
  REQUIRE(loaded.use_feet == original.use_feet);
  REQUIRE(loaded.pm_use_usaqi == original.pm_use_usaqi);
  REQUIRE(loaded.auto_lock_seconds == original.auto_lock_seconds);
  REQUIRE(loaded.configuration_control == original.configuration_control);
  REQUIRE(loaded.co2_abc_days == original.co2_abc_days);
  REQUIRE(loaded.tvoc_learning_offset == original.tvoc_learning_offset);
  REQUIRE(loaded.nox_learning_offset == original.nox_learning_offset);
}

TEST_CASE("Go settings equality includes altitude unit", "[settings]") {
  GoSettings meters;
  GoSettings feet;
  feet.use_feet = true;

  REQUIRE_FALSE(meters.equals(feet));
  REQUIRE_FALSE(feet.equals(meters));
}

TEST_CASE("shared Go config fields and update model", "[settings][config]") {
  STATIC_REQUIRE(std::is_trivially_copyable<GoConfigUpdate>::value);
  REQUIRE(static_cast<uint32_t>(GoConfigField::PmStandard) == (1U << 0));
  REQUIRE(static_cast<uint32_t>(GoConfigField::TemperatureUnit) == (1U << 1));
  REQUIRE(static_cast<uint32_t>(GoConfigField::CloudConnection) == (1U << 2));
  REQUIRE(static_cast<uint32_t>(GoConfigField::ConfigurationControl) == (1U << 3));
  REQUIRE(static_cast<uint32_t>(GoConfigField::Pm25Correction) == (1U << 4));
  REQUIRE(static_cast<uint32_t>(GoConfigField::TemperatureCorrection) == (1U << 5));
  REQUIRE(static_cast<uint32_t>(GoConfigField::HumidityCorrection) == (1U << 6));
  REQUIRE(static_cast<uint32_t>(GoConfigField::Co2AbcDays) == (1U << 7));
  REQUIRE(static_cast<uint32_t>(GoConfigField::TvocLearningOffset) == (1U << 8));
  REQUIRE(static_cast<uint32_t>(GoConfigField::NoxLearningOffset) == (1U << 9));
  REQUIRE(static_cast<uint32_t>(GoConfigField::MeasurementInterval) == (1U << 10));
  REQUIRE(static_cast<uint32_t>(GoConfigField::AltitudeUnit) == (1U << 11));
  REQUIRE(static_cast<uint32_t>(GoConfigField::GpsMode) == (1U << 12));
  REQUIRE(static_cast<uint32_t>(GoConfigField::FrontLedBrightness) == (1U << 13));
  REQUIRE(static_cast<uint32_t>(GoConfigField::BackLedBrightness) == (1U << 14));
  REQUIRE(static_cast<uint32_t>(GoConfigField::TouchLedIntensity) == (1U << 15));
  REQUIRE(static_cast<uint32_t>(GoConfigField::BuzzerEnabled) == (1U << 16));

  const uint32_t mask = static_cast<uint32_t>(GoConfigField::CloudConnection) |
                        static_cast<uint32_t>(GoConfigField::HumidityCorrection);
  REQUIRE(has_go_config_field(mask, GoConfigField::CloudConnection));
  REQUIRE(has_go_config_field(mask, GoConfigField::HumidityCorrection));
  REQUIRE_FALSE(has_go_config_field(mask, GoConfigField::TemperatureUnit));

  const GoConfigUpdate update{};
  REQUIRE_FALSE(update.use_feet);
}

TEST_CASE("shared Go config validation covers interface-managed fields", "[settings][config]") {
  REQUIRE(is_measure_interval_seconds_valid(MEASURE_INTERVAL_SECONDS_MIN));
  REQUIRE(is_measure_interval_seconds_valid(MEASURE_INTERVAL_SECONDS_MAX));
  REQUIRE_FALSE(is_measure_interval_seconds_valid(MEASURE_INTERVAL_SECONDS_MIN - 1));
  REQUIRE_FALSE(is_measure_interval_seconds_valid(MEASURE_INTERVAL_SECONDS_MAX + 1));

  REQUIRE(is_gps_mode_valid(static_cast<int>(GpsMode::AlwaysOff)));
  REQUIRE(is_gps_mode_valid(static_cast<int>(GpsMode::OnWhenTracking)));
  REQUIRE(is_gps_mode_valid(static_cast<int>(GpsMode::AlwaysOn)));
  REQUIRE_FALSE(is_gps_mode_valid(-1));
  REQUIRE_FALSE(is_gps_mode_valid(3));

  REQUIRE(is_led_brightness_valid(static_cast<int>(LedBrightness::Off)));
  REQUIRE(is_led_brightness_valid(static_cast<int>(LedBrightness::Bright)));
  REQUIRE_FALSE(is_led_brightness_valid(-1));
  REQUIRE_FALSE(is_led_brightness_valid(4));

  REQUIRE(is_touch_led_intensity_valid(static_cast<int>(TouchLedIntensity::Off)));
  REQUIRE(is_touch_led_intensity_valid(static_cast<int>(TouchLedIntensity::Bright)));
  REQUIRE_FALSE(is_touch_led_intensity_valid(-1));
  REQUIRE_FALSE(is_touch_led_intensity_valid(3));
}

TEST_CASE("round-trip preserves each ConfigurationControl value", "[settings][config]") {
  FakeConfigStore store;
  GoSettings settings;

  SECTION("Cloud") {
    settings.configuration_control = ConfigurationControl::Cloud;
    REQUIRE(save_go_settings(store, settings));
    REQUIRE(load_go_settings(store).configuration_control == ConfigurationControl::Cloud);
  }

  SECTION("Local") {
    settings.configuration_control = ConfigurationControl::Local;
    REQUIRE(save_go_settings(store, settings));
    REQUIRE(load_go_settings(store).configuration_control == ConfigurationControl::Local);
  }

  SECTION("Both") {
    settings.configuration_control = ConfigurationControl::Both;
    REQUIRE(save_go_settings(store, settings));
    REQUIRE(load_go_settings(store).configuration_control == ConfigurationControl::Both);
  }
}

TEST_CASE("CO2 ABC days validate configured bounds", "[settings][config]") {
  GoSettings settings{};
  settings.co2_abc_days = CO2_ABC_DAYS_DISABLED;
  REQUIRE(is_go_settings_valid(settings));
  settings.co2_abc_days = CO2_ABC_DAYS_MIN;
  REQUIRE(is_go_settings_valid(settings));
  settings.co2_abc_days = CO2_ABC_DAYS_MAX;
  REQUIRE(is_go_settings_valid(settings));
  settings.co2_abc_days = CO2_ABC_DAYS_DISABLED - 1;
  REQUIRE_FALSE(is_go_settings_valid(settings));
  settings.co2_abc_days = 0;
  REQUIRE_FALSE(is_go_settings_valid(settings));
  settings.co2_abc_days = CO2_ABC_DAYS_MAX + 1;
  REQUIRE_FALSE(is_go_settings_valid(settings));
}

TEST_CASE("learning offsets validate configured bounds", "[settings][config]") {
  GoSettings settings{};
  settings.tvoc_learning_offset = LEARNING_OFFSET_HOURS_MIN;
  settings.nox_learning_offset = LEARNING_OFFSET_HOURS_MAX;
  REQUIRE(is_go_settings_valid(settings));

  settings.tvoc_learning_offset = LEARNING_OFFSET_HOURS_MIN - 1;
  REQUIRE_FALSE(is_go_settings_valid(settings));
  settings.tvoc_learning_offset = LEARNING_OFFSET_HOURS_DEFAULT;
  settings.nox_learning_offset = LEARNING_OFFSET_HOURS_MAX + 1;
  REQUIRE_FALSE(is_go_settings_valid(settings));
}

TEST_CASE("configuration source control gates only remote writers", "[settings][config]") {
  GoConfigUpdate update{};
  update.update_mask = static_cast<uint32_t>(GoConfigField::PmStandard);

  REQUIRE(
      is_go_config_update_allowed(ConfigurationControl::Cloud, GoConfigSource::CloudFetch, update));
  REQUIRE_FALSE(is_go_config_update_allowed(ConfigurationControl::Cloud,
                                            GoConfigSource::LocalServer, update));
  REQUIRE_FALSE(
      is_go_config_update_allowed(ConfigurationControl::Local, GoConfigSource::CloudFetch, update));
  REQUIRE(is_go_config_update_allowed(ConfigurationControl::Local, GoConfigSource::LocalServer,
                                      update));
  REQUIRE(
      is_go_config_update_allowed(ConfigurationControl::Both, GoConfigSource::CloudFetch, update));
  REQUIRE(
      is_go_config_update_allowed(ConfigurationControl::Both, GoConfigSource::LocalServer, update));

  const GoConfigSource bypass_sources[] = {
      GoConfigSource::Ble,     GoConfigSource::Ui,     GoConfigSource::Provisioning,
      GoConfigSource::Factory, GoConfigSource::System,
  };
  const ConfigurationControl controls[] = {
      ConfigurationControl::Cloud,
      ConfigurationControl::Local,
      ConfigurationControl::Both,
  };
  for (ConfigurationControl control : controls) {
    for (GoConfigSource source : bypass_sources) {
      REQUIRE(is_go_config_update_allowed(control, source, update));
    }
  }
}

TEST_CASE("cloud control permits only an exact local recovery update", "[settings][config]") {
  GoConfigUpdate update{};
  const uint32_t control_mask = static_cast<uint32_t>(GoConfigField::ConfigurationControl);

  SECTION("Local is a recovery value") {
    update.update_mask = control_mask;
    update.configuration_control = ConfigurationControl::Local;
    REQUIRE(is_go_config_update_allowed(ConfigurationControl::Cloud, GoConfigSource::LocalServer,
                                        update));
  }

  SECTION("Both is a recovery value") {
    update.update_mask = control_mask;
    update.configuration_control = ConfigurationControl::Both;
    REQUIRE(is_go_config_update_allowed(ConfigurationControl::Cloud, GoConfigSource::LocalServer,
                                        update));
  }

  SECTION("Cloud is not a recovery value") {
    update.update_mask = control_mask;
    update.configuration_control = ConfigurationControl::Cloud;
    REQUIRE_FALSE(is_go_config_update_allowed(ConfigurationControl::Cloud,
                                              GoConfigSource::LocalServer, update));
  }

  SECTION("Invalid control is not a recovery value") {
    update.update_mask = control_mask;
    update.configuration_control = static_cast<ConfigurationControl>(99);
    REQUIRE_FALSE(is_go_config_update_allowed(ConfigurationControl::Cloud,
                                              GoConfigSource::LocalServer, update));
  }

  SECTION("Recovery cannot include another field") {
    update.update_mask = control_mask | static_cast<uint32_t>(GoConfigField::AltitudeUnit);
    update.configuration_control = ConfigurationControl::Both;
    REQUIRE_FALSE(is_go_config_update_allowed(ConfigurationControl::Cloud,
                                              GoConfigSource::LocalServer, update));
  }

  SECTION("Empty update is not recovery") {
    REQUIRE_FALSE(is_go_config_update_allowed(ConfigurationControl::Cloud,
                                              GoConfigSource::LocalServer, update));
  }
}

TEST_CASE("measurement corrections round-trip and ignore the retired PM EPA flag",
          "[settings][correction]") {
  FakeConfigStore store;
  GoSettings original;
  original.corrections.pm25.algorithm = Pm25CorrectionAlgorithm::CustomViaPm25Raw;
  original.corrections.pm25.scaling_factor = 1.08f;
  original.corrections.pm25.intercept = -0.2f;
  original.corrections.pm25.use_epa2021 = true;
  original.corrections.temperature.algorithm = LinearCorrectionAlgorithm::Custom;
  original.corrections.temperature.scaling_factor = 1.01f;
  original.corrections.temperature.intercept = -0.4f;
  original.corrections.humidity.algorithm = LinearCorrectionAlgorithm::Custom;
  original.corrections.humidity.scaling_factor = 0.98f;
  original.corrections.humidity.intercept = 1.5f;

  REQUIRE(save_go_settings(store, original));
  bool retired_use_epa2021 = false;
  REQUIRE(store.get_bool("mc_pe", retired_use_epa2021) == ConfigStoreResult::NOT_FOUND);
  REQUIRE(store.set_bool("mc_pe", true) == ConfigStoreResult::OK);
  const GoSettings loaded = load_go_settings(store);

  REQUIRE(loaded.corrections.pm25.algorithm == original.corrections.pm25.algorithm);
  REQUIRE(loaded.corrections.pm25.scaling_factor == original.corrections.pm25.scaling_factor);
  REQUIRE(loaded.corrections.pm25.intercept == original.corrections.pm25.intercept);
  REQUIRE_FALSE(loaded.corrections.pm25.use_epa2021);
  REQUIRE(loaded.corrections.temperature.scaling_factor ==
          original.corrections.temperature.scaling_factor);
  REQUIRE(loaded.corrections.temperature.intercept == original.corrections.temperature.intercept);
  REQUIRE(loaded.corrections.humidity.scaling_factor ==
          original.corrections.humidity.scaling_factor);
  REQUIRE(loaded.corrections.humidity.intercept == original.corrections.humidity.intercept);
}

TEST_CASE("incomplete custom correction falls back independently", "[settings][correction]") {
  FakeConfigStore store;
  REQUIRE(store.set_int("mc_pa", static_cast<int>(Pm25CorrectionAlgorithm::Epa2021)) ==
          ConfigStoreResult::OK);
  REQUIRE(store.set_int("mc_ta", static_cast<int>(LinearCorrectionAlgorithm::Custom)) ==
          ConfigStoreResult::OK);
  REQUIRE(store.set_float("mc_ts", 1.0f) == ConfigStoreResult::OK);
  REQUIRE(store.set_int("mc_ha", static_cast<int>(LinearCorrectionAlgorithm::Custom)) ==
          ConfigStoreResult::OK);
  REQUIRE(store.set_float("mc_hs", 1.0f) == ConfigStoreResult::OK);
  REQUIRE(store.set_float("mc_hi", 0.0f) == ConfigStoreResult::OK);

  const GoSettings loaded = load_go_settings(store);
  REQUIRE(loaded.corrections.pm25.algorithm == Pm25CorrectionAlgorithm::Epa2021);
  REQUIRE(loaded.corrections.temperature.algorithm == LinearCorrectionAlgorithm::None);
  REQUIRE(loaded.corrections.humidity.algorithm == LinearCorrectionAlgorithm::Custom);
}

TEST_CASE("round-trip preserves disable_cloud and static_ip", "[settings][stationary]") {
  FakeConfigStore store;

  GoSettings original;
  original.disable_cloud = true;
  original.static_ip.ip = 0x0100A8C0;          // 192.168.0.1 (LE)
  original.static_ip.netmask = 0x00FFFFFF;     // 255.255.255.0
  original.static_ip.gateway = 0xFE00A8C0;     // 192.168.0.254
  original.static_ip.dns_primary = 0x08080808; // 8.8.8.8
  original.static_ip.dns_secondary = 0x04040808;

  REQUIRE(save_go_settings(store, original));

  GoSettings loaded = load_go_settings(store);
  REQUIRE(loaded.disable_cloud == true);
  REQUIRE(loaded.static_ip.ip == original.static_ip.ip);
  REQUIRE(loaded.static_ip.netmask == original.static_ip.netmask);
  REQUIRE(loaded.static_ip.gateway == original.static_ip.gateway);
  REQUIRE(loaded.static_ip.dns_primary == original.static_ip.dns_primary);
  REQUIRE(loaded.static_ip.dns_secondary == original.static_ip.dns_secondary);
}

TEST_CASE("invalid stored configuration_control falls back to Both",
          "[settings][config][validation]") {
  FakeConfigStore store;
  REQUIRE(store.set_int("cc", 99) == ConfigStoreResult::OK);

  REQUIRE(load_go_settings(store).configuration_control == ConfigurationControl::Both);
}

TEST_CASE("loaded Cloud control and disabled cloud are not normalized",
          "[settings][config][validation]") {
  FakeConfigStore store;
  REQUIRE(store.set_int("cc", static_cast<int>(ConfigurationControl::Cloud)) ==
          ConfigStoreResult::OK);
  REQUIRE(store.set_bool("dc", true) == ConfigStoreResult::OK);

  const GoSettings loaded = load_go_settings(store);
  REQUIRE(loaded.disable_cloud);
  REQUIRE(loaded.configuration_control == ConfigurationControl::Cloud);
}

TEST_CASE("static_ip == 0 round-trips as DHCP", "[settings][stationary]") {
  // Re-provisioning DHCP must clear any previously stored static IP.
  FakeConfigStore store;

  GoSettings with_ip;
  with_ip.static_ip.ip = 0x0100A8C0;
  with_ip.static_ip.netmask = 0x00FFFFFF;
  REQUIRE(save_go_settings(store, with_ip));

  GoSettings dhcp;
  REQUIRE(save_go_settings(store, dhcp));

  GoSettings loaded = load_go_settings(store);
  REQUIRE(loaded.static_ip.ip == 0);
  REQUIRE(loaded.static_ip.netmask == 0);
}

TEST_CASE("round-trip preserves each GpsMode value", "[settings]") {
  FakeConfigStore store;
  GoSettings s;

  SECTION("AlwaysOff") {
    s.gps_mode = GpsMode::AlwaysOff;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).gps_mode == GpsMode::AlwaysOff);
  }

  SECTION("OnWhenTracking") {
    s.gps_mode = GpsMode::OnWhenTracking;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).gps_mode == GpsMode::OnWhenTracking);
  }

  SECTION("AlwaysOn") {
    s.gps_mode = GpsMode::AlwaysOn;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).gps_mode == GpsMode::AlwaysOn);
  }
}

TEST_CASE("round-trip preserves each OperatingMode value", "[settings]") {
  FakeConfigStore store;
  GoSettings s;

  SECTION("Portable") {
    s.operating_mode = OperatingMode::Portable;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).operating_mode == OperatingMode::Portable);
  }

  SECTION("Stationary") {
    s.operating_mode = OperatingMode::Stationary;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).operating_mode == OperatingMode::Stationary);
  }

  SECTION("Offline") {
    s.operating_mode = OperatingMode::Offline;
    REQUIRE(save_go_settings(store, s));
    REQUIRE(load_go_settings(store).operating_mode == OperatingMode::Offline);
  }
}

// ============================================================================
// Validation — save rejects out-of-range values
// ============================================================================

TEST_CASE("save rejects invalid measure_interval_seconds", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  s.measure_interval_seconds = 0;
  REQUIRE_FALSE(save_go_settings(store, s));

  s.measure_interval_seconds = 3601;
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("save rejects invalid gps_mode", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  // Force an out-of-range enum value
  s.gps_mode = static_cast<GpsMode>(3);
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("save rejects invalid operating_mode", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  s.operating_mode = static_cast<OperatingMode>(3);
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("save rejects invalid auto_lock_seconds", "[settings][validation]") {
  FakeConfigStore store;
  GoSettings s;

  s.auto_lock_seconds = 5;
  REQUIRE_FALSE(save_go_settings(store, s));

  s.auto_lock_seconds = 15;
  REQUIRE_FALSE(save_go_settings(store, s));

  s.auto_lock_seconds = -1;
  REQUIRE_FALSE(save_go_settings(store, s));
}

TEST_CASE("invalid configuration_control is rejected before writes",
          "[settings][config][validation]") {
  FakeConfigStore store;
  GoSettings settings;
  settings.configuration_control = static_cast<ConfigurationControl>(99);

  REQUIRE_FALSE(is_go_settings_valid(settings));
  REQUIRE_FALSE(save_go_settings(store, settings));
  REQUIRE(store.write_attempt_count() == 0);
  REQUIRE_FALSE(store.committed());
}

TEST_CASE("Cloud control with disabled cloud is rejected before writes",
          "[settings][config][validation]") {
  FakeConfigStore store;
  GoSettings settings;
  settings.configuration_control = ConfigurationControl::Cloud;
  settings.disable_cloud = true;

  REQUIRE_FALSE(is_go_settings_valid(settings));
  REQUIRE_FALSE(save_go_settings(store, settings));
  REQUIRE(store.write_attempt_count() == 0);
  REQUIRE_FALSE(store.committed());
}

// ============================================================================
// Load ignores invalid stored values and keeps defaults
// ============================================================================

TEST_CASE("load ignores invalid stored values", "[settings][validation]") {
  FakeConfigStore store;

  // Pre-populate with out-of-range values
  GoSettings valid;
  REQUIRE(save_go_settings(store, valid));

  // Overwrite specific keys with invalid values
  store.set_int("mi", 0);   // below range
  store.set_int("gpm", 99); // invalid enum
  store.set_int("opm", -1); // invalid enum
  store.set_int("als", 42); // not in allowed set

  GoSettings loaded = load_go_settings(store);

  // All should fall back to defaults
  REQUIRE(loaded.measure_interval_seconds == 10);
  REQUIRE(loaded.gps_mode == GpsMode::OnWhenTracking);
  REQUIRE(loaded.operating_mode == OperatingMode::Portable);
  REQUIRE(loaded.auto_lock_seconds == 10);
}

// ============================================================================
// Store write failure — save returns false, no commit
// ============================================================================

TEST_CASE("save returns false when store write fails", "[settings]") {
  FakeConfigStore store;
  store.set_fail_writes(true);

  GoSettings s;
  REQUIRE_FALSE(save_go_settings(store, s));
  REQUIRE_FALSE(store.committed());
}

TEST_CASE("save handles failure of every Go settings field write", "[settings]") {
  for (std::size_t write = 1; write <= GO_SETTINGS_WRITE_COUNT; ++write) {
    FakeConfigStore store;
    store.set_fail_write_at(write);

    REQUIRE_FALSE(save_go_settings(store, GoSettings{}));
    REQUIRE(store.write_attempt_count() == write);
    REQUIRE_FALSE(store.committed());
  }
}

TEST_CASE("save writes every Go settings NVS field", "[settings]") {
  FakeConfigStore store;

  REQUIRE(save_go_settings(store, GoSettings{}));
  REQUIRE(store.write_attempt_count() == GO_SETTINGS_WRITE_COUNT);
}

// ============================================================================
// Factory fuel-gauge learning state
// ============================================================================

TEST_CASE("is_factory_learning_stage_active: only Idle is inactive", "[settings][fg]") {
  REQUIRE_FALSE(is_factory_learning_stage_active(FgLearningStage::Idle));
  REQUIRE(is_factory_learning_stage_active(FgLearningStage::Charge));
  REQUIRE(is_factory_learning_stage_active(FgLearningStage::Rest));
  REQUIRE(is_factory_learning_stage_active(FgLearningStage::Discharge));
  REQUIRE(is_factory_learning_stage_active(FgLearningStage::CycleDone));
  REQUIRE(is_factory_learning_stage_active(FgLearningStage::Verify));
  REQUIRE(is_factory_learning_stage_active(FgLearningStage::Complete)); // sticky
  REQUIRE(is_factory_learning_stage_active(FgLearningStage::Failed));   // sticky
}

TEST_CASE("factory settings load from empty store returns defaults", "[settings][fg]") {
  FakeConfigStore store;
  FactorySettings fs;
  REQUIRE(load_factory_settings(store, fs));
  REQUIRE(fs.fg_learning_stage == FgLearningStage::Idle);
  REQUIRE(fs.fg_learning_cycle == 0);
  REQUIRE(fs.fg_learning_itpor_losses == 0);
}

TEST_CASE("factory settings round-trip", "[settings][fg]") {
  FakeConfigStore store;
  FactorySettings in;
  in.fg_learning_stage = FgLearningStage::Discharge;
  in.fg_learning_cycle = 2;
  in.fg_learning_itpor_losses = 1;
  REQUIRE(save_factory_settings(store, in));
  REQUIRE(store.committed());

  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::Discharge);
  REQUIRE(out.fg_learning_cycle == 2);
  REQUIRE(out.fg_learning_itpor_losses == 1);
}

TEST_CASE("save_fg_learning_state writes just the run state", "[settings][fg]") {
  FakeConfigStore store;
  REQUIRE(save_fg_learning_state(store, FgLearningStage::CycleDone, 1, 0));

  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::CycleDone);
  REQUIRE(out.fg_learning_cycle == 1);
}

TEST_CASE("save_go_settings does not touch factory keys", "[settings][fg]") {
  FakeConfigStore store;

  FactorySettings fs;
  fs.fg_learning_stage = FgLearningStage::Verify;
  fs.fg_learning_cycle = 2;
  REQUIRE(save_factory_settings(store, fs));

  // A user settings save must leave the factory state intact.
  GoSettings gs;
  REQUIRE(save_go_settings(store, gs));

  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::Verify);
  REQUIRE(out.fg_learning_cycle == 2);
}

TEST_CASE("clear_factory_settings clears the run state", "[settings][fg]") {
  FakeConfigStore store;
  FactorySettings fs;
  fs.fg_learning_stage = FgLearningStage::Failed;
  fs.fg_learning_cycle = 2;
  REQUIRE(save_factory_settings(store, fs));

  REQUIRE(clear_factory_settings(store));

  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::Idle);
  REQUIRE(out.fg_learning_cycle == 0);
}

TEST_CASE("factory settings load ignores out-of-range stage", "[settings][fg]") {
  FakeConfigStore store;
  store.set_int("fs_s", 99); // invalid enum
  FactorySettings out;
  REQUIRE(load_factory_settings(store, out));
  REQUIRE(out.fg_learning_stage == FgLearningStage::Idle);
}
