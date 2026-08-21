#include "go_settings.h"

#include <cmath>

#include "ag_log.h"

static constexpr const char *TAG = "Settings";

namespace {

constexpr const char *KEY_MEASURE_INTERVAL_SECONDS = "mi";
constexpr const char *KEY_GPS_MODE = "gpm";
constexpr const char *KEY_OPERATING_MODE = "opm";
constexpr const char *KEY_USE_FAHRENHEIT = "uf";
constexpr const char *KEY_USE_FEET = "auf";
constexpr const char *KEY_PM_USE_USAQI = "pmu";
constexpr const char *KEY_AUTO_LOCK_SECONDS = "als";
constexpr const char *KEY_DISABLE_CLOUD = "dc";
constexpr const char *KEY_CONFIGURATION_CONTROL = "cc";
constexpr const char *KEY_CO2_ABC_DAYS = "cad";
constexpr const char *KEY_TVOC_LEARNING_OFFSET = "tlo";
constexpr const char *KEY_NOX_LEARNING_OFFSET = "nlo";
// LED brightness — stored as int, validated against enum range.
constexpr const char *KEY_FRONT_LED_BRIGHTNESS = "lb";
constexpr const char *KEY_BACK_LED_BRIGHTNESS = "blb";
constexpr const char *KEY_TOUCH_LED_INTENSITY = "tlb";
constexpr const char *KEY_BUZZER_ENABLED = "bv";
// Static IP — 5 uint32s persisted as ints. ip == 0 means DHCP.
constexpr const char *KEY_STATIC_IP = "sip";
constexpr const char *KEY_STATIC_NETMASK = "snm";
constexpr const char *KEY_STATIC_GATEWAY = "sgw";
constexpr const char *KEY_STATIC_DNS1 = "sd1";
constexpr const char *KEY_STATIC_DNS2 = "sd2";
// First-boot onboarding guide flag.
constexpr const char *KEY_ONBOARDING_DONE = "obd";
// Measurement corrections. Coefficients use ConfigStore float blobs.
constexpr const char *KEY_PM25_CORRECTION_ALGORITHM = "mc_pa";
constexpr const char *KEY_PM25_CORRECTION_SCALING_FACTOR = "mc_ps";
constexpr const char *KEY_PM25_CORRECTION_INTERCEPT = "mc_pi";
constexpr const char *KEY_TEMP_CORRECTION_ALGORITHM = "mc_ta";
constexpr const char *KEY_TEMP_CORRECTION_SCALING_FACTOR = "mc_ts";
constexpr const char *KEY_TEMP_CORRECTION_INTERCEPT = "mc_ti";
constexpr const char *KEY_HUM_CORRECTION_ALGORITHM = "mc_ha";
constexpr const char *KEY_HUM_CORRECTION_SCALING_FACTOR = "mc_hs";
constexpr const char *KEY_HUM_CORRECTION_INTERCEPT = "mc_hi";
// Factory fuel-gauge learning state — distinct keys, excluded from
// save_go_settings() so factory_reset() leaves them intact.
constexpr const char *KEY_FG_LEARNING_STAGE = "fs_s";
constexpr const char *KEY_FG_LEARNING_CYCLE = "fs_c";
constexpr const char *KEY_FG_LEARNING_ITPOR = "fs_i";
constexpr const char *KEY_FG_LEARNING_FAIL_REASON = "fs_r";

bool is_fg_learning_stage_valid(int value) {
  return value >= static_cast<int>(FgLearningStage::Idle) &&
         value <= static_cast<int>(FgLearningStage::Failed);
}

bool is_byte_valid(int value) { return value >= 0 && value <= 255; }

bool is_operating_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_configuration_control_valid(int value) {
  return value >= static_cast<int>(ConfigurationControl::Cloud) &&
         value <= static_cast<int>(ConfigurationControl::Both);
}

bool is_auto_lock_valid(int value) {
  return value == 0 || value == 10 || value == 30 || value == 60;
}

bool is_pm25_algorithm_valid(int value) {
  return value >= static_cast<int>(Pm25CorrectionAlgorithm::None) &&
         value <= static_cast<int>(Pm25CorrectionAlgorithm::CustomViaPm25Raw);
}

bool is_linear_algorithm_valid(int value) {
  return value >= static_cast<int>(LinearCorrectionAlgorithm::None) &&
         value <= static_cast<int>(LinearCorrectionAlgorithm::Custom);
}

bool load_finite_float(ConfigStore &store, const char *key, float &out) {
  return store.get_float(key, out) == ConfigStoreResult::OK && std::isfinite(out);
}

MeasurementCorrections load_measurement_corrections(ConfigStore &store) {
  MeasurementCorrections corrections{};

  int algorithm = 0;
  if (store.get_int(KEY_PM25_CORRECTION_ALGORITHM, algorithm) == ConfigStoreResult::OK &&
      is_pm25_algorithm_valid(algorithm)) {
    const auto pm_algorithm = static_cast<Pm25CorrectionAlgorithm>(algorithm);
    if (pm_algorithm == Pm25CorrectionAlgorithm::Epa2021) {
      corrections.pm25.algorithm = pm_algorithm;
    } else if (pm_algorithm == Pm25CorrectionAlgorithm::CustomViaPm25Raw) {
      float scaling_factor = 0.0f;
      float intercept = 0.0f;
      if (load_finite_float(store, KEY_PM25_CORRECTION_SCALING_FACTOR, scaling_factor) &&
          load_finite_float(store, KEY_PM25_CORRECTION_INTERCEPT, intercept)) {
        corrections.pm25.algorithm = pm_algorithm;
        corrections.pm25.scaling_factor = scaling_factor;
        corrections.pm25.intercept = intercept;
      }
    }
  }

  algorithm = 0;
  if (store.get_int(KEY_TEMP_CORRECTION_ALGORITHM, algorithm) == ConfigStoreResult::OK &&
      is_linear_algorithm_valid(algorithm)) {
    const auto temp_algorithm = static_cast<LinearCorrectionAlgorithm>(algorithm);
    if (temp_algorithm == LinearCorrectionAlgorithm::Custom) {
      float scaling_factor = 0.0f;
      float intercept = 0.0f;
      if (load_finite_float(store, KEY_TEMP_CORRECTION_SCALING_FACTOR, scaling_factor) &&
          load_finite_float(store, KEY_TEMP_CORRECTION_INTERCEPT, intercept)) {
        corrections.temperature.algorithm = temp_algorithm;
        corrections.temperature.scaling_factor = scaling_factor;
        corrections.temperature.intercept = intercept;
      }
    }
  }

  algorithm = 0;
  if (store.get_int(KEY_HUM_CORRECTION_ALGORITHM, algorithm) == ConfigStoreResult::OK &&
      is_linear_algorithm_valid(algorithm)) {
    const auto humidity_algorithm = static_cast<LinearCorrectionAlgorithm>(algorithm);
    if (humidity_algorithm == LinearCorrectionAlgorithm::Custom) {
      float scaling_factor = 0.0f;
      float intercept = 0.0f;
      if (load_finite_float(store, KEY_HUM_CORRECTION_SCALING_FACTOR, scaling_factor) &&
          load_finite_float(store, KEY_HUM_CORRECTION_INTERCEPT, intercept)) {
        corrections.humidity.algorithm = humidity_algorithm;
        corrections.humidity.scaling_factor = scaling_factor;
        corrections.humidity.intercept = intercept;
      }
    }
  }

  return corrections;
}

bool save_measurement_corrections(ConfigStore &store, const MeasurementCorrections &corrections) {
  if (!are_measurement_corrections_valid(corrections)) {
    return false;
  }

  if (store.set_int(KEY_PM25_CORRECTION_ALGORITHM, static_cast<int>(corrections.pm25.algorithm)) !=
          ConfigStoreResult::OK ||
      store.set_float(KEY_PM25_CORRECTION_SCALING_FACTOR, corrections.pm25.scaling_factor) !=
          ConfigStoreResult::OK ||
      store.set_float(KEY_PM25_CORRECTION_INTERCEPT, corrections.pm25.intercept) !=
          ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_TEMP_CORRECTION_ALGORITHM,
                    static_cast<int>(corrections.temperature.algorithm)) != ConfigStoreResult::OK ||
      store.set_float(KEY_TEMP_CORRECTION_SCALING_FACTOR, corrections.temperature.scaling_factor) !=
          ConfigStoreResult::OK ||
      store.set_float(KEY_TEMP_CORRECTION_INTERCEPT, corrections.temperature.intercept) !=
          ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_HUM_CORRECTION_ALGORITHM,
                    static_cast<int>(corrections.humidity.algorithm)) != ConfigStoreResult::OK ||
      store.set_float(KEY_HUM_CORRECTION_SCALING_FACTOR, corrections.humidity.scaling_factor) !=
          ConfigStoreResult::OK ||
      store.set_float(KEY_HUM_CORRECTION_INTERCEPT, corrections.humidity.intercept) !=
          ConfigStoreResult::OK) {
    return false;
  }

  return true;
}

} // namespace

