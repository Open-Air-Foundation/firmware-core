/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/bq27427/bq27427.h"

#include "esp_log.h"
#include "rtos.h"

static constexpr const char *TAG = "BQ27427";

// ---------------------------------------------------------------------------
// BQ27427 register / subcommand constants (TRM SLUUCD5)
// ---------------------------------------------------------------------------

namespace {

// Standard Command addresses (TRM section 5).
constexpr uint8_t CMD_CONTROL = 0x00;
constexpr uint8_t CMD_VOLTAGE = 0x04;
constexpr uint8_t CMD_FLAGS = 0x06;
constexpr uint8_t CMD_AVG_CURRENT = 0x10;
constexpr uint8_t CMD_AVG_POWER = 0x18;
constexpr uint8_t CMD_SOC = 0x1C;
constexpr uint8_t CMD_INT_TEMP = 0x1E;
constexpr uint8_t CMD_REMAIN_CAP = 0x2A;
constexpr uint8_t CMD_FULL_CHARGE_CAP = 0x2E;

// Control() subcommands (TRM section 4).
constexpr uint16_t CTRL_CONTROL_STATUS = 0x0000;
constexpr uint16_t CTRL_DEVICE_TYPE = 0x0001;
constexpr uint16_t CTRL_CHEM_ID = 0x0008;
constexpr uint16_t CTRL_SET_CFGUPDATE = 0x0013;
constexpr uint16_t CTRL_CHEM_B = 0x0031; // selects Chem ID 0x1202 (4.2 V)
constexpr uint16_t CTRL_RESET = 0x0041;
constexpr uint16_t CTRL_SOFT_RESET = 0x0042;
constexpr uint16_t CTRL_UNSEAL_KEY = 0x8000;

// TI chem-ID labels are HEX: "1202" == 0x1202 (the default "3230" == 0x3230).
constexpr uint16_t CHEM_ID_4V2 = 0x1202;

// Ra0 RAM subclass and State-subclass learning offsets.
constexpr uint8_t SUBCLASS_RA0_RAM = 0x59;
constexpr uint8_t OFFSET_QMAX_CELL0 = 0;           // State subclass (0x52) word 0
constexpr uint8_t OFFSET_UPDATE_STATUS = 2;        // State subclass (0x52) byte 2
constexpr uint8_t UPDATE_STATUS_LEARN_BITS = 0x03; // bit0 (Qmax) + bit1 (Ra)

// Extended command interface (TRM section 6).
constexpr uint8_t CMD_DATA_BLOCK_CLASS = 0x3E;
constexpr uint8_t CMD_DATA_BLOCK = 0x3F;
constexpr uint8_t CMD_BLOCK_DATA_BASE = 0x40;
constexpr uint8_t CMD_BLOCK_DATA_CHECKSUM = 0x60;
constexpr uint8_t CMD_BLOCK_DATA_CONTROL = 0x61;

// Subclass / offsets in Data Memory (TRM section 7.4).
constexpr uint8_t SUBCLASS_STATE = 0x52;
constexpr uint8_t OFFSET_DESIGN_CAPACITY = 6;
constexpr uint8_t OFFSET_DESIGN_ENERGY = 8;
constexpr uint8_t OFFSET_TERMINATE_VOLTAGE = 10;
constexpr uint8_t OFFSET_SLEEP_CURRENT = 23;

// Flags() bit 4 = CFGUPDATE mode active.
constexpr uint16_t FLAG_CFGUPDATE = (1u << 4);

// Settle delay after Control() subcommand write before result read.
constexpr uint32_t CONTROL_SETTLE_MS = 2;

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

BQ27427::BQ27427(i2c_master_bus_handle_t bus) : _bus(bus) {}

BQ27427::BQ27427(i2c_master_bus_handle_t bus, const Config &config) : _bus(bus), _config(config) {}

BQ27427::~BQ27427() {
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool BQ27427::init() {
  if (_dev != nullptr) {
    return true; // already attached
  }

  esp_err_t err = i2c_master_probe(_bus, _config.address, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "probe at 0x%02X failed: %s", _config.address, esp_err_to_name(err));
    return false;
  }

  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = _config.address,
      .scl_speed_hz = _config.scl_speed_hz,
      .scl_wait_us = 20000,
      .flags = {},
  };
  err = i2c_master_bus_add_device(_bus, &dev_cfg, &_dev);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "add_device failed: %s", esp_err_to_name(err));
    _dev = nullptr;
    return false;
  }

  uint16_t device_type = 0;
  if (!control_subcommand(CTRL_DEVICE_TYPE, device_type)) {
    ESP_LOGE(TAG, "DEVICE_TYPE read failed");
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
    return false;
  }
  if (device_type != DEVICE_TYPE_BQ27427) {
    ESP_LOGE(TAG, "DEVICE_TYPE=0x%04X, expected 0x%04X", device_type, DEVICE_TYPE_BQ27427);
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
    return false;
  }

  ESP_LOGI(TAG, "BQ27427 found at 0x%02X (DEVICE_TYPE=0x%04X)", _config.address, device_type);
  return true;
}

