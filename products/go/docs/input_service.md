# Input Service

An RTOS worker turns CAP1203 touch-pad and physical-button activity into typed
`InputPress` events for the orchestrator. Board configuration selects the touch
channels and either native interrupts or expander polling.

## Files

| File | Purpose |
|---|---|
| [go_input.h](../main/go_input.h) | `GoInputConfig`, `InputService`, gesture state |
| [go_input.cpp](../main/go_input.cpp) | Interrupts, polling, gestures, health checks |
| [go_hardware_board.cpp](../main/go_hardware_board.cpp) | Board-specific wiring and CAP1203 repeat configuration |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `CapTouchSensor` | `airgradient-touch` | Read CAP1203 state, acknowledge interrupts, recalibrate |
| `gpio::Hal` | `airgradient-gpio` | Native button interrupts and selected touch-INT backend |
| `RTOS` | `airgradient-common` | Worker, queues, timing, stop handshake |
| `go_events.h` | product | `InputPress` event and payload |

## Public API

`InputService::Config` aliases `GoInputConfig`. Both interactive boot paths use
`GoHardwareBoard::input_config()`; dependencies and borrowed GPIO backends must
outlive the service.

| Method | Behavior |
|---|---|
| `start()` | Create the worker and register native interrupt handlers |
| `stop()` | Join the worker and remove its interrupt handlers |
| `pause()` / `resume()` | Suppress or resume event posting |

### Board Configuration

| Setting | V1 | V2 |
|---|---|---|
| Touch INT | GPIO1, falling-edge interrupt | TCA6408A P4, polled every 20 ms |
| `touch_gpio` | `nullptr` (native backend) | Borrowed expander HAL |
| Enter channel | CH1 | CH3 |
| Up channel | CH2 | CH1 |
| Down channel | CH3 | CH2 |
| Physical buttons | Power GPIO5, Boot GPIO28 | Same as V1 |

`touch_poll_ms = 0` selects interrupts. A nonzero value caps the worker's
queue wait and enables touch-INT level reads in task context. V2 requires
`init_buses()` before starting input. It registers no expander interrupt
handler; physical buttons continue to use native interrupts on both boards.

### Timing Defaults

| Field | Default | Purpose |
|---|---|---|
| `debounce_ms` | `500` | Debounce directional touches and physical-button presses |
| `long_press_ms` | `2000` | Physical-button long press |
| `touch_long_press_ms` | `1000` | Enter long press |
| `touch_double_tap_window_ms` | `100` | Enter double-tap window |
| `touch_watchdog_ms` | `5000` | Touch-INT health check interval |
| `suppress_button_wake` | `false` | Discard the wake press of the Power button |

See [GoInputConfig](../main/go_input.h) for pin, channel, and worker settings.

## Behavior

### Touch Processing

V1's interrupt handler queues a notification; V2 processes touch when a polled
INT level is LOW. Both paths read CAP1203 state, acknowledge the interrupt,
mask noisy channels, and classify gestures in task context. No I2C runs in an
ISR. CAP1203 repeat is enabled only for the board's Up and Down channels.

Up and Down emit debounced `ShortPress` events. Enter emits `ShortPress`,
`LongPress`, or `DoublePress`, independently of directional debounce:

- A single tap waits for the double-tap window before emitting `ShortPress`.
- A second tap within that window emits `DoublePress`.
- A held touch emits `LongPress` at the threshold; release emits no extra event.

When Enter is involved, a second read after interrupt acknowledgement obtains
the settled state: the first read can still contain the latched press after
release. Timer checks recover missed releases and resolve pending gestures.
The queue timeout accounts for gestures, button holds, polling, and health checks.

### Physical Buttons

Both buttons are active-low and use interrupts on both edges. The worker reads
the current level: press arms a timer, release emits `ShortPress`, and a held
button emits `LongPress` at the threshold without another event on release.
Power GPIO5 also connects to charger `/QON`; see
[Power Management](power_management.md) for shutdown and hold-to-restart behavior.

### Touch Recovery

Every five seconds, the worker checks INT through the selected GPIO backend.
If LOW, it clears the CAP1203 latch and checks again; if still LOW, it
reinitializes the controller. V2's check uses I2C through the expander.
Noisy channels are excluded from events and recalibrated when supported.

### Sleep And Wake

`start()` and `stop()` do not change CAP1203 power mode. Touch remains active
in ESP sleep but is not a wake source. Interactive boot initializes CAP1203
and clears stale interrupts; timer wakes leave it alone.

Button-wake boot enables one-shot Power press suppression for a 500 ms window
after interrupt setup. A suppressed press arms no long-press timer, so its
release also emits no event. Expiry prevents a stale flag from swallowing a
later press. Touch events are never suppressed by this flag.

## Edge Cases / Errors

- A failed initial CAP1203 read is acknowledged and discarded without an event.
- A failed expander level read does not count as an asserted touch interrupt.
- Event posting is nonblocking; a full queue drops the event.
- `pause()` suppresses event delivery while the worker continues processing.

[Host tests](../tests/go_input.tests.cpp) cover gesture timing, board mappings,
polling, wake suppression, and touch recovery with mocked hardware.