GoSettings load_go_settings(ConfigStore &store) {
  GoSettings settings;

  int measure_interval_seconds = 0;
  if (store.get_int(KEY_MEASURE_INTERVAL_SECONDS, measure_interval_seconds) ==
          ConfigStoreResult::OK &&
      is_measure_interval_seconds_valid(measure_interval_seconds)) {
    settings.measure_interval_seconds = measure_interval_seconds;
  }

  int gps_mode = 0;
  if (store.get_int(KEY_GPS_MODE, gps_mode) == ConfigStoreResult::OK &&
      is_gps_mode_valid(gps_mode)) {
    settings.gps_mode = static_cast<GpsMode>(gps_mode);
  }

  int operating_mode = 0;
  if (store.get_int(KEY_OPERATING_MODE, operating_mode) == ConfigStoreResult::OK &&
      is_operating_mode_valid(operating_mode)) {
    settings.operating_mode = static_cast<OperatingMode>(operating_mode);
  }

  bool use_fahrenheit = false;
  if (store.get_bool(KEY_USE_FAHRENHEIT, use_fahrenheit) == ConfigStoreResult::OK) {
    settings.use_fahrenheit = use_fahrenheit;
  }

  bool use_feet = false;
  if (store.get_bool(KEY_USE_FEET, use_feet) == ConfigStoreResult::OK) {
    settings.use_feet = use_feet;
  }

  bool pm_use_usaqi = false;
  if (store.get_bool(KEY_PM_USE_USAQI, pm_use_usaqi) == ConfigStoreResult::OK) {
    settings.pm_use_usaqi = pm_use_usaqi;
  }

  int auto_lock_seconds = 0;
  if (store.get_int(KEY_AUTO_LOCK_SECONDS, auto_lock_seconds) == ConfigStoreResult::OK &&
      is_auto_lock_valid(auto_lock_seconds)) {
    settings.auto_lock_seconds = auto_lock_seconds;
  }

  bool disable_cloud = false;
  if (store.get_bool(KEY_DISABLE_CLOUD, disable_cloud) == ConfigStoreResult::OK) {
    settings.disable_cloud = disable_cloud;
  }

  int configuration_control = 0;
  if (store.get_int(KEY_CONFIGURATION_CONTROL, configuration_control) == ConfigStoreResult::OK &&
      is_configuration_control_valid(configuration_control)) {
    settings.configuration_control = static_cast<ConfigurationControl>(configuration_control);
  }

  int co2_abc_days = 0;
  if (store.get_int(KEY_CO2_ABC_DAYS, co2_abc_days) == ConfigStoreResult::OK &&
      is_co2_abc_days_valid(co2_abc_days)) {
    settings.co2_abc_days = co2_abc_days;
  }

  int learning_offset = 0;
  if (store.get_int(KEY_TVOC_LEARNING_OFFSET, learning_offset) == ConfigStoreResult::OK &&
      is_learning_offset_hours_valid(learning_offset)) {
    settings.tvoc_learning_offset = learning_offset;
  }
  if (store.get_int(KEY_NOX_LEARNING_OFFSET, learning_offset) == ConfigStoreResult::OK &&
      is_learning_offset_hours_valid(learning_offset)) {
    settings.nox_learning_offset = learning_offset;
  }

  // LED brightness — missing key or invalid value loads as Off (struct default).
  int led_val = 0;
  if (store.get_int(KEY_FRONT_LED_BRIGHTNESS, led_val) == ConfigStoreResult::OK &&
      is_led_brightness_valid(led_val)) {
    settings.front_led_brightness = static_cast<LedBrightness>(led_val);
  }
  led_val = 0;
  if (store.get_int(KEY_BACK_LED_BRIGHTNESS, led_val) == ConfigStoreResult::OK &&
      is_led_brightness_valid(led_val)) {
    settings.back_led_brightness = static_cast<LedBrightness>(led_val);
  }
  led_val = 0;
  if (store.get_int(KEY_TOUCH_LED_INTENSITY, led_val) == ConfigStoreResult::OK &&
      is_touch_led_intensity_valid(led_val)) {
    settings.touch_led_intensity = static_cast<TouchLedIntensity>(led_val);
  }

  bool buzzer_enabled = false;
  if (store.get_bool(KEY_BUZZER_ENABLED, buzzer_enabled) == ConfigStoreResult::OK) {
    settings.buzzer_enabled = buzzer_enabled;
  }

  // Static IP fields load together; ip == 0 retains the DHCP default.
  int sip = 0;
  if (store.get_int(KEY_STATIC_IP, sip) == ConfigStoreResult::OK && sip != 0) {
    settings.static_ip.ip = static_cast<uint32_t>(sip);
    int snm = 0, sgw = 0, sd1 = 0, sd2 = 0;
    store.get_int(KEY_STATIC_NETMASK, snm);
    store.get_int(KEY_STATIC_GATEWAY, sgw);
    store.get_int(KEY_STATIC_DNS1, sd1);
    store.get_int(KEY_STATIC_DNS2, sd2);
    settings.static_ip.netmask = static_cast<uint32_t>(snm);
    settings.static_ip.gateway = static_cast<uint32_t>(sgw);
    settings.static_ip.dns_primary = static_cast<uint32_t>(sd1);
    settings.static_ip.dns_secondary = static_cast<uint32_t>(sd2);
  }

  // Absent key loads false (fresh unbox shows the guide).
  bool onboarding_done = false;
  if (store.get_bool(KEY_ONBOARDING_DONE, onboarding_done) == ConfigStoreResult::OK) {
    settings.onboarding_done = onboarding_done;
  }

  settings.corrections = load_measurement_corrections(store);

  return settings;
}

