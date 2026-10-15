/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/scd4x/scd4x.h"

#include "esp_log.h"

#include "rtos.h"

#include "scd4x_i2c.h"
#include "sensirion_i2c_hal_esp_idf.h"

static constexpr const char *TAG = "SCD4x";

SCD4x::SCD4x(i2c_master_bus_handle_t i2c_bus, uint8_t address)
    : _i2c_bus(i2c_bus), _address(address), _measuring(false),
      _last_temp_hum{MeasuresInvalid::TEMPERATURE, MeasuresInvalid::HUMIDITY} {}

bool SCD4x::init() { return init(false); }

bool SCD4x::init(bool resume_periodic_measurement) {
  // Temporary power research: never resume an already-running measurement.
  resume_periodic_measurement = false;
  _measuring = false;

  // Probe I2C bus to verify device exists (with retry for boot timing).
  // Done with the ESP-IDF master-probe API directly so we fail fast without
  // touching the Sensirion HAL globals if the sensor is absent.
  bool probed = false;
  for (int i = 0; i < INIT_PROBE_RETRIES; i++) {
    esp_err_t ret = i2c_master_probe(_i2c_bus, _address, I2C_PROBE_TIMEOUT_MS);
    if (ret == ESP_OK) {
      probed = true;
      break;
    }
    ESP_LOGW(TAG, "Probe attempt %d/%d failed: %s", i + 1, INIT_PROBE_RETRIES,
             esp_err_to_name(ret));
    RTOS::delay_ms(INIT_PROBE_DELAY_MS);
  }
  if (!probed) {
    ESP_LOGE(TAG, "SCD4x not found at address 0x%02X", _address);
    return false;
  }

  ESP_LOGI(TAG, "SCD4x found at address 0x%02X", _address);

  // Bind the Sensirion HAL to our I2C bus and configure the driver's target
  // address. Note: these are file-scope globals in embedded-i2c-scd4x, so only
  // one SCD4x instance can be active at a time (see header doc).
  sensirion_i2c_hal_set_bus_handle(_i2c_bus);
  scd4x_init(_address);

  if (resume_periodic_measurement) {
    bool data_ready = false;
    const int16_t resume_err = scd4x_get_data_ready_status(&data_ready);
    if (resume_err == 0 && data_ready) {
      _measuring = true;
      ESP_LOGI(TAG, "Reattached to existing periodic measurement");
      return true;
    }

    ESP_LOGW(TAG, "Unable to resume periodic measurement (err=%d ready=%d); reinitializing",
             resume_err, data_ready);
  }

  // Clean-state sequence (mirrors embedded-i2c-scd4x/example-usage):
  //   wake_up -> stop_periodic_measurement -> reinit
  // Wake-up is not acknowledged reliably, so ignore its return value.
  (void)scd4x_wake_up();
  RTOS::delay_ms(WAKE_UP_DELAY_MS);

  int16_t err = scd4x_stop_periodic_measurement();
  if (err != 0) {
    ESP_LOGW(TAG, "stop_periodic_measurement returned %d (ignored)", err);
  }
  // stop_periodic_measurement requires >500 ms before any further command.
  RTOS::delay_ms(STOP_PERIODIC_DELAY_MS);

  // Temporary power research: leave the sensor stopped and exclude it from polling.
  ESP_LOGW(TAG, "Power research: SCD4x disabled (stop result=%d)", err);
  return false;

  err = scd4x_reinit();
  if (err != 0) {
    ESP_LOGW(TAG, "reinit returned %d (ignored)", err);
  }

  // Best-effort diagnostics: log sensor variant and serial number.
  uint16_t variant_raw = 0;
  if (scd4x_get_sensor_variant_raw(&variant_raw) == 0) {
    const uint16_t variant_bits = variant_raw & SCD4X_SENSOR_VARIANT_MASK;
    const char *variant_name = "unknown";
    switch (variant_bits) {
    case SCD4X_SENSOR_VARIANT_SCD40:
      variant_name = "SCD40";
      break;
    case SCD4X_SENSOR_VARIANT_SCD41:
      variant_name = "SCD41";
      break;
    case SCD4X_SENSOR_VARIANT_SCD42:
      variant_name = "SCD42";
      break;
    case SCD4X_SENSOR_VARIANT_SCD43:
      variant_name = "SCD43";
      break;
    default:
      break;
    }
    ESP_LOGI(TAG, "variant: %s (raw=0x%04X)", variant_name, variant_raw);
  } else {
    ESP_LOGW(TAG, "get_sensor_variant_raw failed (ignored)");
  }

  uint16_t serial_words[SERIAL_NUMBER_WORD_COUNT] = {0};
  if (scd4x_get_serial_number(serial_words, SERIAL_NUMBER_WORD_COUNT) == 0) {
    ESP_LOGI(TAG, "serial: 0x%04X%04X%04X", serial_words[0], serial_words[1], serial_words[2]);
  } else {
    ESP_LOGW(TAG, "get_serial_number failed (ignored)");
  }

  // Start continuous (periodic) measurement mode with retry for transient
  // I2C failures.
  if (!_start_periodic_measurement()) {
    return false;
  }

  if (resume_periodic_measurement) {
    RTOS::delay_ms(FIRST_MEASUREMENT_DELAY_MS);
  }

  ESP_LOGI(TAG, "SCD4x initialized, periodic measurement started");
  return true;
}

