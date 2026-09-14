/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef TCA6408A_H
#define TCA6408A_H

#include <cstdint>

#include "driver/i2c_master.h"

// TI TCA6408A 8-bit I2C I/O expander (SCPS192E).
//
// Registers: 0x00 input, 0x01 output (POR 0xFF), 0x02 polarity (POR 0x00),
// 0x03 configuration (POR 0xFF = all inputs).  The driver caches the output
// and configuration registers so that a single pin change is one I2C write.
//
// The GO v2.0 board leaves ~RESET floating, so a spurious reset returns every
// pin to input mode.  check_and_restore() detects that (configuration reads
// back 0xFF while outputs are expected) and re-applies the cached registers.
//
// ISR-safe: no.  Thread-safe: no.  Blocking: yes (I2C).  Allocates: no after init().
class TCA6408A {
public:
  static constexpr uint8_t DEFAULT_ADDRESS = 0x20; // ADDR pin tied to GND

  struct Config {
    uint8_t address = DEFAULT_ADDRESS;
    uint32_t scl_speed_hz = 400000;
    int timeout_ms = 100;
  };

  TCA6408A(i2c_master_bus_handle_t bus, const Config &config);
  ~TCA6408A();

  TCA6408A(const TCA6408A &) = delete;
  TCA6408A &operator=(const TCA6408A &) = delete;

  // Probe, attach and read the current output/configuration registers into the
  // cache.  Does not change any pin.  Idempotent.
  bool init();
  bool ready() const { return _dev != nullptr; }

  // Write the full output register, then the configuration register.  Writing
  // outputs first means a pin never drives a stale POR level when it becomes
  // an output.
  bool apply(uint8_t output, uint8_t config);

  bool set_direction(uint8_t pin, bool output);
  bool set_level(uint8_t pin, bool high);
  bool get_level(uint8_t pin, bool &high); // reads the input register

  bool read_register(uint8_t reg, uint8_t &out);
  uint8_t cached_output() const { return _output; }
  uint8_t cached_config() const { return _config_reg; }

  // Re-apply the cached registers when the chip reports all-input while at
  // least one output is expected.  Returns true when a restore happened.
  bool check_and_restore();

  static constexpr uint8_t REG_INPUT = 0x00;
  static constexpr uint8_t REG_OUTPUT = 0x01;
  static constexpr uint8_t REG_POLARITY = 0x02;
  static constexpr uint8_t REG_CONFIG = 0x03;

private:
  i2c_master_bus_handle_t _bus;
  i2c_master_dev_handle_t _dev = nullptr;
  Config _config;
  uint8_t _output = 0xFF;
  uint8_t _config_reg = 0xFF;

  bool _write_register(uint8_t reg, uint8_t value);
};

#endif // TCA6408A_H
