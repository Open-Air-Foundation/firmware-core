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
#include <cmath>
#include <cstring>
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
constexpr uint8_t REG_DESIGN_CAPACITY = 0x3C;
constexpr uint8_t REG_DATA_FLASH_CLASS = 0x3E;
constexpr uint8_t REG_DATA_FLASH_BLOCK = 0x3F;
constexpr uint8_t REG_BLOCK_DATA = 0x40;
constexpr uint8_t REG_BLOCK_CHECKSUM = 0x60;
constexpr uint8_t REG_BLOCK_CONTROL = 0x61;
constexpr uint16_t CMD_CONTROL_STATUS = 0x0000;
constexpr uint16_t CMD_DEVICE_TYPE = 0x0001;
constexpr uint16_t CMD_SEAL = 0x0020;
constexpr uint16_t CONTROL_STATUS_SEALED = 1u << 13;
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

// TI SLUUAX0C sections 5.1, 5.3.5, 5.3.13, and 5.5.1.10.
constexpr size_t DATA_FLASH_BLOCK_SIZE = 32;
constexpr uint8_t SUBCLASS_CHARGE = 34;
constexpr uint8_t SUBCLASS_CHARGE_TERMINATION = 36;
constexpr uint8_t OFFSET_TAPER_CURRENT = 0;
constexpr uint8_t OFFSET_MIN_TAPER_CAPACITY = 2;
constexpr uint8_t OFFSET_TAPER_VOLTAGE = 4;
constexpr uint8_t OFFSET_TAPER_WINDOW = 6;
constexpr uint8_t OFFSET_FC_SET_PERCENT = 9;
constexpr uint16_t MAX_TAPER_CURRENT_MA = 1000;
constexpr uint16_t MAX_MIN_TAPER_CAPACITY_RAW = 1000;
constexpr uint16_t MAX_TAPER_VOLTAGE_MV = 1000;
constexpr uint8_t MAX_TAPER_WINDOW_S = 60;
constexpr int8_t FC_SET_USE_TAPER = -1;
constexpr int8_t MAX_FC_SET_PERCENT = 100;
constexpr uint8_t SUBCLASS_DATA = 48;
constexpr uint8_t SUBCLASS_REGISTERS = 64;
constexpr uint8_t OFFSET_PACK_CONFIG_D = 4;
constexpr uint8_t PACK_CONFIG_D_FCFET = 1u << 6;
constexpr uint8_t SUBCLASS_POWER = 68;
constexpr uint8_t SUBCLASS_IT_CFG = 80;
constexpr uint8_t OFFSET_DESIGN_CAPACITY = 12;
constexpr uint8_t OFFSET_DESIGN_VOLTAGE = 0;
constexpr uint8_t OFFSET_CHARGING_VOLTAGE = 0;
constexpr uint8_t OFFSET_DESIGN_ENERGY = 14;
constexpr uint8_t OFFSET_ENERGY_SCALE = 23;
constexpr uint8_t OFFSET_SLEEP_CURRENT = 2;
constexpr uint8_t OFFSET_TERMINATE_VOLTAGE = 64;
constexpr uint8_t TERMINATE_BLOCK = OFFSET_TERMINATE_VOLTAGE / DATA_FLASH_BLOCK_SIZE;
constexpr uint8_t TERMINATE_BLOCK_OFFSET = OFFSET_TERMINATE_VOLTAGE % DATA_FLASH_BLOCK_SIZE;
constexpr uint8_t ENERGY_SCALE_MWH = 1;
constexpr uint8_t ENERGY_SCALE_CWH = 10;
constexpr uint16_t MAX_DESIGN_CAPACITY_MAH = 14500;
constexpr uint16_t MAX_DESIGN_ENERGY_RAW = 32767;
constexpr uint16_t MIN_TERMINATE_VOLTAGE_MV = 2800;
constexpr uint16_t MAX_TERMINATE_VOLTAGE_MV = 3700;
constexpr uint16_t MAX_SLEEP_CURRENT_MA = 100;
constexpr uint16_t MIN_DESIGN_VOLTAGE_MV = 2000;
constexpr uint16_t MAX_DESIGN_VOLTAGE_MV = 5000;
constexpr uint16_t MIN_CHARGING_VOLTAGE_MV = 4000;
constexpr uint16_t MAX_CHARGING_VOLTAGE_MV = 5000;
constexpr uint32_t DATA_FLASH_DELAY_MS = 20;
constexpr uint32_t DATA_FLASH_COMMIT_DELAY_MS = 200;