// ---------------------------------------------------------------------------
// FuelGaugeDevice -- ready
// ---------------------------------------------------------------------------

bool BQ27427::ready() const { return _dev != nullptr; }

// ---------------------------------------------------------------------------
// Standard Command reads
// ---------------------------------------------------------------------------

bool BQ27427::read_soc_percent(uint8_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_SOC, raw)) {
    return false;
  }
  if (raw > 100) {
    return false;
  }
  out = static_cast<uint8_t>(raw);
  return true;
}

bool BQ27427::read_voltage_mv(uint16_t &out) { return _read_word(CMD_VOLTAGE, out); }

bool BQ27427::read_average_current_ma(int16_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_AVG_CURRENT, raw)) {
    return false;
  }
  out = static_cast<int16_t>(raw);
  return true;
}

bool BQ27427::read_average_power_mw(int16_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_AVG_POWER, raw)) {
    return false;
  }
  out = static_cast<int16_t>(raw);
  return true;
}

bool BQ27427::read_remaining_capacity_mah(uint16_t &out) { return _read_word(CMD_REMAIN_CAP, out); }

bool BQ27427::read_full_charge_capacity_mah(uint16_t &out) {
  return _read_word(CMD_FULL_CHARGE_CAP, out);
}

bool BQ27427::read_internal_temperature_c(float &out) {
  uint16_t dk = 0;
  if (!_read_word(CMD_INT_TEMP, dk)) {
    return false;
  }
  out = (static_cast<float>(dk) * 0.1f) - 273.15f;
  return true;
}

bool BQ27427::read_flags(uint16_t &out) { return _read_word(CMD_FLAGS, out); }

// ---------------------------------------------------------------------------
// Control() subcommand
// ---------------------------------------------------------------------------

bool BQ27427::control_subcommand(uint16_t subcmd, uint16_t &result) {
  if (!_write_word(CMD_CONTROL, subcmd)) {
    return false;
  }
  RTOS::delay_ms(CONTROL_SETTLE_MS);
  return _read_word(CMD_CONTROL, result);
}

// ---------------------------------------------------------------------------
// Data Memory reads
// ---------------------------------------------------------------------------

bool BQ27427::read_design_capacity_mah(uint16_t &out) {
  // Reads must START at 0x40 to trigger the chip's block-buffer fill.
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  RTOS::delay_ms(10);
  uint8_t buf[8] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, buf, sizeof(buf))) {
    return false;
  }
  out = (static_cast<uint16_t>(buf[OFFSET_DESIGN_CAPACITY]) << 8) | buf[OFFSET_DESIGN_CAPACITY + 1];
  return true;
}

bool BQ27427::read_cell_config(FgCellConfig &out) {
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  RTOS::delay_ms(10);

  // Read enough bytes to cover all fields (Sleep Current at offset 23/24).
  uint8_t buf[25] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, buf, sizeof(buf))) {
    return false;
  }

  auto unpack = [&buf](uint8_t offset) -> uint16_t {
    return (static_cast<uint16_t>(buf[offset]) << 8) | buf[offset + 1];
  };

  out.design_capacity_mah = unpack(OFFSET_DESIGN_CAPACITY);
  out.design_energy_mwh = unpack(OFFSET_DESIGN_ENERGY);
  out.terminate_voltage_mv = unpack(OFFSET_TERMINATE_VOLTAGE);
  out.sleep_current_ma = unpack(OFFSET_SLEEP_CURRENT);
  return true;
}

