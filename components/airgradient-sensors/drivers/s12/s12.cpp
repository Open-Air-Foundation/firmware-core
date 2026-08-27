/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/s12/s12.h"

#include "esp_log.h"

#include "rtos.h"

static constexpr const char *TAG = "S12";

S12::S12(i2c_master_bus_handle_t i2c_bus, uint8_t address, uint8_t co2_reg_hi)
    : _i2c_bus(i2c_bus), _dev_handle(nullptr), _address(address), _co2_reg_hi(co2_reg_hi),
      _initialized(false), _is_calibrating(false) {}

bool S12::init() {
  if (_initialized) {
    ESP_LOGW(TAG, "init() called more than once, ignoring");
    return true;
  }

  // Probe I2C bus to verify device exists (with retry for boot timing).
  bool probed = false;
  for (int i = 0; i < INIT_PROBE_RETRIES; i++) {
    esp_err_t ret = i2c_master_probe(_i2c_bus, _address, I2C_TIMEOUT_MS);
    if (ret == ESP_OK) {
      probed = true;
      break;
    }
    ESP_LOGW(TAG, "Probe attempt %d/%d failed: %s", i + 1, INIT_PROBE_RETRIES,
             esp_err_to_name(ret));
    RTOS::delay_ms(INIT_PROBE_DELAY_MS);
  }
  if (!probed) {
    ESP_LOGE(TAG, "S12 not found at address 0x%02X", _address);
    return false;
  }

  ESP_LOGI(TAG, "S12 found at address 0x%02X", _address);

  // Add device to I2C bus.
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _address,
      .scl_speed_hz = I2C_CLOCK_HZ,
      .scl_wait_us = 0,
      .flags = {},
  };

  esp_err_t ret = i2c_master_bus_add_device(_i2c_bus, &dev_cfg, &_dev_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add S12 device: %s", esp_err_to_name(ret));
    _dev_handle = nullptr;
    return false;
  }

  _initialized = true;
  ESP_LOGI(TAG, "S12 initialized (CO2 reg 0x%02X)", _co2_reg_hi);
  return true;
}

bool S12::read(CO2Data &out) {
  // Initialize to invalid sentinel before any early return.
  out.co2 = MeasuresInvalid::CO2;

  if (!_initialized || _dev_handle == nullptr) {
    ESP_LOGW(TAG, "Sensor not initialized");
    return false;
  }

  uint8_t data[CO2_REG_READ_LEN] = {0};
  if (!_read_register(_co2_reg_hi, data, sizeof(data))) {
    ESP_LOGE(TAG, "Failed to read CO2 register 0x%02X", _co2_reg_hi);
    return false;
  }

  // Big-endian 16-bit value directly in ppm.
  uint16_t raw = static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
  out.co2 = static_cast<int>(raw);

  ESP_LOGD(TAG, "CO2: %d ppm", out.co2);
  return true;
}

TempHumData S12::temp_hum_data() {
  return TempHumData{MeasuresInvalid::TEMPERATURE, MeasuresInvalid::HUMIDITY};
}