// TI SLUUAX0C sections 5.8.1, A.8 and A.11: F4 calibration, not IEEE float.
constexpr uint8_t SUBCLASS_CALIBRATION = 104;
constexpr uint8_t OFFSET_CC_GAIN = 0;
constexpr uint8_t OFFSET_CC_DELTA = 4;
constexpr size_t CC_CALIBRATION_SIZE = 8;
constexpr double CC_GAIN_NUMERATOR = 4.768;
constexpr double CC_DELTA_PER_GAIN = 1193046.0;
constexpr double MIN_CC_GAIN = 0.1;
constexpr double MAX_CC_GAIN = 40.0;
constexpr double MIN_CC_DELTA = 29800.0;
constexpr double MAX_CC_DELTA = 1190000.0;
constexpr int TI_FLOAT_EXPONENT_BIAS = 128;
constexpr int TI_FLOAT_MANTISSA_BITS = 24;
constexpr uint8_t TI_FLOAT_SIGN = 0x80;

bool valid_cc_calibration(double gain, double delta) {
  return gain >= MIN_CC_GAIN && gain <= MAX_CC_GAIN && delta >= MIN_CC_DELTA &&
         delta <= MAX_CC_DELTA;
}

double decode_ti_float(const uint8_t *data) {
  const uint32_t mantissa = (static_cast<uint32_t>(data[1] | TI_FLOAT_SIGN) << 16) |
                            (static_cast<uint32_t>(data[2]) << 8) | data[3];
  const double value = std::ldexp(static_cast<double>(mantissa),
                                  data[0] - TI_FLOAT_EXPONENT_BIAS - TI_FLOAT_MANTISSA_BITS);
  return (data[1] & TI_FLOAT_SIGN) ? -value : value;
}

// Only called for validated, positive calibration values representable as F4.
void encode_ti_float(double value, uint8_t *data) {
  int exponent;
  const float fraction = std::frexp(static_cast<float>(value), &exponent);
  const uint32_t mantissa = static_cast<uint32_t>(std::ldexp(fraction, TI_FLOAT_MANTISSA_BITS));
  data[0] = static_cast<uint8_t>(exponent + TI_FLOAT_EXPONENT_BIAS);
  data[1] = static_cast<uint8_t>(mantissa >> 16) & ~TI_FLOAT_SIGN;
  data[2] = static_cast<uint8_t>(mantissa >> 8);
  data[3] = static_cast<uint8_t>(mantissa);
}

uint16_t unpack_word(const uint8_t *data, size_t offset) {
  return (static_cast<uint16_t>(data[offset]) << 8) | data[offset + 1];
}

void pack_word(uint8_t *data, size_t offset, uint16_t value) {
  data[offset] = static_cast<uint8_t>(value >> 8);
  data[offset + 1] = static_cast<uint8_t>(value);
}

uint8_t block_checksum(const uint8_t *data) {
  uint8_t sum = 0;
  for (size_t i = 0; i < DATA_FLASH_BLOCK_SIZE; ++i) {
    sum += data[i];
  }
  return static_cast<uint8_t>(0xFF - sum);
}

