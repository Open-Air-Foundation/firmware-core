/**
 * @file bq25629.cpp
 * @brief BQ25629 Battery Charger Driver Implementation
 */

#include "bq25629.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cmath>
#include <cstring>

static const char *TAG = "BQ25629";

namespace drivers {

struct SocPoint {
  uint16_t mv;
  uint8_t soc;
};

// Simple Li-ion / LiPo OCV -> SOC table (tunable).
// Values are approximate for a 1S cell at rest.
static constexpr SocPoint SOC_LUT[] = {
    {4200, 100}, {4110, 90}, {4020, 80}, {3940, 70}, {3860, 60}, {3790, 50},
    {3720, 40},  {3660, 30}, {3600, 20}, {3450, 10}, {3300, 0},
};

static uint16_t clamp_u16(uint16_t v, uint16_t lo, uint16_t hi) {
  if (v < lo) {
    return lo;
  }
  if (v > hi) {
    return hi;
  }
  return v;
}

static uint8_t soc_from_ocv_mv(uint16_t ocv_mv) {
  const size_t n = sizeof(SOC_LUT) / sizeof(SOC_LUT[0]);
  if (n == 0) {
    return 0;
  }

  if (ocv_mv >= SOC_LUT[0].mv) {
    return SOC_LUT[0].soc;
  }
  if (ocv_mv <= SOC_LUT[n - 1].mv) {
    return SOC_LUT[n - 1].soc;
  }

  for (size_t i = 0; i + 1 < n; ++i) {
    const uint16_t mv_hi = SOC_LUT[i].mv;
    const uint16_t mv_lo = SOC_LUT[i + 1].mv;
    if (ocv_mv <= mv_hi && ocv_mv > mv_lo) {
      const uint8_t soc_hi = SOC_LUT[i].soc;
      const uint8_t soc_lo = SOC_LUT[i + 1].soc;

      const uint16_t dv = mv_hi - mv_lo;
      const uint16_t num = ocv_mv - mv_lo;
      const uint16_t dsoc = (uint16_t)(soc_hi - soc_lo);
      const uint16_t add = (uint16_t)((uint32_t)num * (uint32_t)dsoc + (dv / 2)) / dv;
      return (uint8_t)(soc_lo + add);
    }
  }

  return 0;
}

static uint16_t estimate_ocv_mv(uint16_t vbat_mv, int16_t ibat_ma) {
  // Basic load compensation: OCV ~= VBAT + I*R when discharging.
  // This is intentionally simple; tune R for your specific cell and wiring.
  static constexpr uint32_t R_MOHM = 120;
  static constexpr int MAX_COMP_I_MA = 2000;

  uint16_t ocv_mv = vbat_mv;
  if (ibat_ma < 0) {
    int cur_ma = -ibat_ma;
    if (cur_ma > MAX_COMP_I_MA) {
      cur_ma = MAX_COMP_I_MA;
    }
    uint32_t add_mv = (uint32_t)cur_ma * R_MOHM;
    add_mv = (add_mv + 500) / 1000;
    uint32_t tmp = (uint32_t)ocv_mv + add_mv;
    if (tmp > 5000) {
      tmp = 5000;
    }
    ocv_mv = (uint16_t)tmp;
  }
  return ocv_mv;
}

// I2C timeout
constexpr int I2C_TIMEOUT_MS = 1000;

// Register bit masks
namespace BIT_MASK {
// CHARGER_CONTROL_0 (0x16)
constexpr uint8_t EN_AUTO_IBATDIS = (1 << 7);
constexpr uint8_t EN_CHG = (1 << 5);
constexpr uint8_t EN_HIZ = (1 << 4);
constexpr uint8_t FORCE_PMID_DIS = (1 << 3);
constexpr uint8_t WD_RST = (1 << 2);
constexpr uint8_t WATCHDOG_MASK = 0x03; // bits [1:0]
constexpr uint8_t WATCHDOG_DISABLE = 0x00;
constexpr uint8_t WATCHDOG_50S = 0x01;
constexpr uint8_t WATCHDOG_100S = 0x02;
constexpr uint8_t WATCHDOG_200S = 0x03;

// CHARGER_CONTROL_1 (0x17)
constexpr uint8_t REG_RST = (1 << 7);

// CHARGER_CONTROL_2 (0x18)
constexpr uint8_t EN_BYPASS_OTG = (1 << 7);
constexpr uint8_t EN_OTG = (1 << 6);
constexpr uint8_t BATFET_CTRL_WVBUS = (1 << 3);
constexpr uint8_t BATFET_DLY = (1 << 2);
constexpr uint8_t BATFET_CTRL_MASK = 0x03;
constexpr uint8_t BATFET_CTRL_SHUTDOWN = 0x01;
constexpr uint8_t BATFET_CTRL_SHIP = 0x02;
constexpr uint8_t BATFET_CTRL_RESET = 0x03;

// NTC_CONTROL_0 (0x1A)
constexpr uint8_t TS_IGNORE = (1 << 7);

// ADC_CONTROL (0x26)
constexpr uint8_t ADC_EN = (1 << 7);
constexpr uint8_t ADC_RATE = (1 << 6);

// CHARGER_STATUS_1 (0x1E)
constexpr uint8_t CHG_STAT_MASK = 0x18;
constexpr uint8_t CHG_STAT_SHIFT = 3;
constexpr uint8_t VBUS_STAT_MASK = 0x07;
} // namespace BIT_MASK

esp_err_t BQ25629::enable_auto_ibat_discharge(bool enable) {
  esp_err_t ret = modify_register(BQ25629_REG::CHARGER_CONTROL_0, BIT_MASK::EN_AUTO_IBATDIS,
                                  enable ? BIT_MASK::EN_AUTO_IBATDIS : 0);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to %s EN_AUTO_IBATDIS: %s", enable ? "enable" : "disable",
             esp_err_to_name(ret));
  }
  return ret;
}

BQ25629::BQ25629(i2c_master_bus_handle_t i2c_bus, uint8_t device_address)
    : i2c_bus_(i2c_bus), dev_handle_(nullptr), device_address_(device_address),
      initialized_(false) {}

BQ25629::~BQ25629() { deinit(); }

esp_err_t BQ25629::init(const BQ25629_Config &config) {
  esp_err_t ret;

  // Create I2C device
  i2c_device_config_t dev_cfg = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = device_address_,
      .scl_speed_hz = 400000, // 400kHz I2C
  };

  ret = i2c_master_bus_add_device(i2c_bus_, &dev_cfg, &dev_handle_);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(ret));
    return ret;
  }

  // Verify device by reading part information
  uint8_t part_info = 0;
  ret = read_register(BQ25629_REG::PART_INFORMATION, part_info);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read part information: %s", esp_err_to_name(ret));
    i2c_master_bus_rm_device(dev_handle_);
    dev_handle_ = nullptr;
    return ret;
  }

  uint8_t part_number = (part_info >> 3) & 0x07;
  const char *part_name = "UNKNOWN";
  if (part_number == 0x02) {
    part_name = "BQ25628";
  } else if (part_number == 0x06) {
    part_name = "BQ25629";
  } else {
    ESP_LOGW(TAG, "Unexpected part number: 0x%02X (expected 0x02/0x06)", part_number);
  }

  ESP_LOGI(TAG, "%s found, Part Info: 0x%02X", part_name, part_info);

  ret = apply_config(config);
  if (ret != ESP_OK) {
    return ret;
  }

  initialized_ = true;
  ESP_LOGI(TAG, "BQ25629 initialized successfully");
  return ESP_OK;
}

