# GPS Service

AirGradient Go's GPS service owns acquisition and timed sleep of the TAU1113.
`GpsDriver` handles CASIC commands and NMEA parsing; `GpsService` manages the
worker, wake deadline, latest fix, clock sync, and `GpsFixUpdate` events.

## Files

| File | Purpose |
|---|---|
| [`gps_driver.h`](../main/gps/gps_driver.h) / [`gps_driver.cpp`](../main/gps/gps_driver.cpp) | UART, CASIC commands, NMEA parsing |
| [`gps_service.h`](../main/gps/gps_service.h) / [`gps_service.cpp`](../main/gps/gps_service.cpp) | Acquisition and sleep maintenance |
| [`gps_types.h`](../main/gps/gps_types.h) | Data types, invalid sentinels, field validation |
| [`gps_power_policy.h`](../main/gps/gps_power_policy.h) | Sleep intervals by operating mode |

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
| `Config::fast_path_sleep_probe` | `false` | Use the repeated 9600-only probe for fast-path sleep requests |
| `Config::posting_interval_ms` | `5000` | Fix event interval |
| `Config::task_stack_size` | `4096` | Worker stack bytes |
| `Config::task_priority` | `3` | Worker priority, below display |
| `GPS_IDLE_SLEEP_MS` | `30000` | Inactive GPS interval in Portable/Stationary |
| `GPS_ENERGY_SAVING_SLEEP_MS` | `UINT32_MAX` | Energy Saving sleep duration in milliseconds |

## Behavior

### Sleep Policy

Inactive GPS uses CASIC `CFG-SLEEP`, rather than only stopping the GNSS engine.

| GPS State | Policy |
|---|---|
| Inactive in Portable/Stationary: Always Off, or On When Tracking without a session | Repeated 30-second sleep intervals |
| Energy Saving (Offline), any GPS setting | Request maximum-duration sleep; tracking and GPS Test are unavailable |
| Active in Portable/Stationary: Always On, or On When Tracking with a Recording/Paused session | Keep the receiver running |

The service owns wake waiting and repeated sleep commands. Interactive callers
preserve an existing deadline; Energy Saving timer wakes explicitly probe again
because the receiver may have reset or woken independently. Failed probes retain
the old deadline and do not prevent ESP sleep. Charging-only boot also requests
long GPS sleep when Energy Saving is saved. Shutdown sends no GPS sleep command.

The wake deadline uses `RTOS::get_retained_time_ms()` and is stored as a
`uint64_t` in `RtcAppState::gps_wake_deadline_ms`. It is an absolute monotonic
time, not UTC or a sleep duration. Baud is not stored in RTC: boot infers it
from GPS mode and tracking state. Normal connection probing permits an alternate
baud; the Energy Saving timer path uses only 9600.

### Settings Changes While GPS Sleeps

Changing GPS settings does not request a system reset. When the new setting
activates GPS, acquisition waits for the existing sleep deadline, including
when a tracking session is recording or paused. Selecting On When Tracking
without a session leaves GPS inactive. Selecting Always Off stops acquisition,
clears the cached fix, and starts sleep maintenance without ending tracking.

Entering Energy Saving ends Recording or Paused sessions and disables GPS
regardless of the saved GPS setting. Persisted transitions into or out of this
mode request a charger system reset after saving settings and backing up cache.
Battery power permits the reset; USB can keep the system powered. If execution
continues, the new mode runs with the existing GPS deadline.

### Connection and Acquisition

A fresh or sleep-woken receiver is expected at 9600 baud. Normal connection tries the
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

### Fast-Path Sleep Probing

The Energy Saving timer path does no GPS acquisition or route recording.
After sensor initialization it probes at 9600 baud, sending MON-VER every 300 ms
for up to five seconds. A response ends probing and triggers the long sleep
command; 115200 is never tried on this path.

If the early request fails and deep sleep is still planned, the app repeats
this sequence after measurement, storage, and display. It calculates remaining
ESP sleep time after that retry, so probing is included in the measurement
interval. A successful early request skips the retry. Normal interactive probing
still tries the expected baud and then the alternate baud.

### Starting Tracking While GPS Sleeps

Portable and Stationary start the route without resetting; GPS acquisition waits
for its existing deadline. Sensor points can be recorded before a valid GPS fix.
Energy Saving hides the tracking menu and does not resume retained sessions.

## Edge Cases / Errors

- `request_sleep(0)` is rejected. Successful transmission means the command
  left UART, not that hardware sleep was confirmed; no sleep ACK is awaited.
- Failed sleep commands do not create a deadline. Sleep maintenance retries
  after one second; one-shot callers receive `false`.
- Failure to connect at both bauds or switch baud ends acquisition setup.
  A wake deadline alone does not prove the receiver is awake or tracking.
- `stop: task only (GNSS keeps tracking)` logs worker shutdown, not a receiver
  status check. The module may already be asleep or acquisition may have failed.
- A failed or ineffective mode-change reset leaves the existing GPS deadline
  in force. Leaving Energy Saving can therefore leave GPS unavailable until
  its long sleep ends or receiver power is cycled.
