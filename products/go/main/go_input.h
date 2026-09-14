/*
 * AirGradient Go — Input Service
 *
 * Runs as an independent RTOS task that handles all user input: 3
 * capacitive touch pads (via CAP1203) and 2 physical buttons (via GPIO).
 * Classifies raw hardware events into typed InputPress events and posts them
 * to the orchestrator event queue.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "airgradient_gpio.h"
#include "cap_touch_sensor.h"
#include "go_events.h"
#include "go_types.h"
#include "rtos.h"

#include <atomic>
#include <cstdint>

class InputService {
public:
  struct Config {
    int pin_cap_int;                           // CAP1203 INT pin (GPIO)
    int pin_button_power;                      // physical button 1 (Power/lock)
    int pin_button_boot;                       // physical button 2 (Boot/factory-reset)
    uint32_t debounce_ms = 500;                // debounce window (must exceed CAP1203
                                               // re-assertion time to prevent duplicate
                                               // events while finger is held on pad)
    uint32_t long_press_ms = 2000;             // long-press threshold (physical only)
    uint32_t touch_long_press_ms = 1000;       // CH1 (TouchEnter) long-press threshold
    uint32_t touch_double_tap_window_ms = 100; // max gap between CH1 taps for double-press
    uint32_t touch_watchdog_ms = 5000;         // periodic touch health check interval
    uint32_t touch_poll_ms = 20;               // ALERT poll period when the pin has no
                                               // interrupt (v2: expander input)
    uint16_t task_stack_size = 3072;           // RTOS task stack words
    uint8_t task_priority = 6;                 // RTOS task priority
    bool suppress_button_wake = false;         // when true, discard the first
                                               // ButtonPower press event (the
                                               // button wake press that started
                                               // this boot cycle)
  };

  /// Construct the service.  Does not start the task.
  ///
  /// @param touch        CAP1203 touch sensor driver (or mock).  Must outlive
  ///                     this service.
  /// @param gpio         GPIO HAL function-pointer table.
  /// @param event_queue  Orchestrator event queue.  Must be created before
  ///                     start() is called.
  /// @param config       Runtime configuration (pins, debounce, …).
  InputService(CapTouchSensor &touch, const gpio::Hal &gpio, RtosQueueHandle event_queue,
               const Config &config);
  ~InputService();

  /// Start the input processing task and register ISR handlers.
  /// Returns true if the task was created successfully.
  bool start();

  /// Stop the task and unregister ISR handlers.
  void stop();

  /// Suppress all input event posting. Thread-safe: called from the
  /// orchestrator task while the InputService task is running.
  void pause();

  /// Resume input event posting. Thread-safe.
  void resume();

private:
  CapTouchSensor &_touch;
  const gpio::Hal &_gpio;
  RtosQueueHandle _event_queue;
  Config _config;

  RtosQueueHandle _raw_queue; // internal ISR -> task queue
  volatile bool _running = false;
  std::atomic<bool> _paused{false};
  RtosTaskHandle _task_handle = nullptr;
  RtosBinarySemaphore _done_sem; // signalled by task before self-delete

  // Touch debounce: ignore CH2/CH3 touch interrupts arriving within debounce_ms
  // of the previous accepted touch.  Prevents the CAP1203 INT re-assertion
  // (after clear_interrupt) from generating a duplicate event while the finger
  // is still on the pad.  CH1 is exempt — it uses the gesture FSM below.
  uint64_t _last_touch_time_ms = 0;

  // CH1 (TouchEnter) gesture FSM: long-press fires at the threshold while held;
  // a quick second tap within the double-tap window emits a double-press.  CH1
  // has its repeat rate disabled at the chip, so it produces only press/release
  // edges; gesture resolution is timer-driven via check_pending_touch_gesture().
  bool _ch1_down = false;           // between a press-down and its release edge
  uint64_t _ch1_press_start_ms = 0; // press-down timestamp
  bool _ch1_long_fired = false;     // LongPress already emitted for current hold
  bool _ch1_tap_pending = false;    // one short tap buffered, awaiting a second
  uint64_t _ch1_first_tap_ms = 0;   // first-tap release timestamp
  bool _ch1_second_tap = false;     // second press-down qualified within window

  // Touch watchdog: timestamp of the last periodic health check.
  uint64_t _last_touch_check_ms = 0;

  // Set when the ALERT pin cannot raise a GPIO interrupt (v2.0 routes it to
  // the I2C expander): the task polls the level every touch_poll_ms instead.
  bool _poll_touch = false;

  // Wake-press suppression: when true, the next ButtonPower press-down event
  // is discarded (the press that woke the device from deep sleep).  Expires
  // after _suppress_deadline_ms so a stale flag cannot eat a real press.
  bool _suppress_next_power_press = false;
  uint64_t _suppress_deadline_ms = 0;

  // Per-button state for debounce and long-press detection.
  // Index 0 = ButtonPower, index 1 = ButtonBoot.
  // _first_press sentinel: true means the button has never been pressed, so
  // the debounce window is skipped and the first press is always accepted.
  bool _first_press[2] = {true, true};
  uint64_t _last_event_time_ms[2] = {0, 0};
  uint64_t _press_start_time_ms[2] = {0, 0};
  bool _pending_long_press[2] = {false, false};

protected:
  /// Read CAP1203 status, clear interrupt, drive the CH1 gesture FSM, and post
  /// ShortPress events for valid (non-noisy) CH2/CH3 channels.
  void process_touch_interrupt();

  /// Advance the CH1 (TouchEnter) gesture FSM for one read.  ch1_now is the
  /// current debounced CH1 touch state; now_ms is the read timestamp.
  void update_ch1_gesture(bool ch1_now, uint64_t now_ms);

  /// Resolve pending CH1 gestures: fire LongPress at the threshold (confirmed
  /// via peek()), or a single ShortPress once the double-tap window expires.
  void check_pending_touch_gesture();

  /// Debounce a physical button event and start a long-press timer.
  void process_button_event(InputSource source, uint64_t timestamp_ms);

  /// Check all pending long-press timers; fire LongPress or ShortPress
  /// once the threshold is reached.
  void check_pending_long_press();

  /// Check whether the CAP1203 INT line is stuck LOW (asserted) and attempt
  /// recovery.  Escalation: clear_interrupt() first, then full init().
  void check_touch_health();

  /// Compute the timeout (ms) to pass to RTOS::queue_receive so the task wakes
  /// up when the nearest pending long-press timer expires or when the touch
  /// health watchdog fires, whichever comes first.
  uint32_t compute_queue_timeout_ms() const;

  /// Post a typed input event to the orchestrator queue (non-blocking).
  /// Virtual to allow test subclasses to capture events without a real queue.
  virtual void post_input_event(InputSource source, InputType type);

private:
  // ISR handlers (static, minimal: post raw event to _raw_queue).
  static void cap_int_isr(void *arg);
  static void button_power_isr(void *arg);
  static void button_boot_isr(void *arg);

  static void task_entry(void *arg);
  void run();

  /// Map a button index (0=Power, 1=Boot) to its GPIO pin number.
  int pin_for_button_index(int idx) const;
};
