# Input Service

Independent RTOS task that handles all user input for AirGradient Go: 3
capacitive touch pads (via CAP1203) and 2 physical buttons (via GPIO).
Classifies raw hardware events into typed `InputPress` events and posts them
to the orchestrator event queue.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_input.h` | `InputService` class declaration, `Config` struct |
| `products/go/main/go_input.cpp` | ISR handlers, task loop, debounce/classification logic |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `CapTouchSensor` (CAP1203) | `airgradient-touch` | Read touch pad state, clear interrupt |
| `gpio::Hal` | `airgradient-gpio` | GPIO pin configuration, ISR registration, level reads |
| `go_types.h` | product | `InputSource`, `InputType` enums |
| `go_events.h` | product | `Event`, `EventType::InputPress` |
| `RTOS` | `airgradient-common` | `task_create`, `task_delete`, `queue_send`, `get_time_ms` |
| RTOS queue | `airgradient-common` | Internal raw queue (`queue_create` / `queue_receive` / `queue_send_from_isr`) |

## Hardware Inputs

| Input | Source | Mapping |
|---|---|---|
| Touch Enter | CAP1203 `Config::touch_map.enter` (v1: CH1, v2: CH3 = pad T3) | `InputSource::TouchEnter`, short / long / double press |
| Touch Up | CAP1203 `Config::touch_map.up` (v1: CH2, v2: CH1 = pad T1) | `InputSource::TouchUp`, short press only |
| Touch Down | CAP1203 `Config::touch_map.down` (v1: CH3, v2: CH2 = pad T2) | `InputSource::TouchDown`, short press only |

The channel-to-role map comes from `GoBoard::touch_channel_map()`; the Enter
channel is also the one `GoHardwareBoard::new_touch_sensor()` excludes from
the CAP1203 repeat rate so the gesture FSM sees clean press/release edges.
| Button Power | Physical GPIO | `InputSource::ButtonPower`, short or long press |
| Button Boot | Physical GPIO | `InputSource::ButtonBoot`, short or long press |

All three touch channels share a single INT line. The CAP1203 INT fires when
any enabled channel detects a touch. The task reads the CAP1203 registers in
task context to determine which channel(s) fired.

Physical buttons are active-low (pulled high internally; pressed = GPIO low).

`Button Power` (GPIO5) is also wired to the BQ25629 `/QON` pin. The Input
Service only ever sees its GPIO edges, but a `ButtonPower` long press that
the orchestrator turns into shutdown opens the BMS BATFET; if the user
keeps holding, `/QON` re-wakes the charger and the device cold-boots
(hold-to-restart). See [Power Management](power_management.md) for the QON
re-wake details.

## Configuration

`InputService::Config` fields — all pin assignments come from wiring at
construction time (typically from `board_config.h`):

| Field | Default | Notes |
|---|---|---|
| `pin_cap_int` | — | CAP1203 ALERT/INT output pin (GPIO number) |
| `pin_button_power` | — | Power / lock / unlock physical button |
| `pin_button_boot` | — | Boot / factory-reset physical button |
| `debounce_ms` | `500` | Minimum ms between two accepted CH2/CH3 touch and button events. Must exceed the CAP1203 re-assertion time to prevent duplicate events while a finger is held on the pad. CH1 is exempt — it uses the gesture FSM. |
| `long_press_ms` | `2000` | Duration (ms) a button must be held before firing `LongPress` |
| `touch_long_press_ms` | `1000` | Duration (ms) CH1 (TouchEnter) must be held before firing `LongPress` |
| `touch_double_tap_window_ms` | `250` | Maximum gap (ms) between two CH1 taps for a `DoublePress` |
| `touch_watchdog_ms` | `5000` | Interval (ms) between periodic touch health checks. The task wakes at least this often to verify the CAP1203 INT line is not stuck. |
| `task_stack_size` | `3072` | RTOS task stack in words; tune at integration time |
| `task_priority` | `6` | Above GPS task; at or above sensor task |
| `suppress_button_wake` | `false` | When `true`, the first `ButtonPower` press-down event is silently discarded. Set by the button-wake boot path to prevent the wake press from generating a spurious `ShortPress` that would immediately re-lock the device. |

## Usage

```cpp
#include "go_input.h"
#include "board_config.h"

// Dependencies must outlive the service.
CAP1203 cap1203(i2c_bus);
cap1203.init();

InputService::Config cfg{};
cfg.pin_cap_int      = BOARD_PIN_CAP_INT;
cfg.pin_button_power = BOARD_PIN_BUTTON_POWER;
cfg.pin_button_boot  = BOARD_PIN_BUTTON_BOOT;
// debounce_ms and long_press_ms use defaults (500 / 2000)

