#include "tca6408a.h"

namespace {
constexpr uint8_t REG_INPUT = 0;
constexpr uint8_t REG_OUTPUT = 1;
constexpr uint8_t REG_POLARITY = 2;
constexpr uint8_t REG_CONFIG = 3;
constexpr uint8_t PIN_COUNT = 8;
constexpr int TIMEOUT_MS = 100;
constexpr uint32_t CLOCK_HZ = 400000;
} // namespace

TCA6408A::~TCA6408A() {
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
  }
}

bool TCA6408A::init(uint8_t outputs_high, uint8_t inputs) {
  if (!_mutex.is_valid()) {
    return false;
  }
  if (_dev == nullptr) {
    i2c_device_config_t cfg{};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = DEFAULT_ADDRESS;
    cfg.scl_speed_hz = CLOCK_HZ;
    if (i2c_master_bus_add_device(_bus, &cfg, &_dev) != ESP_OK) {
      return false;
    }
  }
  // Set safe levels before enabling output drivers. No polarity inversion.
  if (!write_register(REG_OUTPUT, outputs_high) || !write_register(REG_POLARITY, 0) ||
      !write_register(REG_CONFIG, inputs)) {
    return false;
  }
  _outputs = outputs_high;
  return true;
}

bool TCA6408A::write_register(uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  return _dev != nullptr && i2c_master_transmit(_dev, data, sizeof(data), TIMEOUT_MS) == ESP_OK;
}

bool TCA6408A::set_level(uint8_t pin, bool high) {
  if (pin >= PIN_COUNT || !_mutex.lock()) {
    return false;
  }
  const uint8_t mask = 1u << pin;
  const uint8_t next = high ? (_outputs | mask) : (_outputs & ~mask);
  const bool ok = write_register(REG_OUTPUT, next);
  if (ok) {
    _outputs = next;
  }
  _mutex.unlock();
  return ok;
}

int TCA6408A::get_level(uint8_t pin) {
  if (pin >= PIN_COUNT || _dev == nullptr || !_mutex.lock()) {
    return -1;
  }
  const uint8_t reg = REG_INPUT;
  uint8_t value = 0;
  const bool ok = i2c_master_transmit_receive(_dev, &reg, 1, &value, 1, TIMEOUT_MS) == ESP_OK;
  _mutex.unlock();
  return ok ? ((value >> pin) & 1) : -1;
}
