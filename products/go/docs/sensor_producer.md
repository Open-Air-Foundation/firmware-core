# Sensor Producer

Independent RTOS task that drives sensor measurements for AirGradient Go.
Wraps the shared `SensorManager` component: the orchestrator signals it with
an iteration count, sensor groups, and measurement origin via RTOS task
notification; the task blocks inside `SensorManager::start_measures()` for
the full averaging window, then posts a
`SensorDataReady` event to the orchestrator queue.

The same task also runs blocking CO2 calibration requests and posts their
completion as `Co2CalibrationDone`.

`SensorProducer` holds only a `SensorManager` reference and has no knowledge
of which sensors are wired — that is the product wiring layer's responsibility
(`go_hardware_board.cpp`).

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_sensor_producer.h` | `SensorProducer` class declaration, `Config` struct |
| `products/go/main/go_sensor_producer.cpp` | Task loop, notification handling, event posting |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `SensorManager` | `airgradient-sensors` (`services/sensor_manager.h`) | Blocking multi-iteration sensor averaging |
| `Sensors` struct | `airgradient-sensors` (`services/sensor_manager.h`) | HAL pointer table injected into `SensorManager` by wiring layer |
| `MeasuresAGo` | `airgradient-common` (`measures_types.h`) | Averaging result, wrapped by `SensorEventData` |
| `RTOS` | `airgradient-common` (`rtos.h`) | `task_create()`, `task_delete()`, `queue_send()`, `task_notify_send()`, `task_notify_wait()` |
| `go_events.h` | product | `SensorEventData`, `MeasurementOrigin`, measurement and PM boundary events |
| `Co2CalibrationResult` | `airgradient-sensors` (`sensor_manager.h`) | Completion payload for asynchronous CO2 calibration |

## Public API

| Method | Purpose |
|---|---|
| `start()` / `stop(sleep_pm)` | Manage the producer task and PM shutdown policy |
| `request_measurement(iterations, groups, origin = MeasurementOrigin::Scheduled)` | Request readings tagged as scheduled or manual refresh |
| `request_prepare()` | Wake PM, run blocking warmup, and report preparation boundaries |
| `request_pm_sleep()` | Sleep PM and report completion before the orchestrator isolates the bus |
| `request_self_test()` | Run the Hardware Test sensor sweep |

See [`go_sensor_producer.h`](../main/go_sensor_producer.h) for full signatures
and calibration/settings requests.

## AGo Sensor Wiring

The wiring layer (`go_hardware_board.cpp`) populates a `Sensors` struct and passes it to
`SensorManager`. `SensorProducer` never touches this struct directly.

| `Sensors` field | Driver | Notes |
|---|---|---|
| `temp_hum` | `SHT40` | Dedicated temperature + humidity sensor, V1 only |
| `co2` | `S12`, `SCD4x`, or `STCC4` | Probed in order at boot; first detected wins |
| `pms_a` | `SPS30` | Single particulate matter sensor |
| `pms_b` | `nullptr` | No second PM sensor on AGo |
| `tvoc_nox` | `SGP41` | TVOC + NOx sensor |
| `pressure` | `DPS368` | Barometric pressure + altitude sensor |
| `o3_no2` | `nullptr` | No AlphaSense electrodes on AGo |

`temp_hum_a` uses the product fallback order `DEDICATED` → `CO2` →
`PRESSURE`. On V1, SHT40 is probed only after board-variant detection. If
SHT40 is unavailable, SCD4x/STCC4 CO2-integrated T/RH can supply temperature
and humidity; S12 cannot. DPS368 is the final temperature-only fallback.

Battery management is handled separately by `PowerService` via the
`airgradient-bms` component and is not part of the `Sensors` struct.
Battery data comes from `PowerService::poll_bms()`, not from `SensorManager`.

## Configuration

`SensorProducer::Config` fields:

| Field | Default | Notes |
|---|---|---|
| `task_stack_size` | `4096` | RTOS task stack in bytes; tune at integration time |
| `task_priority` | `5` | Below input task; above idle |
| `co2_abc_days` | `7` | Automatic background calibration period applied before sensor warmup; `-1` disables it |
| `tvoc_learning_offset` | `12` | VOC gas-index learning-time offset in whole hours |
| `nox_learning_offset` | `12` | NOx gas-index learning-time offset in whole hours |

## Usage

```cpp
#include "go_sensor_producer.h"
#include "services/sensor_manager.h"

