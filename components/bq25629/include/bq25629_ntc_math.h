/**
 * @file bq25629_ntc_math.h
 * @brief Pure conversion math for the BQ25629 TS pin NTC network
 *
 * Host-testable: no ESP-IDF dependencies. The charger reports only the TS
 * pin voltage as a percentage of its bias rail; everything in °C is derived
 * here and must match the resistor network on the board.
 */

#pragma once

#include <cmath>
#include <cstdint>

namespace drivers {
namespace ntc_math {

// TS divider network: RT1 pull-up from the bias rail to TS, RT2 pull-down
// from TS to GND, NTC (KNTC0805/10KF) in parallel with RT2.
constexpr float RT1_OHM = 4120.0f;
constexpr float RT2_OHM = 17330.0f;
constexpr float NTC_R25_OHM = 10000.0f;
constexpr float NTC_B_KELVIN = 3950.0f;

constexpr float TS_ADC_PERCENT_PER_LSB = 0.0961f;
constexpr float INVALID_TEMPERATURE_C = -999.0f;

struct NtcConversion {
  bool valid;
  float resistance_ohm;
  float temperature_c;
};

inline float ts_raw_to_percent(uint16_t raw) {
  return (raw & 0x0FFF) * TS_ADC_PERCENT_PER_LSB;
}

/**
 * @brief Convert a TS pin percentage to NTC resistance and temperature
 *
 * Voltage divider: V_TS = V_BIAS * (RT2 || R_NTC) / (RT1 + (RT2 || R_NTC))
 * with ratio = TS% / 100, solved for R_NTC:
 *   R_parallel = (ratio * RT1) / (1 - ratio)
 *   R_NTC = (R_parallel * RT2) / (RT2 - R_parallel)
 * then Steinhart-Hart (B-parameter form):
 *   1/T = 1/298.15K + ln(R_NTC / R25) / B
 *
 * Returns valid=false (resistance 0, temperature INVALID_TEMPERATURE_C) when
 * the ratio is outside the physically possible divider range — including any
 * reading at or above RT2 / (RT1 + RT2), where no NTC value can produce it.
 */
inline NtcConversion convert_ts_percent(float ts_percent) {
  const float adc_ratio = ts_percent / 100.0f;

  if (adc_ratio <= 0.001f || adc_ratio >= 0.999f) {
    return {false, 0.0f, INVALID_TEMPERATURE_C};
  }

  const float r_parallel = (adc_ratio * RT1_OHM) / (1.0f - adc_ratio);
  if (r_parallel >= RT2_OHM || r_parallel < 0.0f) {
    return {false, 0.0f, INVALID_TEMPERATURE_C};
  }

  const float r_ntc = (r_parallel * RT2_OHM) / (RT2_OHM - r_parallel);
  const float t_kelvin =
      1.0f / ((1.0f / 298.15f) + (logf(r_ntc / NTC_R25_OHM) / NTC_B_KELVIN));
  return {true, r_ntc, t_kelvin - 273.15f};
}

} // namespace ntc_math
} // namespace drivers
