/**
 * AirGradient Go — CloudService implementation
 *
 * POST runs first each iteration; FETCH runs after.  Deadlines are
 * start-anchored so wall-clock cadence is preserved across varying
 * call durations.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_cloud.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <cJSON.h>

#include "ag_log.h"
#include "common.h"
#include "go_cloud_types.h"
#include "go_config_types.h"
#include "go_events.h"
#include "go_wifi.h"
#include "retained_uptime.h"
#include "services/ag_client.h"
#include "types/wifi_types.h"

static constexpr const char *TAG = "CloudService";

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

static constexpr uint32_t CLOUD_TASK_STACK_SIZE = 8192;
static constexpr uint32_t CLOUD_TASK_PRIORITY = 4;
static constexpr size_t FETCH_BUFFER_BYTES = 2048;

/// Dashboard "no RSSI" convention; avoids a misleading 0 dB reading.
static constexpr int RSSI_UNAVAILABLE = -127;

namespace {

constexpr const char *JSON_CORRECTIONS = "corrections";
constexpr const char *JSON_PM_STANDARD = "pmStandard";
constexpr const char *JSON_TEMPERATURE_UNIT = "temperatureUnit";
constexpr const char *JSON_MEASUREMENT_INTERVAL = "measurementInterval";
constexpr const char *JSON_GPS_MODE = "gpsMode";
constexpr const char *JSON_BACK_LED_BRIGHTNESS = "backLedBrightness";
constexpr const char *JSON_TOUCH_LED_INTENSITY = "touchLedIntensity";
constexpr const char *JSON_BUZZER_ENABLED = "buzzerEnabled";
constexpr const char *JSON_CO2_CALIBRATION_REQUESTED = "co2CalibrationRequested";
constexpr const char *JSON_LED_TEST_REQUESTED = "ledTestRequested";
constexpr const char *JSON_GPS_TEST_REQUESTED = "gpsTestRequested";
constexpr const char *JSON_ABC_DAYS = "abcDays";
constexpr const char *JSON_TVOC_LEARNING_OFFSET = "tvocLearningOffset";
constexpr const char *JSON_NOX_LEARNING_OFFSET = "noxLearningOffset";
constexpr const char *JSON_PM25 = "pm02";
constexpr const char *JSON_TEMPERATURE = "atmp";
constexpr const char *JSON_HUMIDITY = "rhum";
constexpr const char *JSON_ALGORITHM = "correctionAlgorithm";
constexpr const char *JSON_SLR = "slr";
constexpr const char *JSON_INTERCEPT = "intercept";
constexpr const char *JSON_SCALING_FACTOR = "scalingFactor";
constexpr const char *JSON_SCALING_FACTOR_VIA_PM25 = "scalingFactorViaPm25";

uint32_t deadline_wait_ms(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0 ? 0 : deadline - now;
}

bool is_json_whitespace(char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

bool parse_float(const cJSON *item, float &out) {
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) {
    return false;
  }

  const float value = static_cast<float>(item->valuedouble);
  if (!std::isfinite(value)) {
    return false;
  }

  out = value;
  return true;
}

bool parse_int_range(const cJSON *item, const char *field_name, int min_value, int max_value,
                     int &out) {
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
      std::floor(item->valuedouble) != item->valuedouble || item->valuedouble < min_value ||
      item->valuedouble > max_value) {
    AG_LOGW(TAG, "config %s rejected: expected integer from %d to %d", field_name, min_value,
            max_value);
    return false;
  }

  out = static_cast<int>(item->valuedouble);
  return true;
}

bool parse_gps_mode(const cJSON *item, GpsMode &out) {
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    AG_LOGW(TAG, "config %s rejected: value is not a string", JSON_GPS_MODE);
    return false;
  }

  if (std::strcmp(item->valuestring, "off") == 0) {
    out = GpsMode::AlwaysOff;
    return true;
  }
  if (std::strcmp(item->valuestring, "tracking") == 0) {
    out = GpsMode::OnWhenTracking;
    return true;
  }
  if (std::strcmp(item->valuestring, "always") == 0) {
    out = GpsMode::AlwaysOn;
    return true;
  }

  AG_LOGW(TAG, "config %s rejected: unsupported value '%s'", JSON_GPS_MODE, item->valuestring);
  return false;
}

bool parse_bool(const cJSON *item, const char *field_name, bool &out) {
  if (!cJSON_IsBool(item)) {
    AG_LOGW(TAG, "config %s rejected: value is not a boolean", field_name);
    return false;
  }

  out = cJSON_IsTrue(item) != 0;
  return true;
}

bool parse_co2_abc_days(const cJSON *item, int &out) {
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
      std::floor(item->valuedouble) != item->valuedouble ||
      (item->valuedouble != CO2_ABC_DAYS_DISABLED &&
       (item->valuedouble < CO2_ABC_DAYS_MIN || item->valuedouble > CO2_ABC_DAYS_MAX))) {
    AG_LOGW(TAG, "config %s rejected: expected -1 or integer from %d to %d", JSON_ABC_DAYS,
            CO2_ABC_DAYS_MIN, CO2_ABC_DAYS_MAX);
    return false;
  }

  out = static_cast<int>(item->valuedouble);
  return true;
}

bool parse_learning_offset(const cJSON *item, const char *field_name, int &out) {
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
      std::floor(item->valuedouble) != item->valuedouble ||
      item->valuedouble < LEARNING_OFFSET_HOURS_MIN ||
      item->valuedouble > LEARNING_OFFSET_HOURS_MAX) {
    AG_LOGW(TAG, "config %s rejected: expected integer from %d to %d", field_name,
            LEARNING_OFFSET_HOURS_MIN, LEARNING_OFFSET_HOURS_MAX);
    return false;
  }

  out = static_cast<int>(item->valuedouble);
  return true;
}

bool parse_string_bool(const cJSON *item, const char *field_name, const char *false_value,
                       const char *true_value, bool &out) {
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    AG_LOGW(TAG, "config %s rejected: value is not a string", field_name);
    return false;
  }

  if (std::strcmp(item->valuestring, false_value) == 0) {
    out = false;
    return true;
  }
  if (std::strcmp(item->valuestring, true_value) == 0) {
    out = true;
    return true;
  }

  AG_LOGW(TAG, "config %s rejected: unsupported value '%s'", field_name, item->valuestring);
  return false;
}

bool parse_linear_correction(const cJSON *entry, LinearCorrection &out, const char *target_name) {
  if (!cJSON_IsObject(entry)) {
    AG_LOGW(TAG, "correction %s rejected: entry is not an object", target_name);
    return false;
  }

  const cJSON *algorithm = cJSON_GetObjectItemCaseSensitive(entry, JSON_ALGORITHM);
  if (!cJSON_IsString(algorithm) || algorithm->valuestring == nullptr) {
    AG_LOGW(TAG, "correction %s rejected: algorithm is missing or not a string", target_name);
    return false;
  }

  if (std::strcmp(algorithm->valuestring, "none") == 0) {
    out = LinearCorrection{};
    return true;
  }
  if (std::strcmp(algorithm->valuestring, "custom") != 0) {
    AG_LOGW(TAG, "correction %s rejected: unsupported algorithm '%s'", target_name,
            algorithm->valuestring);
    return false;
  }

  const cJSON *slr = cJSON_GetObjectItemCaseSensitive(entry, JSON_SLR);
  if (!cJSON_IsObject(slr)) {
    AG_LOGW(TAG, "correction %s rejected: custom slr is missing or not an object", target_name);
    return false;
  }

  const cJSON *intercept = cJSON_GetObjectItemCaseSensitive(slr, JSON_INTERCEPT);
  const cJSON *scaling_factor = cJSON_GetObjectItemCaseSensitive(slr, JSON_SCALING_FACTOR);
  LinearCorrection parsed{};
  parsed.algorithm = LinearCorrectionAlgorithm::Custom;
  if (!parse_float(intercept, parsed.intercept) ||
      !parse_float(scaling_factor, parsed.scaling_factor)) {
    AG_LOGW(TAG, "correction %s rejected: custom coefficients are invalid", target_name);
    return false;
  }

  out = parsed;
  return true;
}

bool parse_pm25_correction(const cJSON *entry, Pm25Correction &out) {
  if (!cJSON_IsObject(entry)) {
    AG_LOGW(TAG, "correction %s rejected: entry is not an object", JSON_PM25);
    return false;
  }

  const cJSON *algorithm = cJSON_GetObjectItemCaseSensitive(entry, JSON_ALGORITHM);
  if (!cJSON_IsString(algorithm) || algorithm->valuestring == nullptr) {
    AG_LOGW(TAG, "correction %s rejected: algorithm is missing or not a string", JSON_PM25);
    return false;
  }

  if (std::strcmp(algorithm->valuestring, "none") == 0) {
    out = Pm25Correction{};
    return true;
  }
  if (std::strcmp(algorithm->valuestring, "epa_2021") == 0) {
    out = Pm25Correction{};
    out.algorithm = Pm25CorrectionAlgorithm::Epa2021;
    return true;
  }
  if (std::strcmp(algorithm->valuestring, "custom_via_pm25_raw") != 0) {
    AG_LOGW(TAG, "correction %s rejected: unsupported algorithm '%s'", JSON_PM25,
            algorithm->valuestring);
    return false;
  }

  const cJSON *slr = cJSON_GetObjectItemCaseSensitive(entry, JSON_SLR);
  if (!cJSON_IsObject(slr)) {
    AG_LOGW(TAG, "correction %s rejected: custom slr is missing or not an object", JSON_PM25);
    return false;
  }

  Pm25Correction parsed{};
  parsed.algorithm = Pm25CorrectionAlgorithm::CustomViaPm25Raw;
  const cJSON *intercept = cJSON_GetObjectItemCaseSensitive(slr, JSON_INTERCEPT);
  const cJSON *scaling_factor = cJSON_GetObjectItemCaseSensitive(slr, JSON_SCALING_FACTOR_VIA_PM25);
  if (!parse_float(intercept, parsed.intercept) ||
      !parse_float(scaling_factor, parsed.scaling_factor)) {
    AG_LOGW(TAG, "correction %s rejected: custom parameters are invalid", JSON_PM25);
    return false;
  }

  out = parsed;
  return true;
}

FetchConfigEventPayload parse_cloud_config(const char *buffer, size_t bytes) {
  FetchConfigEventPayload payload{};
  GoConfigUpdate &update = payload.update;
  const char *parse_end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(buffer, bytes, &parse_end, 0);
  if (root == nullptr) {
    AG_LOGW(TAG, "fetch config rejected: malformed JSON");
    return payload;
  }

  bool trailing_data = false;
  const char *end = buffer + bytes;
  for (const char *cursor = parse_end; cursor < end; ++cursor) {
    if (!is_json_whitespace(*cursor)) {
      trailing_data = true;
      break;
    }
  }
  if (trailing_data || !cJSON_IsObject(root)) {
    AG_LOGW(TAG, "fetch config rejected: root is invalid or has trailing data");
    cJSON_Delete(root);
    return payload;
  }

  const cJSON *pm_standard = cJSON_GetObjectItemCaseSensitive(root, JSON_PM_STANDARD);
  if (pm_standard != nullptr &&
      parse_string_bool(pm_standard, JSON_PM_STANDARD, "ugm3", "us-aqi", update.pm_use_usaqi)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::PmStandard);
  }

  const cJSON *temperature_unit = cJSON_GetObjectItemCaseSensitive(root, JSON_TEMPERATURE_UNIT);
  if (temperature_unit != nullptr &&
      parse_string_bool(temperature_unit, JSON_TEMPERATURE_UNIT, "c", "f", update.use_fahrenheit)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::TemperatureUnit);
  }

  const cJSON *measurement_interval =
      cJSON_GetObjectItemCaseSensitive(root, JSON_MEASUREMENT_INTERVAL);
  if (measurement_interval != nullptr &&
      parse_int_range(measurement_interval, JSON_MEASUREMENT_INTERVAL, MEASURE_INTERVAL_SECONDS_MIN,
                      MEASURE_INTERVAL_SECONDS_MAX, update.measure_interval_seconds)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::MeasurementInterval);
  }

  const cJSON *gps_mode = cJSON_GetObjectItemCaseSensitive(root, JSON_GPS_MODE);
  if (gps_mode != nullptr && parse_gps_mode(gps_mode, update.gps_mode)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::GpsMode);
  }

  int led_value = 0;
  const cJSON *back_led_brightness =
      cJSON_GetObjectItemCaseSensitive(root, JSON_BACK_LED_BRIGHTNESS);
  if (back_led_brightness != nullptr &&
      parse_int_range(back_led_brightness, JSON_BACK_LED_BRIGHTNESS,
                      static_cast<int>(LedBrightness::Off), static_cast<int>(LedBrightness::Bright),
                      led_value)) {
    update.back_led_brightness = static_cast<LedBrightness>(led_value);
    update.update_mask |= static_cast<uint32_t>(GoConfigField::BackLedBrightness);
  }

  const cJSON *touch_led_intensity =
      cJSON_GetObjectItemCaseSensitive(root, JSON_TOUCH_LED_INTENSITY);
  if (touch_led_intensity != nullptr &&
      parse_int_range(touch_led_intensity, JSON_TOUCH_LED_INTENSITY,
                      static_cast<int>(TouchLedIntensity::Off),
                      static_cast<int>(TouchLedIntensity::Bright), led_value)) {
    update.touch_led_intensity = static_cast<TouchLedIntensity>(led_value);
    update.update_mask |= static_cast<uint32_t>(GoConfigField::TouchLedIntensity);
  }

  const cJSON *buzzer_enabled = cJSON_GetObjectItemCaseSensitive(root, JSON_BUZZER_ENABLED);
  if (buzzer_enabled != nullptr &&
      parse_bool(buzzer_enabled, JSON_BUZZER_ENABLED, update.buzzer_enabled)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::BuzzerEnabled);
  }

  const cJSON *co2_calibration_requested =
      cJSON_GetObjectItemCaseSensitive(root, JSON_CO2_CALIBRATION_REQUESTED);
  if (co2_calibration_requested != nullptr) {
    (void)parse_bool(co2_calibration_requested, JSON_CO2_CALIBRATION_REQUESTED,
                     payload.co2_calibration_requested);
  }

  const cJSON *led_test_requested = cJSON_GetObjectItemCaseSensitive(root, JSON_LED_TEST_REQUESTED);
  if (led_test_requested != nullptr) {
    (void)parse_bool(led_test_requested, JSON_LED_TEST_REQUESTED, payload.led_test_requested);
  }

  const cJSON *gps_test_requested = cJSON_GetObjectItemCaseSensitive(root, JSON_GPS_TEST_REQUESTED);
  if (gps_test_requested != nullptr) {
    (void)parse_bool(gps_test_requested, JSON_GPS_TEST_REQUESTED, payload.gps_test_requested);
  }

  const cJSON *abc_days = cJSON_GetObjectItemCaseSensitive(root, JSON_ABC_DAYS);
  if (abc_days != nullptr && parse_co2_abc_days(abc_days, update.co2_abc_days)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::Co2AbcDays);
  }

  const cJSON *tvoc_learning_offset =
      cJSON_GetObjectItemCaseSensitive(root, JSON_TVOC_LEARNING_OFFSET);
  if (tvoc_learning_offset != nullptr &&
      parse_learning_offset(tvoc_learning_offset, JSON_TVOC_LEARNING_OFFSET,
                            update.tvoc_learning_offset)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::TvocLearningOffset);
  }

  const cJSON *nox_learning_offset =
      cJSON_GetObjectItemCaseSensitive(root, JSON_NOX_LEARNING_OFFSET);
  if (nox_learning_offset != nullptr &&
      parse_learning_offset(nox_learning_offset, JSON_NOX_LEARNING_OFFSET,
                            update.nox_learning_offset)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::NoxLearningOffset);
  }

  const cJSON *corrections = cJSON_GetObjectItemCaseSensitive(root, JSON_CORRECTIONS);
  if (corrections == nullptr) {
    cJSON_Delete(root);
    return payload;
  }
  if (!cJSON_IsObject(corrections)) {
    AG_LOGW(TAG, "fetch config corrections rejected: value is not an object");
    cJSON_Delete(root);
    return payload;
  }

  const cJSON *pm25 = cJSON_GetObjectItemCaseSensitive(corrections, JSON_PM25);
  if (pm25 != nullptr && parse_pm25_correction(pm25, update.corrections.pm25)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::Pm25Correction);
  }

  const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(corrections, JSON_TEMPERATURE);
  if (temperature != nullptr &&
      parse_linear_correction(temperature, update.corrections.temperature, JSON_TEMPERATURE)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::TemperatureCorrection);
  }

  const cJSON *humidity = cJSON_GetObjectItemCaseSensitive(corrections, JSON_HUMIDITY);
  if (humidity != nullptr &&
      parse_linear_correction(humidity, update.corrections.humidity, JSON_HUMIDITY)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::HumidityCorrection);
  }

  cJSON_Delete(root);
  return payload;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CloudService::CloudService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg)
    : _event_queue(event_queue), _client(deps.client), _wifi(deps.wifi), _cfg(cfg),
      _disable_cloud(cfg.disable_cloud), _config_fetch_enabled(cfg.config_fetch_enabled) {}

CloudService::~CloudService() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool CloudService::start() {
  if (_task_handle != nullptr) {
    return true;
  }

  log_heap(TAG, "cloud.start:enter");
  _shutdown_pending.store(false);

  _fetch_buf = static_cast<char *>(std::malloc(FETCH_BUFFER_BYTES));
  if (_fetch_buf == nullptr) {
    AG_LOGE(TAG, "start: fetch buffer alloc failed (%zu bytes)", FETCH_BUFFER_BYTES);
    return false;
  }

  // Semaphore must exist before task_create so stop() never races.
  if (!_done_sem.create()) {
    AG_LOGE(TAG, "start: semaphore creation failed");
    std::free(_fetch_buf);
    _fetch_buf = nullptr;
    return false;
  }

  const bool ok = RTOS::task_create(_task_entry, "cloud", CLOUD_TASK_STACK_SIZE, this,
                                    CLOUD_TASK_PRIORITY, &_task_handle);
  if (!ok) {
    AG_LOGE(TAG, "start: task_create failed");
    _done_sem.destroy();
    std::free(_fetch_buf);
    _fetch_buf = nullptr;
    _task_handle = nullptr;
    return false;
  }

  AG_LOGI(TAG, "start: task spawned (stack=%lu, prio=%lu, fetch_buf=%zu)",
          static_cast<unsigned long>(CLOUD_TASK_STACK_SIZE),
          static_cast<unsigned long>(CLOUD_TASK_PRIORITY), FETCH_BUFFER_BYTES);
  log_heap(TAG, "cloud.start:exit");
  return true;
}

void CloudService::stop() {
  if (_task_handle == nullptr) {
    return;
  }

  log_heap(TAG, "cloud.stop:enter");
  AG_LOGI(TAG, "stop: latching shutdown");
  _shutdown_pending.store(true);
  _wake();

  // Bounded by one WifiHttpClient timeout (~15 s).
  _done_sem.take();

  // Task is parked on notify-wait; safe to delete externally.
  RTOS::task_delete(_task_handle);
  _task_handle = nullptr;

  if (_fetch_buf != nullptr) {
    std::free(_fetch_buf);
    _fetch_buf = nullptr;
  }
  _done_sem.destroy();

  _shutdown_pending.store(false);
  _post_due = 0;
  _fetch_due.store(0);
  _was_armed = false;
  log_heap(TAG, "cloud.stop:exit");
}

// ---------------------------------------------------------------------------
// State actions
// ---------------------------------------------------------------------------

void CloudService::arm(bool fire_now) {
  if (fire_now) {
    _fire_now_pending.store(true);
  }
  _armed.store(true);
  _wake();
}

void CloudService::disarm() {
  _armed.store(false);
  _wake();
}

void CloudService::set_disable_cloud(bool disable) {
  _disable_cloud.store(disable);
  _wake();
}

void CloudService::set_config_fetch_enabled(bool enabled) {
  if (enabled) {
    _fetch_due.store(static_cast<uint32_t>(RTOS::get_time_ms()));
  }
  _config_fetch_enabled.store(enabled);
  _wake();
}

void CloudService::update_measures_snapshot(const MeasuresAGo &m) {
  _snapshot_mtx.lock();
  _latest_snapshot = m;
  _snapshot_mtx.unlock();
}

// ---------------------------------------------------------------------------
// Task entry / loop
// ---------------------------------------------------------------------------

// static
void CloudService::_task_entry(void *arg) {
  static_cast<CloudService *>(arg)->_run();
  // Park forever — stop() deletes the task externally.
  while (true) {
    RTOS::delay_ms(60000);
  }
}

void CloudService::_run() {
  for (;;) {
    const uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
    const uint32_t wake_ms = _run_iteration(now);

    if (_shutdown_pending.load()) {
      // Park until stop() calls task_delete (avoids double-delete race).
      RTOS::task_notify_take(UINT32_MAX);
      return;
    }

    if (wake_ms > 0) {
      RTOS::task_notify_take(wake_ms);
    }
  }
}

uint32_t CloudService::_run_iteration(uint32_t now) {
  if (_shutdown_pending.load()) {
#ifdef TEST_HOST
    _test_done_signal_count += 1;
#endif
    _done_sem.give();
    return UINT32_MAX;
  }

  const bool armed = _armed.load();
  const bool disable = _disable_cloud.load();
  const bool config_fetch_enabled = _config_fetch_enabled.load();

  // Handle Disarmed→Armed transition: snap deadlines to now (fire_now)
  // or one interval into the future.
  if (armed != _was_armed) {
    if (armed) {
      const bool fire_now = _fire_now_pending.exchange(false);
      _post_due = fire_now ? now : now + _cfg.post_interval_ms;
      _fetch_due.store(fire_now && config_fetch_enabled ? now : now + _cfg.fetch_interval_ms);
    }
    _was_armed = armed;
  }

  if (!armed || disable) {
    // Slide expired deadlines forward while disabled so re-enable
    // doesn't catch up on missed intervals.
    if (armed) {
      if (static_cast<int32_t>(now - _post_due) >= 0) {
        _post_due = now + _cfg.post_interval_ms;
      }
      const uint32_t fetch_due = _fetch_due.load();
      if (config_fetch_enabled && static_cast<int32_t>(now - fetch_due) >= 0) {
        _fetch_due.store(now + _cfg.fetch_interval_ms);
      }
    }
    if (!armed) {
      return UINT32_MAX;
    }
    const uint32_t post_wait = deadline_wait_ms(now, _post_due);
    const uint32_t fetch_wait = deadline_wait_ms(now, _fetch_due.load());
    return config_fetch_enabled ? std::min(post_wait, fetch_wait) : post_wait;
  }

  // POST priority: fire POST first; return 0 to re-sample state before
  // considering FETCH (gates out disarm/disable arriving mid-POST).
  if (static_cast<int32_t>(now - _post_due) >= 0) {
    _do_post(now);
    return 0;
  }

  if (config_fetch_enabled && static_cast<int32_t>(now - _fetch_due.load()) >= 0) {
    _do_fetch(now);
    return 0;
  }

  const uint32_t post_wait = deadline_wait_ms(now, _post_due);
  return config_fetch_enabled ? std::min(post_wait, deadline_wait_ms(now, _fetch_due.load()))
                              : post_wait;
}

// ---------------------------------------------------------------------------
// HTTP legs
// ---------------------------------------------------------------------------

void CloudService::_do_post(uint32_t now_ms) {
  const uint32_t post_started_at = now_ms;
  MeasuresAGo snap = _snapshot_copy();

  const int raw_rssi = _wifi.rssi();
  const int rssi = (raw_rssi == WIFI_RSSI_INVALID) ? RSSI_UNAVAILABLE : raw_rssi;
  const uint32_t boot = retained_uptime::completed_minutes();

  log_heap(TAG, "cloud.post:pre-tls");
  const AgClientResult result = _client.http_post_measures(snap, rssi, boot);
  log_heap(TAG, "cloud.post:post-tls");
  AG_LOGI(TAG, "post_measures result=%d rssi=%d boot=%" PRIu32, static_cast<int>(result), rssi,
          boot);

  Event evt{};
  evt.type = EventType::PostMeasuresResult;
  evt.cloud_result = static_cast<CloudResultByte>(result);
  RTOS::queue_send(_event_queue, &evt);

  // Anchor to start time, not completion.
  _post_due = post_started_at + _cfg.post_interval_ms;
}

void CloudService::_do_fetch(uint32_t now_ms) {
  const uint32_t fetch_started_at = now_ms;
  size_t bytes = 0;
  log_heap(TAG, "cloud.fetch:pre-tls");
  const AgClientResult result = _client.http_fetch_config(_fetch_buf, FETCH_BUFFER_BYTES, &bytes);
  log_heap(TAG, "cloud.fetch:post-tls");

  const size_t logged = bytes < FETCH_BUFFER_BYTES ? bytes : FETCH_BUFFER_BYTES;
  AG_LOGI(TAG, "fetch_config result=%d bytes=%zu body=%.*s", static_cast<int>(result), bytes,
          static_cast<int>(logged), _fetch_buf != nullptr ? _fetch_buf : "");

  FetchConfigEventPayload payload{};
  if (result == AgClientResult::Ok && _fetch_buf != nullptr && bytes < FETCH_BUFFER_BYTES) {
    payload = parse_cloud_config(_fetch_buf, bytes);
  }
  payload.result = static_cast<CloudResultByte>(result);

  Event evt{};
  evt.type = EventType::FetchConfigResult;
  evt.fetch_config = payload;
  RTOS::queue_send(_event_queue, &evt);

  _fetch_due.store(fetch_started_at + _cfg.fetch_interval_ms);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

MeasuresAGo CloudService::_snapshot_copy() {
  _snapshot_mtx.lock();
  MeasuresAGo out = _latest_snapshot;
  _snapshot_mtx.unlock();
  return out;
}

void CloudService::_wake() {
  if (_task_handle == nullptr) {
    return;
  }
  RTOS::task_notify_send(_task_handle, 0);
}
