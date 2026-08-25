# LED Service

Product-specific LED service for AirGradient Go. Drives the LP5036
36-channel I2C LED controller on the mobile-display sub-PCB through an
adaptive render loop. The service manages three independent LED groups
(front indicators, back AQI, touch feedback) and supports time-evolving
effects such as blink, breathe, fade, chase, and sequenced animations.
Active on board variant V1; inert on Prototype.

## Files

| File | Purpose |
|---|---|
| `products/go/main/led/go_led_types.h` | Public types: `Rgb`, `LedBrightness`, `TouchLedIntensity`, `TouchPad`, `BackStep`, `BackAnimation` |
| `products/go/main/led/go_led_hal.h` | Abstract `LedDriver` interface |
| `products/go/main/led/go_led_driver.h` | `LP5036` concrete driver declaration |
| `products/go/main/led/go_led_driver.cpp` | LP5036 I2C implementation |
| `products/go/main/led/go_led.h` | `LedService` class declaration |
| `products/go/main/led/go_led.cpp` | Service implementation (effect engine, render loop, touch flash) |
| `products/go/tests/go_led.tests.cpp` | Host tests with mocked `LedDriver` |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `LedDriver` | `go_led_hal.h` | Abstract driver surface mocked in tests |
| `LP5036` | `go_led_driver.h` | Concrete I2C driver for hardware builds |
| `RTOS` | `airgradient-common` (`rtos.h`) | Queue, task, time for the adaptive render loop |
| `aqi` | `airgradient-common` (`aqi.h`) | `pm25_to_us_aqi` and `us_aqi_to_category` for AQI color mapping |

## Board Variant

The LP5036 is only present on board variant V1. `GoHardwareBoard`
constructs `LP5036` and passes it as `Config::driver` on V1. On
Prototype, `Config::driver` is `nullptr` and the service enters inert
mode: all public methods return immediately, no queue or task is created.
The orchestrator calls `LedService` unconditionally regardless of
variant.

## Public API

### Config

| Field | Type | Default | Purpose |
|---|---|---|---|
| `driver` | `LedDriver*` | `nullptr` | Driver instance; `nullptr` = inert mode |
| `task_stack_size` | `uint16_t` | 2048 | Worker task stack depth |
| `task_priority` | `uint8_t` | 3 | Worker task RTOS priority |
| `queue_depth` | `uint8_t` | 8 | Command queue capacity |
| `touch_flash_ms` | `uint32_t` | 120 | Touch flash duration |
| `frame_interval_ms` | `uint32_t` | 30 | Frame tick interval (~33 fps) |

### Lifecycle

| Method | Returns | Purpose |
|---|---|---|
| `init()` | `bool` | Init driver, create queue. Cached: second call returns first result. |
| `start()` | `bool` | Spawn worker task. Idempotent. |

### Front (Static)

| Method | Purpose |
|---|---|
| `front_set_brightness(b)` | Set front indicator LEDs (OUT30, OUT31) to Off/Dim/Mid/Bright |

### Back (Animated)

| Method | Purpose |
|---|---|
| `back_solid(color)` | All 5 back LEDs at `color`. Static. |
| `back_blink(color, period_ms)` | Toggle on/off at `period_ms`. Looping. |
| `back_breathe(color, period_ms)` | Cosine ramp between 100% and 0%. Looping. |
| `back_fade_to(color, duration_ms)` | Linear interpolate from current color. One-shot. |
| `back_chase(color, step_ms)` | Sequential fill, one LED per `step_ms`. One-shot. |
| `back_off()` | All 5 back LEDs off. Static. |
| `back_set_brightness(b)` | Scale factor: Off=0, Dim=64, Mid=128, Bright=255. |
| `back_play(steps, count)` | Play a transient sequence of up to 6 sub-effects with auto-restore. |
| `back_animate(animation)` | Play a predefined `BackAnimation` (resolves to `back_play`). |
| `back_update_aqi(pm25)` | Resolve PM2.5 to AQI category color, enqueue `back_solid`. |
| `back_clear_aqi()` | Enqueue `back_off`. |

### Touch

