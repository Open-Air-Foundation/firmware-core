/**
 * AirGradient Go — Accelerometer HAL
 *
 * Abstract interface for a 3-axis accelerometer. The Go board carries an
 * ST LIS2DH12; callers depend on hardware capabilities through this
 * interface rather than concrete registers or bus operations.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>
#include <limits>

namespace AccelAxis {
inline constexpr uint8_t X = 0x01;
inline constexpr uint8_t Y = 0x02;
inline constexpr uint8_t Z = 0x04;
inline constexpr uint8_t ALL = X | Y | Z;
} // namespace AccelAxis

enum class AccelRange : uint8_t { G2 = 2, G4 = 4, G8 = 8, G16 = 16 };

struct AccelConfig {
  uint16_t sample_rate_hz = 0; // Caller must select a supported nonzero rate.
  AccelRange range = AccelRange::G2;
  bool high_pass = false; // Apply the hardware filter to XYZ and the interrupt.
  struct Interrupt {
    // OR combination of selected high/low threshold events. Zero masks disable INT1.
    uint8_t high_axes = 0;
    uint8_t low_axes = 0;
    uint16_t threshold_mg = 0;
    uint16_t duration_ms = 0;
    bool latched = false;
    bool active_low = false;
  } interrupt;
};

/// One XYZ sample in milli-g, with the filtering selected by configure().
/// Quality flags are separate from field validity.
struct AccelReading {
  static constexpr int16_t INVALID = std::numeric_limits<int16_t>::min();
  int16_t x_mg = INVALID;
  int16_t y_mg = INVALID;
  int16_t z_mg = INVALID;
  bool overrun = false;
  bool clipped = false;

  bool is_x_valid() const { return x_mg != INVALID; }
  bool is_y_valid() const { return y_mg != INVALID; }
  bool is_z_valid() const { return z_mg != INVALID; }
  bool is_valid() const { return is_x_valid() && is_y_valid() && is_z_valid(); }
};

enum class AccelReadResult { Ready, NotReady, Error };

/// Abstract 3-axis accelerometer.
/// ISR-safe: no. Thread-safe: no; callers must serialize access.
/// Blocking: init/configure and bus operations; configure includes chip settling.
/// Allocates: no after init().
class AccelSensor {
public:
  virtual ~AccelSensor() = default;

  /// Probe identity and leave sampling and interrupt output disabled.
  virtual bool init() = 0;
  virtual uint8_t who_am_i() = 0;
  virtual uint8_t expected_who_am_i() const = 0;

  /// Configure sampling and INT1 after init(). Unsupported/unrepresentable
  /// settings fail without changing hardware. Bus failure invalidates sampling.
  virtual bool configure(const AccelConfig &config) = 0;

  /// Only Ready supplies new XYZ. Other results leave invalid output fields.
  virtual AccelReadResult read(AccelReading &out) = 0;

  /// Read and acknowledge INT1; return bus success and report its active bit.
  virtual bool read_interrupt(bool &active) = 0;

  /// Disable interrupt output and sampling. Further reads fail until configured.
  virtual bool power_down() = 0;
};
