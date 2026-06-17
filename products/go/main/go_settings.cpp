#include "go_settings.h"

#include "ag_log.h"

static constexpr const char *TAG = "Settings";

namespace {

constexpr const char *KEY_MEASURE_INTERVAL_SECONDS = "mi";
constexpr const char *KEY_INACTIVITY_TIMEOUT_SECONDS = "ito";
constexpr const char *KEY_GPS_INTERVAL_SECONDS = "gis";
constexpr const char *KEY_GPS_MODE = "gpm";
constexpr const char *KEY_OPERATING_MODE = "opm";
constexpr const char *KEY_DEVICE_NAME = "dn";
constexpr const char *KEY_USE_FAHRENHEIT = "uf";
constexpr const char *KEY_PM_USE_USAQI = "pmu";
constexpr const char *KEY_AUTO_LOCK_SECONDS = "als";
constexpr const char *KEY_DISABLE_CLOUD = "dc";
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
// Fuel-gauge learning factory state — separate keys, never written by
// save_go_settings(), so factory_reset() leaves them intact.
constexpr const char *KEY_FG_LEARNING_STAGE = "fs_s";
constexpr const char *KEY_FG_LEARNING_CYCLE = "fs_c";
constexpr const char *KEY_FG_LEARNING_ITPOR = "fs_i";

bool is_measure_interval_valid(int value) { return value >= 1 && value <= 3600; }

bool is_inactivity_timeout_valid(int value) { return value >= 5 && value <= 600; }

bool is_gps_interval_valid(int value) { return value >= 1 && value <= 60; }

bool is_gps_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_operating_mode_valid(int value) { return value >= 0 && value <= 2; }

bool is_auto_lock_valid(int value) {
  return value == 0 || value == 10 || value == 30 || value == 60;
}

bool is_device_name_valid(const std::string &value) { return !value.empty() && value.size() <= 64; }

bool is_led_brightness_valid(int value) { return value >= 0 && value <= 3; }

bool is_touch_led_intensity_valid(int value) { return value >= 0 && value <= 2; }

bool is_fg_learning_stage_valid(int value) {
  return value >= static_cast<int>(FgLearningStage::Idle) &&
         value <= static_cast<int>(FgLearningStage::Failed);
}

bool is_byte_valid(int value) { return value >= 0 && value <= 255; }

} // namespace

