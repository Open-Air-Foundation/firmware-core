/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef TCA9536_H
#define TCA9536_H

#include <cstdint>

#include "driver/i2c_master.h"

// TI TCA9536 four-port I2C GPIO expander, output control only.
// The caller owns the initialised bus; this driver owns its device handle.
// Calls are blocking, not ISR-safe, and must be serialised by the caller.
// Use one driver instance per device.
class TCA9536 {
public:
  static constexpr uint8_t DEFAULT_ADDRESS = 0x41;
  static constexpr uint8_t PIN_COUNT = 4;

  struct Config {
    uint32_t scl_speed_hz = 100000;
    int timeout_ms = 100;
  };

  explicit TCA9536(i2c_master_bus_handle_t bus);
  TCA9536(i2c_master_bus_handle_t bus, const Config &config);
  ~TCA9536();

  TCA9536(const TCA9536 &) = delete;
  TCA9536 &operator=(const TCA9536 &) = delete;

  // Attach and check register access without changing any pin or pull-up state.
  // Safe to call again; successful register reads do not verify chip identity.
  bool init();

  // Pins are P0..P3 (0..3). Preload the latch before enabling output.
  // For P3, also disable its alternate interrupt-output function.
  // Other pins and the global pull-up setting are preserved.
  // A failed multi-register operation may have partially taken effect;
  // retrying reads the hardware state again rather than using cached values.
  bool configure_output(uint8_t pin, bool initial_level);

  // Set an existing GPIO output high or low. Rejects inputs and P3 in INT mode.
  // Both output methods return false before init(), for invalid pins, or on I2C errors.
  bool write(uint8_t pin, bool level);

private:
  bool _read_reg(uint8_t reg, uint8_t &out);
  bool _write_reg(uint8_t reg, uint8_t value);
  bool _update_reg(uint8_t reg, uint8_t mask, uint8_t value);

  i2c_master_bus_handle_t _bus = nullptr;
  i2c_master_dev_handle_t _dev = nullptr;
  Config _config;
  bool _initialised = false;
};

#endif // TCA9536_H
