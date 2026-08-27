/**
 * @file bq25629.h
 * @brief BQ25629 Battery Charger Driver
 *
 * Driver for TI BQ25628/BQ25629 battery charge management IC
 * Features:
 * - I2C-controlled 1-cell 2A battery charger
 * - NVDC power path management
 * - Input voltage/current regulation
 * - OTG boost mode support
 * - Integrated 12-bit ADC for monitoring
 * - USB BC1.2 detection
 *
 * Datasheet: bq25629.pdf (Texas Instruments)
 * I2C Address: 0x6A
 */

#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"
#include <cstdint>

namespace drivers {

/**
 * @brief BQ25629 Register Addresses
 */
namespace BQ25629_REG {
// Charging configuration registers
constexpr uint8_t CHARGE_CURRENT_LIMIT = 0x02; // 16-bit
constexpr uint8_t CHARGE_VOLTAGE_LIMIT = 0x04; // 16-bit
constexpr uint8_t INPUT_CURRENT_LIMIT = 0x06;  // 16-bit
constexpr uint8_t INPUT_VOLTAGE_LIMIT = 0x08;  // 16-bit
constexpr uint8_t VOTG_REGULATION = 0x0C;      // 16-bit
constexpr uint8_t MIN_SYSTEM_VOLTAGE = 0x0E;   // 16-bit
constexpr uint8_t PRECHARGE_CONTROL = 0x10;    // 16-bit
constexpr uint8_t TERMINATION_CONTROL = 0x12;  // 16-bit

// Control registers
constexpr uint8_t CHARGE_CONTROL = 0x14;
constexpr uint8_t CHARGE_TIMER_CONTROL = 0x15;
constexpr uint8_t CHARGER_CONTROL_0 = 0x16;
constexpr uint8_t CHARGER_CONTROL_1 = 0x17;
constexpr uint8_t CHARGER_CONTROL_2 = 0x18;
constexpr uint8_t CHARGER_CONTROL_3 = 0x19;

// NTC control registers
constexpr uint8_t NTC_CONTROL_0 = 0x1A;
constexpr uint8_t NTC_CONTROL_1 = 0x1B;
constexpr uint8_t NTC_CONTROL_2 = 0x1C;

// Status and fault registers
constexpr uint8_t CHARGER_STATUS_0 = 0x1D;
constexpr uint8_t CHARGER_STATUS_1 = 0x1E;
constexpr uint8_t FAULT_STATUS_0 = 0x1F;
constexpr uint8_t CHARGER_FLAG_0 = 0x20;
constexpr uint8_t CHARGER_FLAG_1 = 0x21;
constexpr uint8_t FAULT_FLAG_0 = 0x22;

// Mask registers
constexpr uint8_t CHARGER_MASK_0 = 0x23;
constexpr uint8_t CHARGER_MASK_1 = 0x24;
constexpr uint8_t FAULT_MASK_0 = 0x25;

// ADC registers
constexpr uint8_t ADC_CONTROL = 0x26;
constexpr uint8_t ADC_FUNCTION_DISABLE = 0x27;
constexpr uint8_t IBUS_ADC = 0x28;  // 16-bit
constexpr uint8_t IBAT_ADC = 0x2A;  // 16-bit
constexpr uint8_t VBUS_ADC = 0x2C;  // 16-bit
constexpr uint8_t VPMID_ADC = 0x2E; // 16-bit
constexpr uint8_t VBAT_ADC = 0x30;  // 16-bit
constexpr uint8_t VSYS_ADC = 0x32;  // 16-bit
constexpr uint8_t TS_ADC = 0x34;    // 16-bit
constexpr uint8_t TDIE_ADC = 0x36;  // 16-bit

// Part information
constexpr uint8_t PART_INFORMATION = 0x38;
} // namespace BQ25629_REG

/**
 * @brief BQ25629 Charge Status
 */
enum class ChargeStatus : uint8_t {
  NOT_CHARGING = 0x00,
  TRICKLE_PRECHARGE_FASTCHARGE = 0x01,
  TAPER_CHARGE = 0x02,
  TOPOFF_TIMER_ACTIVE = 0x03
};

/**
 * @brief BQ25629 VBUS Status
 */
enum class VBusStatus : uint8_t {
  NO_ADAPTER = 0x00,
  USB_SDP = 0x01, // 500mA
  USB_CDP = 0x02, // 1.5A
  USB_DCP = 0x03, // 1.5A
  UNKNOWN_ADAPTER = 0x04,
  NON_STANDARD = 0x05, // 1A/2.1A/2.4A
  OTG_MODE = 0x07
};

/**
 * @brief BQ25629 Watchdog Timer Timeout
 *
 * Note: Device supports 50s/100s/200s or disable. 5 minutes is not supported
 * by hardware.
 */
enum class WatchdogTimeout : uint8_t { Disable = 0x00, Sec50 = 0x01, Sec100 = 0x02, Sec200 = 0x03 };

/**
 * @brief BQ25629 Status Structure
 */
struct BQ25629_Status {
  ChargeStatus charge_status; // Charging status
  VBusStatus vbus_status;     // VBUS/adapter status
  bool adc_done_stat;         // ADC done (one-shot mode only)
  bool treg_stat;             // Thermal regulation active
  bool vsys_stat;             // VSYS in VSYSMIN regulation
  bool iindpm_stat;           // IINDPM/ILIM (forward) or IOTG (OTG) regulation
  bool vindpm_stat;           // VINDPM (forward) or VOTG (OTG) regulation
  bool safety_tmr_stat;       // Safety timer expired
  bool wd_stat;               // Watchdog timer expired
};

/**
 * @brief BQ25629 Fault Structure
 */
struct BQ25629_Fault {
  bool vbus_fault;  // VBUS fault (OVP or sleep comparator)
  bool bat_fault;   // Battery fault (IBAT OCP or VBAT OVP)
  bool sys_fault;   // VSYS fault (short or OVP)
  bool otg_fault;   // OTG fault (reverse current, UV, or OV)
  bool tshut_fault; // Thermal shutdown
  uint8_t ts_stat;  // TS temperature zone (TS_STAT[2:0])
};

/**
 * @brief BQ25629 Configuration Structure
 */
struct BQ25629_Config {
  uint16_t charge_voltage_mv;      // Battery regulation voltage (3500-4800mV)
  uint16_t charge_current_ma;      // Charge current limit (40-2000mA)
  uint16_t input_current_limit_ma; // Input current limit (100-3200mA)
  uint16_t input_voltage_limit_mv; // Input voltage limit (3800-16800mV)
  uint16_t min_system_voltage_mv;  // Minimum system voltage (2560-3840mV)
  uint16_t precharge_current_ma;   // Pre-charge current (10-310mA)
  uint16_t term_current_ma;        // Termination current (5-310mA)
  bool enable_charging;            // Enable battery charging
  bool enable_adc;                 // Enable ADC conversion
};

/**
 * @brief BQ25629 ADC Measurements
 */
struct BQ25629_ADC_Data {
  int16_t ibus_ma;   // Input current (negative = discharging)
  int16_t ibat_ma;   // Battery current (positive = charging, negative = discharging)
  uint16_t vbus_mv;  // Input voltage
  uint16_t vpmid_mv; // PMID voltage
  uint16_t vbat_mv;  // Battery voltage
  uint16_t vsys_mv;  // System voltage
  float ts_percent;  // TS pin voltage percentage
  int16_t tdie_c;    // Die temperature (°C)
};

/**
 * @brief NTC Temperature Zones (JEITA Profile)
 */
enum class TempZone : uint8_t {
  TS_COLD = 0,   // < 0°C - Charging suspended
  TS_COOL = 1,   // 0-10°C - 20% charge current (slow charge)
  TS_NORMAL = 2, // 10-45°C - 100% charge current (normal)
  TS_WARM = 3,   // 45-60°C - 20% charge current (slow charge)
  TS_HOT = 4,    // > 60°C - Charging suspended
  TS_UNKNOWN = 5 // Cannot determine temperature
};

/**
 * @brief NTC Temperature Data
 */
struct BQ25629_NTC_Data {
  float temperature_c;  // Temperature in °C
  float ts_percent;     // TS ADC reading (%)
  float resistance_ohm; // Calculated NTC resistance (Ω)
  TempZone zone;        // Temperature zone
};

/**
 * @brief BQ25629 Battery Charger Driver Class
 */
class BQ25629 {
public:
  /**
   * @brief Constructor
   * @param i2c_bus I2C bus handle
   * @param device_address I2C device address (default: 0x6A)
   */
  BQ25629(i2c_master_bus_handle_t i2c_bus, uint8_t device_address = 0x6A);

