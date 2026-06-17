/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef FUEL_GAUGE_DEVICE_H
#define FUEL_GAUGE_DEVICE_H

#include <cstddef>
#include <cstdint>

/// Abstract fuel gauge device interface for runtime polling.
///
/// Parallel to BmsDevice.  Only the runtime-poll API needs polymorphism;
/// boot-time Data Memory access stays on the concrete BQ27427 class
/// (only GoHardwareBoard::init_bms calls those, no test surface needed).
///
/// PowerService stores a FuelGaugeDevice pointer.  Tests substitute a
/// mock subclass with scripted return values, the same way they
/// substitute BmsDevice.
class FuelGaugeDevice {
public:
  virtual ~FuelGaugeDevice() = default;

  /// True when the chip has been successfully attached and identified.
  virtual bool ready() const = 0;

  // -- Runtime poll surface (used by PowerService::poll_bms) ---------------

  virtual bool read_soc_percent(uint8_t &out) = 0;
  virtual bool read_voltage_mv(uint16_t &out) = 0;
  virtual bool read_average_current_ma(int16_t &out) = 0;
  virtual bool read_average_power_mw(int16_t &out) = 0;
  virtual bool read_remaining_capacity_mah(uint16_t &out) = 0;
  virtual bool read_full_charge_capacity_mah(uint16_t &out) = 0;
  virtual bool read_internal_temperature_c(float &out) = 0;
  virtual bool read_flags(uint16_t &out) = 0;

  // -- FG-learning surface --------------------------------------------------
  //
  // Used by PowerService for the learning verify read-back and the
  // learning-config write.  Default implementations return false so existing
  // FG drivers/mocks remain valid; BQ27427 overrides them.  Boot-only Data
  // Memory access (chemistry switch) stays on the concrete class.

  /// CONTROL_STATUS (Control 0x0000) — carries QMAX_UP / RES_UP / VOK.
  virtual bool read_control_status(uint16_t &out) {
    (void)out;
    return false;
  }

  /// Learned Qmax (State subclass cell-0 word), raw gauge units.
  virtual bool read_qmax_cell0(uint16_t &out) {
    (void)out;
    return false;
  }

  /// Design Capacity from Data Memory, mAh (non-perturbing read).
  virtual bool read_design_capacity_mah(uint16_t &out) {
    (void)out;
    return false;
  }

  /// Learned Ra resistance grid.  Writes up to @p len entries.
  virtual bool read_ra_table(int16_t *out, size_t len) {
    (void)out;
    (void)len;
    return false;
  }

  /// Set/clear the Update Status learning bits (Qmax + Ra) so learned values
  /// move freely during a run, then revert to bounded field limits.
  virtual bool set_update_status_learning(bool enable) {
    (void)enable;
    return false;
  }
};

#endif // FUEL_GAUGE_DEVICE_H
