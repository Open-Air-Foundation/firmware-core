/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BQ25629_BMS_H
#define BQ25629_BMS_H

#include <cstdint>

#include "bq25629.h"
#include "driver/i2c_master.h"

// TODO: Don't like the class name, should just straight BQ25629. ALso applied for filename. See bq25xx

#include "hal/bms_device.h"

/// BmsDevice adapter for the TI BQ25629 single-cell charger IC.
///
/// Wraps the vendor-level @c drivers::BQ25629 component and maps it to
/// the @c BmsDevice HAL interface used by the BMS stack.
///
/// The adapter owns the vendor driver instance internally.  Product code
/// supplies the I2C bus handle and a @c drivers::BQ25629_Config at
/// construction time; @c init() forwards the config to the vendor driver.
class BQ25629Bms : public BmsDevice {
public:
  /// Construct the adapter.
  /// @param i2c_bus  I2C master bus handle (shared, not owned).
  /// @param config   Charger configuration forwarded to the vendor driver.
  /// @param address  I2C device address (default 0x6A).
  explicit BQ25629Bms(i2c_master_bus_handle_t i2c_bus, const drivers::BQ25629_Config &config,
                      uint8_t address = 0x6A);

  ~BQ25629Bms() override = default;

  // -- BmsDevice interface --------------------------------------------------

  bool init() override;
  bool read_telemetry(BmsTelemetry &out) override;
  bool read_status(BmsStatus &out) override;
  bool get_charging_state(BmsChargingState &state) override;
  bool get_battery_percentage(float *output) override;
  bool update_watchdog() override;
  bool feature_ship_available() const override;
  bool enter_ship_mode() override;
  bool set_pmid_enabled(bool enabled) override;
  bool resync_pmid() override;
  bool hard_resync_pmid() override;
  bool power_cycle() override;
  bool set_charge_enable(bool enabled) override;
  bool set_charge_current_ma(uint16_t current_ma) override;
  bool set_watchdog_timeout_ms(uint32_t timeout_ms) override;
  bool dump_power_registers(const char *context) override;

private:
  /// Apply the full PMID configuration sequence:
  ///   HIZ off -> TS check on -> VOTG=5100 -> EN_BYPASS_OTG=0 -> EN_OTG=1
  ///   -> settle -> readback verify.
  /// Used by init() and resync_pmid().
  bool _apply_pmid_config();

  drivers::BQ25629 _charger;
  drivers::BQ25629_Config _config;
  bool _pmid_enabled = false;
};

#endif // BQ25629_BMS_H
