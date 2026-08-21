/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_FIELD_NAMES_H
#define AG_LOCAL_SERVER_FIELD_NAMES_H

// Single source of truth for the camelCase JSON field names used by the
// measures and config payloads. Serialization, parsing, and error-field
// mapping all reference these, so a key cannot drift between the read and
// write paths. Host tests intentionally assert the literal strings instead,
// so a rename here is caught by the contract tests rather than hidden.
//
// Enum value strings (e.g. "ugm3", "c") stay local to config_json.cpp: they
// are single-use value sets, not field names.
namespace fields {

// --- Identity + measures (GET /api/v1/measures) --------------------------
inline constexpr const char *SERIAL_NUMBER = "serialNumber";
inline constexpr const char *MODEL = "model";
inline constexpr const char *FIRMWARE = "firmware";
inline constexpr const char *WIFI_RSSI = "wifiRssi";
inline constexpr const char *BOOT = "boot";
inline constexpr const char *CO2 = "co2";
inline constexpr const char *PM01 = "pm01";
inline constexpr const char *PM25 = "pm25";
inline constexpr const char *PM10 = "pm10";
inline constexpr const char *PM003_COUNT = "pm003Count";
inline constexpr const char *PM005_COUNT = "pm005Count";
inline constexpr const char *PM01_COUNT = "pm01Count";
inline constexpr const char *PM02_COUNT = "pm02Count";
inline constexpr const char *PM50_COUNT = "pm50Count";
inline constexpr const char *PM10_COUNT = "pm10Count";
inline constexpr const char *TEMPERATURE = "temperature";
inline constexpr const char *HUMIDITY = "humidity";
inline constexpr const char *TVOC_INDEX = "tvocIndex";
inline constexpr const char *TVOC_RAW = "tvocRaw";
inline constexpr const char *NOX_INDEX = "noxIndex";
inline constexpr const char *NOX_RAW = "noxRaw";
inline constexpr const char *BATT_PERCENT = "battPercent";
inline constexpr const char *BATT_VOLT = "battVolt";
inline constexpr const char *CHARGE_VOLT = "chargeVolt";

// --- Config catalog (GET / PUT /api/v1/config) ---------------------------
inline constexpr const char *COUNTRY = "country";
inline constexpr const char *PM_STANDARD = "pmStandard";
inline constexpr const char *TEMPERATURE_UNIT = "temperatureUnit";
inline constexpr const char *ALTITUDE_UNIT = "altitudeUnit";
inline constexpr const char *POST_DATA_TO_CLOUD = "postDataToCloud";
inline constexpr const char *CLOUD_CONNECTION = "cloudConnection";
inline constexpr const char *CONFIGURATION_CONTROL = "configurationControl";
inline constexpr const char *MEASUREMENT_INTERVAL = "measurementInterval";
inline constexpr const char *GPS_MODE = "gpsMode";
inline constexpr const char *FRONT_LED_BRIGHTNESS = "frontLedBrightness";
inline constexpr const char *BACK_LED_BRIGHTNESS = "backLedBrightness";
inline constexpr const char *TOUCH_LED_INTENSITY = "touchLedIntensity";
inline constexpr const char *BUZZER_ENABLED = "buzzerEnabled";
inline constexpr const char *CO2_ABC_DAYS = "co2AbcDays";
inline constexpr const char *TVOC_LEARNING_OFFSET = "tvocLearningOffset";
inline constexpr const char *NOX_LEARNING_OFFSET = "noxLearningOffset";
inline constexpr const char *LED_MODE = "ledMode";
inline constexpr const char *LED_BAR_BRIGHTNESS = "ledBarBrightness";
inline constexpr const char *DISPLAY_BRIGHTNESS = "displayBrightness";
inline constexpr const char *MQTT_BROKER_URL = "mqttBrokerUrl";
inline constexpr const char *HTTP_DOMAIN = "httpDomain";
inline constexpr const char *CORRECTIONS = "corrections";

// --- corrections sub-keys (inner measure keys reuse PM25 / TEMPERATURE / HUMIDITY)
inline constexpr const char *CORRECTION_ALGORITHM = "correctionAlgorithm";
inline constexpr const char *SLR = "slr";
inline constexpr const char *INTERCEPT = "intercept";
inline constexpr const char *SCALING_FACTOR = "scalingFactor";
inline constexpr const char *USE_EPA2021 = "useEpa2021";

// --- Dotted error-field keys for the nested corrections entries -----------
inline constexpr const char *CORRECTIONS_PM25 = "corrections.pm25";
inline constexpr const char *CORRECTIONS_TEMPERATURE = "corrections.temperature";
inline constexpr const char *CORRECTIONS_HUMIDITY = "corrections.humidity";

} // namespace fields

#endif // AG_LOCAL_SERVER_FIELD_NAMES_H
