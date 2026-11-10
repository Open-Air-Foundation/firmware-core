/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BQ27742_H
#define BQ27742_H

#include "driver/i2c_master.h"

#include "hal/fuel_gauge_device.h"
#include "types/bms_types.h"

// BQ27742-G1 telemetry and cell configuration. Chemistry, calibration, and
// protection provisioning remain factory responsibilities.
class BQ27742 : public FuelGaugeDevice {
public:
  static constexpr uint32_t DEFAULT_UNSEAL_KEY = 0x36720414;

  explicit BQ27742(i2c_master_bus_handle_t bus) : _bus(bus) {}
  ~BQ27742() override;
  bool init();
  bool ready() const override { return _ready; }
  bool read_soc_percent(uint8_t &out) override;
  bool read_voltage_mv(uint16_t &out) override;
  bool read_average_current_ma(int16_t &out) override;
  bool read_average_power_mw(int16_t &out) override;
  bool read_remaining_capacity_mah(uint16_t &out) override;
  bool read_full_charge_capacity_mah(uint16_t &out) override;
  bool read_internal_temperature_c(float &out) override;
  bool read_flags(uint16_t &out) override;
  bool read_design_capacity_mah(uint16_t &out) override;

  /// Explicit access control; pass the pack's key if changed from TI's default.
  bool unseal(uint32_t key = DEFAULT_UNSEAL_KEY);
  bool seal();

  /// Call after init() and unseal(), before runtime polling starts.
  /// Leaves out unchanged on failure. Does not change the seal state.
  bool read_cell_config(FgCellConfig &out);

  /// Updates only changed flash blocks and verifies readback. Requires unseal().
  /// Preserves the energy scale; mWh must be exactly representable at that scale.
  /// A failure can leave earlier blocks updated; caller owns resealing.
  bool write_cell_config(const FgCellConfig &config);

  struct VoltageConfig {
    uint16_t design_voltage_mv;
    uint16_t charging_voltage_mv;
  };

  /// Nominal cell voltage and full-charge detection reference (not charger control).
  /// Requires unseal(); preserves unrelated flash and skips unchanged blocks.
  bool read_voltage_config(VoltageConfig &out);
  bool write_voltage_config(const VoltageConfig &config);

  // These APIs describe the BQ27427 learning workflow, not this gauge.
  bool read_control_status(uint16_t &) override { return false; }
  bool read_qmax_cell0(uint16_t &) override { return false; }
  bool read_ra_table(int16_t *, size_t) override { return false; }
  bool select_chemistry_4v2() override { return false; }
  bool set_update_status_learning(bool) override { return false; }

private:
  bool read_word(uint8_t reg, uint16_t &out);
  bool write_byte(uint8_t reg, uint8_t value);
  bool write_control_word(uint16_t value);
  bool read_access_status(uint16_t &out);
  bool data_flash_available();
  bool select_data_flash_block(uint8_t subclass, uint8_t block);
  bool read_data_flash_block(uint8_t subclass, uint8_t block, uint8_t *data);
  bool write_data_flash_block(uint8_t subclass, uint8_t block, const uint8_t *data);
  i2c_master_bus_handle_t _bus;
  i2c_master_dev_handle_t _dev = nullptr;
  bool _ready = false;
};

#endif // BQ27742_H