bool GoSettings::equals(const GoSettings &other) const {
  return measure_interval_seconds == other.measure_interval_seconds &&
         use_fahrenheit == other.use_fahrenheit && use_feet == other.use_feet &&
         pm_use_usaqi == other.pm_use_usaqi && gps_mode == other.gps_mode &&
         operating_mode == other.operating_mode && auto_lock_seconds == other.auto_lock_seconds &&
         front_led_brightness == other.front_led_brightness &&
         back_led_brightness == other.back_led_brightness &&
         touch_led_intensity == other.touch_led_intensity &&
         buzzer_enabled == other.buzzer_enabled && disable_cloud == other.disable_cloud &&
         configuration_control == other.configuration_control &&
         co2_abc_days == other.co2_abc_days && static_ip.ip == other.static_ip.ip &&
         tvoc_learning_offset == other.tvoc_learning_offset &&
         nox_learning_offset == other.nox_learning_offset &&
         static_ip.netmask == other.static_ip.netmask &&
         static_ip.gateway == other.static_ip.gateway &&
         static_ip.dns_primary == other.static_ip.dns_primary &&
         static_ip.dns_secondary == other.static_ip.dns_secondary &&
         onboarding_done == other.onboarding_done &&
         measurement_corrections_equal(corrections, other.corrections);
}

