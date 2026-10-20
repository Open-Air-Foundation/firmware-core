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
 * The DPS368 replaced the battery NTC as the observable on 2026-08-31. The
 * NTC sits on the battery pack, so its offset against the board differs by
 * up to 1.1 degC between units (pack seating / charger-heat coupling, not a
 * thermistor-part difference: the spread collapsed to <0.4 degC under
 * external heating in an oven run to 60 degC board temperature). The
 * factory-trimmed DPS is on the PCB and needs no per-unit calibration.
 *
 * ALPHA is the SHT's share of the board excess and depends on where the heat
 * is generated: with external power the charger dissipates next to the SHT
 * tab, on battery the ESP/PM floor dominates. Measured with a continuous
 * ambient reference (four units, 2026-08-27..09-01):
 *   battery   0.522 / 0.493 / 0.477 / 0.485  (mean 0.49)
 *   external  0.545 / 0.514 / 0.531 / 0.544  (mean 0.53)
 * The per-band constants below cover both regimes with one value.
 * The intercept comes from the 10.8 h single-unit fit (quasi-steady
 * filtered, R^2 = 0.898, residual RMS = 75 mK). Unit-to-unit spread within a
 * regime is ~0.03, i.e. about +-0.3 degC of residual at a typical 7 degC
 * board excess. Constants are enclosure properties of the assembled Go and
 * assume the dedicated SHT is the temp/hum source (not a fallback sensor).
 *
 * ALPHA and the intercept describe natural convection around the running
 * device in thermal equilibrium: readings in the first ~10 min after a cold
 * boot are mis-corrected until the thermal floor establishes, a power-source
 * change over-reads by up to ~0.7 degC for ~30 min while the DPS catches up
 * with the SHT, and forced airflow breaks the coupling the model assumes
 * (not detected: the band ALPHA applies regardless). ALPHA is selected from
 * three ambient bands (cold / normal / hot) with hysteresis.
 *
 * Host-testable: no ESP-IDF dependencies.
 */

#pragma once

#include <cmath>

namespace thermal_comp {

/// ALPHA is banded by estimated ambient temperature (cold / normal / hot).
/// One value per band for every unit: the minimax of the corrected-vs-ambient
/// bias over units A/C/D in still air (2026-08-31..09-04 soaks, AG Ambient
/// reference). NORMAL covers battery (0.48-0.51) and USB charging
/// (0.51-0.53) at 26-30 degC with |bias| <= 0.55 degC; HOT is the 36-38 degC
/// box (unit C excluded: over-corrects by ~2 degC, position in the box).
/// COLD equals NORMAL: in the 2026-09-07 fridge run the board excess over
/// the SHT collapsed to ~0 (DPS ~= NTC ~= SHT), so ALPHA has no leverage
/// there and the value only shapes the transition through the 10 degC edge.
constexpr float ALPHA_COLD = 0.50f;
constexpr float ALPHA_NORMAL = 0.50f;
constexpr float ALPHA_HOT = 0.525f;
constexpr float INTERCEPT_C = -0.13f;

/// Band edges on the *estimated ambient* (not the board temperature, which
/// carries self-heating). A band change at ALPHA_COLD != ALPHA_NORMAL steps
/// the output by (dAlpha/(1-ALPHA))*board_excess, so edges carry hysteresis
/// to keep the reading from chattering at a boundary.
constexpr float BAND_COLD_MAX_C = 10.0f;
constexpr float BAND_HOT_MIN_C = 35.0f;
constexpr float BAND_HYSTERESIS_C = 1.0f;

enum class ThermalBand { Cold, Normal, Hot };

inline const char *band_name(ThermalBand band) {
  switch (band) {
  case ThermalBand::Cold:
    return "COLD";
  case ThermalBand::Hot:
    return "HOT";
  case ThermalBand::Normal:
    break;
  }
  return "NORMAL";
}

inline float alpha_for_band(ThermalBand band) {
  switch (band) {
  case ThermalBand::Cold:
    return ALPHA_COLD;
  case ThermalBand::Hot:
    return ALPHA_HOT;
  case ThermalBand::Normal:
    break;
  }
  return ALPHA_NORMAL;
}

/// Sticky band selection: leaving a band requires crossing its edge plus the
/// hysteresis margin, so a reading sitting on an edge does not flip bands.
class BandSelector {
public:
  ThermalBand update(float ambient_estimate_c) {
    switch (_band) {
    case ThermalBand::Cold:
      if (ambient_estimate_c > BAND_COLD_MAX_C + BAND_HYSTERESIS_C) {
        _band = ThermalBand::Normal;
      }
      break;
    case ThermalBand::Hot:
      if (ambient_estimate_c < BAND_HOT_MIN_C - BAND_HYSTERESIS_C) {
        _band = ThermalBand::Normal;
      }
      break;
    case ThermalBand::Normal:
      if (ambient_estimate_c < BAND_COLD_MAX_C - BAND_HYSTERESIS_C) {
        _band = ThermalBand::Cold;
      } else if (ambient_estimate_c > BAND_HOT_MIN_C + BAND_HYSTERESIS_C) {
        _band = ThermalBand::Hot;
      }
      break;
    }
    return _band;
  }

  ThermalBand band() const { return _band; }

private:
  ThermalBand _band = ThermalBand::Normal;
};

/// The model describes heat flowing from the board into the SHT tab. When
/// the board reads cooler than the tab — cold boot before the DPS settles,
/// a hand-warmed tab, a device colder than the room — inverting it amplifies
/// the tab's excess instead of removing board heat (seen: raw 29.9 with the
/// board at 27.0 reported 33.4). The raw reading is the better estimate then.
inline bool board_heats_tab(float sht_c, float board_c) { return board_c > sht_c; }

/// Boot-time sanity check of the DPS die temperature against the battery NTC.
/// Both sit on the board within ~1.5 degC of each other on every good unit
/// (charging gradient included, up to 60 degC board); a DPS whose factory
/// coefficients are off reads a constant offset of several degC (seen: 7.5)
/// and would push the compensation by ALPHA times that. The verdict is taken
/// once, from the first sample where both readings are valid, and latched
/// until reboot so the user-facing value cannot flip mid-session.
constexpr float DPS_NTC_MAX_DIFF_C = 4.0f;

inline bool dps_plausible(float ntc_c, float dps_c) {
  return std::fabs(ntc_c - dps_c) <= DPS_NTC_MAX_DIFF_C;
}

class DpsCheck {
public:
  enum class State { Pending, Ok, Rejected };

  /// Feed a pair of valid readings; only the first pair decides.
  State update(float ntc_c, float dps_c) {
    if (_state == State::Pending) {
      _state = dps_plausible(ntc_c, dps_c) ? State::Ok : State::Rejected;
    }
    return _state;
  }
  State state() const { return _state; }
  bool rejected() const { return _state == State::Rejected; }

private:
  State _state = State::Pending;
};

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
