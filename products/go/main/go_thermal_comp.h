/**
 * @file go_thermal_comp.h
 * @brief SHT thermal-offset compensation for the Go enclosure
 *
 * Board heat (charger/battery plus a constant electronics floor) conducts
 * along the PCB into the SHT tab and inflates its reading. The DPS368
 * barometer's die temperature is the observable of that internal heat:
 *
 *   T_sht = T_amb + ALPHA * (T_board - T_amb) + INTERCEPT_C
 *
 * The DPS368 die temperature is the observable rather than the battery NTC:
 * the NTC sits on the battery pack, so its offset against the board varies
 * by up to 1.1 degC between units with pack seating and charger-heat
 * coupling, and that spread would reach the corrected value amplified by
 * 1/(1-ALPHA). The factory-trimmed DPS is on the PCB and needs no per-unit
 * calibration.
 *
 * ALPHA is the SHT's share of the board excess and depends on where the heat
 * is generated: with external power the charger dissipates next to the SHT
 * tab, on battery the ESP/PM floor dominates. Measured on four units against
 * a continuous ambient reference:
 *   battery   0.522 / 0.493 / 0.477 / 0.485  -> ALPHA_BATTERY 0.49
 *   external  0.545 / 0.514 / 0.531 / 0.544  -> ALPHA_EXTERNAL_POWER 0.53
 * The intercept comes from the 10.8 h single-unit fit (quasi-steady
 * filtered, R^2 = 0.898, residual RMS = 75 mK); a 10-min-bucket fit and a
 * charge-step experiment reproduce the still-air ALPHA within 3%.
 * Unit-to-unit spread within a regime is ~0.02, i.e. about +-0.3 degC of
 * residual at a typical 7 degC board excess. Constants are enclosure
 * properties of the assembled Go and assume the dedicated SHT is the
 * temp/hum source (not a fallback sensor).
 *
 * ALPHA and the intercept describe natural convection around the running
 * device in thermal equilibrium: readings in the first ~10 min after a cold
 * boot are mis-corrected until the thermal floor establishes, a power-source
 * change over-reads by up to ~0.7 degC for ~30 min while the DPS catches up
 * with the SHT, and forced airflow invalidates the correction entirely (see
 * go_airflow_detect.h).
 *
 * Host-testable: no ESP-IDF dependencies.
 */

#pragma once

#include <cmath>

namespace thermal_comp {

constexpr float ALPHA_BATTERY = 0.49f;
constexpr float ALPHA_EXTERNAL_POWER = 0.53f;
constexpr float INTERCEPT_C = -0.13f;

inline float alpha_for_power_source(bool external_power) {
  return external_power ? ALPHA_EXTERNAL_POWER : ALPHA_BATTERY;
}

inline float correct_temperature(float sht_c, float board_c, float alpha) {
  return (sht_c - alpha * board_c - INTERCEPT_C) / (1.0f - alpha);
}

/// Magnus-form saturation vapour pressure (hPa).
inline float saturation_pressure_hpa(float t_c) {
  return 6.112f * expf(17.62f * t_c / (243.12f + t_c));
}

/// Re-express relative humidity at the corrected temperature: the absolute
/// water content is unchanged, so RH scales by the saturation-pressure ratio.
inline float correct_humidity(float rh_raw, float t_raw_c, float t_corrected_c) {
  const float rh =
      rh_raw * saturation_pressure_hpa(t_raw_c) / saturation_pressure_hpa(t_corrected_c);
  if (rh > 100.0f) {
    return 100.0f;
  }
  return rh < 0.0f ? 0.0f : rh;
}

} // namespace thermal_comp