bool is_go_settings_valid(const GoSettings &settings) {
  if (!is_measure_interval_seconds_valid(settings.measure_interval_seconds)) {
    return false;
  }

  if (!is_gps_mode_valid(static_cast<int>(settings.gps_mode))) {
    return false;
  }

  if (!is_operating_mode_valid(static_cast<int>(settings.operating_mode))) {
    return false;
  }

  if (!is_configuration_control_valid(static_cast<int>(settings.configuration_control))) {
    return false;
  }

  if (settings.configuration_control == ConfigurationControl::Cloud && settings.disable_cloud) {
    return false;
  }

  if (!is_co2_abc_days_valid(settings.co2_abc_days)) {
    return false;
  }

  if (!is_learning_offset_hours_valid(settings.tvoc_learning_offset) ||
      !is_learning_offset_hours_valid(settings.nox_learning_offset)) {
    return false;
  }

  if (!is_auto_lock_valid(settings.auto_lock_seconds)) {
    return false;
  }

  if (!is_led_brightness_valid(static_cast<int>(settings.front_led_brightness)) ||
      !is_led_brightness_valid(static_cast<int>(settings.back_led_brightness)) ||
      !is_touch_led_intensity_valid(static_cast<int>(settings.touch_led_intensity))) {
    return false;
  }

  if (!are_measurement_corrections_valid(settings.corrections)) {
    return false;
  }

  return true;
}

