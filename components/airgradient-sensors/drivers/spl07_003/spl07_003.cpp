/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/spl07_003/spl07_003.h"

#include <cmath>

#include "esp_log.h"

#include "rtos.h"

static constexpr const char *TAG = "SPL07-003";

SPL07_003::SPL07_003(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : _i2c_bus(i2c_bus), _dev_handle(nullptr), _address(address), _initialized(false), _c0(0),
      _c1(0), _c00(0), _c10(0), _c01(0), _c11(0), _c20(0), _c21(0), _c30(0), _c31(0), _c40(0),
      _last_temp_hum{} {}

SPL07_003::~SPL07_003() { _release_device(); }

bool SPL07_003::init() {
  if (_initialized) {
    return true;
  }

  if (_i2c_bus == nullptr || (_address != ADDRESS_SDO_GND && _address != ADDRESS_SDO_VDD)) {
    ESP_LOGE(TAG, "Invalid I2C bus or address");
    return false;
  }

  // Retry cleanup if a previous failed initialization could not release its handle.
  if (!_release_device()) {
    return false;
  }

  esp_err_t ret = i2c_master_probe(_i2c_bus, _address, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Probe failed at address 0x%02X: %s", _address, esp_err_to_name(ret));
    return false;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _address,
      .scl_speed_hz = I2C_CLOCK_HZ,
      .scl_wait_us = 0,
      .flags = {},
  };
  ret = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add device: %s", esp_err_to_name(ret));
    _dev_handle = nullptr;
    return false;
  }

  if (!_configure()) {
    _release_device();
    return false;
  }

  _initialized = true;
  ESP_LOGI(TAG, "Initialized at address 0x%02X", _address);
  return true;
}

bool SPL07_003::_configure() {
  uint8_t id = 0;
  if (!_read_register(REG_ID, &id, sizeof(id))) {
    return false;
  }
  // Verify identity before writing anything: DPS368 can occupy the same address.
  if (id != PRODUCT_REVISION_ID) {
    ESP_LOGE(TAG, "Unexpected product/revision ID: 0x%02X (expected 0x%02X)", id,
             PRODUCT_REVISION_ID);
    return false;
  }

  if (!_write_register(REG_RESET, SOFT_RESET_CMD)) {
    return false;
  }
  RTOS::delay_ms(RESET_DELAY_MS);

  bool ready = false;
  for (int attempt = 0; attempt < READY_POLL_MAX_ATTEMPTS; ++attempt) {
    uint8_t status = 0;
    if (!_read_register(REG_MEAS_CFG, &status, sizeof(status))) {
      return false;
    }
    if ((status & INIT_READY_MASK) == INIT_READY_MASK) {
      ready = true;
      break;
    }
    RTOS::delay_ms(READY_POLL_DELAY_MS);
  }
  if (!ready) {
    ESP_LOGE(TAG, "Sensor/calibration readiness timed out");
    return false;
  }

  if (!_read_coefficients()) {
    return false;
  }

  return _write_register(REG_PRS_CFG, PRS_CFG_1HZ_16X) &&
         _write_register(REG_TMP_CFG, TMP_CFG_1HZ_16X) &&
         _write_register(REG_CFG_REG, CFG_SHIFT_ENABLE) &&
         _write_register(REG_MEAS_CFG, MODE_CONTINUOUS_INTERNAL);
}

bool SPL07_003::read(PressureData &out) {
  out = PressureData{};
  _last_temp_hum = TempHumData{};

  if (!_initialized || _dev_handle == nullptr) {
    ESP_LOGW(TAG, "Device not initialized");
    return false;
  }

  uint8_t status = 0;
  if (!_read_register(REG_MEAS_CFG, &status, sizeof(status))) {
    return false;
  }
  // Leave both result registers unread until a fresh pair is available. This
  // avoids compensating pressure with zero or an indefinitely cached temperature.
  if ((status & DATA_READY_MASK) != DATA_READY_MASK) {
    return false;
  }

  uint8_t raw[RAW_BUFFER_SIZE];
  if (!_read_register(REG_PSR_B2, raw, sizeof(raw))) {
    return false;
  }
  int32_t pressure_raw =
      (static_cast<int32_t>(raw[0]) << 16) | (static_cast<int32_t>(raw[1]) << 8) | raw[2];
  int32_t temperature_raw =
      (static_cast<int32_t>(raw[3]) << 16) | (static_cast<int32_t>(raw[4]) << 8) | raw[5];
  float p = static_cast<float>(_sign_extend(pressure_raw, 24)) / SCALE_FACTOR;
  float t = static_cast<float>(_sign_extend(temperature_raw, 24)) / SCALE_FACTOR;

  TempHumData temperature;
  temperature.temperature =
      static_cast<float>(_c0) * TEMPERATURE_OFFSET_SCALE + static_cast<float>(_c1) * t;
  if (!temperature.is_temp_valid() || temperature.temperature > MAX_SENSOR_TEMPERATURE_C) {
    ESP_LOGW(TAG, "Invalid compensation temperature");
    return false;
  }
  _last_temp_hum = temperature;

  // Datasheet section 4.6.1: evaluate each polynomial from highest power down.
  // Pressure contribution: c00 + c10*p + c20*p^2 + c30*p^3 + c40*p^4.
  float pressure_polynomial = static_cast<float>(_c30) + p * static_cast<float>(_c40);
  pressure_polynomial = static_cast<float>(_c20) + p * pressure_polynomial;
  pressure_polynomial = static_cast<float>(_c10) + p * pressure_polynomial;
  pressure_polynomial = static_cast<float>(_c00) + p * pressure_polynomial;

  // Temperature correction: t * (c01 + c11*p + c21*p^2 + c31*p^3).
  float temperature_coefficient = static_cast<float>(_c21) + p * static_cast<float>(_c31);
  temperature_coefficient = static_cast<float>(_c11) + p * temperature_coefficient;
  temperature_coefficient = static_cast<float>(_c01) + p * temperature_coefficient;

  const float pressure_pa = pressure_polynomial + t * temperature_coefficient;

  PressureData data;
  data.pressure = pressure_pa / PA_PER_HPA;
  if (!data.is_pressure_valid()) {
    ESP_LOGW(TAG, "Invalid compensated pressure");
    return false;
  }
  out.pressure = data.pressure;
  data.altitude = _calculate_altitude(data.pressure);
  if (data.is_altitude_valid()) {
    out.altitude = data.altitude;
  }

  ESP_LOGD(TAG, "Pressure: %.2f hPa, Altitude: %.1f m", out.pressure, out.altitude);
  return true;
}

