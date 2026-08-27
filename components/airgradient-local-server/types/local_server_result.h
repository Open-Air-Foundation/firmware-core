/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_RESULT_H
#define AG_LOCAL_SERVER_RESULT_H

#include <cstdint>

// Results are pointer-free: providers return only enums. The component owns
// and serializes all error strings (canonical field name + a standardized
// message per status). This removes any borrowed-string lifetime hazard — a
// provider can never accidentally return a stack/local pointer that dangles
// during error serialization.

// Whether the config resource is exposed and, if so, read-only or read-write.
enum class ConfigAccess : uint8_t { Disabled, ReadOnly, ReadWrite };

// Mirrors the config catalog; the component maps each id to its canonical
// camelCase wire key (e.g. CountryCode -> "country", TemperatureUnit ->
// "temperatureUnit") when building an error body. The nested corrections
// entries map to dotted keys (e.g. CorrectionsPm25 -> "corrections.pm25").
// None is used when no specific field applies.
enum class ConfigFieldId : uint8_t {
  None,
  CountryCode,            // "country"
  PmStandard,             // "pmStandard"
  TemperatureUnit,        // "temperatureUnit"
  PostDataToCloud,        // "postDataToCloud"
  CloudConnection,        // "cloudConnection"
  ConfigurationControl,   // "configurationControl"
  Co2AbcDays,             // "co2AbcDays"
  TvocLearningOffset,     // "tvocLearningOffset"
  NoxLearningOffset,      // "noxLearningOffset"
  LedMode,                // "ledMode"
  LedBarBrightness,       // "ledBarBrightness"
  DisplayBrightness,      // "displayBrightness"
  MqttBrokerUrl,          // "mqttBrokerUrl"
  HttpDomain,             // "httpDomain"
  Corrections,            // "corrections" (whole object)
  CorrectionsPm25,        // "corrections.pm25"
  CorrectionsTemperature, // "corrections.temperature"
  CorrectionsHumidity,    // "corrections.humidity"
  MeasurementInterval,    // "measurementInterval"
  GpsMode,                // "gpsMode"
  FrontLedBrightness,     // "frontLedBrightness"
  BackLedBrightness,      // "backLedBrightness"
  TouchLedIntensity,      // "touchLedIntensity"
  BuzzerEnabled,          // "buzzerEnabled"
  AltitudeUnit,           // "altitudeUnit"
};

enum class ConfigSubmitStatus : uint8_t {
  Accepted,     // validated and admitted for later processing -> 202
  InvalidValue, // out of range / bad enum (semantic)           -> 400
  Forbidden,    // configuration_control gate / policy          -> 403
  NotSupported, // field not supported on this model            -> 404
  Busy,         // temporary admission / queue saturation       -> 503
  Internal,     // unexpected provider failure                  -> 500
};

struct ConfigSubmitResult {
  ConfigSubmitStatus status = ConfigSubmitStatus::Internal;
  // The offending field for InvalidValue / NotSupported; None otherwise. The
  // component maps it to the canonical wire key in the error body.
  ConfigFieldId field = ConfigFieldId::None;
};

enum class ActionId : uint8_t { CalibrateCo2, TestLeds, TestGps };

enum class ActionStatus : uint8_t {
  Dispatched,   // accepted and queued (fire-and-forget) -> 200
  Rejected,     // policy / state gate                   -> 403
  NotSupported, // action not available on this model    -> 404
  Busy,         // temporary admission / queue saturation -> 503
};

struct ActionResult {
  ActionStatus status = ActionStatus::NotSupported;
};

#endif // AG_LOCAL_SERVER_RESULT_H
