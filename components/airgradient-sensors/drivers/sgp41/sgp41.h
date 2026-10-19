/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SGP41_HPP
#define SGP41_HPP

#include <stdint.h>

#include "driver/i2c_master.h"

#include "hal/tvoc_nox_sensor.h"

/**
 * @brief SGP41 TVOC and NOx sensor driver
 *
 * Communicates with sensor using I2C protocol.
 * Supports raw TVOC and NOx measurements with temperature/humidity
 * compensation. Includes optional 10-second conditioning period for initial
 * sensor stabilization.
 */
class SGP41 : public TVOCNOxSensor {
public:
  /**
   * @brief I2C address (fixed for SGP41)
   */
  static constexpr uint8_t ADDRESS = 0x59;

  /**
   * @brief Construct SGP41 sensor with I2C bus
   * @param i2cBus I2C master bus handle
   * @param address I2C address (0x59)
   */
  explicit SGP41(i2c_master_bus_handle_t i2c_bus, uint8_t address = ADDRESS);
  virtual ~SGP41() = default;

  // TVOCNOxSensor interface implementation
  bool init() override;
  bool read(TVOCNOxData &out) override;

  /// Turn both hotplates off and stop measurement, leaving the sensor idle.
  bool turn_heater_off();

  /**
   * @brief Run single conditioning cycle
   * @note Each call takes 50ms
   * @return true if successful, false otherwise
   */
  bool run_conditioning() override;

  /**
   * @brief Set temperature and humidity compensation
   *
   * Stored values persist across read() calls until the next
   * set_compensation() call. Out-of-range values are clamped to the
   * sensor's supported range.
   *
   * @param temperature_c Temperature in °C (-45 to 130)
   * @param humidity_pct  Relative humidity in % (0 to 100)
   */
  void set_compensation(float temperature_c, float humidity_pct) override;

private:
  // I2C handles (objects)
  i2c_master_bus_handle_t _i2c_bus;
  i2c_master_dev_handle_t _dev_handle;

  // Private variables
  uint8_t _address;
  bool _has_compensation;
  float _comp_temperature;
  float _comp_humidity;

  // Command constants (16-bit, big-endian)
  static constexpr uint16_t CMD_MEASURE_RAW = 0x2619;
  static constexpr uint16_t CMD_CONDITIONING = 0x2612;
  static constexpr uint16_t CMD_SELF_TEST = 0x280e;
  static constexpr uint16_t CMD_HEATER_OFF = 0x3615;

  // Timing constants (milliseconds)
  static constexpr uint8_t MEASURE_DELAY_MS = 50;
  static constexpr uint8_t CONDITIONING_DELAY_MS = 50;
  static constexpr uint16_t SELF_TEST_DELAY_MS = 320;
  static constexpr uint8_t HEATER_OFF_DELAY_MS = 1;
  static constexpr uint8_t RETRY_DELAY_MS = 10; // Delay between read retry attempts
  static constexpr uint8_t RETRY_MAX = 2;       // Maximum read retry attempts

  // Default compensation values
  static constexpr float DEFAULT_TEMPERATURE = 25.0f;
  static constexpr float DEFAULT_HUMIDITY = 50.0f;

  // Compensation value ranges
  static constexpr float MIN_TEMPERATURE = -45.0f;
  static constexpr float MAX_TEMPERATURE = 130.0f;
  static constexpr float MIN_HUMIDITY = 0.0f;
  static constexpr float MAX_HUMIDITY = 100.0f;

  // Data buffer sizes
  static constexpr uint8_t CMD_SIZE = 2;             // Command only
  static constexpr uint8_t CMD_WITH_PARAMS_SIZE = 8; // Command + 2 params with CRC
  static constexpr uint8_t RESPONSE_SIZE = 6;        // 2 words (TVOC + NOx), each with CRC

  /**
   * @brief Calculate CRC8 checksum
   * @param data Data buffer
   * @param len Length of data
   * @return CRC8 value
   */
  uint8_t _calculateCrc8(const uint8_t *data, uint8_t len) const;

  /**
   * @brief Convert temperature to SGP41 ticks
   * @param temperature Temperature in °C
   * @return 16-bit tick value
   */
  uint16_t _temperatureToTicks(float temperature) const;

  /**
   * @brief Convert humidity to SGP41 ticks
   * @param humidity Relative humidity in %
   * @return 16-bit tick value
   */
  uint16_t _humidityToTicks(float humidity) const;

  /**
   * @brief Send I2C command without parameters
   * @param command 16-bit command
   * @return true if successful, false otherwise
   */
  bool _sendCommand(uint16_t command);

  /**
   * @brief Send I2C command with two parameters
   * @param command 16-bit command
   * @param param1 First parameter (humidity ticks)
   * @param param2 Second parameter (temperature ticks)
   * @return true if successful, false otherwise
   */
  bool _sendCommandWithParams(uint16_t command, uint16_t param1, uint16_t param2);

  /**
   * @brief Read raw TVOC and NOx signals
   * @param vocRaw Output for TVOC raw value
   * @param noxRaw Output for NOx raw value
   * @return true if successful, false otherwise
   */
  bool _readRawSignals(uint16_t &vocRaw, uint16_t &noxRaw);
};

#endif // SGP41_HPP
