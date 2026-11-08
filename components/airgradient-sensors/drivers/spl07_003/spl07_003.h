/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SPL07_003_H
#define SPL07_003_H

#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/pressure_sensor.h"

/**
 * @brief Goertek SPL07-003 barometric pressure sensor driver.
 *
 * Uses I2C continuous pressure and internal temperature measurements at 1 Hz
 * with 16x oversampling. Reports pressure (hPa), derived altitude (meters),
 * and temperature (degrees Celsius); humidity remains invalid.
 * The caller owns the bus and must keep it alive until this driver is destroyed.
 */
class SPL07_003 : public PressureSensor {
public:
  static constexpr uint8_t ADDRESS_SDO_GND = 0x76;
  static constexpr uint8_t ADDRESS_SDO_VDD = 0x77;

  explicit SPL07_003(i2c_master_bus_handle_t i2c_bus, uint8_t address = ADDRESS_SDO_VDD);
  ~SPL07_003() override;

  SPL07_003(const SPL07_003 &) = delete;
  SPL07_003 &operator=(const SPL07_003 &) = delete;

  bool init() override;

  // Requires both measurements to be ready. Returns true for valid pressure;
  // altitude is validated independently. Every attempt invalidates cached temperature.
  bool read(PressureData &out) override;
  bool supports_temp_hum() const override;
  TempHumData temp_hum_data() override;

private:
  i2c_master_bus_handle_t _i2c_bus;
  i2c_master_dev_handle_t _dev_handle;
  uint8_t _address;
  bool _initialized;

  // Factory calibration coefficients, decoded once during initialization.
  int32_t _c0;
  int32_t _c1;
  int32_t _c00;
  int32_t _c10;
  int32_t _c01;
  int32_t _c11;
  int32_t _c20;
  int32_t _c21;
  int32_t _c30;
  int32_t _c31;
  int32_t _c40;
  TempHumData _last_temp_hum;

  static constexpr uint32_t I2C_CLOCK_HZ = 100000;
  static constexpr int I2C_TIMEOUT_MS = 1000;

  static constexpr uint8_t REG_PSR_B2 = 0x00;
  static constexpr uint8_t REG_PRS_CFG = 0x06;
  static constexpr uint8_t REG_TMP_CFG = 0x07;
  static constexpr uint8_t REG_MEAS_CFG = 0x08;
  static constexpr uint8_t REG_CFG_REG = 0x09;
  static constexpr uint8_t REG_RESET = 0x0C;
  static constexpr uint8_t REG_ID = 0x0D;
  static constexpr uint8_t REG_COEF_START = 0x10;

  // Full product/revision ID documented by the SPL07-003 v1.0 datasheet.
  static constexpr uint8_t PRODUCT_REVISION_ID = 0x11;
  static constexpr uint8_t MEAS_CFG_COEF_RDY = (1 << 7);
  static constexpr uint8_t MEAS_CFG_SENSOR_RDY = (1 << 6);
  static constexpr uint8_t MEAS_CFG_TMP_RDY = (1 << 5);
  static constexpr uint8_t MEAS_CFG_PRS_RDY = (1 << 4);
  static constexpr uint8_t INIT_READY_MASK = MEAS_CFG_COEF_RDY | MEAS_CFG_SENSOR_RDY;
  static constexpr uint8_t DATA_READY_MASK = MEAS_CFG_TMP_RDY | MEAS_CFG_PRS_RDY;

  static constexpr uint8_t PRS_CFG_1HZ_16X = 0x04;
  static constexpr uint8_t TMP_CFG_1HZ_16X = 0x04;
  // TMP_EXT is bit 3 of MEAS_CFG, unlike DPS368. Leave it clear for internal temperature.
  static constexpr uint8_t MODE_CONTINUOUS_INTERNAL = 0x07;
  static constexpr uint8_t CFG_SHIFT_ENABLE = 0x0C;
  static constexpr uint8_t SOFT_RESET_CMD = 0x09;

  static constexpr uint32_t RESET_DELAY_MS = 50;
  static constexpr uint32_t READY_POLL_DELAY_MS = 10;
  static constexpr int READY_POLL_MAX_ATTEMPTS = 10;
  static constexpr size_t COEF_BUFFER_SIZE = 21;
  static constexpr size_t RAW_BUFFER_SIZE = 6;

  // Datasheet Table 4: 16x oversampling, with pressure and temperature shift enabled.
  static constexpr float SCALE_FACTOR = 253952.0f;
  static constexpr float TEMPERATURE_OFFSET_SCALE = 0.5f;
  static constexpr float MAX_SENSOR_TEMPERATURE_C = 85.0f;
  static constexpr float PA_PER_HPA = 100.0f;
  static constexpr float SEA_LEVEL_PRESSURE_HPA = 1013.25f;
  static constexpr float ALTITUDE_SCALE_M = 44330.0f;
  static constexpr float ALTITUDE_EXPONENT = 0.1903f;

  bool _read_register(uint8_t reg, uint8_t *data, size_t len);
  bool _write_register(uint8_t reg, uint8_t value);
  bool _configure();
  bool _read_coefficients();
  bool _release_device();

  static int32_t _sign_extend(int32_t value, uint8_t bits);
  static float _calculate_altitude(float pressure_hpa);
};

#endif // SPL07_003_H
