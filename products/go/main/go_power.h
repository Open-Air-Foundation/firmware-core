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
  UnderTemperature,
};

// ---------------------------------------------------------------------------
// FgLearningFlag
// ---------------------------------------------------------------------------

/// Bit masks for PowerSnapshot::fg_learning_flags. poll_bms_fg_learning() maps
/// the gauge Flags() bits and its part-neutral FgLearningProgress into this
/// compact byte.
enum FgLearningFlag : uint8_t {
  FG_LEARN_FC = 1u << 0,        ///< Flags() FC        (full charge)
  FG_LEARN_CHG = 1u << 1,       ///< Flags() CHG       (charging)
  FG_LEARN_DSG = 1u << 2,       ///< Flags() DSG       (discharging)
  FG_LEARN_ITPOR = 1u << 3,     ///< Flags() ITPOR     (POR wiped learning)
  FG_LEARN_OCV_TAKEN = 1u << 4, ///< Flags() OCVTAKEN
  FG_LEARN_QMAX_UP = 1u << 5,   ///< FgLearningProgress::qmax_updated
  FG_LEARN_RES_UP = 1u << 6,    ///< FgLearningProgress::ra_updated
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

  // FG learning flags packed into one byte, decoded by poll_bms() from the
  // gauge Flags() and CONTROL_STATUS registers. Consumed (parsed on the fly)
  // by the pure FgLearningController FSM and the learning dashboard.
  uint8_t fg_learning_flags = 0;       ///< bitmask of FgLearningFlag
  bool external_input_present = false; ///< plugged vs battery, at boot/poll
  bool edv_cutoff_reached = false;     ///< derived: ship_mode_request == OverDischarge

  /// Learning-only: the discharge half of a learning cycle has reached its end
  /// point.  On v1 that is the firmware EDV cutoff that also ships the device;
  /// on v2 the gauge owns undervoltage, so it is the cell reaching the learning
  /// discharge floor while the system stays up.
  bool discharge_target_reached = false;

  /// Learning-only: Design Capacity as configured in the gauge, mAh.
  uint16_t fg_design_capacity_mah = 0;

  /// True when the gauge is holding the CHG FET open on temperature
  /// (Flags CHG_SUS or CHG_INH).  Charger status is rewritten away from
  /// ChargeTerminationDone while this is set, because the charger reports a
  /// gauge-blocked charge and a finished one identically.
  bool charge_blocked_by_gauge = false;

  /// True when charging has been paused because the battery is full and
  /// external power is present.  Cleared when SOC drops below the resume
  /// threshold.
  bool full_charge_paused = false;

  /// Non-None when a safety trip requires the orchestrator to show a
  /// warning and enter ship mode.
  ShipModeRequest ship_mode_request = ShipModeRequest::None;
};

// ---------------------------------------------------------------------------
// FgLearningVerifyReadout
// ---------------------------------------------------------------------------

/// Aggregated learned-value read-back for the factory learning verify step.
/// Filled by PowerService::read_fg_learning_verify(); consumed by the pure
/// FgLearningController::verify_pass().
struct FgLearningVerifyReadout {
  bool ok = false;                  ///< all underlying reads succeeded
  bool itpor = false;               ///< Flags() ITPOR (a POR wiped learning)
  bool qmax_up = false;             ///< the gauge reported a learned Qmax
  uint16_t qmax_mah = 0;            ///< learned Qmax in mAh (raw * DC / 2^14)
  uint16_t design_capacity_mah = 0; ///< configured Design Capacity
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
    bool fg_has_protector = false;             ///< Gauge opens the DSG FET on undervoltage by
                                               ///< itself (v2.0 BQ27742-G1), so the firmware EDV
                                               ///< ship-mode path below is not needed.
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

