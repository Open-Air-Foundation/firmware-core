#ifndef GO_SETTINGS_H
#define GO_SETTINGS_H

#include <string>

#include "config_store.h"
#include "go_types.h"
#include "led/go_led_types.h"
#include "types/wifi_types.h"

struct GoSettings {
  // --- Measurement interval ---
  int measure_interval_seconds = 10; // 1..3600

  // --- Display ---
  bool use_fahrenheit = false;
  bool pm_use_usaqi = false;

  // --- GPS ---
  int gps_interval_seconds = 5;
  GpsMode gps_mode = GpsMode::OnWhenTracking;

  // --- Device behavior ---
  OperatingMode operating_mode = OperatingMode::Portable;
  int inactivity_timeout_seconds = 5;
  int auto_lock_seconds = 10; // 0 = auto-lock disabled

  // --- Identity ---
  std::string device_name = "airgradient-go";

  // --- LED brightness ---
  LedBrightness front_led_brightness = LedBrightness::Off;
  LedBrightness back_led_brightness = LedBrightness::Off;
  TouchLedIntensity touch_led_intensity = TouchLedIntensity::Off;

  // --- Buzzer ---
  bool buzzer_enabled = false;

  // --- Stationary connectivity ---
  bool disable_cloud = false;     // honored by CloudService
  WifiStaticIpConfig static_ip{}; // ip == 0 means DHCP

  // --- First-boot onboarding ---
  // Getting Started guide latch (NVS "obd"); cleared on factory reset.
  bool onboarding_done = false;
};

GoSettings load_go_settings(ConfigStore &store);
bool save_go_settings(ConfigStore &store, const GoSettings &settings);
void print_settings(const GoSettings &settings);

// ---------------------------------------------------------------------------
// Fuel-gauge learning — factory/production state
// ---------------------------------------------------------------------------

/// Phase of the automated fuel-gauge learning run.  Drives the FgLearning
/// controller and persists across reboots and factory_reset().
enum class FgLearningStage : uint8_t {
  Idle = 0,  ///< Not learning (normal operation)
  Charge,    ///< Charging to Full Charge
  Rest,      ///< Charge off, quiet load, capturing OCV1
  Discharge, ///< Unplug cue raised; draining toward EDV
  CycleDone, ///< EDV reached — persisted+committed before ship mode
  Verify,    ///< Re-plugged after final cycle; checking pass criteria
  Complete,  ///< Learned + verified → normal operation
  Failed,    ///< Gave up (POR-loss loop, or verify failed at cap)
};

/// Persisted learning-run state.  Factory/production-level, distinct from
/// user GoSettings: written under separate "go"-namespace keys that
/// save_go_settings() never touches, so factory_reset() leaves it intact.
struct FactorySettings {
  FgLearningStage fg_learning_stage = FgLearningStage::Idle;
  uint8_t fg_learning_cycle = 0;        ///< 1-based cycle in progress
  uint8_t fg_learning_itpor_losses = 0; ///< POR-induced restart count
};

/// Load/save the full FactorySettings block (keys "fs_s"/"fs_c"/"fs_i").
/// Absent keys load as struct defaults (Idle / 0 / 0).
bool load_factory_settings(ConfigStore &store, FactorySettings &out);
bool save_factory_settings(ConfigStore &store, const FactorySettings &in);

/// Atomic single-commit write of just the run state.  Used for the pre-ship
/// CycleDone persist, which must confirm before ship mode.
bool save_fg_learning_state(ConfigStore &store, FgLearningStage stage, uint8_t cycle,
                            uint8_t itpor_losses);

/// Explicit, deliberate clear of factory state back to defaults.
/// factory_reset() does NOT call this; only the "reset learning" path does.
bool clear_factory_settings(ConfigStore &store);

#endif // GO_SETTINGS_H
