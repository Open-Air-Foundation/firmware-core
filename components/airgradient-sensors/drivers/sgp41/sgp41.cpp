/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/sgp41/sgp41.h"
#include "esp_log.h"
#include "rtos.h"
#include <cstring>

static constexpr const char *TAG = "SGP41";

SGP41::SGP41(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : _i2c_bus(i2c_bus), _dev_handle(nullptr), _address(address), _has_compensation(false),
      _comp_temperature(DEFAULT_TEMPERATURE), _comp_humidity(DEFAULT_HUMIDITY) {}

bool SGP41::init() {
  // Probe I2C bus to verify device exists
  esp_err_t ret = i2c_master_probe(_i2c_bus, _address, 1000);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to probe SGP41 at address 0x%02X: %s", _address, esp_err_to_name(ret));
    return false;
  }

  ESP_LOGI(TAG, "SGP41 found at address 0x%02X", _address);

  // Add device to I2C bus
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _address,
      .scl_speed_hz = 100000, // 100kHz for SGP41
      .scl_wait_us = 0,
      .flags = {},
  };

  ret = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add SGP41 device: %s", esp_err_to_name(ret));
    return false;
  }

  ESP_LOGI(TAG, "SGP41 initialized successfully");
  return true;
}

bool SGP41::turn_heater_off() {
  if (_dev_handle == nullptr || !_sendCommand(CMD_HEATER_OFF)) {
    return false;
  }
  RTOS::delay_ms(HEATER_OFF_DELAY_MS);
  return true;
}

bool SGP41::read(TVOCNOxData &out) {
  // Initialize to invalid sentinels
  out.tvoc_index = MeasuresInvalid::TVOC;
  out.tvoc_raw = MeasuresInvalid::TVOC;
  out.nox_index = MeasuresInvalid::NOX;
  out.nox_raw = MeasuresInvalid::NOX;

  if (_dev_handle == nullptr) {
    ESP_LOGE(TAG, "Device not initialized");
    return false;
  }

  // Get compensation values (stored or defaults)
  float temp = _has_compensation ? _comp_temperature : DEFAULT_TEMPERATURE;
  float hum = _has_compensation ? _comp_humidity : DEFAULT_HUMIDITY;

  // Send measure raw signals command with parameters
  if (!_sendCommandWithParams(CMD_MEASURE_RAW, _humidityToTicks(hum), _temperatureToTicks(temp))) {
    ESP_LOGW(TAG, "Failed to send measure command");
    return false;
  }

  // Wait for measurement to complete
  RTOS::delay_ms(MEASURE_DELAY_MS);

  // Read raw signals
  uint16_t vocRaw, noxRaw;
  if (!_readRawSignals(vocRaw, noxRaw)) {
    ESP_LOGW(TAG, "Failed to read raw signals");
    return false;
  }

  // Convert to int and populate output (only raw values, leave index as -1)
  out.tvoc_raw = static_cast<int>(vocRaw);
  out.nox_raw = static_cast<int>(noxRaw);

  ESP_LOGD(TAG, "TVOC raw: %d, NOx raw: %d", out.tvoc_raw, out.nox_raw);

  return true;
}

void SGP41::set_compensation(float temperature_c, float humidity_pct) {
  // Clamp to sensor-supported range
  if (temperature_c < MIN_TEMPERATURE || temperature_c > MAX_TEMPERATURE) {
    ESP_LOGW(TAG, "Temperature out of range: %.2f (valid: %.1f to %.1f)", temperature_c,
             MIN_TEMPERATURE, MAX_TEMPERATURE);
    return;
  }
  if (humidity_pct < MIN_HUMIDITY || humidity_pct > MAX_HUMIDITY) {
    ESP_LOGW(TAG, "Humidity out of range: %.2f (valid: %.1f to %.1f)", humidity_pct, MIN_HUMIDITY,
             MAX_HUMIDITY);
    return;
  }

  _comp_temperature = temperature_c;
  _comp_humidity = humidity_pct;
  _has_compensation = true;

  ESP_LOGD(TAG, "Compensation set: T=%.2f°C, RH=%.2f%%", temperature_c, humidity_pct);
}

bool SGP41::run_conditioning() {
  if (_dev_handle == nullptr) {
    ESP_LOGE(TAG, "Device not initialized");
    return false;
  }

  // Use stored or default compensation values
  float temp = _has_compensation ? _comp_temperature : DEFAULT_TEMPERATURE;
  float hum = _has_compensation ? _comp_humidity : DEFAULT_HUMIDITY;

  // Send conditioning command with parameters
  if (!_sendCommandWithParams(CMD_CONDITIONING, _humidityToTicks(hum), _temperatureToTicks(temp))) {
    ESP_LOGW(TAG, "Conditioning command failed");
    return false;
  }

  // Wait for conditioning cycle to complete
  RTOS::delay_ms(CONDITIONING_DELAY_MS);

  // Read and validate VOC raw value with retry logic (required by protocol)
  uint8_t response[3]; // VOC_MSB, VOC_LSB, CRC
  esp_err_t ret;
  uint8_t retry_count = 0;
  do {
    ret = i2c_master_receive(_dev_handle, response, 3, 1000);
    if (ret == ESP_OK) {
      break; // Success, exit retry loop
    }
    // Delay before next retry attempt
    RTOS::delay_ms(RETRY_DELAY_MS);
  } while (++retry_count <= RETRY_MAX);

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to read conditioning response after %d retries: %s", retry_count,
             esp_err_to_name(ret));
    return false;
  }

  // Validate CRC
  if (_calculateCrc8(&response[0], 2) != response[2]) {
    ESP_LOGW(TAG, "CRC mismatch in conditioning response");
    return false;
  }

  return true;
}