  /**
   * @brief Destructor
   */
  ~BQ25629();

  /**
   * @brief Initialize the BQ25629 device
   * @param config Configuration structure
   * @return ESP_OK on success
   */
  esp_err_t init(const BQ25629_Config &config);

  /**
   * @brief Deinitialize the device
   * @return ESP_OK on success
   */
  esp_err_t deinit();

  // CHARGER_CONTROL_0 (0x16) bit7 EN_AUTO_IBATDIS.
  // When enabled, the device can automatically enable IBAT discharge.
  esp_err_t enable_auto_ibat_discharge(bool enable);

  /**
   * @brief Enable battery charging
   * @param enable True to enable, false to disable
   * @return ESP_OK on success
   */
  esp_err_t enable_charging(bool enable);

  /**
   * @brief Enable OTG boost mode
   * @param enable True to enable, false to disable
   * @return ESP_OK on success
   */
  esp_err_t enable_otg(bool enable);

  /**
   * @brief Read the EN_OTG bit (CHARGER_CONTROL_2 bit 6)
   *
   * Reflects the register state, not whether the boost is actively running.
   * The chip masks EN_OTG internally when VBUS is present, and can clear it
   * autonomously on BAT_OTGZ, OTG hiccup, or TS faults
   * (datasheet §8.3.10.3-4).
   *
   * @param[out] enabled True if EN_OTG=1
   * @return ESP_OK on success
   */
  esp_err_t get_otg_enabled(bool &enabled);