  /// @param bms     Optional BMS device. A non-null device must outlive this service.
  /// @param gpio    GPIO HAL function-pointer table.
  /// @param config  Runtime configuration (wake pins, sleep threshold).
  PowerService(BmsDevice *bms, const gpio::Hal &gpio, const Config &config);

  /// Attach a BMS that became available after this service was constructed.
  /// Non-owning: the BMS must outlive PowerService.
  void set_bms(BmsDevice *bms);

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

  /// Factory-learning poll: a normal poll_bms() plus the learning-only fields
  /// (packed fg_learning_flags incl. the extra CONTROL_STATUS read,
  /// external_input_present, edv_cutoff_reached). Used only by the factory
  /// FgLearningRunner so the normal field path pays no extra fuel-gauge reads.
  PowerSnapshot poll_bms_fg_learning(bool pm_invalid_hint = false);

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

  /// Re-kick PMID boost if collapsed.  Acts only when on battery with a
  /// valid vpmid below PMID_HEALTHY_MIN_MV.
  /// @return true if a re-kick was performed.
  bool rekick_pmid_if_collapsed(const BmsTelemetry &t, BmsPowerSource src);

  /// Read fresh status + telemetry, then delegate to
  /// rekick_pmid_if_collapsed().
  /// @return true if a re-kick was performed.
  bool ensure_pmid_healthy();

  /// Power-cycle the PM sensor via boost kill (V1 only).
  ///
  /// Isolates EN_PM, kills the PMID boost so the SPS30 fully de-powers,
  /// then restores both.  Only useful on V1 where PMID is the SPS30's
  /// sole power source.
  void recover_pm_sensor();

  /// Reset BMS watchdog.  Must be called periodically (< 10 s interval).
  /// @return true if the watchdog reset succeeded.
  bool reset_watchdog();

  /// Read back the learned fuel-gauge values for the factory learning verify
  /// step (Qmax, Ra grid, Design Capacity, ITPOR, progress). Aggregates several
  /// gauge reads; `ok` is false if any required read failed.
  FgLearningVerifyReadout read_fg_learning_verify();

  // -------------------------------------------------------------------------
  // Factory learning charge / gauge control (runner-facing)
  // -------------------------------------------------------------------------

  /// Program the fast-charge current limit (CC mode). @return true on success.
  bool set_charge_current_ma(uint16_t current_ma);

  /// Manually enable/disable the battery charge path (true = charging off).
  void set_manual_charge_disabled(bool disabled);

  /// Idempotently switch the gauge to the 4.2 V chemistry (Chem ID 0x1202).
  /// No-op (returns false) when no fuel gauge is attached.
  bool set_chemistry_4v2();

  /// Set/clear the gauge Update Status learning bits. No-op (false) when no
  /// fuel gauge is attached.
  bool set_update_status_learning(bool enable);

  /// Consecutive readings under the ship threshold.  Mirrored into
  /// RtcAppState by poll_bms() so it survives deep sleep.
  int edv_low_count() const { return _edv_low_count; }

  /// Trigger BMS QoN (ship mode).  Device powers off.  Does not return.
  void shutdown();

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
  /// @param low_battery  A reading has already come in under the ship
  ///                     threshold, so re-check the cell on the watch interval
  ///                     instead of the measurement interval.
  SleepDecision decide_sleep(const GoSettings &settings, LockState lock_state, OperatingMode mode,
                             uint32_t awake_ms, bool low_battery = false) const;

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
  //
  // The firmware shuts the device down on its own so the user sees why and the
  // storage layer gets to close.  A gauge that owns undervoltage is the
  // backstop underneath that, not the normal way the device stops: letting it
  // be the one to cut a running system means no warning, and its recovery
  // threshold then re-closes the FET once the unloaded cell springs back,
  // which cycles.
  static constexpr float EDV_SHIP_THRESHOLD_V = 2.9f;
  /// Used where the gauge has its own undervoltage trip underneath this one.
  static constexpr float EDV_SHIP_THRESHOLD_PROTECTED_V = 2.8f;
  static constexpr int EDV_SHIP_DEBOUNCE_SAMPLES = 3;
  /// Fewer samples where the gauge's own trip waits underneath: the cell is on
  /// the steep part of its curve by then, and the watch interval that separates
  /// these readings is a minute, so a third one costs margin the shutdown needs.
  static constexpr int EDV_SHIP_DEBOUNCE_SAMPLES_PROTECTED = 2;
  /// Sleep length once a reading comes in under the threshold.  The device
  /// stops following the measurement interval and just re-checks the cell, so
  /// the debounce completes in minutes instead of never.
  static constexpr uint32_t LOW_BATTERY_WATCH_INTERVAL_MS = 60000;

