/**
 * AirGradient Go — Power Management Service
 *
 * Handles BMS status polling, battery monitoring, sleep cycle management,
 * RTC state persistence, and shutdown.  Called synchronously by the
 * orchestrator — no independent task.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "airgradient_gpio.h"
#include "go_settings.h"
#include "go_types.h"
#include "hal/bms_device.h"

#include <cstdint>

class FuelGaugeDevice;

// ---------------------------------------------------------------------------
// BatteryPercentSource
// ---------------------------------------------------------------------------

/// Identifies where the battery_percentage value in PowerSnapshot came from.
enum class BatteryPercentSource : uint8_t {
  Unknown,        ///< Not yet polled
  FuelGauge,      ///< Read from BQ27427 (V1 with FG attached, read OK)
  BatteryCharger, ///< Voltage-curve estimate from BQ25629 (fallback)
};

inline const char *bms_battery_percent_source_str(BatteryPercentSource s) {
  switch (s) {
  case BatteryPercentSource::Unknown:
    return "Unknown";
  case BatteryPercentSource::FuelGauge:
    return "FG";
  case BatteryPercentSource::BatteryCharger:
    return "BMS";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// ShipModeRequest
// ---------------------------------------------------------------------------

/// Requested ship-mode reason, set by PowerService::poll_bms() when a
/// safety trip fires.  The orchestrator shows a warning and calls shutdown().
enum class ShipModeRequest : uint8_t {
  None,
  OverDischarge,
  OverTemperature,
};

// ---------------------------------------------------------------------------
// PowerSnapshot
// ---------------------------------------------------------------------------

/// Aggregated BMS snapshot for the orchestrator and display.
/// All fields are initialized to invalid sentinels; call poll_bms() to fill.
struct PowerSnapshot {
  float battery_voltage = BmsInvalid::VOLT;
  float charging_voltage = BmsInvalid::VOLT;
  float battery_percentage = -1.0f;
  BmsChargingState charging_status = BmsChargingState::Unknown;
  bool critical = false; ///< true when battery_percentage < BATTERY_CRITICAL_PERCENT

  /// Full charger status (power source, regulation flags, fault flags).
  BmsStatus charger_status{};

  /// Full ADC telemetry (currents, voltages, temperatures).
  BmsTelemetry telemetry{};

  /// Where the battery_percentage value came from (FG or BMS fallback).
  BatteryPercentSource battery_percent_source = BatteryPercentSource::Unknown;

  // FG snapshot (populated only when an FG is attached and the read
  // succeeded).  Invalid sentinels otherwise.
  uint8_t fg_soc_percent = BmsInvalid::SOC_PERCENT;
  uint16_t fg_voltage_mv = BmsInvalid::VOLTAGE_MV;
  int16_t fg_current_ma = BmsInvalid::CURRENT_MA;
  int16_t fg_power_mw = BmsInvalid::POWER_MW;
  uint16_t fg_remaining_capacity_mah = BmsInvalid::CAPACITY_MAH;
  uint16_t fg_full_charge_capacity_mah = BmsInvalid::CAPACITY_MAH;
  float fg_internal_temperature_c = BmsInvalid::FG_TEMP_C;
  uint16_t fg_flags = 0;

  /// True when charging has been paused because the battery is full and
  /// external power is present.  Cleared when SOC drops below the resume
  /// threshold.
  bool full_charge_paused = false;

  /// Non-None when a safety trip requires the orchestrator to show a
  /// warning and enter ship mode.
  ShipModeRequest ship_mode_request = ShipModeRequest::None;

  // FG learning flags (V1 only).  Populated by poll_bms() from Flags() and
  // CONTROL_STATUS; consumed by FgLearningController.  Invalid/false otherwise.
  bool fg_flag_fc = false;   ///< Flags() FC      (full charge)
  bool fg_flag_chg = false;  ///< Flags() CHG     (fast-charge allowed)
  bool fg_flag_dsg = false;  ///< Flags() DSG     (discharging)
  bool fg_qmax_up = false;   ///< CONTROL_STATUS QMAX_UP
  bool fg_res_up = false;    ///< CONTROL_STATUS RES_UP
  bool fg_itpor = false;     ///< Flags() ITPOR   (POR wiped learning)
  bool fg_ocv_taken = false; ///< Flags() OCVTAKEN

  /// Plugged (external input present) vs on battery, at boot/poll.
  bool external_input_present = false;

  /// Derived mirror of ship_mode_request == OverDischarge, exposed so the
  /// pure FSM transitions Discharge → CycleDone without the enum dependency.
  bool edv_cutoff_reached = false;
};

// ---------------------------------------------------------------------------
// FG-learning verify read-back
// ---------------------------------------------------------------------------

/// Ra table size for the verify read-back.  Kept host-safe (not pulled from
/// bq27427.h).  A static_assert in GoHardwareBoard (target build) checks this
/// equals BQ27427::RA_TABLE_SIZE so the two cannot drift.
inline constexpr int FG_RA_TABLE_SIZE = 15;

/// Aggregated learned-value read-back, built by read_fg_learning_verify() and
/// converted to FgLearningController::VerifyInputs by the orchestrator.
struct FgLearningVerifyReadout {
  bool ok = false;
  bool itpor = false;
  bool qmax_up = false;
  uint16_t qmax_mah = 0;
  uint16_t design_capacity_mah = 0;
  int16_t ra[FG_RA_TABLE_SIZE] = {};
};

// ---------------------------------------------------------------------------
// Charging state helper
// ---------------------------------------------------------------------------

/// Return true when the BMS charging state indicates active charging.
/// Excludes NotCharging, Unknown (read error / uninitialised), and
/// ChargeTerminationDone (battery full, no longer drawing current).
inline bool is_bms_charging(BmsChargingState state) {
  return state != BmsChargingState::NotCharging && state != BmsChargingState::Unknown &&
         state != BmsChargingState::ChargeTerminationDone;
}

// ---------------------------------------------------------------------------
// PowerService
// ---------------------------------------------------------------------------

class PowerService {
public:
  // -------------------------------------------------------------------------
  // Configuration
  // -------------------------------------------------------------------------

  struct Config {
    int pin_wake_button_power;                 ///< GPIO for deep sleep wake (Button Power)
    int pin_wake_button_boot;                  ///< GPIO for deep sleep wake (Button Boot)
    int pin_ext_wdt = -1;                      ///< External watchdog GPIO (-1 = disabled)
    int deep_sleep_threshold_ms = 5000;        ///< Minimum interval (ms) to prefer deep sleep
    int pin_pm_power = -1;                     ///< PM sensor power GPIO (-1 = no hold)
    uint8_t pm_power_on_level = 1;             ///< GPIO level meaning "PM on"
                                               ///<   Prototype: 1 (active-high)
                                               ///<   v1:        0 (active-low)
    uint32_t sensor_hold_max_sleep_ms = 20000; ///< Max sleep (ms) to hold PM sensor powered
    uint32_t pm_sleep_threshold_ms = 20000;    ///< Min measure interval (ms) to power-cycle PM
  };

  // -------------------------------------------------------------------------
  // Sleep type
  // -------------------------------------------------------------------------

  /// Sleep type selected by decide_sleep().
  enum class SleepType {
    None, ///< Do not sleep (device is unlocked, not Offline, or interval too short)
    Deep, ///< Deep sleep — CPU reboots on wake; does not return from enter_sleep()
  };

  /// Combined result of sleep decision: what type and for how long.
  struct SleepDecision {
    SleepType type;       ///< None or Deep
    uint32_t duration_ms; ///< How long to sleep (0 when type == None)
  };

  // -------------------------------------------------------------------------
  // Construction
  // -------------------------------------------------------------------------

  /// @param bms     BMS device (via BmsDevice HAL).  Must outlive this service.
  /// @param gpio    GPIO HAL function-pointer table.
  /// @param config  Runtime configuration (wake pins, sleep threshold).
  PowerService(BmsDevice &bms, const gpio::Hal &gpio, const Config &config);

  /// Attach an already-initialised fuel gauge for runtime use.
  /// Non-owning: the fuel gauge must outlive PowerService.
  /// Pass nullptr (or skip the call entirely) on prototype boards.
  void set_fuel_gauge(FuelGaugeDevice *fg);

  // -------------------------------------------------------------------------
  // BMS operations (called by orchestrator on timer)
  // -------------------------------------------------------------------------

  /// Poll BMS for current status.  Fast I2C read, non-blocking.
  /// Returns a PowerSnapshot with all fields populated (invalid sentinels on error).
  ///
  /// @param pm_invalid_hint  Caller-supplied flag indicating the latest PM
  ///   read returned an invalid sentinel.  When true and the chip reports
  ///   on battery, poll_bms triggers a PMID resync (full re-prep + verify)
  ///   to recover from a suspected autonomous EN_OTG clear.  No-op
  ///   otherwise.  Default false preserves the cheap-poll behavior for
  ///   call sites that don't track PM validity.
  PowerSnapshot poll_bms(bool pm_invalid_hint = false);

  /// Lightweight charging-status-only poll
  /// Use on a fast timer to detect plug/unplug quickly without the cost
  /// of a full ADC + battery-percentage poll.
  /// @param[out] state  Populated on success.
  /// @return true if the read succeeded.
  bool poll_charging_status(BmsChargingState &state);

  /// Lightweight charger status poll.
  ///
  /// Used by the fast runtime timer to detect plug/unplug changes and keep the
  /// PMID rail configured correctly without waiting for the full telemetry poll.
  /// @param[out] status Populated on success.
  /// @return true if the read succeeded.
  bool poll_status(BmsStatus &status);

  /// Reset BMS watchdog.  Must be called periodically (< 10 s interval).
  /// @return true if the watchdog reset succeeded.
  bool reset_watchdog();

  /// Trigger BMS QoN (ship mode).  Device powers off.  Does not return.
  void shutdown();

  // -------------------------------------------------------------------------
  // FG-learning control (used by the orchestrator's learning wiring)
  // -------------------------------------------------------------------------

  /// Aggregate the learned-value read-back for the verify step.  Returns a
  /// readout with ok=false when no FG is attached or a read fails.
  FgLearningVerifyReadout read_fg_learning_verify();

  /// Manually hold or release the battery charge path for a learning phase.
  /// While disabled the learning run owns the charge state; poll_bms()'s
  /// thermal / full-charge auto-toggles are suppressed.
  void set_manual_charge_disabled(bool disabled);

  /// Program the fast-charge current limit (mA).  Forwards to the BMS.
  bool set_charge_current_ma(uint16_t current_ma);

  /// Set/clear the FG Update Status learning bits (no-op without an FG).
  bool set_update_status_learning(bool enable);

  /// Re-configure the BMS watchdog timeout.  See
  /// BmsDevice::set_watchdog_timeout_ms for semantics.  Forwards directly;
  /// no policy or kick-cadence change is applied here.
  bool set_watchdog_timeout_ms(uint32_t timeout_ms);

  // -------------------------------------------------------------------------
  // External watchdog
  // -------------------------------------------------------------------------

  /// Configure the external watchdog GPIO as output (LOW).
  /// No-op if pin_ext_wdt < 0 in Config.
  void init_ext_watchdog();

  /// Pulse the external watchdog GPIO (HIGH 20 ms, then LOW).
  /// No-op if pin_ext_wdt < 0 in Config.
  void reset_ext_watchdog();

  // -------------------------------------------------------------------------
  // RTC state persistence
  // -------------------------------------------------------------------------

  /// Save application state to RTC memory before sleep.
  /// Under TEST_HOST the state is stored in a regular static variable.
  void save_state(const RtcAppState &state);

  /// Load application state from RTC memory after wake.
  /// Returns default-constructed RtcAppState when no valid state has been saved.
  RtcAppState load_state() const;

  // -------------------------------------------------------------------------
  // Sleep cycle
  // -------------------------------------------------------------------------

  /// Determine whether to sleep, which type, and for how long.
  ///
  /// Pure logic — no platform dependencies; testable on host.
  /// Uses Config::deep_sleep_threshold_ms from construction.
  ///
  /// Rules:
  ///   - Not Offline mode  -> {None, 0}  (only Offline sleeps)
  ///   - Unlocked          -> {None, 0}  (never sleep while user is active)
  ///   - sleep_ms >= deep_sleep_threshold_ms -> {Deep, sleep_ms}
  ///   - sleep_ms <  deep_sleep_threshold_ms -> {None, 0}  (stay awake; avoid
  ///     deep sleep overhead exceeding the benefit for short intervals)
  ///
  /// sleep_ms = min(enabled intervals) - awake_ms, clamped to 0.
  ///
  /// @param settings   Current settings (sensor and display intervals).
  /// @param lock_state Current lock state.
  /// @param mode       Current operating mode.
  /// @param awake_ms   Milliseconds the device has been awake this cycle.
  SleepDecision decide_sleep(const GoSettings &settings, LockState lock_state, OperatingMode mode,
                             uint32_t awake_ms) const;

  /// Return true when the given sleep duration is short enough that keeping
  /// the PM sensor powered (GPIO held) across deep sleep is beneficial.
  ///
  /// Pure logic — no platform dependencies; testable on host.
  bool should_hold_pm_sensor(uint32_t sleep_duration_ms) const;

  /// Return true when the measurement interval is long enough to justify
  /// power-cycling the PM sensor between measurements (Portable mode).
  ///
  /// The threshold is `Config::pm_sleep_threshold_ms` (default 20 s), which
  /// accounts for ~10 s warmup plus a minimum off-time to make the power
  /// cycle worthwhile.
  ///
  /// Pure logic — no platform dependencies; testable on host.
  bool should_sleep_pm_sensor(uint32_t measure_interval_ms) const;

  /// Control PM sensor power GPIO.  Drives the pin to the variant-appropriate
  /// level (on=true → pm_power_on_level, on=false → inverted).
  /// No-op when `Config::pin_pm_power < 0`.
  void set_pm_power(bool on);

  /// Enter deep sleep.  Does not return — CPU reboots on wake.
  ///
  /// Configures timer and GPIO wake sources, then calls
  /// esp_deep_sleep_start().  Only call when decide_sleep() returns Deep.
  ///
  /// When `should_hold_pm_sensor(sleep_duration_ms)` is true, the PM power
  /// GPIO is held HIGH during deep sleep via `gpio_hold_en()`.  On ESP32-C5
  /// per-pin hold automatically persists through deep sleep.  The caller
  /// must set `RtcAppState::sensors_warm`
  /// accordingly before calling `save_state()`.
  ///
  /// @param sleep_duration_ms How long to sleep before timer wake.
  void enter_sleep(uint32_t sleep_duration_ms);

  // -------------------------------------------------------------------------
  // Boot path (static — call before any service is constructed)
  // -------------------------------------------------------------------------

  /// Determine wake cause early in app_main.
  /// Translates esp_sleep_get_wakeup_cause() to WakeCause.
  static WakeCause get_wake_cause();

  /// Returns true when this boot should follow the abbreviated fast path:
  ///   cause == WakeCause::Timer && state.lock_state == LockState::Locked
  ///
  /// Pure logic — no platform dependencies; testable on host.
  static bool is_fast_path_wake(WakeCause cause, const RtcAppState &state);

  /// Release GPIO holds that were enabled before the previous deep sleep.
  ///
  /// Must be called **after** `init_gpio()` has reconfigured the held pin
  /// as output HIGH.  While hold is active the pad stays latched; the GPIO
  /// driver writes the new output config to registers underneath.
  /// Releasing hold then lets the fresh output driver take over with no
  /// power glitch.  No-op when pin_pm_power < 0.
  /// Guarded by `#ifndef TEST_HOST`.
  static void release_sleep_gpio_holds(int pin_pm_power);

  // -------------------------------------------------------------------------
  // Constants
  // -------------------------------------------------------------------------

  /// Battery percentage below which the critical flag is set in PowerSnapshot.
  /// Fixed threshold — not a user-configurable setting.
  static constexpr float BATTERY_CRITICAL_PERCENT = 5.0f;

  // --- EDV (over-discharge) thresholds ---
  static constexpr float EDV_SHIP_THRESHOLD_V = 2.9f;
  static constexpr int EDV_SHIP_DEBOUNCE_SAMPLES = 3;

  // --- OT (over-temperature) thresholds ---
  //
  // Two-tier policy: CUTOFF disables charging while still allowing the
  // system to run; SHIP trips ship mode at the higher threshold.
  // Hysteresis between CUTOFF (50 °C) and RESUME (47 °C) prevents
  // chattering near the cutoff boundary.  Values validated on hardware
  // against AGo's single-cell Li-ion pack.
  static constexpr int16_t OT_CHARGE_HOT_CUTOFF_C = 50;
  static constexpr int16_t OT_CHARGE_HOT_RESUME_C = 47;
  static constexpr int16_t OT_SHIP_THRESHOLD_C = 60;

  // --- Full-charge pause ---
  //
  // When the battery is full and USB is present, charging is paused to
  // reduce cell stress.  Charging resumes when SOC drops below the
  // resume threshold.  Hysteresis (100 → 95%) prevents chattering.
  static constexpr uint8_t FULL_CHARGE_RESUME_SOC = 95;

private:
  BmsDevice &_bms;
  const gpio::Hal &_gpio;
  Config _config;
  FuelGaugeDevice *_fg = nullptr;

  // --- EDV trip-state members ---
  int _edv_low_count = 0;

  // --- OT trip-state members ---

  /// True while charging is held off by the over-temperature guard (cell
  /// crossed OT_CHARGE_HOT_CUTOFF_C going up).  Cleared when the cell
  /// cools below OT_CHARGE_HOT_RESUME_C.  Edge-triggered: only issue
  /// set_charge_enable(false / true) on the transitions, not every poll.
  bool _thermal_charge_disabled = false;

  // --- Full-charge pause state ---

  /// True while charging is held off because the battery reached full
  /// charge while plugged in.  Cleared when SOC drops to
  /// FULL_CHARGE_RESUME_SOC.  Edge-triggered like the thermal guard.
  bool _full_charge_paused = false;

  /// True while a FG-learning phase owns the charge path (set via
  /// set_manual_charge_disabled).  Suppresses poll_bms()'s thermal /
  /// full-charge auto re-enable so learning controls charging.
  bool _manual_charge_disabled = false;

  /// Log charger status, ADC telemetry, and FG telemetry for a single
  /// poll_bms() snapshot.  Pure side-effect (serial output); does not
  /// mutate any state.
  void _log_poll_snapshot(const PowerSnapshot &snap);

  /// Configure timer and GPIO wake sources before entering sleep.
  /// Wrapped in #ifndef TEST_HOST — not callable from host test builds.
  void configure_wake_sources(uint32_t timer_ms);
};

// ---------------------------------------------------------------------------
// Free function — early boot path
// ---------------------------------------------------------------------------

/// Read RtcAppState from RTC memory.  Returns default state if no valid
/// state has been saved.  No dependencies — safe to call early in app_main
/// before PowerService is constructed.
RtcAppState load_rtc_app_state();