bool S12::do_baseline_calibration(int baseline_ppm) {
  if (!_initialized || _dev_handle == nullptr) {
    ESP_LOGW(TAG, "Sensor not initialized, cannot start calibration");
    return false;
  }

  // Clamp invalid / sentinel inputs to the Senseair-recommended default.
  uint16_t baseline_concentration_ppm =
      (baseline_ppm <= 0) ? CAL_DEFAULT_BASELINE_PPM : static_cast<uint16_t>(baseline_ppm);

  // Clear any lingering calibration-in-progress state before writing registers
  // so a failed start never leaves us "calibrating" forever.
  _is_calibrating = false;

  // 1. Clear previous calibration status (single-byte write to 0x81).
  const uint8_t clear_frame[] = {REG_CALIBRATION_STATUS, 0x00};
  if (!_write_bytes(clear_frame, sizeof(clear_frame))) {
    ESP_LOGE(TAG, "Failed to clear calibration status");
    return false;
  }

  // 2. Write calibration target concentration (big-endian) to 0x84/0x85.
  const uint8_t baseline_frame[] = {
      REG_CALIBRATION_TARGET_MSB,
      static_cast<uint8_t>((baseline_concentration_ppm >> 8) & 0xFF),
      static_cast<uint8_t>(baseline_concentration_ppm & 0xFF),
  };
  if (!_write_bytes(baseline_frame, sizeof(baseline_frame))) {
    ESP_LOGE(TAG, "Failed to write baseline concentration (%u ppm)", baseline_concentration_ppm);
    return false;
  }

  // 3. Issue the target calibration command (0x7C05 to 0x82/0x83).
  const uint8_t cmd_frame[] = {REG_CALIBRATION_COMMAND_MSB, CAL_PREFIX, CAL_TARGET};
  if (!_write_bytes(cmd_frame, sizeof(cmd_frame))) {
    ESP_LOGE(TAG, "Failed to issue baseline calibration command");
    return false;
  }

  _is_calibrating = true;
  ESP_LOGI(TAG, "Baseline calibration started (baseline=%u ppm)", baseline_concentration_ppm);
  return true;
}

bool S12::is_baseline_calibration_done() {
  // Idle contract: if no calibration is in progress, report "done" so callers
  // that never started one aren't blocked waiting on us.
  if (!_is_calibrating) {
    return true;
  }

  if (!_initialized || _dev_handle == nullptr) {
    ESP_LOGW(TAG, "Sensor not initialized while polling calibration status");
    _is_calibrating = false;
    return false;
  }

  // Check for a sensor-reported calibration error first: if set, the sensor
  // will never report "done" so we give up and let the orchestrator treat it
  // as a timeout/failure.
  uint8_t err_status = 0;
  if (!_read_register(REG_ERROR_STATUS_LSB, &err_status, 1)) {
    // Transient read failure — stay in "calibrating" and retry next poll.
    ESP_LOGW(TAG, "Failed to read error status during calibration polling");
    return false;
  }
  if ((err_status & ERR_STATUS_CALIBRATION) != 0) {
    ESP_LOGE(TAG, "Sensor reported calibration error (error_status=0x%02X)", err_status);
    _is_calibrating = false;
    return false;
  }

  uint8_t cal_status = 0;
  if (!_read_register(REG_CALIBRATION_STATUS, &cal_status, 1)) {
    ESP_LOGW(TAG, "Failed to read calibration status during polling");
    return false;
  }

  if ((cal_status & CAL_STATUS_TARGET_DONE) != 0) {
    ESP_LOGI(TAG, "Baseline calibration complete (cal_status=0x%02X)", cal_status);
    _is_calibrating = false;
    return true;
  }

  // Still waiting for the next measurement cycle to flip the done bit.
  return false;
}