bool SCD4x::_start_periodic_measurement() {
  for (int i = 0; i < START_MEASUREMENT_RETRIES; i++) {
    int16_t err = scd4x_start_periodic_measurement();
    if (err == 0) {
      _measuring = true;
      return true;
    }
    ESP_LOGW(TAG, "start_periodic_measurement attempt %d/%d failed (err=%d)", i + 1,
             START_MEASUREMENT_RETRIES, err);
    RTOS::delay_ms(START_MEASUREMENT_RETRY_DELAY_MS);
  }

  ESP_LOGE(TAG, "Failed to start periodic measurement after %d attempts",
           START_MEASUREMENT_RETRIES);
  return false;
}

bool SCD4x::read(CO2Data &out) {
  // Initialize to invalid sentinel
  out.co2 = MeasuresInvalid::CO2;

  if (!_measuring) {
    ESP_LOGW(TAG, "Sensor not initialized or not measuring");
    return false;
  }

  for (int i = 0; i < READ_MEASUREMENT_RETRIES; i++) {
    bool data_ready = false;
    int16_t err = scd4x_get_data_ready_status(&data_ready);
    if (err != 0) {
      ESP_LOGW(TAG, "get_data_ready_status attempt %d/%d failed (err=%d)", i + 1,
               READ_MEASUREMENT_RETRIES, err);
    } else if (!data_ready) {
      ESP_LOGW(TAG, "Measurement not ready on attempt %d/%d", i + 1, READ_MEASUREMENT_RETRIES);
    } else {
      uint16_t co2_ppm = 0;
      int32_t temp_m_deg_c = 0;
      int32_t hum_m_percent = 0;
      err = scd4x_read_measurement(&co2_ppm, &temp_m_deg_c, &hum_m_percent);
      if (err == 0) {
        out.co2 = static_cast<int>(co2_ppm);

        _last_temp_hum.temperature = static_cast<float>(temp_m_deg_c) / 1000.0f;

        float hum = static_cast<float>(hum_m_percent) / 1000.0f;
        if (hum < 0.0f) {
          hum = 0.0f;
        }
        if (hum > 100.0f) {
          hum = 100.0f;
        }
        _last_temp_hum.humidity = hum;

        ESP_LOGD(TAG, "CO2: %d ppm, Temp: %.2f C, Hum: %.2f%%", out.co2, _last_temp_hum.temperature,
                 _last_temp_hum.humidity);
        return true;
      }

      ESP_LOGW(TAG, "read_measurement attempt %d/%d failed (err=%d)", i + 1,
               READ_MEASUREMENT_RETRIES, err);
    }

    if (i + 1 < READ_MEASUREMENT_RETRIES) {
      RTOS::delay_ms(READ_MEASUREMENT_RETRY_DELAY_MS);
    }
  }

  ESP_LOGE(TAG, "Failed to read measurement after %d attempts", READ_MEASUREMENT_RETRIES);
  return false;
}

bool SCD4x::supports_temp_hum() const { return true; }

TempHumData SCD4x::temp_hum_data() { return _last_temp_hum; }

