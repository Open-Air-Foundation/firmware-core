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

// Values are also exposed by the BLE Status trk field.
enum class TrackingState : uint8_t {
  Idle = 0,
  Recording = 1,
  Paused = 2,
};

// --- Tracking timing data; pure operations live in go_type_helpers.hpp ---

inline constexpr uint32_t TRACKING_TIME_INVALID_S = UINT32_MAX;

struct TrackingTiming {
  uint32_t session_started_s = TRACKING_TIME_INVALID_S;
  uint32_t recording_accumulated_s = TRACKING_TIME_INVALID_S; ///< Completed active segments
  uint32_t recording_started_s = TRACKING_TIME_INVALID_S; ///< Current segment; invalid if paused
  uint32_t last_record_s = TRACKING_TIME_INVALID_S;       ///< Last accepted write, not sync
};

static_assert(sizeof(TrackingTiming) == 16, "Tracking timing must contain four 32-bit fields");

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
// point for a fresh power-on (Portable, Idle, Locked).

enum class RtcAppFlag : uint32_t {
  GpsEnabled = 1u << 0,
  SensorsWarm = 1u << 1,           ///< Sensors kept powered during last deep sleep
  ChargingOnlyRequested = 1u << 2, ///< One-shot request after USB-powered user shutdown
  // Bits 3–31 are reserved for future flags.
};

struct RtcAppState {
  OperatingMode mode = OperatingMode::Portable;
  Behavior behavior = Behavior::Idle;
  LockState lock_state = LockState::Locked;
  TrackingState tracking_state = TrackingState::Idle;
  uint32_t tracking_session_id = 0; ///< 5-digit session ID; 0 = no active session
  uint32_t flags = static_cast<uint32_t>(RtcAppFlag::GpsEnabled);

  constexpr bool has_flag(RtcAppFlag flag) const {
    return (flags & static_cast<uint32_t>(flag)) != 0;
  }

  constexpr void set_flag(RtcAppFlag flag, bool enabled) {
    const auto mask = static_cast<uint32_t>(flag);
    flags = enabled ? (flags | mask) : (flags & ~mask);
  }
};

static_assert(sizeof(RtcAppState) == 12, "RTC app state layout must remain 12 bytes");

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

  /// Optional measurement from fast-path boot (promotion case).
  /// When non-null, the Orchestrator seeds its authoritative raw snapshot.
  const MeasuresAGo *fast_path_measures = nullptr;
};

#endif // GO_TYPES_H
