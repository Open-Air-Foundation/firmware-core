/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "internal/config_json.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <cJSON.h>

#include "internal/field_names.h"

namespace config_json {

namespace {

// Enum value sets. Kept identical to the existing Home Assistant integration.
constexpr const char *PM_STANDARD_VALUES[] = {"ugm3", "us-aqi"};
constexpr const char *TEMP_UNIT_VALUES[] = {"c", "f"};
constexpr const char *ALTITUDE_UNIT_VALUES[] = {"m", "ft"};
constexpr const char *CONFIG_CONTROL_VALUES[] = {"cloud", "local", "both"};
constexpr const char *GPS_MODE_VALUES[] = {"off", "tracking", "always"};
constexpr const char *LED_MODE_VALUES[] = {"co2", "pm", "iaqs", "off"};

bool is_whitespace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

template <size_t N> bool enum_contains(const char *value, const char *const (&options)[N]) {
  for (size_t i = 0; i < N; ++i) {
    if (std::strcmp(value, options[i]) == 0) {
      return true;
    }
  }
  return false;
}

// Validate one known string field; returns true and assigns on success.
bool take_string(const cJSON *item, std::optional<std::string> &out) {
  if (!cJSON_IsString(item) || item->valuestring == nullptr) {
    return false;
  }
  out = item->valuestring;
  return true;
}

template <size_t N>
bool take_enum(const cJSON *item, const char *const (&options)[N],
               std::optional<std::string> &out) {
  if (!cJSON_IsString(item) || item->valuestring == nullptr ||
      !enum_contains(item->valuestring, options)) {
    return false;
  }
  out = item->valuestring;
  return true;
}

bool take_int(const cJSON *item, std::optional<int> &out) {
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble) ||
      std::floor(item->valuedouble) != item->valuedouble) {
    return false;
  }
  out = item->valueint;
  return true;
}

bool take_bool(const cJSON *item, std::optional<bool> &out) {
  if (!cJSON_IsBool(item)) {
    return false;
  }
  out = cJSON_IsTrue(item) != 0;
  return true;
}

// Parse an "slr" member (object or null) of a correction entry. On an unknown
// sub-key, writes a dotted path ("corrections.<measure>.slr.<key>") into
// unknown_key and returns UnknownField. Structural type errors return
// InvalidValue (the caller has already set `field` to the measure's id).
ParseStatus parse_slr(const cJSON *slr_item, bool allow_epa, const char *measure_key,
                      std::optional<SlrParams> &out, char *unknown_key) {
  if (cJSON_IsNull(slr_item)) {
    out = std::nullopt;
    return ParseStatus::Ok;
  }
  if (!cJSON_IsObject(slr_item)) {
    return ParseStatus::InvalidValue;
  }

  SlrParams params;
  for (const cJSON *sub = slr_item->child; sub != nullptr; sub = sub->next) {
    const char *key = sub->string;
    if (key == nullptr) {
      return ParseStatus::InvalidValue;
    }
    if (std::strcmp(key, fields::INTERCEPT) == 0) {
      if (!cJSON_IsNumber(sub)) {
        return ParseStatus::InvalidValue;
      }
      params.intercept = sub->valuedouble;
    } else if (std::strcmp(key, fields::SCALING_FACTOR) == 0) {
      if (!cJSON_IsNumber(sub)) {
        return ParseStatus::InvalidValue;
      }
      params.scaling_factor = sub->valuedouble;
    } else if (allow_epa && std::strcmp(key, fields::USE_EPA2021) == 0) {
      if (!cJSON_IsBool(sub)) {
        return ParseStatus::InvalidValue;
      }
      params.use_epa2021 = cJSON_IsTrue(sub) != 0;
    } else {
      std::snprintf(unknown_key, MAX_UNKNOWN_KEY, "%s.%s.%s.%s", fields::CORRECTIONS, measure_key,
                    fields::SLR, key);
      return ParseStatus::UnknownField;
    }
  }
  out = params;
  return ParseStatus::Ok;
}

