#pragma once

#include <driver/i2c_master.h>
#include "hal/pressure_sensor.h"

class SPL07 : public PressureSensor {
public:
  explicit SPL07(i2c_master_bus_handle_t bus, uint8_t address = 0x77)
      : _bus(bus), _address(address) {}
  ~SPL07() override;
  bool init() override;
  bool read(PressureData &out) override;
  bool supports_temp_hum() const override { return true; }
  TempHumData temp_hum_data() override { return _temperature; }

private:
  bool read_register(uint8_t reg, uint8_t *data, size_t len);
  bool write_register(uint8_t reg, uint8_t value);
  bool read_coefficients();
  i2c_master_bus_handle_t _bus;
  uint8_t _address;
  i2c_master_dev_handle_t _dev = nullptr;
  bool _ready = false;
  int32_t _c0 = 0, _c1 = 0, _c00 = 0, _c10 = 0, _c01 = 0;
  int32_t _c11 = 0, _c20 = 0, _c21 = 0, _c30 = 0, _c31 = 0, _c40 = 0;
  TempHumData _temperature{};
};
