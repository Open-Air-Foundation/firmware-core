/**
 * AirGradient Go -- local API product service
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_local_api.h"

#include <cmath>
#include <cstring>

#include "go_events.h"
#include "measurement_corrections.h"
#include "retained_uptime.h"

namespace {

constexpr const char *PM_STANDARD_MASS = "ugm3";
constexpr const char *PM_STANDARD_US_AQI = "us-aqi";
constexpr const char *TEMPERATURE_UNIT_CELSIUS = "c";
constexpr const char *TEMPERATURE_UNIT_FAHRENHEIT = "f";
constexpr const char *ALTITUDE_UNIT_METERS = "m";
constexpr const char *ALTITUDE_UNIT_FEET = "ft";
constexpr const char *CONFIG_CONTROL_CLOUD = "cloud";
constexpr const char *CONFIG_CONTROL_LOCAL = "local";
constexpr const char *CONFIG_CONTROL_BOTH = "both";
constexpr const char *GPS_MODE_OFF = "off";
constexpr const char *GPS_MODE_TRACKING = "tracking";
constexpr const char *GPS_MODE_ALWAYS = "always";
constexpr int LOCAL_SERVER_CO2_ABC_DAYS_DISABLED = 0;

constexpr const char *CORRECTION_NONE = "none";
constexpr const char *CORRECTION_EPA_2021 = "epa_2021";
constexpr const char *CORRECTION_CUSTOM_PM25 = "custom_via_pm25_raw";
constexpr const char *CORRECTION_CUSTOM_LINEAR = "custom";

void copy_string(char *destination, size_t destination_size, const char *source) {
  if (destination_size == 0) {
    return;
  }

  std::memset(destination, 0, destination_size);
  if (source != nullptr) {
    std::strncpy(destination, source, destination_size - 1);
  }
}

bool finite_float(float value) { return std::isfinite(value); }

bool take_finite_float(const std::optional<float> &source, float &destination) {
  if (!source.has_value() || !std::isfinite(*source)) {
    return false;
  }

  destination = *source;
  return true;
}

CorrectionEntry make_pm25_correction(const Pm25Correction &correction) {
  CorrectionEntry entry{};
  switch (correction.algorithm) {
  case Pm25CorrectionAlgorithm::None:
    entry.algorithm = CORRECTION_NONE;
    break;
  case Pm25CorrectionAlgorithm::Epa2021:
    entry.algorithm = CORRECTION_EPA_2021;
    break;
  case Pm25CorrectionAlgorithm::CustomViaPm25Raw: {
    entry.algorithm = CORRECTION_CUSTOM_PM25;
    SlrParams slr{};
    slr.intercept = correction.intercept;
    slr.scaling_factor = correction.scaling_factor;
    entry.slr = slr;
    break;
  }
  }
  return entry;
}

CorrectionEntry make_linear_correction(const LinearCorrection &correction) {
  CorrectionEntry entry{};
  switch (correction.algorithm) {
  case LinearCorrectionAlgorithm::None:
    entry.algorithm = CORRECTION_NONE;
    break;
  case LinearCorrectionAlgorithm::Custom: {
    entry.algorithm = CORRECTION_CUSTOM_LINEAR;
    SlrParams slr{};
    slr.intercept = correction.intercept;
    slr.scaling_factor = correction.scaling_factor;
    entry.slr = slr;
    break;
  }
  }
  return entry;
}

bool is_source_allowed(ConfigurationControl control, const GoConfigUpdate &update,
                       bool exact_control_recovery) {
  if (control == ConfigurationControl::Cloud && !exact_control_recovery) {
    return false;
  }
  return is_go_config_update_allowed(control, GoConfigSource::LocalServer, update);
}

} // namespace

GoLocalApiService::GoLocalApiService(RtosQueueHandle event_queue, const Config &config)
    : _event_queue(event_queue) {
  copy_string(_system_info.serial_number, sizeof(_system_info.serial_number), config.serial_number);
  copy_string(_system_info.model, sizeof(_system_info.model), config.model);
  copy_string(_system_info.firmware, sizeof(_system_info.firmware), config.firmware_version);
  _config = map_config(_active_config);
}

bool GoLocalApiService::is_valid() const {
  if (_event_queue == nullptr) {
    return false;
  }
#ifdef TEST_HOST
  return true;
#else
  return _mutex.is_valid();
#endif
}

Measures GoLocalApiService::get_measures() {
  if (!lock()) {
    return Measures{};
  }
  const Measures measures = _measures;
  _mutex.unlock();
  return measures;
}

SystemInfo GoLocalApiService::get_system_info() {
  if (!lock()) {
    return SystemInfo{};
  }
  SystemInfo system_info = _system_info;
  _mutex.unlock();
  system_info.boot = retained_uptime::completed_minutes();
  return system_info;
}

LocalServerConfig GoLocalApiService::get_config() {
  if (!lock()) {
    return LocalServerConfig{};
  }
  const LocalServerConfig config = _config;
  _mutex.unlock();
  return config;
}

ConfigSubmitResult GoLocalApiService::submit_config(const LocalServerConfig &partial) {
  if (!lock()) {
    return {ConfigSubmitStatus::Internal, ConfigFieldId::None};
  }
  const ConfigAccess access_snapshot = _access;
  const ActiveConfigSnapshot active_snapshot = _active_config;
  const uint32_t queue_epoch_snapshot = _queue_epoch;
  _mutex.unlock();

  if (access_snapshot != ConfigAccess::ReadWrite) {
    return {ConfigSubmitStatus::Forbidden, ConfigFieldId::None};
  }

  const bool exact_control_recovery = is_exact_control_recovery(partial);
  if (active_snapshot.configuration_control == ConfigurationControl::Cloud &&
      !exact_control_recovery) {
    return {ConfigSubmitStatus::Forbidden, ConfigFieldId::None};
  }

  const ConfigFieldId unsupported_field = first_unsupported_field(partial);
  if (unsupported_field != ConfigFieldId::None) {
    return {ConfigSubmitStatus::NotSupported, unsupported_field};
  }

  GoConfigUpdate update{};
  const ConfigSubmitResult translated = translate_config(partial, active_snapshot, update);
  if (translated.status != ConfigSubmitStatus::Accepted) {
    return translated;
  }

  return admit_config(update, exact_control_recovery, queue_epoch_snapshot);
}

ActionResult GoLocalApiService::trigger(ActionId action) {
  if (!lock()) {
    return {ActionStatus::Busy};
  }

  if (_access != ConfigAccess::ReadWrite) {
    _mutex.unlock();
    return {ActionStatus::Rejected};
  }

  if (action != ActionId::CalibrateCo2 && action != ActionId::TestLeds &&
      action != ActionId::TestGps) {
    _mutex.unlock();
    return {ActionStatus::NotSupported};
  }

  if (_count == LOCAL_API_REQUEST_QUEUE_DEPTH) {
    _mutex.unlock();
    return {ActionStatus::Busy};
  }

  LocalApiRequest request{};
  request.kind = LocalApiRequestKind::Action;
  request.action = action;
  if (!append_and_signal_locked(request)) {
    _mutex.unlock();
    return {ActionStatus::Busy};
  }

  _mutex.unlock();
  return {ActionStatus::Dispatched};
}

void GoLocalApiService::publish_measurement_snapshot(const MeasuresAGo &corrected) {
  const Measures measures = map_measures(corrected);
  if (!lock()) {
    return;
  }
  _measures = measures;
  _mutex.unlock();
}

void GoLocalApiService::publish_config_snapshot(const GoSettings &settings) {
  const ActiveConfigSnapshot active = make_active_config(settings);
  LocalServerConfig config = map_config(active);

  if (!lock()) {
    return;
  }
  _active_config = active;
  _config = config;
  _mutex.unlock();
}

void GoLocalApiService::publish_wifi_rssi(std::optional<int> wifi_rssi) {
  if (!lock()) {
    return;
  }
  _system_info.wifi_rssi = wifi_rssi;
  _mutex.unlock();
}

void GoLocalApiService::set_access(ConfigAccess access) {
  if (!lock()) {
    return;
  }
  _access = access;
  _mutex.unlock();
}

ConfigAccess GoLocalApiService::access() const {
  if (!lock()) {
    return ConfigAccess::Disabled;
  }
  const ConfigAccess current = _access;
  _mutex.unlock();
  return current;
}

bool GoLocalApiService::pop_request(uint32_t event_epoch, LocalApiRequest &request) {
  if (!lock()) {
    return false;
  }

  if (event_epoch != _queue_epoch || _count == 0) {
    _mutex.unlock();
    return false;
  }

  request = _requests[_head];
  _requests[_head] = LocalApiRequest{};
  _head = (_head + 1) % LOCAL_API_REQUEST_QUEUE_DEPTH;
  --_count;

  _mutex.unlock();
  return true;
}

size_t GoLocalApiService::clear_requests() {
  if (!lock()) {
    return 0;
  }

  const size_t discarded = _count;
  for (LocalApiRequest &request : _requests) {
    request = LocalApiRequest{};
  }
  _head = 0;
  _tail = 0;
  _count = 0;
  ++_queue_epoch;

  _mutex.unlock();
  return discarded;
}

uint32_t GoLocalApiService::queue_epoch() const {
  if (!lock()) {
    return 0;
  }
  const uint32_t epoch = _queue_epoch;
  _mutex.unlock();
  return epoch;
}

Measures GoLocalApiService::map_measures(const MeasuresAGo &corrected) {
  Measures measures{};

  if (corrected.co2.is_valid()) {
    measures.co2.co2 = corrected.co2.co2;
  }
  if (corrected.pm_a.is_pm_01_valid() && finite_float(corrected.pm_a.pm_01)) {
    measures.pm_a.pm_01 = corrected.pm_a.pm_01;
  }
  if (corrected.pm_a.is_pm_25_valid() && finite_float(corrected.pm_a.pm_25)) {
    measures.pm_a.pm_25 = corrected.pm_a.pm_25;
  }
  if (corrected.pm_a.is_pm_10_valid() && finite_float(corrected.pm_a.pm_10)) {
    measures.pm_a.pm_10 = corrected.pm_a.pm_10;
  }
  if (corrected.pm_a.is_pm_03_pc_valid() && finite_float(corrected.pm_a.pm_03_pc)) {
    measures.pm_a.pm_03_pc = corrected.pm_a.pm_03_pc;
  }
  if (corrected.pm_a.is_pm_05_pc_valid() && finite_float(corrected.pm_a.pm_05_pc)) {
    measures.pm_a.pm_05_pc = corrected.pm_a.pm_05_pc;
  }
  if (corrected.pm_a.is_pm_01_pc_valid() && finite_float(corrected.pm_a.pm_01_pc)) {
    measures.pm_a.pm_01_pc = corrected.pm_a.pm_01_pc;
  }
  if (corrected.pm_a.is_pm_25_pc_valid() && finite_float(corrected.pm_a.pm_25_pc)) {
    measures.pm_a.pm_25_pc = corrected.pm_a.pm_25_pc;
  }
  if (corrected.pm_a.is_pm_5_pc_valid() && finite_float(corrected.pm_a.pm_5_pc)) {
    measures.pm_a.pm_5_pc = corrected.pm_a.pm_5_pc;
  }
  if (corrected.pm_a.is_pm_10_pc_valid() && finite_float(corrected.pm_a.pm_10_pc)) {
    measures.pm_a.pm_10_pc = corrected.pm_a.pm_10_pc;
  }
  if (corrected.temp_hum_a.is_temp_valid() && finite_float(corrected.temp_hum_a.temperature)) {
    measures.temp_hum_a.temperature = corrected.temp_hum_a.temperature;
  }
  if (corrected.temp_hum_a.is_hum_valid() && finite_float(corrected.temp_hum_a.humidity)) {
    measures.temp_hum_a.humidity = corrected.temp_hum_a.humidity;
  }
  if (corrected.tvoc_nox.is_tvoc_index_valid()) {
    measures.tvoc_nox.tvoc_index = corrected.tvoc_nox.tvoc_index;
  }
  if (corrected.tvoc_nox.is_tvoc_raw_valid()) {
    measures.tvoc_nox.tvoc_raw = corrected.tvoc_nox.tvoc_raw;
  }
  if (corrected.tvoc_nox.is_nox_index_valid()) {
    measures.tvoc_nox.nox_index = corrected.tvoc_nox.nox_index;
  }
  if (corrected.tvoc_nox.is_nox_raw_valid()) {
    measures.tvoc_nox.nox_raw = corrected.tvoc_nox.nox_raw;
  }
  if (corrected.power.is_battery_percentage_valid() &&
      finite_float(corrected.power.battery_percentage)) {
    measures.power.battery_percentage = corrected.power.battery_percentage;
  }
  if (corrected.power.is_battery_voltage_valid() && finite_float(corrected.power.battery_voltage)) {
    measures.power.battery_voltage = corrected.power.battery_voltage;
  }
  if (corrected.power.is_charging_voltage_valid() &&
      finite_float(corrected.power.charging_voltage)) {
    measures.power.charging_voltage = corrected.power.charging_voltage;
  }

  return measures;
}

GoLocalApiService::ActiveConfigSnapshot
GoLocalApiService::make_active_config(const GoSettings &settings) {
  ActiveConfigSnapshot active{};
  active.pm_use_usaqi = settings.pm_use_usaqi;
  active.use_fahrenheit = settings.use_fahrenheit;
  active.use_feet = settings.use_feet;
  active.disable_cloud = settings.disable_cloud;
  active.configuration_control = settings.configuration_control;
  active.measure_interval_seconds = settings.measure_interval_seconds;
  active.gps_mode = settings.gps_mode;
  active.front_led_brightness = settings.front_led_brightness;
  active.back_led_brightness = settings.back_led_brightness;
  active.touch_led_intensity = settings.touch_led_intensity;
  active.buzzer_enabled = settings.buzzer_enabled;
  active.co2_abc_days = settings.co2_abc_days;
  active.tvoc_learning_offset = settings.tvoc_learning_offset;
  active.nox_learning_offset = settings.nox_learning_offset;
  active.corrections = settings.corrections;
  return active;
}

LocalServerConfig GoLocalApiService::map_config(const ActiveConfigSnapshot &active) {
  LocalServerConfig config{};
  config.pm_standard = active.pm_use_usaqi ? PM_STANDARD_US_AQI : PM_STANDARD_MASS;
  config.temperature_unit =
      active.use_fahrenheit ? TEMPERATURE_UNIT_FAHRENHEIT : TEMPERATURE_UNIT_CELSIUS;
  config.altitude_unit = active.use_feet ? ALTITUDE_UNIT_FEET : ALTITUDE_UNIT_METERS;
  config.cloud_connection = !active.disable_cloud;
  config.measurement_interval_seconds = active.measure_interval_seconds;
  config.front_led_brightness = static_cast<int>(active.front_led_brightness);
  config.back_led_brightness = static_cast<int>(active.back_led_brightness);
  config.touch_led_intensity = static_cast<int>(active.touch_led_intensity);
  config.buzzer_enabled = active.buzzer_enabled;
  config.co2_abc_days = active.co2_abc_days == CO2_ABC_DAYS_DISABLED
                            ? LOCAL_SERVER_CO2_ABC_DAYS_DISABLED
                            : active.co2_abc_days;
  config.tvoc_learning_offset = active.tvoc_learning_offset;
  config.nox_learning_offset = active.nox_learning_offset;

  switch (active.configuration_control) {
  case ConfigurationControl::Cloud:
    config.configuration_control = CONFIG_CONTROL_CLOUD;
    break;
  case ConfigurationControl::Local:
    config.configuration_control = CONFIG_CONTROL_LOCAL;
    break;
  case ConfigurationControl::Both:
    config.configuration_control = CONFIG_CONTROL_BOTH;
    break;
  }

  switch (active.gps_mode) {
  case GpsMode::AlwaysOff:
    config.gps_mode = GPS_MODE_OFF;
    break;
  case GpsMode::OnWhenTracking:
    config.gps_mode = GPS_MODE_TRACKING;
    break;
  case GpsMode::AlwaysOn:
    config.gps_mode = GPS_MODE_ALWAYS;
    break;
  }

  Corrections corrections{};
  corrections.pm25 = make_pm25_correction(active.corrections.pm25);
  corrections.temperature = make_linear_correction(active.corrections.temperature);
  corrections.humidity = make_linear_correction(active.corrections.humidity);
  config.corrections = corrections;
  return config;
}

ConfigFieldId GoLocalApiService::first_unsupported_field(const LocalServerConfig &partial) {
  if (partial.country.has_value()) {
    return ConfigFieldId::CountryCode;
  }
  if (partial.post_data_to_cloud.has_value()) {
    return ConfigFieldId::PostDataToCloud;
  }
  if (partial.led_mode.has_value()) {
    return ConfigFieldId::LedMode;
  }
  if (partial.led_bar_brightness.has_value()) {
    return ConfigFieldId::LedBarBrightness;
  }
  if (partial.display_brightness.has_value()) {
    return ConfigFieldId::DisplayBrightness;
  }
  if (partial.mqtt_broker_url.has_value()) {
    return ConfigFieldId::MqttBrokerUrl;
  }
  if (partial.http_domain.has_value()) {
    return ConfigFieldId::HttpDomain;
  }
  return ConfigFieldId::None;
}

bool GoLocalApiService::is_exact_control_recovery(const LocalServerConfig &partial) {
  if (!partial.configuration_control.has_value() ||
      (*partial.configuration_control != CONFIG_CONTROL_LOCAL &&
       *partial.configuration_control != CONFIG_CONTROL_BOTH)) {
    return false;
  }

  return !partial.country.has_value() && !partial.pm_standard.has_value() &&
         !partial.temperature_unit.has_value() && !partial.altitude_unit.has_value() &&
         !partial.post_data_to_cloud.has_value() && !partial.cloud_connection.has_value() &&
         !partial.measurement_interval_seconds.has_value() && !partial.gps_mode.has_value() &&
         !partial.front_led_brightness.has_value() && !partial.back_led_brightness.has_value() &&
         !partial.touch_led_intensity.has_value() && !partial.buzzer_enabled.has_value() &&
         !partial.co2_abc_days.has_value() && !partial.tvoc_learning_offset.has_value() &&
         !partial.nox_learning_offset.has_value() && !partial.led_mode.has_value() &&
         !partial.led_bar_brightness.has_value() && !partial.display_brightness.has_value() &&
         !partial.mqtt_broker_url.has_value() && !partial.http_domain.has_value() &&
         !partial.corrections.has_value();
}

ConfigSubmitResult GoLocalApiService::translate_config(const LocalServerConfig &partial,
                                                       const ActiveConfigSnapshot &active,
                                                       GoConfigUpdate &update) {
  update.corrections = active.corrections;

  if (partial.pm_standard.has_value()) {
    if (*partial.pm_standard == PM_STANDARD_MASS) {
      update.pm_use_usaqi = false;
    } else if (*partial.pm_standard == PM_STANDARD_US_AQI) {
      update.pm_use_usaqi = true;
    } else {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::PmStandard};
    }
    update.update_mask |= static_cast<uint32_t>(GoConfigField::PmStandard);
  }

  if (partial.temperature_unit.has_value()) {
    if (*partial.temperature_unit == TEMPERATURE_UNIT_CELSIUS) {
      update.use_fahrenheit = false;
    } else if (*partial.temperature_unit == TEMPERATURE_UNIT_FAHRENHEIT) {
      update.use_fahrenheit = true;
    } else {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::TemperatureUnit};
    }
    update.update_mask |= static_cast<uint32_t>(GoConfigField::TemperatureUnit);
  }

  if (partial.altitude_unit.has_value()) {
    if (*partial.altitude_unit == ALTITUDE_UNIT_METERS) {
      update.use_feet = false;
    } else if (*partial.altitude_unit == ALTITUDE_UNIT_FEET) {
      update.use_feet = true;
    } else {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::AltitudeUnit};
    }
    update.update_mask |= static_cast<uint32_t>(GoConfigField::AltitudeUnit);
  }

  if (partial.measurement_interval_seconds.has_value()) {
    if (!is_measure_interval_seconds_valid(*partial.measurement_interval_seconds)) {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::MeasurementInterval};
    }
    update.measure_interval_seconds = *partial.measurement_interval_seconds;
    update.update_mask |= static_cast<uint32_t>(GoConfigField::MeasurementInterval);
  }

  if (partial.gps_mode.has_value()) {
    if (*partial.gps_mode == GPS_MODE_OFF) {
      update.gps_mode = GpsMode::AlwaysOff;
    } else if (*partial.gps_mode == GPS_MODE_TRACKING) {
      update.gps_mode = GpsMode::OnWhenTracking;
    } else if (*partial.gps_mode == GPS_MODE_ALWAYS) {
      update.gps_mode = GpsMode::AlwaysOn;
    } else {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::GpsMode};
    }
    update.update_mask |= static_cast<uint32_t>(GoConfigField::GpsMode);
  }

  if (partial.front_led_brightness.has_value()) {
    if (!is_led_brightness_valid(*partial.front_led_brightness)) {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::FrontLedBrightness};
    }
    update.front_led_brightness = static_cast<LedBrightness>(*partial.front_led_brightness);
    update.update_mask |= static_cast<uint32_t>(GoConfigField::FrontLedBrightness);
  }

  if (partial.back_led_brightness.has_value()) {
    if (!is_led_brightness_valid(*partial.back_led_brightness)) {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::BackLedBrightness};
    }
    update.back_led_brightness = static_cast<LedBrightness>(*partial.back_led_brightness);
    update.update_mask |= static_cast<uint32_t>(GoConfigField::BackLedBrightness);
  }

  if (partial.touch_led_intensity.has_value()) {
    if (!is_touch_led_intensity_valid(*partial.touch_led_intensity)) {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::TouchLedIntensity};
    }
    update.touch_led_intensity = static_cast<TouchLedIntensity>(*partial.touch_led_intensity);
    update.update_mask |= static_cast<uint32_t>(GoConfigField::TouchLedIntensity);
  }

  if (partial.buzzer_enabled.has_value()) {
    update.buzzer_enabled = *partial.buzzer_enabled;
    update.update_mask |= static_cast<uint32_t>(GoConfigField::BuzzerEnabled);
  }

  if (partial.cloud_connection.has_value()) {
    update.disable_cloud = !*partial.cloud_connection;
    update.update_mask |= static_cast<uint32_t>(GoConfigField::CloudConnection);
  }

  if (partial.configuration_control.has_value()) {
    if (*partial.configuration_control == CONFIG_CONTROL_CLOUD) {
      update.configuration_control = ConfigurationControl::Cloud;
    } else if (*partial.configuration_control == CONFIG_CONTROL_LOCAL) {
      update.configuration_control = ConfigurationControl::Local;
    } else if (*partial.configuration_control == CONFIG_CONTROL_BOTH) {
      update.configuration_control = ConfigurationControl::Both;
    } else {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::ConfigurationControl};
    }
    update.update_mask |= static_cast<uint32_t>(GoConfigField::ConfigurationControl);
  }

  if (partial.co2_abc_days.has_value()) {
    if (*partial.co2_abc_days == LOCAL_SERVER_CO2_ABC_DAYS_DISABLED) {
      update.co2_abc_days = CO2_ABC_DAYS_DISABLED;
    } else if (*partial.co2_abc_days < CO2_ABC_DAYS_MIN ||
               *partial.co2_abc_days > CO2_ABC_DAYS_MAX) {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::Co2AbcDays};
    } else {
      update.co2_abc_days = *partial.co2_abc_days;
    }
    update.update_mask |= static_cast<uint32_t>(GoConfigField::Co2AbcDays);
  }

  if (partial.tvoc_learning_offset.has_value()) {
    if (!is_learning_offset_hours_valid(*partial.tvoc_learning_offset)) {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::TvocLearningOffset};
    }
    update.tvoc_learning_offset = *partial.tvoc_learning_offset;
    update.update_mask |= static_cast<uint32_t>(GoConfigField::TvocLearningOffset);
  }

  if (partial.nox_learning_offset.has_value()) {
    if (!is_learning_offset_hours_valid(*partial.nox_learning_offset)) {
      return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::NoxLearningOffset};
    }
    update.nox_learning_offset = *partial.nox_learning_offset;
    update.update_mask |= static_cast<uint32_t>(GoConfigField::NoxLearningOffset);
  }

  if (partial.corrections.has_value()) {
    if (partial.corrections->pm25.has_value()) {
      if (!translate_pm25_correction(*partial.corrections->pm25, update.corrections.pm25)) {
        return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::CorrectionsPm25};
      }
      update.update_mask |= static_cast<uint32_t>(GoConfigField::Pm25Correction);
    }
    if (partial.corrections->temperature.has_value()) {
      if (!translate_linear_correction(*partial.corrections->temperature,
                                       update.corrections.temperature)) {
        return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::CorrectionsTemperature};
      }
      update.update_mask |= static_cast<uint32_t>(GoConfigField::TemperatureCorrection);
    }
    if (partial.corrections->humidity.has_value()) {
      if (!translate_linear_correction(*partial.corrections->humidity,
                                       update.corrections.humidity)) {
        return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::CorrectionsHumidity};
      }
      update.update_mask |= static_cast<uint32_t>(GoConfigField::HumidityCorrection);
    }
  }

  MeasurementCorrections candidate = active.corrections;
  if (has_go_config_field(update.update_mask, GoConfigField::Pm25Correction)) {
    candidate.pm25 = update.corrections.pm25;
  }
  if (has_go_config_field(update.update_mask, GoConfigField::TemperatureCorrection)) {
    candidate.temperature = update.corrections.temperature;
  }
  if (has_go_config_field(update.update_mask, GoConfigField::HumidityCorrection)) {
    candidate.humidity = update.corrections.humidity;
  }
  if (!are_measurement_corrections_valid(candidate)) {
    return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::Corrections};
  }

  const bool candidate_disable_cloud =
      has_go_config_field(update.update_mask, GoConfigField::CloudConnection)
          ? update.disable_cloud
          : active.disable_cloud;
  const ConfigurationControl candidate_control =
      has_go_config_field(update.update_mask, GoConfigField::ConfigurationControl)
          ? update.configuration_control
          : active.configuration_control;
  if (candidate_control == ConfigurationControl::Cloud && candidate_disable_cloud) {
    return {ConfigSubmitStatus::InvalidValue, ConfigFieldId::ConfigurationControl};
  }

  return {ConfigSubmitStatus::Accepted, ConfigFieldId::None};
}

bool GoLocalApiService::translate_pm25_correction(const CorrectionEntry &entry,
                                                  Pm25Correction &correction) {
  if (entry.algorithm == CORRECTION_NONE) {
    if (entry.slr.has_value()) {
      return false;
    }
    correction = Pm25Correction{};
    return true;
  }

  if (entry.algorithm == CORRECTION_EPA_2021) {
    if (entry.slr.has_value()) {
      return false;
    }
    correction = Pm25Correction{};
    correction.algorithm = Pm25CorrectionAlgorithm::Epa2021;
    return true;
  }

  if (entry.algorithm != CORRECTION_CUSTOM_PM25 || !entry.slr.has_value()) {
    return false;
  }

  Pm25Correction parsed{};
  parsed.algorithm = Pm25CorrectionAlgorithm::CustomViaPm25Raw;
  if (!take_finite_float(entry.slr->intercept, parsed.intercept) ||
      !take_finite_float(entry.slr->scaling_factor, parsed.scaling_factor)) {
    return false;
  }
  correction = parsed;
  return true;
}

bool GoLocalApiService::translate_linear_correction(const CorrectionEntry &entry,
                                                    LinearCorrection &correction) {
  if (entry.algorithm == CORRECTION_NONE) {
    if (entry.slr.has_value()) {
      return false;
    }
    correction = LinearCorrection{};
    return true;
  }

  if (entry.algorithm != CORRECTION_CUSTOM_LINEAR || !entry.slr.has_value() ||
      entry.slr->use_epa2021.has_value()) {
    return false;
  }

  LinearCorrection parsed{};
  parsed.algorithm = LinearCorrectionAlgorithm::Custom;
  if (!take_finite_float(entry.slr->intercept, parsed.intercept) ||
      !take_finite_float(entry.slr->scaling_factor, parsed.scaling_factor)) {
    return false;
  }
  correction = parsed;
  return true;
}

ConfigSubmitResult GoLocalApiService::admit_config(const GoConfigUpdate &update,
                                                   bool exact_control_recovery,
                                                   uint32_t expected_epoch) {
  if (!lock()) {
    return {ConfigSubmitStatus::Internal, ConfigFieldId::None};
  }

  if (_access != ConfigAccess::ReadWrite ||
      !is_source_allowed(_active_config.configuration_control, update, exact_control_recovery)) {
    _mutex.unlock();
    return {ConfigSubmitStatus::Forbidden, ConfigFieldId::None};
  }

  if (update.update_mask == 0) {
    _mutex.unlock();
    return {ConfigSubmitStatus::Accepted, ConfigFieldId::None};
  }

  if (_queue_epoch != expected_epoch) {
    _mutex.unlock();
    return {ConfigSubmitStatus::Busy, ConfigFieldId::None};
  }

  if (_count == LOCAL_API_REQUEST_QUEUE_DEPTH) {
    _mutex.unlock();
    return {ConfigSubmitStatus::Busy, ConfigFieldId::None};
  }

  LocalApiRequest request{};
  request.kind = LocalApiRequestKind::Config;
  request.config = update;
  if (!append_and_signal_locked(request)) {
    _mutex.unlock();
    return {ConfigSubmitStatus::Busy, ConfigFieldId::None};
  }

  _mutex.unlock();
  return {ConfigSubmitStatus::Accepted, ConfigFieldId::None};
}

bool GoLocalApiService::append_and_signal_locked(const LocalApiRequest &request) {
  _requests[_tail] = request;
  _tail = (_tail + 1) % LOCAL_API_REQUEST_QUEUE_DEPTH;
  ++_count;

  Event event{};
  event.type = EventType::LocalApiRequestReady;
  event.local_api_epoch = _queue_epoch;
  if (!RTOS::queue_send(_event_queue, &event, 0)) {
    rollback_tail_locked();
    return false;
  }
  return true;
}

void GoLocalApiService::rollback_tail_locked() {
  _tail = (_tail + LOCAL_API_REQUEST_QUEUE_DEPTH - 1) % LOCAL_API_REQUEST_QUEUE_DEPTH;
  _requests[_tail] = LocalApiRequest{};
  --_count;
}

bool GoLocalApiService::lock() const {
#ifndef TEST_HOST
  if (!_mutex.is_valid()) {
    return false;
  }
#endif
  return _mutex.lock();
}