  /// End of the discharge half of a learning cycle on a gauge that owns
  /// undervoltage itself (v2).  Set at the gauge's Terminate Voltage, which
  /// leaves 300 mV before its UV Prot opens the pack, so the run can persist
  /// its stage and rest instead of losing power mid-cycle.
  static constexpr uint16_t FG_LEARNING_DISCHARGE_END_MV = 3000;

  // --- Battery temperature thresholds ---
  static constexpr int16_t CHARGE_MIN_TEMPERATURE_C = 0;
  static constexpr int16_t CHARGE_MAX_TEMPERATURE_C = 45;
  static constexpr int16_t CHARGE_RECOVERY_MIN_TEMPERATURE_C = 2;
  static constexpr int16_t CHARGE_RECOVERY_MAX_TEMPERATURE_C = 43;
  static constexpr int16_t DISCHARGE_MIN_TEMPERATURE_C = -10;
  static constexpr int16_t DISCHARGE_MAX_TEMPERATURE_C = 60;

  // --- PMID boost recovery ---
  static constexpr uint16_t PMID_HEALTHY_MIN_MV = 4500; ///< Floor below which PMID is collapsed
  static constexpr uint32_t PMID_REKICK_OFF_MS = 15;    ///< EN_OTG low dwell before re-assert
  /// The charger will not start the boost below its own VBAT_OTG window
  /// (2.9-3.1 V, SLUSEG4C §8.3.10.3.4), so re-kicking under this only costs
  /// current and a log line.  Uses the top of the window, since the exact trip
  /// point varies per part.
  static constexpr float PMID_BOOST_MIN_BATTERY_V = 3.1f;

  // --- PM sensor recovery (V1 boost-kill power cycle) ---
  static constexpr uint32_t PM_RECOVER_OFF_MS = 50;     ///< SPS30 discharge after boost kill
  static constexpr uint32_t PM_RECOVER_SETTLE_MS = 100; ///< PMID ramp + SPS30 boot after restore

  // --- Full-charge pause ---
  //
  // When the battery is full and USB is present, charging is paused to
  // reduce cell stress.  Charging resumes when SOC drops below the
  // resume threshold.  Hysteresis (100 → 95%) prevents chattering.
  static constexpr uint8_t FULL_CHARGE_RESUME_SOC = 95;

private:
  BmsDevice *_bms;
  const gpio::Hal &_gpio;
  Config _config;
  FuelGaugeDevice *_fg = nullptr;

  // --- EDV trip-state members ---
  int _edv_low_count = 0;
  bool _edv_count_seeded = false;
  bool _pmid_boost_blocked_logged = false;

  // --- Battery temperature state ---

  /// True while charging is held off because the battery temperature is
  /// invalid or outside the charging range. Edge-triggered: only issue
  /// set_charge_enable(false / true) on state transitions, not every poll.
  bool _thermal_charge_disabled = false;

  // --- Full-charge pause state ---

  /// True while charging is held off because the battery reached full
  /// charge while plugged in.  Cleared when SOC drops to
  /// FULL_CHARGE_RESUME_SOC.  Edge-triggered like the thermal guard.
  bool _full_charge_paused = false;

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