  /**
   * @brief Set charge current limit
   * @param current_ma Charge current in mA (40-2000mA)
   * @return ESP_OK on success
   */
  esp_err_t set_charge_current(uint16_t current_ma);

  /**
   * @brief Set charge voltage limit
   * @param voltage_mv Battery voltage in mV (3500-4800mV)
   * @return ESP_OK on success
   */
  esp_err_t set_charge_voltage(uint16_t voltage_mv);

  /**
   * @brief Set input current limit
   * @param current_ma Input current in mA (100-3200mA)
   * @return ESP_OK on success
   */
  esp_err_t set_input_current_limit(uint16_t current_ma);

  /**
   * @brief Get charge status
   * @param status Output charge status
   * @return ESP_OK on success
   */
  esp_err_t get_charge_status(ChargeStatus &status);

  /**
   * @brief Get VBUS status
   * @param status Output VBUS status
   * @return ESP_OK on success
   */
  esp_err_t get_vbus_status(VBusStatus &status);

  /**
   * @brief Read ADC measurements
   * @param data Output ADC data structure
   * @return ESP_OK on success
   */
  esp_err_t read_adc(BQ25629_ADC_Data &data);

  /**
   * @brief Enable ADC conversion
   * @param enable True to enable, false to disable
   * @param continuous True for continuous mode, false for one-shot
   * @return ESP_OK on success
   */
  esp_err_t enable_adc(bool enable, bool continuous = true);

  /**
   * @brief Reset watchdog timer
   * @return ESP_OK on success
   */
  esp_err_t reset_watchdog();

  /**
   * @brief Set watchdog timeout (50s/100s/200s/disable)
   * @param timeout Watchdog timeout selection
   * @return ESP_OK on success
   */
  esp_err_t set_watchdog_timeout(WatchdogTimeout timeout);

  /**
   * @brief Enable PMID discharge (force ~30mA discharge current)
   * @param enable True to enable PMID discharge, false to disable
   * @return ESP_OK on success
   */
  esp_err_t enable_pmid_discharge(bool enable);

  /**
   * @brief Set OTG boost voltage at PMID output
   * @param voltage_mv Output voltage in mV (3840-5200mV, step 80mV)
   * @return ESP_OK on success
   */
  esp_err_t set_votg_voltage(uint16_t voltage_mv);

  /**
   * @brief Disable HIZ mode (required for OTG boost operation)
   * @return ESP_OK on success
   */
  esp_err_t disable_hiz_mode();

  /**
   * @brief Set TS_IGNORE to ignore thermistor check
   * @param ignore True to ignore TS, false to enable TS check
   * @return ESP_OK on success
   */
  esp_err_t set_ts_ignore(bool ignore);

  /**
   * @brief Enable PMID 5V boost output (complete setup sequence)
   *
   * This function performs all steps needed to enable PMID 5V boost:
   * 1. Disable HIZ mode
   * 2. Enable TS check (do not ignore)
   * 3. Set VOTG to 5V
   * 4. Disable bypass OTG
   * 5. Enable OTG boost mode
   * 6. Wait for stabilization
   *
   * @return ESP_OK on success
   */
  esp_err_t enable_pmid_5v_boost();

  /**
   * @brief Enable bypass OTG mode (direct battery to PMID)
   * @param enable True to enable bypass OTG, false to disable
   * @return ESP_OK on success
   */
  esp_err_t enable_bypass_otg(bool enable);