// Parse one correction entry object (pm25 / temperature / humidity). The caller has
// set `field` to the matching ConfigFieldId so InvalidValue reports a dotted
// "corrections.<measure>" field. Unknown sub-keys yield a dotted UnknownField.
ParseStatus parse_entry(const cJSON *entry, bool allow_epa, const char *measure_key,
                        std::optional<CorrectionEntry> &out, char *unknown_key) {
  if (!cJSON_IsObject(entry)) {
    return ParseStatus::InvalidValue;
  }

  CorrectionEntry e;
  for (const cJSON *sub = entry->child; sub != nullptr; sub = sub->next) {
    const char *key = sub->string;
    if (key == nullptr) {
      return ParseStatus::InvalidValue;
    }
    if (std::strcmp(key, fields::CORRECTION_ALGORITHM) == 0) {
      if (!cJSON_IsString(sub) || sub->valuestring == nullptr) {
        return ParseStatus::InvalidValue;
      }
      e.algorithm = sub->valuestring;
    } else if (std::strcmp(key, fields::SLR) == 0) {
      const ParseStatus st = parse_slr(sub, allow_epa, measure_key, e.slr, unknown_key);
      if (st != ParseStatus::Ok) {
        return st;
      }
    } else {
      std::snprintf(unknown_key, MAX_UNKNOWN_KEY, "%s.%s.%s", fields::CORRECTIONS, measure_key,
                    key);
      return ParseStatus::UnknownField;
    }
  }
  out = e;
  return ParseStatus::Ok;
}

// Parse the nested "corrections" object. Reports dotted field ids / keys.
ParseStatus parse_corrections(const cJSON *item, std::optional<Corrections> &out,
                              ConfigFieldId &field, char *unknown_key) {
  field = ConfigFieldId::Corrections;
  if (!cJSON_IsObject(item)) {
    return ParseStatus::InvalidValue;
  }

  Corrections c;
  for (const cJSON *inner = item->child; inner != nullptr; inner = inner->next) {
    const char *key = inner->string;
    if (key == nullptr) {
      return ParseStatus::InvalidValue;
    }
    if (std::strcmp(key, fields::PM25) == 0) {
      field = ConfigFieldId::CorrectionsPm25;
      const ParseStatus st =
          parse_entry(inner, /*allow_epa=*/true, fields::PM25, c.pm25, unknown_key);
      if (st != ParseStatus::Ok) {
        return st;
      }
    } else if (std::strcmp(key, fields::TEMPERATURE) == 0) {
      field = ConfigFieldId::CorrectionsTemperature;
      const ParseStatus st =
          parse_entry(inner, /*allow_epa=*/false, fields::TEMPERATURE, c.temperature, unknown_key);
      if (st != ParseStatus::Ok) {
        return st;
      }
    } else if (std::strcmp(key, fields::HUMIDITY) == 0) {
      field = ConfigFieldId::CorrectionsHumidity;
      const ParseStatus st =
          parse_entry(inner, /*allow_epa=*/false, fields::HUMIDITY, c.humidity, unknown_key);
      if (st != ParseStatus::Ok) {
        return st;
      }
    } else {
      std::snprintf(unknown_key, MAX_UNKNOWN_KEY, "%s.%s", fields::CORRECTIONS, key);
      return ParseStatus::UnknownField;
    }
  }
  out = c;
  return ParseStatus::Ok;
}

