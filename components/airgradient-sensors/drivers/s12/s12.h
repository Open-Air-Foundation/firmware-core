/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef S12_HPP
#define S12_HPP

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/co2_sensor.h"

/**
 * @brief SenseAir S12 CO2 sensor driver (I2C variant)
 *
 * Communicates with the sensor using the ESP-IDF I2C master driver.
 * Exposes init(), read(), and manual baseline calibration via the
 * CO2Sensor virtual API (do_baseline_calibration /
 * is_baseline_calibration_done).
 *
 * The CO2 value is read from a single 16-bit big-endian register pair.
 * By default the "Measured Filtered Pressure Compensated" register
 * (0x06/0x07) is used; a different source register can be provided via
 * the constructor if a product needs the raw or unfiltered variant.
 *
 * Calibration is driven non-blocking: do_baseline_calibration() issues the S12
 * target command for baseline calibration and returns immediately; the caller
 * (e.g. SensorManager::calibrate_co2()) polls is_baseline_calibration_done() on
 * its own cadence.
 */
class S12 : public CO2Sensor {
public:
  /** @brief Default 7-bit I2C address. */
  static constexpr uint8_t ADDRESS_DEFAULT = 0x68;

  /**
   * @brief CO2 source register (high byte).
   *
   * 0x06/0x07 = Measured concentration, filtered, pressure compensated.
   * Other S12 register options (raw / unfiltered) follow the same
   * big-endian 16-bit layout and can be selected by passing a different
   * high-byte register to the constructor.
   */
  static constexpr uint8_t REG_CO2_FILT_PRES_COMP_HI = 0x06;

  /**
   * @brief Construct an S12 driver bound to an I2C master bus.
   * @param i2c_bus    Initialized I2C master bus handle.
   * @param address    7-bit device address (default 0x68).
   * @param co2_reg_hi High byte of the 16-bit CO2 register to read
   *                   (default 0x06 = filtered + pressure compensated).
   */
  explicit S12(i2c_master_bus_handle_t i2c_bus, uint8_t address = ADDRESS_DEFAULT,
               uint8_t co2_reg_hi = REG_CO2_FILT_PRES_COMP_HI);
  virtual ~S12() = default;

  // CO2Sensor interface implementation
  bool init() override;
  bool read(CO2Data &out) override;
  TempHumData temp_hum_data() override;

  /**
   * @brief Report that this driver supports manual baseline calibration.
   */
  bool supports_calibration() const override { return true; }

  /**
   * @brief Start a manual baseline calibration (non-blocking).
   *
   * Clears the calibration status register, writes the calibration target
   * register (0x84/0x85) with the requested reference concentration, then
   * issues the S12 target calibration command (0x7C05) to 0x82/0x83.
   * The function returns as soon as the command has been accepted; callers
   * poll is_baseline_calibration_done() to observe completion.
   *
   * @param baseline_ppm Reference CO2 concentration in ppm
   *                     (clamped to 400 ppm when <= 0).
   * @return true if the command was accepted by the sensor.
   */
  bool do_baseline_calibration(int baseline_ppm = 400) override;

  /**
   * @brief Poll whether a previously started calibration has finished.
   *
   * - Returns true when no calibration is in progress (idle contract).
   * - Returns true once the sensor reports target-done in the calibration
   *   status register (0x81 bit 0x10).
   * - Returns false while the S12 has not yet completed a new measurement
   *   cycle, on transient read errors, or when the sensor flags a
   *   calibration error in the error status register (0x01 bit 0x08).
   *   In the error case the internal "calibrating" flag is cleared so the
   *   caller's polling loop eventually times out.
   */
  bool is_baseline_calibration_done() override;

  bool supports_abc_period_configuration() const override { return true; }
  bool set_abc_period_days(int days) override;

private:
  i2c_master_bus_handle_t _i2c_bus;
  i2c_master_dev_handle_t _dev_handle;
  uint8_t _address;
  uint8_t _co2_reg_hi;
  bool _initialized;
  bool _is_calibrating;

  // I2C configuration
  static constexpr uint32_t I2C_CLOCK_HZ = 100000;
  static constexpr int I2C_TIMEOUT_MS = 1000;

  // IO retry parameters (mirrors the reference S12 I2C driver)
  static constexpr int IO_RETRY_COUNT = 5;
  static constexpr int IO_RETRY_DELAY_MS = 10;
  static constexpr int IO_PROBE_TICKLE_MS = 20;

