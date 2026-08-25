/**
 * AirGradient Go -- LED service declaration
 *
 * Drives three LED groups (front, back, touch) through an abstract
 * LedDriver surface.  Uses an adaptive render loop that sleeps when
 * all LEDs are static and ticks at frame rate only when an animation
 * is active.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "go_led_hal.h"
#include "go_led_types.h"
#include "rtos.h"

#include <cstdint>
#include <optional>

class LedService {
public:
  struct Config {
    LedDriver *driver = nullptr;
    uint16_t task_stack_size = 2048;
    uint8_t task_priority = 3;
    uint8_t queue_depth = 8;
    uint32_t touch_flash_ms = 120;
    uint32_t frame_interval_ms = 30; // ~33 fps when animating
  };

  explicit LedService(const Config &config);
  ~LedService();

  LedService(const LedService &) = delete;
  LedService &operator=(const LedService &) = delete;

  bool init();
  bool start();

  // --- Front (static) ---
  void front_set_brightness(LedBrightness brightness);

  // --- Back (animated) ---
  void back_solid(Rgb color);
  void back_blink(Rgb color, uint32_t period_ms);
  void back_breathe(Rgb color, uint32_t period_ms);
  void back_fade_to(Rgb color, uint32_t duration_ms);
  void back_chase(Rgb color, uint32_t step_ms);
  void back_off();
  void back_set_brightness(LedBrightness brightness);

  // --- Back (sequences) ---

  /// Play a transient sequence of up to MAX_SEQUENCE_STEPS sub-effects.
  /// Each step runs to completion, then the next starts.  When the
  /// sequence finishes, the service auto-restores the previous back
  /// effect if it was static.  See "Auto-Restore" in the spec.
  static constexpr uint8_t MAX_SEQUENCE_STEPS = 6;
  void back_play(const BackStep *steps, uint8_t count);

  /// Play a predefined Go-specific animation.  Resolves the enum to a
  /// constexpr BackStep array on the caller side, then enqueues via
  /// back_play().
  void back_animate(BackAnimation animation);

  /// Convenience: resolves PM2.5 -> AQI -> category -> Rgb, then
  /// enqueues back_solid() with the resolved color.
  void back_update_aqi(float pm25_ugm3);

  /// Convenience: enqueues back_off().  Use when a PM2.5 reading fails
  /// validation so the back LEDs do not show a stale color.
  void back_clear_aqi();

  // --- Touch ---
  /// Momentary flash of a single pad for Config::touch_flash_ms.
  void touch_flash(TouchPad pad);
  /// Light (or clear) all three touch pads steadily at the current intensity,
  /// with no auto-off. Used by the hardware peripheral test so an operator can
  /// visually confirm the touch LEDs. Independent of touch_flash().
  void touch_set_all(bool on);
  void touch_set_intensity(TouchLedIntensity intensity);

private:
  // -----------------------------------------------------------------------
  // Command struct -- flat layout, no union
  // -----------------------------------------------------------------------

  struct Cmd {
    enum class Kind : uint8_t {
      FrontSetBrightness,
      BackSolid,
      BackBlink,
      BackBreathe,
      BackFadeTo,
      BackChase,
      BackOff,
      BackSetBrightness,
      BackPlay,
      TouchFlash,
      TouchSetAll,
      TouchSetIntensity,
    };

    Kind kind;
    Rgb color;
    uint32_t param_ms = 0;
    LedBrightness brightness{};
    BackStep steps[MAX_SEQUENCE_STEPS]{};
    uint8_t step_count = 0;
    TouchPad pad{};
    TouchLedIntensity intensity{};
    bool on = false; // TouchSetAll: steady on/off
  };

  // -----------------------------------------------------------------------
  // Back effect state -- tagged-union state machine
  // -----------------------------------------------------------------------

  struct BackEffectState {
    enum class Type : uint8_t { Off, Solid, Blink, Breathe, Fade, Chase, Sequence };
    Type type = Type::Off;

    Rgb color;
    uint32_t param_ms = 0;
    Rgb fade_from;

    uint32_t started_at_ms = 0;

    // Sequence fields (only used when type == Sequence)
    BackStep steps[MAX_SEQUENCE_STEPS];
    uint8_t step_count = 0;
    uint8_t current_step = 0;
  };

  // -----------------------------------------------------------------------
  // Internal helpers
  // -----------------------------------------------------------------------

  bool _is_inert() const;
  void _enqueue(const Cmd &cmd);

  void _process_cmd(const Cmd &cmd, uint32_t now_ms);
  void _tick_back(uint32_t now_ms);
  void _tick_touch(uint32_t now_ms);
  void _render_front();
  void _render_back();
  void _render_touch();
  bool _is_back_static() const;
  uint32_t _next_wait_timeout_ms(uint32_t now_ms) const;

  Rgb _compute_back_frame(uint32_t now_ms);
  Rgb _compute_primitive_frame(BackEffectState::Type type, Rgb color, uint32_t param_ms,
                               Rgb fade_from, uint32_t elapsed_ms) const;
  bool _is_primitive_done(BackEffectState::Type type, uint32_t param_ms, uint32_t elapsed_ms) const;
  uint32_t _sequence_step_duration(const BackStep &step) const;
  void _advance_sequence(uint32_t now_ms);
  void _apply_auto_restore(uint32_t now_ms);

  void _start_back_effect(BackEffectState::Type type, Rgb color, uint32_t param_ms,
                          uint32_t now_ms);
  void _clear_saved_effect();
  static BackEffectState::Type _step_to_type(BackStep::Effect e);

  // -----------------------------------------------------------------------
  // Worker
  // -----------------------------------------------------------------------

  static void _task_entry(void *arg);
  void _run();

  // -----------------------------------------------------------------------
  // State
  // -----------------------------------------------------------------------

  Config _config;

  // Lifecycle
  bool _init_ok = false;
  bool _init_called = false;
  bool _started = false;

  // Worker-owned state
  uint32_t _now_ms = 0; // Current time snapshot for this processing cycle
  LedBrightness _front_brightness = LedBrightness::Off;
  BackEffectState _back_effect;
  LedBrightness _back_brightness = LedBrightness::Off;
  TouchLedIntensity _touch_intensity = TouchLedIntensity::Off;
  TouchPad _touch_active_pad{};
  bool _touch_active = false;
  bool _touch_steady = false; // all pads lit steadily (test), independent of flash
  uint32_t _touch_started_at_ms = 0;
  Rgb _last_rendered_back;
  std::optional<Rgb> _uniform_back_output;
  BackEffectState _saved_back_effect;
  bool _has_saved_back_effect = false;

  // Dirty flags
  bool _front_dirty = false;
  bool _back_dirty = false;
  bool _touch_dirty = false;

  // Driver error tracking (edge-triggered logging)
  bool _driver_error_logged = false;

  RtosQueueHandle _queue = nullptr;
  RtosTaskHandle _task = nullptr;

  // -----------------------------------------------------------------------
  // Test access
  // -----------------------------------------------------------------------

#ifdef TEST_HOST
  friend class LedServiceTestAccess;

public:
  void pump_for_test(uint32_t now_ms);
#endif
};