// Sensor drivers and Sensors struct owned by the wiring layer.
Sensors sensors{};
sensors.temp_hum = &sht40_driver;
sensors.co2      = &co2_driver;
sensors.pms_a    = &pms5003_driver;
sensors.tvoc_nox = &sgp41_driver;
// pms_b, o3_no2 remain nullptr

SensorManager sensor_manager(sensors);

SensorProducer::Config cfg{};  // defaults: stack=4096, priority=5
SensorProducer sensor_producer(sensor_manager, event_queue, cfg);
sensor_producer.start();

// Orchestrator triggers a measurement cycle when a sensor timer fires:
sensor_producer.request_measurement(1, SensorGroup::All);  // single iteration, all sensors

// Orchestrator triggers PM warmup after powering on the sensor via GPIO:
sensor_producer.request_prepare();  // blocks task for ~10 s warmup

// BLE, local HTTP, or UI requests CO2 calibration asynchronously:
sensor_producer.request_co2_calibration();

// Short sleep: stop the task but keep SPS30 measuring for a warm wake.
sensor_producer.stop(false);

// Long sleep or PM power-off: stop the task and put SPS30 to sleep.
sensor_producer.stop(true);
```

The orchestrator selects the shutdown mode from the planned sleep duration.
Short sleeps that hold PM power pass `sleep_pm=false` and persist
`sensors_warm=true`. Long sleeps pass `sleep_pm=true` and persist
`sensors_warm=false`, ensuring that the next timer boot performs the full PM
warmup.

## Event Output

`SensorProducer` posts `EventType::SensorDataReady` to the orchestrator queue
once per measurement cycle, after `start_measures()` returns.

For CO2 calibration, `handle_calibration()` calls the blocking
`SensorManager::calibrate_co2()` and attempts one zero-wait
`Co2CalibrationDone` carrying `Success`, `Unsupported`, or `Failed`; a full
central queue drops the completion without retry. The orchestrator maps a
delivered completion to the on-device snackbar and, when connected, the BLE
Config command result.

The event union contains `SensorEventData sensor_data`, with two members:

| Member | Meaning |
|---|---|
| `measures` | `MeasuresAGo` result; unavailable sensor fields retain invalid sentinels |
| `origin` | `MeasurementOrigin::Scheduled` (default) or `MeasurementOrigin::Refresh`, copied from the request |

The producer does not infer origin from timing. The orchestrator tags the
request; a scheduled measurement reused for a shake keeps `Scheduled`.
Both origins update current readings, while only `Scheduled` writes chart
cache and route points.

At boot and for `request_prepare()`, the producer posts `PmPreparationStarted`,
calls `pm_wake()` and blocking `warmup()`, then posts `PmPrepared`.
`request_pm_sleep()` posts `PmSensorAsleep` after the sleep call. These events
report operation boundaries, not guaranteed sensor validity. Measurement and
PM boundary posts wait for central queue space. Calibration completion keeps
its existing zero-wait delivery.

## Iteration Count and Sensor Groups

On AGo, the orchestrator always requests 1 iteration. The product relies on
each driver's normal single-cycle measurement mode, so multi-iteration firmware
averaging adds no meaningful data quality. The per-iteration 2 s delay is
skipped when `iterations == 1`.

The `SensorGroup` parameter controls which sensor categories are polled:

| Group | Sensors |
|---|---|
| `PM` | `pms_a`, `pms_b` |
| `Other` | `temp_hum`, `co2`, `o3_no2`, `pressure` |
| `TvocNox` | `tvoc_nox` (SGP41 read + algorithm step when configured) |
| `All` | All of the above (default) |

The task encodes requests into the `uint32_t` notification: iterations in
bits 0–7, group mask in bits 8–15, and `NOTIFY_REFRESH` in bit 16 when origin
is `Refresh`. An unset origin bit means `Scheduled`. On decode, zero iterations
defaults to 1, and `SensorGroup::None` defaults to `All`.

Six sentinel values use the remaining notification space:

| Sentinel | Value | Purpose |
|---|---|---|
| `NOTIFY_CALIBRATION` | `UINT32_MAX` | CO2 background calibration |
| `NOTIFY_PREPARE` | `UINT32_MAX - 1` | PM sensor warmup after power cycle |
| `NOTIFY_PM_SLEEP` | `UINT32_MAX - 2` | Put the PM sensor to sleep, then post `PmSensorAsleep` |
| `NOTIFY_SELF_TEST` | `UINT32_MAX - 3` | Run one all-sensor self-test and post `SensorTestDone` |
| `NOTIFY_CO2_ABC_PERIOD` | `UINT32_MAX - 4` | Apply `Config::co2_abc_days` |
| `NOTIFY_TVOC_NOX_LEARNING_OFFSETS` | `UINT32_MAX - 5` | Apply `Config` gas-index learning offsets |

The producer applies the configured ABC period before normal sensor warmup. At
runtime, the orchestrator sends ABC changes through the producer and receives a
`Co2AbcPeriodDone` event after application, so the producer remains the sole
owner of CO2 I2C access.
S12 and SCD41 persist the setting in sensor EEPROM only when the requested
period differs from the current period or the sensor's automatic calibration is
disabled.

The producer configures the SGP41 VOC and NOx algorithms with their independent
learning offsets before enabling the sampler. Runtime changes yield a
`TvocNoxLearningOffsetDone` event. Applying an offset preserves every other
Sensirion tuning parameter and does not reset either algorithm, so it does not
introduce a new 45-second blackout.

## Internal Architecture

### Task Loop

After warmup, the producer enables a gas-index sampler when an SGP41
sensor is wired and `configure_tvoc_nox_index()` succeeds. The sampler
converts the indefinite `task_notify_wait` into a timeout-driven loop so
the Sensirion algorithm is fed at a fixed cadence (Kconfig-configurable,
default 10 s) independent of the measurement interval.

```text
SensorProducer::run():
  warmup()
  sampler_enabled = has_tvoc_nox_sensor() && configure_tvoc_nox_index(TICK_MS)

  while _running:
    timeout = time_until_next_tick if sampler_enabled else UINT32_MAX
    notified = task_notify_wait(&notify_value, timeout)

    if !_running: break

    if notified:
      dispatch calibration / prepare / PM sleep / self-test / measurement

    if sampler_enabled && tick_due:
      handle_sampler_tick()