InputService input_svc(cap1203, gpio::native::hal, event_queue, cfg);
input_svc.start();

// Clean shutdown (blocks until task exits via semaphore handshake):
input_svc.stop();
```

## Event Output

All events are posted to the orchestrator queue as `EventType::InputPress`
with an `InputEventData` payload:

```cpp
struct InputEventData {
  InputSource source;  // TouchUp / TouchDown / TouchEnter / ButtonPower / ButtonBoot
  InputType   type;    // ShortPress / LongPress / DoublePress
};
```

CH2/CH3 touch pads only ever produce `ShortPress`. CH1 (TouchEnter) produces
`ShortPress`, `LongPress`, or `DoublePress` via its gesture FSM. Physical
buttons produce either `ShortPress` or `LongPress` depending on hold duration.

## Internal Architecture

### ISR → Task Pipeline

```text
GPIO interrupt
  • Touch INT: falling edge only (CAP1203 asserts INT low on touch)
  • Button GPIOs: any edge (press-down = falling, release = rising)
  |
  v
Static ISR handler (cap_int_isr / button_power_isr / button_boot_isr)
  • Minimal: no heap, no blocking, no logging
  • Posts RawInputEvent{source, 0} via RTOS ISR-safe queue send
  |
  v
Internal raw queue (depth 8, DRAM)
  |
  v
InputService task (input_task)
  • Records timestamp via RTOS::get_time_ms() at dequeue time
  • Routes to process_touch_interrupt() or process_button_event()
  |
  v