esp_err_t BQ25629::apply_config(const BQ25629_Config &config) {
  esp_err_t ret;

  // Best-effort: enable EN_AUTO_IBATDIS (CHARGER_CONTROL_0 bit7).
  // Keep init running even if this fails.
  (void)enable_auto_ibat_discharge(true);

  // Configure charge voltage
  ret = set_charge_voltage(config.charge_voltage_mv);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set charge voltage");
    return ret;
  }

  // Configure charge current
  ret = set_charge_current(config.charge_current_ma);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set charge current");
    return ret;
  }

  // Configure input current limit
  ret = set_input_current_limit(config.input_current_limit_ma);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set input current limit");
    return ret;
  }

  // Configure input voltage limit per datasheet: VINDPM_mV = code * 40
  // code = VINDPM_mV / 40
  uint16_t vindpm_value = config.input_voltage_limit_mv / 40;
  vindpm_value = vindpm_value > 0x1A4 ? 0x1A4 : vindpm_value;
  vindpm_value = vindpm_value < 0x5F ? 0x5F : vindpm_value;
  ret = write_register_16(BQ25629_REG::INPUT_VOLTAGE_LIMIT, vindpm_value << 5);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set input voltage limit");
    return ret;
  }

  // Configure minimum system voltage
  uint16_t vsysmin_value = (config.min_system_voltage_mv - 2560) / 80;
  vsysmin_value = vsysmin_value > 0x10 ? 0x10 : vsysmin_value;
  vsysmin_value = vsysmin_value < 0x00 ? 0x00 : vsysmin_value;
  ret = write_register_16(BQ25629_REG::MIN_SYSTEM_VOLTAGE, vsysmin_value << 6);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set minimum system voltage");
    return ret;
  }

  // Configure pre-charge current
  uint16_t iprechg_value = (config.precharge_current_ma - 10) / 10;
  iprechg_value = iprechg_value > 0x1F ? 0x1F : iprechg_value;
  if (iprechg_value < 1)
    iprechg_value = 1;
  ret = write_register_16(BQ25629_REG::PRECHARGE_CONTROL, iprechg_value << 3);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set pre-charge current");
    return ret;
  }

  // Configure termination current
  uint16_t iterm_value = (config.term_current_ma - 5) / 5;
  iterm_value = iterm_value > 0x3E ? 0x3E : iterm_value;
  if (iterm_value < 1)
    iterm_value = 1;
  ret = write_register_16(BQ25629_REG::TERMINATION_CONTROL, iterm_value << 2);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to set termination current");
    return ret;
  }

  // Enable/disable charging
  ret = enable_charging(config.enable_charging);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to configure charging enable");
    return ret;
  }

  // Enable/disable ADC
  if (config.enable_adc) {
    ret = enable_adc(true, true);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to enable ADC");
      return ret;
    }
  }

  // Ensure BATFET_DLY = 0 (25 ms fast disconnect) for deterministic ship-mode timing.
  ret = modify_register(BQ25629_REG::CHARGER_CONTROL_2, BIT_MASK::BATFET_DLY, 0);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to clear BATFET_DLY");
    return ret;
  }

  return ESP_OK;
}

esp_err_t BQ25629::soft_reset() {
  // REG_RST self-clears when the reset completes.  Register-domain only:
  // BATFET stays on, so SYS (and the MCU running this code) keep power.
  esp_err_t ret = modify_register(BQ25629_REG::CHARGER_CONTROL_1, BIT_MASK::REG_RST,
                                  BIT_MASK::REG_RST);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write REG_RST: %s", esp_err_to_name(ret));
    return ret;
  }

  // Poll for self-clear: a single coarse tick (10 ms at 100 Hz FreeRTOS can
  // round down to ~0 ms) must not turn an in-progress reset into a hard
  // failure — the failure path here strands the chip at POR defaults.
  uint8_t cc1 = 0;
  bool cleared = false;
  for (int attempt = 0; attempt < 3 && !cleared; ++attempt) {
    vTaskDelay(pdMS_TO_TICKS(10));
    ret = read_register(BQ25629_REG::CHARGER_CONTROL_1, cc1);
    if (ret != ESP_OK) {
      return ret;
    }
    cleared = (cc1 & BIT_MASK::REG_RST) == 0;
  }
  if (!cleared) {
    ESP_LOGE(TAG, "REG_RST did not self-clear (0x17=0x%02X)", cc1);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "register reset complete (all registers at POR defaults)");
  return ESP_OK;
}

esp_err_t BQ25629::deinit() {
  if (dev_handle_ != nullptr) {
    esp_err_t ret = i2c_master_bus_rm_device(dev_handle_);
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "Failed to remove I2C device: %s", esp_err_to_name(ret));
      return ret;
    }
    dev_handle_ = nullptr;
  }
  initialized_ = false;
  ESP_LOGI(TAG, "BQ25629 deinitialized");
  return ESP_OK;
}

esp_err_t BQ25629::enable_charging(bool enable) {
  return modify_register(BQ25629_REG::CHARGER_CONTROL_0, BIT_MASK::EN_CHG,
                         enable ? BIT_MASK::EN_CHG : 0);
}

esp_err_t BQ25629::enable_otg(bool enable) {
  return modify_register(BQ25629_REG::CHARGER_CONTROL_2, BIT_MASK::EN_OTG,
                         enable ? BIT_MASK::EN_OTG : 0);
}

esp_err_t BQ25629::get_otg_enabled(bool &enabled) {
  uint8_t reg = 0;
  esp_err_t ret = read_register(BQ25629_REG::CHARGER_CONTROL_2, reg);
  if (ret != ESP_OK) {
    return ret;
  }
  enabled = (reg & BIT_MASK::EN_OTG) != 0;
  return ESP_OK;
}

esp_err_t BQ25629::set_charge_current(uint16_t current_ma) {
  // Clamp to valid range: 40-2000mA
  if (current_ma < 40)
    current_ma = 40;
  if (current_ma > 2000)
    current_ma = 2000;

  // Calculate register value per datasheet: ICHG_mA = code * 40
  // code = ICHG_mA / 40
  uint16_t reg_value = current_ma / 40;
  reg_value = reg_value << 5; // Shift to bits [10:5]

  return write_register_16(BQ25629_REG::CHARGE_CURRENT_LIMIT, reg_value);
}

