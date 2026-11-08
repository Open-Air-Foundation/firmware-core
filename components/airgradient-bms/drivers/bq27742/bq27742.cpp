/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "bq27742.h"

#include "ag_log.h"
#include "rtos.h"
#include "types/bms_types.h"
#include <limits>

static constexpr const char *TAG = "BQ27742";

namespace {
// TI SLUUAX0C, standard data commands. Words are little-endian.
constexpr uint8_t ADDRESS = 0x55;
constexpr uint16_t DEVICE_TYPE = 0x0742;
constexpr uint8_t REG_CONTROL = 0x00;
constexpr uint8_t REG_VOLTAGE = 0x08;
constexpr uint8_t REG_FLAGS = 0x0A;
constexpr uint8_t REG_REMAINING = 0x10;
constexpr uint8_t REG_FULL = 0x12;
constexpr uint8_t REG_CURRENT = 0x14;
constexpr uint8_t REG_TEMPERATURE = 0x28;
constexpr uint8_t REG_SOC = 0x2C;
constexpr uint16_t CMD_DEVICE_TYPE = 0x0001;
constexpr uint16_t FLAG_DSG = 1u << 0;
constexpr uint16_t FLAG_CHG = 1u << 3;
constexpr uint16_t FLAG_FC = 1u << 9;
constexpr uint32_t CLOCK_HZ = 100000;
constexpr int TIMEOUT_MS = 500;
constexpr uint32_t CLOCK_STRETCH_US = 150000; // TRM: up to 144 ms during internal DF updates
constexpr uint32_t COMMAND_DELAY_MS = 80;     // 66 ms Control() settle plus RTOS tick rounding
constexpr uint16_t MAX_CELL_MV = 6000;
constexpr uint16_t MAX_SOC_PERCENT = 100;
constexpr int32_t MILLIVOLTS_PER_VOLT = 1000;
constexpr float KELVIN_PER_TEMPERATURE_UNIT = 0.1f;
constexpr float KELVIN_TO_CELSIUS = 273.15f;
constexpr float MIN_TEMPERATURE_C = -40.0f;
constexpr float MAX_TEMPERATURE_C = 125.0f;
} // namespace

BQ27742::~BQ27742() {
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
  }
}

bool BQ27742::init() {
  if (_ready)
    return true;
  if (_dev == nullptr) {
    i2c_device_config_t cfg{};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = ADDRESS;
    cfg.scl_speed_hz = CLOCK_HZ;
    cfg.scl_wait_us = CLOCK_STRETCH_US;
    if (i2c_master_bus_add_device(_bus, &cfg, &_dev) != ESP_OK)
      return false;
  }
  constexpr uint8_t device_type_command[] = {REG_CONTROL, static_cast<uint8_t>(CMD_DEVICE_TYPE),
                                             static_cast<uint8_t>(CMD_DEVICE_TYPE >> 8)};
  if (i2c_master_transmit(_dev, device_type_command, sizeof(device_type_command), TIMEOUT_MS) !=
      ESP_OK)
    return false;
  RTOS::delay_ms(COMMAND_DELAY_MS);
  uint16_t id = 0;
  if (!read_word(REG_CONTROL, id) || id != DEVICE_TYPE) {
    AG_LOGE(TAG, "device identification failed (0x%04x)", id);
    return false;
  }
  _ready = true;
  AG_LOGI(TAG, "runtime telemetry ready; pack configuration preserved");
  return true;
}

bool BQ27742::read_word(uint8_t reg, uint16_t &out) {
  uint8_t data[2];
  if (_dev == nullptr ||
      i2c_master_transmit_receive(_dev, &reg, 1, data, sizeof(data), TIMEOUT_MS) != ESP_OK)
    return false;
  out = static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8);
  return true;
}

bool BQ27742::read_soc_percent(uint8_t &out) {
  uint16_t raw;
  if (!_ready || !read_word(REG_SOC, raw) || raw > MAX_SOC_PERCENT)
    return false;
  out = static_cast<uint8_t>(raw);
  return true;
}

bool BQ27742::read_voltage_mv(uint16_t &out) {
  uint16_t raw;
  if (!_ready || !read_word(REG_VOLTAGE, raw) || raw == 0 || raw > MAX_CELL_MV)
    return false;
  out = raw;
  return true;
}

bool BQ27742::read_average_current_ma(int16_t &out) {
  uint16_t raw;
  if (!_ready || !read_word(REG_CURRENT, raw))
    return false;
  const int16_t value = static_cast<int16_t>(raw);
  if (value == BmsInvalid::CURRENT_MA)
    return false;
  out = value;
  return true;
}

bool BQ27742::read_average_power_mw(int16_t &out) {
  // This gauge has no AveragePower command. Derive mW from mV and signed mA.
  uint16_t mv;
  int16_t ma;
  if (!read_voltage_mv(mv) || !read_average_current_ma(ma))
    return false;
  const int32_t mw = static_cast<int32_t>(mv) * ma / MILLIVOLTS_PER_VOLT;
  if (mw <= std::numeric_limits<int16_t>::min() || mw > std::numeric_limits<int16_t>::max())
    return false;
  out = static_cast<int16_t>(mw);
  return true;
}

bool BQ27742::read_remaining_capacity_mah(uint16_t &out) {
  uint16_t raw;
  if (!_ready || !read_word(REG_REMAINING, raw) || raw == BmsInvalid::CAPACITY_MAH)
    return false;
  out = raw;
  return true;
}

bool BQ27742::read_full_charge_capacity_mah(uint16_t &out) {
  uint16_t raw;
  if (!_ready || !read_word(REG_FULL, raw) || raw == 0 || raw == BmsInvalid::CAPACITY_MAH)
    return false;
  out = raw;
  return true;
}

bool BQ27742::read_internal_temperature_c(float &out) {
  uint16_t raw;
  if (!_ready || !read_word(REG_TEMPERATURE, raw))
    return false;
  const float celsius = raw * KELVIN_PER_TEMPERATURE_UNIT - KELVIN_TO_CELSIUS;
  if (celsius < MIN_TEMPERATURE_C || celsius > MAX_TEMPERATURE_C)
    return false;
  out = celsius;
  return true;
}

bool BQ27742::read_flags(uint16_t &out) {
  uint16_t raw;
  if (!_ready || !read_word(REG_FLAGS, raw))
    return false;
  // Normalize to the flags consumed by PowerService. Other raw bits have
  // different meanings; in particular CHG is bit 3 rather than bit 8.
  out = ((raw & FLAG_DSG) ? FgFlags::DSG : 0) | ((raw & FLAG_CHG) ? FgFlags::CHG : 0) |
        ((raw & FLAG_FC) ? FgFlags::FC : 0);
  return true;
}
