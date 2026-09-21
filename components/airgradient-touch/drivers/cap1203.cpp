/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "cap1203.h"

#include "esp_log.h"

static constexpr const char *TAG = "CAP1203";

CAP1203::CAP1203(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : _i2c_bus(i2c_bus), _address(address), _config{} {}

CAP1203::CAP1203(i2c_master_bus_handle_t i2c_bus, uint8_t address, const Config &config)
    : _i2c_bus(i2c_bus), _address(address), _config(config) {}

CAP1203::~CAP1203() { _rm_device(); }

bool CAP1203::init() {
  _rm_device();

  if (_i2c_bus == nullptr) {
    ESP_LOGE(TAG, "i2c_bus is null");
    return false;
  }

  // Probe to confirm the device is on the bus before adding it.
  esp_err_t err = i2c_master_probe(_i2c_bus, _address, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "probe failed at 0x%02X: %s", _address, esp_err_to_name(err));
    return false;
  }

  // Register device handle on the bus.
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _address,
      .scl_speed_hz = _config.scl_speed_hz,
      .scl_wait_us = 0,
      .flags = {},
  };
  err = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_dev_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "add device failed: %s", esp_err_to_name(err));
    return false;
  }

  // Verify product and manufacturer identity.
  uint8_t product_id = 0;
  uint8_t manufacturer_id = 0;
  uint8_t revision = 0;

  if (!_read_reg(REG_PRODUCT_ID, product_id) || !_read_reg(REG_MANUFACTURER_ID, manufacturer_id) ||
      !_read_reg(REG_REVISION, revision)) {
    ESP_LOGE(TAG, "failed to read identity registers");
    _rm_device();
    return false;
  }

  ESP_LOGI(TAG, "product=0x%02X manufacturer=0x%02X revision=0x%02X", product_id, manufacturer_id,
           revision);

  if (product_id != EXPECTED_PRODUCT_ID) {
    ESP_LOGE(TAG, "unexpected product ID 0x%02X (expected 0x%02X)", product_id,
             EXPECTED_PRODUCT_ID);
    _rm_device();
    return false;
  }

  if (manufacturer_id != EXPECTED_MANUFACTURER_ID) {
    ESP_LOGE(TAG, "unexpected manufacturer ID 0x%02X (expected 0x%02X)", manufacturer_id,
             EXPECTED_MANUFACTURER_ID);
    _rm_device();
    return false;
  }

  // Apply sensitivity, thresholds, channel enable, and interrupt config.
  if (!_apply_config()) {
    ESP_LOGE(TAG, "apply config failed");
    _rm_device();
    return false;
  }

  // Clear any pending interrupt so the INT line starts deasserted (HIGH).
  // Without this, a stale touch event from before init could leave INT LOW,
  // preventing the first falling-edge ISR from ever firing.
  clear_interrupt();

  ESP_LOGI(TAG, "initialised at 0x%02X", _address);
  return true;
}

bool CAP1203::read(TouchData &out) {
  out.touched = 0;
  out.noise = 0;

  if (_dev_handle == nullptr) {
    return false;
  }

  // Pure read: does not clear the INT latch. The CAP1203 holds
  // SENSOR_INPUT_STATUS until the INT bit is cleared, so the caller must call
  // clear_interrupt() to acknowledge and advance to fresh state.
  if (!_read_reg(REG_SENSOR_INPUT_STATUS, out.touched)) {
    return false;
  }

  if (!_read_reg(REG_NOISE_FLAG_STATUS, out.noise)) {
    return false;
  }

  return true;
}

bool CAP1203::calibrate(uint8_t channel_mask) {
  return _write_reg(REG_CALIBRATION_ACTIVATE, channel_mask);
}

bool CAP1203::clear_interrupt() {
  if (_dev_handle == nullptr) {
    return false;
  }

  uint8_t main_ctrl = 0;
  if (!_read_reg(REG_MAIN_CONTROL, main_ctrl)) {
    return false;
  }

  // Clear the INT bit (bit 0) if set.
  if (main_ctrl & 0x01) {
    main_ctrl &= static_cast<uint8_t>(~0x01u);
    if (!_write_reg(REG_MAIN_CONTROL, main_ctrl)) {
      return false;
    }
  }

  // Read GENERAL_STATUS to latch-clear the touch event flags.
  uint8_t status = 0;
  return _read_reg(REG_GENERAL_STATUS, status);
}