esp_err_t BQ25629::set_charge_voltage(uint16_t voltage_mv) {
  // Clamp to valid range: 3500-4800mV
  if (voltage_mv < 3500)
    voltage_mv = 3500;
  if (voltage_mv > 4800)
    voltage_mv = 4800;

  // Calculate register value per datasheet: VREG_mV = code * 10
  // code = VREG_mV / 10
  uint16_t reg_value = voltage_mv / 10;
  reg_value = reg_value << 3; // Shift to bits [11:3]

  return write_register_16(BQ25629_REG::CHARGE_VOLTAGE_LIMIT, reg_value);
}

esp_err_t BQ25629::set_input_current_limit(uint16_t current_ma) {
  // Clamp to valid range: 100-3200mA
  if (current_ma < 100)
    current_ma = 100;
  if (current_ma > 3200)
    current_ma = 3200;

  // Calculate register value per datasheet: IINDPM_mA = code * 20
  // code = IINDPM_mA / 20
  uint16_t reg_value = current_ma / 20;
  reg_value = reg_value << 4; // Shift to bits [11:4]

  return write_register_16(BQ25629_REG::INPUT_CURRENT_LIMIT, reg_value);
}

esp_err_t BQ25629::get_charge_status(ChargeStatus &status) {
  uint8_t reg_value;
  esp_err_t ret = read_register(BQ25629_REG::CHARGER_STATUS_1, reg_value);
  if (ret != ESP_OK) {
    return ret;
  }

  uint8_t chg_stat = (reg_value & BIT_MASK::CHG_STAT_MASK) >> BIT_MASK::CHG_STAT_SHIFT;
  status = static_cast<ChargeStatus>(chg_stat);
  return ESP_OK;
}

esp_err_t BQ25629::get_vbus_status(VBusStatus &status) {
  uint8_t reg_value;
  esp_err_t ret = read_register(BQ25629_REG::CHARGER_STATUS_1, reg_value);
  if (ret != ESP_OK) {
    return ret;
  }

  uint8_t vbus_stat = reg_value & BIT_MASK::VBUS_STAT_MASK;
  status = static_cast<VBusStatus>(vbus_stat);
  return ESP_OK;
}

esp_err_t BQ25629::read_status(BQ25629_Status &status) {
  uint8_t status0 = 0, status1 = 0;

  // Read CHARGER_STATUS_0 (0x1D)
  esp_err_t ret = read_register(BQ25629_REG::CHARGER_STATUS_0, status0);
  if (ret != ESP_OK)
    return ret;

  // Read CHARGER_STATUS_1 (0x1E)
  ret = read_register(BQ25629_REG::CHARGER_STATUS_1, status1);
  if (ret != ESP_OK)
    return ret;

  // Parse status
  status.charge_status =
      static_cast<ChargeStatus>((status1 & BIT_MASK::CHG_STAT_MASK) >> BIT_MASK::CHG_STAT_SHIFT);
  status.vbus_status = static_cast<VBusStatus>(status1 & BIT_MASK::VBUS_STAT_MASK);
  status.adc_done_stat = (status0 & (1 << 6)) != 0;
  status.treg_stat = (status0 & (1 << 5)) != 0;
  status.vsys_stat = (status0 & (1 << 4)) != 0;
  status.iindpm_stat = (status0 & (1 << 3)) != 0;
  status.vindpm_stat = (status0 & (1 << 2)) != 0;
  status.safety_tmr_stat = (status0 & (1 << 1)) != 0;
  status.wd_stat = (status0 & (1 << 0)) != 0;

  return ESP_OK;
}

esp_err_t BQ25629::read_fault(BQ25629_Fault &fault) {
  uint8_t fault0 = 0;

  // Read FAULT_STATUS_0 (0x1F)
  esp_err_t ret = read_register(BQ25629_REG::FAULT_STATUS_0, fault0);
  if (ret != ESP_OK)
    return ret;

  // Parse faults
  fault.vbus_fault = (fault0 & (1 << 7)) != 0;
  fault.bat_fault = (fault0 & (1 << 6)) != 0;
  fault.sys_fault = (fault0 & (1 << 5)) != 0;
  fault.otg_fault = (fault0 & (1 << 4)) != 0;
  fault.tshut_fault = (fault0 & (1 << 3)) != 0;
  fault.ts_stat = static_cast<uint8_t>(fault0 & 0x07);

  return ESP_OK;
}

esp_err_t BQ25629::read_adc(BQ25629_ADC_Data &data) {
  esp_err_t ret;
  uint16_t raw_value;

  // Ensure ADC is enabled in continuous mode before reading
  // ADC_EN may be disabled after watchdog timeout or one-shot completion
  uint8_t adc_ctrl = 0;
  ret = read_register(BQ25629_REG::ADC_CONTROL, adc_ctrl);
  if (ret == ESP_OK && !(adc_ctrl & BIT_MASK::ADC_EN)) {
    // ADC is disabled, re-enable in continuous mode
    ret = enable_adc(true, true);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "Failed to re-enable ADC: %s", esp_err_to_name(ret));
    } else {
      // Wait for first conversion to complete (~30ms for 12-bit)
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }

  // Read IBUS (2mA per LSB, 2's complement)
  // Bits [15:1] contain the data, bit [0] is reserved
  ret = read_register_16(BQ25629_REG::IBUS_ADC, raw_value);
  if (ret == ESP_OK) {
    int16_t signed_value = static_cast<int16_t>(raw_value) >> 1; // Arithmetic shift preserves sign
    data.ibus_ma = signed_value * 2;
  }

  // Read IBAT (4mA per LSB, 2's complement)
  // Bits [15:2] contain the data, bits [1:0] are reserved
  ret = read_register_16(BQ25629_REG::IBAT_ADC, raw_value);
  if (ret == ESP_OK) {
    if (raw_value == 0x8000) {
      data.ibat_ma = 0; // Polarity changed during measurement
    } else {
      int16_t signed_value =
          static_cast<int16_t>(raw_value) >> 2; // Arithmetic shift preserves sign
      data.ibat_ma = signed_value * 4;
    }
  }

  // Read VBUS (3.97mV per LSB)
  // Bits [14:2] contain the data, bits [15] and [1:0] are reserved
  ret = read_register_16(BQ25629_REG::VBUS_ADC, raw_value);
  if (ret == ESP_OK) {
    data.vbus_mv = (((raw_value >> 2) & 0x1FFF) * 397) / 100;
  }

  // Read VPMID (3.97mV per LSB)
  // Bits [14:2] contain the data, bits [15] and [1:0] are reserved
  ret = read_register_16(BQ25629_REG::VPMID_ADC, raw_value);
  if (ret == ESP_OK) {
    data.vpmid_mv = (((raw_value >> 2) & 0x1FFF) * 397) / 100;
  }

  // Read VBAT (1.99mV per LSB)
  // Bits [12:1] contain the data, bits [15:13] and [0] are reserved
  ret = read_register_16(BQ25629_REG::VBAT_ADC, raw_value);
  if (ret == ESP_OK) {
    data.vbat_mv = (((raw_value >> 1) & 0x0FFF) * 199) / 100;
  }

  // Read VSYS (1.99mV per LSB)
  // Bits [12:1] contain the data, bits [15:13] and [0] are reserved
  ret = read_register_16(BQ25629_REG::VSYS_ADC, raw_value);
  if (ret == ESP_OK) {
    data.vsys_mv = (((raw_value >> 1) & 0x0FFF) * 199) / 100;
  }

  // Read TS (0.0961% per LSB)
  ret = read_register_16(BQ25629_REG::TS_ADC, raw_value);
  if (ret == ESP_OK) {
    data.ts_percent = (raw_value & 0x0FFF) * 0.0961f;
  }

  // Read TDIE (0.5°C per LSB, 2's complement)
  ret = read_register_16(BQ25629_REG::TDIE_ADC, raw_value);
  if (ret == ESP_OK) {
    data.tdie_c = static_cast<int16_t>(raw_value & 0x0FFF);
    if (data.tdie_c & 0x0800) { // Sign extend
      data.tdie_c |= 0xF000;
    }
    data.tdie_c = data.tdie_c / 2; // 0.5°C per LSB
  }

  return ESP_OK;
}

