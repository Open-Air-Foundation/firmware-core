/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef CAP1203_H
#define CAP1203_H

#include "cap_touch_sensor.h"

#include "driver/i2c_master.h"

#include <cstdint>

// CAP1203 3-channel capacitive touch sensor driver (Microchip Technology).
// Communicates over I2C. The I2C bus must be initialised by the caller before
// init() is called; this driver registers and removes its own device handle.
//
// Supports: noise detection, manual recalibration, per-channel thresholds,
// global sensitivity control, and selective channel/interrupt enable masks.
//
// Interrupt acknowledgement (clear_interrupt()) is driver-specific and not
// part of the CapTouchSensor HAL interface.
class CAP1203 : public CapTouchSensor {
public:
  struct Config {
    // Channels to enable for touch detection. Bitmask of TouchChannel flags.
    uint8_t enabled_channels = TouchChannel::ALL;

    // Channels that drive the interrupt line. Bitmask of TouchChannel flags.
    uint8_t interrupt_channels = TouchChannel::ALL;

    // Channels with the repeat rate enabled. Bitmask of TouchChannel flags.
    // Default matches the chip power-on default (all enabled): while a touch is
    // held, the chip re-asserts the interrupt at the repeat rate. Clearing a
    // channel's bit makes that channel interrupt only on press and release,
    // giving a clean two-edge stream for gesture detection.
    uint8_t repeat_rate_channels = TouchChannel::ALL;

    // Global sensitivity: delta_sense 0–7 (0 = most sensitive, 7 = least).
    uint8_t delta_sense = 2;

    // Base capacitance shift 0–15. Matches the chip power-on default (0x0F).
    // Lowering this alters the internal delta-count normalization and can
    // prevent weaker channels from reaching the detection threshold.
    uint8_t base_shift = 0x0F;

    // Per-channel touch detection thresholds. Range 0–127.
    uint8_t threshold_ch1 = 64;
    uint8_t threshold_ch2 = 64;
    uint8_t threshold_ch3 = 64;

    // Recalibrate a channel whose touch has been held longer than
    // max_duration_code, which rescues a pad something is resting on. Off by
    // default, matching the chip. The code is 0–15; 0x6 is 2800 ms and each
    // step is listed in the datasheet (Table 5-12).
    bool max_duration_recal = false;
    uint8_t max_duration_code = 0x6;

    // I2C bus speed.
    uint32_t scl_speed_hz = 100000;

    // I2C transaction timeout.
    int timeout_ms = 1000;
  };

  static constexpr uint8_t DEFAULT_ADDRESS = 0x28;

  // Constructs the driver. init() must be called before any other method.
  // i2c_bus: a pre-initialised I2C master bus handle.
  explicit CAP1203(i2c_master_bus_handle_t i2c_bus, uint8_t address = DEFAULT_ADDRESS);
  CAP1203(i2c_master_bus_handle_t i2c_bus, uint8_t address, const Config &config);
  ~CAP1203() override;

  // CapTouchSensor interface
  bool init() override;
  bool read(TouchData &out) override;
  bool supports_noise() const override { return true; }
  bool supports_calibration() const override { return true; }
  bool calibrate(uint8_t channel_mask) override;

  // Clears the interrupt line. Reads MAIN_CONTROL to clear the INT bit, then
  // reads GENERAL_STATUS to latch-clear the touch status flags.
  // Call this from the interrupt handler or after processing a touch event.
  bool clear_interrupt() override;

private:
  bool _read_reg(uint8_t reg, uint8_t &out);
  bool _write_reg(uint8_t reg, uint8_t val);
  bool _apply_config();
  bool _apply_max_duration();
  void _rm_device();

  i2c_master_bus_handle_t _i2c_bus;
  i2c_master_dev_handle_t _dev_handle{nullptr};
  uint8_t _address;
  Config _config;

  // Register addresses
  static constexpr uint8_t REG_MAIN_CONTROL = 0x00;
  static constexpr uint8_t REG_GENERAL_STATUS = 0x02;
  static constexpr uint8_t REG_SENSOR_INPUT_STATUS = 0x03;
  static constexpr uint8_t REG_NOISE_FLAG_STATUS = 0x0A;
  static constexpr uint8_t REG_SENSITIVITY_CONTROL = 0x1F;
  static constexpr uint8_t REG_CONFIGURATION = 0x20;
  static constexpr uint8_t REG_SENSOR_INPUT_CONFIG = 0x22;
  static constexpr uint8_t REG_SENSOR_INPUT_ENABLE = 0x21;
  static constexpr uint8_t REG_CALIBRATION_ACTIVATE = 0x26;
  static constexpr uint8_t REG_INTERRUPT_ENABLE = 0x27;
  static constexpr uint8_t REG_REPEAT_RATE_ENABLE = 0x28;
  static constexpr uint8_t REG_THRESHOLD_CH1 = 0x30;
  static constexpr uint8_t REG_THRESHOLD_CH2 = 0x31;
  static constexpr uint8_t REG_THRESHOLD_CH3 = 0x32;
  static constexpr uint8_t REG_PRODUCT_ID = 0xFD;
  static constexpr uint8_t REG_MANUFACTURER_ID = 0xFE;
  static constexpr uint8_t REG_REVISION = 0xFF;

  // Expected identity values
  static constexpr uint8_t EXPECTED_PRODUCT_ID = 0x6D;
  static constexpr uint8_t EXPECTED_MANUFACTURER_ID = 0x5D;
};

#endif // CAP1203_H
