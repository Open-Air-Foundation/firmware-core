/**
 * AirGradient Go — Offline BLE measurement payload
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_ble_advertisement.h"

#include <algorithm>
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

struct V2Field {
  unsigned bit;
  unsigned width;
};

constexpr V2Field V2_VERSION{0, 3};
constexpr V2Field V2_TEMPERATURE{3, 11};
constexpr V2Field V2_HUMIDITY{14, 7};
constexpr V2Field V2_PM25{21, 14};
constexpr V2Field V2_CO2{35, 14};
constexpr V2Field V2_PRESSURE{49, 13};
constexpr V2Field V2_BATTERY{62, 7};
constexpr double V2_TEMPERATURE_OFFSET_C = 40.0;
constexpr double V2_TEMPERATURE_SCALE = 10.0;
constexpr double V2_PRESSURE_OFFSET_HPA = -300.0;

constexpr uint32_t invalid_value(V2Field field) { return (1u << field.width) - 1; }

// Scales value + offset; out-of-range, invalid or nonfinite values use the
// field's all-ones sentinel. Round to nearest, ties away from zero.
uint32_t encode_v2_field(V2Field field, double value, bool valid, double offset, double scale) {
  const uint32_t invalid = invalid_value(field);
  if (!valid || !std::isfinite(value)) {
    return invalid;
  }
  const double rounded = std::round((value + offset) * scale);
  if (rounded < 0 || rounded >= invalid) {
    return invalid;
  }
  return static_cast<uint32_t>(rounded);
}

void put_bits(PayloadV2 &payload, V2Field field, uint32_t value) {
  for (unsigned i = 0; i < field.width; ++i) {
    if ((value >> i) & 1u) {
      const unsigned bit = field.bit + i;
      payload[bit / BITS_PER_BYTE] |= static_cast<uint8_t>(1u << (bit % BITS_PER_BYTE));
    }
  }
}

} // namespace

PayloadV2 encode_v2(const MeasuresAGo &m) {
  PayloadV2 payload{};
  put_bits(payload, V2_VERSION, VERSION_V2);
  put_bits(payload, V2_TEMPERATURE,
           encode_v2_field(V2_TEMPERATURE, m.temp_hum_a.temperature, m.temp_hum_a.is_temp_valid(),
                           V2_TEMPERATURE_OFFSET_C, V2_TEMPERATURE_SCALE));
  put_bits(payload, V2_HUMIDITY,
           encode_v2_field(V2_HUMIDITY, m.temp_hum_a.humidity, m.temp_hum_a.is_hum_valid(), 0,
                           WHOLE_UNIT_SCALE));
  put_bits(payload, V2_PM25,
           encode_v2_field(V2_PM25, m.pm_a.pm_25, m.pm_a.is_pm_25_valid(), 0, PM25_SCALE));
  put_bits(payload, V2_CO2,
           encode_v2_field(V2_CO2, m.co2.co2, m.co2.is_valid(), 0, WHOLE_UNIT_SCALE));
  put_bits(payload, V2_PRESSURE,
           encode_v2_field(V2_PRESSURE, m.pressure.pressure, m.pressure.is_pressure_valid(),
                           V2_PRESSURE_OFFSET_HPA, PRESSURE_SCALE));
  put_bits(payload, V2_BATTERY,
           encode_v2_field(V2_BATTERY, m.power.battery_percentage,
                           m.power.is_battery_percentage_valid(), 0, WHOLE_UNIT_SCALE));
  return payload;
}

ManufacturerDataV2 manufacturer_data_v2(const PayloadV2 &payload) {
  ManufacturerDataV2 data{};
  data[0] = static_cast<uint8_t>(COMPANY_ID);
  data[1] = static_cast<uint8_t>(COMPANY_ID >> BITS_PER_BYTE);
  std::copy(payload.begin(), payload.end(), data.begin() + COMPANY_ID_SIZE);
  return data;
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
  const size_t payload_size = std::min<size_t>(report.payload_size, PAYLOAD_SIZE);
  for (size_t i = 0; i < payload_size; ++i) {
    std::snprintf(payload + i * 2, 3, "%02x", report.payload[i]);
  }
  std::snprintf(buffer, size,
                "seq=%" PRIu32 " outcome=%s failed_step=%s adv_start_ms=%" PRIu32
                " adv_duration_ms=%" PRIu32 " awake_ms=%" PRIu32 " payload=%s",
                report.sequence, outcome_name(report.outcome), step_name(report.failed_step),
                report.advertising_started_ms, report.advertising_duration_ms, report.awake_ms,
                payload);
}

} // namespace GoBleAdvertisement