// Apply a single object member. Returns Ok / InvalidValue / UnknownField and,
// on a known-field type error, sets `field`; on an unknown key, writes the
// offending (possibly dotted) key into `unknown_key`.
ParseStatus apply_item(const cJSON *item, LocalServerConfig &out, ConfigFieldId &field,
                       char *unknown_key) {
  const char *key = item->string;
  if (key == nullptr) {
    return ParseStatus::UnknownField;
  }

  if (std::strcmp(key, fields::COUNTRY) == 0) {
    field = ConfigFieldId::CountryCode;
    return take_string(item, out.country) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::PM_STANDARD) == 0) {
    field = ConfigFieldId::PmStandard;
    return take_enum(item, PM_STANDARD_VALUES, out.pm_standard) ? ParseStatus::Ok
                                                                : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::TEMPERATURE_UNIT) == 0) {
    field = ConfigFieldId::TemperatureUnit;
    return take_enum(item, TEMP_UNIT_VALUES, out.temperature_unit) ? ParseStatus::Ok
                                                                   : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::ALTITUDE_UNIT) == 0) {
    field = ConfigFieldId::AltitudeUnit;
    return take_enum(item, ALTITUDE_UNIT_VALUES, out.altitude_unit) ? ParseStatus::Ok
                                                                    : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::POST_DATA_TO_CLOUD) == 0) {
    field = ConfigFieldId::PostDataToCloud;
    return take_bool(item, out.post_data_to_cloud) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::CLOUD_CONNECTION) == 0) {
    field = ConfigFieldId::CloudConnection;
    return take_bool(item, out.cloud_connection) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::CONFIGURATION_CONTROL) == 0) {
    field = ConfigFieldId::ConfigurationControl;
    return take_enum(item, CONFIG_CONTROL_VALUES, out.configuration_control)
               ? ParseStatus::Ok
               : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::MEASUREMENT_INTERVAL) == 0) {
    field = ConfigFieldId::MeasurementInterval;
    return take_int(item, out.measurement_interval_seconds) ? ParseStatus::Ok
                                                            : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::GPS_MODE) == 0) {
    field = ConfigFieldId::GpsMode;
    return take_enum(item, GPS_MODE_VALUES, out.gps_mode) ? ParseStatus::Ok
                                                          : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::FRONT_LED_BRIGHTNESS) == 0) {
    field = ConfigFieldId::FrontLedBrightness;
    return take_int(item, out.front_led_brightness) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::BACK_LED_BRIGHTNESS) == 0) {
    field = ConfigFieldId::BackLedBrightness;
    return take_int(item, out.back_led_brightness) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::TOUCH_LED_INTENSITY) == 0) {
    field = ConfigFieldId::TouchLedIntensity;
    return take_int(item, out.touch_led_intensity) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::BUZZER_ENABLED) == 0) {
    field = ConfigFieldId::BuzzerEnabled;
    return take_bool(item, out.buzzer_enabled) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::CO2_ABC_DAYS) == 0) {
    field = ConfigFieldId::Co2AbcDays;
    return take_int(item, out.co2_abc_days) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::TVOC_LEARNING_OFFSET) == 0) {
    field = ConfigFieldId::TvocLearningOffset;
    return take_int(item, out.tvoc_learning_offset) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::NOX_LEARNING_OFFSET) == 0) {
    field = ConfigFieldId::NoxLearningOffset;
    return take_int(item, out.nox_learning_offset) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::LED_MODE) == 0) {
    field = ConfigFieldId::LedMode;
    return take_enum(item, LED_MODE_VALUES, out.led_mode) ? ParseStatus::Ok
                                                          : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::LED_BAR_BRIGHTNESS) == 0) {
    field = ConfigFieldId::LedBarBrightness;
    return take_int(item, out.led_bar_brightness) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::DISPLAY_BRIGHTNESS) == 0) {
    field = ConfigFieldId::DisplayBrightness;
    return take_int(item, out.display_brightness) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::MQTT_BROKER_URL) == 0) {
    field = ConfigFieldId::MqttBrokerUrl;
    return take_string(item, out.mqtt_broker_url) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::HTTP_DOMAIN) == 0) {
    field = ConfigFieldId::HttpDomain;
    return take_string(item, out.http_domain) ? ParseStatus::Ok : ParseStatus::InvalidValue;
  }
  if (std::strcmp(key, fields::CORRECTIONS) == 0) {
    return parse_corrections(item, out.corrections, field, unknown_key);
  }

  std::snprintf(unknown_key, MAX_UNKNOWN_KEY, "%s", key);
  return ParseStatus::UnknownField;
}

