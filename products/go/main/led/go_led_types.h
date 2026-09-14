/**
 * AirGradient Go -- LED public types
 *
 * Product-specific types shared between the LED service, orchestrator,
 * and UI layers.  No dependencies beyond <cstdint>.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>

struct Rgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

/// Physical LED wiring on the LP5036.  RGB groups are addressed by their
/// blue-channel base (OUTn = B, OUTn+1 = G, OUTn+2 = R); -1 marks an LED
/// that does not exist on the board.
struct LedMap {
  static constexpr uint8_t MAX_BACK = 5;

  int8_t front[2];          ///< single-channel white LEDs
  uint8_t back[MAX_BACK];   ///< AQI ring, in animation order
  uint8_t back_count;
  int8_t touch_select;      ///< pad feedback groups
  int8_t touch_left;
  int8_t touch_right;

  /// v1.0 LED board: LED1/LED2/LED10 touch, five back LEDs, LED25/26 front.
  static constexpr LedMap v1() { return {{30, 31}, {6, 12, 15, 18, 24}, 5, 27, 3, 0}; }

  /// v2.0 main board: only LED1–LED4 are routed (OUT0–OUT11).  Roles are
  /// provisional until the enclosure assignment is confirmed (bring-up P9-03):
  /// LED1/LED2 keep the v1 touch-right/left slots, LED3/LED4 form the ring.
  static constexpr LedMap v2() { return {{-1, -1}, {6, 9, 0, 0, 0}, 2, -1, 3, 0}; }
};

enum class LedBrightness : uint8_t {
  Off = 0,
  Dim = 1,
  Mid = 2,
  Bright = 3,
};

enum class TouchLedIntensity : uint8_t {
  Off = 0,
  Dim = 1,
  Bright = 2,
};

enum class TouchPad : uint8_t {
  Select = 0,
  Left = 1,
  Right = 2,
};

/// A single step in a back-LED sequence. Used by back_play() and
/// internally by back_animate().
struct BackStep {
  enum class Effect : uint8_t { Solid, Blink, Breathe, Fade, Chase };
  Effect effect;
  Rgb color;
  uint32_t param_ms; // Hold (Solid), period (Blink/Breathe), duration (Fade), step time (Chase)
};

/// Predefined Go-specific back-LED sequences. Each value maps to an
/// internal constexpr BackStep array resolved at enqueue time.
/// These are convenience wrappers so the orchestrator does not need
/// to know the step details.
enum class BackAnimation : uint8_t {
  Boot = 0,
};
