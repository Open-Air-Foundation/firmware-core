# LED Service

Product-specific LED service for AirGradient Go. Drives the LP5036
36-channel I2C LED controller on the mobile-display sub-PCB through an
adaptive render loop. The service manages two independent LED groups
(back indicator and touch feedback) and supports time-evolving
effects such as blink, breathe, fade, chase, and sequenced animations.

## Files

| File | Purpose |
|---|---|
| `products/go/main/led/go_led_types.h` | Public types: `LedBrightness`, `TouchLedIntensity`, `TouchPad`, `BackStep`, `BackAnimation` |
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
| `air_quality`, `Rgb` | `airgradient-common` (`air_quality.h`, `rgb.h`) | Shared AQI/CO2 classification and unscaled RGB colors |

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
| `back_channels` / `back_channel_count` | Array / `uint8_t` | Zero-filled / `0` | Up to five populated RGB groups, in animation order |
| `touch_channels` / `touch_channel_count` | Array / `uint8_t` | Zero-filled / `0` | Up to three RGB groups in Select/Left/Right order; zero disables output |

### Lifecycle

| Method | Returns | Purpose |
|---|---|---|
| `init()` | `bool` | Init driver, create queue. Cached: second call returns first result. |
| `start()` | `bool` | Spawn worker task. Idempotent. |

### Back (Animated)

| Method | Purpose |
|---|---|
| `back_solid(color)` | All configured back LEDs at `color`. Static. |
| `back_blink(color, period_ms)` | Toggle on/off at `period_ms`. Looping. |
| `back_breathe(color, period_ms)` | Cosine ramp between 100% and 0%. Looping. |
| `back_fade_to(color, duration_ms)` | Linear interpolate from current color. One-shot. |
| `back_chase(color, step_ms)` | Sequential fill, one LED per `step_ms`. One-shot. |
| `back_off()` | All configured back LEDs off. Static. |
| `back_set_brightness(b)` | Scale factor: Off=0, Dim=64, Mid=128, Bright=255. |
| `back_play(steps, count)` | Play a transient sequence of up to 6 sub-effects with auto-restore. |
| `back_animate(animation)` | Play a predefined `BackAnimation` (resolves to `back_play`). |
| `back_update_aqi(pm25)` | Resolve PM2.5 to AQI category color, enqueue `back_solid`. |
| `back_clear_aqi()` | Enqueue `back_off`. |
| `back_update_co2(ppm)` | Resolve CO2 category color; invalid input enqueues `back_off`. |

### Touch

| Method | Purpose |
|---|---|
| `touch_flash(pad)` | Flash white on the given pad for `touch_flash_ms`, then off. |
| `touch_set_all(on)` | Light (or clear) all three pads steadily at the current intensity, with no auto-off. Used by the hardware peripheral test. |
| `touch_set_intensity(i)` | Off/Dim/Bright. Off suppresses all flashes. |

## LED Groups And Channel Map

The board supplies both layouts. Each number is a blue-channel base; green
and red use the next two LP5036 outputs.

| Board | Back Channels | Touch Channels (Select / Left / Right) |
|---|---|---|
| V1 | `6, 12, 15, 18, 24` | `27, 3, 0` |
| V2 | `0, 3, 6, 9` | None |

V2's four RGB LEDs all belong to the back group. Touch LED commands do not
enqueue work or write outputs on V2. Chase rendering and duration use the
configured count (`count × step_ms`). V2 uses LED1–LED4 order; physical animation
order still requires hardware confirmation.

V1 wiring:

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

LED8 (OUT21/22/23) is reserved for a future follow-up.

## Brightness and PWM Tables

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

### Selected Indicator

`LED Indicator` selects AQI (default) or CO2, independently of `LED Brightness`.
The orchestrator uses corrected PM2.5 for AQI and the unchanged CO2 reading for
CO2. Selecting a source immediately uses its latest reading; invalid or missing
data turns the back LEDs off, without falling back to the other source. There
is no hysteresis. PM Display units do not affect either indicator.

Diagnostic and melody overrides restore the selected indicator afterward.
Brightness Off preserves the selection and logical color for later use.

### Composite LED Test

The orchestrator provides a blocking diagnostic that exercises every mapped
LED group. Back LEDs show red, green, and blue for one second each: five on V1,
four on V2. V1's three touch LEDs remain bright during the test. After three
seconds, the orchestrator restores the configured group levels and the latest
valid selected indicator color, or clears the back LEDs when its reading is invalid.

The delay blocks only the orchestrator task; the LED worker continues rendering
the back sequence. Requests are ignored while an interactive hardware-test
screen owns the LEDs.

