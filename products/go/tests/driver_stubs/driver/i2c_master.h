#pragma once

#include <cstddef>
#include <cstdint>

// Native test declarations for the ESP-IDF calls used by LIS2DH12.
// Only the dedicated driver-test target includes this directory.
struct TestI2cBus;
using i2c_master_bus_handle_t = TestI2cBus *;
using i2c_master_dev_handle_t = TestI2cBus *;
using esp_err_t = int;
inline constexpr esp_err_t ESP_OK = 0;
inline constexpr esp_err_t ESP_FAIL = -1;
inline constexpr int I2C_ADDR_BIT_LEN_7 = 0;

struct i2c_device_config_t {
  int dev_addr_length;
  uint16_t device_address;
  uint32_t scl_speed_hz;
  uint32_t scl_wait_us;
  struct {
    unsigned disable_ack_check : 1;
  } flags;
};

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *config,
                                    i2c_master_dev_handle_t *device);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *bytes, size_t size,
                              int timeout_ms);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *write,
                                      size_t write_size, uint8_t *read, size_t read_size,
                                      int timeout_ms);