bool has_incomplete_slr(const std::optional<CorrectionEntry> &entry) {
  return entry.has_value() && entry->slr.has_value() &&
         (!entry->slr->intercept.has_value() || !entry->slr->scaling_factor.has_value());
}

// Emit up to seven significant digits instead of cJSON's widened double representation.
bool add_float_to_object(cJSON *object, const char *key, float value) {
  constexpr size_t TEXT_BUFFER_SIZE = 32;
  char text[TEXT_BUFFER_SIZE] = {};
  const int written = std::snprintf(text, sizeof(text), "%.7g", static_cast<double>(value));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(text)) {
    return false;
  }

  return cJSON_AddRawToObject(object, key, text) != nullptr;
}

// Serialize one correction entry; emits "slr": null when no SLR params apply.
// `allow_epa` gates the pm25-only "useEpa2021" sub-key.
bool add_entry(cJSON *parent, const char *key, const std::optional<CorrectionEntry> &entry,
               bool allow_epa) {
  if (!entry.has_value()) {
    return true;
  }
  cJSON *obj = cJSON_CreateObject();
  if (obj == nullptr) {
    return false;
  }
  if (cJSON_AddStringToObject(obj, fields::CORRECTION_ALGORITHM, entry->algorithm.c_str()) ==
      nullptr) {
    cJSON_Delete(obj);
    return false;
  }
  if (entry->slr.has_value()) {
    cJSON *slr = cJSON_CreateObject();
    if (slr == nullptr || !add_float_to_object(slr, fields::INTERCEPT, *entry->slr->intercept) ||
        !add_float_to_object(slr, fields::SCALING_FACTOR, *entry->slr->scaling_factor) ||
        (allow_epa && entry->slr->use_epa2021.has_value() &&
         cJSON_AddBoolToObject(slr, fields::USE_EPA2021, *entry->slr->use_epa2021) == nullptr) ||
        !cJSON_AddItemToObject(obj, fields::SLR, slr)) {
      cJSON_Delete(slr);
      cJSON_Delete(obj);
      return false;
    }
  } else if (cJSON_AddNullToObject(obj, fields::SLR) == nullptr) {
    cJSON_Delete(obj);
    return false;
  }
  if (!cJSON_AddItemToObject(parent, key, obj)) {
    cJSON_Delete(obj);
    return false;
  }
  return true;
}

} // namespace

ParseResult parse(const char *body, size_t len, LocalServerConfig &out) {
  ParseResult result;
  if (body == nullptr || len == 0) {
    result.status = ParseStatus::InvalidBody;
    return result;
  }

  const char *parse_end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(body, len, &parse_end, /*require_null_terminated=*/0);
  if (root == nullptr) {
    result.status = ParseStatus::InvalidBody;
    return result;
  }

  // Reject a non-object root (array, string, number, etc.).
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    result.status = ParseStatus::InvalidBody;
    return result;
  }

  // Reject trailing non-whitespace after the root object.
  const char *end = body + len;
  for (const char *p = parse_end; p != nullptr && p < end; ++p) {
    if (!is_whitespace(*p)) {
      cJSON_Delete(root);
      result.status = ParseStatus::InvalidBody;
      return result;
    }
  }

  for (const cJSON *item = root->child; item != nullptr; item = item->next) {
    ConfigFieldId field = ConfigFieldId::None;
    const ParseStatus status = apply_item(item, out, field, result.unknown_key);
    if (status == ParseStatus::UnknownField) {
      result.status = ParseStatus::UnknownField;
      cJSON_Delete(root);
      return result;
    }
    if (status == ParseStatus::InvalidValue) {
      result.status = ParseStatus::InvalidValue;
      result.field = field;
      cJSON_Delete(root);
      return result;
    }
  }

  cJSON_Delete(root);
  result.status = ParseStatus::Ok;
  return result;
}