  /**
   * @brief Enter ship mode (ultra-low power, 0.15 μA)
   * @return ESP_OK on success
   */
  esp_err_t enter_ship_mode();

  /**
   * @brief Enter shutdown mode (ultra-low power, 0.1 μA)
   * @return ESP_OK on success
   */
  esp_err_t enter_shutdown_mode();

  /**
   * @brief System power reset (power cycle)
   * @return ESP_OK on success
   */
  esp_err_t system_power_reset();

  /**
   * @brief Read charger status
   * @param status Output: charger status structure
   * @return ESP_OK on success
   */
  esp_err_t read_status(BQ25629_Status &status);

  /**
   * @brief Read fault status
   * @param fault Output: fault status structure
   * @return ESP_OK on success
   */
  esp_err_t read_fault(BQ25629_Fault &fault);

  /**
   * @brief Check if battery is present
   * @param present Output: true if battery present
   * @return ESP_OK on success
   */
  esp_err_t is_battery_present(bool &present);

  /**
   * @brief Check if charging
   * @param charging Output: true if charging
   * @return ESP_OK on success
   */
  esp_err_t is_charging(bool &charging);

  /**
   * @brief Estimate battery state-of-charge (0-100%)
   *
   * This is a simple voltage-correlation gauge using VBAT (and IBAT for a basic
   * load compensation while discharging). It is not a coulomb counter.
   *
   * Accuracy is best when battery current is low or after the pack has rested.
   *
   * @param percent Output: estimated SOC in percent (0-100)
   * @return ESP_OK on success
   */
  esp_err_t estimate_battery_percent(uint8_t &percent);

  /**
   * @brief Configure battery temperature protection
   *
   * Charging is suspended below 0°C and above 45°C. The COOL zone from
   * 0°C to 10°C uses 20% charge current. OTG is suspended outside the
   * -10°C to 60°C range. Written values are read back and verified.
   *
   * @return ESP_OK on success
   */
  esp_err_t configure_jeita_profile();

  /**
   * @brief Read NTC temperature and zone
   *
   * Reads TS ADC value, converts to temperature using Steinhart-Hart equation,
   * and determines the current temperature zone.
   *
   * NTC: KNTC0805/10KF (R25=10kΩ, B=3950K)
   * Divider: RT1=4.12kΩ, RT2=17.33kΩ
   *
   * @param data Output: NTC temperature data
   * @return ESP_OK on success
   */
  esp_err_t read_ntc_temperature(BQ25629_NTC_Data &data);

  /**
   * @brief Get current temperature zone from hardware TS_STAT register
   *
   * Reads TS_STAT[2:0] from FAULT_STATUS_0 register to determine
   * the hardware-detected temperature zone.
   *
   * @param zone Output: Current temperature zone
   * @return ESP_OK on success
   */
  esp_err_t get_temperature_zone(TempZone &zone);

  /**
   * @brief Log charger limit registers (Charge Current, Voltage, Input Current,
   * Voltage)
   *
   * Reads and logs the following registers:
   * - 0x02: Charge Current Limit (mA)
   * - 0x04: Charge Voltage Limit (mV)
   * - 0x06: Input Current Limit (mA)
   * - 0x08: Input Voltage Limit (mV)
   *
   * @return ESP_OK on success
   */
  esp_err_t log_charger_limits();

  /**
   * @brief Read register (8-bit)
   * @param reg_addr Register address
   * @param value Output value
   * @return ESP_OK on success
   */
  esp_err_t read_register(uint8_t reg_addr, uint8_t &value);

  /**
   * @brief Write register (8-bit)
   * @param reg_addr Register address
   * @param value Value to write
   * @return ESP_OK on success
   */
  esp_err_t write_register(uint8_t reg_addr, uint8_t value);

  /**
   * @brief Read 16-bit register (little-endian)
   * @param reg_addr Register address (LSB)
   * @param value Output value
   * @return ESP_OK on success
   */
  esp_err_t read_register_16(uint8_t reg_addr, uint16_t &value);

  /**
   * @brief Write 16-bit register (little-endian)
   * @param reg_addr Register address (LSB)
   * @param value Value to write
   * @return ESP_OK on success
   */
  esp_err_t write_register_16(uint8_t reg_addr, uint16_t value);

private:
  i2c_master_bus_handle_t i2c_bus_;
  i2c_master_dev_handle_t dev_handle_;
  uint8_t device_address_;
  bool initialized_;

  // Helper functions
  esp_err_t modify_register(uint8_t reg_addr, uint8_t mask, uint8_t value);
};

} // namespace drivers