| Method | Purpose |
|---|---|
| `touch_flash(pad)` | Flash white on the given pad for `touch_flash_ms`, then off. |
| `touch_set_all(on)` | Light (or clear) all three pads steadily at the current intensity, with no auto-off. Used by the hardware peripheral test. |
| `touch_set_intensity(i)` | Off/Dim/Bright. Off suppresses all flashes. |

## LED Groups and Channel Map

| Logical LED | LP5036 Channel(s) | Group |
|---|---|---|
| LED1 | OUT0/1/2 (B/G/R) | Touch Right |
| LED2 | OUT3/4/5 | Touch Left |
| LED3 | OUT6/7/8 | Back (index 0) |
| LED5 | OUT12/13/14 | Back (index 1) |
| LED6 | OUT15/16/17 | Back (index 2) |
| LED7 | OUT18/19/20 | Back (index 3) |
| LED9 | OUT24/25/26 | Back (index 4) |
| LED10 | OUT27/28/29 | Touch Select |
| LED25 | OUT30 | Front indicator |
| LED26 | OUT31 | Front indicator |

LED8 (OUT21/22/23) is reserved for a future follow-up.

## Brightness and PWM Tables

### Front PWM

| `LedBrightness` | PWM |
|---|---:|
| Off | 0 |
| Dim | 5 |
| Mid | 13 |
| Bright | 26 |

### Back Scale

| `LedBrightness` | Scale Factor |
|---|---:|
| Off | 0 |
| Dim | 64 |
| Mid | 128 |
| Bright | 255 |

Output per channel: `(effect_value * scale) / 255`.

### Touch PWM

| `TouchLedIntensity` | PWM |
|---|---:|
| Off | 0 |
| Dim | 64 |
| Bright | 255 |

## Behavior

### Composite LED Test

The orchestrator provides a blocking diagnostic that exercises every mapped
LED group. Both front indicators and all three touch LEDs remain bright while
the five back LEDs show red, green, and blue for one second each. After three
seconds, the orchestrator restores the configured group levels and the latest
valid AQI color, or clears the back LEDs when PM2.5 is invalid.

The delay blocks only the orchestrator task; the LED worker continues rendering
the back sequence. Requests are ignored while an interactive hardware-test
screen owns the LEDs. Prototype boards remain inert because their LED service
has no driver.

### Adaptive Render Loop

The worker task uses an adaptive queue timeout:

- **Back animation active:** tick at `frame_interval_ms` (~33 fps).
- **Touch off-edge pending:** wake at the deadline.
- **All static:** block on queue indefinitely (zero CPU).

Each group has an independent dirty flag. The flag is set when state
changes and cleared after each write attempt regardless of success or
failure. Failed writes are not retried.

### Back Effect Engine

The back LED group supports time-evolving effects through a tagged-union
state machine. No virtual dispatch, no heap allocation.

Effects:

- **Off / Solid**: Static. Worker sleeps.
- **Blink**: First half on, second half off. Looping.
- **Breathe**: Cosine waveform `(1 + cos(2*pi*t/period)) / 2`. Starts
  at 100%. Looping.
- **Fade**: Linear interpolation from captured `fade_from` to target.
  One-shot; transitions to Solid on completion.
- **Chase**: Sequential per-LED fill. One-shot; transitions to Solid on
  completion.
- **Sequence**: Ordered chain of sub-effects (up to 6 steps). Each step
  runs to completion, then the next starts. When done, auto-restore
  fires (see below).

### Auto-Restore

Sequences are transient overlays. When `back_play()` is called, the
worker saves the current back effect if it is static. When the sequence
completes:

- **Saved effect exists**: restore it.
- **No saved effect** (previous was non-static): hold final frame.

Rules:

- Direct primitive calls (`back_solid`, `back_off`, `back_update_aqi`,
  etc.) **clear** the saved effect.
- `back_set_brightness()` does **not** clear it.
- A new `back_play()` during an active sequence **keeps** the original
  saved effect.

### Sequence Ordering

Transient animations (`back_animate`, `back_play`) must be called
**after** steady-state updates (`back_update_aqi`, `back_solid`, etc.),
not before. A direct primitive call replaces any active sequence and
clears the saved effect, which cancels a preceding animation before it
can render.

Correct ordering:

```text
back_update_aqi(pm25);          // set steady-state AQI color
back_animate(BackAnimation::X); // overlay plays, auto-restores to AQI color
```

Wrong ordering:

```text
back_animate(BackAnimation::X); // starts sequence, saves current state
back_update_aqi(pm25);          // kills sequence immediately, clears saved state
```

### Zero Timing Parameters

| Call | `param_ms = 0` |
|---|---|
| `back_blink(color, 0)` | Treated as `back_solid(color)` |
| `back_breathe(color, 0)` | Treated as `back_solid(color)` |
| `back_fade_to(color, 0)` | Immediate snap to target |
| `back_chase(color, 0)` | All 5 LEDs immediately lit |

### Touch Flash

- `touch_flash(pad)` turns off the previous pad (if any), writes white
  to the new pad, schedules off-edge at `now + touch_flash_ms`.
- A new flash before the off-edge preempts: old pad off, new pad on.
- `TouchLedIntensity::Off` suppresses all flashes immediately.

### Touch Steady (All Pads)

- `touch_set_all(true)` lights all three touch pads white at the current
  intensity with no off-edge; they hold until `touch_set_all(false)`.
- Independent of the flash path (`_touch_steady` render gate), so it does
  not interfere with `touch_flash()`. Used by the hardware peripheral test
  so an operator can visually confirm the touch LEDs.

### Input Source to Touch Pad Mapping

| `InputSource` | `TouchPad` | Physical LED |
|---|---|---|
| `TouchEnter` | `Select` | LED10 (OUT27/28/29) |
| `TouchUp` | `Left` | LED2 (OUT3/4/5) |
| `TouchDown` | `Right` | LED1 (OUT0/1/2) |

## AQI Color Map

`back_update_aqi(pm25)` resolves color using
`aqi::pm25_to_us_aqi()` and `aqi::us_aqi_to_category()`.

| AQI Range | Category | RGB |
|---:|---|---|
| 0..50 | Good | 0, 255, 0 |
| 51..100 | Moderate | 255, 255, 0 |
| 101..150 | Unhealthy for Sensitive Groups | 255, 128, 0 |
| 151..200 | Unhealthy | 255, 0, 0 |
| 201..300 | Very Unhealthy | 128, 0, 128 |
| 301..500 | Hazardous | 139, 69, 19 |
| Invalid | n/a | `back_clear_aqi()` sends Off |

## Predefined Animations

### `BackAnimation::Boot`

White chase fill (100 ms per LED) followed by white hold (400 ms).
Played by `GoApp::run_interactive()` on `WakeCause::PowerOn` before
display init, so it fires immediately on power-on. Auto-restores to Off;
the orchestrator sets the AQI color when the first measurement arrives.

## Persistence

| Setting | NVS Key | BLE Key | Enum | Valid Range | Default |
|---|---|---|---|---:|---|
| Front brightness | `lb` | `fled` | `LedBrightness` | 0..3 | Off |
| Back brightness | `blb` | `bled` | `LedBrightness` | 0..3 | Off |
| Touch intensity | `tlb` | `tled` | `TouchLedIntensity` | 0..2 | Off |

Missing NVS key or invalid value loads as Off. BLE writes with
out-of-range values are silently rejected. Settings are applied
at boot in `Orchestrator::init()` and immediately on UI change in
`apply_settings_change()`.

## Inert Mode

When `Config::driver == nullptr`:

- `init()` and `start()` return `true` without creating queue or task.
- All public methods return immediately without enqueuing.
- `pump_for_test()` is a no-op.

## Host Testing

Under `TEST_HOST`, the service uses an in-class ring buffer instead of a
FreeRTOS queue and exposes `pump_for_test(now_ms)` via a friend class.
Tests mock `LedDriver` and drive virtual time through `pump_for_test`.
The LP5036 `.cpp` is not linked into host tests.

## Edge Cases and Errors

- **Driver errors**: Failed I2C writes are logged (edge-triggered:
  first failure WARN, first recovery INFO) and not retried. The dirty
  flag is cleared unconditionally to avoid busy-looping on a faulty bus.
- **Queue full**: Enqueue is dropped silently. The next call re-syncs.
- **Mutators before init/start**: All are no-ops. No crash, no queue
  access.
- **Fade from Chase**: `_last_rendered_back` stores the chase base color
  (uniform), so fading from an active chase may produce a visible jump
  for partially-lit LEDs. Acceptable because chase typically completes
  before a fade begins.
