/**
 * AirGradient Go — LIS2DH12 3-axis accelerometer driver
 *
 * I2C driver for the ST LIS2DH12. Configurable high-resolution sampling,
 * high-pass filtering, and threshold interrupts on INT1.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "accel_sensor.h"

#include <driver/i2c_master.h>

#include <cstddef>
#include <cstdint>

class LIS2DH12 : public AccelSensor {
public:
  struct Config {
    uint8_t address = 0x18;         ///< 7-bit address (SA0 = GND)
    uint32_t scl_speed_hz = 400000; ///< 400 kHz fast-mode
    int timeout_ms = 50;
  };

  /// Expected response from the WHO_AM_I register (datasheet §8.3).
  static constexpr uint8_t WHO_AM_I_EXPECTED = 0x33;

  LIS2DH12(i2c_master_bus_handle_t bus, const Config &config);
  ~LIS2DH12() override;

  LIS2DH12(const LIS2DH12 &) = delete;
  LIS2DH12 &operator=(const LIS2DH12 &) = delete;

  /// Verify identity and power down. configure() selects the operating settings.
  bool init() override;
  uint8_t who_am_i() override;
  uint8_t expected_who_am_i() const override { return WHO_AM_I_EXPECTED; }

  /// High-resolution rates: 1, 10, 25, 50, 100, 200, 400 Hz.
  /// Threshold and duration must be exactly representable at the selected range/rate.
  bool configure(const AccelConfig &config) override;
  AccelReadResult read(AccelReading &out) override;
  bool read_interrupt(bool &active) override;
  bool power_down() override;

private:
  Config _config;
  i2c_master_bus_handle_t _bus = nullptr;
  i2c_master_dev_handle_t _dev = nullptr;
  bool _identified = false;
  bool _configured = false;
  uint8_t _mg_per_lsb = 0;

  bool _write_reg(uint8_t reg, uint8_t value);
  bool _read_reg(uint8_t reg, uint8_t &out);
  bool _read_block(uint8_t reg, uint8_t *buf, size_t len);
};
