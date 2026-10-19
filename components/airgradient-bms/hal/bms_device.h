/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BMS_DEVICE_H
#define BMS_DEVICE_H

#include "types/bms_types.h"

/// Abstract BMS (Battery Management System) device interface.
///
/// Replaces the old BatteryMgmtSensor HAL.  The abstraction is no longer
/// "a sensor" -- it models a power-management device with telemetry reads,
/// status queries, and optional hardware features such as ship mode.
///
/// Policy (watchdog cadence, sleep decisions, shutdown UX) lives above this
/// interface in product-level code.  The HAL only exposes hardware primitives.
class BmsDevice {
public:
  virtual ~BmsDevice() = default;

  /// Initialize the BMS hardware.
  /// @return true on success.
  virtual bool init() = 0;

  // -- Telemetry ------------------------------------------------------------

  /// Read voltage telemetry (battery voltage, charging voltage).
  /// @param out Populated on success; left at invalid sentinels on failure.
  /// @return true if the read succeeded.
  virtual bool read_telemetry(BmsTelemetry &out) = 0;

  /// Enable or disable telemetry ADC conversions. Does not change the power path.
  /// Returns false if unsupported or if the operation failed.
  virtual bool set_adc_enabled(bool) { return false; }

  /// Read charger status (charging state).
  /// @param out Populated on success; left at defaults on failure.
  /// @return true if the read succeeded.
  virtual bool read_status(BmsStatus &out) = 0;

  /// Lightweight charging-state-only query (single register read).
  /// Use for fast polling when full status is not needed.
  /// @param[out] state Populated on success.
  /// @return true if the read succeeded.
  virtual bool get_charging_state(BmsChargingState &state) = 0;

  /// Estimate battery state-of-charge as a percentage (0–100 %).
  /// @param output Populated on success; left untouched on failure.
  /// @return true if the read succeeded.
  virtual bool get_battery_percentage(float *output) = 0;

  // -- Watchdog -------------------------------------------------------------

  /// Reset the BMS hardware watchdog timer.
  /// Must be called periodically to prevent the watchdog from expiring.
  /// @return true if the reset succeeded (or no reset was needed yet).
  virtual bool update_watchdog() = 0;

  // -- Optional hardware features -------------------------------------------

  /// @return true if this device supports hardware ship mode.
  virtual bool feature_ship_available() const = 0;

  /// Request the device to enter ship mode (power off).
  /// This call is expected not to return on success.
  /// @return false if ship mode is not supported or if the request failed.
  virtual bool enter_ship_mode() = 0;

  /// Configure the PMID rail operating mode.
  ///
  /// Products can use this to switch between external-input pass-through and
  /// OTG boost depending on charger state.
  /// @return true if the request succeeded.
  virtual bool configure_pmid_mode(BmsPmidMode) { return false; }

  // -- Power-path control ---------------------------------------------------

  /// Enable or disable the PMID boost converter.
  ///   true  -> arms boost (writes EN_OTG=1).  PMID comes from boost when
  ///            VBUS is absent; chip masks internally when VBUS is present.
  ///   false -> disables boost (writes EN_OTG=0).  PMID stays alive via buck
  ///            while VBUS is present; collapses on unplug.
  /// Single I2C write, no settling delay, no policy.
  /// @return true on success.
  virtual bool set_pmid_enabled(bool enabled) = 0;

  /// Re-apply the full PMID configuration sequence (preamble + arm + verify).
  ///
  /// Used for recovery when an external observation (e.g. PM-sensor read
  /// failure on battery) suggests the chip may have autonomously cleared
  /// EN_OTG or otherwise drifted from the configured state.  Blocking call;
  /// implementations may include a short settle delay.
  /// @return true if PMID is armed and verified after the sequence.
  virtual bool resync_pmid() = 0;

  /// Enable or disable the battery charging current path.  When disabled the
  /// charger IC holds the cell at its current SOC but continues to power the
  /// system rail from VBUS.
  /// @return true on success.
  virtual bool set_charge_enable(bool enabled) = 0;

  /// Set the fast-charge current limit (CC mode), mA.  Driver clamps to
  /// chip-supported range and granularity.
  /// @return true on success.
  virtual bool set_charge_current_ma(uint16_t current_ma) = 0;

  /// Configure the chip-level watchdog timeout.
  ///   timeout_ms == 0 -> watchdog disabled (chip never auto-clears EN_OTG
  ///                      on lost host).
  ///   timeout_ms  > 0 -> driver selects the closest chip-supported timeout
  ///                      value at or above the requested period.  Caller
  ///                      must then invoke update_watchdog() periodically
  ///                      inside that window or the chip will auto-clear
  ///                      EN_OTG and re-enter a safe state.
  /// @return true on success.
  virtual bool set_watchdog_timeout_ms(uint32_t timeout_ms) = 0;
};

#endif // BMS_DEVICE_H
