/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "tca9536.h"

#include "esp_log.h"

namespace {
static constexpr const char *TAG = "TCA9536";

// TI SCPS275A, sections 8.6.2 and 8.6.3.
constexpr uint8_t REG_OUTPUT = 0x01;
constexpr uint8_t REG_CONFIGURATION = 0x03;
constexpr uint8_t REG_SPECIAL_FUNCTION = 0x50;
constexpr uint8_t PIN_INTERRUPT = 3;
constexpr uint8_t P3_AS_INTERRUPT = 0x80;
constexpr uint8_t PULLUPS_DISABLED = 0x40;
constexpr uint32_t MAX_SCL_SPEED_HZ = 1000000;
} // namespace

TCA9536::TCA9536(i2c_master_bus_handle_t bus) : TCA9536(bus, Config{}) {}

TCA9536::TCA9536(i2c_master_bus_handle_t bus, const Config &config) : _bus(bus), _config(config) {}

TCA9536::~TCA9536() {
  if (_dev != nullptr) {
    if (i2c_master_bus_rm_device(_dev) != ESP_OK) {
      ESP_LOGE(TAG, "remove device failed");
    }
  }
}

bool TCA9536::init() {
  _initialised = false;
  if (_bus == nullptr || _config.scl_speed_hz == 0 || _config.scl_speed_hz > MAX_SCL_SPEED_HZ ||
      _config.timeout_ms <= 0) {
    return false;
  }

  if (_dev == nullptr) {
    i2c_device_config_t config{};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = DEFAULT_ADDRESS;
    config.scl_speed_hz = _config.scl_speed_hz;
    if (i2c_master_bus_add_device(_bus, &config, &_dev) != ESP_OK) {
      ESP_LOGE(TAG, "add device failed at 0x%02X", DEFAULT_ADDRESS);
      _dev = nullptr;
      return false;
    }
  }

  // Reads check presence without resetting a device that may already drive outputs.
  uint8_t value = 0;
  _initialised = _read_reg(REG_OUTPUT, value) && _read_reg(REG_CONFIGURATION, value) &&
                 _read_reg(REG_SPECIAL_FUNCTION, value);
  return _initialised;
}

bool TCA9536::configure_output(uint8_t pin, bool initial_level) {
  if (!_initialised || pin >= PIN_COUNT) {
    return false;
  }

  const uint8_t mask = static_cast<uint8_t>(1U << pin);
  // The output latch powers up high. Set the desired level before driving the pin.
  if (!_update_reg(REG_OUTPUT, mask, initial_level ? mask : 0) ||
      !_update_reg(REG_CONFIGURATION, mask, 0)) {
    return false;
  }

  // Select P3's GPIO function only after its latch and direction are ready.
  return pin != PIN_INTERRUPT || _update_reg(REG_SPECIAL_FUNCTION, P3_AS_INTERRUPT, 0);
}

bool TCA9536::configure_input(uint8_t pin) {
  if (!_initialised || pin >= PIN_COUNT) {
    return false;
  }
  const uint8_t mask = static_cast<uint8_t>(1U << pin);
  return _update_reg(REG_CONFIGURATION, mask, mask) &&
         (pin != PIN_INTERRUPT || _update_reg(REG_SPECIAL_FUNCTION, P3_AS_INTERRUPT, 0));
}

bool TCA9536::set_pullups_enabled(bool enabled) {
  return _initialised &&
         _update_reg(REG_SPECIAL_FUNCTION, PULLUPS_DISABLED, enabled ? 0 : PULLUPS_DISABLED);
}

bool TCA9536::write(uint8_t pin, bool level) {
  if (!_initialised || pin >= PIN_COUNT) {
    return false;
  }

  const uint8_t mask = static_cast<uint8_t>(1U << pin);
  uint8_t configuration = 0;
  if (!_read_reg(REG_CONFIGURATION, configuration) || (configuration & mask) != 0) {
    return false;
  }
  if (pin == PIN_INTERRUPT) {
    uint8_t special = 0;
    if (!_read_reg(REG_SPECIAL_FUNCTION, special) || (special & P3_AS_INTERRUPT) != 0) {
      return false;
    }
  }
  return _update_reg(REG_OUTPUT, mask, level ? mask : 0);
}

bool TCA9536::_read_reg(uint8_t reg, uint8_t &out) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t value = 0;
  if (i2c_master_transmit_receive(_dev, &reg, sizeof(reg), &value, sizeof(value),
                                  _config.timeout_ms) != ESP_OK) {
    ESP_LOGE(TAG, "read register 0x%02X failed", reg);
    return false;
  }
  out = value;
  return true;
}

bool TCA9536::_write_reg(uint8_t reg, uint8_t value) {
  if (_dev == nullptr) {
    return false;
  }
  const uint8_t data[] = {reg, value};
  if (i2c_master_transmit(_dev, data, sizeof(data), _config.timeout_ms) != ESP_OK) {
    ESP_LOGE(TAG, "write register 0x%02X failed", reg);
    return false;
  }
  return true;
}

bool TCA9536::_update_reg(uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t current = 0;
  if (!_read_reg(reg, current)) {
    return false;
  }
  const uint8_t updated = static_cast<uint8_t>((current & ~mask) | (value & mask));
  return updated == current || _write_reg(reg, updated);
}