bool SPL07_003::supports_temp_hum() const { return true; }

TempHumData SPL07_003::temp_hum_data() { return _last_temp_hum; }

bool SPL07_003::_read_register(uint8_t reg, uint8_t *data, size_t len) {
  if (_dev_handle == nullptr) {
    return false;
  }
  esp_err_t ret = i2c_master_transmit_receive(_dev_handle, &reg, 1, data, len, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "I2C read failed for register 0x%02X: %s", reg, esp_err_to_name(ret));
    return false;
  }
  return true;
}

bool SPL07_003::_write_register(uint8_t reg, uint8_t value) {
  if (_dev_handle == nullptr) {
    return false;
  }
  uint8_t buf[2] = {reg, value};
  esp_err_t ret = i2c_master_transmit(_dev_handle, buf, sizeof(buf), I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "I2C write failed for register 0x%02X: %s", reg, esp_err_to_name(ret));
    return false;
  }
  return true;
}

bool SPL07_003::_read_coefficients() {
  uint8_t coef[COEF_BUFFER_SIZE];
  if (!_read_register(REG_COEF_START, coef, sizeof(coef))) {
    return false;
  }

  // SPL07-003 datasheet section 7.11: packed signed 12-, 20-, and 16-bit values.
  _c0 = _sign_extend((static_cast<int32_t>(coef[0]) << 4) | (coef[1] >> 4), 12);
  _c1 = _sign_extend((static_cast<int32_t>(coef[1] & 0x0F) << 8) | coef[2], 12);
  _c00 = _sign_extend((static_cast<int32_t>(coef[3]) << 12) | (static_cast<int32_t>(coef[4]) << 4) |
                          (coef[5] >> 4),
                      20);
  _c10 = _sign_extend((static_cast<int32_t>(coef[5] & 0x0F) << 16) |
                          (static_cast<int32_t>(coef[6]) << 8) | coef[7],
                      20);
  _c01 = _sign_extend((static_cast<int32_t>(coef[8]) << 8) | coef[9], 16);
  _c11 = _sign_extend((static_cast<int32_t>(coef[10]) << 8) | coef[11], 16);
  _c20 = _sign_extend((static_cast<int32_t>(coef[12]) << 8) | coef[13], 16);
  _c21 = _sign_extend((static_cast<int32_t>(coef[14]) << 8) | coef[15], 16);
  _c30 = _sign_extend((static_cast<int32_t>(coef[16]) << 8) | coef[17], 16);
  _c31 = _sign_extend((static_cast<int32_t>(coef[18]) << 4) | (coef[19] >> 4), 12);
  _c40 = _sign_extend((static_cast<int32_t>(coef[19] & 0x0F) << 8) | coef[20], 12);
  return true;
}

bool SPL07_003::_release_device() {
  _initialized = false;
  _last_temp_hum = TempHumData{};
  if (_dev_handle == nullptr) {
    return true;
  }
  esp_err_t ret = i2c_master_bus_rm_device(_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to remove device: %s", esp_err_to_name(ret));
    return false;
  }
  _dev_handle = nullptr;
  return true;
}

int32_t SPL07_003::_sign_extend(int32_t value, uint8_t bits) {
  if (value & (int32_t{1} << (bits - 1))) {
    value -= int32_t{1} << bits;
  }
  return value;
}

float SPL07_003::_calculate_altitude(float pressure_hpa) {
  // Match the existing pressure driver's standard-atmosphere altitude convention.
  return ALTITUDE_SCALE_M *
         (1.0f - std::pow(pressure_hpa / SEA_LEVEL_PRESSURE_HPA, ALTITUDE_EXPONENT));
}
