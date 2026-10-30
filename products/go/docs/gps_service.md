# GPS Service

AirGradient Go's GPS service owns acquisition and timed sleep of the TAU1113.
`GpsDriver` handles CASIC commands and NMEA parsing; `GpsService` manages the
worker, wake deadline, latest fix, clock sync, and `GpsFixUpdate` events.

## Files

| File | Purpose |
|---|---|
| [`gps_driver.h`](../main/gps/gps_driver.h) / [`gps_driver.cpp`](../main/gps/gps_driver.cpp) | UART, CASIC commands, NMEA parsing |
| [`gps_service.h`](../main/gps/gps_service.h) / [`gps_service.cpp`](../main/gps/gps_service.cpp) | Acquisition, sleep maintenance, synchronous fast-path read |
| [`gps_types.h`](../main/gps/gps_types.h) | Data types, invalid sentinels, field validation |
| [`gps_power_policy.h`](../main/gps/gps_power_policy.h) | Sleep intervals and tracking reset threshold |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `AirgradientSerial` | `airgradient-serial` | Injected UART transport |
| `libnmea-esp32` | `components/libnmea-esp32` | NMEA parsing |
| `RTOS` | `airgradient-common` | Tasks, notifications, mutexes, retained clock |
| `Event` | [`go_events.h`](../main/go_events.h) | Posts `GpsFixUpdate` to the orchestrator |

## Public API

| Method | Behavior |
|---|---|
| `start()` | Start acquisition; wait for any existing sleep interval first |
| `start_sleeping(interval_ms)` | Maintain sleep, sending another interval whenever GPS is expected to wake |
| `stop()` | Join the worker and close UART; leave the receiver's power state unchanged |
| `request_sleep(duration_ms)` | Stop the worker and send one sleep command; an existing sleep interval is not replaced |
| `sleep_remaining_ms()` | Expected remaining sleep duration, or zero |
| `sleep_deadline_ms()` | Absolute retained-clock deadline for RTC persistence, or zero if unscheduled |
| `get_latest_fix()` | Return the mutex-protected fix snapshot |
| `set_posting_interval_ms(ms)` | Change event cadence |
| `set_aiding_data(data)` | Queue approximate position/time for A-GNSS injection |

`start()` and `start_sleeping()` are idempotent for the same worker mode.
Success means the task started, not that GPS is ready or has a fix. Lifecycle
calls have one owning caller; `stop()` interrupts a pending wake wait.

### Configuration

| Symbol | Default | Purpose |
|---|---|---|
| `Config::baud_rate` | `115200` | Baud used for active acquisition |
| `Config::expected_baud` | `9600` | First baud tried when connecting |
| `Config::wake_deadline_ms` | `0` | Restore a scheduled wake from `RtcAppState` |
| `Config::posting_interval_ms` | `5000` | Fix event interval |
| `Config::task_stack_size` | `4096` | Worker stack bytes |
| `Config::task_priority` | `3` | Worker priority, below display |
| `GPS_IDLE_SLEEP_MS` | `30000` | Inactive GPS interval in Portable/Stationary; Offline fallback when no next wake interval is supplied |
| `GPS_RESET_THRESHOLD_MS` | `30000` | Offline tracking wait/reset threshold (30 seconds); provisional pending boot-time measurement |
| `GPS_RESET_CONFIRM_MS` | `1000` | Wait after an accepted reset request before falling back |

## Behavior

### Sleep Policy

Inactive GPS uses CASIC `CFG-SLEEP`, rather than only stopping the GNSS engine.

| GPS State | Policy |
|---|---|
| Inactive in Portable/Stationary: Always Off, or On When Tracking without a session | Repeated 30-second sleep intervals |
| Inactive in Offline: Always Off, or On When Tracking without a session | Interactive maintenance uses time until the next measurement; fast-path boot requests one full measurement interval before gas conditioning |
| Active: Always On, or On When Tracking with a Recording/Paused session | Keep the receiver running, including throughout ESP deep sleep |

