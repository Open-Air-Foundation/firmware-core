# airgradient-sensors

Shared environmental sensor stack: HAL interfaces per sensor type, concrete
sensor drivers grouped by device, and the `SensorManager` orchestrator that
polls and averages readings.

## Status

`Stable`.

## Scope

This component owns:

- HAL interfaces for environmental sensor types (temp/hum, PM, CO2, TVOC/NOx,
  O3/NO2, pressure)
- concrete sensor driver implementations grouped by device
- shared sensor orchestration (`SensorManager`) — polling, averaging,
  warmup
- the `Sensors` aggregate struct used to inject HAL pointers

This component does not own:

- battery management (lives in [`airgradient-bms`](../airgradient-bms/README.md))
- product-specific sensor selection or wiring
- payload upload, persistence, or remote-config logic

## Directory Layout

```text
components/airgradient-sensors/
  hal/
  drivers/
  services/
  tests/
  Kconfig
  CMakeLists.txt
  README.md
```

- `hal/` — public sensor interfaces (one header per sensor type)
- `drivers/` — concrete sensor implementations grouped by device family
- `services/` — `SensorManager` orchestrator
- `tests/` — host-side tests owned by this component

## Public Includes

```cpp
#include "hal/temp_hum_sensor.h"
#include "services/sensor_manager.h"
#include "drivers/sht40/sht40.h"
```

Guideline:

- include from `hal/` when depending on an interface
- include from `services/` when using shared orchestration
- include from `drivers/` only when instantiating a concrete driver

## Design

```text
caller -> SensorManager(Sensors&) -> HAL interfaces -> concrete drivers -> bus (I2C / serial / ADC)
```

Product code instantiates the concrete drivers it needs, packs HAL pointers
into a `Sensors` struct, and passes it to `SensorManager`. Skipping a
sensor type is as simple as leaving its pointer null.

## HAL Interfaces

| Header | Interface | Optional capability hooks |
|---|---|---|
| `hal/temp_hum_sensor.h` | `TempHumSensor` | — |
| `hal/pm_sensor.h` | `PMSensor` | `supports_temp_hum()` / `temp_hum_data()` for PM sensors with integrated temp/hum (e.g. PMS5003T); optional `sleep()` / `wake()` low-power hooks (default no-op) |
| `hal/co2_sensor.h` | `CO2Sensor` | `supports_temp_hum()` / `temp_hum_data()` for CO2 sensors with integrated temp/hum; optional automatic-background-calibration-period hooks |
| `hal/tvoc_nox_sensor.h` | `TVOCNOxSensor` | `run_conditioning()` (default no-op) for warmup; `set_compensation()` (default no-op) for ambient temp/hum compensation |
| `hal/o3_no2_sensor.h` | `O3No2Sensor` | — |
| `hal/pressure_sensor.h` | `PressureSensor` | — |

## Drivers

| Driver | Device | Bus | Notes |
|---|---|---|---|
| `drivers/sht40` | Sensirion SHT40 | I2C | Temperature + humidity |
| `drivers/pms5003` | Plantower PMS5003 / PMS5003T | Serial (UART or I2C-to-UART bridge) | PM. PMS5003T variant exposes temp/hum |
| `drivers/sps30` | Sensirion SPS30 | I2C | PM mass + number concentrations. Standard-particle, `pm_03_pc`, and `pm_5_pc` left as invalid sentinels. `sleep()`/`wake()` use native Sleep (`0x1001`)/Wake-up (`0x1103`); `init()` recovers a sensor left asleep |
| `drivers/s8` | SenseAir S8 | Modbus RTU over serial | CO2 |
| `drivers/sunlight` | SenseAir Sunlight | Modbus RTU over serial | CO2 |
| `drivers/s12` | SenseAir S12 | I2C | CO2; reads a single big-endian 16-bit register (default 0x06/0x07 = filtered, pressure-compensated). Supports background calibration. Automatic-background-calibration requests are temporarily acknowledged without sensor I/O to avoid unsafe ABC and MeterControl writes; no integrated temp/hum |
| `drivers/stcc4` | Sensirion STCC4 | I2C | CO2 with integrated temp/hum (`supports_temp_hum() = true`) |
| `drivers/scd4x` | Sensirion SCD41 | I2C | CO2 with integrated temp/hum, configurable automatic self-calibration period, and opt-in retained periodic-measurement reattachment after deep sleep. Thin adapter around the shared `embedded-i2c-scd4x` driver. Singleton — only one instance on the bus, since the underlying driver keeps the I2C handle and address in file-scope globals |
| `drivers/sgp41` | Sensirion SGP41 | I2C | TVOC + NOx |
| `drivers/alpha_sense` | AlphaSense O3 / NO2 | I2C (dual ADS1115) | Electrochemical front-end |
| `drivers/dps368` | Infineon DPS368 | I2C | Pressure |
| `drivers/co2_common` | (helper) | — | Shared Modbus CRC helper used by S8 and Sunlight |

### SPS30 Particle-Count Mapping

SPS30 reports number concentrations in particles per cm³. The shared
`PMData` convention follows PMS5003 units, particles per 0.1 L, so the
driver multiplies SPS30 counts by 100 before publishing them.

Only same-named bins are mapped:

| SPS30 value | `PMData` field |
|---|---|
| PM0.5 number | `pm_05_pc` |
| PM1.0 number | `pm_01_pc` |
| PM2.5 number | `pm_25_pc` |
| PM10 number | `pm_10_pc` |

SPS30 PM4.0 mass / number have no matching `PMData` field and are not
exposed. `pm_03_pc` and `pm_5_pc` remain invalid because SPS30 does not
report PM0.3 or PM5.0 count bins.

