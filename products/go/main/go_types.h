#ifndef GO_TYPES_H
#define GO_TYPES_H

#include <cstdint>

// --- Operating mode and behavior ---

enum class OperatingMode : uint8_t {
  Portable,
  Stationary,
  Offline,
};

enum class Behavior : uint8_t {
  Tracking,
  Idle,
  Shutdown,
};

enum class LockState : uint8_t {
  Locked,
  Unlocked,
};

enum class WakeCause : uint8_t {
  PowerOn, // fresh boot or power-on reset
  Timer,   // deep sleep timer expiry
  Button,  // GPIO wake from deep sleep
};

enum class GpsMode : uint8_t {
  AlwaysOff,
  OnWhenTracking,
  AlwaysOn,
};

enum class InputSource : uint8_t {
  TouchUp,
  TouchDown,
  TouchEnter,
  ButtonPower,
  ButtonBoot,
};

enum class InputType : uint8_t {
  ShortPress,
  LongPress,
  DoublePress,
};

// --- RTC-persisted application state ---
//
// Saved to RTC memory before deep sleep; restored on wake to resume application
// state without full re-initialization. Defaults represent a safe starting
// point for a fresh power-on (Offline, Idle, Locked).

struct RtcAppState {
  OperatingMode mode = OperatingMode::Portable;
  Behavior behavior = Behavior::Idle;
  LockState lock_state = LockState::Locked;
  bool gps_enabled = true;
  bool tracking_active = false;
  uint32_t tracking_session_id = 0; ///< 5-digit session ID; 0 = no active session
  bool sensors_warm = false;        ///< Sensors kept powered during last deep sleep
};

// --- Forward declarations for BootHandoff pointer members ---

struct RtcDisplaySnapshot;
struct MeasuresAGo;

// --- Boot-to-runtime handoff ---
//
// Describes what boot has already done so the Orchestrator can skip redundant
// work and pick up where boot left off.  Default-initialized values represent
// a fresh power-on boot where nothing has been done yet.

struct BootHandoff {
  /// Display already shows a valid frame for the current state.
  /// When true, init() skips update_display() and sets state directly.
  bool display_painted = false;

  /// A measurement was already completed during boot (fast-path).
  /// When true, the Orchestrator sets _first_measurement_done and
  /// skips the initial measurement request.
  bool measurement_completed = false;

  /// Suppress the first ButtonPower short-press event. Used when the
  /// wake press that triggered boot should not toggle lock state.
  bool suppress_wake_press = false;

  /// Initial lock state. Boot paths that unlock early (button wake,
  /// fast-path promotion on button press) set this to Unlocked.
  LockState initial_lock_state = LockState::Locked;

  /// Optional RTC display snapshot for seeding stale display values.
  /// Used by button-wake path to avoid dashes before fresh data arrives.
  const RtcDisplaySnapshot *display_snapshot = nullptr;

  /// This boot resumes from a PMID BATFET recovery cycle: RTC state was
  /// rebuilt from the NVS resume blob, display_snapshot carries the
  /// pre-reboot frame, and the whole boot must stay invisible (no chime,
  /// no boot LED animation, no splash, no "Unlocked" snackbar, no e-paper
  /// flash — DisplayService::init_resume instead of init).
  bool pmid_resume = false;

  /// Optional measurement from fast-path boot (promotion case).
  /// When non-null, the Orchestrator seeds _cached_measures.
  const MeasuresAGo *fast_path_measures = nullptr;
};

#endif // GO_TYPES_H