bool save_go_settings(ConfigStore &store, const GoSettings &settings) {
  if (!is_go_settings_valid(settings)) {
    return false;
  }

  if (store.set_int(KEY_MEASURE_INTERVAL_SECONDS, settings.measure_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_GPS_MODE, static_cast<int>(settings.gps_mode)) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_OPERATING_MODE, static_cast<int>(settings.operating_mode)) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_USE_FAHRENHEIT, settings.use_fahrenheit) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_USE_FEET, settings.use_feet) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_PM_USE_USAQI, settings.pm_use_usaqi) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_AUTO_LOCK_SECONDS, settings.auto_lock_seconds) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_DISABLE_CLOUD, settings.disable_cloud) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_CONFIGURATION_CONTROL, static_cast<int>(settings.configuration_control)) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_CO2_ABC_DAYS, settings.co2_abc_days) != ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_TVOC_LEARNING_OFFSET, settings.tvoc_learning_offset) !=
          ConfigStoreResult::OK ||
      store.set_int(KEY_NOX_LEARNING_OFFSET, settings.nox_learning_offset) !=
          ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_FRONT_LED_BRIGHTNESS, static_cast<int>(settings.front_led_brightness)) !=
      ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_BACK_LED_BRIGHTNESS, static_cast<int>(settings.back_led_brightness)) !=
      ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_TOUCH_LED_INTENSITY, static_cast<int>(settings.touch_led_intensity)) !=
      ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_bool(KEY_BUZZER_ENABLED, settings.buzzer_enabled) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_STATIC_IP, static_cast<int>(settings.static_ip.ip)) !=
      ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_STATIC_NETMASK, static_cast<int>(settings.static_ip.netmask)) !=
      ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_STATIC_GATEWAY, static_cast<int>(settings.static_ip.gateway)) !=
      ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_STATIC_DNS1, static_cast<int>(settings.static_ip.dns_primary)) !=
      ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_STATIC_DNS2, static_cast<int>(settings.static_ip.dns_secondary)) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_ONBOARDING_DONE, settings.onboarding_done) != ConfigStoreResult::OK) {
    return false;
  }

  if (!save_measurement_corrections(store, settings.corrections)) {
    return false;
  }

  if (store.commit() != ConfigStoreResult::OK) {
    return false;
  }

  print_settings(settings);
  return true;
}

// ---------------------------------------------------------------------------
// Factory fuel-gauge learning state
// ---------------------------------------------------------------------------

bool is_factory_learning_stage_active(FgLearningStage stage) {
  return stage != FgLearningStage::Idle;
}

bool load_factory_settings(ConfigStore &store, FactorySettings &out) {
  FactorySettings fs;

  int stage = 0;
  if (store.get_int(KEY_FG_LEARNING_STAGE, stage) == ConfigStoreResult::OK &&
      is_fg_learning_stage_valid(stage)) {
    fs.fg_learning_stage = static_cast<FgLearningStage>(stage);
  }

  int cycle = 0;
  if (store.get_int(KEY_FG_LEARNING_CYCLE, cycle) == ConfigStoreResult::OK &&
      is_byte_valid(cycle)) {
    fs.fg_learning_cycle = static_cast<uint8_t>(cycle);
  }

  int itpor = 0;
  if (store.get_int(KEY_FG_LEARNING_ITPOR, itpor) == ConfigStoreResult::OK &&
      is_byte_valid(itpor)) {
    fs.fg_learning_itpor_losses = static_cast<uint8_t>(itpor);
  }

  int reason = 0;
  if (store.get_int(KEY_FG_LEARNING_FAIL_REASON, reason) == ConfigStoreResult::OK &&
      is_byte_valid(reason)) {
    fs.fg_learning_fail_reason = static_cast<uint8_t>(reason);
  }

  out = fs;
  return true;
}