The service owns wake waiting and repeated sleep commands. Before ESP deep
sleep, the app requests one sleep interval only if GPS is inactive. An existing
GPS deadline is preserved: it does not shorten ESP sleep or make the ESP wait
for GPS to wake. The fast path makes a final request using the ESP sleep duration
only if the earlier deadline has expired. Successful transmission records an
expected wake deadline; it does not confirm physical receiver sleep.
Charging-only boot does not manage GPS; shutdown does not send a GPS sleep command.
When Offline has no next wake interval, the policy falls back to `GPS_IDLE_SLEEP_MS`.

The wake deadline uses `RTOS::get_retained_time_ms()` and is stored as a
`uint64_t` in `RtcAppState::gps_wake_deadline_ms`. It is an absolute monotonic
time, not UTC or a sleep duration. Baud is not stored in RTC: boot infers it
from GPS mode and tracking state, with a fallback to the other baud.

### Settings Changes While GPS Sleeps

Changing GPS settings does not request a system reset. When the new setting
activates GPS, acquisition waits for the existing sleep deadline, including
when a tracking session is recording or paused. Selecting On When Tracking
without a session leaves GPS inactive. Selecting Always Off stops acquisition,
clears the cached fix, and starts sleep maintenance without ending tracking.

Changing operating mode also preserves any existing GPS sleep deadline. On
leaving Offline for Portable or Stationary, the remaining Offline interval can
therefore exceed 30 seconds. Subsequent inactive sleep requests use the new
mode's policy. The battery-powered reset described below applies only to
starting a new tracking session.

### Connection and Acquisition

A fresh or sleep-woken receiver is expected at 9600 baud. Connecting tries the
expected baud, then the other supported baud, using a MON-VER response to
identify the module. Sending sleep does not switch the module to 115200.
Active acquisition switches to 115200 when needed, including Always On without
tracking. Receiver configuration polls MON-VER, enables ephemeris saving, and
sets/polls the constellation mask before GNSS start.

The worker waits for the scheduled wake with an interruptible notification
wait, capped at one hour per wait. It then configures acquisition and reads
NMEA. Events are posted at the configured interval even for `NoFix`, so the UI
can clear stale location data. Consumers validate fix and individual fields.
The first valid GPS timestamp synchronizes system time through the RTOS layer.

A-GNSS data can be supplied before or during acquisition. The worker copies
pending aiding data under its mutex, then sends CASIC AID-POS/AID-TIME outside
the lock. Aiding time can seed the system clock until GPS supplies valid time.

### Fast-Path Read

`GoApp` calls `gps_read_once()` after sensor initialization, before gas
conditioning, so UART work overlaps PM warmup without starting a GPS task.
A receiver whose wake deadline is still in the future is left alone. Otherwise, the helper connects, switches/configures the receiver
if needed, and reads until both fix and position are valid, the timeout expires,
or the power-button abort flag is set. It closes UART on return and leaves
GPS running. The normal read window is three seconds; lack of a position does
not stop the tracking session.

### Starting Tracking While GPS Sleeps

Portable and Stationary wait for the remaining interval without rebooting.
Offline may use a BQ system power reset when the remaining wait exceeds the
threshold and a fresh BMS status confirms battery power (`None` or `OtgMode`).
External input, unknown power status, or a shorter wait uses the normal wait
path. USB serial connectivity alone does not mean external power is present.

Before reset, the app commits only the `trk_start` intent flag and fully paints
“Starting tracking...”. No route exists yet. After reboot, it clears the flag,
creates the session, shows “Started” with its ID, and enters the cold
Offline fast path. Recovery failure enters interactive boot with “Tracking not
started”. Confirmation means the session exists, not that GPS has a position.
See [Tracking Lifecycle](orchestrator.md#tracking-lifecycle) and
[Boot Path Selection](hardware_init.md#boot-path-selection).

## Edge Cases / Errors

- `request_sleep(0)` is rejected. Successful transmission means the command
  left UART, not that hardware sleep was confirmed; no sleep ACK is awaited.
- Failed sleep commands do not create a deadline. Sleep maintenance retries
  after one second; one-shot callers receive `false`.
- Failure to connect at both bauds or switch baud ends acquisition setup.
  A wake deadline alone does not prove the receiver is awake or tracking.
- `stop: task only (GNSS keeps tracking)` logs worker shutdown, not a receiver
  status check. The module may already be asleep or acquisition may have failed.
- If the reset request fails or power does not cycle, the app clears the intent
  and starts tracking while waiting. If clearing fails, tracking start fails.
