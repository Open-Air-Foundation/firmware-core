/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "tca6408a.h"

#include "esp_log.h"

static constexpr const char *TAG = "TCA6408A";

namespace {

class Guard {
public:
  explicit Guard(SemaphoreHandle_t lock) : _lock(lock) {
    if (_lock != nullptr) {
      xSemaphoreTake(_lock, portMAX_DELAY);
    }
  }
  ~Guard() {
    if (_lock != nullptr) {
      xSemaphoreGive(_lock);
    }
  }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;

private:
  SemaphoreHandle_t _lock;
};

} // namespace

TCA6408A::TCA6408A(i2c_master_bus_handle_t bus, const Config &config)
    : _bus(bus), _config(config), _lock(xSemaphoreCreateMutex()) {}

TCA6408A::~TCA6408A() {
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
  if (_lock != nullptr) {
    vSemaphoreDelete(_lock);
    _lock = nullptr;
  }
}

bool TCA6408A::init() {
  if (_lock == nullptr) {
    ESP_LOGE(TAG, "mutex allocation failed");
    return false;
  }
  Guard guard(_lock);
  if (_dev != nullptr) {
    return true;
  }

  esp_err_t err = i2c_master_probe(_bus, _config.address, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "probe at 0x%02X failed: %s", _config.address, esp_err_to_name(err));
    return false;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _config.address,
      .scl_speed_hz = _config.scl_speed_hz,
      .scl_wait_us = 0,
      .flags = {},
  };
  err = i2c_master_bus_add_device(_bus, &dev_cfg, &_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
    _dev = nullptr;
    return false;
  }

  uint8_t output = 0;
  uint8_t config = 0;
  if (!read_register(REG_OUTPUT, output) || !read_register(REG_CONFIG, config)) {
    ESP_LOGE(TAG, "register readback failed");
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
    return false;
  }
  _output = output;
  _config_reg = config;
  ESP_LOGI(TAG, "found at 0x%02X (output=0x%02X config=0x%02X)", _config.address, _output,
           _config_reg);
  return true;
}

bool TCA6408A::apply(uint8_t output, uint8_t config) {
  Guard guard(_lock);
  return _apply_unlocked(output, config);
}

bool TCA6408A::_apply_unlocked(uint8_t output, uint8_t config) {
  if (!_write_register(REG_OUTPUT, output)) {
    return false;
  }
  _output = output;
  if (!_write_register(REG_CONFIG, config)) {
    return false;
  }
  _config_reg = config;
  return true;
}

bool TCA6408A::set_direction(uint8_t pin, bool output) {
  if (pin > 7) {
    return false;
  }
  Guard guard(_lock);
  const uint8_t mask = static_cast<uint8_t>(1u << pin);
  const uint8_t next =
      output ? static_cast<uint8_t>(_config_reg & ~mask) : static_cast<uint8_t>(_config_reg | mask);
  if (next == _config_reg) {
    return true;
  }
  if (!_write_register(REG_CONFIG, next)) {
    return false;
  }
  _config_reg = next;
  return true;
}

bool TCA6408A::set_level(uint8_t pin, bool high) {
  if (pin > 7) {
    return false;
  }
  Guard guard(_lock);
  const uint8_t mask = static_cast<uint8_t>(1u << pin);
  const uint8_t next =
      high ? static_cast<uint8_t>(_output | mask) : static_cast<uint8_t>(_output & ~mask);
  // Always write: the cache cannot see a chip reset (floating ~RESET on v2.0),
  // and one I2C write is cheaper than a read-verify.
  if (!_write_register(REG_OUTPUT, next)) {
    return false;
  }
  _output = next;
  return true;
}

bool TCA6408A::get_level(uint8_t pin, bool &high) {
  if (pin > 7) {
    return false;
  }
  uint8_t input = 0;
  if (!read_register(REG_INPUT, input)) {
    return false;
  }
  high = (input & static_cast<uint8_t>(1u << pin)) != 0;
  return true;
}

bool TCA6408A::check_and_restore() {
  Guard guard(_lock);
  if (_config_reg == 0xFF) {
    return false; // no outputs expected, nothing to restore
  }
  uint8_t config = 0;
  if (!read_register(REG_CONFIG, config)) {
    return false;
  }
  if (config == _config_reg) {
    return false;
  }
  ESP_LOGW(TAG, "config reads 0x%02X, expected 0x%02X — chip reset? restoring", config,
           _config_reg);
  return _apply_unlocked(_output, _config_reg); // apply() would take the lock twice
}

bool TCA6408A::read_register(uint8_t reg, uint8_t &out) {
  if (_dev == nullptr) {
    return false;
  }
  esp_err_t err = i2c_master_transmit_receive(_dev, &reg, 1, &out, 1, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read reg 0x%02X failed: %s", reg, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool TCA6408A::_write_register(uint8_t reg, uint8_t value) {
  if (_dev == nullptr) {
    return false;
  }
  const uint8_t buf[2] = {reg, value};
  esp_err_t err = i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write reg 0x%02X failed: %s", reg, esp_err_to_name(err));
    return false;
  }
  return true;
}
