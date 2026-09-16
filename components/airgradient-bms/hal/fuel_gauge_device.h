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

#include "types/bms_types.h"

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

  // -- Fuel-gauge learning reads (used by PowerService verify/poll) --------
  //
  // Non-perturbing reads needed by the factory learning path. Kept on the
  // HAL (not just the concrete driver) so PowerService can aggregate them
  // through its FuelGaugeDevice pointer and host tests can mock them.

  /// Impedance-Track learning progress, in part-neutral terms.  Each driver
  /// reads whichever register its gauge keeps it in.
  virtual bool read_learning_progress(FgLearningProgress &out) = 0;

  /// Learned Qmax for cell 0, in mAh.  The raw data-flash units differ between
  /// parts, so each driver applies its own conversion.
  virtual bool read_qmax_mah(uint16_t &out) = 0;

  /// Ra impedance grid (Ra0 RAM subclass). @p len must be >= FG_RA_TABLE_SIZE.
  virtual bool read_ra_table(int16_t *out, size_t len) = 0;

  /// Configured Design Capacity (Data Memory read; non-perturbing).
  virtual bool read_design_capacity_mah(uint16_t &out) = 0;

  // -- Fuel-gauge learning config (perturbing — CFGUPDATE / chemistry) ------
  //
  // On the HAL so the factory learning path can drive them through the same
  // PowerService gauge pointer and host tests can mock them.

  /// Switch to the 4.2 V chemistry (CHEM_B -> Chem ID 0x1202). Idempotent.
  virtual bool select_chemistry_4v2() = 0;

  /// Set/clear the Update Status learning bits (Qmax + Ra free-move).
  virtual bool set_update_status_learning(bool enable) = 0;
};

#endif // FUEL_GAUGE_DEVICE_H