bool valid_cell_config(const FgCellConfig &config) {
  return config.design_capacity_mah <= MAX_DESIGN_CAPACITY_MAH &&
         config.terminate_voltage_mv >= MIN_TERMINATE_VOLTAGE_MV &&
         config.terminate_voltage_mv <= MAX_TERMINATE_VOLTAGE_MV &&
         config.sleep_current_ma <= MAX_SLEEP_CURRENT_MA;
}
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
  // Derive mW from mV and signed average mA.
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

bool BQ27742::read_design_capacity_mah(uint16_t &out) {
  uint16_t raw;
  if (!_ready || !read_word(REG_DESIGN_CAPACITY, raw) || raw > MAX_DESIGN_CAPACITY_MAH) {
    return false;
  }
  out = raw;
  return true;
}

bool BQ27742::write_byte(uint8_t reg, uint8_t value) {
  const uint8_t tx[] = {reg, value};
  if (_dev == nullptr || i2c_master_transmit(_dev, tx, sizeof(tx), TIMEOUT_MS) != ESP_OK) {
    return false;
  }
  RTOS::delay_ms(DATA_FLASH_DELAY_MS);
  return true;
}

bool BQ27742::write_control_word(uint16_t value) {
  const uint8_t tx[] = {REG_CONTROL, static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8)};
  if (!_ready || i2c_master_transmit(_dev, tx, sizeof(tx), TIMEOUT_MS) != ESP_OK) {
    return false;
  }
  RTOS::delay_ms(COMMAND_DELAY_MS);
  return true;
}

bool BQ27742::read_access_status(uint16_t &out) {
  // Raw BQ27742 status is private: the HAL learning flags describe BQ27427.
  return write_control_word(CMD_CONTROL_STATUS) && read_word(REG_CONTROL, out);
}

bool BQ27742::unseal(uint32_t key) {
  uint16_t status;
  if (!read_access_status(status)) {
    return false;
  }
  if (!(status & CONTROL_STATUS_SEALED)) {
    return true;
  }
  // Key 1 (low word) then Key 0 (high word), each sent little-endian.
  if (!write_control_word(static_cast<uint16_t>(key)) ||
      !write_control_word(static_cast<uint16_t>(key >> 16))) {
    return false;
  }
  return read_access_status(status) && !(status & CONTROL_STATUS_SEALED);
}

bool BQ27742::seal() {
  uint16_t status;
  return write_control_word(CMD_SEAL) && read_access_status(status) &&
         (status & CONTROL_STATUS_SEALED);
}

bool BQ27742::data_flash_available() {
  uint16_t status;
  if (!read_access_status(status) || (status & CONTROL_STATUS_SEALED)) {
    AG_LOGW(TAG, "cell configuration requires an unsealed gauge");
    return false;
  }
  return true;
}

bool BQ27742::select_data_flash_block(uint8_t subclass, uint8_t block) {
  return write_byte(REG_BLOCK_CONTROL, 0x00) && write_byte(REG_DATA_FLASH_CLASS, subclass) &&
         write_byte(REG_DATA_FLASH_BLOCK, block);
}

bool BQ27742::read_data_flash_block(uint8_t subclass, uint8_t block, uint8_t *data) {
  if (!select_data_flash_block(subclass, block)) {
    return false;
  }
  // Read the complete block and its checksum before modifying any bytes.
  const uint8_t reg = REG_BLOCK_DATA;
  uint8_t rx[DATA_FLASH_BLOCK_SIZE + 1];
  if (i2c_master_transmit_receive(_dev, &reg, sizeof(reg), rx, sizeof(rx), TIMEOUT_MS) != ESP_OK ||
      block_checksum(rx) != rx[DATA_FLASH_BLOCK_SIZE]) {
    return false;
  }
  std::memcpy(data, rx, DATA_FLASH_BLOCK_SIZE);
  return true;
}

