#include "shake_detector.h"

#include <algorithm>
#include <cstdlib>

bool ShakeDetector::valid_config(const Config &c) {
  return static_cast<unsigned>(c.axis) <= static_cast<unsigned>(ShakeAxis::Z) && c.release_mg > 0 &&
         c.peak_mg > c.release_mg && c.peak_mg < Config::MAX_PEAK_MG &&
         c.dominance_percent >= 100 && c.dominance_percent <= Config::MAX_DOMINANCE_PERCENT &&
         c.required_peaks >= 4 && c.min_peak_ms > 0 && c.min_peak_ms <= Config::MAX_WINDOW_MS &&
         c.max_peak_ms >= c.min_peak_ms && c.window_ms >= c.min_peak_ms * (c.required_peaks - 1) &&
         c.window_ms <= Config::MAX_WINDOW_MS && c.max_peak_ms <= c.window_ms &&
         c.max_sample_gap_ms > 0 && c.max_sample_gap_ms <= c.min_peak_ms &&
         c.quiet_lead_ms <= Config::MAX_WINDOW_MS && c.cooldown_ms <= Config::MAX_COOLDOWN_MS &&
         c.rearm_quiet_ms > 0 && c.rearm_quiet_ms <= Config::MAX_WINDOW_MS;
}

void ShakeDetector::reset_sequence() {
  _peaks = 0;
  _last_sign = 0;
  _zone = 0;
  _lead_ready = false;
}

void ShakeDetector::reset_capture() {
  reset_sequence();
  _have_sample = false;
  _quiet = false;
}

ShakeDetector::Result ShakeDetector::update(const AccelReading &sample, uint32_t now) {
  if (!valid_config(_config) || !sample.is_valid() || sample.clipped) {
    reset_capture();
    return Result::Invalid;
  }
  if (_have_sample && (now - _last_sample) > _config.max_sample_gap_ms) {
    reset_capture();
  }
  if (sample.overrun) {
    reset_capture(); // Missing samples cannot complete a sequence.
  }
  _have_sample = true;
  _last_sample = now;

  const int values[] = {sample.x_mg, sample.y_mg, sample.z_mg};
  const unsigned axis = static_cast<unsigned>(_config.axis);
  const int along = values[axis];
  const int amplitude = std::abs(along);
  const int across = std::max(std::abs(values[(axis + 1) % 3]), std::abs(values[(axis + 2) % 3]));
  const bool quiet = std::max(amplitude, across) <= _config.release_mg;
  if (quiet) {
    if (!_quiet) {
      _quiet_start = now;
    }
    _quiet = true;
    _last_quiet = now;
    if ((now - _quiet_start) >= _config.quiet_lead_ms) {
      _lead_ready = true;
    }
    if (_cooling && (now - _quiet_start) >= _config.rearm_quiet_ms) {
      _rearm_ready = true;
    }
  } else {
    _quiet = false;
  }

  if (_cooling) {
    if ((now - _last_shake) < _config.cooldown_ms || !_rearm_ready) {
      return Result::None;
    }
    _cooling = false;
  }

  bool expired = false;
  if (_peaks != 0 &&
      ((now - _last_peak) > _config.max_peak_ms || (now - _first_peak) > _config.window_ms)) {
    reset_sequence();
    expired = true;
  }
  if (amplitude <= _config.release_mg) {
    _zone = 0;
  }
  // Qualify each accepted peak, not every sample between peaks. Brief X/Z
  // spikes in a hand-held shake must not erase earlier qualified Y peaks.
  // A skipped peak cannot advance the sequence or extend its timeout.
  if (amplitude < _config.peak_mg || amplitude * 100 < across * _config.dominance_percent) {
    // Allow the gesture to ramp up after stillness, but expire old qualification.
    if (!quiet && _peaks == 0 && now - _last_quiet > _config.max_peak_ms) {
      _lead_ready = false;
    }
    if (expired) {
      return Result::Rejected;
    }
    return amplitude >= _config.peak_mg ? Result::OffAxis : Result::None;
  }

  const int sign = along > 0 ? 1 : -1;
  if (sign == _zone) {
    return Result::None; // A held peak counts only once.
  }
  _zone = sign;
  if (_peaks == 0) {
    if (_config.quiet_lead_ms != 0 && (!_lead_ready || now - _last_quiet > _config.max_peak_ms)) {
      return Result::None;
    }
    _first_peak = now;
  } else if (sign == _last_sign || (now - _last_peak) < _config.min_peak_ms) {
    reset_sequence();
    return Result::Rejected;
  }
  _last_sign = sign;
  _last_peak = now;
  ++_peaks;
  if (_peaks < _config.required_peaks) {
    return Result::Peak;
  }

  _last_shake = now;
  _cooling = true;
  _rearm_ready = false;
  reset_sequence();
  _quiet = false;
  return Result::Shake;
}
