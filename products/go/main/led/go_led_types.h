/**
 * AirGradient Go -- LED public types
 *
 * Product-specific types shared between the LED service, orchestrator,
 * and UI layers. Uses the common RGB value type.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>

#include "rgb.h"

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