bool BQ27742::write_data_flash_block(uint8_t subclass, uint8_t block, const uint8_t *data) {
  if (!select_data_flash_block(subclass, block)) {
    return false;
  }
  // Byte writes work with the gauge's non-incremental I2C write interface.
  for (size_t i = 0; i < DATA_FLASH_BLOCK_SIZE; ++i) {
    if (!write_byte(static_cast<uint8_t>(REG_BLOCK_DATA + i), data[i])) {
      return false;
    }
  }
  if (!write_byte(REG_BLOCK_CHECKSUM, block_checksum(data))) {
    return false;
  }
  RTOS::delay_ms(DATA_FLASH_COMMIT_DELAY_MS);

  uint8_t verify[DATA_FLASH_BLOCK_SIZE];
  return read_data_flash_block(subclass, block, verify) &&
         std::memcmp(data, verify, DATA_FLASH_BLOCK_SIZE) == 0;
}

bool BQ27742::read_cell_config(FgCellConfig &out) {
  uint8_t data[DATA_FLASH_BLOCK_SIZE];
  uint8_t power[DATA_FLASH_BLOCK_SIZE];
  uint8_t it_cfg[DATA_FLASH_BLOCK_SIZE];
  if (!data_flash_available() || !read_data_flash_block(SUBCLASS_DATA, 0, data) ||
      !read_data_flash_block(SUBCLASS_POWER, 0, power) ||
      !read_data_flash_block(SUBCLASS_IT_CFG, TERMINATE_BLOCK, it_cfg)) {
    return false;
  }
  const uint8_t scale = data[OFFSET_ENERGY_SCALE];
  const uint16_t energy_raw = unpack_word(data, OFFSET_DESIGN_ENERGY);
  const uint32_t energy_mwh = static_cast<uint32_t>(energy_raw) * scale;
  if ((scale != ENERGY_SCALE_MWH && scale != ENERGY_SCALE_CWH) ||
      energy_raw > MAX_DESIGN_ENERGY_RAW || energy_mwh > std::numeric_limits<uint16_t>::max()) {
    return false;
  }
  const FgCellConfig config = {
      .design_capacity_mah = unpack_word(data, OFFSET_DESIGN_CAPACITY),
      .design_energy_mwh = static_cast<uint16_t>(energy_mwh),
      .terminate_voltage_mv = unpack_word(it_cfg, TERMINATE_BLOCK_OFFSET),
      .sleep_current_ma = unpack_word(power, OFFSET_SLEEP_CURRENT),
  };
  if (!valid_cell_config(config)) {
    return false;
  }
  out = config;
  return true;
}

bool BQ27742::write_cell_config(const FgCellConfig &config) {
  if (!valid_cell_config(config) || !data_flash_available()) {
    return false;
  }
  uint8_t data[DATA_FLASH_BLOCK_SIZE];
  uint8_t power[DATA_FLASH_BLOCK_SIZE];
  uint8_t it_cfg[DATA_FLASH_BLOCK_SIZE];
  if (!read_data_flash_block(SUBCLASS_DATA, 0, data) ||
      !read_data_flash_block(SUBCLASS_POWER, 0, power) ||
      !read_data_flash_block(SUBCLASS_IT_CFG, TERMINATE_BLOCK, it_cfg)) {
    return false;
  }
  const uint8_t scale = data[OFFSET_ENERGY_SCALE];
  if ((scale != ENERGY_SCALE_MWH && scale != ENERGY_SCALE_CWH) ||
      config.design_energy_mwh % scale != 0 ||
      config.design_energy_mwh / scale > MAX_DESIGN_ENERGY_RAW) {
    return false;
  }
  const uint16_t energy_raw = config.design_energy_mwh / scale;
  const bool data_changed =
      unpack_word(data, OFFSET_DESIGN_CAPACITY) != config.design_capacity_mah ||
      unpack_word(data, OFFSET_DESIGN_ENERGY) != energy_raw;
  const bool power_changed = unpack_word(power, OFFSET_SLEEP_CURRENT) != config.sleep_current_ma;
  const bool it_changed =
      unpack_word(it_cfg, TERMINATE_BLOCK_OFFSET) != config.terminate_voltage_mv;
  pack_word(data, OFFSET_DESIGN_CAPACITY, config.design_capacity_mah);
  pack_word(data, OFFSET_DESIGN_ENERGY, energy_raw);
  pack_word(power, OFFSET_SLEEP_CURRENT, config.sleep_current_ma);
  pack_word(it_cfg, TERMINATE_BLOCK_OFFSET, config.terminate_voltage_mv);

  // Each block commits independently; a later failure does not roll back earlier blocks.
  if ((data_changed && !write_data_flash_block(SUBCLASS_DATA, 0, data)) ||
      (power_changed && !write_data_flash_block(SUBCLASS_POWER, 0, power)) ||
      (it_changed && !write_data_flash_block(SUBCLASS_IT_CFG, TERMINATE_BLOCK, it_cfg))) {
    AG_LOGE(TAG, "cell configuration write or readback failed");
    return false;
  }
  return true;
}

