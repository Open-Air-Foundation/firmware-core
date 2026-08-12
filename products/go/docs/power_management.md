# Power Management Service

Product-specific power management for AirGradient Go. Handles BMS status
polling, battery monitoring, sleep cycle management, RTC state persistence,
and shutdown. Called synchronously by the orchestrator — no independent task.

For AGo, the power service also manages:

- **Session-armed PMID:** `EN_OTG` is armed once during `BQ25629Bms::init()`
  and held for the lifetime of the session. The chip handles buck↔boost
  transitions autonomously on VBUS plug/unplug. `set_pm_power()` drives only
  the EN_PM GPIO (Prototype: SPS30 VDD load switch; V1: SPS30 I2C bus
  isolation), never `EN_OTG`. See ["Why PMID is session-armed"](#why-pmid-is-session-armed)
  below for the cell-OCP rationale
- **EDV (over-discharge) trip:** requests ship mode
  (`ShipModeRequest::OverDischarge`) when cell voltage stays below 2.9 V
  for 3 consecutive polls while on battery. The orchestrator shows a
  warning on `Screen::Info` then calls `shutdown()`
- **Battery-temperature protection:** charging is allowed from 0 °C through
  45 °C. After a temperature or invalid-NTC block, charging resumes only from
  2 °C through 43 °C. Discharging is allowed from -10 °C through 60 °C;
  temperatures outside that range request the corresponding
  `ShipModeRequest::UnderTemperature` or `ShipModeRequest::OverTemperature`.
  An invalid NTC reading disables charging only and does not request shutdown
- **Full-charge pause:** disables charging when the battery is full and
  USB is present. Resumes when SOC drops to 95 %. V1 detects full via
  BQ27427 FC flag; Prototype falls back to `ChargeTerminationDone` +
  100 % SOC. Interacts safely with the thermal charge guard
- **BMS watchdog wrapper:** `set_watchdog_timeout_ms()` forwards to the
  HAL without adding policy

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_power.h` | `PowerSnapshot` struct, `PowerService` class declaration |
| `products/go/main/go_power.cpp` | BMS polling, sleep entry/exit, RTC state, fast-path boot logic |

## Dependencies

| Dependency | Component | Usage |
|---|---|---|
| `BmsDevice` | `airgradient-bms` (HAL) | BMS telemetry, status, battery %, watchdog reset, PMID, charging, ship mode |
| `FuelGaugeDevice` | `airgradient-bms` (HAL) | Optional fuel gauge runtime polling (V1 only). Attached via `set_fuel_gauge()` |
| `gpio::Hal` | `airgradient-gpio` | Configure GPIO wake sources for deep sleep |
| `go_types.h` | product | `RtcAppState`, `WakeCause`, `LockState` |
| `go_settings.h` | product | `GoSettings` for interval-based sleep decisions |
| `esp_sleep.h` | ESP-IDF | `esp_sleep_*` functions (deep sleep) |

## PowerSnapshot Fields

`PowerSnapshot` aggregates BMS data for the orchestrator and display.  All
fields default to invalid sentinels (`BmsInvalid::VOLT` / `-1.0f` / `false`).

| Field | Type | Sentinel | Description |
|---|---|---|---|
| `battery_voltage` | `float` | `-1.0f` | Battery voltage (V) from ADC |
| `charging_voltage` | `float` | `-1.0f` | Charging/bus voltage (V) from ADC |
| `battery_percentage` | `float` | `-1.0f` | Canonical SOC (0--100%). Source depends on `battery_percent_source` |
| `charging_status` | `BmsChargingState` | `Unknown` | Enumerated charging state |
| `critical` | `bool` | `false` | Set when `battery_percentage` is valid and below `BATTERY_CRITICAL_PERCENT` (5 %) |
| `charger_status` | `BmsStatus` | all defaults | Full charger status: power source, regulation flags, fault flags |
| `telemetry` | `BmsTelemetry` | all invalid | Full ADC telemetry: currents, system/PMID voltages, thermistor, die and battery temperature |
| `battery_percent_source` | `BatteryPercentSource` | `Unknown` | Where `battery_percentage` came from: `FuelGauge` or `BatteryCharger` |
| `fg_soc_percent` | `uint8_t` | `255` | FG SOC (0--100%). V1 with FG attached only |
| `fg_voltage_mv` | `uint16_t` | `UINT16_MAX` | FG battery terminal voltage (mV) |
| `fg_current_ma` | `int16_t` | `INT16_MIN` | FG average current (signed mA) |
| `fg_power_mw` | `int16_t` | `INT16_MIN` | FG average power (signed mW) |
| `fg_remaining_capacity_mah` | `uint16_t` | `UINT16_MAX` | FG remaining capacity (mAh) |
| `fg_full_charge_capacity_mah` | `uint16_t` | `UINT16_MAX` | FG full charge capacity (mAh) |
| `fg_internal_temperature_c` | `float` | `-273.16` | FG die temperature (C) |
| `fg_flags` | `uint16_t` | `0` | FG flags register (decoded via `FgFlags::FC`, `CHG`, `DSG`, etc.) |
| `full_charge_paused` | `bool` | `false` | True when charging is paused because battery is full + USB present |
| `ship_mode_request` | `ShipModeRequest` | `None` | Non-`None` when a safety trip requires warning and ship mode: `OverDischarge`, `OverTemperature`, or `UnderTemperature`. Invalid NTC alone never sets this field |

### SOC Source Preference

`poll_bms()` prefers the fuel gauge SOC when available:

1. If an FG is attached (`_fg != nullptr && _fg->ready()`) and
   `read_soc_percent()` succeeds: `battery_percentage` comes from the FG,
   `battery_percent_source = FuelGauge`
2. Otherwise: `battery_percentage` comes from `_bms.get_battery_percentage()`
   (BQ25629 voltage-curve estimate), `battery_percent_source = BatteryCharger`

FG telemetry fields are read independently. A partial FG read failure blanks
only the failed field; the remaining FG fields populate normally.

### Logging

`poll_bms()` emits three log lines per poll via `_log_poll_snapshot()`:

1. **Charger status** — percentage, source, voltages, critical flag,
   charging state, power source, `full_chg_paused`, regulation/fault flags
2. **BQ25629 ADC telemetry** — input/battery current, system/PMID voltage,
   thermistor, die/battery temperature
3. **FG telemetry** (V1 only, when FG attached and ready) — SOC, voltage,
   current, power, remaining/full capacity, internal temperature, decoded
   flags (`FgFlags::FC|CHG|DSG|...`), thermal charge-off state. ITPOR
   flag triggers a separate `AG_LOGW`

### FG Flag Decode

`FgFlags` (in `bms_types.h`) provides named constants for the BQ27427
Flags register bits:

| Flag | Bit | Description |
|---|---|---|
| `DSG` | 0 | Discharging |
| `BAT_DET` | 3 | Battery detected |
| `CFGUP` | 4 | Config update mode active |
| `ITPOR` | 5 | Gauge POR / reset detected |
| `OCVTAKEN` | 7 | OCV measurement taken |
| `CHG` | 8 | Charge condition |
| `FC` | 9 | Full Charge |

## Critical Battery Threshold

```cpp
static constexpr float PowerService::BATTERY_CRITICAL_PERCENT = 5.0f;
```

This is a **fixed constant**, not a user-configurable setting.  When the
orchestrator receives a `PowerSnapshot` with `critical == true` it should:

1. Show a low-battery warning on the display.
2. Initiate an automatic shutdown to protect the battery cell.

## PowerService Config

Passed to the constructor; provides pin assignments and the deep-sleep
threshold:

| Field | Type | Default | Description |
|---|---|---|---|
| `pin_wake_button_power` | `int` | — | GPIO number for Button Power deep-sleep wake |
| `pin_wake_button_boot` | `int` | — | GPIO number for Button Boot deep-sleep wake (`-1` on ESP32-C5 — GPIO28 is not RTC-capable) |
| `pin_ext_wdt` | `int` | `-1` | External watchdog GPIO (`-1` = disabled); pulsed HIGH 20 ms on reset |
| `deep_sleep_threshold_ms` | `int` | `5000` | Minimum sleep duration (ms) to bother entering deep sleep; shorter intervals stay awake. AGo sets this to `5000` |
| `pin_pm_power` | `int` | `-1` | PM sensor power-enable GPIO (`-1` = no GPIO hold during sleep) |
| `pm_power_on_level` | `uint8_t` | `1` | GPIO level meaning "PM on". Prototype: `1` (active-high); V1: `0` (active-low). Set from `pm_power_on_level(variant)` at `PowerService` construction |
| `sensor_hold_max_sleep_ms` | `uint32_t` | `20000` | Maximum sleep duration (ms) for which the PM sensor power GPIO is held HIGH during deep sleep. Above this threshold the sensor powers off normally |
| `pm_sleep_threshold_ms` | `uint32_t` | `20000` | Minimum measurement interval (ms) to power-cycle the PM sensor between measurements in non-Offline modes. Accounts for ~10 s warmup plus minimum off-time |

## Sleep Type Selection

`decide_sleep(settings, lock_state, mode, awake_ms)` is pure logic (no
platform calls, testable on host). Returns `SleepDecision {type, duration_ms}`:

```text
Not Offline mode → {None, 0}   (only Offline mode sleeps)
Unlocked         → {None, 0}   (never sleep while user is active)

sleep_ms = (measure_interval_seconds * 1000) - awake_ms   (clamped to 0)

sleep_ms >= deep_sleep_threshold_ms → {Deep, sleep_ms}
sleep_ms <  deep_sleep_threshold_ms → {None, 0}   (stay awake)
```

The single `measure_interval_seconds` (always ≥ 1) determines the sleep
duration directly. `awake_ms` is subtracted so the total cycle (awake +
sleep) matches the configured interval.

## Sleep Entry

`enter_sleep(sleep_duration_ms)` — only called when `decide_sleep()` returns `Deep`:

1. If `should_hold_pm_sensor(sleep_duration_ms)` is true (sleep < `sensor_hold_max_sleep_ms`
   and `pin_pm_power >= 0`):
   - `gpio_hold_en(pin_pm_power)` — latch the current output level.  On
     ESP32-C5 (`SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP=1`) per-pin hold
     automatically persists during deep sleep; no global
     `gpio_deep_sleep_hold_en()` call is needed
   - The PM sensor fan keeps spinning; the sensor stays warm
2. Calls `configure_wake_sources(sleep_duration_ms)`:
   - Timer: `esp_sleep_enable_timer_wakeup()` (µs)
   - Buttons: `esp_sleep_enable_ext1_wakeup()` with a combined bitmask for
     both button pins (ESP32-C5 target uses EXT1; no EXT0 support on this chip)
3. Calls `esp_deep_sleep_start()` — does **not** return.

The caller must set `RtcAppState::sensors_warm` via
`should_hold_pm_sensor()` and call `save_state()` **before** `enter_sleep()`.
The same decision controls sensor-producer shutdown: held sleeps stop the task
with `sleep_pm=false`, while longer sleeps use `sleep_pm=true`. This keeps the
SPS30's internal measurement state consistent with the persisted warm flag.

### Retained Uptime

The shared `airgradient-common` retained uptime utility stores one
invalid-sentinel-initialized start timestamp in RTC data. `GoApp::run()`
initializes it before boot-path selection. The ESP32-C5 retained monotonic clock
continues through intentional deep sleep, so the reported completed-minute
uptime includes both awake and deep-sleep time without a per-sleep checkpoint or
accumulation of requested sleep durations.

Deep sleep preserves the start timestamp. Power-on, software, OTA, panic,
watchdog, brownout, and other non-deep-sleep resets reload its invalid
initializer and begin a new session. Uptime is not part of `RtcAppState`, and
`PowerService` does not update it during sleep entry.

## PM Sensor Warm-Hold

For short deep sleeps (< `sensor_hold_max_sleep_ms`, default 20 s) the SPS30
power-enable GPIO is held HIGH during sleep via `gpio_hold_en()`. On ESP32-C5,
per-pin hold persists through deep sleep automatically, so no global
`gpio_deep_sleep_hold_en()` call is needed. The fan keeps spinning and the
sensor stays in measurement mode.

On the next timer wake the fast path reads `RtcAppState::sensors_warm`:

| `sensors_warm` | Behavior |
|---|---|
| `true`  | `release_sleep_gpio_holds()` calls `gpio_hold_dis()` on the pin, `SPS30::init(skip_reset=true)` re-attaches without resetting, **warmup loop skipped entirely** — boot drops from ~14–17 s to ~4–7 s |
| `false` | Normal cold boot: full `SPS30::init()` with `CMD_RESET`, 10 s interruptible warmup |

The same warm flag lets SCD4x reattach to periodic measurement retained across
deep sleep. When an unread sample is ready, the driver skips wake, stop,
reinitialization, and start commands. If retained state cannot be confirmed,
the driver performs clean initialization and waits for the first periodic
sample before the fast path continues. S12 requires no special handling, and
STCC4 already detects retained continuous measurement during initialization.

For sleeps ≥ 20 s the SPS30 powers off normally and the full warmup runs on
wake — the power saved by sleeping far outweighs the warmup cost. These cold PM
wakes intentionally use normal SCD4x initialization because the PM warmup gives
the restarted CO2 measurement enough time to become ready.

`should_hold_pm_sensor(duration_ms)` is pure logic (testable on host):

```cpp
return pin_pm_power >= 0 && duration_ms < sensor_hold_max_sleep_ms;
```

`release_sleep_gpio_holds(pin_pm_power)` is a static method called in the
boot path **after** `init_gpio()` has reconfigured the pin as output HIGH.
While the hold is active the pad stays latched HIGH; the GPIO driver writes
the new output configuration to registers underneath. Releasing the hold
then lets the fresh output driver take over with zero power glitch.

## PM Sensor Sleep (Active Mode Power-Cycling)

In non-Offline modes (Portable, Stationary) the device stays awake but
the SPS30 PM sensor may idle for long periods between measurements,
drawing 45–65 mA of continuous fan current.  When the measurement
interval is at or above `pm_sleep_threshold_ms` (default 20 s) the
orchestrator puts the SPS30 into its native **Sleep** mode (`0x1001`)
between measurements, then isolates it from the I2C bus (V1) via
`set_pm_power(false)`.

### Cycle

```text
Measurement completes → on_sensor_data() → request_pm_sleep()
    ↓
SensorProducer sleeps SPS30 (fan stopped) → posts PmSensorAsleep
    → orchestrator set_pm_power(false) (isolate bus)
    ↓
Idle (PM asleep, fan stopped, ~8 µA)
    ↓
Pre-wake timer fires (interval − warmup before next measurement)
    → set_pm_power(true) (connect bus) → request_prepare()
    ↓
SensorProducer pm_wake() + warmup() (~10 s of discard reads)
    — fan spins up during warmup
    ↓
Measurement timer fires → request_measurement(1, All)
    — PM data is stable, fan has spun up during warmup
```

### Eligibility

The orchestrator checks eligibility inline at each decision point —
no persistent mode flag is tracked:

```cpp
_mode != OperatingMode::Offline && should_sleep_pm_sensor(interval_ms)
```

### Edge Cases

| Scenario | Handling |
|---|---|
| **Unlock** | Display shows cached data; PM wakes at the next pre-wake timer |
| **Interval shortened below threshold** | `reschedule_sensor_timer()` calls `set_pm_power(true)` + `request_prepare()` (wakes) |
| **Interval lengthened above threshold** | `reschedule_sensor_timer()` calls `request_pm_sleep()` |
| **Mode change** | `change_mode()` calls `set_pm_power(true)` + `request_prepare()` (connect + wake) |

### Methods

`should_sleep_pm_sensor(measure_interval_ms)` is pure logic (testable on
host):

```cpp
return pin_pm_power >= 0 && measure_interval_ms >= pm_sleep_threshold_ms;
```

`set_pm_power(on)` drives only the EN_PM GPIO. No-op when
`pin_pm_power < 0`. The GPIO level is determined by `pm_power_on_level`
in the config (Prototype: active-high level 1; V1: active-low level 0).
The GPIO is a VDD load switch on Prototype but only an I2C bus isolator
on V1, so on V1 fan current is cut by the SPS30 Sleep command, not here:

- `set_pm_power(true)` — drives the GPIO to the variant-appropriate
  on-level (`pm_power_on_level`)
- `set_pm_power(false)` — drives the GPIO to the off-level (inverted
  `pm_power_on_level`)

PM sleep/wake themselves are owned by the sensor producer:
`request_pm_sleep()` sleeps the SPS30 and posts `PmSensorAsleep` (the
orchestrator then isolates the bus); `request_prepare()` wakes + warms.

`EN_OTG` is **not** touched by `set_pm_power`. It is armed once during
`BQ25629Bms::init()` and the chip handles buck↔boost transitions
autonomously (VBUS present → buck pass-through with `EN_OTG` masked
internally; VBUS absent → boost runs to drive PMID = 5 V from VBAT).

### Why PMID is session-armed

An earlier implementation toggled `EN_OTG` on every PM measurement cycle
to save ~220 µA quiescent on battery when PM was off. This was reverted
because each boost cold-start charges the PMID output capacitance from
~VBAT to 5.1 V with an inrush spike that can exceed 1S cell-protection
OCP. When the cell protection FET opens, VBAT collapses, VSYS collapses,
and the ESP32 fully depowers — observed as `reset_reason=1` (POWERON)
because the brownout detector's RTC-domain latch is wiped along with
everything else. The failure was stochastic per boost cold-start and
correlated with cold-start count rather than SOC or instantaneous load.

The trade is ~220 µA of additional quiescent draw on battery (when PM is
idle) in exchange for indefinite uptime relative to this failure mode.
See `components/airgradient-bms/drivers/bq25629/bq25629_bms.cpp` `init()`
for the one-shot sequence (HIZ off → TS check on → VOTG=5100 →
EN_BYPASS_OTG=0 → EN_OTG=1 → settle → register-readback verify).

The `BmsDevice::set_pmid_enabled()` HAL primitive is retained for
explicit lifecycle control (e.g. shutdown sequencing) but is no longer
called on the per-measurement path.

### PMID recovery via PM-invalid hint

`poll_bms(bool pm_invalid_hint)` accepts a caller-supplied flag. When
`true` **and** the chip reports on battery (`power_source == None`),
`poll_bms` calls `BmsDevice::resync_pmid()` to re-apply the full PMID
prep sequence (HIZ off → TS on → VOTG → BYPASS off → EN_OTG=1 → settle
→ verify). This recovers from a suspected autonomous `EN_OTG` clear
(BAT_OTGZ / OTG hiccup / TS faults per BQ25629 datasheet §8.3.10.3-4).

The orchestrator computes the hint inline at every `poll_bms` call site
from existing state:

```cpp
_svc.power_service.poll_bms(_first_measurement_done &&
                            !_raw_measures.pm_a.is_pm_25_valid());
```

USB-present is skipped (the chip masks `EN_OTG` internally, so the boost
isn't in play). A partial status read (no power-source available) is
also skipped to avoid acting on stale state. The recovery is bounded by
the BMS poll cadence — at most one resync per poll cycle — so a stuck
chip cannot trigger thrashing.

## Wake Cause Mapping

`get_wake_cause()` maps `esp_sleep_get_wakeup_cause()` to `WakeCause`:

| ESP-IDF cause | `WakeCause` |
|---|---|
| `ESP_SLEEP_WAKEUP_TIMER` | `Timer` |
| `ESP_SLEEP_WAKEUP_EXT0/EXT1/GPIO` | `Button` |
| `ESP_SLEEP_WAKEUP_UNDEFINED` (first power-on) | `PowerOn` |

## Boot Path Routing

`GoApp::run()` selects the boot path via the pure function
`select_boot_path(cause, state)`:

### Fast-path (timer wake, locked)

`is_fast_path_wake(cause, state)` returns `true` when:

```cpp
cause == WakeCause::Timer && state.lock_state == LockState::Locked
```

When true, `GoApp::run_fast_path(state)` is called — **never returns**.
The fast path either enters deep sleep (CPU reboots on wake) or promotes to
`run_interactive()`. The core logic lives in `execute_fast_path()` which
returns a `FastPathResult` (testable on host). A `BootHandoff` struct
describes what has been done when promoting.

Promotion happens when:

- **Sleep too short** (`< deep_sleep_threshold_ms`): stays locked, display
  shows correct locked frame, measurement completed.
- **Button press during fast path**: ISR detects the press during warmup,
  measurement, or GPS read. Unlocks, suppresses wake press, loads RTC
  snapshot for initial display.

### Button-wake path (button wake, Offline mode)

```cpp
cause == WakeCause::Button && state.mode == OperatingMode::Offline
```

Checked after the fast-path condition. `load_rtc_app_state()` returns a
default `RtcAppState` (mode = `Portable`) when no valid state exists, so the
condition is naturally false on the first power-on and falls through to
`run_interactive()`.

When true, `GoApp::run_button_wake_path(state)` renders the wake frame
immediately from the RTC display snapshot and initializes peripherals in
parallel while the display refreshes. See
[ARCHITECTURE.md → Wake and Boot Path](../ARCHITECTURE.md#wake-and-boot-path)
for the four-phase sequence.

### All other cases

`GoApp::run_interactive(cause, {})` is called with a default `BootHandoff`.
All init runs via idempotent GoBoard methods.

## RTC State Persistence

Two separate RTC-memory regions survive deep sleep:

### App state (`go_power.cpp`)

```cpp
RTC_DATA_ATTR static RtcAppState s_rtc_state;
RTC_DATA_ATTR static bool s_rtc_state_valid = false;
```

- `save_state(state)` — copies `state` into `s_rtc_state` and sets the valid flag.
- `load_state()` — returns a copy of `s_rtc_state` if valid; otherwise returns
  a default-constructed `RtcAppState` (safe starting point for fresh power-on).

Under `TEST_HOST`, `RTC_DATA_ATTR` is defined away so the variables become
ordinary statics — `save_state()` / `load_state()` work identically.

### Display snapshot (`go_display.cpp`)

```cpp
RTC_DATA_ATTR static RtcDisplaySnapshot s_rtc_display_snapshot;
RTC_DATA_ATTR static bool s_rtc_display_snapshot_valid = false;
```

Saved by `save_rtc_display_snapshot(values)` in `prepare_for_sleep()` after
the final display update. Loaded by `load_rtc_display_snapshot(out)` in
`run_button_wake_path()` before the early paint.

Contains the sensor values, GPS clock, battery state, status flags, and
rendering settings from the last displayed frame. Allows the button-wake path
to render a meaningful Home screen without reading NVS or sensors.

### RTC memory budget

| Region | Size | Location |
|---|---|---|
| `RtcAppState` + valid flag | ~14 B | `go_power.cpp` |
| `PayloadCacheStorageData` | ~1.5 KB | `rtc_payload_cache_storage.cpp` |
| `RtcDisplaySnapshot` + valid flag | ~43 B | `go_display.cpp` |
| **Total** | **~1.6 KB** | ESP32-C5: 8 KB available |

## LP Core Watchdog Feed (Deep Sleep)

During deep sleep no code runs on the main CPU.  The pre-sleep
`reset_ext_watchdog()` pulse buys one timeout window (~6 min).  For sleep
intervals longer than that, the LP Core takes over.

### Files

| File | Purpose |
|---|---|
| `products/go/main/ulp/wdt_feed.c` | LP Core program: pulse LP_IO_2 HIGH 20 ms, return |
| `products/go/main/go_ulp.h` | Declare `ulp_wdt_start()`, `ulp_wdt_stop()` |
| `products/go/main/go_ulp.cpp` | Load binary, start/stop with `stall_rdy` polling |

### Lifecycle

| Phase | Call | Where |
|---|---|---|
| Fast-path boot | `ulp_wdt_stop()` | Before `init_core()` |
| Button-wake boot | `ulp_wdt_stop()` | After Phase 1 (SPI/display), before Phase 2 (I2C) |
| Fast-path sleep | `ulp_wdt_start()` | After ISR removal, before `enter_sleep()` |
| Orchestrator sleep | `ulp_wdt_start()` | End of `prepare_for_sleep()`, after `reset_ext_watchdog()` |
| Fresh boot | (none) | LP Core not running on first power-on |

### LP timer interval

60 s — matches the orchestrator's awake-mode watchdog interval.  With a
6-minute watchdog timeout this gives a 6x safety margin.

### GPIO2 pin mux

GPIO2 is driven by the regular GPIO matrix (main CPU) while awake and
by the RTCIO subsystem (LP Core) during sleep.  `ulp_wdt_start()` calls
`rtc_gpio_init(PIN_EXT_WDT)` to switch to RTCIO mode; `init_ext_watchdog()`
inside `init_power()` naturally switches back on the next wake.

### Stop safety

`ulp_wdt_stop()` waits 20 ms (the pulse duration) then calls
`ulp_lp_core_stop()`.  After deep sleep wake the LP Core is not
executing — the delay is a no-op safety margin.  `ulp_lp_core_stop()`
is a harmless no-op when the LP Core is not running (fresh boot,
non-Offline modes).

## BMS Watchdog

The BQ25629 charge-control watchdog is **disabled** at BMS init
(`set_watchdog_timeout(WatchdogTimeout::Disable)` in
`components/airgradient-bms/drivers/bq25629/bq25629_bms.cpp`). With the timer
off, the charger never resets its configuration on a missed kick, so **no
periodic reset is required** — not on the measurement tick, not across deep
sleep, and not across a multi-second blocking OTA transfer. The orchestrator
does **not** call `reset_watchdog()` on any timer.

`PowerService::reset_watchdog()` and `set_watchdog_timeout_ms()` remain as thin
wrappers over `BmsDevice` for callers that intentionally re-enable the timer, but
nothing in the steady-state Go firmware drives them. The only watchdog the
firmware actively feeds is the external GPIO2 hardware watchdog (see
[External Watchdog](#external-watchdog)).

## Full-Charge Pause

When the battery is full and USB is present, `poll_bms()` disables
charging via `set_charge_enable(false)` to reduce cell stress. Charging
resumes when SOC drops to `FULL_CHARGE_RESUME_SOC` (95 %).

### Detection

| Board | Full condition |
|---|---|
| V1 (FG attached) | `fg_flags & FgFlags::FC` (BQ27427 Full Charge flag) |
| Prototype (no FG) | `ChargeTerminationDone` + `battery_percentage >= 100` |

The plugged condition requires `bms_power_source_has_external_input()`.

### Thermal Interaction

Both `_thermal_charge_disabled` and `_full_charge_paused` are independent
latches. The actual `set_charge_enable()` I2C write is only issued when it
would change the effective state. When either flag wants charging off, the
hardware stays off. The last flag to clear re-enables charging:

- Temperature or invalid-NTC recovery with full-charge active →
  `_thermal_charge_disabled` clears only for a valid battery temperature from
  2 °C through 43 °C; no `set_charge_enable(true)` (full-charge pause holds)
- Full-charge resume with temperature protection active →
  `_full_charge_paused` cleared, no `set_charge_enable(true)` (temperature
  protection holds)

### Snapshot

`PowerSnapshot::full_charge_paused` surfaces the current state to the
orchestrator and display. The status bar shows a plug icon when
`is_plugged_in && !is_battery_charging`.

## Shutdown (QoN / Ship Mode)

`PowerService::shutdown()` triggers BMS QoN (ship mode) via
`_bms.enter_ship_mode()`, which writes the BQ25629 registers to cut
power to the entire system. If the I2C write fails (e.g. VBUS present
prevents BATFET disconnect), the method falls back to deep sleep with
GPIO wake sources.

### QON Re-Wake (Hold-to-Restart)

The power button (`PIN_BUTTON_POWER`, GPIO5) is wired to both the ESP32
GPIO and the BQ25629 `/QON` pin. For user-initiated shutdown this makes
the gesture significant:

- **Long press, then release** — the device powers off and stays off.
- **Long press held continuously** — after `enter_ship_mode()` opens the
  BATFET (~25 ms, `BATFET_DLY = 0`), the still-held `/QON` line qualifies
  a ship-mode wake (≥ ~17 ms) and the BQ25629 re-closes the BATFET. The
  system powers back on as a **full cold boot**: the BATFET cut drops the
  RTC domain, so `get_wake_cause()` reports `WakeCause::PowerOn`,
  `load_state()` returns defaults, and the device takes the normal
  cold-boot path. This is effectively a hardware power-cycle restart.

Once the BATFET opens there is no firmware running to suppress the
re-wake — it is autonomous BQ25629 behavior. With USB present, ship mode
is refused (the deep-sleep fallback runs instead), so the restart
behavior applies only on battery.

Ship mode is no longer called directly from `poll_bms()`. Instead,
safety trips (EDV and high- or low-battery-temperature) set
`PowerSnapshot::ship_mode_request`, and the orchestrator handles the actual
shutdown after displaying a warning. An invalid NTC reading disables charging
without setting a ship-mode request.

The orchestrator's unified `shutdown(ShipModeRequest reason)` pipeline:

1. Show the reason-specific shutdown screen — all variants share the
   same unified template: `Screen::ShutdownDischarge` (EDV),
   `Screen::ShutdownTemperature` (high temperature),
   `Screen::ShutdownTemperatureLow` (low temperature), or
   `Screen::ShutdownUser` (user-initiated long-press)
2. Queue the shutdown frame with `update_display(wait=true)` and
   `DisplayService::flush()` so the e-paper paint is complete before continuing
3. Stop tracking if active; backup chart cache
4. Disable PM sensor power (`set_pm_power(false)`)
5. Slow down before power cut (`SHUTDOWN_POWER_OFF_SETTLE_MS`) so the painted
   reason screen remains visible and BLE disconnect notice can drain
6. `power_service.shutdown()` — BMS QoN → deep sleep fallback

## External Watchdog

An external hardware watchdog is connected to `PIN_EXT_WDT` (GPIO2). Two
methods on `PowerService` wrap the free functions from
`components/airgradient-common/common.h`:

| Method | Calls | Effect |
|---|---|---|
| `init_ext_watchdog()` | `ext_watchdog_init(gpio, pin)` | Configures pin as output, drives LOW |
| `reset_ext_watchdog()` | `ext_watchdog_reset(gpio, pin)` | Pulses pin HIGH for 20 ms, then LOW |

Both are no-ops when `Config::pin_ext_wdt < 0`.

Pulse points:

| When | Where | Purpose |
|---|---|---|
| Boot (fast + full) | `GoHardwareBoard::power()` on first access | First pulse after wake/power-on |
| Every 60 s | Orchestrator `check_timers()` | Periodic keep-alive |
| Before sleep | Orchestrator `prepare_for_sleep()` | Maximize timeout window during sleep |
| OTA start gap | `enter_ota()` (BLE) / WiFi pre-check | Cover the gap to the first progress tick of a blocking transfer |
| Every non-terminal OTA progress tick | `OtaService` progress callback | Keep the watchdog fed while the orchestrator loop is blocked in `run_ble()` / `run_wifi_check()` |

During a blocking OTA transfer the orchestrator main loop (and its 60 s pulse) is
frozen, so `OtaService` feeds the external watchdog from the component's progress
callback on every non-terminal tick (`Starting` / `Checking` / `Downloading` /
`Applying`) via the borrowed `PowerService`. This removes any reliance on a
transfer fitting inside a single ~6 min window. See
[`ota_service.md`](ota_service.md) and
[`orchestrator.md` → OTA](orchestrator.md#firmware-update-ota).

## Platform Abstraction Summary

| Method | Testable on Host? | Notes |
|---|---|---|
| `decide_sleep()` | Yes | Pure logic |
| `should_hold_pm_sensor()` | Yes | Pure logic |
| `should_sleep_pm_sensor()` | Yes | Pure logic |
| `set_pm_power()` | Yes (mock gpio::Hal + BmsDevice) | PMID arm + GPIO level |
| `set_fuel_gauge()` | Yes (mock FuelGaugeDevice) | Non-owning pointer setter |
| `is_fast_path_wake()` | Yes | Pure logic |
| `poll_bms()` | Yes (mock BmsDevice + FuelGaugeDevice) | I2C reads via driver; FG reads when attached |
| `poll_status()` | Yes (mock BmsDevice) | Fast status poll |
| `set_watchdog_timeout_ms()` | Yes (mock BmsDevice) | Forward to BmsDevice |
| `reset_watchdog()` | Yes (mock BmsDevice) | |
| `save_state()` / `load_state()` | Yes | `RTC_DATA_ATTR` defined away |
| `enter_sleep()` | No | Calls `esp_sleep_*` + `gpio_hold_en()` |
| `configure_wake_sources()` | No | Calls `esp_sleep_*` |
| `release_sleep_gpio_holds()` | No | Calls `gpio_hold_dis()` |
| `get_wake_cause()` | No | Calls `esp_sleep_get_wakeup_cause()` |
| `shutdown()` | No | BMS hardware command |
| `init_ext_watchdog()` | Yes (mock gpio::Hal) | GPIO config via HAL |
| `reset_ext_watchdog()` | Yes (mock gpio::Hal) | GPIO pulse via HAL |

## Fuel-Gauge Learning Surface

`PowerService` exposes a small learning-facing surface used only by the factory
`FgLearningRunner` — the normal field poll path is untouched. `poll_bms()`
itself does **no** learning work; `poll_bms_fg_learning()` runs a normal poll and
then layers the learning-only fields on top (the packed `fg_learning_flags`
including one extra CONTROL_STATUS read, `external_input_present`, and the
derived `edv_cutoff_reached`), keeping that read off the field hot path.

| Method | Returns | Notes |
|---|---|---|
| `poll_bms_fg_learning()` | `PowerSnapshot` | Normal poll + learning fields (one extra CONTROL_STATUS read) |
| `read_fg_learning_verify()` | `FgLearningVerifyReadout` | Aggregates Qmax, Ra grid, Design Capacity, ITPOR, QMAX_UP |
| `set_charge_current_ma()` | `bool` | Program ICHG via BmsDevice |
| `set_manual_charge_disabled()` | `void` | Enable / disable charge path |
| `set_chemistry_4v2()` | `bool` | Idempotent Chem ID `0x1202` switch (no-op without a gauge) |
| `set_update_status_learning()` | `bool` | Lift / restore gauge change limits |

See [`fg_learning.md`](fg_learning.md) for the full factory boot path.
