/**
 * AirGradient Go — Offline BLE measurement payload
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_ble_advertisement.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>

// RTC_DATA_ATTR keeps the wake report across deep sleep; host tests define it away.
#if !defined(TEST_HOST) && defined(__has_include)
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#endif

#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif

namespace GoBleAdvertisement {
namespace {

constexpr double TEMPERATURE_SCALE = 100.0;
constexpr double PM25_SCALE = 10.0;
constexpr double PRESSURE_SCALE = 10.0;
constexpr double WHOLE_UNIT_SCALE = 1.0;
constexpr int32_t MIN_I16_VALUE = -32767; // -32768 is the invalid sentinel.
constexpr int32_t MAX_I16_VALUE = 32767;
constexpr int32_t MAX_U16_VALUE = INVALID_U16 - 1;
constexpr unsigned BITS_PER_BYTE = 8;

uint16_t encode_scaled(double value, bool valid, double scale, int32_t min_value, int32_t max_value,
                       uint16_t invalid) {
  if (!valid || !std::isfinite(value)) {
    return invalid;
  }
  const double rounded = std::round(value * scale);
  if (rounded < min_value || rounded > max_value) {
    return invalid;
  }
  return static_cast<uint16_t>(static_cast<int32_t>(rounded));
}

void write_u16(Payload &payload, size_t offset, uint16_t value) {
  payload[offset] = static_cast<uint8_t>(value);
  payload[offset + 1] = static_cast<uint8_t>(value >> BITS_PER_BYTE);
}

} // namespace

Payload encode(const MeasuresAGo &m) {
  Payload payload;
  payload.fill(INVALID_U8);
  payload[VERSION_OFFSET] = VERSION;
  write_u16(payload, TEMPERATURE_OFFSET,
            encode_scaled(m.temp_hum_a.temperature, m.temp_hum_a.is_temp_valid(), TEMPERATURE_SCALE,
                          MIN_I16_VALUE, MAX_I16_VALUE, INVALID_I16));
  payload[HUMIDITY_OFFSET] =
      static_cast<uint8_t>(encode_scaled(m.temp_hum_a.humidity, m.temp_hum_a.is_hum_valid(),
                                         WHOLE_UNIT_SCALE, 0, INVALID_U8 - 1, INVALID_U8));
  write_u16(payload, PM25_OFFSET,
            encode_scaled(m.pm_a.pm_25, m.pm_a.is_pm_25_valid(), PM25_SCALE, 0, MAX_U16_VALUE,
                          INVALID_U16));
  write_u16(
      payload, CO2_OFFSET,
      encode_scaled(m.co2.co2, m.co2.is_valid(), WHOLE_UNIT_SCALE, 0, MAX_U16_VALUE, INVALID_U16));
  write_u16(payload, PRESSURE_OFFSET,
            encode_scaled(m.pressure.pressure, m.pressure.is_pressure_valid(), PRESSURE_SCALE, 0,
                          MAX_U16_VALUE, INVALID_U16));
  write_u16(payload, ALTITUDE_OFFSET,
            encode_scaled(m.pressure.altitude, m.pressure.is_altitude_valid(), WHOLE_UNIT_SCALE,
                          MIN_I16_VALUE, MAX_I16_VALUE, INVALID_I16));
  payload[BATTERY_OFFSET] = static_cast<uint8_t>(
      encode_scaled(m.power.battery_percentage, m.power.is_battery_percentage_valid(),
                    WHOLE_UNIT_SCALE, 0, INVALID_U8 - 1, INVALID_U8));
  return payload;
}

namespace {

RTC_DATA_ATTR WakeReport s_wake_report;
RTC_DATA_ATTR bool s_wake_report_valid = false;
RTC_DATA_ATTR uint32_t s_wake_report_sequence = 0;

const char *outcome_name(WakeOutcome outcome) {
  switch (outcome) {
  case WakeOutcome::Advertised:
    return "advertised";
  case WakeOutcome::SkippedButton:
    return "skipped-button";
  case WakeOutcome::SkippedSleepTooShort:
    return "skipped-sleep-too-short";
  case WakeOutcome::SkippedShipMode:
    return "skipped-ship-mode";
  case WakeOutcome::SkippedPromotion:
    return "skipped-promotion";
  case WakeOutcome::SetupFailed:
    return "setup-failed";
  case WakeOutcome::StopFailed:
    return "stop-failed";
  }
  return "unknown";
}

const char *step_name(SetupStep step) {
  switch (step) {
  case SetupStep::None:
    return "none";
  case SetupStep::Init:
    return "init";
  case SetupStep::Payload:
    return "payload";
  case SetupStep::Interval:
    return "interval";
  case SetupStep::Start:
    return "start";
  }
  return "unknown";
}

} // namespace

void store_wake_report(WakeReport report) {
  report.sequence = ++s_wake_report_sequence;
  s_wake_report = report;
  s_wake_report_valid = true;
}

bool take_wake_report(WakeReport &out) {
  if (!s_wake_report_valid) {
    return false;
  }
  out = s_wake_report;
  s_wake_report_valid = false;
  return true;
}

void format_wake_report(const WakeReport &report, char *buffer, size_t size) {
  if (buffer == nullptr || size == 0) {
    return;
  }
  char payload[PAYLOAD_SIZE * 2 + 1] = "none";
  if (report.has_payload) {
    for (size_t i = 0; i < PAYLOAD_SIZE; ++i) {
      std::snprintf(payload + i * 2, 3, "%02x", report.payload[i]);
    }
  }
  std::snprintf(buffer, size,
                "seq=%" PRIu32 " outcome=%s failed_step=%s adv_start_ms=%" PRIu32
                " adv_duration_ms=%" PRIu32 " awake_ms=%" PRIu32 " payload=%s",
                report.sequence, outcome_name(report.outcome), step_name(report.failed_step),
                report.advertising_started_ms, report.advertising_duration_ms, report.awake_ms,
                payload);
}

} // namespace GoBleAdvertisement