size_t serialize(const LocalServerConfig &cfg, char *buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) {
    return 0;
  }
  if (cfg.corrections.has_value() && (has_incomplete_slr(cfg.corrections->pm25) ||
                                      has_incomplete_slr(cfg.corrections->temperature) ||
                                      has_incomplete_slr(cfg.corrections->humidity))) {
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return 0;
  }
  if (cfg.country.has_value()) {
    cJSON_AddStringToObject(root, fields::COUNTRY, cfg.country->c_str());
  }
  if (cfg.pm_standard.has_value()) {
    cJSON_AddStringToObject(root, fields::PM_STANDARD, cfg.pm_standard->c_str());
  }
  if (cfg.temperature_unit.has_value()) {
    cJSON_AddStringToObject(root, fields::TEMPERATURE_UNIT, cfg.temperature_unit->c_str());
  }
  if (cfg.altitude_unit.has_value()) {
    cJSON_AddStringToObject(root, fields::ALTITUDE_UNIT, cfg.altitude_unit->c_str());
  }
  if (cfg.post_data_to_cloud.has_value()) {
    cJSON_AddBoolToObject(root, fields::POST_DATA_TO_CLOUD, *cfg.post_data_to_cloud);
  }
  if (cfg.cloud_connection.has_value()) {
    cJSON_AddBoolToObject(root, fields::CLOUD_CONNECTION, *cfg.cloud_connection);
  }
  if (cfg.configuration_control.has_value()) {
    cJSON_AddStringToObject(root, fields::CONFIGURATION_CONTROL,
                            cfg.configuration_control->c_str());
  }
  if (cfg.measurement_interval_seconds.has_value()) {
    cJSON_AddNumberToObject(root, fields::MEASUREMENT_INTERVAL,
                            static_cast<double>(*cfg.measurement_interval_seconds));
  }
  if (cfg.gps_mode.has_value()) {
    cJSON_AddStringToObject(root, fields::GPS_MODE, cfg.gps_mode->c_str());
  }
  if (cfg.front_led_brightness.has_value()) {
    cJSON_AddNumberToObject(root, fields::FRONT_LED_BRIGHTNESS,
                            static_cast<double>(*cfg.front_led_brightness));
  }
  if (cfg.back_led_brightness.has_value()) {
    cJSON_AddNumberToObject(root, fields::BACK_LED_BRIGHTNESS,
                            static_cast<double>(*cfg.back_led_brightness));
  }
  if (cfg.touch_led_intensity.has_value()) {
    cJSON_AddNumberToObject(root, fields::TOUCH_LED_INTENSITY,
                            static_cast<double>(*cfg.touch_led_intensity));
  }
  if (cfg.buzzer_enabled.has_value()) {
    cJSON_AddBoolToObject(root, fields::BUZZER_ENABLED, *cfg.buzzer_enabled);
  }
  if (cfg.co2_abc_days.has_value()) {
    cJSON_AddNumberToObject(root, fields::CO2_ABC_DAYS, static_cast<double>(*cfg.co2_abc_days));
  }
  if (cfg.tvoc_learning_offset.has_value()) {
    cJSON_AddNumberToObject(root, fields::TVOC_LEARNING_OFFSET,
                            static_cast<double>(*cfg.tvoc_learning_offset));
  }
  if (cfg.nox_learning_offset.has_value()) {
    cJSON_AddNumberToObject(root, fields::NOX_LEARNING_OFFSET,
                            static_cast<double>(*cfg.nox_learning_offset));
  }
  if (cfg.led_mode.has_value()) {
    cJSON_AddStringToObject(root, fields::LED_MODE, cfg.led_mode->c_str());
  }
  if (cfg.led_bar_brightness.has_value()) {
    cJSON_AddNumberToObject(root, fields::LED_BAR_BRIGHTNESS,
                            static_cast<double>(*cfg.led_bar_brightness));
  }
  if (cfg.display_brightness.has_value()) {
    cJSON_AddNumberToObject(root, fields::DISPLAY_BRIGHTNESS,
                            static_cast<double>(*cfg.display_brightness));
  }
  if (cfg.mqtt_broker_url.has_value()) {
    cJSON_AddStringToObject(root, fields::MQTT_BROKER_URL, cfg.mqtt_broker_url->c_str());
  }
  if (cfg.http_domain.has_value()) {
    cJSON_AddStringToObject(root, fields::HTTP_DOMAIN, cfg.http_domain->c_str());
  }
  if (cfg.corrections.has_value()) {
    cJSON *corr = cJSON_CreateObject();
    if (corr == nullptr ||
        !add_entry(corr, fields::PM25, cfg.corrections->pm25, /*allow_epa=*/true) ||
        !add_entry(corr, fields::TEMPERATURE, cfg.corrections->temperature, /*allow_epa=*/false) ||
        !add_entry(corr, fields::HUMIDITY, cfg.corrections->humidity, /*allow_epa=*/false) ||
        !cJSON_AddItemToObject(root, fields::CORRECTIONS, corr)) {
      cJSON_Delete(corr);
      cJSON_Delete(root);
      return 0;
    }
  }

  const bool ok = cJSON_PrintPreallocated(root, buf, static_cast<int>(buf_len), /*format=*/0);
  cJSON_Delete(root);
  if (!ok) {
    return 0;
  }
  return std::strlen(buf);
}

