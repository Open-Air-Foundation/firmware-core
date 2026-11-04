/**
 * AirGradient Go — Input Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_input.h"

#include "ag_log.h"
#include "go_events.h"
#include "rtos.h"

#include <algorithm>

static constexpr const char *TAG = "InputService";

// Depth of the internal ISR-to-task raw event queue.  Deep enough to absorb
// a burst of touch + button events without dropping; shallow enough to stay
// in DRAM budget.
static constexpr uint8_t RAW_QUEUE_DEPTH = 8;

// GPIO level when a physical button is pressed (active-low with pull-up).
static constexpr int BUTTON_PRESSED_LEVEL = 0;

// wake-press suppression deadline
static constexpr uint32_t SUPPRESS_WINDOW_MS = 500;

// ---------------------------------------------------------------------------
// Internal raw event (file-local; not exposed in the header)
// ---------------------------------------------------------------------------

// Minimal struct posted from ISR context to the task queue.
// Must be trivially copyable; no heap, no strings.
//
// For touch events, source is set to InputSource::TouchEnter as a sentinel
// (actual channel is determined by reading CAP1203 in task context).
// For physical buttons, source is ButtonPower or ButtonBoot.
//
// timestamp_ms is populated in task context after dequeuing (not in ISR) to
// avoid direct hardware-timer reads from interrupt context.
struct RawInputEvent {
  InputSource source;    // which input triggered
  uint64_t timestamp_ms; // filled in task context; 0 when posted from ISR
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

InputService::InputService(CapTouchSensor &touch, const gpio::Hal &gpio,
                           RtosQueueHandle event_queue, const Config &config)
    : _touch(touch), _gpio(gpio), _event_queue(event_queue), _config(config), _raw_queue(nullptr),
      _suppress_next_power_press(config.suppress_button_wake) {}

InputService::~InputService() {
  if (_running) {
    stop();
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool InputService::start() {
  _raw_queue = RTOS::queue_create(RAW_QUEUE_DEPTH, sizeof(RawInputEvent));
  if (_raw_queue == nullptr) {
    return false;
  }

  _running = true;
  const bool ok =
      RTOS::task_create(task_entry, "input_task", static_cast<uint32_t>(_config.task_stack_size),
                        this, static_cast<uint32_t>(_config.task_priority), &_task_handle);
  if (!ok) {
    _running = false;
    RTOS::queue_delete(_raw_queue);
    _raw_queue = nullptr;
    return false;
  }
  return true;
}

void InputService::stop() {
  _running = false;

  // Post a dummy event to unblock queue_receive so the task can observe
  // _running == false and exit cleanly.
  if (_raw_queue != nullptr) {
    RawInputEvent dummy{InputSource::TouchEnter, 0};
    RTOS::queue_send(_raw_queue, &dummy, 0);
  }

  // Block until the task signals completion via semaphore, then clean up.
  // The task self-deletes after giving the semaphore, so no external
  // task_delete is needed.
  if (_task_handle != nullptr && _done_sem.is_created()) {
    _done_sem.take();
    _done_sem.destroy();
    _task_handle = nullptr;
  }

  // Unregister ISR handlers.
  _gpio.disable_interrupt(_config.pin_cap_int);
  _gpio.remove_interrupt_handler(_config.pin_cap_int);
  _gpio.disable_interrupt(_config.pin_button_power);
  _gpio.remove_interrupt_handler(_config.pin_button_power);
  _gpio.disable_interrupt(_config.pin_button_boot);
  _gpio.remove_interrupt_handler(_config.pin_button_boot);

  if (_raw_queue != nullptr) {
    RTOS::queue_delete(_raw_queue);
    _raw_queue = nullptr;
  }
}

// ---------------------------------------------------------------------------
// Task entry point (static)
// ---------------------------------------------------------------------------

// static
void InputService::task_entry(void *arg) {
  static_cast<InputService *>(arg)->run();
  RTOS::task_delete(nullptr);
}

// ---------------------------------------------------------------------------
// ISR handlers (static, minimal)
// ---------------------------------------------------------------------------

// Called from ISR when CAP1203 INT line goes low (any touch channel active).
// static
void InputService::cap_int_isr(void *arg) {
  InputService *self = static_cast<InputService *>(arg);
  if (self->_raw_queue == nullptr) {
    return;
  }
  // Use TouchEnter as a sentinel: actual channel is read in task context.
  RawInputEvent ev{InputSource::TouchEnter, 0};
  RTOS::queue_send_from_isr(self->_raw_queue, &ev);
}

// static
void InputService::button_power_isr(void *arg) {
  InputService *self = static_cast<InputService *>(arg);
  if (self->_raw_queue == nullptr) {
    return;
  }
  RawInputEvent ev{InputSource::ButtonPower, 0};
  RTOS::queue_send_from_isr(self->_raw_queue, &ev);
}

// static
void InputService::button_boot_isr(void *arg) {
  InputService *self = static_cast<InputService *>(arg);
  if (self->_raw_queue == nullptr) {
    return;
  }
  RawInputEvent ev{InputSource::ButtonBoot, 0};
  RTOS::queue_send_from_isr(self->_raw_queue, &ev);
}

// ---------------------------------------------------------------------------
// Task loop
// ---------------------------------------------------------------------------

void InputService::run() {
  // Create the done semaphore inside the task so it is valid for the entire
  // task lifetime.  stop() blocks on this semaphore before returning.
  _done_sem.create();

  // Configure GPIO pins as inputs with pull-ups, falling-edge interrupts.
  _gpio.configure(_config.pin_cap_int, gpio::Mode::Input, gpio::PullMode::PullUp,
                  gpio::InterruptType::FallingEdge);
  _gpio.configure(_config.pin_button_power, gpio::Mode::Input, gpio::PullMode::PullUp,
                  gpio::InterruptType::AnyEdge);
  _gpio.configure(_config.pin_button_boot, gpio::Mode::Input, gpio::PullMode::PullUp,
                  gpio::InterruptType::AnyEdge);

  // Register and enable ISR handlers.
  _gpio.add_interrupt_handler(_config.pin_cap_int, cap_int_isr, this);
  _gpio.add_interrupt_handler(_config.pin_button_power, button_power_isr, this);
  _gpio.add_interrupt_handler(_config.pin_button_boot, button_boot_isr, this);
  _gpio.enable_interrupt(_config.pin_cap_int);
  _gpio.enable_interrupt(_config.pin_button_power);
  _gpio.enable_interrupt(_config.pin_button_boot);

  // Arm the wake-press suppression deadline now that ISRs are live
  if (_suppress_next_power_press) {
    _suppress_deadline_ms = RTOS::get_time_ms() + SUPPRESS_WINDOW_MS;
  }

  // Clear any pending touch interrupt. A touch or noise event may have
  // asserted INT (LOW) before the falling-edge ISR was registered, so that
  // edge was missed and INT is stuck LOW.
  _touch.clear_interrupt();

  _last_touch_check_ms = RTOS::get_time_ms();

  RawInputEvent raw{};
  while (_running) {
    // Block with a dynamic timeout so we wake up when a long-press timer
    // expires even without a new event arriving.
    const uint32_t timeout_ms = compute_queue_timeout_ms();
    if (RTOS::queue_receive(_raw_queue, &raw, timeout_ms)) {
      // Timestamp is recorded here (task context) rather than in ISR to avoid
      // direct hardware-timer reads from interrupt context.
      raw.timestamp_ms = RTOS::get_time_ms();

      switch (raw.source) {
      case InputSource::TouchEnter:
        // TouchEnter is the touch sentinel; actual channel determined by
        // reading CAP1203.
        process_touch_interrupt();
        break;

      case InputSource::ButtonPower:
      case InputSource::ButtonBoot:
        process_button_event(raw.source, raw.timestamp_ms);
        break;

      default:
        break;
      }
    }

    // Always check pending long-press timers (covers both the timeout and the
    // event-received paths — a button release does not generate an interrupt).
    check_pending_long_press();

    // Resolve pending CH1 touch gestures (long-press threshold, double-tap
    // window) — timer-driven, independent of new interrupts arriving.
    check_pending_touch_gesture();

    // Periodic touch health check: detect and recover from a stuck INT line.
    const uint64_t now_ms = RTOS::get_time_ms();
    if ((now_ms - _last_touch_check_ms) >= static_cast<uint64_t>(_config.touch_watchdog_ms)) {
      _last_touch_check_ms = now_ms;
      check_touch_health();
    }
  }

  // Signal stop() that the task loop has exited before self-deleting.
  if (_done_sem.is_created()) {
    _done_sem.give();
  }
}

// ---------------------------------------------------------------------------
// Touch processing
// ---------------------------------------------------------------------------

void InputService::process_touch_interrupt() {
  TouchData data{};
  if (!_touch.read(data)) {
    AG_LOGW(TAG, "touch read failed (I2C)");
    // I2C failure — clear the interrupt anyway to avoid an interrupt storm,
    // then skip this event.
    _touch.clear_interrupt();
    return;
  }
  _touch.clear_interrupt(); // acknowledge: read() does not clear the latch

  // Filter out channels flagged as noisy; noisy reads are unreliable.
  const uint8_t valid_touches = data.touched & static_cast<uint8_t>(~data.noise);

  // Attempt recalibration if noise is persistent.
  if (data.noise != 0 && _touch.supports_calibration()) {
    _touch.calibrate(data.noise);
  }

  const uint64_t now = RTOS::get_time_ms();

  // CH1 = TouchEnter: edge-driven gesture FSM (long-press + double-press), not
  // time-debounced so a quick double tap survives.  The first read() is the
  // latched status, which stays CH1 from press through release; a second read()
  // after clear gives the settled state so the release edge is seen instead of
  // stale CH1.  Only re-read when CH1 is involved (or a press is in flight).
  uint8_t ch1_valid = valid_touches; // fall back to latched read
  if ((valid_touches & TouchChannel::CH1) || _ch1_down) {
    TouchData settled{};
    if (_touch.read(settled)) {
      ch1_valid = settled.touched & static_cast<uint8_t>(~settled.noise);
    }
  }
  update_ch1_gesture((ch1_valid & TouchChannel::CH1) != 0, now);

  // CH2 = TouchUp, CH3 = TouchDown: immediate short press.  Time debounce
  // rejects the CAP1203 INT re-assertion while the finger is held on the pad.
  if ((now - _last_touch_time_ms) >= static_cast<uint64_t>(_config.debounce_ms)) {
    bool posted = false;
    if (valid_touches & TouchChannel::CH2) {
      post_input_event(InputSource::TouchUp, InputType::ShortPress);
      posted = true;
    }
    if (valid_touches & TouchChannel::CH3) {
      post_input_event(InputSource::TouchDown, InputType::ShortPress);
      posted = true;
    }
    if (posted) {
      _last_touch_time_ms = now;
    }
  }
}

void InputService::update_ch1_gesture(bool ch1_now, uint64_t now_ms) {
  if (ch1_now && !_ch1_down) {
    // Press-down (rising edge): arm the long-press timer.  Qualify a second
    // tap if this press follows a buffered tap within the double-tap window.
    _ch1_down = true;
    _ch1_press_start_ms = now_ms;
    _ch1_long_fired = false;
    _ch1_second_tap =
        _ch1_tap_pending &&
        (now_ms - _ch1_first_tap_ms) <= static_cast<uint64_t>(_config.touch_double_tap_window_ms);
    return;
  }

  if (!ch1_now && _ch1_down) {
    // Release (falling edge).
    _ch1_down = false;
    if (_ch1_long_fired) {
      // Long-press already emitted for this hold: consume the release silently.
      _ch1_long_fired = false;
      _ch1_tap_pending = false;
      _ch1_second_tap = false;
      return;
    }
    if (_ch1_second_tap) {
      // Second short tap within the window: quick double press.
      post_input_event(InputSource::TouchEnter, InputType::DoublePress);
      _ch1_tap_pending = false;
      _ch1_second_tap = false;
      return;
    }
    // First short tap: buffer it.  check_pending_touch_gesture() emits a single
    // ShortPress once the double-tap window expires without a second tap.
    _ch1_tap_pending = true;
    _ch1_first_tap_ms = now_ms;
  }
  // ch1_now && _ch1_down (held) or !ch1_now && !_ch1_down (idle): no transition.
}

void InputService::check_pending_touch_gesture() {
  const uint64_t now_ms = RTOS::get_time_ms();

  // Long-press: fires at the threshold while still held.  Confirm the finger is
  // still on CH1 with a read() (no clear) before emitting — guards against a
  // dropped release edge that would otherwise leave the FSM stuck "pressed".
  if (_ch1_down && !_ch1_long_fired &&
      (now_ms - _ch1_press_start_ms) >= static_cast<uint64_t>(_config.touch_long_press_ms)) {
    TouchData data{};
    const bool held = _touch.read(data) &&
                      (data.touched & static_cast<uint8_t>(~data.noise) & TouchChannel::CH1) != 0;
    if (held) {
      post_input_event(InputSource::TouchEnter, InputType::LongPress);
      _ch1_long_fired = true;
    } else {
      // Finger already lifted but the release edge was missed: resolve as a tap.
      _ch1_down = false;
      if (_ch1_second_tap) {
        post_input_event(InputSource::TouchEnter, InputType::DoublePress);
        _ch1_tap_pending = false;
        _ch1_second_tap = false;
      } else {
        _ch1_tap_pending = true;
        _ch1_first_tap_ms = now_ms;
      }
    }
  } else if (_ch1_down && _ch1_long_fired) {
    // Safety: a missed release edge after a long-press would strand _ch1_down.
    // Detect the lifted finger via read() so the FSM cannot stay stuck.
    TouchData data{};
    if (_touch.read(data) &&
        (data.touched & static_cast<uint8_t>(~data.noise) & TouchChannel::CH1) == 0) {
      _ch1_down = false;
      _ch1_long_fired = false;
    }
  }

  // Single tap: emit once the double-tap window expires with no second press
  // and no press currently in progress.
  if (_ch1_tap_pending && !_ch1_down &&
      (now_ms - _ch1_first_tap_ms) > static_cast<uint64_t>(_config.touch_double_tap_window_ms)) {
    post_input_event(InputSource::TouchEnter, InputType::ShortPress);
    _ch1_tap_pending = false;
  }
}

// ---------------------------------------------------------------------------
// Touch health watchdog
// ---------------------------------------------------------------------------

void InputService::check_touch_health() {
  // GPIO-only check first (no I2C cost when healthy).
  // INT is active-low: 0 = asserted (touch pending or stuck), 1 = deasserted.
  const int level = _gpio.get_level(_config.pin_cap_int);
  if (level != 0) {
    return; // INT deasserted — healthy.
  }

  // INT is asserted.  Attempt escalating recovery.
  AG_LOGW(TAG, "touch INT stuck low — attempting clear_interrupt");
  _touch.clear_interrupt();

  if (_gpio.get_level(_config.pin_cap_int) != 0) {
    AG_LOGI(TAG, "touch recovered after clear_interrupt");
    return;
  }

  // Still stuck — full re-initialization resets all chip registers.
  AG_LOGW(TAG, "touch still stuck — attempting full re-init");
  if (_touch.init()) {
    AG_LOGI(TAG, "touch recovered after re-init");
  } else {
    AG_LOGE(TAG, "touch re-init failed — touch unavailable until next boot");
  }
}

// ---------------------------------------------------------------------------
// Physical button processing
// ---------------------------------------------------------------------------

void InputService::process_button_event(InputSource source, uint64_t timestamp_ms) {
  const int idx = (source == InputSource::ButtonPower) ? 0 : 1;
  const int pin = pin_for_button_index(idx);
  const int level = _gpio.get_level(pin);

  if (level == BUTTON_PRESSED_LEVEL) {
    // Wake-press suppression: discard the first ButtonPower press after a
    // button wake so it cannot generate a spurious ShortPress that would
    // re-lock the device before the user interacts.  The suppression window
    // expires after a short deadline so a stale flag cannot eat a real press.
    if (source == InputSource::ButtonPower && _suppress_next_power_press) {
      _suppress_next_power_press = false;
      if (_suppress_deadline_ms == 0 || timestamp_ms < _suppress_deadline_ms) {
        AG_LOGW(TAG, "ButtonPower event suppressed");
        return; // within window — suppress the spurious wake press
      }
      // Deadline expired — this is a real press; fall through to normal handling.
    }

    // Press-down: debounce and arm long-press timer.
    // _first_press acts as an invalid-sentinel: skip the debounce window on
    // the very first press so that a press at t=0 is never spuriously rejected.
    if (!_first_press[idx] &&
        (timestamp_ms - _last_event_time_ms[idx]) < static_cast<uint64_t>(_config.debounce_ms)) {
      return;
    }
    _first_press[idx] = false;
    _last_event_time_ms[idx] = timestamp_ms;
    _press_start_time_ms[idx] = timestamp_ms;
    _pending_long_press[idx] = true;
  } else {
    // Release: if long-press timer is still pending, classify as short press
    // immediately.  The _pending_long_press guard also prevents bounce on the
    // rising edge from posting duplicate events.
    if (_pending_long_press[idx]) {
      _pending_long_press[idx] = false;
      post_input_event(source, InputType::ShortPress);
    }
  }
}

void InputService::check_pending_long_press() {
  const uint64_t now_ms = RTOS::get_time_ms();

  for (int idx = 0; idx < 2; ++idx) {
    if (!_pending_long_press[idx]) {
      continue;
    }
    const uint64_t elapsed_ms = now_ms - _press_start_time_ms[idx];
    if (elapsed_ms < static_cast<uint64_t>(_config.long_press_ms)) {
      // Timer has not expired yet; keep waiting.
      continue;
    }

    // Long-press threshold reached.  Check whether the button is still held.
    const int pin = pin_for_button_index(idx);
    const InputSource source = (idx == 0) ? InputSource::ButtonPower : InputSource::ButtonBoot;
    _pending_long_press[idx] = false;

    if (_gpio.get_level(pin) == BUTTON_PRESSED_LEVEL) {
      post_input_event(source, InputType::LongPress);
    } else {
      // Button was released before the threshold — classify as short press.
      post_input_event(source, InputType::ShortPress);
    }
  }
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint32_t InputService::compute_queue_timeout_ms() const {
  const uint64_t now_ms = RTOS::get_time_ms();

  // Start with the touch watchdog interval as the upper bound so the task
  // wakes periodically even when no buttons are pressed.
  uint32_t min_remaining_ms = _config.touch_watchdog_ms;

  for (int idx = 0; idx < 2; ++idx) {
    if (!_pending_long_press[idx]) {
      continue;
    }
    const uint64_t elapsed_ms = now_ms - _press_start_time_ms[idx];
    if (elapsed_ms >= static_cast<uint64_t>(_config.long_press_ms)) {
      // Already expired; return 0 to wake immediately.
      return 0;
    }
    const uint32_t remaining_ms = static_cast<uint32_t>(_config.long_press_ms - elapsed_ms);
    min_remaining_ms = std::min(min_remaining_ms, remaining_ms);
  }

  // CH1 long-press threshold: wake to fire LongPress while the finger is held.
  if (_ch1_down && !_ch1_long_fired) {
    const uint64_t elapsed_ms = now_ms - _ch1_press_start_ms;
    if (elapsed_ms >= static_cast<uint64_t>(_config.touch_long_press_ms)) {
      return 0;
    }
    min_remaining_ms =
        std::min(min_remaining_ms, static_cast<uint32_t>(_config.touch_long_press_ms - elapsed_ms));
  }

  // CH1 double-tap window: wake to resolve a buffered single tap once expired.
  if (_ch1_tap_pending && !_ch1_down) {
    const uint64_t elapsed_ms = now_ms - _ch1_first_tap_ms;
    if (elapsed_ms > static_cast<uint64_t>(_config.touch_double_tap_window_ms)) {
      return 0;
    }
    min_remaining_ms =
        std::min(min_remaining_ms,
                 static_cast<uint32_t>(_config.touch_double_tap_window_ms - elapsed_ms + 1));
  }

  return min_remaining_ms;
}

int InputService::pin_for_button_index(int idx) const {
  return (idx == 0) ? _config.pin_button_power : _config.pin_button_boot;
}

void InputService::pause() { _paused.store(true, std::memory_order_release); }

void InputService::resume() { _paused.store(false, std::memory_order_release); }

void InputService::post_input_event(InputSource source, InputType type) {
  if (_paused.load(std::memory_order_acquire)) {
    return;
  }
  Event evt{};
  evt.type = EventType::InputPress;
  evt.input.source = source;
  evt.input.type = type;
  // Non-blocking send: drop the event if the orchestrator queue is full.
  RTOS::queue_send(_event_queue, &evt, 0);
}
