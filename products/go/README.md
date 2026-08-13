# AirGradient Go

Firmware for the [AirGradient Go](https://www.airgradient.com/portable/)
portable air quality monitor with GPS, e-paper display, BLE, and battery.

## Sensors

- **PM** — Sensirion SPS30 (PM1.0, PM2.5, PM10 mass + particle counts)
- **CO2** — SenseAir S12 / Sensirion SCD4x / Sensirion STCC4 (probed in
  order at boot; first detected wins)
- **TVOC / NOx** — Sensirion SGP41
- **Temp / Humidity** — SHT40 on V1, then fallback from CO2 and pressure
  sensors
- **Pressure** — Infineon DPS368
- **Battery** — TI BQ25629 charger IC; TI BQ27427 Impedance Track fuel gauge
  (V1 board only)

## Hardware Notes

A single firmware binary supports both the **Prototype** and **V1** boards.
Board variant is detected at runtime by probing the BQ27427 fuel gauge at
I2C address `0x55` during `init_buses()`. All variant-conditional behavior is
gated on `board.variant()`.

| Variant | PM GPIO26 (EN_PM) | Fuel Gauge | SOC Source |
|---|---|---|---|
| Prototype | Active-high VDD load switch | None | BQ25629 voltage-curve estimate |
| V1 | Active-low I2C bus isolation | BQ27427 | FG-derived (BQ25629 fallback) |

### Temperature and Humidity Source

V1 boards probe SHT40 during `sensors()`. `SensorManager` resolves
`temp_hum_a` in this priority order: dedicated SHT40, CO2-integrated T/RH
(SCD4x or STCC4), then DPS368 temperature-only fallback. Prototype boards do
not probe SHT40 and use the same fallback chain without the dedicated source.

### PMID Power Rail

The SPS30 PM sensor is powered by the PMID +5 V rail from the BQ25629.
PMID `EN_OTG` is armed **once during BMS init** and held for the lifetime
of the session. The chip handles VBUS pass-through ↔ boost transitions
autonomously based on its own VBUS-detect:

- VBUS present → buck pass-through (`EN_OTG` masked internally by the chip)
- VBUS absent → boost runs to drive PMID = 5 V from VBAT

`set_pm_power(true/false)` drives the EN_PM GPIO (GPIO26), never `EN_OTG`.
Its effect is variant-specific: on Prototype it is a load switch gating
PMID → SPS30 VDD; on V1 it only isolates the SPS30 from the shared I2C
bus — the sensor stays powered by always-on PMID.

On V1 the ~50 mA fan current is cut between measurements by the SPS30's
native **Sleep** command (`0x1001`), not the GPIO. After each measurement
the orchestrator calls `request_pm_sleep()`; the sensor producer sleeps
the SPS30 and posts `PmSensorAsleep`, after which the orchestrator
isolates the bus with `set_pm_power(false)`. The pre-wake path connects
the bus (`set_pm_power(true)`) then wakes + warms via `request_prepare()`.
A sensor left asleep across deep sleep is recovered by `SPS30::init()`,
which sends the wake sequence on probe failure.

Per-measurement `EN_OTG` toggling was tried (saves ~220 µA quiescent on
battery when PM is off) and reverted: each boost cold-start charges the
PMID output capacitance from ~VBAT to 5.1 V with an inrush spike that can
exceed 1S cell-protection OCP, opening the protection FET and causing a
POWERON reset. Holding `EN_OTG=1` trades ~220 µA quiescent for indefinite
uptime on battery.

All three boot paths call `init_core()` (which runs `init_bms()` →
arms PMID) before `power().set_pm_power(true)` and `sensors()`.

### Power Button and Restart

The power button (`PIN_BUTTON_POWER`, GPIO5) drives both the ESP32 GPIO
and the BQ25629 `/QON` pin, so the long-press gesture has two outcomes:

- **Long press, then release** — normal power off (ship mode; BATFET
  opens and the system stays off).
- **Long press and keep holding** — hardware power-cycle restart on
  battery. Once the BATFET opens, the still-held `/QON` line qualifies a
  ship-mode wake (≥ ~17 ms) and the BQ25629 re-closes the BATFET. Because
  the BATFET cut drops the RTC domain, the device returns as
  `WakeCause::PowerOn` (full cold boot, RTC state wiped). With USB
  present, ship mode is refused and the path falls back to deep sleep, so
  the restart behavior is battery-only.

### Manufacturing Mode

While a unit is still un-onboarded (`onboarding_done == false`), a short
press of Button 2 (`PIN_BUTTON_BOOT`) skips the Getting Started guide and
enters Stationary operating mode **ephemerally** — nothing is written to
NVS. This lets the production team exercise the full Stationary path (Wi-Fi,
cloud) without latching `onboarding_done`. The device tracks an internal
manufacturing flag and runs a cleanup reset at shutdown: saved Wi-Fi
credentials, routes, and all Go settings except active measurement corrections
are wiped before power-off. This retains production-configured PM, temperature,
and humidity corrections. BLE bond deletion is a safe no-op once Stationary
mode has torn down the BLE host. A plain reboot likewise returns to fresh
onboarding. Button 2 long press remains factory reset.

### Cell Safety

- **EDV (over-discharge):** ship mode requested when cell voltage stays
  below 2.9 V for 3 consecutive polls while on battery. The orchestrator
  shows a warning on `Screen::Info` before entering ship mode.
- **Battery-temperature protection:** charging is allowed from 0 °C through
  45 °C. After a temperature or invalid-NTC block, charging resumes only from
  2 °C through 43 °C. Discharging is allowed from -10 °C through 60 °C;
  temperatures outside that range cause distinct cold- or hot-temperature
  shutdowns. An invalid NTC reading disables charging only and does not request
  shutdown.
- **Full-charge pause:** when the battery is full and USB is present,
  charging is disabled to reduce cell stress. Resumes when SOC drops
  to 95 %. V1 uses the BQ27427 FC flag; Prototype falls back to
  BQ25629 `ChargeTerminationDone` + 100 % SOC.
- **BATFET_DLY:** explicitly cleared to 0 (25 ms fast disconnect)
  during BQ25629 init for deterministic ship-mode timing.

### Fuel Gauge (V1 Only)

On V1, `init_bms()` initialises the BQ27427 with a two-pass corruption
recovery sequence and idempotent cell-config write. At runtime,
`poll_bms()` prefers FG-derived SOC and surfaces FG telemetry in
`PowerSnapshot`. Three log lines are emitted per poll: charger status,
BQ25629 ADC telemetry, and FG telemetry with decoded flags
(`FgFlags::FC`, `CHG`, `DSG`, etc.).

## Build

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/go build
```

The build derives `PROJECT_VER` from the latest reachable
`go-vMAJOR.MINOR.PATCH` tag:

- exact clean `go-v1.2.3` tag — `1.2.3`
- later clean commit — `1.2.3-gabcdef0`
- tracked or untracked worktree changes — `1.2.3-gabcdef0-dirty`
- no matching tag — `0.0.0-gabcdef0`, with `-dirty` when applicable

The `MAJOR.MINOR.PATCH` portion is limited to 16 characters so the complete
development version remains within ESP-IDF's 31-character application-version
field.

Version resolution happens during CMake configuration. When reusing an existing
build directory, run `idf.py -C products/go reconfigure` after changing tags or
worktree cleanliness.

Push an annotated tag whose commit is already on `main` to build and publish
the firmware bundle through the repository-level
[`release.yml`](../../.github/workflows/release.yml) workflow:

```sh
git tag -a go-v1.2.3 -m "AirGradient Go v1.2.3"
git push origin go-v1.2.3
```

The release ZIP contains the OTA application, OTA data initializer, bootloader,
partition table, and merged factory-flash binary.

## Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — boot paths, event model, module
  structure
- [`feature_overview.md`](feature_overview.md) — product-facing feature summary
- [`docs/`](docs) — per-service implementation notes (BLE, cloud, display,
  GPS, input, Local Server, orchestrator, OTA, power, sensor producer,
  settings, storage, UI, Wi-Fi)
- [`../../docs/local_http_api.md`](../../docs/local_http_api.md) — local HTTP
  API, mDNS discovery, and AirGradient Go support
- [`docs/local_server.md`](docs/local_server.md) — Local Server lifecycle,
  request queue, and OTA access policy
- [`docs/measurement_corrections.md`](docs/measurement_corrections.md) — raw
  and corrected measurement views and their consumers
- [`docs/serial_command_service.md`](docs/serial_command_service.md) —
  manufacturing-only USB Serial/JTAG command protocol
- [`docs/fg_learning.md`](docs/fg_learning.md) — factory fuel-gauge learning
  boot path (`FgLearningRunner` / `FgLearningController` split, dashboard)
- [`docs/hardware_test.md`](docs/hardware_test.md) — on-device Hardware Test
  surface (Peripheral, GPS, Accelerometer, FG Learning arm)
- [`go_ble_client.md`](go_ble_client.md) — client-side BLE integration spec
  for mobile app developers (discovery, pairing, GATT, payloads, history)
- [`specs/`](specs) — design specs and refactor plans (temporary; deleted
  once shipped, per [`docs/STYLE.md`](../../docs/STYLE.md))
- [`tests/`](tests) — host tests (`go_*.tests.cpp`)
- [`tests/ble-integration/`](tests/ble-integration/) — BLE hardware integration
  suite for the Portable GATT surface
- [`tests/local-server-integration/`](tests/local-server-integration/) — Local
  Server hardware integration suite for Stationary mDNS, HTTP, config, action,
  and OTA policy surfaces
- `main/board_config.h` — pin assignments and I2C addresses