const char *config_field_wire_key(ConfigFieldId id) {
  switch (id) {
  case ConfigFieldId::CountryCode:
    return fields::COUNTRY;
  case ConfigFieldId::PmStandard:
    return fields::PM_STANDARD;
  case ConfigFieldId::TemperatureUnit:
    return fields::TEMPERATURE_UNIT;
  case ConfigFieldId::PostDataToCloud:
    return fields::POST_DATA_TO_CLOUD;
  case ConfigFieldId::CloudConnection:
    return fields::CLOUD_CONNECTION;
  case ConfigFieldId::ConfigurationControl:
    return fields::CONFIGURATION_CONTROL;
  case ConfigFieldId::MeasurementInterval:
    return fields::MEASUREMENT_INTERVAL;
  case ConfigFieldId::GpsMode:
    return fields::GPS_MODE;
  case ConfigFieldId::FrontLedBrightness:
    return fields::FRONT_LED_BRIGHTNESS;
  case ConfigFieldId::BackLedBrightness:
    return fields::BACK_LED_BRIGHTNESS;
  case ConfigFieldId::TouchLedIntensity:
    return fields::TOUCH_LED_INTENSITY;
  case ConfigFieldId::BuzzerEnabled:
    return fields::BUZZER_ENABLED;
  case ConfigFieldId::Co2AbcDays:
    return fields::CO2_ABC_DAYS;
  case ConfigFieldId::TvocLearningOffset:
    return fields::TVOC_LEARNING_OFFSET;
  case ConfigFieldId::NoxLearningOffset:
    return fields::NOX_LEARNING_OFFSET;
  case ConfigFieldId::LedMode:
    return fields::LED_MODE;
  case ConfigFieldId::LedBarBrightness:
    return fields::LED_BAR_BRIGHTNESS;
  case ConfigFieldId::DisplayBrightness:
    return fields::DISPLAY_BRIGHTNESS;
  case ConfigFieldId::MqttBrokerUrl:
    return fields::MQTT_BROKER_URL;
  case ConfigFieldId::HttpDomain:
    return fields::HTTP_DOMAIN;
  case ConfigFieldId::Corrections:
    return fields::CORRECTIONS;
  case ConfigFieldId::CorrectionsPm25:
    return fields::CORRECTIONS_PM25;
  case ConfigFieldId::CorrectionsTemperature:
    return fields::CORRECTIONS_TEMPERATURE;
  case ConfigFieldId::CorrectionsHumidity:
    return fields::CORRECTIONS_HUMIDITY;
  case ConfigFieldId::AltitudeUnit:
    return fields::ALTITUDE_UNIT;
  case ConfigFieldId::None:
    return nullptr;
  }
  return nullptr;
}

} // namespace config_json