GoSettings load_go_settings(ConfigStore &store) {
  GoSettings settings;

  int measure_interval_seconds = 0;
  if (store.get_int(KEY_MEASURE_INTERVAL_SECONDS, measure_interval_seconds) ==
          ConfigStoreResult::OK &&
      is_measure_interval_valid(measure_interval_seconds)) {
    settings.measure_interval_seconds = measure_interval_seconds;
  }

  int inactivity_timeout_seconds = 0;
  if (store.get_int(KEY_INACTIVITY_TIMEOUT_SECONDS, inactivity_timeout_seconds) ==
          ConfigStoreResult::OK &&
      is_inactivity_timeout_valid(inactivity_timeout_seconds)) {
    settings.inactivity_timeout_seconds = inactivity_timeout_seconds;
  }

  int gps_interval_seconds = 0;
  if (store.get_int(KEY_GPS_INTERVAL_SECONDS, gps_interval_seconds) == ConfigStoreResult::OK &&
      is_gps_interval_valid(gps_interval_seconds)) {
    settings.gps_interval_seconds = gps_interval_seconds;
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

  std::string device_name;
  if (store.get_string(KEY_DEVICE_NAME, device_name) == ConfigStoreResult::OK &&
      is_device_name_valid(device_name)) {
    settings.device_name = device_name;
  }

  bool use_fahrenheit = false;
  if (store.get_bool(KEY_USE_FAHRENHEIT, use_fahrenheit) == ConfigStoreResult::OK) {
    settings.use_fahrenheit = use_fahrenheit;
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

  return settings;
}

bool save_go_settings(ConfigStore &store, const GoSettings &settings) {
  if (!is_measure_interval_valid(settings.measure_interval_seconds)) {
    return false;
  }

  if (!is_inactivity_timeout_valid(settings.inactivity_timeout_seconds)) {
    return false;
  }

  if (!is_gps_interval_valid(settings.gps_interval_seconds)) {
    return false;
  }

  if (!is_gps_mode_valid(static_cast<int>(settings.gps_mode))) {
    return false;
  }

  if (!is_operating_mode_valid(static_cast<int>(settings.operating_mode))) {
    return false;
  }

  if (!is_auto_lock_valid(settings.auto_lock_seconds)) {
    return false;
  }

  if (!is_device_name_valid(settings.device_name)) {
    return false;
  }

  if (store.set_int(KEY_MEASURE_INTERVAL_SECONDS, settings.measure_interval_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_INACTIVITY_TIMEOUT_SECONDS, settings.inactivity_timeout_seconds) !=
      ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_int(KEY_GPS_INTERVAL_SECONDS, settings.gps_interval_seconds) !=
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

  if (store.set_string(KEY_DEVICE_NAME, settings.device_name) != ConfigStoreResult::OK) {
    return false;
  }

  if (store.set_bool(KEY_USE_FAHRENHEIT, settings.use_fahrenheit) != ConfigStoreResult::OK) {
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

  if (store.commit() != ConfigStoreResult::OK) {
    return false;
  }

  print_settings(settings);
  return true;
}

void print_settings(const GoSettings &settings) {
  AG_LOGI(TAG,
          "** settings | meas_int=%d | gps_int=%d gps_mode=%d "
          "op_mode=%d | inactivity_to=%d auto_lock=%d | fahrenheit=%s usaqi=%s | "
          "led: front=%d back=%d touch=%d | buzzer=%s | "
          "device_name=%s | disable_cloud=%s static_ip=%s onboarding_done=%s **",
          settings.measure_interval_seconds, settings.gps_interval_seconds, settings.gps_mode,
          settings.operating_mode, settings.inactivity_timeout_seconds, settings.auto_lock_seconds,
          settings.use_fahrenheit ? "true" : "false", settings.pm_use_usaqi ? "true" : "false",
          static_cast<int>(settings.front_led_brightness),
          static_cast<int>(settings.back_led_brightness),
          static_cast<int>(settings.touch_led_intensity), settings.buzzer_enabled ? "on" : "off",
          settings.device_name.c_str(), settings.disable_cloud ? "true" : "false",
          settings.static_ip.ip != 0 ? "set" : "dhcp", settings.onboarding_done ? "true" : "false");
}

// ---------------------------------------------------------------------------
// FactorySettings — fuel-gauge learning state (separate keys, survives
// factory_reset()).
// ---------------------------------------------------------------------------

bool load_factory_settings(ConfigStore &store, FactorySettings &out) {
  out = FactorySettings{}; // defaults; absent keys keep them

  int stage = 0;
  if (store.get_int(KEY_FG_LEARNING_STAGE, stage) == ConfigStoreResult::OK &&
      is_fg_learning_stage_valid(stage)) {
    out.fg_learning_stage = static_cast<FgLearningStage>(stage);
  }

  int cycle = 0;
  if (store.get_int(KEY_FG_LEARNING_CYCLE, cycle) == ConfigStoreResult::OK &&
      is_byte_valid(cycle)) {
    out.fg_learning_cycle = static_cast<uint8_t>(cycle);
  }

  int itpor = 0;
  if (store.get_int(KEY_FG_LEARNING_ITPOR, itpor) == ConfigStoreResult::OK &&
      is_byte_valid(itpor)) {
    out.fg_learning_itpor_losses = static_cast<uint8_t>(itpor);
  }

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
  if (store.set_int(KEY_FG_LEARNING_CYCLE, cycle) != ConfigStoreResult::OK) {
    return false;
  }
  if (store.set_int(KEY_FG_LEARNING_ITPOR, itpor_losses) != ConfigStoreResult::OK) {
    return false;
  }
  return store.commit() == ConfigStoreResult::OK;
}

bool clear_factory_settings(ConfigStore &store) {
  store.erase(KEY_FG_LEARNING_STAGE);
  store.erase(KEY_FG_LEARNING_CYCLE);
  store.erase(KEY_FG_LEARNING_ITPOR);
  return store.commit() == ConfigStoreResult::OK;
}
