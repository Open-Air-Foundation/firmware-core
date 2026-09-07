/**
 * @file go_airflow_detect.h
 * @brief External-airflow detection from the SHT's own short-term jitter
 *
 * Moving air strips the boundary layer at the sensor face and flutters the
 * reading on second timescales; still air does not. The jitter of a window
 * of successive 10 s samples (population stdev of first differences — a
 * high-pass that discards slow drift) therefore separates the convection
 * regimes that the thermal compensation in go_thermal_comp.h depends on.
 *
 * Thresholds calibrated 2026-08-28 on ONE unit at the 10 s cadence: still
 * air never exceeded 27 mK across 65 two-minute windows (median 16 mK, both
 * idle and charging); a direct desk fan ran 24–72 mK (median 41). The gap
 * 27–41 mK (gentle drafts) is uncharacterized, and per-unit sensor noise
 * floors may differ — revisit with multi-unit data before fleet defaults.
 *
 * Entry and exit are deliberately asymmetric: a false positive would discard
 * a correct compensation (+2.4…+3.8 degC error), while a missed detection
 * only delays flagging an error that itself develops with tau ~12–21 min.
 *
 * Host-testable: no ESP-IDF dependencies.
 */

#pragma once

#include <cmath>

namespace airflow_detect {

constexpr float ENTER_JITTER_MK = 30.0f; ///< window >= this counts toward raising
constexpr int ENTER_WINDOWS = 2;         ///< consecutive loud windows to raise (~4 min)
constexpr float EXIT_JITTER_MK = 22.0f;  ///< window <= this counts toward clearing
constexpr int EXIT_WINDOWS = 5;          ///< consecutive quiet windows to clear (~10 min,
                                         ///< matching the measured thermal recovery time)
constexpr int WINDOW_DIFFS = 11;         ///< first differences per window (~2 min at 10 s)

// The jitter statistics above are defined at the 10 s cadence; a pair of
// samples spanning a sensor hiccup or reboot is not comparable and is dropped.
constexpr float MIN_GAP_S = 5.0f;
constexpr float MAX_GAP_S = 25.0f;

class Detector {
public:
  /// Feed every RAW (uncompensated) SHT temperature sample as it arrives.
  void add_sample(float sht_raw_c, float uptime_s) {
    if (_has_prev) {
      const float gap_s = uptime_s - _prev_time_s;
      if (gap_s >= MIN_GAP_S && gap_s <= MAX_GAP_S) {
        const float diff = sht_raw_c - _prev_c;
        _sum += diff;
        _sum_sq += diff * diff;
        if (++_count >= WINDOW_DIFFS) {
          close_window();
        }
      }
    }
    _prev_c = sht_raw_c;
    _prev_time_s = uptime_s;
    _has_prev = true;
  }

  /// True while sustained external airflow is detected.
  bool airflow() const { return _airflow; }

  /// Jitter of the most recently completed window; negative until one exists.
  float last_window_jitter_mk() const { return _last_jitter_mk; }

private:
  void close_window() {
    const float mean = _sum / static_cast<float>(_count);
    const float variance = _sum_sq / static_cast<float>(_count) - mean * mean;
    _last_jitter_mk = std::sqrt(variance < 0.0f ? 0.0f : variance) * 1000.0f;
    _sum = 0.0f;
    _sum_sq = 0.0f;
    _count = 0;
    if (_airflow) {
      // Windows between EXIT and ENTER are ambiguous and hold the streak
      // instead of resetting it: right after a fan stops, the re-warming
      // board stirs its own micro-convection and jitter hovers at the exit
      // boundary (field data 2026-08-31: two units pinned at ~22 mK for
      // several windows, stretching recovery from ~12 to ~18 min). Only a
      // window loud enough to raise the flag is evidence the wind continues.
      if (_last_jitter_mk <= EXIT_JITTER_MK) {
        ++_quiet_streak;
        if (_quiet_streak >= EXIT_WINDOWS) {
          _airflow = false;
          _quiet_streak = 0;
        }
      } else if (_last_jitter_mk >= ENTER_JITTER_MK) {
        _quiet_streak = 0;
      }
    } else {
      _loud_streak = (_last_jitter_mk >= ENTER_JITTER_MK) ? _loud_streak + 1 : 0;
      if (_loud_streak >= ENTER_WINDOWS) {
        _airflow = true;
        _loud_streak = 0;
      }
    }
  }

  float _prev_c = 0.0f;
  float _prev_time_s = 0.0f;
  bool _has_prev = false;
  float _sum = 0.0f;
  float _sum_sq = 0.0f;
  int _count = 0;
  float _last_jitter_mk = -1.0f;
  int _loud_streak = 0;
  int _quiet_streak = 0;
  bool _airflow = false;
};

} // namespace airflow_detect
