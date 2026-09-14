/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/dps368/dps368.h"

#include <cmath>

#include "esp_log.h"

#include "rtos.h"

static constexpr const char *TAG = "DPS368";

DPS368::DPS368(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : _i2c_bus(i2c_bus), _dev_handle(nullptr), _address(address), _variant(Variant::DPS368), _c0(0),
      _c1(0), _c00(0), _c10(0), _c01(0), _c11(0), _c20(0), _c21(0), _c30(0), _c31(0), _c40(0),
      _last_traw_sc(0.0f), _last_temp_hum{MeasuresInvalid::TEMPERATURE, MeasuresInvalid::HUMIDITY} {
}

bool DPS368::init() {
  // Probe I2C bus to verify device exists
  esp_err_t ret = i2c_master_probe(_i2c_bus, _address, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to probe DPS368 at address 0x%02X: %s", _address, esp_err_to_name(ret));
    return false;
  }

  ESP_LOGI(TAG, "DPS368 found at address 0x%02X", _address);

  // Add device to I2C bus
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _address,
      .scl_speed_hz = I2C_CLOCK_HZ,
      .scl_wait_us = 0,
      .flags = {},
  };

  ret = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add DPS368 device: %s", esp_err_to_name(ret));
    return false;
  }

  // Verify product ID
  uint8_t prod_id = 0;
  if (!_read_register(REG_ID, &prod_id, 1)) {
    ESP_LOGE(TAG, "Failed to read product ID");
    return false;
  }

  if (prod_id == PRODUCT_ID_SPL07_003) {
    _variant = Variant::SPL07003;
    ESP_LOGI(TAG, "SPL07-003 detected (ID 0x%02X)", prod_id);
  } else {
    _variant = Variant::DPS368;
    if (prod_id != PRODUCT_ID_DPS368) {
      ESP_LOGW(TAG, "Unexpected product ID: 0x%02X (expected 0x%02X)", prod_id, PRODUCT_ID_DPS368);
    }
    ESP_LOGI(TAG, "DPS368 Product ID: 0x%02X", prod_id);
  }

  // Soft reset
  if (!_write_register(REG_RESET, SOFT_RESET_CMD)) {
    ESP_LOGE(TAG, "Failed to reset sensor");
    return false;
  }

  RTOS::delay_ms(RESET_DELAY_MS);

  // Wait for calibration coefficients to be ready
  uint8_t meas_cfg = 0;
  bool coef_ready = false;
  for (int i = 0; i < COEF_POLL_MAX_ATTEMPTS; i++) {
    if (_read_register(REG_MEAS_CFG, &meas_cfg, 1) && (meas_cfg & MEAS_CFG_COEF_RDY)) {
      coef_ready = true;
      break;
    }
    RTOS::delay_ms(COEF_POLL_DELAY_MS);
  }

  if (!coef_ready) {
    ESP_LOGW(TAG, "Coefficients not ready, sensor may not work properly");
  }

  // Read factory calibration coefficients
  if (!_read_coefficients()) {
    ESP_LOGE(TAG, "Failed to read calibration coefficients");
    return false;
  }

  // Configure pressure: 1 meas/sec, 16x oversampling
  if (!_write_register(REG_PRS_CFG, PRS_CFG_16X)) {
    ESP_LOGE(TAG, "Failed to configure pressure measurement");
    return false;
  }

  // Configure temperature: 1 meas/sec, 16x oversampling.  The DPS368 selects
  // the external MEMS sensor with TMP_CFG bit 7; on the SPL07-003 that bit is
  // part of the rate field, and the sensor select lives in MEAS_CFG bit 3.
  const bool spl07 = _variant == Variant::SPL07003;
  if (!_write_register(REG_TMP_CFG, spl07 ? TMP_CFG_16X : TMP_CFG_EXT_16X)) {
    ESP_LOGE(TAG, "Failed to configure temperature measurement");
    return false;
  }

  // Enable result bit-shift (required for >8x oversampling, datasheet Table 9)
  if (!_write_register(REG_CFG_REG, CFG_SHIFT_ENABLE)) {
    ESP_LOGE(TAG, "Failed to enable result bit shift");
    return false;
  }

  // Start continuous pressure + temperature measurement
  const uint8_t meas_ctrl = spl07 ? (MODE_CONTINUOUS | MEAS_CFG_TMP_EXT_SPL07) : MODE_CONTINUOUS;
  if (!_write_register(REG_MEAS_CFG, meas_ctrl)) {
    ESP_LOGE(TAG, "Failed to start continuous measurement");
    return false;
  }

  ESP_LOGI(TAG, "%s initialized successfully", spl07 ? "SPL07-003" : "DPS368");
  return true;
}

