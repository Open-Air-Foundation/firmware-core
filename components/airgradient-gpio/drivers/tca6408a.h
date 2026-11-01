#pragma once

#include <cstdint>
#include <driver/i2c_master.h>
#include "rtos.h"

// Task-context GPIO expander. The mutex protects output read/modify/write
// across display, storage and input tasks sharing the same device.
class TCA6408A {
public:
  static constexpr uint8_t DEFAULT_ADDRESS = 0x20;
  explicit TCA6408A(i2c_master_bus_handle_t bus) : _bus(bus) {}
  ~TCA6408A();
  bool init(uint8_t outputs_high, uint8_t inputs);
  bool set_level(uint8_t pin, bool high);
  int get_level(uint8_t pin); // -1 on transport error

private:
  bool write_register(uint8_t reg, uint8_t value);
  i2c_master_bus_handle_t _bus;
  i2c_master_dev_handle_t _dev = nullptr;
  RtosMutex _mutex;
  uint8_t _outputs = 0xFF;
};