esp_err_t BQ25629::enable_adc(bool enable, bool continuous) {
  uint8_t value = 0;
  if (enable) {
    value |= BIT_MASK::ADC_EN;
    if (!continuous) {
      value |= BIT_MASK::ADC_RATE; // One-shot mode
    }
  }
  return write_register(BQ25629_REG::ADC_CONTROL, value);
}

esp_err_t BQ25629::set_watchdog_timeout(WatchdogTimeout timeout) {
  uint8_t value = static_cast<uint8_t>(timeout) & BIT_MASK::WATCHDOG_MASK;
  esp_err_t ret = modify_register(BQ25629_REG::CHARGER_CONTROL_0, BIT_MASK::WATCHDOG_MASK, value);
  if (ret == ESP_OK) {
    const char *label = "UNKNOWN";
    switch (timeout) {
    case WatchdogTimeout::Disable:
      label = "DISABLED";
      break;
    case WatchdogTimeout::Sec50:
      label = "50s";
      break;
    case WatchdogTimeout::Sec100:
      label = "100s";
      break;
    case WatchdogTimeout::Sec200:
      label = "200s";
      break;
    }
    ESP_LOGI(TAG, "Watchdog timeout set to %s", label);
  } else {
    ESP_LOGE(TAG, "Failed to set watchdog timeout: %s", esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t BQ25629::reset_watchdog() {
  return modify_register(BQ25629_REG::CHARGER_CONTROL_0, BIT_MASK::WD_RST, BIT_MASK::WD_RST);
}

esp_err_t BQ25629::is_battery_present(bool &present) {
  uint16_t vbat_mv;
  esp_err_t ret = read_register_16(BQ25629_REG::VBAT_ADC, vbat_mv);
  if (ret != ESP_OK) {
    return ret;
  }

  // Convert to actual voltage (1.99mV per LSB)
  vbat_mv = ((vbat_mv >> 1) * 199) / 100;

  // Battery present if voltage > 2V
  present = (vbat_mv > 2000);
  return ESP_OK;
}

esp_err_t BQ25629::is_charging(bool &charging) {
  ChargeStatus status;
  esp_err_t ret = get_charge_status(status);
  if (ret != ESP_OK) {
    return ret;
  }

  charging = (status != ChargeStatus::NOT_CHARGING);
  return ESP_OK;
}

esp_err_t BQ25629::estimate_battery_percent(uint8_t &percent) {
  BQ25629_ADC_Data adc = {};
  esp_err_t err = read_adc(adc);
  if (err != ESP_OK) {
    return err;
  }

  // Battery not present (or reading invalid): report 0%.
  if (adc.vbat_mv < 2000) {
    percent = 0;
    return ESP_OK;
  }

  ChargeStatus chg = ChargeStatus::NOT_CHARGING;
  bool charging = false;
  err = get_charge_status(chg);
  if (err == ESP_OK) {
    if (chg != ChargeStatus::NOT_CHARGING) {
      charging = true;
    }
    if (chg == ChargeStatus::TOPOFF_TIMER_ACTIVE) {
      percent = 100;
      return ESP_OK;
    }
  }

  uint16_t ocv_mv = adc.vbat_mv;
  if (!charging) {
    ocv_mv = estimate_ocv_mv(adc.vbat_mv, adc.ibat_ma);
  }

  // Clamp to table range to avoid weird values from load/charger effects.
  const size_t n = sizeof(SOC_LUT) / sizeof(SOC_LUT[0]);
  const uint16_t ocv_clamped = clamp_u16(ocv_mv, SOC_LUT[n - 1].mv, SOC_LUT[0].mv);
  uint8_t soc = soc_from_ocv_mv(ocv_clamped);

  // While charging, avoid reporting 100% unless top-off is active.
  if (charging && soc > 99) {
    soc = 99;
  }

  percent = soc;
  return ESP_OK;
}

esp_err_t BQ25629::log_charger_limits() {
  esp_err_t ret;
  uint16_t reg_value;

  ESP_LOGI(TAG, "=== BQ25629 Charger Limit Registers ===");

  // REG0x02: Charge Current Limit (ICHG)
  // Bits [10:5], POR: 320mA (8h), Range: 40-2000mA, Step: 40mA
  // Formula per datasheet: ICHG_mA = code * 40
  ret = read_register_16(BQ25629_REG::CHARGE_CURRENT_LIMIT, reg_value);
  if (ret == ESP_OK) {
    uint16_t ichg_code = (reg_value >> 5) & 0x3F; // Extract bits [10:5]
    uint16_t ichg_ma = ichg_code * 40;
    ESP_LOGI(TAG, "REG0x02 (Charge Current Limit): 0x%04X → %u mA", reg_value, ichg_ma);
  } else {
    ESP_LOGE(TAG, "Failed to read REG0x02: %s", esp_err_to_name(ret));
  }

  // REG0x04: Charge Voltage Limit (VBATREG)
  // Bits [11:3], POR: 4200mV (1A4h), Range: 3500-4800mV, Step: 10mV
  // Formula per datasheet: VREG_mV = code * 10
  ret = read_register_16(BQ25629_REG::CHARGE_VOLTAGE_LIMIT, reg_value);
  if (ret == ESP_OK) {
    uint16_t vbatreg_code = (reg_value >> 3) & 0x1FF; // Extract bits [11:3]
    uint16_t vbatreg_mv = vbatreg_code * 10;
    ESP_LOGI(TAG, "REG0x04 (Charge Voltage Limit): 0x%04X → %u mV", reg_value, vbatreg_mv);
  } else {
    ESP_LOGE(TAG, "Failed to read REG0x04: %s", esp_err_to_name(ret));
  }

  // REG0x06: Input Current Limit (IINDPM)
  // Bits [11:4], POR: 3200mA (A0h), Range: 100-3200mA, Step: 20mA
  // Formula per datasheet: IINDPM_mA = code * 20
  ret = read_register_16(BQ25629_REG::INPUT_CURRENT_LIMIT, reg_value);
  if (ret == ESP_OK) {
    uint16_t iindpm_code = (reg_value >> 4) & 0xFF; // Extract bits [11:4]
    uint16_t iindpm_ma = iindpm_code * 20;
    ESP_LOGI(TAG, "REG0x06 (Input Current Limit): 0x%04X → %u mA", reg_value, iindpm_ma);
  } else {
    ESP_LOGE(TAG, "Failed to read REG0x06: %s", esp_err_to_name(ret));
  }

  // REG0x08: Input Voltage Limit (VINDPM)
  // Bits [13:5], POR: 4600mV (73h), Range: 3800-16800mV, Step: 40mV
  // Formula per datasheet: VINDPM_mV = code * 40
  ret = read_register_16(BQ25629_REG::INPUT_VOLTAGE_LIMIT, reg_value);
  if (ret == ESP_OK) {
    uint16_t vindpm_code = (reg_value >> 5) & 0x1FF; // Extract bits [13:5]
    uint16_t vindpm_mv = vindpm_code * 40;
    ESP_LOGI(TAG, "REG0x08 (Input Voltage Limit): 0x%04X → %u mV", reg_value, vindpm_mv);
  } else {
    ESP_LOGE(TAG, "Failed to read REG0x08: %s", esp_err_to_name(ret));
  }

  ESP_LOGI(TAG, "=======================================");

  return ESP_OK;
}

esp_err_t BQ25629::log_otg_registers(const char *context) {
  // Best-effort reads; read_register() already logs on I2C failure and the
  // field stays 0 in the table so the dump never aborts mid-way.  Failures
  // are counted so a bus-dead all-zero dump cannot masquerade as a healthy
  // "EN_OTG=0, no faults" chip state.
  int failed_reads = 0;
  auto rd8 = [&](uint8_t reg, uint8_t &v) {
    if (read_register(reg, v) != ESP_OK) {
      ++failed_reads;
    }
  };
  uint8_t cc0 = 0, cc2 = 0, cc3 = 0, ntc0 = 0, cs0 = 0, cs1 = 0, fs0 = 0;
  uint16_t votg = 0;
  rd8(BQ25629_REG::CHARGER_CONTROL_0, cc0);
  rd8(BQ25629_REG::CHARGER_CONTROL_2, cc2);
  rd8(BQ25629_REG::CHARGER_CONTROL_3, cc3);
  rd8(BQ25629_REG::NTC_CONTROL_0, ntc0);
  rd8(BQ25629_REG::CHARGER_STATUS_0, cs0);
  rd8(BQ25629_REG::CHARGER_STATUS_1, cs1);
  rd8(BQ25629_REG::FAULT_STATUS_0, fs0);
  if (read_register_16(BQ25629_REG::VOTG_REGULATION, votg) != ESP_OK) {
    ++failed_reads;
  }

  // Latched flag registers: unlike the instantaneous *_STATUS regs above,
  // these record any event since the last read (and clear on read), so a
  // transient boost hiccup / PMID-UVP that STATUS misses is captured here.
  // This function is the sole reader/owner of the FLAG registers — adding
  // another consumer (e.g. an INT-pin fault handler) would silently race
  // with these clear-on-read accesses.
  uint8_t cflag0 = 0, cflag1 = 0, fflag0 = 0;
  rd8(BQ25629_REG::CHARGER_FLAG_0, cflag0);
  rd8(BQ25629_REG::CHARGER_FLAG_1, cflag1);
  rd8(BQ25629_REG::FAULT_FLAG_0, fflag0);

  BQ25629_ADC_Data adc{};
  read_adc(adc);

  const uint8_t en_otg = (cc2 >> 6) & 0x01;
  const uint8_t en_bypass = (cc2 >> 7) & 0x01;
  const uint8_t votg_code = (votg >> 6) & 0x7F; // REG0x0C bits [12:6]
  const uint16_t votg_mv = (votg_code >= 0x30) ? ((votg_code - 0x30) * 80 + 3840) : 0;
  const uint8_t vbat_otg_min = (cc3 >> 4) & 0x01;
  const uint8_t vbat_uvlo = (cc3 >> 5) & 0x01;
  const uint8_t ibat_pk = (cc3 >> 6) & 0x03;
  const uint8_t en_hiz = (cc0 >> 4) & 0x01;
  const uint8_t force_pmid_dis = (cc0 >> 3) & 0x01;
  const uint8_t wd = cc0 & 0x03;
  const uint8_t ts_ignore = (ntc0 >> 7) & 0x01;
  const uint8_t vbus_stat = cs1 & 0x07;

  static const char *const WD_STR[] = {"disabled", "50s", "100s", "200s"};
  static const char *const IBATPK_STR[] = {"reserved", "reserved", "6A", "12A"};
  const char *vbus_str;
  switch (vbus_stat) {
  case 0: vbus_str = "No input"; break;
  case 1: vbus_str = "USB SDP"; break;
  case 2: vbus_str = "USB CDP"; break;
  case 3: vbus_str = "USB DCP"; break;
  case 4: vbus_str = "Unknown adapter"; break;
  case 5: vbus_str = "Non-standard"; break;
  case 7: vbus_str = "OTG (boost active)"; break;
  default: vbus_str = "?"; break;
  }

  ESP_LOGI(TAG, "+=========== OTG registers [%s] ===========", context ? context : "");
  if (failed_reads > 0) {
    ESP_LOGW(TAG, "| !! %d register reads FAILED — zeroed fields below are unreliable",
             failed_reads);
  }
  ESP_LOGI(TAG, "| REG  FIELD          BITS   VAL   DECODED");
  ESP_LOGI(TAG, "| 0x18 EN_OTG         [6]    %u     %s", en_otg,
           en_otg ? "boost ENABLED" : "boost disabled");
  ESP_LOGI(TAG, "| 0x18 EN_BYPASS_OTG  [7]    %u     %s", en_bypass,
           en_bypass ? "bypass ON" : "bypass off");
  ESP_LOGI(TAG, "| 0x0C VOTG           [12:6] 0x%02X  %u mV (raw 0x%04X)", votg_code, votg_mv, votg);
  ESP_LOGI(TAG, "| 0x19 VBAT_OTG_MIN   [4]    %u     %s", vbat_otg_min,
           vbat_otg_min ? "start>2.6V / stop<2.4V" : "start>3.0V / stop<2.8V (default)");
  ESP_LOGI(TAG, "| 0x19 VBAT_UVLO      [5]    %u     %s", vbat_uvlo,
           vbat_uvlo ? "UVLO 1.8V / SHORT 1.85V" : "UVLO 2.2V / SHORT 2.05V (default)");
  ESP_LOGI(TAG, "| 0x19 IBAT_PK        [7:6]  %u     %s", ibat_pk, IBATPK_STR[ibat_pk]);
  ESP_LOGI(TAG, "| 0x16 EN_HIZ         [4]    %u     %s", en_hiz,
           en_hiz ? "HIZ ON (blocks boost!)" : "HIZ off");
  ESP_LOGI(TAG, "| 0x16 WATCHDOG       [1:0]  %u     %s", wd, WD_STR[wd]);
  ESP_LOGI(TAG, "| 0x16 FORCE_PMID_DIS [3]    %u     %s", force_pmid_dis,
           force_pmid_dis ? "PMID 30mA sink ON (fights boost!)" : "off");
  ESP_LOGI(TAG, "| 0x1A TS_IGNORE      [7]    %u     %s", ts_ignore,
           ts_ignore ? "TS ignored" : "TS gates OTG");
  ESP_LOGI(TAG, "| 0x1E VBUS_STAT      [2:0]  %u     %s", vbus_stat, vbus_str);
  ESP_LOGI(TAG,
           "| 0x1D STATUS 0x%02X:  VOTG_reg=%u IOTG_reg=%u VSYS=%u TREG=%u SAFE_TMR=%u WD_EXP=%u",
           cs0, (cs0 >> 2) & 1, (cs0 >> 3) & 1, (cs0 >> 4) & 1, (cs0 >> 5) & 1, (cs0 >> 1) & 1,
           cs0 & 1);
  ESP_LOGI(TAG, "| 0x1F FAULT  0x%02X:  VBUS=%u BAT=%u SYS=%u OTG=%u TSHUT=%u TS_zone=%u", fs0,
           (fs0 >> 7) & 1, (fs0 >> 6) & 1, (fs0 >> 5) & 1, (fs0 >> 4) & 1, (fs0 >> 3) & 1,
           fs0 & 0x07);
  ESP_LOGI(TAG,
           "| 0x22 FAULT_FLAG (latched) 0x%02X: VBUS=%u BAT=%u SYS=%u OTG=%u TSHUT=%u", fflag0,
           (fflag0 >> 7) & 1, (fflag0 >> 6) & 1, (fflag0 >> 5) & 1, (fflag0 >> 4) & 1,
           (fflag0 >> 3) & 1);
  ESP_LOGI(TAG,
           "| 0x20/0x21 FLAG (latched) 0x%02X / 0x%02X  (nonzero => event fired since last read)",
           cflag0, cflag1);
  ESP_LOGI(TAG, "| ADC: VPMID=%u VBAT=%u VSYS=%u VBUS=%u mV  IBUS=%d IBAT=%d mA", adc.vpmid_mv,
           adc.vbat_mv, adc.vsys_mv, adc.vbus_mv, adc.ibus_ma, adc.ibat_ma);
  ESP_LOGI(TAG, "| OK boost => EN_OTG=1, HIZ=0, VBUS_STAT=OTG, VPMID~5000, no BAT/SYS/OTG fault");
  ESP_LOGI(TAG, "+================================================");
  return failed_reads == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t BQ25629::read_register(uint8_t reg_addr, uint8_t &value) {
  if (dev_handle_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t ret = i2c_master_transmit_receive(dev_handle_, &reg_addr, 1, &value, 1, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read register 0x%02X: %s", reg_addr, esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t BQ25629::write_register(uint8_t reg_addr, uint8_t value) {
  if (dev_handle_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  uint8_t write_buf[2] = {reg_addr, value};
  esp_err_t ret = i2c_master_transmit(dev_handle_, write_buf, 2, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write register 0x%02X: %s", reg_addr, esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t BQ25629::read_register_16(uint8_t reg_addr, uint16_t &value) {
  if (dev_handle_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  // BQ25629 uses little-endian format
  uint8_t data[2];
  esp_err_t ret = i2c_master_transmit_receive(dev_handle_, &reg_addr, 1, data, 2, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read 16-bit register 0x%02X: %s", reg_addr, esp_err_to_name(ret));
    return ret;
  }

  value = data[0] | (data[1] << 8); // Little-endian
  return ESP_OK;
}

esp_err_t BQ25629::write_register_16(uint8_t reg_addr, uint16_t value) {
  if (dev_handle_ == nullptr) {
    return ESP_ERR_INVALID_STATE;
  }

  // BQ25629 uses little-endian format
  uint8_t write_buf[3];
  write_buf[0] = reg_addr;
  write_buf[1] = value & 0xFF;        // LSB first
  write_buf[2] = (value >> 8) & 0xFF; // MSB second

  esp_err_t ret = i2c_master_transmit(dev_handle_, write_buf, 3, I2C_TIMEOUT_MS);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write 16-bit register 0x%02X: %s", reg_addr, esp_err_to_name(ret));
  }
  return ret;
}

esp_err_t BQ25629::modify_register(uint8_t reg_addr, uint8_t mask, uint8_t value) {
  uint8_t reg_value;
  esp_err_t ret = read_register(reg_addr, reg_value);
  if (ret != ESP_OK) {
    return ret;
  }

  reg_value = (reg_value & ~mask) | (value & mask);
  return write_register(reg_addr, reg_value);
}

esp_err_t BQ25629::enable_pmid_discharge(bool enable) {
  ESP_LOGI(TAG, "%s PMID discharge (~30mA)", enable ? "Enabling" : "Disabling");

  // Set FORCE_PMID_DIS bit in REG0x16
  // This forces ~30mA discharge current from PMID
  // Useful for discharging PMID capacitor before mode transitions
  esp_err_t ret = modify_register(BQ25629_REG::CHARGER_CONTROL_0, BIT_MASK::FORCE_PMID_DIS,
                                  enable ? BIT_MASK::FORCE_PMID_DIS : 0);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "PMID discharge %s", enable ? "enabled" : "disabled");
  } else {
    ESP_LOGE(TAG, "Failed to configure PMID discharge: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t BQ25629::set_votg_voltage(uint16_t voltage_mv) {
  // Clamp to valid range: 3840-5200mV
  if (voltage_mv < 3840)
    voltage_mv = 3840;
  if (voltage_mv > 5200)
    voltage_mv = 5200;

  ESP_LOGI(TAG, "Setting VOTG voltage to %u mV", voltage_mv);

  // Calculate register value: 80mV steps starting at 3840mV
  // REG0x0C bits [12:6] - offset 0x30 base
  // VOTG[6:0] = (voltage_mv - 3840) / 80 + 0x30
  uint8_t votg_code = ((voltage_mv - 3840) / 80) + 0x30;
  // Shift to bits [12:6] in 16-bit register
  uint16_t reg_value = ((uint16_t)votg_code) << 6;

  ESP_LOGI(TAG, "VOTG code: 0x%02X, reg_value: 0x%04X", votg_code, reg_value);

  esp_err_t ret = write_register_16(BQ25629_REG::VOTG_REGULATION, reg_value);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "VOTG voltage set to %u mV", voltage_mv);
  } else {
    ESP_LOGE(TAG, "Failed to set VOTG voltage: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t BQ25629::disable_hiz_mode() {
  ESP_LOGI(TAG, "Disabling HIZ mode (required for OTG boost)");

  // Clear EN_HIZ bit in REG0x16
  // HIZ mode must be disabled for boost converter to operate
  esp_err_t ret = modify_register(BQ25629_REG::CHARGER_CONTROL_0, BIT_MASK::EN_HIZ,
                                  0); // Clear HIZ bit

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "HIZ mode disabled");
  } else {
    ESP_LOGE(TAG, "Failed to disable HIZ mode: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t BQ25629::set_ts_ignore(bool ignore) {
  ESP_LOGI(TAG, "%s temperature sensor (TS) check", ignore ? "Ignoring" : "Enabling");

  // Set TS_IGNORE bit in REG0x1A (NTC_CONTROL_0)
  // If no thermistor is connected, set TS_IGNORE=1 to allow OTG operation
  esp_err_t ret = modify_register(BQ25629_REG::NTC_CONTROL_0, BIT_MASK::TS_IGNORE,
                                  ignore ? BIT_MASK::TS_IGNORE : 0);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "TS check %s", ignore ? "ignored" : "enabled");
  } else {
    ESP_LOGE(TAG, "Failed to configure TS_IGNORE: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t BQ25629::enable_pmid_5v_boost() {
  ESP_LOGI(TAG, "Enabling PMID 5V boost output...");
  esp_err_t ret;

  // Note: Watchdog timer remains enabled (default 50s)
  // Controller must call reset_watchdog() periodically to prevent OTG disable
  // If watchdog expires, EN_OTG will be reset to 0 automatically

  // Step 1: Disable HIZ mode (required for boost operation)
  ret = disable_hiz_mode();
  if (ret != ESP_OK)
    return ret;
  vTaskDelay(pdMS_TO_TICKS(10));

  // Step 2: Enable TS check (do not ignore for safety)
  ret = set_ts_ignore(false);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to enable TS check, continuing...");
  }
  vTaskDelay(pdMS_TO_TICKS(10));

  // Step 3: Set VOTG to 5.0V
  ret = set_votg_voltage(5100);
  if (ret != ESP_OK)
    return ret;
  vTaskDelay(pdMS_TO_TICKS(10));

  // Step 4: Disable bypass OTG (we want boost mode, not bypass)
  ret = enable_bypass_otg(false);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "Failed to disable bypass OTG, continuing...");
  }
  vTaskDelay(pdMS_TO_TICKS(10));

  // Step 5: Enable OTG boost mode
  ret = enable_otg(true);
  if (ret != ESP_OK)
    return ret;

  // Step 6: Wait for boost to stabilize (30ms per datasheet)
  ESP_LOGI(TAG, "Waiting for boost mode startup (300ms)...");
  vTaskDelay(pdMS_TO_TICKS(300));

  // Step 7: Verify OTG mode is active by checking VBUS_STAT
  VBusStatus vbus_status;
  ret = get_vbus_status(vbus_status);
  if (ret == ESP_OK) {
    if (vbus_status == VBusStatus::OTG_MODE) {
      ESP_LOGI(TAG, "PMID 5V boost enabled successfully! (VBUS_STAT=OTG_MODE)");
    } else {
      ESP_LOGW(TAG, "OTG mode not confirmed (VBUS_STAT=%d)", (int)vbus_status);
    }
  }

  return ESP_OK;
}

esp_err_t BQ25629::enable_bypass_otg(bool enable) {
  ESP_LOGI(TAG, "%s bypass OTG mode (direct battery→PMID)", enable ? "Enabling" : "Disabling");

  // Set EN_BYPASS_OTG bit in REG0x18
  // This enables direct path from battery to PMID (highest efficiency)
  // No buck/boost conversion
  esp_err_t ret = modify_register(BQ25629_REG::CHARGER_CONTROL_2, BIT_MASK::EN_BYPASS_OTG,
                                  enable ? BIT_MASK::EN_BYPASS_OTG : 0);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Bypass OTG mode %s", enable ? "enabled" : "disabled");
  } else {
    ESP_LOGE(TAG, "Failed to configure bypass OTG: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t BQ25629::enter_ship_mode() {
  ESP_LOGI(TAG, "Entering ship mode (battery leakage: 0.15 μA)");

  // Set BATFET_CTRL = 10 (ship mode) in REG0x18
  // Ship mode: BATFET disconnects, can wake via QON button (17ms press) or
  // adapter
  esp_err_t ret = modify_register(BQ25629_REG::CHARGER_CONTROL_2, BIT_MASK::BATFET_CTRL_MASK,
                                  BIT_MASK::BATFET_CTRL_SHIP);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Ship mode command sent. System will power off after "
                  "tBATFET_DLY delay");
  } else {
    ESP_LOGE(TAG, "Failed to enter ship mode: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t BQ25629::enter_shutdown_mode() {
  ESP_LOGI(TAG, "Entering shutdown mode (battery leakage: 0.1 μA)");

  // Set BATFET_CTRL = 01 (shutdown mode) in REG0x18
  // Shutdown mode: BATFET disconnects, can only wake via adapter (QON disabled)
  // Note: Requires VBUS < VVBUS_UVLO (no adapter connected)
  esp_err_t ret = modify_register(BQ25629_REG::CHARGER_CONTROL_2, BIT_MASK::BATFET_CTRL_MASK,
                                  BIT_MASK::BATFET_CTRL_SHUTDOWN);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Shutdown mode command sent. System will power off after "
                  "tBATFET_DLY delay");
  } else {
    ESP_LOGE(TAG, "Failed to enter shutdown mode: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t BQ25629::system_power_reset() {
  ESP_LOGI(TAG, "Initiating system power reset");

  // Set BATFET_CTRL = 11 (system reset) in REG0x18
  // System reset: BATFET cycles off then on, restarting the system
  esp_err_t ret = modify_register(BQ25629_REG::CHARGER_CONTROL_2, BIT_MASK::BATFET_CTRL_MASK,
                                  BIT_MASK::BATFET_CTRL_RESET);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "System reset command sent. System will restart after ~10ms");
  } else {
    ESP_LOGE(TAG, "Failed to initiate system reset: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t BQ25629::configure_jeita_profile() {
  ESP_LOGI(TAG, "Configuring JEITA temperature profile");
  esp_err_t ret;

  // REG0x1A (NTC_Control_0): Set COOL/WARM charge current to 20%
  // Bits [7:6] TS_ISET_WARM = 01 (20%)
  // Bits [5:4] TS_ISET_COOL = 01 (20%)
  // Value: 0x25 = 0b00100101
  ret = write_register(BQ25629_REG::NTC_CONTROL_0, 0x25);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write NTC_CONTROL_0: %s", esp_err_to_name(ret));
    return ret;
  }
  ESP_LOGD(TAG, "NTC_CONTROL_0 = 0x25 (WARM/COOL = 20%%)");

  // REG0x1B (NTC_Control_1): Set temperature thresholds
  // TS_TH6 [7:6] = 00 (60°C hot threshold)
  // TS_TH5 [5:4] = 01 (45°C warm threshold)
  // TS_TH2 [3:2] = 01 (10°C cool threshold)
  // TS_TH1 [1:0] = 11 (0°C cold threshold)
  // Value: 0x27 = 0b00100111
  ret = write_register(BQ25629_REG::NTC_CONTROL_1, 0x27);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write NTC_CONTROL_1: %s", esp_err_to_name(ret));
    return ret;
  }
  ESP_LOGD(TAG, "NTC_CONTROL_1 = 0x27 (TH1=0°C, TH2=10°C, TH5=45°C, TH6=60°C)");

  // REG0x1C (NTC_Control_2): Keep default voltage settings
  // VRECHG_TH_PREWARM/PRECOOL unchanged
  // Value: 0x3F (default)
  ret = write_register(BQ25629_REG::NTC_CONTROL_2, 0x3F);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to write NTC_CONTROL_2: %s", esp_err_to_name(ret));
    return ret;
  }
  ESP_LOGD(TAG, "NTC_CONTROL_2 = 0x3F (default voltage settings)");

  ESP_LOGI(TAG, "JEITA profile OK (0/10/45/60\u00b0C, COOL/WARM=20%%)");

  return ESP_OK;
}

esp_err_t BQ25629::read_ntc_temperature(BQ25629_NTC_Data &data) {
  esp_err_t ret;

  // Read TS ADC value (12-bit in low bits, 0.0961%/LSB)
  uint16_t ts_adc_raw = 0;
  ret = read_register_16(BQ25629_REG::TS_ADC, ts_adc_raw);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read TS_ADC: %s", esp_err_to_name(ret));
    return ret;
  }
  ts_adc_raw &= 0x0FFF; // Mask to 12-bit value (consistent with read_adc)

  // Convert ADC to percentage (0.0961% per LSB)
  data.ts_percent = ts_adc_raw * 0.0961f;

  // Calculate NTC resistance using voltage divider equation
  // V_TS = V_BIAS * (RT2 || R_NTC) / (RT1 + (RT2 || R_NTC))
  // Where RT1 = 5.23kΩ (pull-up), RT2 = 30.1kΩ (pull-down)
  //
  // ADC reads: TS% = V_TS / V_BIAS * 100
  // Let ratio = TS% / 100 = V_TS / V_BIAS
  //
  // Solving for R_NTC:
  // Step 1: Find parallel resistance R_parallel = R_NTC || RT2
  //   ratio = R_parallel / (RT1 + R_parallel)
  //   R_parallel = (ratio * RT1) / (1 - ratio)
  // Step 2: Solve for R_NTC from parallel equation
  //   R_parallel = (R_NTC * RT2) / (R_NTC + RT2)
  //   R_NTC = (R_parallel * RT2) / (RT2 - R_parallel)

  const float RT1 = 5230.0f;  // 5.23kΩ
  const float RT2 = 30100.0f; // 30.1kΩ
  const float R25 = 10000.0f; // 10kΩ @ 25°C
  const float B = 3950.0f;    // B constant (3950K)

  float adc_ratio = data.ts_percent / 100.0f;

  // Handle edge cases
  if (adc_ratio <= 0.001f || adc_ratio >= 0.999f) {
    data.resistance_ohm = 0.0f;
    data.temperature_c = -999.0f;
    data.zone = TempZone::TS_UNKNOWN;
    ESP_LOGW(TAG, "TS ADC out of range: %.2f%%", data.ts_percent);
    return ESP_OK;
  }

  // Calculate parallel resistance first
  float r_parallel = (adc_ratio * RT1) / (1.0f - adc_ratio);

  // Check if parallel resistance is valid
  if (r_parallel >= RT2 || r_parallel < 0.0f) {
    data.resistance_ohm = 0.0f;
    data.temperature_c = -999.0f;
    data.zone = TempZone::TS_UNKNOWN;
    ESP_LOGW(TAG, "Invalid parallel resistance: %.1fΩ (TS=%.2f%%)", r_parallel, data.ts_percent);
    return ESP_OK;
  }

  // Calculate R_NTC from parallel resistance
  data.resistance_ohm = (r_parallel * RT2) / (RT2 - r_parallel);

  // Steinhart-Hart equation: 1/T = 1/T0 + (1/B) * ln(R/R0)
  // Where T0 = 298.15K (25°C), R0 = R25 = 10kΩ
  float t_kelvin = 1.0f / ((1.0f / 298.15f) + (logf(data.resistance_ohm / R25) / B));
  data.temperature_c = t_kelvin - 273.15f;

  // Determine temperature zone
  if (data.temperature_c < 0.0f) {
    data.zone = TempZone::TS_COLD;
  } else if (data.temperature_c < 10.0f) {
    data.zone = TempZone::TS_COOL;
  } else if (data.temperature_c < 45.0f) {
    data.zone = TempZone::TS_NORMAL;
  } else if (data.temperature_c < 60.0f) {
    data.zone = TempZone::TS_WARM;
  } else {
    data.zone = TempZone::TS_HOT;
  }

  ESP_LOGD(TAG, "NTC: %.2f°C (%.1fΩ, TS=%.1f%%), Zone=%d", data.temperature_c, data.resistance_ohm,
           data.ts_percent, static_cast<int>(data.zone));

  return ESP_OK;
}

esp_err_t BQ25629::get_temperature_zone(TempZone &zone) {
  esp_err_t ret;

  // Read FAULT_STATUS_0 (0x1F) to get TS_STAT[2:0]
  uint8_t fault_status = 0;
  ret = read_register(BQ25629_REG::FAULT_STATUS_0, fault_status);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to read FAULT_STATUS_0: %s", esp_err_to_name(ret));
    return ret;
  }

  // Extract TS_STAT[2:0] bits
  uint8_t ts_stat = fault_status & 0x07;

  // Map TS_STAT to TempZone
  // TS_STAT encoding (from datasheet):
  // 000 = Normal
  // 001 = Warm (VLTF < VTS < VHTF)
  // 010 = Cool (VHTF < VTS < VLTF)
  // 011 = Cold (VTS > VLTF)
  // 100 = Hot (VTS < VHTF)
  // 101-111 = Reserved
  switch (ts_stat) {
  case 0x00:
    zone = TempZone::TS_NORMAL;
    break;
  case 0x01:
    zone = TempZone::TS_WARM;
    break;
  case 0x02:
    zone = TempZone::TS_COOL;
    break;
  case 0x03:
    zone = TempZone::TS_COLD;
    break;
  case 0x04:
    zone = TempZone::TS_HOT;
    break;
  default:
    zone = TempZone::TS_UNKNOWN;
    ESP_LOGW(TAG, "Unknown TS_STAT value: 0x%02X", ts_stat);
    break;
  }

  return ESP_OK;
}

} // namespace drivers