  // Initialization retry parameters
  static constexpr int INIT_PROBE_RETRIES = 3;
  static constexpr int INIT_PROBE_DELAY_MS = 100;

  // CO2 register read width (two data bytes, big-endian ppm).
  static constexpr size_t CO2_REG_READ_LEN = 2;

  // --- Calibration register map ---------------------------------------------
  // Error status low byte (bit 0x08 == calibration error).
  static constexpr uint8_t REG_ERROR_STATUS_LSB = 0x01;
  // Calibration status register (bit 0x10 == target calibration done).
  static constexpr uint8_t REG_CALIBRATION_STATUS = 0x81;
  // Calibration command register (16-bit, MSB at 0x82, LSB at 0x83).
  static constexpr uint8_t REG_CALIBRATION_COMMAND_MSB = 0x82;
  // Calibration target concentration (16-bit, MSB at 0x84, LSB at 0x85, ppm).
  static constexpr uint8_t REG_CALIBRATION_TARGET_MSB = 0x84;
  // ABC Period (HR14) is an EEPROM-backed 16-bit hour count.
  static constexpr uint8_t REG_ABC_PERIOD_MSB = 0x9A;
  // System Control Register; writing 0xFF resets the sensor.
  static constexpr uint8_t REG_SCR = 0xA3;
  // MeterControl (HR19); a set bit disables the corresponding feature.
  static constexpr uint8_t REG_METER_CONTROL = 0xA5;

  // --- Calibration command / status bits ------------------------------------
  // 0x7C is the S12 calibration command prefix; 0x05 selects "target".
  static constexpr uint8_t CAL_PREFIX = 0x7C;
  static constexpr uint8_t CAL_TARGET = 0x05;
  // Bit set in REG_CALIBRATION_STATUS once target calibration finishes.
  static constexpr uint8_t CAL_STATUS_TARGET_DONE = 0x10;
  // Bit set in REG_ERROR_STATUS_LSB when the sensor flags a calibration error.
  static constexpr uint8_t ERR_STATUS_CALIBRATION = 0x08;
  // Default baseline concentration used when the caller passes <= 0 ppm.
  static constexpr uint16_t CAL_DEFAULT_BASELINE_PPM = 400;
  static constexpr uint8_t METER_CONTROL_ABC_DISABLE = 1U << 1;
  static constexpr uint8_t SCR_RESET = 0xFF;
  static constexpr int ABC_DAYS_DISABLED = -1;
  static constexpr int ABC_DAYS_MIN = 1;
  static constexpr int ABC_DAYS_MAX = 200;
  static constexpr uint16_t HOURS_PER_DAY = 24;
  // Datasheet maximum is 180 ms. Use 300 ms to include a conservative margin.
  static constexpr uint32_t EEPROM_WRITE_SETTLE_MS = 300;
  // Communication resumes 30 ms after SCR. Use twice that time as margin.
  static constexpr uint32_t SENSOR_RESET_SETTLE_MS = 60;
  static constexpr size_t EEPROM_MAX_PAYLOAD_LEN = 2;

  /**
   * @brief Read `len` bytes from an S12 register with retry + bus tickle.
   * @param reg Register address (high byte for multi-byte reads).
   * @param buf Output buffer, must hold `len` bytes.
   * @param len Number of bytes to read.
   * @return true on success, false after exhausting retries.
   */
  bool _read_register(uint8_t reg, uint8_t *buf, size_t len);

  /**
   * @brief Write a pre-formed register+payload byte sequence with retry.
   *
   * The caller is responsible for placing the destination register address
   * as the first byte of `buf`, followed by the payload bytes in S12 order
   * (big-endian for multi-byte registers). Uses the same retry + probe-tickle
   * pattern as _read_register().
   *
   * @param buf Buffer containing the register address followed by payload.
   * @param len Total number of bytes in `buf` (>= 1).
   * @return true on success, false after exhausting retries.
   */
  bool _write_bytes(const uint8_t *buf, size_t len);

  /**
   * @brief Attempt one EEPROM write and wait for it to settle.
   */
  bool _write_eeprom(const uint8_t *buf, size_t len);

  /**
   * @brief Reset the sensor through SCR and wait for communication to resume.
   */
  bool _reset_sensor();
};

#endif // S12_HPP