### Adaptive Render Loop

The worker task uses an adaptive queue timeout:

- **Back animation active:** tick at `frame_interval_ms` (~33 fps).
- **Touch off-edge pending:** wake at the deadline.
- **All static:** block on queue indefinitely (zero CPU).

Each group has an independent dirty flag. The back renderer caches its last
successful uniform physical output and skips the full group when the effective
RGB value has not changed. Logical indicator and effect state therefore remain
current while back brightness is Off without repeatedly writing zero over I2C.
Enabling brightness renders the latest logical state. Chase frames remain
per-LED and invalidate the uniform cache.

Dirty flags are cleared after each render pass. A failed uniform write clears
the output cache, but it is not retried until another state change requests a
render.

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
  `back_update_co2`, etc.) **clear** the saved effect.
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
| `back_chase(color, 0)` | All configured back LEDs immediately lit |

### Touch Flash

- `touch_flash(pad)` turns off the previous pad (if any), writes white
  to the new pad, schedules off-edge at `now + touch_flash_ms`.
- A new flash before the off-edge preempts: old pad off, new pad on.
- `TouchLedIntensity::Off` suppresses all flashes immediately without driver
  writes or an off-edge wake.

### Touch Steady (All Pads)

- `touch_set_all(true)` lights all three touch pads white at the current
  intensity with no off-edge; they hold until `touch_set_all(false)`.
- Independent of the flash path (`_touch_steady` render gate), so it does
  not interfere with `touch_flash()`. Used by the V1 hardware peripheral test
  so an operator can visually confirm the touch LEDs.

### V1 Input Source To Touch Pad Mapping

| `InputSource` | `TouchPad` | Physical LED |
|---|---|---|
| `TouchEnter` | `Select` | LED10 (OUT27/28/29) |
| `TouchUp` | `Left` | LED2 (OUT3/4/5) |
| `TouchDown` | `Right` | LED1 (OUT0/1/2) |

## AQI Color Map

`back_update_aqi(pm25)` resolves color using
`air_quality::pm25_to_us_aqi()`, `air_quality::us_aqi_to_category()`, and
`air_quality::category_to_rgb()`.

| AQI Range | Category | RGB |
|---:|---|---|
| 0..50 | Good | 0, 255, 0 |
| 51..100 | Moderate | 255, 255, 0 |
| 101..150 | Unhealthy for Sensitive Groups | 255, 128, 0 |
| 151..200 | Unhealthy | 255, 0, 0 |
| 201..300 | Very Unhealthy | 128, 0, 128 |
| 301..500 | Hazardous | 139, 69, 19 |
| Invalid | n/a | `back_clear_aqi()` sends Off |

## CO2 Color Map

`back_update_co2(ppm)` uses `air_quality::co2_to_category()` and
`air_quality::category_to_rgb()`. These are CO2 categories, not US AQI values.

| CO2 (ppm) | Category | Color | RGB |
|---|---|---|---|
| 0–800 | Excellent | Green | 0, 255, 0 |
| 801–1000 | Acceptable | Yellow | 255, 255, 0 |
| 1001–1500 | Not Ideal | Orange | 255, 128, 0 |
| 1501–2000 | To Be Avoided | Red | 255, 0, 0 |
| 2001–3000 | Unhealthy | Purple | 128, 0, 128 |
| 3001–10000 | Very Unhealthy | Brown | 139, 69, 19 |

`CO2Data::is_valid()` accepts 0–10000 ppm. Values outside that range turn the
indicator off. RGB values above are unscaled; brightness applies afterward.

## Predefined Animations

### `BackAnimation::Boot`

White chase fill (100 ms per LED) followed by white hold (400 ms).
Played by `GoApp::run_interactive()` on `WakeCause::PowerOn` before
display init, so it fires immediately on power-on. Auto-restores to Off;
the orchestrator sets the selected indicator when the first measurement arrives.

## Persistence

| Setting | NVS Key | BLE Key | Enum | Valid Range | Default |
|---|---|---|---|---:|---|
| Back indicator | `bli` | `bledi` | `BackLedIndicator` | 0..1 in NVS; `aqi`/`co2` over BLE | AQI |
| Back brightness | `blb` | `bled` | `LedBrightness` | 0..3 | Off |
| Touch intensity | `tlb` | `tled` | `TouchLedIntensity` | 0..2 | Off |

Missing or invalid NVS values load as AQI for the indicator and Off for
brightness/intensity. Invalid BLE values return `invalid_config_value`. Settings
apply at boot and after successful persistence; see [Settings](settings.md#gosettings-fields).

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

- **Invalid counts**: initialization rejects counts beyond the channel arrays.
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
