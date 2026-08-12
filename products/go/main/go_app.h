#pragma once

#include "go_board.h"
#include "go_display.h"
#include "go_power.h"
#include "go_types.h"
#include "gps/gps_types.h"
#include "measures_types.h"

class GoApp {
public:
  explicit GoApp(GoBoard &board);

  /// Main entry point.  Selects boot path and runs.
  /// Never returns on target.
  void run();

private:
  // --- Boot paths ---
  void run_fast_path(const RtcAppState &state);
  void run_button_wake_path(const RtcAppState &state);
  void run_interactive(WakeCause cause, BootHandoff handoff);

  /// Dedicated factory fuel-gauge learning path. Entered from run() before
  /// select_boot_path() when a learning run is armed/resumed. Brings up the
  /// core, constructs FgLearningRunner, and hands off. Never returns on target.
  void run_factory_learning_path(const RtcAppState &state);

  // --- Testable fast-path core ---

  struct FastPathResult {
    enum class Outcome { Sleep, Promote, Shutdown };
    Outcome outcome;
    BootHandoff handoff;
    MeasuresAGo measures;
    bool has_measures;
    uint32_t sleep_duration_ms; ///< Valid when outcome == Sleep
    bool sensors_warm;          ///< RTC persistence before sleep
  };

  FastPathResult execute_fast_path(const RtcAppState &state, const volatile bool &button_flag,
                                   const RtcDisplaySnapshot *snapshot, bool snapshot_valid);

  GoBoard &_board;

#ifdef TEST_HOST
  friend class GoAppTestAccess;
#endif
};

// ---------------------------------------------------------------------------
// Boot path selection
// ---------------------------------------------------------------------------

enum class BootPath { FastPath, ButtonWake, Interactive };

BootPath select_boot_path(WakeCause cause, const RtcAppState &state);

// ---------------------------------------------------------------------------
// Pure utility functions
// ---------------------------------------------------------------------------

/// Convert shared Measures to product-specific MeasuresAGo.
MeasuresAGo measures_to_ago(const Measures &m);

/// Build DisplayValues for the fast-path locked dashboard.
DisplayValues build_fast_path_display(const MeasuresAGo &measures, const GpsData &gps,
                                      const PowerSnapshot &bms, const GoSettings &settings,
                                      bool tracking_active);

/// Build DisplayValues for button-wake early paint (snapshot-based).
DisplayValues build_wake_values(const RtcDisplaySnapshot &snapshot, bool snapshot_valid);

/// Build DisplayValues for the cold-boot splash (Screen::Info, "Booting...").
/// Used when Interactive boot has no snapshot and no fast-path measurement.
DisplayValues build_boot_splash_values();

/// Splash text. Kept in sync with the UIManager seed via show_info().
inline constexpr const char *BOOT_SPLASH_TEXT = "Getting Ready";

/// Determine whether GPS should be active based on settings and RTC state.
/// Used by all three boot paths — eliminates the duplicated inline check.
bool is_gps_active_at_boot(const GoSettings &settings, const RtcAppState &state);
