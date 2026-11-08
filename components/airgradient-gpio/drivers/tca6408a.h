/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef TCA6408A_H
#define TCA6408A_H

#include "airgradient_gpio.h"
#include "rtos.h"

#include <cstdint>
#include "driver/i2c_master.h"

namespace gpio::tca6408a {

inline constexpr uint8_t ADDRESS_LOW = 0x20;
inline constexpr uint8_t ADDRESS_HIGH = 0x21;
inline constexpr int PIN_COUNT = 8;
inline constexpr uint32_t DEFAULT_SCL_SPEED_HZ = 100000;
inline constexpr uint32_t MAX_SCL_SPEED_HZ = 400000;
inline constexpr int DEFAULT_TIMEOUT_MS = 100;

struct Config {
  uint8_t address = ADDRESS_LOW;
  uint32_t scl_speed_hz = DEFAULT_SCL_SPEED_HZ;
  int timeout_ms = DEFAULT_TIMEOUT_MS;
};

// Caller-owned state; fields are managed by the functions below. Use one
// context per device and keep it alive while HAL/Pin bindings are in use.
// The mutex makes Context noncopyable and nonmovable. Stop users and call
// deinit() before destroying the context or its bus.
struct Context {
  i2c_master_bus_handle_t bus = nullptr;
  i2c_master_dev_handle_t device = nullptr;
  Config config{};
  RtosMutex mutex;
  uint8_t output_latch = UINT8_MAX;
  bool output_latch_valid = false;
};

// Reads the output latch into the cache without changing pins or acknowledging INT.
// Repeated calls recheck the device; deinit first to change bus or config.
bool init(Context &context, i2c_master_bus_handle_t bus, const Config &config = {});
// Removes the I2C device without changing pin levels. Safe to repeat.
// On failure, retains the handle so the caller can retry.
bool deinit(Context &context);

// Borrowed binding. Supports Input/Output, Floating, and interrupts Disabled.
// Explicit initial levels preload the output latch; omission preserves it.
// All operations are task-only and may block on the mutex and I2C transfers.
// Output updates use the cache; reinitialize after external reset or power loss.
Hal hal(Context &context);

// Reads input levels, including polarity inversion, and acknowledges INT.
// Leaves levels unchanged on error. HAL get_level() also reads the whole port.
bool read_port(Context &context, uint8_t &levels);
// Replaces all output latches without changing directions.
bool write_port(Context &context, uint8_t levels);
// Bit set = invert that input in reads. Replaces the entire polarity register.
bool set_input_inversion(Context &context, uint8_t mask);

} // namespace gpio::tca6408a

#endif // TCA6408A_H