bool DPS368::read(PressureData &out) {
  // Initialize to invalid sentinels
  out.pressure = MeasuresInvalid::PRESSURE;
  out.altitude = MeasuresInvalid::ALTITUDE;

  if (_dev_handle == nullptr) {
    ESP_LOGE(TAG, "Device not initialized");
    return false;
  }

  // Check if measurements are ready
  uint8_t meas_cfg = 0;
  if (!_read_register(REG_MEAS_CFG, &meas_cfg, 1)) {
    ESP_LOGW(TAG, "Failed to read measurement status");
    return false;
  }

  bool prs_ready = (meas_cfg & MEAS_CFG_PRS_RDY) != 0;
  bool tmp_ready = (meas_cfg & MEAS_CFG_TMP_RDY) != 0;

  if (!prs_ready && !tmp_ready) {
    ESP_LOGD(TAG, "No new measurements available");
    return false;
  }

  // Read temperature first (needed for pressure compensation)
  if (tmp_ready) {
    uint8_t tmp_raw[3];
    if (!_read_register(REG_TMP_B2, tmp_raw, 3)) {
      ESP_LOGW(TAG, "Failed to read temperature registers");
      return false;
    }

    int32_t tmp_raw_val = ((int32_t)tmp_raw[0] << 16) | ((int32_t)tmp_raw[1] << 8) | tmp_raw[2];
    tmp_raw_val = _sign_extend_24bit(tmp_raw_val);

    // Scaled temperature (datasheet Section 4.9.2)
    _last_traw_sc = static_cast<float>(tmp_raw_val) / SCALE_FACTOR;

    // Compute and cache compensated temperature for temp_hum_data()
    _last_temp_hum.temperature =
        static_cast<float>(_c0) * 0.5f + static_cast<float>(_c1) * _last_traw_sc;
    // DPS368 does not measure humidity
    _last_temp_hum.humidity = MeasuresInvalid::HUMIDITY;
  }

  // Read and compensate pressure
  if (prs_ready) {
    uint8_t psr_raw[3];
    if (!_read_register(REG_PSR_B2, psr_raw, 3)) {
      ESP_LOGW(TAG, "Failed to read pressure registers");
      return false;
    }

    int32_t psr_raw_val = ((int32_t)psr_raw[0] << 16) | ((int32_t)psr_raw[1] << 8) | psr_raw[2];
    psr_raw_val = _sign_extend_24bit(psr_raw_val);

    // Scaled pressure
    float psr_sc = static_cast<float>(psr_raw_val) / SCALE_FACTOR;

    // Full compensation formula (DPS368 datasheet Section 4.9.1, SPL07-003
    // datasheet Section 4.6.1; c31/c40 are zero on the DPS368):
    // Pcomp = c00 + Praw_sc*(c10 + Praw_sc*(c20 + Praw_sc*(c30 + Praw_sc*c40)))
    //       + Traw_sc*c01
    //       + Traw_sc*Praw_sc*(c11 + Praw_sc*(c21 + Praw_sc*c31))
    float pressure_pa =
        static_cast<float>(_c00) +
        psr_sc * (static_cast<float>(_c10) +
                  psr_sc * (static_cast<float>(_c20) +
                            psr_sc * (static_cast<float>(_c30) +
                                      psr_sc * static_cast<float>(_c40)))) +
        _last_traw_sc * static_cast<float>(_c01) +
        _last_traw_sc * psr_sc *
            (static_cast<float>(_c11) +
             psr_sc * (static_cast<float>(_c21) + psr_sc * static_cast<float>(_c31)));

    float pressure_hpa = pressure_pa / 100.0f;
    out.pressure = pressure_hpa;
    out.altitude = _calculate_altitude(pressure_hpa);

    ESP_LOGD(TAG, "Pressure: %.2f hPa, Altitude: %.1f m", out.pressure, out.altitude);
  }

  return true;
}

bool DPS368::supports_temp_hum() const { return true; }

TempHumData DPS368::temp_hum_data() { return _last_temp_hum; }

// --- Private methods ---

bool DPS368::_read_register(uint8_t reg, uint8_t *data, size_t len) {
  esp_err_t ret = i2c_master_transmit_receive(_dev_handle, &reg, 1, data, len, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGD(TAG, "I2C read failed for reg 0x%02X: %s", reg, esp_err_to_name(ret));
    return false;
  }
  return true;
}

bool DPS368::_write_register(uint8_t reg, uint8_t value) {
  uint8_t buf[2] = {reg, value};
  esp_err_t ret = i2c_master_transmit(_dev_handle, buf, 2, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGD(TAG, "I2C write failed for reg 0x%02X: %s", reg, esp_err_to_name(ret));
    return false;
  }
  return true;
}