bool BQ27742::read_voltage_config(VoltageConfig &out) {
  uint8_t data[DATA_FLASH_BLOCK_SIZE];
  uint8_t charge[DATA_FLASH_BLOCK_SIZE];
  if (!data_flash_available() || !read_data_flash_block(SUBCLASS_DATA, 0, data) ||
      !read_data_flash_block(SUBCLASS_CHARGE, 0, charge)) {
    return false;
  }
  out = {unpack_word(data, OFFSET_DESIGN_VOLTAGE), unpack_word(charge, OFFSET_CHARGING_VOLTAGE)};
  return true;
}

bool BQ27742::disable_full_charge_cutoff() {
  uint8_t data[DATA_FLASH_BLOCK_SIZE];
  if (!data_flash_available() || !read_data_flash_block(SUBCLASS_REGISTERS, 0, data)) {
    return false;
  }
  if (!(data[OFFSET_PACK_CONFIG_D] & PACK_CONFIG_D_FCFET)) {
    return true;
  }
  data[OFFSET_PACK_CONFIG_D] &= ~PACK_CONFIG_D_FCFET;
  return write_data_flash_block(SUBCLASS_REGISTERS, 0, data);
}

bool BQ27742::read_cc_sense_mohm(float &gain_mohm, float &delta_mohm) {
  uint8_t data[DATA_FLASH_BLOCK_SIZE];
  if (!data_flash_available() || !read_data_flash_block(SUBCLASS_CALIBRATION, 0, data)) {
    return false;
  }
  const double gain = decode_ti_float(&data[OFFSET_CC_GAIN]);
  const double delta = decode_ti_float(&data[OFFSET_CC_DELTA]);
  if (gain <= 0.0 || delta <= 0.0) {
    return false;
  }
  gain_mohm = static_cast<float>(CC_GAIN_NUMERATOR / gain);
  delta_mohm = static_cast<float>(CC_GAIN_NUMERATOR * CC_DELTA_PER_GAIN / delta);
  return true;
}

bool BQ27742::write_cc_sense_mohm(float sense_mohm) {
  if (!std::isfinite(sense_mohm) || sense_mohm <= 0.0f) {
    return false;
  }
  const double gain = CC_GAIN_NUMERATOR / sense_mohm;
  const double delta = gain * CC_DELTA_PER_GAIN;
  if (!valid_cc_calibration(gain, delta) || !data_flash_available()) {
    return false;
  }
  uint8_t data[DATA_FLASH_BLOCK_SIZE];
  if (!read_data_flash_block(SUBCLASS_CALIBRATION, 0, data)) {
    return false;
  }
  uint8_t expected[CC_CALIBRATION_SIZE];
  encode_ti_float(gain, &expected[OFFSET_CC_GAIN]);
  encode_ti_float(delta, &expected[OFFSET_CC_DELTA]);
  if (std::memcmp(data, expected, sizeof(expected)) == 0) {
    return true;
  }
  std::memcpy(data, expected, sizeof(expected));
  return write_data_flash_block(SUBCLASS_CALIBRATION, 0, data);
}