Orchestrator event queue  (EventType::InputPress)
```

### Touch Interrupt Processing

1. Call `CapTouchSensor::read()` to get `TouchData` (touched + noise bitmasks).
2. Call `CapTouchSensor::clear_interrupt()` to de-assert the CAP1203 INT line.
3. Compute `valid_touches = touched & ~noise` to discard noisy channels.
4. If any channel has a noise flag, attempt recalibration via `calibrate(noise)`.
5. Drive the CH1 gesture FSM with the current CH1 state (see below).
6. Map valid CH2/CH3 bits to their `InputSource` and post `ShortPress`
   (time-debounced by `debounce_ms`).

The touch-sentinel approach (ISR always posts `TouchEnter` as a placeholder)
keeps ISR code minimal: actual channel determination happens in task context
where I2C reads are safe.

### CH1 (TouchEnter) Gesture FSM

CH1 has its **repeat rate disabled at the chip** (`repeat_rate_channels`
excludes CH1), so it produces a clean two-edge stream per touch: a press and a
release. The FSM classifies these edges plus two timers into short / long /
double press. It is **not** subject to the `debounce_ms` window so a quick
double tap is not collapsed.

The CH1 state comes from a **second `read()` after `clear_interrupt()`**, not
the first read: the CAP1203 holds `SENSOR_INPUT_STATUS` at CH1 from press
through release, so the first (latched) read returns stale CH1 on the release
interrupt. The post-clear read reflects the settled state (`CH1` while held,
`0` once released), giving a reliable release edge. `read()` itself never
clears the latch — `clear_interrupt()` is the explicit ack. The re-read only
runs when CH1 is involved (`latched & CH1` or a press in flight), so CH2/CH3
stay at a single read.

- **Press-down**: arm the long-press timer (`press_start = now`). If a buffered
  tap exists within `touch_double_tap_window_ms`, flag this as a second tap.
- **Release** (short): if a `LongPress` already fired this hold, consume the
  release silently; if flagged as a second tap, emit `DoublePress`; otherwise
  buffer the tap (single `ShortPress` resolves on window expiry).
- **`check_pending_touch_gesture`** (each loop iteration):
  - At `touch_long_press_ms`, confirm the finger is still on CH1 via a
    `read()` (no clear). If held → `LongPress`; if lifted (release edge
    dropped) → resolve as a tap.
  - After a `LongPress`, a safety `read()` clears the pressed state if the
    release edge was missed, so the FSM cannot stay stuck.
  - Once `touch_double_tap_window_ms` elapses with a buffered tap and no press
    in progress → emit a single `ShortPress`.
- **Dynamic timeout**: `compute_queue_timeout_ms()` folds in the CH1 long-press
  threshold and double-tap window so the task wakes to resolve gestures.

A single tap is therefore delayed by `touch_double_tap_window_ms` (the cost of
distinguishing it from a double tap); long-press fires proactively at the
threshold while the finger is still held.

### Physical Button Processing

Buttons use **both-edge** interrupts (press-down = falling, release = rising).
The task reads the GPIO level after dequeuing to distinguish press from release.
Debounce and long-press detection run entirely in task context:

1. **Press-down** (GPIO low):
   - **Debounce**: Accept only if `now - last_event_time >= debounce_ms`.
   - **Arm timer**: Record `press_start_time = now`, set `pending_long_press = true`.
2. **Release** (GPIO high):
   - If `pending_long_press` is still true, classify as `ShortPress` immediately
     and cancel the timer. The `pending_long_press` guard also prevents bounce
     on the rising edge from posting duplicate events.
3. **Dynamic timeout**: RTOS queue receive uses a timeout equal to the minimum
   of the nearest pending long-press expiry and the touch watchdog interval, so
   the task wakes for whichever fires first.
4. **Check expiry** (`check_pending_long_press`): On each loop iteration
   (after receive or timeout), if `now - press_start >= long_press_ms`:
   - Read GPIO level: if still low (held) → `LongPress`
   - If high (released) → `ShortPress` (safety net; normally handled by step 2)

Short-press latency is determined by how long the user holds the button
(typically 100–200 ms), rather than waiting for the full `long_press_ms`
timeout.

## CAP1203 Noise Handling

The CAP1203 reports per-channel noise flags in `TouchData.noise`. Channels with
noise flags set are masked out before posting events:

```cpp
uint8_t valid_touches = data.touched & ~data.noise;
```

When noise is non-zero and the sensor supports calibration, the service
triggers hardware recalibration on the affected channels:

```cpp
if (data.noise != 0 && _touch.supports_calibration()) {
    _touch.calibrate(data.noise);
}
```

## Touch Health Watchdog

The CAP1203 INT line is configured for **falling-edge** GPIO interrupts. If the
`clear_interrupt()` I2C transaction fails (e.g. bus contention with other
devices), the INT line stays permanently LOW and no further falling edges can
occur — silently killing all touch input until a power cycle.

The input task runs a periodic health check every `touch_watchdog_ms`
(default 5 s) to detect and recover from this condition:

1. **GPIO-only check** — read the INT pin level. If HIGH (deasserted), touch is
   healthy; return immediately with no I2C cost.
2. **Level 1 recovery** — INT is LOW. Call `clear_interrupt()` to clear the
   CAP1203 interrupt latch, then re-read the pin. If the pin is now HIGH, log
   recovery and return.
3. **Level 2 recovery** — still stuck. Call `init()` to fully re-initialize the
   CAP1203 (probe, identity check, config write, interrupt clear). Log the
   outcome.

`compute_queue_timeout_ms()` caps the task's queue-receive timeout at
`touch_watchdog_ms` so the task always wakes periodically, even when no
long-press timers are pending.

## `CapTouchSensor::clear_interrupt()` — HAL Extension

The `clear_interrupt()` method was added to `CapTouchSensor` (base class) as a
virtual no-op (`return true`) so `InputService` can call it through the HAL
interface without requiring a concrete `CAP1203` reference. `CAP1203` overrides
it to read `MAIN_CONTROL` (clears INT bit) and `GENERAL_STATUS` (latches
touch-status flags).

`read()` is side-effect-free: it returns the current status registers but does
**not** clear the INT latch. The caller acks via `clear_interrupt()`, and the
CH1 gesture FSM re-reads after the ack to obtain settled state.

## Deep Sleep

Physical button GPIOs are configured as deep sleep wake sources by the Power
Management service before entering sleep. The Input Service does not manage
sleep wake configuration.

### Wake-Press Suppression

On wake from a button press, the GPIO edge that woke the device may still be
visible to the Input Service after it starts (e.g. if the button is still held
when `run()` configures its ISR, or if the release edge is detected shortly
after startup). Without suppression, this can generate a spurious
`ButtonPower ShortPress` that toggles the lock state — re-locking the device
that was just unlocked by the button-wake path.

When `Config::suppress_button_wake = true`, the first `ButtonPower` press-down
event is discarded:

```text
process_button_event(ButtonPower, ts):
  if level == PRESSED and _suppress_next_power_press:
    _suppress_next_power_press = false   // arm consumed; future presses normal
    return                               // no long-press timer armed → no event
```

Since the long-press timer is never armed for the suppressed press, the
corresponding release (rising edge) also produces no event — the suppression
covers the complete wake press/release cycle.

`_suppress_next_power_press` is initialized from `config.suppress_button_wake`
in the constructor and is a one-shot: subsequent `ButtonPower` presses behave
normally. Touch pad events are never suppressed.

## Testability

- Inject a mock `CapTouchSensor` returning controlled `TouchData`.
- Construct `gpio::Hal` with mock function pointers for level reads and ISR
  registration.
- The debounce and long-press classification logic is in `process_button_event`
  and `check_pending_long_press` — pure functions of timestamps and GPIO level,
   testable without RTOS.
- RTOS queue operations are no-ops under `TEST_HOST` so the
  translation unit compiles cleanly for native host tests.
