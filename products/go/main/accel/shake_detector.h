// AirGradient Go — bench-validated side-to-side shake detector.
#pragma once

#include "accel_sensor.h"

#include <cstdint>

enum class ShakeAxis : uint8_t { X, Y, Z };

class ShakeDetector {
public:
  struct Config {
    static constexpr int MAX_PEAK_MG = 4096; // Detector tuned for +/-4 g capture.
    static constexpr uint32_t MAX_WINDOW_MS = 10000;
    static constexpr uint32_t MAX_COOLDOWN_MS = 60000;
    static constexpr int MAX_DOMINANCE_PERCENT = 1000;
    ShakeAxis axis = ShakeAxis::Y;
    int peak_mg = 400;
    int release_mg = 150;
    int dominance_percent = 150;
    uint8_t required_peaks = 6; // Three left/right pairs, either starting direction.
    uint32_t min_peak_ms = 50;
    uint32_t max_peak_ms = 400;
    uint32_t window_ms = 1600;
    uint32_t max_sample_gap_ms = 50;
    uint32_t quiet_lead_ms = 0;
    uint32_t cooldown_ms = 1000;
  };
  enum class Result { None, Peak, Shake, Rejected, Invalid, OffAxis };

  explicit ShakeDetector(const Config &config) : _config(config) {}
  static bool valid_config(const Config &config);
  Result update(const AccelReading &sample, uint32_t now);
  // A new capture cannot continue an old sequence. Cooldown survives captures.
  void reset_capture();
  bool in_cooldown(uint32_t now) const;
  uint8_t peaks() const { return _peaks; }

private:
  Config _config;
  uint8_t _peaks = 0;
  int _last_sign = 0;
  int _zone = 0;
  uint32_t _first_peak = 0;
  uint32_t _last_peak = 0;
  uint32_t _last_sample = 0;
  uint32_t _quiet_start = 0;
  uint32_t _last_quiet = 0;
  uint32_t _last_shake = 0;
  bool _have_sample = false;
  bool _quiet = false;
  bool _lead_ready = false;
  bool _cooling = false;

  void reset_sequence();
};
