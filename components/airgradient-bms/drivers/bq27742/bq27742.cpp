/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/bq27742/bq27742.h"

#include <cstdio>

#include "esp_log.h"
#include "rtos.h"

static constexpr const char *TAG = "BQ27742";

// ---------------------------------------------------------------------------
// bq27742-G1 register / subcommand constants (datasheet Tables 8–10, TRM §4–5)
// ---------------------------------------------------------------------------

namespace {

// Standard commands.
constexpr uint8_t CMD_CONTROL = 0x00;
constexpr uint8_t CMD_TEMPERATURE = 0x06;
constexpr uint8_t CMD_VOLTAGE = 0x08;
constexpr uint8_t CMD_FLAGS = 0x0A;
constexpr uint8_t CMD_REMAIN_CAP = 0x10;
constexpr uint8_t CMD_FULL_CHARGE_CAP = 0x12;
constexpr uint8_t CMD_AVG_CURRENT = 0x14;
constexpr uint8_t CMD_SAFETY_STATUS = 0x1A;
constexpr uint8_t CMD_INT_TEMP = 0x28;
constexpr uint8_t CMD_SOC = 0x2C;

// Extended commands.
constexpr uint8_t CMD_DESIGN_CAPACITY = 0x3C;
constexpr uint8_t CMD_DATA_FLASH_CLASS = 0x3E;
constexpr uint8_t CMD_DATA_FLASH_BLOCK = 0x3F;
constexpr uint8_t CMD_BLOCK_DATA_BASE = 0x40;
constexpr uint8_t CMD_BLOCK_DATA_CHECKSUM = 0x60;
constexpr uint8_t CMD_BLOCK_DATA_CONTROL = 0x61;
constexpr uint8_t CMD_PROTECTOR_STATUS = 0x6D;
constexpr uint8_t CMD_PROTECTOR_STATE = 0x78;

// Control() subcommands.
constexpr uint16_t CTRL_CONTROL_STATUS = 0x0000;
constexpr uint16_t CTRL_DEVICE_TYPE = 0x0001;
constexpr uint16_t CTRL_FW_VERSION = 0x0002;
constexpr uint16_t CTRL_HW_VERSION = 0x0003;
constexpr uint16_t CTRL_PREV_MACWRITE = 0x0007;
constexpr uint16_t CTRL_IT_ENABLE = 0x0021;

// Default unseal key 0x36720414 (TRM §5.9.1): Control(0x0414) then Control(0x3672).
constexpr uint16_t UNSEAL_KEY_1 = 0x0414;
constexpr uint16_t UNSEAL_KEY_0 = 0x3672;

// Data-flash subclasses and offsets (TRM Tables 5-3…5-9).
constexpr uint8_t SUBCLASS_SAFETY = 2;   // OV/UV/OT protection thresholds
constexpr uint8_t SUBCLASS_DATA = 48;    // Design Capacity @12, Design Energy @14
constexpr uint8_t SUBCLASS_POWER = 68;   // Sleep Current @2
constexpr uint8_t SUBCLASS_IT_CFG = 80;  // Terminate Voltage @64
constexpr uint8_t SUBCLASS_STATE = 82;   // Qmax Cell 0 @0, Update Status @2
constexpr uint8_t SUBCLASS_RA0 = 88;     // flag @0, Ra 0..14 @2..30
constexpr uint8_t OFFSET_OV_PROT_THRESHOLD = 0;
constexpr uint8_t OFFSET_OV_PROT_RECOVERY = 3;
constexpr uint8_t OFFSET_UV_PROT_THRESHOLD = 5;
constexpr uint8_t OFFSET_UV_PROT_RECOVERY = 8;
constexpr uint8_t OFFSET_OT_CHG = 12;
constexpr uint8_t OFFSET_OT_CHG_RECOVERY = 15;
constexpr uint8_t OFFSET_OT_DSG = 17;
constexpr uint8_t OFFSET_OT_DSG_RECOVERY = 20;
constexpr uint8_t OFFSET_DESIGN_CAPACITY = 12;
constexpr uint8_t OFFSET_DESIGN_ENERGY = 14;
constexpr uint8_t OFFSET_SLEEP_CURRENT = 2;
constexpr uint8_t OFFSET_TERMINATE_VOLTAGE = 64;
constexpr uint8_t OFFSET_QMAX_CELL0 = 0;
constexpr uint8_t OFFSET_UPDATE_STATUS = 2;
constexpr uint8_t OFFSET_RA_FIRST = 2;

constexpr size_t DF_BLOCK_SIZE = 32;

// Flags() bits on this part (TRM Table 4-4).
constexpr uint16_t RAW_FLAG_DSG = (1u << 0);
constexpr uint16_t RAW_FLAG_CHG = (1u << 3);
constexpr uint16_t RAW_FLAG_FC = (1u << 9);

constexpr uint32_t CONTROL_SETTLE_MS = 2;
constexpr uint32_t DF_SETTLE_MS = 10;

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

BQ27742::BQ27742(i2c_master_bus_handle_t bus) : _bus(bus) {}

BQ27742::BQ27742(i2c_master_bus_handle_t bus, const Config &config) : _bus(bus), _config(config) {}

BQ27742::~BQ27742() {
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------

bool BQ27742::init() {
  if (_dev != nullptr) {
    return true;
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
  if (device_type != DEVICE_TYPE_BQ27742) {
    ESP_LOGE(TAG, "DEVICE_TYPE=0x%04X, expected 0x%04X", device_type, DEVICE_TYPE_BQ27742);
    _log_identity_diagnostics(device_type);
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
    return false;
  }

  uint16_t fw = 0;
  read_fw_version(fw);
  ESP_LOGI(TAG, "bq27742-G1 found at 0x%02X (DEVICE_TYPE=0x%04X FW=0x%04X)", _config.address,
           device_type, fw);
  return true;
}

bool BQ27742::ready() const { return _dev != nullptr; }

// ---------------------------------------------------------------------------
// Standard command reads
// ---------------------------------------------------------------------------

bool BQ27742::read_soc_percent(uint8_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_SOC, raw) || raw > 100) {
    return false;
  }
  out = static_cast<uint8_t>(raw);
  return true;
}

bool BQ27742::read_voltage_mv(uint16_t &out) { return _read_word(CMD_VOLTAGE, out); }

bool BQ27742::read_average_current_ma(int16_t &out) {
  uint16_t raw = 0;
  if (!_read_word(CMD_AVG_CURRENT, raw)) {
    return false;
  }
  out = static_cast<int16_t>(raw);
  return true;
}

bool BQ27742::read_average_power_mw(int16_t &out) {
  uint16_t mv = 0;
  int16_t ma = 0;
  if (!read_voltage_mv(mv) || !read_average_current_ma(ma)) {
    return false;
  }
  const int32_t mw = (static_cast<int32_t>(mv) * static_cast<int32_t>(ma)) / 1000;
  if (mw > INT16_MAX || mw < INT16_MIN) {
    return false;
  }
  out = static_cast<int16_t>(mw);
  return true;
}

bool BQ27742::read_remaining_capacity_mah(uint16_t &out) { return _read_word(CMD_REMAIN_CAP, out); }

bool BQ27742::read_full_charge_capacity_mah(uint16_t &out) {
  return _read_word(CMD_FULL_CHARGE_CAP, out);
}

bool BQ27742::read_internal_temperature_c(float &out) {
  uint16_t dk = 0;
  if (!_read_word(CMD_INT_TEMP, dk)) {
    return false;
  }
  out = (static_cast<float>(dk) * 0.1f) - 273.15f;
  return true;
}

bool BQ27742::read_temperature_c(float &out) {
  uint16_t dk = 0;
  if (!_read_word(CMD_TEMPERATURE, dk)) {
    return false;
  }
  out = (static_cast<float>(dk) * 0.1f) - 273.15f;
  return true;
}

bool BQ27742::read_flags_raw(uint16_t &out) { return _read_word(CMD_FLAGS, out); }

bool BQ27742::read_flags(uint16_t &out) {
  uint16_t raw = 0;
  if (!read_flags_raw(raw)) {
    return false;
  }
  // The gauge is powered from the cell, so a readable Flags() implies a cell.
  uint16_t norm = FgFlags::BAT_DET;
  if (raw & RAW_FLAG_DSG) {
    norm |= FgFlags::DSG;
  }
  if (raw & RAW_FLAG_CHG) {
    norm |= FgFlags::CHG;
  }
  if (raw & RAW_FLAG_FC) {
    norm |= FgFlags::FC;
  }
  out = norm;
  return true;
}

bool BQ27742::read_safety_status(uint16_t &out) { return _read_word(CMD_SAFETY_STATUS, out); }

bool BQ27742::read_protector_status(uint8_t &out) { return _read_byte(CMD_PROTECTOR_STATUS, out); }

bool BQ27742::read_protector_state(uint8_t &out) { return _read_byte(CMD_PROTECTOR_STATE, out); }

// ---------------------------------------------------------------------------
// Control() subcommands
// ---------------------------------------------------------------------------

void BQ27742::_log_identity_diagnostics(uint16_t first_device_type) {
  constexpr int REPEATS = 10;
  constexpr uint16_t FW_VERSION_EXPECTED = 0x0103;
  constexpr uint16_t HW_VERSION_A = 0x0000;
  constexpr uint16_t HW_VERSION_B = 0x0060;

  ESP_LOGW(TAG, "identity probe (read-only) — the part answers on I2C but reports a "
                "different DEVICE_TYPE");

  char list[REPEATS * 6 + 1] = {};
  size_t used = 0;
  int reads_ok = 0;
  int identical = 0;
  int as_expected = 0;
  for (int i = 0; i < REPEATS; ++i) {
    uint16_t v = 0;
    if (!control_subcommand(CTRL_DEVICE_TYPE, v)) {
      used += static_cast<size_t>(snprintf(list + used, sizeof(list) - used, " ....."));
      continue;
    }
    ++reads_ok;
    if (v == first_device_type) {
      ++identical;
    }
    if (v == DEVICE_TYPE_BQ27742) {
      ++as_expected;
    }
    used += static_cast<size_t>(snprintf(list + used, sizeof(list) - used, " %04X", v));
  }
  ESP_LOGW(TAG, "  DEVICE_TYPE x%d:%s", REPEATS, list);
  ESP_LOGW(TAG, "  reads_ok=%d/%d identical=%d correct_0742=%d", reads_ok, REPEATS, identical,
           as_expected);

  uint16_t fw = 0;
  if (control_subcommand(CTRL_FW_VERSION, fw)) {
    ESP_LOGW(TAG, "  FW_VERSION=0x%04X (bq27742-G1 reports 0x%04X) %s", fw, FW_VERSION_EXPECTED,
             fw == FW_VERSION_EXPECTED ? "MATCH" : "mismatch");
  } else {
    ESP_LOGW(TAG, "  FW_VERSION read failed");
  }

  uint16_t hw = 0;
  if (control_subcommand(CTRL_HW_VERSION, hw)) {
    ESP_LOGW(TAG, "  HW_VERSION=0x%04X (bq27742-G1 reports 0x%04X or 0x%04X) %s", hw, HW_VERSION_A,
             HW_VERSION_B, (hw == HW_VERSION_A || hw == HW_VERSION_B) ? "MATCH" : "mismatch");
  } else {
    ESP_LOGW(TAG, "  HW_VERSION read failed");
  }

  // PREV_MACWRITE returns the previous Control() subcommand code, so it shows
  // directly whether the subcommand latch is working.
  uint16_t prev = 0;
  if (control_subcommand(CTRL_PREV_MACWRITE, prev)) {
    ESP_LOGW(TAG, "  PREV_MACWRITE=0x%04X (expect 0x%04X, the HW_VERSION just issued) %s", prev,
             CTRL_HW_VERSION, prev == CTRL_HW_VERSION ? "MATCH — latch works" : "mismatch");
  } else {
    ESP_LOGW(TAG, "  PREV_MACWRITE read failed");
  }

  // Voltage() is a standard command: it does not go through Control() at all,
  // so it tells us whether the part behaves like a gauge independently of the
  // subcommand mechanism.  Compare against the charger's VBAT_ADC in the same
  // boot.
  uint16_t mv = 0;
  if (_read_word(CMD_VOLTAGE, mv)) {
    ESP_LOGW(TAG, "  Voltage()=%u mV (standard command — compare with charger vbat)", mv);
  } else {
    ESP_LOGW(TAG, "  Voltage() read failed");
  }

  ESP_LOGW(TAG, "  I2C link to 0x%02X is WORKING (%d/%d reads returned data)", _config.address,
           reads_ok, REPEATS);
}

bool BQ27742::control_subcommand(uint16_t subcmd, uint16_t &result) {
  if (!_write_word(CMD_CONTROL, subcmd)) {
    return false;
  }
  RTOS::delay_ms(CONTROL_SETTLE_MS);
  return _read_word(CMD_CONTROL, result);
}

bool BQ27742::read_control_status(uint16_t &out) {
  return control_subcommand(CTRL_CONTROL_STATUS, out);
}

bool BQ27742::read_fw_version(uint16_t &out) { return control_subcommand(CTRL_FW_VERSION, out); }

bool BQ27742::select_chemistry_4v2() {
  // No Chem-ID switching on this part; the chemistry table is loaded into
  // data flash with bqStudio (bring-up plan P5-14).
  return ready();
}

bool BQ27742::set_update_status_learning(bool enable) {
  if (!ready()) {
    return false;
  }
  if (!enable) {
    return true; // IT cannot be disabled once enabled (TRM §4.1.1: QEN latches)
  }
  if (!_unseal()) {
    return false;
  }
  if (!_write_word(CMD_CONTROL, CTRL_IT_ENABLE)) {
    return false;
  }
  RTOS::delay_ms(DF_SETTLE_MS);
  uint8_t status = 0;
  if (read_update_status(status)) {
    ESP_LOGI(TAG, "IT ENABLE sent, Update Status=0x%02X", status);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Data flash reads
// ---------------------------------------------------------------------------

bool BQ27742::read_design_capacity_mah(uint16_t &out) {
  return _read_word(CMD_DESIGN_CAPACITY, out);
}

bool BQ27742::read_qmax_cell0(uint16_t &out) {
  uint8_t block[DF_BLOCK_SIZE] = {};
  if (!_read_df_block(SUBCLASS_STATE, 0, block)) {
    return false;
  }
  out = (static_cast<uint16_t>(block[OFFSET_QMAX_CELL0]) << 8) | block[OFFSET_QMAX_CELL0 + 1];
  return true;
}

bool BQ27742::read_update_status(uint8_t &out) {
  uint8_t block[DF_BLOCK_SIZE] = {};
  if (!_read_df_block(SUBCLASS_STATE, 0, block)) {
    return false;
  }
  out = block[OFFSET_UPDATE_STATUS];
  return true;
}

bool BQ27742::read_ra_table(int16_t *out, size_t len) {
  if (out == nullptr || len < RA_TABLE_SIZE) {
    return false;
  }
  uint8_t block[DF_BLOCK_SIZE] = {};
  if (!_read_df_block(SUBCLASS_RA0, 0, block)) {
    return false;
  }
  for (size_t i = 0; i < RA_TABLE_SIZE; ++i) {
    const size_t off = OFFSET_RA_FIRST + i * 2;
    out[i] = static_cast<int16_t>((static_cast<uint16_t>(block[off]) << 8) | block[off + 1]);
  }
  return true;
}

bool BQ27742::read_cell_config(FgCellConfig &out) {
  uint8_t block[DF_BLOCK_SIZE] = {};
  auto unpack = [&block](uint8_t offset) -> uint16_t {
    return (static_cast<uint16_t>(block[offset]) << 8) | block[offset + 1];
  };

  if (!_read_df_block(SUBCLASS_DATA, 0, block)) {
    return false;
  }
  out.design_capacity_mah = unpack(OFFSET_DESIGN_CAPACITY);
  out.design_energy_mwh = unpack(OFFSET_DESIGN_ENERGY);

  if (!_read_df_block(SUBCLASS_IT_CFG, OFFSET_TERMINATE_VOLTAGE / DF_BLOCK_SIZE, block)) {
    return false;
  }
  out.terminate_voltage_mv = unpack(OFFSET_TERMINATE_VOLTAGE % DF_BLOCK_SIZE);

  if (!_read_df_block(SUBCLASS_POWER, 0, block)) {
    return false;
  }
  out.sleep_current_ma = unpack(OFFSET_SLEEP_CURRENT);
  return true;
}

bool BQ27742::read_protection_config(FgProtectionConfig &out) {
  uint8_t block[DF_BLOCK_SIZE] = {};
  if (!_read_df_block(SUBCLASS_SAFETY, 0, block)) {
    return false;
  }
  auto unpack = [&block](uint8_t offset) -> uint16_t {
    return (static_cast<uint16_t>(block[offset]) << 8) | block[offset + 1];
  };
  out.ov_prot_threshold_mv = unpack(OFFSET_OV_PROT_THRESHOLD);
  out.ov_prot_recovery_mv = unpack(OFFSET_OV_PROT_RECOVERY);
  out.uv_prot_threshold_mv = unpack(OFFSET_UV_PROT_THRESHOLD);
  out.uv_prot_recovery_mv = unpack(OFFSET_UV_PROT_RECOVERY);
  out.ot_chg_dc = static_cast<int16_t>(unpack(OFFSET_OT_CHG));
  out.ot_chg_recovery_dc = static_cast<int16_t>(unpack(OFFSET_OT_CHG_RECOVERY));
  out.ot_dsg_dc = static_cast<int16_t>(unpack(OFFSET_OT_DSG));
  out.ot_dsg_recovery_dc = static_cast<int16_t>(unpack(OFFSET_OT_DSG_RECOVERY));
  return true;
}

// ---------------------------------------------------------------------------
// Data flash writes
// ---------------------------------------------------------------------------
bool BQ27742::write_protection_config(const FgProtectionConfig &cfg) {
  if (!ready()) {
    return false;
  }

  // Out-of-range data flash values are accepted silently and persist, so the
  // limits from TRM Table 5-3 are enforced here instead of by the gauge.
  struct Bound {
    int32_t value;
    int32_t min;
    int32_t max;
    const char *name;
  };
  const Bound bounds[] = {
      {cfg.ov_prot_threshold_mv, 4200, 4600, "OV Prot Threshold"},
      {cfg.ov_prot_recovery_mv, 4100, 4500, "OV Prot Recovery"},
      {cfg.uv_prot_threshold_mv, 2300, 3100, "UV Prot Threshold"},
      {cfg.uv_prot_recovery_mv, 2400, 3200, "UV Prot Recovery"},
      {cfg.ot_chg_dc, 0, 1200, "OT Chg"},
      {cfg.ot_chg_recovery_dc, 0, 1200, "OT Chg Recovery"},
      {cfg.ot_dsg_dc, 0, 1200, "OT Dsg"},
      {cfg.ot_dsg_recovery_dc, 0, 1200, "OT Dsg Recovery"},
  };
  for (const Bound &b : bounds) {
    if (b.value < b.min || b.value > b.max) {
      ESP_LOGE(TAG, "%s %ld outside %ld..%ld - refusing to write protection config", b.name,
               static_cast<long>(b.value), static_cast<long>(b.min), static_cast<long>(b.max));
      return false;
    }
  }

  uint8_t block[DF_BLOCK_SIZE] = {};
  if (!_read_df_block(SUBCLASS_SAFETY, 0, block)) {
    return false;
  }
  // Only the eight threshold words are replaced.  The U1 delay/time fields at
  // offsets 2, 7, 14 and 19 keep whatever the gauge shipped with: a zero in UV
  // Prot Delay disables undervoltage protection entirely (TRM §5.3.1.2).
  auto pack = [&block](uint8_t offset, uint16_t value) {
    block[offset] = static_cast<uint8_t>((value >> 8) & 0xFF);
    block[offset + 1] = static_cast<uint8_t>(value & 0xFF);
  };
  pack(OFFSET_OV_PROT_THRESHOLD, cfg.ov_prot_threshold_mv);
  pack(OFFSET_OV_PROT_RECOVERY, cfg.ov_prot_recovery_mv);
  pack(OFFSET_UV_PROT_THRESHOLD, cfg.uv_prot_threshold_mv);
  pack(OFFSET_UV_PROT_RECOVERY, cfg.uv_prot_recovery_mv);
  pack(OFFSET_OT_CHG, static_cast<uint16_t>(cfg.ot_chg_dc));
  pack(OFFSET_OT_CHG_RECOVERY, static_cast<uint16_t>(cfg.ot_chg_recovery_dc));
  pack(OFFSET_OT_DSG, static_cast<uint16_t>(cfg.ot_dsg_dc));
  pack(OFFSET_OT_DSG_RECOVERY, static_cast<uint16_t>(cfg.ot_dsg_recovery_dc));

  if (!_write_df_block(SUBCLASS_SAFETY, 0, block)) {
    return false;
  }

  RTOS::delay_ms(50);
  FgProtectionConfig verify{};
  if (!read_protection_config(verify)) {
    ESP_LOGE(TAG, "protection config readback failed after write");
    return false;
  }
  if (verify != cfg) {
    ESP_LOGE(TAG, "protection config write did NOT stick - readback mismatch");
    return false;
  }
  ESP_LOGI(TAG,
           "protection config verified (OV %u/%u mV, UV %u/%u mV, OT chg %.1f/%.1f C, "
           "dsg %.1f/%.1f C)",
           cfg.ov_prot_threshold_mv, cfg.ov_prot_recovery_mv, cfg.uv_prot_threshold_mv,
           cfg.uv_prot_recovery_mv, cfg.ot_chg_dc / 10.0f, cfg.ot_chg_recovery_dc / 10.0f,
           cfg.ot_dsg_dc / 10.0f, cfg.ot_dsg_recovery_dc / 10.0f);
  return true;
}


bool BQ27742::write_cell_config(const FgCellConfig &cfg) {
  if (!ready()) {
    return false;
  }
  if (!_write_df_word(SUBCLASS_DATA, OFFSET_DESIGN_CAPACITY, cfg.design_capacity_mah) ||
      !_write_df_word(SUBCLASS_DATA, OFFSET_DESIGN_ENERGY, cfg.design_energy_mwh) ||
      !_write_df_word(SUBCLASS_IT_CFG, OFFSET_TERMINATE_VOLTAGE, cfg.terminate_voltage_mv) ||
      !_write_df_word(SUBCLASS_POWER, OFFSET_SLEEP_CURRENT, cfg.sleep_current_ma)) {
    return false;
  }

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
  ESP_LOGI(TAG, "cell config verified (DC=%u DE=%u TermV=%u SleepI=%u); applies at next IT init",
           cfg.design_capacity_mah, cfg.design_energy_mwh, cfg.terminate_voltage_mv,
           cfg.sleep_current_ma);
  return true;
}

// ---------------------------------------------------------------------------
// Data flash helpers
// ---------------------------------------------------------------------------

bool BQ27742::_unseal() {
  // Sending the keys while already unsealed is harmless.
  if (!_write_word(CMD_CONTROL, UNSEAL_KEY_1) || !_write_word(CMD_CONTROL, UNSEAL_KEY_0)) {
    return false;
  }
  RTOS::delay_ms(DF_SETTLE_MS);
  return true;
}

bool BQ27742::_read_df_block(uint8_t subclass, uint8_t block, uint8_t *out32) {
  if (!_unseal()) {
    return false;
  }
  if (!_write_byte(CMD_BLOCK_DATA_CONTROL, 0x00) || !_write_byte(CMD_DATA_FLASH_CLASS, subclass) ||
      !_write_byte(CMD_DATA_FLASH_BLOCK, block)) {
    return false;
  }
  RTOS::delay_ms(DF_SETTLE_MS);
  return _read_block(CMD_BLOCK_DATA_BASE, out32, DF_BLOCK_SIZE);
}

bool BQ27742::_write_df_block(uint8_t subclass, uint8_t block, const uint8_t *in32) {
  if (!_unseal()) {
    return false;
  }
  if (!_write_byte(CMD_BLOCK_DATA_CONTROL, 0x00) || !_write_byte(CMD_DATA_FLASH_CLASS, subclass) ||
      !_write_byte(CMD_DATA_FLASH_BLOCK, block)) {
    return false;
  }
  RTOS::delay_ms(DF_SETTLE_MS);
  if (!_write_block(CMD_BLOCK_DATA_BASE, in32, DF_BLOCK_SIZE)) {
    return false;
  }
  RTOS::delay_ms(DF_SETTLE_MS);

  uint16_t sum = 0;
  for (size_t i = 0; i < DF_BLOCK_SIZE; ++i) {
    sum += in32[i];
  }
  const uint8_t csum = static_cast<uint8_t>(255 - (sum & 0xFF));
  // Writing the checksum commits the block to data flash.
  if (!_write_byte(CMD_BLOCK_DATA_CHECKSUM, csum)) {
    return false;
  }
  RTOS::delay_ms(100); // flash program time
  return true;
}

bool BQ27742::_write_df_word(uint8_t subclass, uint8_t offset, uint16_t value) {
  const uint8_t block_index = offset / DF_BLOCK_SIZE;
  const uint8_t idx = offset % DF_BLOCK_SIZE;
  if (idx + 1 >= DF_BLOCK_SIZE) {
    return false; // word must not straddle a block
  }
  uint8_t block[DF_BLOCK_SIZE] = {};
  if (!_read_df_block(subclass, block_index, block)) {
    return false;
  }
  block[idx] = static_cast<uint8_t>((value >> 8) & 0xFF);
  block[idx + 1] = static_cast<uint8_t>(value & 0xFF);
  return _write_df_block(subclass, block_index, block);
}

// ---------------------------------------------------------------------------
// I2C helpers
// ---------------------------------------------------------------------------

bool BQ27742::_read_word(uint8_t cmd, uint16_t &out) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {0, 0};
  esp_err_t err = i2c_master_transmit_receive(_dev, &cmd, 1, buf, sizeof(buf), _config.timeout_ms);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "read cmd 0x%02X failed: %s", cmd, esp_err_to_name(err));
    return false;
  }
  out = static_cast<uint16_t>(buf[0]) | (static_cast<uint16_t>(buf[1]) << 8);
  return true;
}

bool BQ27742::_write_word(uint8_t cmd, uint16_t value) {
  // This part supports only the 1-byte write format and NACKs every data byte
  // after the first (TRM §3.4, Figure 3-1 and the "Attempt at incremental
  // writes" case below it).  A single 3-byte transaction therefore delivers
  // the low byte and drops the high one, leaving Control() with no complete
  // subcommand to latch — so the word goes out as two 1-byte writes to
  // consecutive command addresses.  The BQ27427 on v1 does accept the
  // incremental form, which is why the same code works there.
  return _write_byte(cmd, static_cast<uint8_t>(value & 0xFF)) &&
         _write_byte(static_cast<uint8_t>(cmd + 1), static_cast<uint8_t>((value >> 8) & 0xFF));
}

bool BQ27742::_read_byte(uint8_t reg, uint8_t &out) {
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

bool BQ27742::_write_byte(uint8_t reg, uint8_t value) {
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

bool BQ27742::_read_block(uint8_t reg, uint8_t *buf, size_t len) {
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

bool BQ27742::_write_block(uint8_t reg, const uint8_t *buf, size_t len) {
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
