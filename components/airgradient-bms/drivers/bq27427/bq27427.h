/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BQ27427_H
#define BQ27427_H

#include <cstdint>

#include "driver/i2c_master.h"

#include "hal/fuel_gauge_device.h"
#include "types/bms_types.h"

/// TI BQ27427 single-cell System-Side Impedance Track fuel gauge driver.
///
/// Implements FuelGaugeDevice for the runtime poll surface and exposes
/// additional Data Memory operations as non-virtual methods on the
/// concrete class.  Owns the bus protocol, the chip-specific access
/// constraints, and the device-type verification on init().
///
/// Code style follows the existing BQ25629Bms driver: constructor takes
/// the I2C bus handle and a Config, bool init() returns success, all
/// public methods return bool, file-local TAG, esp_log.h for logging,
/// RTOS::delay_ms for any delay.
///
/// References:
///   - Datasheet SLUSEB5B (Dec 2022, rev Sep 2025)
///   - TRM SLUUCD5 (Jan 2023)
class BQ27427 : public FuelGaugeDevice {
public:
  /// Fixed 7-bit I2C address.
  static constexpr uint8_t DEFAULT_ADDRESS = 0x55;

  /// Expected response from Control(DEVICE_TYPE) used to confirm part.
  static constexpr uint16_t DEVICE_TYPE_BQ27427 = 0x0427;

  /// Ra resistance-grid size (TRM Ra0 RAM subclass).
  static constexpr int RA_TABLE_SIZE = 15;

  struct Config {
    uint8_t address = DEFAULT_ADDRESS;
    uint32_t scl_speed_hz = 400000;
    int timeout_ms = 100;
  };

  explicit BQ27427(i2c_master_bus_handle_t bus);
  BQ27427(i2c_master_bus_handle_t bus, const Config &config);
  ~BQ27427() override;

  BQ27427(const BQ27427 &) = delete;
  BQ27427 &operator=(const BQ27427 &) = delete;

  /// Probe, attach to the bus, verify Control(DEVICE_TYPE) == 0x0427.
  /// Idempotent.  @return true on success.
  bool init();

  // -- FuelGaugeDevice (runtime poll surface) --------------------------------
  bool ready() const override;
  bool read_soc_percent(uint8_t &out) override;
  bool read_voltage_mv(uint16_t &out) override;
  bool read_average_current_ma(int16_t &out) override;
  bool read_average_power_mw(int16_t &out) override;
  bool read_remaining_capacity_mah(uint16_t &out) override;
  bool read_full_charge_capacity_mah(uint16_t &out) override;
  bool read_internal_temperature_c(float &out) override;
  bool read_flags(uint16_t &out) override;

  // -- FG-learning surface (overrides FuelGaugeDevice) -----------------------
  bool read_control_status(uint16_t &out) override;      // Control(0x0000)
  bool read_qmax_cell0(uint16_t &out) override;          // State subclass word 0
  bool read_ra_table(int16_t *out, size_t len) override; // Ra0 RAM subclass 0x59
  bool set_update_status_learning(bool enable) override; // Update Status bit0+bit1

  // -- Control() subcommand (non-virtual; concrete class only) ---------------
  bool control_subcommand(uint16_t subcmd, uint16_t &result);

  // -- Boot-only chemistry switch (concrete class; GoHardwareBoard) ----------

  /// Read Control(CHEM_ID = 0x0008).
  bool read_chem_id(uint16_t &out);

  /// Select CHEM_B (4.2 V profile, Chem ID 0x1202), idempotent.  Changing the
  /// Chem ID RESETS IT learning, so this must run before any learning run and
  /// is a no-op once already on 0x1202.
  bool select_chemistry_4v2();

  // -- Data Memory (non-virtual; concrete class only) ------------------------

  /// DM read; does not enter CFGUPDATE; does not perturb learned state.
  bool read_design_capacity_mah(uint16_t &out) override;

  /// DM read of the full 4-field FgCellConfig block.  Same property
  /// as read_design_capacity_mah — non-perturbing.
  bool read_cell_config(FgCellConfig &out);

  /// Atomic write of all four FgCellConfig fields in a single
  /// CFGUPDATE session.  ALWAYS enters CFGUPDATE — caller is
  /// responsible for only invoking this when the persisted values
  /// actually differ from the desired target.
  bool write_cell_config(const FgCellConfig &cfg);

  /// Issue Control(RESET = 0x0041).  Full device reset; reloads all
  /// RAM data memory from chip ROM defaults.  Used to recover from
  /// corrupted persistent state.
  bool reset_to_factory_defaults();

private:
  i2c_master_bus_handle_t _bus = nullptr;
  i2c_master_dev_handle_t _dev = nullptr;
  Config _config;

  bool _read_word(uint8_t cmd, uint16_t &out);
  bool _write_word(uint8_t cmd, uint16_t value);
  bool _read_byte(uint8_t reg, uint8_t &out);
  bool _write_byte(uint8_t reg, uint8_t value);
  bool _read_block(uint8_t reg, uint8_t *buf, size_t len);
  bool _write_block(uint8_t reg, const uint8_t *buf, size_t len);
  bool _select_data_block(uint8_t subclass, uint8_t block_offset);
  bool _wait_cfgupdate_flag(bool expected_set, uint32_t timeout_ms);
  bool _unseal();
};

#endif // BQ27427_H