bool save_factory_settings(ConfigStore &store, const FactorySettings &in) {
  return save_fg_learning_state(store, in.fg_learning_stage, in.fg_learning_cycle,
                                in.fg_learning_itpor_losses);
}

bool save_fg_learning_state(ConfigStore &store, FgLearningStage stage, uint8_t cycle,
                            uint8_t itpor_losses) {
  if (store.set_int(KEY_FG_LEARNING_STAGE, static_cast<int>(stage)) != ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_FG_LEARNING_CYCLE, static_cast<int>(cycle)) != ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_FG_LEARNING_ITPOR, static_cast<int>(itpor_losses)) !=
      ConfigStoreResult::OK) {
    return false;
  }
  return store.commit() == ConfigStoreResult::OK;
}

bool save_fg_learning_fail_reason(ConfigStore &store, uint8_t reason) {
  if (store.set_int(KEY_FG_LEARNING_FAIL_REASON, static_cast<int>(reason)) !=
      ConfigStoreResult::OK) {
    return false;
  }
  return store.commit() == ConfigStoreResult::OK;
}

bool clear_factory_settings(ConfigStore &store) {
  store.erase(KEY_FG_LEARNING_STAGE);
  store.erase(KEY_FG_LEARNING_CYCLE);
  store.erase(KEY_FG_LEARNING_ITPOR);
  store.erase(KEY_FG_LEARNING_FAIL_REASON);
  return store.commit() == ConfigStoreResult::OK;
}

void print_settings(const GoSettings &settings) {
  AG_LOGI(TAG,
          "** settings | meas_int=%d | gps_mode=%d "
          "op_mode=%d | auto_lock=%d | fahrenheit=%s feet=%s usaqi=%s | "
          "led: front=%d back=%d touch=%d | buzzer=%s | "
          "disable_cloud=%s config_control=%d co2_abc_days=%d "
          "tvoc_learning_offset=%d nox_learning_offset=%d static_ip=%s "
          "onboarding_done=%s **",
          settings.measure_interval_seconds, settings.gps_mode, settings.operating_mode,
          settings.auto_lock_seconds, settings.use_fahrenheit ? "true" : "false",
          settings.use_feet ? "true" : "false", settings.pm_use_usaqi ? "true" : "false",
          static_cast<int>(settings.front_led_brightness),
          static_cast<int>(settings.back_led_brightness),
          static_cast<int>(settings.touch_led_intensity), settings.buzzer_enabled ? "on" : "off",
          settings.disable_cloud ? "true" : "false",
          static_cast<int>(settings.configuration_control), settings.co2_abc_days,
          settings.tvoc_learning_offset, settings.nox_learning_offset,
          settings.static_ip.ip != 0 ? "set" : "dhcp", settings.onboarding_done ? "true" : "false");
  AG_LOGI(TAG,
          "** corrections | pm_alg=%d pm_scale=%.6f pm_intercept=%.6f "
          "temp_alg=%d temp_scale=%.6f temp_intercept=%.6f "
          "hum_alg=%d hum_scale=%.6f hum_intercept=%.6f **",
          static_cast<int>(settings.corrections.pm25.algorithm),
          static_cast<double>(settings.corrections.pm25.scaling_factor),
          static_cast<double>(settings.corrections.pm25.intercept),
          static_cast<int>(settings.corrections.temperature.algorithm),
          static_cast<double>(settings.corrections.temperature.scaling_factor),
          static_cast<double>(settings.corrections.temperature.intercept),
          static_cast<int>(settings.corrections.humidity.algorithm),
          static_cast<double>(settings.corrections.humidity.scaling_factor),
          static_cast<double>(settings.corrections.humidity.intercept));
}
