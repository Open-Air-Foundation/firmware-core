/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef DPS368_H
#define DPS368_H

#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/pressure_sensor.h"

/**
 * @brief Infineon DPS368 / Goertek SPL07-003 barometric pressure sensor driver
 *
 * Communicates with sensor using I2C protocol.
 * Uses continuous measurement mode with 16x oversampling.
 * Provides compensated pressure (hPa) and derived altitude (meters).
 *
 * The SPL07-003 (GO v2.0 board) shares the DPS368 register map, scale
 * factors and coefficient layout, with three differences handled here:
 * product ID 0x11, the TMP_EXT bit lives in MEAS_CFG (bit 3) instead of
 * TMP_CFG (bit 7), and two extra coefficients (c31, c40) extend the
 * compensation polynomial. The variant is detected from the ID register.
 *
 * Temperature is read internally for pressure compensation and also
 * exposed through the PressureSensor HAL via supports_temp_hum() and
 * temp_hum_data(). Humidity is not measured by this sensor.
 */
class DPS368 : public PressureSensor {
public:
  // I2C address options
  static constexpr uint8_t ADDRESS_SDO_GND = 0x76;
  static constexpr uint8_t ADDRESS_SDO_VDD = 0x77;

  /**
   * @brief Construct DPS368 sensor with I2C bus
   * @param i2c_bus I2C master bus handle
   * @param address I2C address (0x76 or 0x77)
   */
  explicit DPS368(i2c_master_bus_handle_t i2c_bus, uint8_t address = ADDRESS_SDO_VDD);
  ~DPS368() override = default;

  enum class Variant : uint8_t {
    DPS368,   ///< Infineon, product ID 0x10
    SPL07003, ///< Goertek, product ID 0x11
  };

  // PressureSensor interface implementation
  bool init() override;
  bool read(PressureData &out) override;
  bool supports_temp_hum() const override;
  TempHumData temp_hum_data() override;

  /// Detected part (valid after init()).
  Variant variant() const { return _variant; }

private:
  i2c_master_bus_handle_t _i2c_bus;
  i2c_master_dev_handle_t _dev_handle;
  uint8_t _address;
  Variant _variant;

  // Factory calibration coefficients (read once during init)
  int32_t _c0;
  int32_t _c1;
  int32_t _c00;
  int32_t _c10;
  int32_t _c01;
  int32_t _c11;
  int32_t _c20;
  int32_t _c21;
  int32_t _c30;
  int32_t _c31; ///< SPL07-003 only (0 on DPS368)
  int32_t _c40; ///< SPL07-003 only (0 on DPS368)

  // Last scaled temperature reading (needed for pressure compensation)
  float _last_traw_sc;

  // Cached last temperature reading for temp_hum_data() retrieval
  TempHumData _last_temp_hum;

  // I2C configuration
  static constexpr uint32_t I2C_CLOCK_HZ = 100000;
  static constexpr int I2C_TIMEOUT_MS = 1000;

  // Register addresses
  static constexpr uint8_t REG_PSR_B2 = 0x00;
  static constexpr uint8_t REG_TMP_B2 = 0x03;
  static constexpr uint8_t REG_PRS_CFG = 0x06;
  static constexpr uint8_t REG_TMP_CFG = 0x07;
  static constexpr uint8_t REG_MEAS_CFG = 0x08;
  static constexpr uint8_t REG_CFG_REG = 0x09;
  static constexpr uint8_t REG_RESET = 0x0C;
  static constexpr uint8_t REG_ID = 0x0D;
  static constexpr uint8_t REG_COEF_START = 0x10;

  // Product IDs (register 0x0D)
  static constexpr uint8_t PRODUCT_ID_DPS368 = 0x10;
  static constexpr uint8_t PRODUCT_ID_SPL07_003 = 0x11;

  // Measurement configuration bits
  static constexpr uint8_t MEAS_CFG_COEF_RDY = (1 << 7);
  static constexpr uint8_t MEAS_CFG_TMP_RDY = (1 << 5);
  static constexpr uint8_t MEAS_CFG_PRS_RDY = (1 << 4);
  static constexpr uint8_t MEAS_CFG_TMP_EXT_SPL07 = (1 << 3); // SPL07-003: 1 = MEMS sensor

  // Measurement modes
  static constexpr uint8_t MODE_STANDBY = 0x00;
  static constexpr uint8_t MODE_CONTINUOUS = 0x07;

  // Configuration values
  static constexpr uint8_t PRS_CFG_16X = 0x04;      // 1 meas/sec, 16x oversampling
  static constexpr uint8_t TMP_CFG_EXT_16X = 0x84;  // DPS368: external MEMS, 1 meas/sec, 16x
  static constexpr uint8_t TMP_CFG_16X = 0x04;      // SPL07-003: 1 meas/sec, 16x (no TMP_EXT bit)
  static constexpr uint8_t CFG_SHIFT_ENABLE = 0x0C; // P_SHIFT + T_SHIFT for >8x oversampling
  static constexpr uint8_t SOFT_RESET_CMD = 0x89;

  // Scale factor for 16x oversampling with bit-shift enabled (datasheet Table 9)
  static constexpr float SCALE_FACTOR = 253952.0f;

  // Standard sea-level pressure for altitude calculation (hPa)
  static constexpr float SEA_LEVEL_PRESSURE_HPA = 1013.25f;

  // Timing constants (milliseconds)
  static constexpr uint8_t RESET_DELAY_MS = 50;
  static constexpr uint8_t COEF_POLL_DELAY_MS = 10;
  static constexpr uint8_t COEF_POLL_MAX_ATTEMPTS = 10;

  // Calibration coefficient sizes: DPS368 c0..c30 (0x10-0x21), SPL07-003 adds
  // c31/c40 (0x22-0x24).
  static constexpr uint8_t COEF_BUFFER_SIZE_DPS368 = 18;
  static constexpr uint8_t COEF_BUFFER_SIZE_SPL07 = 21;

  // I2C helpers
  bool _read_register(uint8_t reg, uint8_t *data, size_t len);
  bool _write_register(uint8_t reg, uint8_t value);

  // Initialization helpers
  bool _read_coefficients();

  // Data conversion
  static int32_t _sign_extend_24bit(int32_t value);
  static int32_t _sign_extend_20bit(int32_t value);
  static int32_t _sign_extend_16bit(int32_t value);
  static int32_t _sign_extend_12bit(int32_t value);
  static float _calculate_altitude(float pressure_hpa);
};

#endif // !DPS368_H