// ---------------------------------------------------------------------------
// FG-learning reads
// ---------------------------------------------------------------------------

bool BQ27427::read_control_status(uint16_t &out) {
  return control_subcommand(CTRL_CONTROL_STATUS, out);
}

bool BQ27427::read_chem_id(uint16_t &out) { return control_subcommand(CTRL_CHEM_ID, out); }

bool BQ27427::read_qmax_cell0(uint16_t &out) {
  // Block reads must START at 0x40 to trigger the chip's block-buffer fill.
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  RTOS::delay_ms(10);
  uint8_t buf[2] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE + OFFSET_QMAX_CELL0, buf, sizeof(buf))) {
    return false;
  }
  out = (static_cast<uint16_t>(buf[0]) << 8) | buf[1]; // MSB-first DM convention
  return true;
}

bool BQ27427::read_ra_table(int16_t *out, size_t len) {
  if (out == nullptr || len == 0) {
    return false;
  }
  if (!_select_data_block(SUBCLASS_RA0_RAM, 0x00)) {
    return false;
  }
  RTOS::delay_ms(10);
  uint8_t buf[RA_TABLE_SIZE * 2] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, buf, sizeof(buf))) {
    return false;
  }
  const size_t n = (len < static_cast<size_t>(RA_TABLE_SIZE)) ? len : RA_TABLE_SIZE;
  for (size_t i = 0; i < n; ++i) {
    out[i] = static_cast<int16_t>((static_cast<uint16_t>(buf[2 * i]) << 8) | buf[2 * i + 1]);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Data Memory writes
// ---------------------------------------------------------------------------

bool BQ27427::write_cell_config(const FgCellConfig &cfg) {
  if (_dev == nullptr) {
    return false;
  }

  // UNSEAL — required because BlockDataChecksum (0x60) writes are
  // UNSEALED-only (TRM section 6.4).
  if (!_unseal()) {
    return false;
  }

  // Enter CFGUPDATE mode.
  if (!_write_word(CMD_CONTROL, CTRL_SET_CFGUPDATE)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(true, 2000)) {
    ESP_LOGW(TAG, "Could not enter CFGUPDATE — aborting cell config write");
    return false;
  }

  // Re-select the block (CFGUPDATE entry can clear the block pointer).
  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  RTOS::delay_ms(10);

  // Read the entire 32-byte block.
  uint8_t block[32] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }

  auto pack = [&block](uint8_t offset, uint16_t value) {
    block[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
    block[offset + 1] = static_cast<uint8_t>(value & 0xFF);
  };

  // Modify all four fields locally (MSB-first per Data Memory convention).
  pack(OFFSET_DESIGN_CAPACITY, cfg.design_capacity_mah);
  pack(OFFSET_DESIGN_ENERGY, cfg.design_energy_mwh);
  pack(OFFSET_TERMINATE_VOLTAGE, cfg.terminate_voltage_mv);
  pack(OFFSET_SLEEP_CURRENT, cfg.sleep_current_ma);

  // Write the full 32-byte block back.
  if (!_write_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }
  RTOS::delay_ms(10);

  // Fresh checksum from the modified block.
  uint16_t sum = 0;
  for (size_t i = 0; i < sizeof(block); ++i) {
    sum += block[i];
  }
  const uint8_t new_csum = static_cast<uint8_t>(255 - (sum & 0xFF));

  // Commit: writing the checksum transfers BlockData() to RAM.
  if (!_write_byte(CMD_BLOCK_DATA_CHECKSUM, new_csum)) {
    return false;
  }
  RTOS::delay_ms(20);

  // Exit CFGUPDATE.
  if (!_write_word(CMD_CONTROL, CTRL_SOFT_RESET)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(false, 2000)) {
    return false;
  }

  // Readback verification.
  RTOS::delay_ms(50);
  FgCellConfig verify{};
  if (!read_cell_config(verify)) {
    ESP_LOGE(TAG, "cell config readback failed after write");
    return false;
  }
  if (verify != cfg) {
    ESP_LOGE(TAG, "cell config write did NOT stick — readback mismatch");
    return false;
  }
  ESP_LOGI(TAG, "cell config verified (DC=%u DE=%u TermV=%u SleepI=%u)", cfg.design_capacity_mah,
           cfg.design_energy_mwh, cfg.terminate_voltage_mv, cfg.sleep_current_ma);
  return true;
}

bool BQ27427::set_update_status_learning(bool enable) {
  if (_dev == nullptr) {
    return false;
  }
  if (!_unseal()) {
    return false;
  }
  if (!_write_word(CMD_CONTROL, CTRL_SET_CFGUPDATE)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(true, 2000)) {
    ESP_LOGW(TAG, "Could not enter CFGUPDATE — aborting Update Status write");
    return false;
  }

  if (!_select_data_block(SUBCLASS_STATE, 0x00)) {
    return false;
  }
  RTOS::delay_ms(10);

  uint8_t block[32] = {};
  if (!_read_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }

  if (enable) {
    block[OFFSET_UPDATE_STATUS] |= UPDATE_STATUS_LEARN_BITS;
  } else {
    block[OFFSET_UPDATE_STATUS] &= static_cast<uint8_t>(~UPDATE_STATUS_LEARN_BITS);
  }

  if (!_write_block(CMD_BLOCK_DATA_BASE, block, sizeof(block))) {
    return false;
  }
  RTOS::delay_ms(10);

  uint16_t sum = 0;
  for (size_t i = 0; i < sizeof(block); ++i) {
    sum += block[i];
  }
  const uint8_t new_csum = static_cast<uint8_t>(255 - (sum & 0xFF));
  if (!_write_byte(CMD_BLOCK_DATA_CHECKSUM, new_csum)) {
    return false;
  }
  RTOS::delay_ms(20);

  if (!_write_word(CMD_CONTROL, CTRL_SOFT_RESET)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(false, 2000)) {
    return false;
  }
  ESP_LOGI(TAG, "Update Status learning bits %s", enable ? "set" : "cleared");
  return true;
}

bool BQ27427::select_chemistry_4v2() {
  uint16_t chem = 0;
  if (!read_chem_id(chem)) {
    ESP_LOGE(TAG, "CHEM_ID read failed — cannot verify chemistry");
    return false;
  }
  if (chem == CHEM_ID_4V2) {
    return true; // already on the 4.2 V profile — idempotent
  }

  ESP_LOGW(TAG, "Switching chemistry 0x%04X -> 0x%04X (CHEM_B); IT learning resets", chem,
           CHEM_ID_4V2);

  // Changing Chem ID resets IT learning — must precede any learning run.
  if (!_unseal()) {
    return false;
  }
  if (!_write_word(CMD_CONTROL, CTRL_SET_CFGUPDATE)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(true, 2000)) {
    ESP_LOGW(TAG, "Could not enter CFGUPDATE — aborting chemistry switch");
    return false;
  }
  if (!_write_word(CMD_CONTROL, CTRL_CHEM_B)) {
    return false;
  }
  RTOS::delay_ms(100);
  if (!_write_word(CMD_CONTROL, CTRL_SOFT_RESET)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(false, 3000)) {
    ESP_LOGW(TAG, "CFGUPDATE did not clear after chemistry switch");
    return false;
  }

  uint16_t after = 0;
  if (!read_chem_id(after) || after != CHEM_ID_4V2) {
    ESP_LOGE(TAG, "chemistry switch did NOT stick (now 0x%04X)", after);
    return false;
  }
  ESP_LOGI(TAG, "chemistry now 0x%04X (4.2 V profile)", after);
  return true;
}

bool BQ27427::reset_to_factory_defaults() {
  ESP_LOGW(TAG, "Resetting fuel gauge to factory defaults (Control RESET=0x0041)");

  if (!_unseal()) {
    return false;
  }
  if (!_write_word(CMD_CONTROL, CTRL_SET_CFGUPDATE)) {
    return false;
  }
  if (!_wait_cfgupdate_flag(true, 2000)) {
    ESP_LOGW(TAG, "Could not enter CFGUPDATE — aborting RESET");
    return false;
  }

  if (!_write_word(CMD_CONTROL, CTRL_RESET)) {
    return false;
  }
  // The chip re-initialises RAM from ROM.
  RTOS::delay_ms(500);
  if (!_wait_cfgupdate_flag(false, 3000)) {
    ESP_LOGW(TAG, "CFGUPDATE did not clear after RESET");
    return false;
  }

  uint16_t dc = 0;
  if (read_design_capacity_mah(dc)) {
    ESP_LOGI(TAG, "Reset complete — Design Capacity is now %umAh (ROM default)", dc);
  } else {
    ESP_LOGW(TAG, "Reset complete but could not read back Design Capacity");
  }
  return true;
}

// ---------------------------------------------------------------------------
// I2C helpers
// ---------------------------------------------------------------------------

bool BQ27427::_read_word(uint8_t cmd, uint16_t &out) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {0, 0};
  esp_err_t err = i2c_master_transmit_receive(_dev, &cmd, 1, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    return false;
  }
  // BQ27xxx standard commands are little-endian.
  out = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
  return true;
}

