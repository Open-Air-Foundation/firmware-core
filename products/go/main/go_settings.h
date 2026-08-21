#ifndef GO_SETTINGS_H
#define GO_SETTINGS_H

#include "config_store.h"
#include "go_config_types.h"
#include "go_types.h"
#include "led/go_led_types.h"
#include "measurement_corrections.h"
#include "types/wifi_types.h"

struct GoSettings {
  // --- Measurement interval ---
  int measure_interval_seconds = MEASURE_INTERVAL_SECONDS_DEFAULT;

  // --- Display ---
  bool use_fahrenheit = false;
  bool use_feet = false;
  bool pm_use_usaqi = false;

  // --- GPS ---
  GpsMode gps_mode = GpsMode::OnWhenTracking;

  // --- Device behavior ---
  OperatingMode operating_mode = OperatingMode::Portable;
  int auto_lock_seconds = 10; // 0 = auto-lock disabled

  // --- LED brightness ---
  LedBrightness front_led_brightness = LedBrightness::Off;
  LedBrightness back_led_brightness = LedBrightness::Off;
  TouchLedIntensity touch_led_intensity = TouchLedIntensity::Off;

  // --- Buzzer ---
  bool buzzer_enabled = false;

  // --- Stationary connectivity ---
  bool disable_cloud = false; // honored by CloudService
  ConfigurationControl configuration_control = ConfigurationControl::Both;
  int co2_abc_days = CO2_ABC_DAYS_DEFAULT;
  int tvoc_learning_offset = LEARNING_OFFSET_HOURS_DEFAULT;
  int nox_learning_offset = LEARNING_OFFSET_HOURS_DEFAULT;
  WifiStaticIpConfig static_ip{}; // ip == 0 means DHCP

  // --- First-boot onboarding ---
  // Getting Started guide latch (NVS "obd"); cleared on factory reset.
  bool onboarding_done = false;

  // --- Measurement corrections ---
  MeasurementCorrections corrections{};

  bool equals(const GoSettings &other) const;
};

GoSettings load_go_settings(ConfigStore &store);
bool is_go_settings_valid(const GoSettings &settings);
bool save_go_settings(ConfigStore &store, const GoSettings &settings);
void print_settings(const GoSettings &settings);

// ---------------------------------------------------------------------------
// Factory fuel-gauge learning state (production-level, separate from user
// GoSettings). Stored under distinct keys in the existing "go" namespace and
// never written by save_go_settings(), so factory_reset() leaves it intact.
// ---------------------------------------------------------------------------

enum class FgLearningStage : uint8_t {
  Idle = 0,  ///< not learning (normal operation)
  Charge,    ///< charging to full charge
  Rest,      ///< charge off, quiet load, capturing OCV1
  Discharge, ///< unplug cue raised; draining on battery toward EDV
  CycleDone, ///< EDV reached — committed before ship mode
  Verify,    ///< re-plugged after final cycle; checking pass criteria
  Complete,  ///< learned + verified → normal operation forever
  Failed,    ///< gave up (POR-loss loop, or verify failed at cap)
};

struct FactorySettings {
  FgLearningStage fg_learning_stage = FgLearningStage::Idle;
  uint8_t fg_learning_cycle = 0;        ///< 1-based cycle in progress
  uint8_t fg_learning_itpor_losses = 0; ///< POR-induced restart count
  uint8_t fg_learning_fail_reason = 0;  ///< FgLearnFailReason (as byte); 0 = None
};

/// True when a learning run is in progress and should pre-empt normal boot.
/// Both terminals (Complete, Failed) are sticky: a finished unit re-enters its
/// result screen on every boot until a BOOT press clears the run. Pure —
/// host-testable boot-routing predicate.
bool is_factory_learning_stage_active(FgLearningStage stage);

bool load_factory_settings(ConfigStore &store, FactorySettings &out);
bool save_factory_settings(ConfigStore &store, const FactorySettings &in);

/// Atomic single-commit write of just the run state, for the pre-ship
/// CycleDone persist (must confirm before ship mode).
bool save_fg_learning_state(ConfigStore &store, FgLearningStage stage, uint8_t cycle,
                            uint8_t itpor_losses);

/// Persist the fail reason (FgLearnFailReason as a byte) so the Failed
/// dashboard shows the cause after a sticky-Failed reboot.
bool save_fg_learning_fail_reason(ConfigStore &store, uint8_t reason);

/// Explicit, deliberate clear of factory state. factory_reset() does NOT call
/// this; it is invoked only by the "reset learning" action.
bool clear_factory_settings(ConfigStore &store);

#endif // GO_SETTINGS_H
