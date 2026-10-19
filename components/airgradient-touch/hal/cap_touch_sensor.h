/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef CAP_TOUCH_SENSOR_H
#define CAP_TOUCH_SENSOR_H

#include <cstdint>

// Bitmask flags identifying each capacitive touch input channel.
// Combine with | to address multiple channels simultaneously.
namespace TouchChannel {
inline constexpr uint8_t CH1 = 0x01;
inline constexpr uint8_t CH2 = 0x02;
inline constexpr uint8_t CH3 = 0x04;
inline constexpr uint8_t ALL = CH1 | CH2 | CH3;
} // namespace TouchChannel

// Output of a single touch sensor read.
// touched: bitmask of TouchChannel flags currently reporting a touch.
// noise:   bitmask of TouchChannel flags reporting a noise condition.
//          Only meaningful when CapTouchSensor::supports_noise() == true;
//          implementations that do not support noise detection always
//          leave this field as 0.
struct TouchData {
  uint8_t touched{0};
  uint8_t noise{0};
};

// Abstract interface for a capacitive touch sensor.
//
// ISR-safe: no
// Thread-safe: no
// Blocking: init() may block during device probe and configuration
// Allocates: no (after init())
class CapTouchSensor {
public:
  virtual ~CapTouchSensor() = default;

  // Initialises the device. Must be called before read().
  // Returns false if the device could not be found or configured.
  virtual bool init() = 0;

  // Reads the current touch state into out. Returns false on I2C failure.
  // out.noise is only valid when supports_noise() == true.
  // Does NOT acknowledge the interrupt; call clear_interrupt() to ack and
  // advance to fresh state (the CAP1203 latches status until cleared).
  virtual bool read(TouchData &out) = 0;

  // Returns true if this implementation can report per-channel noise flags.
  // When false, out.noise will always be 0 after read().
  virtual bool supports_noise() const { return false; }

  // Returns true if this implementation supports manual recalibration.
  // When false, calibrate() is a no-op that returns false.
  virtual bool supports_calibration() const { return false; }

  // Triggers hardware recalibration on the channels indicated by channel_mask.
  // channel_mask is a bitmask of TouchChannel flags.
  // Returns false if unsupported or if the operation failed.
  virtual bool calibrate(uint8_t channel_mask) { return false; }

  // Clears the hardware interrupt line after a touch event has been processed.
  // Driver-specific implementations (e.g. CAP1203) override this method.
  // Default: no-op, returns true.
  virtual bool clear_interrupt() { return true; }

  // Enables/disables deep sleep. Sensing is disabled while asleep.
  // Returns false if unsupported or if communication failed.
  virtual bool set_sleep(bool) { return false; }
};

#endif // CAP_TOUCH_SENSOR_H