bool BQ27427::_write_word(uint8_t cmd, uint16_t value) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[3] = {cmd, static_cast<uint8_t>(value & 0xFF),
                    static_cast<uint8_t>((value >> 8) & 0xFF)};
  esp_err_t err = i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ27427::_read_byte(uint8_t reg, uint8_t &out) {
  if (_dev == nullptr) {
    return false;
  }
  esp_err_t err = i2c_master_transmit_receive(_dev, &reg, 1, &out, 1, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read byte 0x%02X failed: %s", reg, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ27427::_write_byte(uint8_t reg, uint8_t value) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {reg, value};
  esp_err_t err = i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write byte 0x%02X failed: %s", reg, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ27427::_read_block(uint8_t reg, uint8_t *buf, size_t len) {
  if (_dev == nullptr || buf == nullptr || len == 0) {
    return false;
  }
  esp_err_t err = i2c_master_transmit_receive(_dev, &reg, 1, buf, len, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read block 0x%02X len=%u failed: %s", reg, static_cast<unsigned>(len),
             esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ27427::_write_block(uint8_t reg, const uint8_t *buf, size_t len) {
  if (_dev == nullptr || buf == nullptr || len == 0 || len > 64) {
    return false;
  }
  uint8_t tx[65];
  tx[0] = reg;
  for (size_t i = 0; i < len; ++i) {
    tx[1 + i] = buf[i];
  }
  esp_err_t err = i2c_master_transmit(_dev, tx, len + 1, _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "write block 0x%02X len=%u failed: %s", reg, static_cast<unsigned>(len),
             esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ27427::_select_data_block(uint8_t subclass, uint8_t block_offset) {
  if (!_write_byte(CMD_BLOCK_DATA_CONTROL, 0x00)) {
    return false;
  }
  if (!_write_byte(CMD_DATA_BLOCK_CLASS, subclass)) {
    return false;
  }
  if (!_write_byte(CMD_DATA_BLOCK, block_offset)) {
    return false;
  }
  return true;
}

bool BQ27427::_wait_cfgupdate_flag(bool expected_set, uint32_t timeout_ms) {
  const uint32_t start = static_cast<uint32_t>(RTOS::get_time_ms());
  while (true) {
    uint16_t flags = 0;
    if (read_flags(flags)) {
      const bool now_set = (flags & FLAG_CFGUPDATE) != 0;
      if (now_set == expected_set) {
        return true;
      }
    }
    const uint32_t elapsed = static_cast<uint32_t>(RTOS::get_time_ms()) - start;
    if (elapsed >= timeout_ms) {
      ESP_LOGW(TAG, "CFGUPDATE flag did not become %s within %ums", expected_set ? "set" : "clear",
               static_cast<unsigned>(timeout_ms));
      return false;
    }
    RTOS::delay_ms(25);
  }
}

bool BQ27427::_unseal() {
  if (!_write_word(CMD_CONTROL, CTRL_UNSEAL_KEY)) {
    return false;
  }
  if (!_write_word(CMD_CONTROL, CTRL_UNSEAL_KEY)) {
    return false;
  }
  RTOS::delay_ms(10);
  return true;
}
