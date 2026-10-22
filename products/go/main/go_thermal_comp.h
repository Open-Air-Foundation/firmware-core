/**
 * @file go_thermal_comp.h
 * @brief SHT thermal-offset compensation for the Go enclosure
 *
 * Board heat (charger/battery plus a constant electronics floor) conducts
 * along the PCB into the SHT tab and inflates its reading. The battery NTC
 * (charger TS input) is the observable of that internal heat:
 *
 *   T_sht = T_amb + ALPHA * (T_board - T_amb) + INTERCEPT_C
 *
 * The NTC is used instead of the DPS368 die temperature because the DPS
 * depends on per-chip factory coefficients: one of four units read ~7 degC
 * low, which the inversion turns into an error of ALPHA/(1-ALPHA) times that.
 * The NTC fits all four units with one set of constants. Its cost is a
 * wider unit-to-unit spread (pack seating / charger-heat coupling).
 *
 * Fit against the AG Ambient reference, four units A-D, 2026-08-27..09-11,
 * 60 s buckets in still air, 15 min after boot and 45 min after a power
 * change excluded: ALPHA and the intercept are the minimax of the
 * corrected-vs-ambient median bias over every unit and power regime at
 * 26-31 degC (battery and USB) and in the 38 degC box (battery), worst
 * |bias| 1.2 degC. The best ALPHA per band at a fixed intercept is the same
 * for both (0.425 / 0.42), so one value covers them. Constants are enclosure
 * properties of the assembled Go and assume the dedicated SHT is the
 * temp/hum source (not a fallback sensor).
 *
 * Known limits: near 0 degC the board excess over the SHT collapses and the
 * model has no leverage (fridge run: +5.0..5.6 degC); the settle windows
 * above are not covered by the stated bias; forced airflow breaks the
 * coupling the model assumes (not detected).
 *
 * Host-testable: no ESP-IDF dependencies.
 */

#pragma once

#include <cmath>

namespace thermal_comp {

constexpr float ALPHA = 0.42f;
constexpr float INTERCEPT_C = 0.38f;

/// The model describes heat flowing from the board into the SHT tab. When
/// the board reads cooler than the tab — cold boot before the battery pack
/// warms, a hand-warmed tab, a device colder than the room — inverting it
/// amplifies the tab's excess instead of removing board heat. The raw
/// reading is the better estimate then.
inline bool board_heats_tab(float sht_c, float board_c) { return board_c > sht_c; }

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