bool S12::set_abc_period_days(int days) {
  ESP_LOGW(TAG,
           "ABC configuration temporarily ignored. By default its enabled with period 180 hours.");
  return true;

  if (!_initialized || _dev_handle == nullptr) {
    ESP_LOGW(TAG, "Sensor not initialized, cannot configure ABC period");
    return false;
  }
  if (days != ABC_DAYS_DISABLED && (days < ABC_DAYS_MIN || days > ABC_DAYS_MAX)) {
    ESP_LOGW(TAG, "ABC period %d days is outside supported values -1 or %d..%d", days, ABC_DAYS_MIN,
             ABC_DAYS_MAX);
    return false;
  }

  uint8_t meter_control = 0;
  if (!_read_register(REG_METER_CONTROL, &meter_control, sizeof(meter_control))) {
    ESP_LOGE(TAG, "Failed to read ABC meter control");
    return false;
  }

  if (days == ABC_DAYS_DISABLED) {
    if ((meter_control & METER_CONTROL_ABC_DISABLE) == 0) {
      const uint8_t meter_control_frame[] = {
          REG_METER_CONTROL,
          static_cast<uint8_t>(meter_control | METER_CONTROL_ABC_DISABLE),
      };
      if (!_write_bytes(meter_control_frame, sizeof(meter_control_frame))) {
        ESP_LOGE(TAG, "Failed to disable ABC");
        return false;
      }
      RTOS::delay_ms(EEPROM_WRITE_DELAY_MS);
    }
    ESP_LOGI(TAG, "ABC disabled");
    return true;
  }

  const uint16_t requested_hours = static_cast<uint16_t>(days * HOURS_PER_DAY);
  uint8_t period_bytes[2] = {0};
  if (!_read_register(REG_ABC_PERIOD_MSB, period_bytes, sizeof(period_bytes))) {
    ESP_LOGE(TAG, "Failed to read ABC period");
    return false;
  }
  const uint16_t current_hours =
      static_cast<uint16_t>((static_cast<uint16_t>(period_bytes[0]) << 8) | period_bytes[1]);
  if (current_hours != requested_hours) {
    const uint8_t period_frame[] = {
        REG_ABC_PERIOD_MSB,
        static_cast<uint8_t>(requested_hours >> 8),
        static_cast<uint8_t>(requested_hours & 0xFF),
    };
    if (!_write_bytes(period_frame, sizeof(period_frame))) {
      ESP_LOGE(TAG, "Failed to set ABC period to %u hours", requested_hours);
      return false;
    }
    RTOS::delay_ms(EEPROM_WRITE_DELAY_MS);
  }

  if ((meter_control & METER_CONTROL_ABC_DISABLE) != 0) {
    const uint8_t meter_control_frame[] = {
        REG_METER_CONTROL,
        static_cast<uint8_t>(meter_control & ~METER_CONTROL_ABC_DISABLE),
    };
    if (!_write_bytes(meter_control_frame, sizeof(meter_control_frame))) {
      ESP_LOGE(TAG, "Failed to enable ABC");
      return false;
    }
    RTOS::delay_ms(EEPROM_WRITE_DELAY_MS);
  }

  ESP_LOGI(TAG, "ABC period configured to %d days (%u hours)", days, requested_hours);
  return true;
}

bool S12::_read_register(uint8_t reg, uint8_t *buf, size_t len) {
  if (_dev_handle == nullptr || buf == nullptr || len == 0) {
    return false;
  }

  esp_err_t last_err = ESP_FAIL;
  for (int i = 0; i < IO_RETRY_COUNT; i++) {
    last_err = i2c_master_transmit_receive(_dev_handle, &reg, 1, buf, len, I2C_TIMEOUT_MS);
    if (last_err == ESP_OK) {
      return true;
    }

    // Tickle the bus in case the target is waking up.
    (void)i2c_master_probe(_i2c_bus, _address, IO_PROBE_TICKLE_MS);
    RTOS::delay_ms(IO_RETRY_DELAY_MS);
  }

  ESP_LOGW(TAG, "read reg 0x%02X failed after %d retries: %s", reg, IO_RETRY_COUNT,
           esp_err_to_name(last_err));
  return false;
}

bool S12::_write_bytes(const uint8_t *buf, size_t len) {
  if (_dev_handle == nullptr || buf == nullptr || len == 0) {
    return false;
  }

  esp_err_t last_err = ESP_FAIL;
  for (int i = 0; i < IO_RETRY_COUNT; i++) {
    last_err = i2c_master_transmit(_dev_handle, buf, len, I2C_TIMEOUT_MS);
    if (last_err == ESP_OK) {
      return true;
    }

    // Tickle the bus in case the target is waking up.
    (void)i2c_master_probe(_i2c_bus, _address, IO_PROBE_TICKLE_MS);
    RTOS::delay_ms(IO_RETRY_DELAY_MS);
  }

  ESP_LOGW(TAG, "write reg 0x%02X failed after %d retries: %s", buf[0], IO_RETRY_COUNT,
           esp_err_to_name(last_err));
  return false;
}
