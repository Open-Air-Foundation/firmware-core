/**
 * @file go_thermal_comp.h
 * @brief SHT thermal-offset compensation for the Go enclosure
 *
 * Board heat (battery/charger plus a constant electronics floor) conducts
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
 * calibration. Fit from a 10.8 h continuous-reference dataset, quasi-steady
 * filtered: R^2 = 0.898, residual RMS = 75 mK; a 10-min-bucket fit
 * reproduces ALPHA within 3%, and a charge-step experiment reproduces it
 * independently within 3%. Constants are enclosure properties of the
 * assembled Go and assume the dedicated SHT is the temp/hum source (not a
 * fallback sensor).
 *
 * Both ALPHA and the intercept are properties of natural convection around
 * the running device: readings in the first ~10 min after a cold boot are
 * mis-corrected until the thermal floor establishes, and forced airflow
 * invalidates the correction entirely (see go_airflow_detect.h).
 *
 * Host-testable: no ESP-IDF dependencies.
 */

#pragma once

#include <cmath>

namespace thermal_comp {

constexpr float ALPHA = 0.522f;
constexpr float INTERCEPT_C = -0.13f;

inline float correct_temperature(float sht_c, float board_c) {
  return (sht_c - ALPHA * board_c - INTERCEPT_C) / (1.0f - ALPHA);
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