## SensorManager

The orchestrator lives in `services/sensor_manager.{h,cpp}`. See that header
for the full interface; the most-used pieces are summarised below.

### `SensorGroup`

Bitmask enum that controls which sensors `start_measures()` polls:

| Value | Sensors polled |
|---|---|
| `PM` | `pms_a`, `pms_b` |
| `Other` | `temp_hum`, `co2`, `o3_no2`, `pressure` |
| `TvocNox` | `tvoc_nox` (SGP41 read + gas-index algorithm step when configured) |
| `All` | All of the above (default) |

Combine with `operator|`; test with `has_group()`.

### Gas-Index Algorithm

`SensorManager` hosts the Sensirion gas-index algorithm for converting raw
SGP41 VOC/NOx ticks into calibrated 1..500 index values. The algorithm is
a shared-component concern (sensor-level signal processing), not a
product-layer concern.

- `configure_tvoc_nox_index(sampling_interval_ms)` — initialises the VOC
  and NOx algorithm instances. Accepts `1000` or `10000` ms only.
- `set_tvoc_nox_compensation(temperature_c, humidity_pct)` — forwards
  ambient compensation to the driver via `TVOCNOxSensor::set_compensation()`.
- When configured, `_accumulate_tvoc_nox()` feeds each raw sample into the
  algorithm and produces index values. The algorithm returns `0` during the
  initial ~45 s blackout; these are treated as invalid and not accumulated.
- When unconfigured, `_accumulate_tvoc_nox()` passes through driver-reported
  index fields (which are invalid sentinels from the SGP41 driver).

### `start_measures(int iterations, SensorGroup groups = SensorGroup::All)`

- `groups` selects which sensor categories to poll. Skipped groups leave
  their fields at invalid sentinels.
- When `iterations == 1`, the per-iteration delay is skipped and the call
  returns as soon as I2C reads complete.
- Per-iteration pacing target: `CONFIG_AVERAGING_ITERATION_INTERVAL_MS`.

### `warmup_sensor()`

Blocking helper that prepares TVOC/NOx and PM sensors before the first real
measurement:

- For each iteration, calls `tvoc_nox->run_conditioning()` once and
  performs one discard `read()` on `pms_a` / `pms_b`.
- Iteration count is derived from Kconfig as
  `CONFIG_SENSOR_WARMUP_DURATION_MS / CONFIG_SENSOR_WARMUP_INTERVAL_MS`,
  paced to `CONFIG_SENSOR_WARMUP_INTERVAL_MS` with elapsed-time
  compensation.
- Returns immediately if `tvoc_nox`, `pms_a`, and `pms_b` are all null.
- Conditioning failures are logged via `AG_LOGW` and do not abort the
  warmup.
- Temp/hum, CO2, O3/NO2, and pressure sensors are not touched.

## Configuration

Tuning knobs live under **AirGradient Sensors** in `menuconfig` (see
`components/airgradient-sensors/Kconfig`):

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_AVERAGING_ITERATION_INTERVAL_MS` | `2000` | Target wall-clock duration of one averaging iteration inside `start_measures()` |
| `CONFIG_SENSOR_WARMUP_DURATION_MS` | `10000` | Total duration of `warmup_sensor()`. Iteration count = this / `SENSOR_WARMUP_INTERVAL_MS` |
| `CONFIG_SENSOR_WARMUP_INTERVAL_MS` | `1000` | Duration of a single iteration inside `warmup_sensor()` |
| `CONFIG_SGP41_INDEX_SAMPLING_INTERVAL_1S` / `_10S` | `_10S` | Choice: cadence for the gas-index algorithm sampler (1 s or 10 s). The actual millisecond value is derived in code as `SGP41_INDEX_SAMPLING_INTERVAL_MS` |

For host (`TEST_HOST`) builds, `services/sensor_manager.h` supplies the same
numeric defaults via `#ifndef` fallbacks so host tests exercise identical
behavior without `sdkconfig.h`.

## Dependencies

- `components/airgradient-common/` — shared `Measures` types and RTOS
  abstraction
- `components/airgradient-serial/` — serial transport abstractions (UART,
  I2C-to-UART bridge) used by Modbus and PMS sensors
- `components/ads1115/` — generic ADC helper used by AlphaSense
- `components/embedded-i2c-scd4x/` — Sensirion SCD4x driver wrapped by the
  `scd4x` adapter
- `components/sensirion-gas-index-algorithm/` — vendored Sensirion
  gas-index algorithm (float variant) consumed by `SensorManager` for
  SGP41 raw-to-index conversion
- `esp_driver_gpio`, `esp_driver_i2c` — ESP-IDF drivers used by various
  sensor implementations

## Tests

Host tests live under `components/airgradient-sensors/tests/` and run
through the [tests runner](../../tests/README.md). Current coverage focuses
on `SensorManager` averaging, fallback, null-handling, and iteration timing
behavior.

## Notes

### Adding a new sensor

1. Add or extend the public interface in `hal/` if needed.
2. Place the concrete implementation under `drivers/<driver_name>/`.
3. Keep driver-private helpers next to that driver.
4. Update `services/sensor_manager.*` only if the orchestrator must read
   the new sensor.
5. Update `components/airgradient-sensors/CMakeLists.txt` for new source
   files.
6. Add or extend host tests under
   `components/airgradient-sensors/tests/` when the behavior belongs to
   this component.

### Design intent

Clarity over abstraction. One place for environmental sensor code, with
explicit separation between interfaces (`hal/`), drivers (`drivers/`), and
orchestration (`services/`).
