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

| Variant | PM Enable Polarity | Fuel Gauge | SOC Source |
|---|---|---|---|
| Prototype | Active-high (level 1) | None | BQ25629 voltage-curve estimate |
| V1 | Active-low (level 0) | BQ27427 | FG-derived (BQ25629 fallback) |

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

`set_pm_power(true/false)` drives only the EN_PM load switch GPIO that
gates PMID → SPS30 VDD. It cuts the SPS30's ~50 mA fan current between
measurements without ever touching `EN_OTG`.

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
manufacturing flag and runs a full factory reset at shutdown, so any
settings, Wi-Fi credentials, or BLE bonds changed during testing are wiped
before power-off. A plain reboot likewise returns to fresh onboarding.
Button 2 long press remains factory reset.

### Cell Safety

- **EDV (over-discharge):** ship mode requested when cell voltage stays
  below 2.9 V for 3 consecutive polls while on battery. The orchestrator
  shows a warning on `Screen::Info` before entering ship mode.
- **OT (over-temperature):** charge cutoff at 50 C (resume at 47 C);
  ship mode requested at 60 C with a warning display before shutdown.
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

## Documentation

- [`ARCHITECTURE.md`](ARCHITECTURE.md) — boot paths, event model, module
  structure
- [`docs/`](docs) — per-service implementation notes (BLE, cloud, display,
  fuel-gauge learning, GPS, input, orchestrator, OTA, power, sensor producer,
  settings, storage, UI, Wi-Fi)
- [`go_ble_client.md`](go_ble_client.md) — client-side BLE integration spec
  for mobile app developers (discovery, pairing, GATT, payloads, history)
- [`specs/`](specs) — design specs and refactor plans (temporary; deleted
  once shipped, per [`docs/STYLE.md`](../../docs/STYLE.md))
- [`tests/`](tests) — host tests (`go_*.tests.cpp`) plus the BLE
  integration suite under `tests/ble-integration/`
- `main/board_config.h` — pin assignments and I2C addresses
