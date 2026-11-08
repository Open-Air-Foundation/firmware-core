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

// Runtime-only BQ27742-G1 support. Provisioning and protection configuration
// belong to the pack's factory setup; never run BQ27427 learning commands here.
class BQ27742 : public FuelGaugeDevice {
public:
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

  // These APIs describe the BQ27427 learning workflow, not this gauge.
  bool read_control_status(uint16_t &) override { return false; }
  bool read_qmax_cell0(uint16_t &) override { return false; }
  bool read_ra_table(int16_t *, size_t) override { return false; }
  bool read_design_capacity_mah(uint16_t &) override { return false; }
  bool select_chemistry_4v2() override { return false; }
  bool set_update_status_learning(bool) override { return false; }

private:
  bool read_word(uint8_t reg, uint16_t &out);
  i2c_master_bus_handle_t _bus;
  i2c_master_dev_handle_t _dev = nullptr;
  bool _ready = false;
};

#endif // BQ27742_H