bool CAP1203::_read_reg(uint8_t reg, uint8_t &out) {
  if (_dev_handle == nullptr) {
    return false;
  }
  return i2c_master_transmit_receive(_dev_handle, &reg, 1, &out, 1, _config.timeout_ms) == ESP_OK;
}

bool CAP1203::_write_reg(uint8_t reg, uint8_t val) {
  if (_dev_handle == nullptr) {
    return false;
  }
  const uint8_t buf[2] = {reg, val};
  return i2c_master_transmit(_dev_handle, buf, sizeof(buf), _config.timeout_ms) == ESP_OK;
}

bool CAP1203::_apply_config() {
  if (_config.delta_sense > 7) {
    ESP_LOGE(TAG, "delta_sense %u out of range (0-7)", _config.delta_sense);
    return false;
  }
  if (_config.base_shift > 0x0F) {
    ESP_LOGE(TAG, "base_shift %u out of range (0-15)", _config.base_shift);
    return false;
  }
  if (_config.threshold_ch1 > 127 || _config.threshold_ch2 > 127 || _config.threshold_ch3 > 127) {
    ESP_LOGE(TAG, "threshold out of range (0-127)");
    return false;
  }
  if (_config.max_duration_code > 0x0F) {
    ESP_LOGE(TAG, "max_duration_code %u out of range (0-15)", _config.max_duration_code);
    return false;
  }
  if (!_apply_max_duration()) {
    return false;
  }

  const uint8_t sens = static_cast<uint8_t>((_config.delta_sense << 4) | _config.base_shift);

  return _write_reg(REG_SENSITIVITY_CONTROL, sens) &&
         _write_reg(REG_SENSOR_INPUT_ENABLE, _config.enabled_channels) &&
         _write_reg(REG_INTERRUPT_ENABLE, _config.interrupt_channels) &&
         _write_reg(REG_REPEAT_RATE_ENABLE, _config.repeat_rate_channels) &&
         _write_reg(REG_THRESHOLD_CH1, _config.threshold_ch1) &&
         _write_reg(REG_THRESHOLD_CH2, _config.threshold_ch2) &&
         _write_reg(REG_THRESHOLD_CH3, _config.threshold_ch3);
}

// Both fields share a register with settings the driver does not own, so each
// is a read-modify-write rather than a blind byte.
bool CAP1203::_apply_max_duration() {
  uint8_t cfg = 0;
  if (!_read_reg(REG_CONFIGURATION, cfg)) {
    return false;
  }
  constexpr uint8_t MAX_DUR_EN = 0x08; // Configuration bit 3
  const uint8_t want_cfg = _config.max_duration_recal ? static_cast<uint8_t>(cfg | MAX_DUR_EN)
                                                      : static_cast<uint8_t>(cfg & ~MAX_DUR_EN);
  if (want_cfg != cfg && !_write_reg(REG_CONFIGURATION, want_cfg)) {
    return false;
  }
  if (!_config.max_duration_recal) {
    return true; // the duration itself only matters once it is enabled
  }

  uint8_t input_cfg = 0;
  if (!_read_reg(REG_SENSOR_INPUT_CONFIG, input_cfg)) {
    return false;
  }
  // Top nibble is MAX_DUR, bottom nibble is the repeat rate, which stays.
  const uint8_t want_input =
      static_cast<uint8_t>((_config.max_duration_code << 4) | (input_cfg & 0x0F));
  if (want_input != input_cfg && !_write_reg(REG_SENSOR_INPUT_CONFIG, want_input)) {
    return false;
  }
  return true;
}

void CAP1203::_rm_device() {
  if (_dev_handle == nullptr) {
    return;
  }
  i2c_master_bus_rm_device(_dev_handle);
  _dev_handle = nullptr;
}
