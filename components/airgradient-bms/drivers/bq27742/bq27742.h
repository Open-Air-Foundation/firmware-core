/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BQ27742_H
#define BQ27742_H

#include <cstdint>

#include "driver/i2c_master.h"

#include "hal/fuel_gauge_device.h"
#include "types/bms_types.h"

/// TI bq27742-G1 single-cell Impedance Track fuel gauge with integrated
/// hardware protection (GO v2.0 board).
///
/// Same I2C address as the BQ27427 it replaces (0x55) but a different
/// command map, no CFGUPDATE mode, and data flash that is only reachable
/// when UNSEALED.  Standard commands are little-endian; data flash blocks
/// are MSB-first.
///
/// Flags() is normalised to the FgFlags layout PowerService already reads
/// (DSG/CHG/FC/BAT_DET); read_flags_raw() returns the chip's own bits.
///
/// References:
///   - Datasheet SLUSBV9D (Rev. D, Jan 2018)
///   - TRM SLUUAX0C (Jul 2015)
class BQ27742 : public FuelGaugeDevice {
public:
  static constexpr uint8_t DEFAULT_ADDRESS = 0x55;
  static constexpr uint16_t DEVICE_TYPE_BQ27742 = 0x0742;
  static constexpr size_t RA_TABLE_SIZE = FG_RA_TABLE_SIZE;

  struct Config {
    uint8_t address = DEFAULT_ADDRESS;
    /// 100 kHz, not the bus's 400 kHz: on the GO v2.0 bus the incremental
    /// 2-byte read of a Control() result is corrupted at 400 kHz (the gauge
    /// clock-stretches while serving it), which breaks unseal and every
    /// data-flash access.  Standard commands were fine at either speed.
    uint32_t scl_speed_hz = 100000;
    int timeout_ms = 100;
  };

  /// Protection status bits, ProtectorStatus() 0x6D (TRM Table 4-7).
  struct ProtectorStatus {
    static constexpr uint8_t CHG_OFF = (1u << 7);
    static constexpr uint8_t DSG_OFF = (1u << 6);
    static constexpr uint8_t CVM = (1u << 5);
    static constexpr uint8_t UVP = (1u << 4);
    static constexpr uint8_t OVP = (1u << 3);
    static constexpr uint8_t SCD = (1u << 2);
    static constexpr uint8_t OCD = (1u << 1);
    static constexpr uint8_t OCC = (1u << 0);
  };

  explicit BQ27742(i2c_master_bus_handle_t bus);
  BQ27742(i2c_master_bus_handle_t bus, const Config &config);
  ~BQ27742() override;

  BQ27742(const BQ27742 &) = delete;
  BQ27742 &operator=(const BQ27742 &) = delete;

  /// Probe, attach, verify Control(DEVICE_TYPE) == 0x0742.  Idempotent.
  bool init();

  // -- FuelGaugeDevice (runtime poll surface) --------------------------------
  bool ready() const override;
  bool read_soc_percent(uint8_t &out) override;
  bool read_voltage_mv(uint16_t &out) override;
  bool read_average_current_ma(int16_t &out) override;
  bool read_average_power_mw(int16_t &out) override; ///< derived: V * I (no AveragePower command)
  bool read_remaining_capacity_mah(uint16_t &out) override;
  bool read_full_charge_capacity_mah(uint16_t &out) override;
  bool read_internal_temperature_c(float &out) override;
  bool read_flags(uint16_t &out) override; ///< normalised to FgFlags bit layout

  // -- Fuel-gauge learning reads/config (FuelGaugeDevice overrides) ----------
  bool read_control_status(uint16_t &out) override; ///< raw CONTROL_STATUS (bq27742 layout)
  bool read_qmax_cell0(uint16_t &out) override;
  bool read_ra_table(int16_t *out, size_t len) override;
  bool read_design_capacity_mah(uint16_t &out) override; ///< DesignCapacity() 0x3C, sealed-readable
  bool select_chemistry_4v2() override;                  ///< no-op: chemistry is a data-flash image
  bool set_update_status_learning(bool enable) override; ///< enable -> IT ENABLE; disable -> no-op

  // -- bq27742-specific --------------------------------------------------------
  bool control_subcommand(uint16_t subcmd, uint16_t &result);
  bool read_fw_version(uint16_t &out);
  bool read_flags_raw(uint16_t &out);
  bool read_temperature_c(float &out); ///< Temperature() 0x06: pack thermistor
  bool read_safety_status(uint16_t &out);
  bool read_protector_status(uint8_t &out);
  bool read_protector_state(uint8_t &out);
  bool read_update_status(uint8_t &out);

  /// Data-flash read of Design Capacity / Design Energy / Terminate Voltage /
  /// Sleep Current.  Unseals the gauge (idempotent; it stays unsealed).
  bool read_cell_config(FgCellConfig &out);

  /// Data-flash read of the firmware-layer protection thresholds (Safety
  /// subclass).  Unseals the gauge (idempotent; it stays unsealed).
  bool read_protection_config(FgProtectionConfig &out);

  /// Data-flash write of the same eight fields as one block operation.
  /// Rejects out-of-range values: the gauge accepts and keeps them silently.
  /// Leaves the U1 delay/time fields in the same block untouched.
  bool write_protection_config(const FgProtectionConfig &cfg);

  /// Data-flash write of the same four fields.  Does NOT reset the gauge:
  /// a RESET briefly opens both protection FETs, which would drop Pack+ and
  /// reboot the system when running on battery.  New values are picked up by
  /// Impedance Track at the next gauge power cycle or IT ENABLE.
  bool write_cell_config(const FgCellConfig &cfg);

  /// Data-flash read of Pack Configuration D, Prot OC/OV Config (subclass 64)
  /// and Prot Checksum (subclass 57).  Unseals the gauge.
  bool read_protector_config(FgProtectorConfig &out);

  /// Data-flash write of Pack Configuration D and Prot OV Config followed by
  /// the Prot Checksum that makes them valid, verified through
  /// PROTECTOR_CHKSUM and the FET state.  Prot OC Config is preserved.
  /// Between the two block commits the stored checksum is stale and the
  /// gauge may open both FETs for up to a second, so call this only while an
  /// adapter powers the system.
  bool write_protector_config(uint8_t pack_config_d, uint8_t prot_ov_config);

private:
  i2c_master_bus_handle_t _bus = nullptr;
  i2c_master_dev_handle_t _dev = nullptr;
  Config _config;

  /// Read-only probe logged when DEVICE_TYPE does not match, before the I2C
  /// handle is released — separates a flaky read from a part that is not a
  /// bq27742-G1.
  void _log_identity_diagnostics(uint16_t first_device_type);

  bool _read_word(uint8_t cmd, uint16_t &out);
  bool _write_word(uint8_t cmd, uint16_t value);
  bool _read_byte(uint8_t reg, uint8_t &out);
  bool _write_byte(uint8_t reg, uint8_t value);
  bool _read_block(uint8_t reg, uint8_t *buf, size_t len);
  bool _write_block(uint8_t reg, const uint8_t *buf, size_t len);
  bool _unseal();
  bool _read_df_block(uint8_t subclass, uint8_t block, uint8_t *out32);
  bool _write_df_block(uint8_t subclass, uint8_t block, const uint8_t *in32);
  bool _write_df_word(uint8_t subclass, uint8_t offset, uint16_t value);
};

#endif // BQ27742_H