```

#### `handle_measurement(notify_value)`

When the sampler is active, the `TvocNox` bit is stripped from the
requested group mask so measurement notifications never read SGP41 at an
irregular cadence. After `start_measures()` returns, the cached TVOC/NOx
from the most recent sampler tick is spliced into the result before posting
`SensorDataReady`. When the sampler is inactive, the original mask is
preserved so `All`/`TvocNox` still reads raw SGP41 values.

The latest valid `temp_hum_a` from each measurement is cached for
compensation push.

#### `handle_calibration()`

Calls `SensorManager::calibrate_co2()` synchronously on the producer task, then
posts the result to the central queue with zero wait. `Unsupported` means no CO2
sensor is selected or the selected driver does not support manual calibration.
S12 and SCD4x support it; the current STCC4 driver does not.

#### `handle_sampler_tick()`

Pushes cached temp/hum compensation to the SGP41 driver (if available),
then calls `start_measures(1, TvocNox)` and caches the result in
`_last_tvoc_nox`. Missed ticks are skipped, not replayed.

#### Offline (fast path)

In Offline mode, no `SensorProducer` is created. The fast path calls
`start_measures(1, All)` directly on `SensorManager`. The algorithm is
never configured (`_index_configured == false`), so index fields stay at
invalid sentinels. A retained UX placeholder in `execute_fast_path()`
(`go_app.cpp`) overwrites the index fields with raw ticks so the display
still shows a value.

### Orchestrator Signalling

```cpp
// Orchestrator calls this when a sensor timer fires:
sensor_producer.request_measurement(1, groups);