bool SCD4x::do_baseline_calibration(int baseline_ppm) {
  if (!_measuring) {
    ESP_LOGW(TAG, "Sensor not initialized or not measuring, cannot start FRC");
    return false;
  }

  // Clamp invalid / sentinel inputs to the Sensirion-recommended default.
  const uint16_t target_ppm =
      (baseline_ppm <= 0) ? CAL_DEFAULT_TARGET_PPM : static_cast<uint16_t>(baseline_ppm);

  ESP_LOGI(TAG, "Starting forced recalibration (target=%u ppm)", target_ppm);

  // FRC is only allowed in idle mode: stop periodic measurement and wait for
  // the sensor to finish its in-flight cycle (Sensirion requires >= 500 ms).
  int16_t err = scd4x_stop_periodic_measurement();
  if (err != 0) {
    ESP_LOGW(TAG, "stop_periodic_measurement before FRC returned %d (ignored)", err);
  }
  _measuring = false;
  RTOS::delay_ms(STOP_PERIODIC_DELAY_MS);

  // Issue FRC. The sensor returns a raw correction word:
  //   - 0xFFFF means the sensor has not been operated long enough to
  //     produce a valid correction (FRC failed).
  //   - otherwise the applied correction in ppm is (raw - 0x8000).
  uint16_t frc_correction = 0;
  const int16_t frc_err = scd4x_perform_forced_recalibration(target_ppm, &frc_correction);
  const bool frc_ok = (frc_err == 0) && (frc_correction != FRC_FAILED_SENTINEL);

  if (frc_err != 0) {
    ESP_LOGE(TAG, "perform_forced_recalibration returned err=%d", frc_err);
  } else if (frc_correction == FRC_FAILED_SENTINEL) {
    ESP_LOGE(TAG,
             "FRC failed: sensor reports it has not been operated long enough "
             "(raw=0x%04X)",
             frc_correction);
  } else {
    const int correction_ppm =
        static_cast<int>(frc_correction) - static_cast<int>(FRC_CORRECTION_OFFSET);
    ESP_LOGI(TAG, "FRC accepted: correction=%d ppm (raw=0x%04X)", correction_ppm, frc_correction);
  }

  // Always attempt to return the sensor to periodic measurement so normal
  // reads resume, regardless of FRC success.
  const bool restarted = _start_periodic_measurement();
  if (!restarted) {
    ESP_LOGE(TAG, "Failed to restart periodic measurement after FRC");
    return false;
  }

  return frc_ok;
}

bool SCD4x::supports_abc_period_configuration() const { return true; }

bool SCD4x::set_abc_period_days(int days) {
  if (!_measuring || !supports_abc_period_configuration()) {
    ESP_LOGW(TAG, "SCD41 ABC period configuration is unavailable");
    return false;
  }
  if (days != ABC_DAYS_DISABLED && (days < ABC_DAYS_MIN || days > ABC_DAYS_MAX)) {
    ESP_LOGW(TAG, "ABC period %d days is outside supported values -1 or %d..%d", days, ABC_DAYS_MIN,
             ABC_DAYS_MAX);
    return false;
  }

  int16_t err = scd4x_stop_periodic_measurement();
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to stop periodic measurement before ABC configuration (err=%d)", err);
    return false;
  }
  _measuring = false;
  RTOS::delay_ms(STOP_PERIODIC_DELAY_MS);

  uint16_t asc_enabled = 0;
  bool configured = false;
  err = scd4x_get_automatic_self_calibration_enabled(&asc_enabled);
  if (err != 0) {
    ESP_LOGE(TAG, "Failed to read SCD41 automatic self-calibration state (err=%d)", err);
  } else if (days == ABC_DAYS_DISABLED) {
    if (asc_enabled != 0) {
      err = scd4x_set_automatic_self_calibration_enabled(0);
      if (err != 0) {
        ESP_LOGE(TAG, "Failed to disable SCD41 automatic self-calibration (err=%d)", err);
      } else {
        err = scd4x_persist_settings();
        if (err != 0) {
          ESP_LOGE(TAG, "Failed to persist disabled SCD41 automatic self-calibration (err=%d)",
                   err);
        }
      }
    }
    configured = err == 0;
  } else {
    const uint16_t requested_hours = static_cast<uint16_t>(days * HOURS_PER_DAY);
    uint16_t current_hours = 0;
    err = scd4x_get_automatic_self_calibration_standard_period(&current_hours);
    if (err != 0) {
      ESP_LOGE(TAG, "Failed to read SCD41 ABC period (err=%d)", err);
    } else {
      if (current_hours != requested_hours) {
        err = scd4x_set_automatic_self_calibration_standard_period(requested_hours);
        if (err != 0) {
          ESP_LOGE(TAG, "Failed to set SCD41 ABC period (err=%d)", err);
        }
      }
      if (err == 0 && asc_enabled == 0) {
        err = scd4x_set_automatic_self_calibration_enabled(1);
        if (err != 0) {
          ESP_LOGE(TAG, "Failed to enable SCD41 automatic self-calibration (err=%d)", err);
        }
      }
      if (err == 0 && (current_hours != requested_hours || asc_enabled == 0)) {
        err = scd4x_persist_settings();
        if (err != 0) {
          ESP_LOGE(TAG, "Failed to persist SCD41 ABC period (err=%d)", err);
        }
      }
      configured = err == 0;
    }
  }

  if (!_start_periodic_measurement()) {
    ESP_LOGE(TAG, "Failed to restart periodic measurement after ABC configuration");
    return false;
  }

  if (configured) {
    if (days == ABC_DAYS_DISABLED) {
      ESP_LOGI(TAG, "SCD41 automatic self-calibration disabled");
    } else {
      ESP_LOGI(TAG, "SCD41 ABC period configured to %d days (%u hours)", days,
               static_cast<uint16_t>(days * HOURS_PER_DAY));
    }
  }
  return configured;
}
