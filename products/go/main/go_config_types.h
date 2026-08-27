/**
 * AirGradient Go -- shared configuration types
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_CONFIG_TYPES_H
#define GO_CONFIG_TYPES_H

#include <cstdint>
#include <type_traits>

#include "go_types.h"
#include "led/go_led_types.h"
#include "measurement_corrections.h"

enum class ConfigurationControl : uint8_t {
  Cloud,
  Local,
  Both,
};

enum class GoConfigField : uint32_t {
  PmStandard = 1U << 0,
  TemperatureUnit = 1U << 1,
  CloudConnection = 1U << 2,
  ConfigurationControl = 1U << 3,
  Pm25Correction = 1U << 4,
  TemperatureCorrection = 1U << 5,
  HumidityCorrection = 1U << 6,
  Co2AbcDays = 1U << 7,
  TvocLearningOffset = 1U << 8,
  NoxLearningOffset = 1U << 9,
  MeasurementInterval = 1U << 10,
  AltitudeUnit = 1U << 11,
  GpsMode = 1U << 12,
  FrontLedBrightness = 1U << 13,
  BackLedBrightness = 1U << 14,
  TouchLedIntensity = 1U << 15,
  BuzzerEnabled = 1U << 16,
};

constexpr int MEASURE_INTERVAL_SECONDS_MIN = 1;
constexpr int MEASURE_INTERVAL_SECONDS_MAX = 3600;
constexpr int MEASURE_INTERVAL_SECONDS_DEFAULT = 10;

constexpr int CO2_ABC_DAYS_DISABLED = -1;
constexpr int CO2_ABC_DAYS_MIN = 1;
constexpr int CO2_ABC_DAYS_MAX = 200;
constexpr int CO2_ABC_DAYS_DEFAULT = 7;

constexpr int LEARNING_OFFSET_HOURS_MIN = 1;
constexpr int LEARNING_OFFSET_HOURS_MAX = 1000;
constexpr int LEARNING_OFFSET_HOURS_DEFAULT = 12;

inline bool is_measure_interval_seconds_valid(int value) {
  return value >= MEASURE_INTERVAL_SECONDS_MIN && value <= MEASURE_INTERVAL_SECONDS_MAX;
}

inline bool is_gps_mode_valid(int value) {
  return value >= static_cast<int>(GpsMode::AlwaysOff) &&
         value <= static_cast<int>(GpsMode::AlwaysOn);
}

inline bool is_led_brightness_valid(int value) {
  return value >= static_cast<int>(LedBrightness::Off) &&
         value <= static_cast<int>(LedBrightness::Bright);
}

inline bool is_touch_led_intensity_valid(int value) {
  return value >= static_cast<int>(TouchLedIntensity::Off) &&
         value <= static_cast<int>(TouchLedIntensity::Bright);
}

inline bool is_co2_abc_days_valid(int value) {
  return value == CO2_ABC_DAYS_DISABLED || (value >= CO2_ABC_DAYS_MIN && value <= CO2_ABC_DAYS_MAX);
}

inline bool is_learning_offset_hours_valid(int value) {
  return value >= LEARNING_OFFSET_HOURS_MIN && value <= LEARNING_OFFSET_HOURS_MAX;
}

inline bool has_go_config_field(uint32_t mask, GoConfigField field) {
  return (mask & static_cast<uint32_t>(field)) != 0;
}

struct GoConfigUpdate {
  uint32_t update_mask = 0;
  bool pm_use_usaqi = false;
  bool use_fahrenheit = false;
  bool use_feet = false;
  bool disable_cloud = false;
  ConfigurationControl configuration_control = ConfigurationControl::Both;
  int measure_interval_seconds = MEASURE_INTERVAL_SECONDS_DEFAULT;
  GpsMode gps_mode = GpsMode::OnWhenTracking;
  LedBrightness front_led_brightness = LedBrightness::Off;
  LedBrightness back_led_brightness = LedBrightness::Off;
  TouchLedIntensity touch_led_intensity = TouchLedIntensity::Off;
  bool buzzer_enabled = false;
  int co2_abc_days = CO2_ABC_DAYS_DEFAULT;
  int tvoc_learning_offset = LEARNING_OFFSET_HOURS_DEFAULT;
  int nox_learning_offset = LEARNING_OFFSET_HOURS_DEFAULT;
  MeasurementCorrections corrections{};
};

static_assert(std::is_trivially_copyable<GoConfigUpdate>::value,
              "Go config updates must be queue-copyable");

enum class GoConfigSource : uint8_t {
  CloudFetch,
  LocalServer,
  Ble,
  Ui,
  Provisioning,
  Factory,
  System,
};

inline bool is_go_config_update_allowed(ConfigurationControl control, GoConfigSource source,
                                        const GoConfigUpdate &update) {
  switch (source) {
  case GoConfigSource::CloudFetch:
    return control == ConfigurationControl::Cloud || control == ConfigurationControl::Both;
  case GoConfigSource::LocalServer: {
    if (control == ConfigurationControl::Local || control == ConfigurationControl::Both) {
      return true;
    }
    if (control != ConfigurationControl::Cloud) {
      return false;
    }

    // Keep a control-only recovery path so Cloud cannot permanently lock out
    // local configuration management.
    const uint32_t control_mask = static_cast<uint32_t>(GoConfigField::ConfigurationControl);
    return update.update_mask == control_mask &&
           (update.configuration_control == ConfigurationControl::Local ||
            update.configuration_control == ConfigurationControl::Both);
  }
  case GoConfigSource::Ble:
  case GoConfigSource::Ui:
  case GoConfigSource::Provisioning:
  case GoConfigSource::Factory:
  case GoConfigSource::System:
    return true;
  }

  return false;
}

#endif // GO_CONFIG_TYPES_H