// Orchestrator calls this to warm up PM sensor after power-on:
sensor_producer.request_prepare();

// Internally every request uses the same overwrite notification slot:
uint32_t value = (static_cast<uint32_t>(groups) << 8) | iterations;
// Manual refresh requests also set NOTIFY_REFRESH (bit 16).
RTOS::task_notify_send(_task_handle, value);
// overwrites any unconsumed notification
```

All requests share one task-notification value with overwrite semantics. There
is no request FIFO, busy response, in-flight calibration owner, or duplicate
gate. A new request replaces any unconsumed measurement, prepare, sleep,
self-test, or calibration request. While a handler is blocking, at most the
latest request remains latched for the next loop iteration; repeated requests
can therefore coalesce and do not guarantee one completion per call.

For scheduled measurements and shake refresh, the orchestrator tracks one
pending measurement and PM preparation/sleep state. It waits for the relevant
PM completion event before requesting a refresh measurement, and reuses an
already-requested measurement instead of sending another. The notification
transport remains unchanged; there is no command queue.

PM preparation still blocks this task in `warmup()`. Gas-index sampler ticks
run between command handlers, not during warmup. Refresh results use the
latest sampler cache when the sampler is active.

### Task Duration

The sensor task is blocked for the full measurement window:
`iterations × CONFIG_AVERAGING_ITERATION_INTERVAL_MS`.  During this time the
orchestrator continues processing other events (GPS fixes, button presses,
timers) uninterrupted.  This is the primary reason for the independent task —
`SensorManager::start_measures()` is inherently blocking.

## stop() Behaviour

`stop()` sets `_running = false`, synchronously asks `SensorManager` to sleep the
PM sensor, sends a zero-value task notification to
unblock `task_notify_wait()`, waits briefly (10 ms), then forcefully deletes
the task via `RTOS::task_delete(_task_handle)`. The forced delete is safe
because `SensorManager` holds no mutexes, so there is no risk of deadlock or
resource leak.

The task entry function blocks indefinitely after `run()` returns (rather than
self-deleting) to avoid a double-delete race with `stop()`.

After `stop()` returns, `_task_handle` is `nullptr`.  A subsequent call to
`start()` creates a fresh task.

## Initialization Order

The wiring layer must follow this sequence:

1. Initialise I2C/UART buses.
2. Construct concrete driver instances (`SHT40` on V1, CO2, SPS30, SGP41).
3. Call `driver.init()` on each.
4. Populate a `Sensors` struct (unused pointers set to `nullptr`).
5. Construct `SensorManager(sensors)`.
6. Construct `SensorProducer(sensor_manager, event_queue, config)`.
7. Call `sensor_producer.start()`.

## Thread Safety

`SensorProducer` has no shared mutable state accessed from multiple threads
other than:

- `_running` (`volatile bool`): written by the orchestrator via `stop()` and
  read by the sensor task.  No mutex needed — the flag is used only to signal
   exit; the task is deleted immediately after by `RTOS::task_delete()`.
- `_task_handle`: written by `start()` / `stop()` (orchestrator thread) and
  read by `request_measurement()` (also orchestrator thread).  Both callers
  are in the same thread, so no protection is required.

## Testability

RTOS task notification and queue calls are no-ops under `TEST_HOST`, allowing
the file to compile in native host test builds.

For host testing:

- Inject a mock `SensorManager` (or construct one with mock sensor HALs via
  the `Sensors` struct pattern).
- Replace the RTOS queue with a test double or inspect the `Event` struct
  directly.
- `start()` is a no-op in `TEST_HOST` mode (`RTOS::task_create` returns
  `false`); call `run()` directly in tests to exercise the task loop.
- Verify `request_measurement(0)` causes the task to use 1 iteration.
- Verify `SensorDataReady` carries the readings and the requested origin in
  `SensorEventData`.
