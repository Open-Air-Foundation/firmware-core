/**
 * AirGradient Go -- LED service implementation
 *
 * Adaptive render loop, back-LED effect engine, touch flash manager,
 * and front direct writes.  See products/go/specs/led_service.md for
 * the full design.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_led.h"

#include "ag_log.h"
#include "aqi.h"

#include <algorithm>
#include <cmath>

static constexpr const char *TAG = "Led";

// ===========================================================================
// Named constants -- no magic numbers
// ===========================================================================

// --- Front LED channels (single-channel PWM) ---
static constexpr uint8_t FRONT_CH_LED25 = 30;
static constexpr uint8_t FRONT_CH_LED26 = 31;

// --- Front PWM levels ---
static constexpr uint8_t FRONT_PWM_OFF = 0;
static constexpr uint8_t FRONT_PWM_DIM = 5;
static constexpr uint8_t FRONT_PWM_MID = 13;
static constexpr uint8_t FRONT_PWM_BRIGHT = 26;

// --- Back LED groups (blue-channel base per RGB group) ---
static constexpr uint8_t NUM_BACK_LEDS = 5;
static constexpr uint8_t BACK_B_CHANNELS[NUM_BACK_LEDS] = {6, 12, 15, 18, 24};

// --- Back brightness scale (output = effect_value * scale / 255) ---
static constexpr uint8_t BACK_SCALE_OFF = 0;
static constexpr uint8_t BACK_SCALE_DIM = 64;
static constexpr uint8_t BACK_SCALE_MID = 128;
static constexpr uint8_t BACK_SCALE_BRIGHT = 255;

// --- Touch LED groups (blue-channel base per RGB group) ---
static constexpr uint8_t TOUCH_CH_SELECT = 27; // LED10: OUT27/28/29
static constexpr uint8_t TOUCH_CH_LEFT = 3;    // LED2: OUT3/4/5
static constexpr uint8_t TOUCH_CH_RIGHT = 0;   // LED1: OUT0/1/2

// --- Touch PWM levels ---
static constexpr uint8_t TOUCH_PWM_OFF = 0;
static constexpr uint8_t TOUCH_PWM_DIM = 64;
static constexpr uint8_t TOUCH_PWM_BRIGHT = 255;

// --- AQI colors ---
static constexpr Rgb AQI_GOOD = {0, 255, 0};
static constexpr Rgb AQI_MODERATE = {255, 255, 0};
static constexpr Rgb AQI_UNHEALTHY_SENSITIVE = {255, 128, 0};
static constexpr Rgb AQI_UNHEALTHY = {255, 0, 0};
static constexpr Rgb AQI_VERY_UNHEALTHY = {128, 0, 128};
static constexpr Rgb AQI_HAZARDOUS = {139, 69, 19};

// --- Predefined animation step arrays ---
static constexpr BackStep BOOT_STEPS[] = {
    {BackStep::Effect::Chase, {255, 255, 255}, 100}, // white fill ~100 ms
    {BackStep::Effect::Solid, {255, 255, 255}, 400}, // hold white 400 ms
};
static constexpr uint8_t BOOT_STEPS_COUNT = sizeof(BOOT_STEPS) / sizeof(BOOT_STEPS[0]);

// --- Math ---
static constexpr float TWO_PI = 6.2831853f;

// ===========================================================================
// Helpers
// ===========================================================================

static bool rgb_eq(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

static uint8_t front_pwm_for(LedBrightness b) {
  switch (b) {
  case LedBrightness::Off:
    return FRONT_PWM_OFF;
  case LedBrightness::Dim:
    return FRONT_PWM_DIM;
  case LedBrightness::Mid:
    return FRONT_PWM_MID;
  case LedBrightness::Bright:
    return FRONT_PWM_BRIGHT;
  }
  return FRONT_PWM_OFF;
}

static uint8_t back_scale_for(LedBrightness b) {
  switch (b) {
  case LedBrightness::Off:
    return BACK_SCALE_OFF;
  case LedBrightness::Dim:
    return BACK_SCALE_DIM;
  case LedBrightness::Mid:
    return BACK_SCALE_MID;
  case LedBrightness::Bright:
    return BACK_SCALE_BRIGHT;
  }
  return BACK_SCALE_OFF;
}

static uint8_t touch_pwm_for(TouchLedIntensity i) {
  switch (i) {
  case TouchLedIntensity::Off:
    return TOUCH_PWM_OFF;
  case TouchLedIntensity::Dim:
    return TOUCH_PWM_DIM;
  case TouchLedIntensity::Bright:
    return TOUCH_PWM_BRIGHT;
  }
  return TOUCH_PWM_OFF;
}

/// Scale a single RGB channel by the brightness factor.
static uint8_t scale_channel(uint8_t value, uint8_t scale) {
  return static_cast<uint8_t>((static_cast<uint16_t>(value) * scale) / 255);
}

static Rgb scale_rgb(Rgb c, uint8_t scale) {
  return {scale_channel(c.r, scale), scale_channel(c.g, scale), scale_channel(c.b, scale)};
}

/// Linear interpolation of a single channel.
static uint8_t lerp_channel(uint8_t a, uint8_t b_val, uint32_t elapsed, uint32_t duration) {
  if (duration == 0 || elapsed >= duration) {
    return b_val;
  }
  int32_t diff = static_cast<int32_t>(b_val) - static_cast<int32_t>(a);
  return static_cast<uint8_t>(a + (diff * static_cast<int32_t>(elapsed)) /
                                      static_cast<int32_t>(duration));
}

static Rgb lerp_rgb(Rgb from, Rgb to, uint32_t elapsed, uint32_t duration) {
  return {lerp_channel(from.r, to.r, elapsed, duration),
          lerp_channel(from.g, to.g, elapsed, duration),
          lerp_channel(from.b, to.b, elapsed, duration)};
}

static Rgb category_to_rgb(aqi::UsAqiCategory cat) {
  switch (cat) {
  case aqi::UsAqiCategory::Good:
    return AQI_GOOD;
  case aqi::UsAqiCategory::Moderate:
    return AQI_MODERATE;
  case aqi::UsAqiCategory::UnhealthySensitive:
    return AQI_UNHEALTHY_SENSITIVE;
  case aqi::UsAqiCategory::Unhealthy:
    return AQI_UNHEALTHY;
  case aqi::UsAqiCategory::VeryUnhealthy:
    return AQI_VERY_UNHEALTHY;
  case aqi::UsAqiCategory::Hazardous:
    return AQI_HAZARDOUS;
  case aqi::UsAqiCategory::Invalid:
    return {0, 0, 0};
  }
  return {0, 0, 0};
}

// step_to_type is a private static method of LedService (see go_led.h)
// because it needs access to the private BackEffectState::Type.

// ===========================================================================
// Construction / Destruction
// ===========================================================================

LedService::LedService(const Config &config) : _config(config) {}

LedService::~LedService() {
  if (_task != nullptr) {
    RTOS::task_delete(_task);
  }
  if (_queue != nullptr) {
    RTOS::queue_delete(_queue);
  }
}

// ===========================================================================
// Lifecycle
// ===========================================================================

bool LedService::init() {
  if (_init_called) {
    return _init_ok;
  }
  _init_called = true;

  if (_is_inert()) {
    _init_ok = true;
    return true;
  }

  if (!_config.driver->init()) {
    AG_LOGE(TAG, "driver init failed");
    _init_ok = false;
    return false;
  }

  _queue = RTOS::queue_create(_config.queue_depth, sizeof(Cmd));
  if (_queue == nullptr) {
    AG_LOGE(TAG, "queue_create failed");
    _init_ok = false;
    return false;
  }

  _init_ok = true;
  return true;
}

bool LedService::start() {
  if (_is_inert()) {
    return true;
  }
  if (!_init_ok) {
    return false;
  }
  if (_started) {
    return true;
  }

  // task_create returns false under TEST_HOST (no real thread); that is
  // expected -- tests drive the service via pump_for_test() instead.
  RTOS::task_create(&LedService::_task_entry, "led", _config.task_stack_size, this,
                    _config.task_priority, &_task);

  _started = true;
  return true;
}

// ===========================================================================
// Public API -- Front
// ===========================================================================

void LedService::front_set_brightness(LedBrightness brightness) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::FrontSetBrightness;
  cmd.brightness = brightness;
  _enqueue(cmd);
}

// ===========================================================================
// Public API -- Back (primitive effects)
// ===========================================================================

void LedService::back_solid(Rgb color) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::BackSolid;
  cmd.color = color;
  _enqueue(cmd);
}

void LedService::back_blink(Rgb color, uint32_t period_ms) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::BackBlink;
  cmd.color = color;
  cmd.param_ms = period_ms;
  _enqueue(cmd);
}

void LedService::back_breathe(Rgb color, uint32_t period_ms) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::BackBreathe;
  cmd.color = color;
  cmd.param_ms = period_ms;
  _enqueue(cmd);
}

void LedService::back_fade_to(Rgb color, uint32_t duration_ms) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::BackFadeTo;
  cmd.color = color;
  cmd.param_ms = duration_ms;
  _enqueue(cmd);
}

void LedService::back_chase(Rgb color, uint32_t step_ms) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::BackChase;
  cmd.color = color;
  cmd.param_ms = step_ms;
  _enqueue(cmd);
}

void LedService::back_off() {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::BackOff;
  _enqueue(cmd);
}

void LedService::back_set_brightness(LedBrightness brightness) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::BackSetBrightness;
  cmd.brightness = brightness;
  _enqueue(cmd);
}

// ===========================================================================
// Public API -- Back (sequences)
// ===========================================================================

void LedService::back_play(const BackStep *steps, uint8_t count) {
  if (steps == nullptr || count == 0) {
    return;
  }
  Cmd cmd{};
  cmd.kind = Cmd::Kind::BackPlay;
  cmd.step_count = std::min(count, MAX_SEQUENCE_STEPS);
  for (uint8_t i = 0; i < cmd.step_count; ++i) {
    cmd.steps[i] = steps[i];
  }
  _enqueue(cmd);
}

void LedService::back_animate(BackAnimation animation) {
  switch (animation) {
  case BackAnimation::Boot:
    back_play(BOOT_STEPS, BOOT_STEPS_COUNT);
    break;
  }
}

void LedService::back_update_aqi(float pm25_ugm3) {
  const int aqi_value = aqi::pm25_to_us_aqi(pm25_ugm3);
  if (aqi_value == aqi::INVALID_AQI) {
    back_clear_aqi();
    return;
  }
  const aqi::UsAqiCategory cat = aqi::us_aqi_to_category(aqi_value);
  if (cat == aqi::UsAqiCategory::Invalid) {
    back_clear_aqi();
    return;
  }
  back_solid(category_to_rgb(cat));
}

void LedService::back_clear_aqi() { back_off(); }

// ===========================================================================
// Public API -- Touch
// ===========================================================================

void LedService::touch_flash(TouchPad pad) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::TouchFlash;
  cmd.pad = pad;
  _enqueue(cmd);
}

void LedService::touch_set_all(bool on) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::TouchSetAll;
  cmd.on = on;
  _enqueue(cmd);
}

void LedService::touch_set_intensity(TouchLedIntensity intensity) {
  Cmd cmd{};
  cmd.kind = Cmd::Kind::TouchSetIntensity;
  cmd.intensity = intensity;
  _enqueue(cmd);
}

// ===========================================================================
// Internal -- Inert mode check
// ===========================================================================

bool LedService::_is_inert() const { return _config.driver == nullptr; }

// ===========================================================================
// Internal -- Enqueue
// ===========================================================================

void LedService::_enqueue(const Cmd &cmd) {
  if (_is_inert() || !_init_ok || !_started) {
    return;
  }
  RTOS::queue_send(_queue, &cmd, 0);
}

// ===========================================================================
// Internal -- Command processing
// ===========================================================================

void LedService::_process_cmd(const Cmd &cmd, uint32_t now_ms) {
  switch (cmd.kind) {

  case Cmd::Kind::FrontSetBrightness:
    _front_brightness = cmd.brightness;
    _front_dirty = true;
    break;

  case Cmd::Kind::BackSolid:
    _clear_saved_effect();
    _start_back_effect(BackEffectState::Type::Solid, cmd.color, 0, now_ms);
    _last_rendered_back = cmd.color;
    break;

  case Cmd::Kind::BackBlink:
    _clear_saved_effect();
    if (cmd.param_ms == 0) {
      // Zero period: treated as solid (spec)
      _start_back_effect(BackEffectState::Type::Solid, cmd.color, 0, now_ms);
      _last_rendered_back = cmd.color;
    } else {
      _start_back_effect(BackEffectState::Type::Blink, cmd.color, cmd.param_ms, now_ms);
      _last_rendered_back = cmd.color; // on at t=0 (first half is on)
    }
    break;

  case Cmd::Kind::BackBreathe:
    _clear_saved_effect();
    if (cmd.param_ms == 0) {
      _start_back_effect(BackEffectState::Type::Solid, cmd.color, 0, now_ms);
      _last_rendered_back = cmd.color;
    } else {
      _start_back_effect(BackEffectState::Type::Breathe, cmd.color, cmd.param_ms, now_ms);
      _last_rendered_back = cmd.color; // 100% at t=0
    }
    break;

  case Cmd::Kind::BackFadeTo: {
    _clear_saved_effect();
    Rgb captured_from = _last_rendered_back;
    _back_effect = BackEffectState{};
    _back_effect.type = BackEffectState::Type::Fade;
    _back_effect.color = cmd.color;
    _back_effect.param_ms = cmd.param_ms;
    _back_effect.started_at_ms = now_ms;
    _back_effect.fade_from = captured_from;
    // At t=0 with duration>0, output is fade_from; with duration=0, output is color.
    if (cmd.param_ms == 0) {
      _last_rendered_back = cmd.color;
    }
    // else _last_rendered_back stays as captured_from (correct at t=0)
    _back_dirty = true;
    break;
  }

  case Cmd::Kind::BackChase:
    _clear_saved_effect();
    if (cmd.param_ms == 0) {
      // Zero step time: all LEDs immediately lit (spec)
      _start_back_effect(BackEffectState::Type::Solid, cmd.color, 0, now_ms);
      _last_rendered_back = cmd.color;
    } else {
      _start_back_effect(BackEffectState::Type::Chase, cmd.color, cmd.param_ms, now_ms);
      _last_rendered_back = cmd.color; // base color for uniform tracking
    }
    break;

  case Cmd::Kind::BackOff:
    _clear_saved_effect();
    _start_back_effect(BackEffectState::Type::Off, {}, 0, now_ms);
    _last_rendered_back = {0, 0, 0};
    break;

  case Cmd::Kind::BackSetBrightness: {
    LedBrightness old = _back_brightness;
    _back_brightness = cmd.brightness;
    if (old != _back_brightness) {
      _back_dirty = true;
    }
    // Does NOT clear saved effect (brightness is orthogonal)
    break;
  }

  case Cmd::Kind::BackPlay: {
    // Save current effect if static and no prior saved effect
    if (!_has_saved_back_effect && _is_back_static()) {
      _saved_back_effect = _back_effect;
      _has_saved_back_effect = true;
    }
    // If _has_saved_back_effect is already true (active sequence), the new
    // sequence inherits the original restore target.

    // Set up sequence
    _back_effect.type = BackEffectState::Type::Sequence;
    _back_effect.step_count = cmd.step_count;
    _back_effect.current_step = 0;
    for (uint8_t i = 0; i < cmd.step_count; ++i) {
      _back_effect.steps[i] = cmd.steps[i];
    }

    // Set up the first step as the active primitive
    if (cmd.step_count > 0) {
      const BackStep &first = _back_effect.steps[0];
      _back_effect.color = first.color;
      _back_effect.param_ms = first.param_ms;
      _back_effect.started_at_ms = now_ms;

      // Capture fade_from for Fade steps
      if (first.effect == BackStep::Effect::Fade) {
        _back_effect.fade_from = _last_rendered_back;
      }

      // Update _last_rendered_back with the initial frame of the first step
      _last_rendered_back = _compute_primitive_frame(_step_to_type(first.effect), first.color,
                                                     first.param_ms, _back_effect.fade_from, 0);
    }
    _back_dirty = true;
    break;
  }

  case Cmd::Kind::TouchFlash: {
    if (_touch_intensity == TouchLedIntensity::Off) {
      return;
    }
    // Turn off previous pad (if any) before activating new one
    if (_touch_active) {
      _touch_active = false;
      _touch_dirty = true;
      _render_touch(); // immediate write: old pad off
    }
    _touch_active_pad = cmd.pad;
    _touch_active = true;
    _touch_started_at_ms = now_ms;
    _touch_dirty = true;
    break;
  }

  case Cmd::Kind::TouchSetAll: {
    // Steady all-pads on/off (hardware test). Independent of flash state.
    _touch_steady = cmd.on;
    _touch_dirty = true;
    break;
  }

  case Cmd::Kind::TouchSetIntensity: {
    TouchLedIntensity old = _touch_intensity;
    _touch_intensity = cmd.intensity;
    if (cmd.intensity == TouchLedIntensity::Off && _touch_active) {
      // Suppress active flash and cancel off-edge
      _touch_active = false;
      _touch_started_at_ms = 0;
      _touch_dirty = true;
    } else if (old != cmd.intensity && (_touch_active || _touch_steady)) {
      // Re-render at the new intensity (covers a steady all-pads test).
      _touch_dirty = true;
    }
    break;
  }
  }
}

// ===========================================================================
// Internal -- Back effect helpers
// ===========================================================================

void LedService::_start_back_effect(BackEffectState::Type type, Rgb color, uint32_t param_ms,
                                    uint32_t now_ms) {
  _back_effect = BackEffectState{};
  _back_effect.type = type;
  _back_effect.color = color;
  _back_effect.param_ms = param_ms;
  _back_effect.started_at_ms = now_ms;
  _back_dirty = true;
}

void LedService::_clear_saved_effect() { _has_saved_back_effect = false; }

LedService::BackEffectState::Type LedService::_step_to_type(BackStep::Effect e) {
  using T = BackEffectState::Type;
  switch (e) {
  case BackStep::Effect::Solid:
    return T::Solid;
  case BackStep::Effect::Blink:
    return T::Blink;
  case BackStep::Effect::Breathe:
    return T::Breathe;
  case BackStep::Effect::Fade:
    return T::Fade;
  case BackStep::Effect::Chase:
    return T::Chase;
  }
  return T::Solid;
}

bool LedService::_is_back_static() const {
  switch (_back_effect.type) {
  case BackEffectState::Type::Off:
  case BackEffectState::Type::Solid:
    return true;
  case BackEffectState::Type::Blink:
  case BackEffectState::Type::Breathe:
    return false;
  case BackEffectState::Type::Fade: {
    if (_back_effect.param_ms == 0) {
      return true;
    }
    uint32_t elapsed = _now_ms - _back_effect.started_at_ms;
    return elapsed >= _back_effect.param_ms;
  }
  case BackEffectState::Type::Chase: {
    if (_back_effect.param_ms == 0) {
      return true;
    }
    uint32_t elapsed = _now_ms - _back_effect.started_at_ms;
    uint32_t total = static_cast<uint32_t>(NUM_BACK_LEDS) * _back_effect.param_ms;
    return elapsed >= total;
  }
  case BackEffectState::Type::Sequence:
    return false;
  }
  return true;
}

uint32_t LedService::_sequence_step_duration(const BackStep &step) const {
  switch (step.effect) {
  case BackStep::Effect::Solid:
    return step.param_ms;
  case BackStep::Effect::Blink:
    return step.param_ms;
  case BackStep::Effect::Breathe:
    return step.param_ms;
  case BackStep::Effect::Fade:
    return step.param_ms;
  case BackStep::Effect::Chase:
    return static_cast<uint32_t>(NUM_BACK_LEDS) * step.param_ms;
  }
  return 0;
}

// ===========================================================================
// Internal -- Back effect tick
// ===========================================================================

void LedService::_tick_back(uint32_t now_ms) {
  if (_back_effect.type == BackEffectState::Type::Off ||
      _back_effect.type == BackEffectState::Type::Solid) {
    return; // static -- no ticking needed
  }

  uint32_t elapsed = now_ms - _back_effect.started_at_ms;

  if (_back_effect.type == BackEffectState::Type::Sequence) {
    if (_back_effect.current_step >= _back_effect.step_count) {
      _apply_auto_restore(now_ms);
      return;
    }
    const BackStep &step = _back_effect.steps[_back_effect.current_step];
    uint32_t step_dur = _sequence_step_duration(step);
    if (elapsed >= step_dur) {
      _advance_sequence(now_ms);
      return;
    }
    // Step still active -- compute frame
    Rgb frame = _compute_back_frame(now_ms);
    if (step.effect == BackStep::Effect::Chase) {
      // Chase needs per-LED rendering every tick
      _back_dirty = true;
    } else if (!rgb_eq(frame, _last_rendered_back)) {
      _last_rendered_back = frame;
      _back_dirty = true;
    }
    return;
  }

  if (_back_effect.type == BackEffectState::Type::Fade) {
    if (_back_effect.param_ms == 0 || elapsed >= _back_effect.param_ms) {
      // Fade complete -- transition to Solid so subsequent ticks are no-ops
      Rgb final_color = _back_effect.color;
      _back_effect.type = BackEffectState::Type::Solid;
      _back_effect.color = final_color;
      if (!rgb_eq(_last_rendered_back, final_color)) {
        _last_rendered_back = final_color;
        _back_dirty = true;
      }
      return;
    }
  }

  if (_back_effect.type == BackEffectState::Type::Chase) {
    uint32_t total = static_cast<uint32_t>(NUM_BACK_LEDS) * _back_effect.param_ms;
    if (elapsed >= total) {
      // Chase complete -- transition to Solid so subsequent ticks are no-ops
      Rgb final_color = _back_effect.color;
      _back_effect.type = BackEffectState::Type::Solid;
      _back_effect.color = final_color;
      _last_rendered_back = final_color;
      _back_dirty = true;
      return;
    }
    // Chase animating -- per-LED rendering each tick
    _back_dirty = true;
    return;
  }

  // Blink / Breathe -- looping, always compute frame
  Rgb frame = _compute_back_frame(now_ms);
  if (!rgb_eq(frame, _last_rendered_back)) {
    _last_rendered_back = frame;
    _back_dirty = true;
  }
}

void LedService::_advance_sequence(uint32_t now_ms) {
  _back_effect.current_step++;

  if (_back_effect.current_step >= _back_effect.step_count) {
    _apply_auto_restore(now_ms);
    return;
  }

  const BackStep &next = _back_effect.steps[_back_effect.current_step];
  _back_effect.color = next.color;
  _back_effect.param_ms = next.param_ms;
  _back_effect.started_at_ms = now_ms;

  if (next.effect == BackStep::Effect::Fade) {
    _back_effect.fade_from = _last_rendered_back;
  }

  // Update _last_rendered_back for the initial frame of the new step
  _last_rendered_back = _compute_primitive_frame(_step_to_type(next.effect), next.color,
                                                 next.param_ms, _back_effect.fade_from, 0);
  _back_dirty = true;
}

void LedService::_apply_auto_restore(uint32_t now_ms) {
  if (_has_saved_back_effect) {
    _back_effect = _saved_back_effect;
    _has_saved_back_effect = false;
    _back_effect.started_at_ms = now_ms;
    // Compute the initial frame of the restored effect
    _last_rendered_back = _compute_back_frame(now_ms);
    _back_dirty = true;
  } else {
    // Hold final frame -- become Solid at the last rendered color
    _back_effect.type = BackEffectState::Type::Solid;
    _back_effect.color = _last_rendered_back;
    _back_effect.param_ms = 0;
    _back_dirty = true;
  }
}

// ===========================================================================
// Internal -- Frame computation
// ===========================================================================

Rgb LedService::_compute_back_frame(uint32_t now_ms) {
  if (_back_effect.type == BackEffectState::Type::Sequence) {
    if (_back_effect.current_step < _back_effect.step_count) {
      const BackStep &step = _back_effect.steps[_back_effect.current_step];
      uint32_t elapsed = now_ms - _back_effect.started_at_ms;
      return _compute_primitive_frame(_step_to_type(step.effect), _back_effect.color,
                                      _back_effect.param_ms, _back_effect.fade_from, elapsed);
    }
    return _last_rendered_back;
  }

  uint32_t elapsed = now_ms - _back_effect.started_at_ms;
  return _compute_primitive_frame(_back_effect.type, _back_effect.color, _back_effect.param_ms,
                                  _back_effect.fade_from, elapsed);
}

Rgb LedService::_compute_primitive_frame(BackEffectState::Type type, Rgb color, uint32_t param_ms,
                                         Rgb fade_from, uint32_t elapsed_ms) const {
  switch (type) {
  case BackEffectState::Type::Off:
    return {0, 0, 0};

  case BackEffectState::Type::Solid:
    return color;

  case BackEffectState::Type::Blink: {
    if (param_ms == 0) {
      return color;
    }
    uint32_t phase = elapsed_ms % param_ms;
    bool on = phase < (param_ms / 2);
    return on ? color : Rgb{0, 0, 0};
  }

  case BackEffectState::Type::Breathe: {
    if (param_ms == 0) {
      return color;
    }
    float t = static_cast<float>(elapsed_ms % param_ms) / static_cast<float>(param_ms);
    float factor = (1.0f + cosf(TWO_PI * t)) / 2.0f;
    return {static_cast<uint8_t>(static_cast<float>(color.r) * factor),
            static_cast<uint8_t>(static_cast<float>(color.g) * factor),
            static_cast<uint8_t>(static_cast<float>(color.b) * factor)};
  }

  case BackEffectState::Type::Fade:
    return lerp_rgb(fade_from, color, elapsed_ms, param_ms);

  case BackEffectState::Type::Chase:
    // Chase returns base color (uniform representation for _last_rendered_back).
    // Actual per-LED rendering happens in _render_back().
    return color;

  case BackEffectState::Type::Sequence:
    // Sequences delegate to primitive via _compute_back_frame(); should not reach here.
    return _last_rendered_back;
  }
  return {0, 0, 0};
}

bool LedService::_is_primitive_done(BackEffectState::Type type, uint32_t param_ms,
                                    uint32_t elapsed_ms) const {
  switch (type) {
  case BackEffectState::Type::Off:
  case BackEffectState::Type::Solid:
    return elapsed_ms >= param_ms;
  case BackEffectState::Type::Blink:
  case BackEffectState::Type::Breathe:
    return elapsed_ms >= param_ms; // one cycle
  case BackEffectState::Type::Fade:
    return param_ms == 0 || elapsed_ms >= param_ms;
  case BackEffectState::Type::Chase:
    return elapsed_ms >= static_cast<uint32_t>(NUM_BACK_LEDS) * param_ms;
  case BackEffectState::Type::Sequence:
    return false;
  }
  return true;
}

// ===========================================================================
// Internal -- Touch tick
// ===========================================================================

void LedService::_tick_touch(uint32_t now_ms) {
  if (_touch_active && (now_ms - _touch_started_at_ms) >= _config.touch_flash_ms) {
    _touch_active = false;
    _touch_started_at_ms = 0;
    _touch_dirty = true;
  }
}

uint32_t LedService::_next_wait_timeout_ms(uint32_t now_ms) const {
  if (!_is_back_static()) {
    return _config.frame_interval_ms;
  }
  if (!_touch_active) {
    return UINT32_MAX;
  }

  const uint32_t elapsed_ms = now_ms - _touch_started_at_ms;
  if (elapsed_ms >= _config.touch_flash_ms) {
    return 0;
  }
  return _config.touch_flash_ms - elapsed_ms;
}

// ===========================================================================
// Internal -- Render
// ===========================================================================

void LedService::_render_front() {
  if (!_front_dirty) {
    return;
  }
  _front_dirty = false;

  uint8_t pwm = front_pwm_for(_front_brightness);
  bool ok = _config.driver->set_channel(FRONT_CH_LED25, pwm);
  ok = _config.driver->set_channel(FRONT_CH_LED26, pwm) && ok;

  if (!ok && !_driver_error_logged) {
    AG_LOGW(TAG, "front write failed");
    _driver_error_logged = true;
  } else if (ok && _driver_error_logged) {
    AG_LOGI(TAG, "driver recovered");
    _driver_error_logged = false;
  }
}

void LedService::_render_back() {
  if (!_back_dirty) {
    return;
  }
  _back_dirty = false;

  uint8_t scale = back_scale_for(_back_brightness);
  bool ok = true;

  // Determine if we need per-LED chase rendering
  bool chase_active = false;
  if (_back_effect.type == BackEffectState::Type::Chase) {
    chase_active = true;
  } else if (_back_effect.type == BackEffectState::Type::Sequence &&
             _back_effect.current_step < _back_effect.step_count) {
    chase_active = _back_effect.steps[_back_effect.current_step].effect == BackStep::Effect::Chase;
  }

  if (chase_active) {
    uint32_t elapsed = _now_ms - _back_effect.started_at_ms;
    for (uint8_t i = 0; i < NUM_BACK_LEDS; ++i) {
      uint32_t threshold = static_cast<uint32_t>(i) * _back_effect.param_ms;
      Rgb led_color;
      if (_back_effect.param_ms == 0 || elapsed >= threshold) {
        led_color = scale_rgb(_back_effect.color, scale);
      } else {
        led_color = {0, 0, 0};
      }
      ok = _config.driver->set_rgb(BACK_B_CHANNELS[i], led_color.r, led_color.g, led_color.b) && ok;
    }
  } else {
    // Uniform: all 5 LEDs same color
    Rgb scaled = scale_rgb(_last_rendered_back, scale);
    for (uint8_t i = 0; i < NUM_BACK_LEDS; ++i) {
      ok = _config.driver->set_rgb(BACK_B_CHANNELS[i], scaled.r, scaled.g, scaled.b) && ok;
    }
  }

  if (!ok && !_driver_error_logged) {
    AG_LOGW(TAG, "back write failed");
    _driver_error_logged = true;
  } else if (ok && _driver_error_logged) {
    AG_LOGI(TAG, "driver recovered");
    _driver_error_logged = false;
  }
}

void LedService::_render_touch() {
  if (!_touch_dirty) {
    return;
  }
  _touch_dirty = false;

  // Write all three touch pads: active pad gets PWM, others get off.
  static constexpr uint8_t ALL_TOUCH_CHANNELS[] = {TOUCH_CH_SELECT, TOUCH_CH_LEFT, TOUCH_CH_RIGHT};
  static constexpr TouchPad ALL_TOUCH_PADS[] = {TouchPad::Select, TouchPad::Left, TouchPad::Right};

  bool ok = true;
  for (uint8_t i = 0; i < 3; ++i) {
    // Lit when the steady test mode is on, or this is the active flash pad.
    const bool lit = _touch_steady || (_touch_active && ALL_TOUCH_PADS[i] == _touch_active_pad);
    if (lit) {
      uint8_t pwm = touch_pwm_for(_touch_intensity);
      ok = _config.driver->set_rgb(ALL_TOUCH_CHANNELS[i], pwm, pwm, pwm) && ok;
    } else {
      ok = _config.driver->set_rgb(ALL_TOUCH_CHANNELS[i], 0, 0, 0) && ok;
    }
  }

  if (!ok && !_driver_error_logged) {
    AG_LOGW(TAG, "touch write failed");
    _driver_error_logged = true;
  } else if (ok && _driver_error_logged) {
    AG_LOGI(TAG, "driver recovered");
    _driver_error_logged = false;
  }
}

// ===========================================================================
// Worker
// ===========================================================================

void LedService::_task_entry(void *arg) { static_cast<LedService *>(arg)->_run(); }

void LedService::_run() {
  while (true) {
    _now_ms = RTOS::get_time_ms();
    const uint32_t timeout_ms = _next_wait_timeout_ms(_now_ms);

    Cmd cmd{};
    bool got_cmd = RTOS::queue_receive(_queue, &cmd, timeout_ms);

    _now_ms = RTOS::get_time_ms();

    if (got_cmd) {
      _process_cmd(cmd, _now_ms);
    }

    _tick_back(_now_ms);
    _tick_touch(_now_ms);
    _render_front();
    _render_back();
    _render_touch();
  }
}

// ===========================================================================
// Test pump -- synchronous driver for host tests
// ===========================================================================

#ifdef TEST_HOST

void LedService::pump_for_test(uint32_t now_ms) {
  if (_is_inert() || !_init_ok) {
    return;
  }

  _now_ms = now_ms;

  // Drain all queued commands
  Cmd cmd{};
  while (RTOS::queue_receive(_queue, &cmd, 0)) {
    _process_cmd(cmd, now_ms);
  }

  _tick_back(now_ms);
  _tick_touch(now_ms);
  _render_front();
  _render_back();
  _render_touch();
}

#endif // TEST_HOST