bool BQ27742::read_taper_config(TaperConfig &out) {
  uint8_t data[DATA_FLASH_BLOCK_SIZE];
  if (!data_flash_available() || !read_data_flash_block(SUBCLASS_CHARGE_TERMINATION, 0, data)) {
    return false;
  }
  out = {unpack_word(data, OFFSET_TAPER_CURRENT), unpack_word(data, OFFSET_MIN_TAPER_CAPACITY),
         unpack_word(data, OFFSET_TAPER_VOLTAGE), data[OFFSET_TAPER_WINDOW],
         static_cast<int8_t>(data[OFFSET_FC_SET_PERCENT])};
  return true;
}

bool BQ27742::write_taper_config(const TaperConfig &config) {
  if (config.current_ma == 0 || config.current_ma > MAX_TAPER_CURRENT_MA ||
      config.min_capacity_raw > MAX_MIN_TAPER_CAPACITY_RAW ||
      config.voltage_mv > MAX_TAPER_VOLTAGE_MV || config.window_s == 0 ||
      config.window_s > MAX_TAPER_WINDOW_S || config.fc_set_percent < FC_SET_USE_TAPER ||
      config.fc_set_percent > MAX_FC_SET_PERCENT || !data_flash_available()) {
    return false;
  }
  uint8_t data[DATA_FLASH_BLOCK_SIZE];
  if (!read_data_flash_block(SUBCLASS_CHARGE_TERMINATION, 0, data)) {
    return false;
  }
  uint8_t expected[DATA_FLASH_BLOCK_SIZE];
  std::memcpy(expected, data, sizeof(expected));
  pack_word(expected, OFFSET_TAPER_CURRENT, config.current_ma);
  pack_word(expected, OFFSET_MIN_TAPER_CAPACITY, config.min_capacity_raw);
  pack_word(expected, OFFSET_TAPER_VOLTAGE, config.voltage_mv);
  expected[OFFSET_TAPER_WINDOW] = config.window_s;
  expected[OFFSET_FC_SET_PERCENT] = static_cast<uint8_t>(config.fc_set_percent);
  if (std::memcmp(data, expected, sizeof(expected)) == 0) {
    return true;
  }
  return write_data_flash_block(SUBCLASS_CHARGE_TERMINATION, 0, expected);
}

bool BQ27742::write_voltage_config(const VoltageConfig &config) {
  if (config.design_voltage_mv < MIN_DESIGN_VOLTAGE_MV ||
      config.design_voltage_mv > MAX_DESIGN_VOLTAGE_MV ||
      config.charging_voltage_mv < MIN_CHARGING_VOLTAGE_MV ||
      config.charging_voltage_mv > MAX_CHARGING_VOLTAGE_MV || !data_flash_available()) {
    return false;
  }
  uint8_t data[DATA_FLASH_BLOCK_SIZE];
  uint8_t charge[DATA_FLASH_BLOCK_SIZE];
  if (!read_data_flash_block(SUBCLASS_DATA, 0, data) ||
      !read_data_flash_block(SUBCLASS_CHARGE, 0, charge)) {
    return false;
  }
  const bool data_changed = unpack_word(data, OFFSET_DESIGN_VOLTAGE) != config.design_voltage_mv;
  const bool charge_changed =
      unpack_word(charge, OFFSET_CHARGING_VOLTAGE) != config.charging_voltage_mv;
  pack_word(data, OFFSET_DESIGN_VOLTAGE, config.design_voltage_mv);
  pack_word(charge, OFFSET_CHARGING_VOLTAGE, config.charging_voltage_mv);
  return (!data_changed || write_data_flash_block(SUBCLASS_DATA, 0, data)) &&
         (!charge_changed || write_data_flash_block(SUBCLASS_CHARGE, 0, charge));
}