bool DPS368::_read_coefficients() {
  uint8_t coef[COEF_BUFFER_SIZE_SPL07] = {};
  const uint8_t coef_len =
      _variant == Variant::SPL07003 ? COEF_BUFFER_SIZE_SPL07 : COEF_BUFFER_SIZE_DPS368;

  if (!_read_register(REG_COEF_START, coef, coef_len)) {
    return false;
  }

  // Parse coefficients per datasheet Section 8.11 (Table 18)

  // c0: 12-bit 2's complement (bytes 0-1)
  _c0 = ((int32_t)coef[0] << 4) | ((coef[1] >> 4) & 0x0F);
  _c0 = _sign_extend_12bit(_c0);

  // c1: 12-bit 2's complement (bytes 1-2)
  _c1 = (((int32_t)(coef[1] & 0x0F) << 8) | coef[2]);
  _c1 = _sign_extend_12bit(_c1);

  // c00: 20-bit 2's complement (bytes 3-5)
  _c00 = ((int32_t)coef[3] << 12) | ((int32_t)coef[4] << 4) | ((coef[5] >> 4) & 0x0F);
  _c00 = _sign_extend_20bit(_c00);

  // c10: 20-bit 2's complement (bytes 5-7)
  _c10 = (((int32_t)(coef[5] & 0x0F) << 16) | ((int32_t)coef[6] << 8) | coef[7]);
  _c10 = _sign_extend_20bit(_c10);

  // c01: 16-bit 2's complement (bytes 8-9)
  _c01 = ((int32_t)coef[8] << 8) | coef[9];
  _c01 = _sign_extend_16bit(_c01);

  // c11: 16-bit 2's complement (bytes 10-11)
  _c11 = ((int32_t)coef[10] << 8) | coef[11];
  _c11 = _sign_extend_16bit(_c11);

  // c20: 16-bit 2's complement (bytes 12-13)
  _c20 = ((int32_t)coef[12] << 8) | coef[13];
  _c20 = _sign_extend_16bit(_c20);

  // c21: 16-bit 2's complement (bytes 14-15)
  _c21 = ((int32_t)coef[14] << 8) | coef[15];
  _c21 = _sign_extend_16bit(_c21);

  // c30: 16-bit 2's complement (bytes 16-17)
  _c30 = ((int32_t)coef[16] << 8) | coef[17];
  _c30 = _sign_extend_16bit(_c30);

  // SPL07-003 only (datasheet Table 10): c31 12-bit (0x22, 0x23[7:4]),
  // c40 12-bit (0x23[3:0], 0x24)
  if (_variant == Variant::SPL07003) {
    _c31 = ((int32_t)coef[18] << 4) | ((coef[19] >> 4) & 0x0F);
    _c31 = _sign_extend_12bit(_c31);
    _c40 = (((int32_t)(coef[19] & 0x0F) << 8) | coef[20]);
    _c40 = _sign_extend_12bit(_c40);
  } else {
    _c31 = 0;
    _c40 = 0;
  }

  ESP_LOGI(TAG, "Calibration: c0=%ld c1=%ld", _c0, _c1);
  ESP_LOGI(TAG, "Calibration: c00=%ld c10=%ld c01=%ld", _c00, _c10, _c01);
  ESP_LOGI(TAG, "Calibration: c11=%ld c20=%ld c21=%ld c30=%ld c31=%ld c40=%ld", _c11, _c20, _c21,
           _c30, _c31, _c40);

  return true;
}

int32_t DPS368::_sign_extend_24bit(int32_t value) {
  if (value & 0x800000) {
    value -= 0x1000000;
  }
  return value;
}

int32_t DPS368::_sign_extend_20bit(int32_t value) {
  if (value & 0x80000) {
    value -= 0x100000;
  }
  return value;
}

int32_t DPS368::_sign_extend_16bit(int32_t value) {
  if (value & 0x8000) {
    value -= 0x10000;
  }
  return value;
}

int32_t DPS368::_sign_extend_12bit(int32_t value) {
  if (value & 0x800) {
    value -= 0x1000;
  }
  return value;
}

float DPS368::_calculate_altitude(float pressure_hpa) {
  // Standard barometric formula (ISA model)
  // altitude = 44330 * (1 - (P / P0) ^ 0.1903)
  // where P0 = standard sea-level pressure (1013.25 hPa)
  if (pressure_hpa <= 0.0f) {
    return MeasuresInvalid::ALTITUDE;
  }
  return 44330.0f * (1.0f - std::pow(pressure_hpa / SEA_LEVEL_PRESSURE_HPA, 0.1903f));
}