uint8_t SGP41::_calculateCrc8(const uint8_t *data, uint8_t len) const {
  // CRC-8 polynomial: 0x31 (x^8 + x^5 + x^4 + 1)
  // Initialization: 0xFF
  uint8_t crc = 0xFF;

  for (uint8_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (uint8_t bit = 8; bit > 0; --bit) {
      if (crc & 0x80) {
        crc = (crc << 1) ^ 0x31;
      } else {
        crc = (crc << 1);
      }
    }
  }

  return crc;
}

uint16_t SGP41::_temperatureToTicks(float temperature) const {
  // Formula from SGP4x datasheet: ticks = (temperature + 45) × 65535 / 175
  return static_cast<uint16_t>((temperature + 45.0f) * 65535.0f / 175.0f);
}

uint16_t SGP41::_humidityToTicks(float humidity) const {
  // Formula from SGP4x datasheet: ticks = humidity × 65535 / 100
  return static_cast<uint16_t>(humidity * 65535.0f / 100.0f);
}

bool SGP41::_sendCommand(uint16_t command) {
  // Prepare command buffer (big-endian)
  uint8_t cmd[CMD_SIZE];
  cmd[0] = (command >> 8) & 0xFF; // MSB
  cmd[1] = command & 0xFF;        // LSB

  // Send command
  esp_err_t ret = i2c_master_transmit(_dev_handle, cmd, CMD_SIZE, 1000);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send command 0x%04X: %s", command, esp_err_to_name(ret));
    return false;
  }

  return true;
}

bool SGP41::_sendCommandWithParams(uint16_t command, uint16_t param1, uint16_t param2) {
  // Prepare command buffer with parameters and CRCs (big-endian)
  uint8_t cmd[CMD_WITH_PARAMS_SIZE];

  // Command (big-endian)
  cmd[0] = (command >> 8) & 0xFF;
  cmd[1] = command & 0xFF;

  // Parameter 1 (humidity) with CRC
  cmd[2] = (param1 >> 8) & 0xFF; // MSB
  cmd[3] = param1 & 0xFF;        // LSB
  cmd[4] = _calculateCrc8(&cmd[2], 2);

  // Parameter 2 (temperature) with CRC
  cmd[5] = (param2 >> 8) & 0xFF; // MSB
  cmd[6] = param2 & 0xFF;        // LSB
  cmd[7] = _calculateCrc8(&cmd[5], 2);

  // Send command with parameters
  esp_err_t ret = i2c_master_transmit(_dev_handle, cmd, CMD_WITH_PARAMS_SIZE, 1000);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to send command 0x%04X with params: %s", command, esp_err_to_name(ret));
    return false;
  }

  return true;
}

bool SGP41::_readRawSignals(uint16_t &vocRaw, uint16_t &noxRaw) {
  // Read response (6 bytes: VOC_MSB, VOC_LSB, VOC_CRC, NOX_MSB, NOX_LSB,
  // NOX_CRC)
  uint8_t response[RESPONSE_SIZE];

  // Read with retry logic (sensor may still be busy)
  esp_err_t ret;
  uint8_t retry_count = 0;
  do {
    ret = i2c_master_receive(_dev_handle, response, RESPONSE_SIZE, 1000);
    if (ret == ESP_OK) {
      break; // Success, exit retry loop
    }
    // Delay before next retry attempt
    RTOS::delay_ms(RETRY_DELAY_MS);
  } while (++retry_count <= RETRY_MAX);

  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to receive raw signals after %d retries: %s", retry_count,
             esp_err_to_name(ret));
    return false;
  }

  // Validate VOC CRC (bytes 0-1, CRC at byte 2)
  if (_calculateCrc8(&response[0], 2) != response[2]) {
    ESP_LOGW(TAG, "VOC CRC mismatch");
    return false;
  }

  // Validate NOx CRC (bytes 3-4, CRC at byte 5)
  if (_calculateCrc8(&response[3], 2) != response[5]) {
    ESP_LOGW(TAG, "NOx CRC mismatch");
    return false;
  }

  // Extract values (big-endian)
  vocRaw = (response[0] << 8) | response[1];
  noxRaw = (response[3] << 8) | response[4];

  return true;
}
