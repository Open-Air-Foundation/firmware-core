# airgradient-bms

Shared battery management (BMS) stack: HAL interfaces for charger and
fuel-gauge devices, public BMS data types, and concrete drivers.
Decoupled from the environmental sensor stack (`airgradient-sensors`).

## Status

`Stable`.

## Scope

This component owns:

- the `BmsDevice` HAL interface (charger / PMIC abstraction)
- the `FuelGaugeDevice` HAL interface (fuel-gauge runtime-poll
  abstraction)
- public BMS types: telemetry, status, charging state, power source,
  fuel-gauge cell configuration
- sentinel values and field-level validation helpers for BMS data
- concrete charger and fuel-gauge driver implementations

This component does not own:

- product-level power policy, watchdog cadence, sleep decisions, or
  shutdown behavior
- fuel-gauge bring-up decisions (corruption recovery, idempotent
  cell-config writes) — those live in product-level board init code
- environmental sensing (lives in `airgradient-sensors`)

## Directory Layout

```text
components/airgradient-bms/
  hal/
  types/
  drivers/
  tests/
  CMakeLists.txt
  README.md
```

- `hal/` — `BmsDevice` and `FuelGaugeDevice` abstract interfaces
- `types/` — `BmsTelemetry`, `BmsStatus`, `BmsChargingState`,
  `BmsPowerSource`, `FgCellConfig`, `BmsInvalid` sentinels, and inline
  `_str` helpers
- `drivers/` — concrete driver implementations grouped by IC family
- `tests/` — host-side tests owned by this component

## Public Includes

```cpp
#include "hal/bms_device.h"
#include "hal/fuel_gauge_device.h"
#include "types/bms_types.h"
#include "drivers/bq25629/bq25629_bms.h"
#include "drivers/bq27427/bq27427.h"
#include "drivers/bq27742/bq27742.h"
```

Guideline:

- include from `hal/` when depending on the abstract interface
- include from `types/` when depending on BMS data types only
  (`FgCellConfig`, sentinels) — no ESP-IDF dependency
- include from `drivers/` only when instantiating a concrete driver

## Design

```text
caller -> BmsDevice&       -> BQ25629Bms -> i2c_master -> charger IC
caller -> FuelGaugeDevice& -> BQ27427    -> i2c_master -> fuel gauge IC
caller -> FuelGaugeDevice& -> BQ27742    -> i2c_master -> fuel gauge IC
```

`BmsDevice` models a charger / PMIC with telemetry reads, status
queries, and power-path control. `FuelGaugeDevice` models a fuel gauge
with runtime SOC, voltage, current, and capacity reads.

Public BMS types live in `types/` and never carry ESP-IDF symbols, so
host tests can construct and validate them without the I2C master.

### Shared Interfaces

| Interface | Contract |
|---|---|
| [`BmsDevice`](hal/bms_device.h) | Charger telemetry, status, SOC estimation, watchdog, and power controls |
| [`FuelGaugeDevice`](hal/fuel_gauge_device.h) | Gauge readiness, SOC, voltage, current, power, capacity, temperature, and flags |
| [`BmsTelemetry` / `BmsStatus`](types/bms_types.h) | Default invalid values and field-specific validation helpers |

Check read results and validate each field before use; a successful telemetry
read does not guarantee every field is available. Unsupported optional
operations, such as ADC control or system reset, return `false`.

Fuel-gauge learning operations are also exposed through `FuelGaugeDevice`,
but support depends on the driver. Cell configuration and factory reset stay
on concrete drivers. The caller supplies battery parameters and decides when
to change them.

Power-control calls are hardware requests. Persist required state before ship
mode or system reset. A successful reset request means the command was written,
not that power cycled; the caller owns recovery if execution continues.

### Drivers

| Driver | IC | Notes |
|---|---|---|
| `drivers/bq25xx` | TI BQ25672 / BQ25798 | Multi-cell charger family |
| `drivers/bq25629` | TI BQ25629 | Single-cell charger; wraps the vendor `bq25629` component |
| `drivers/bq27427` | TI BQ27427 | Single-cell Impedance Track fuel gauge; implements `FuelGaugeDevice` |
| `drivers/bq27742` | TI BQ27742-G1 | Single-cell fuel gauge with protection; telemetry and cell configuration |

### BQ25629 Driver

`BQ25629Bms` wraps the vendor driver and accepts a shared I2C bus, charger
configuration, and device address at construction. Its concrete behavior is:

- `init()` disables the chip watchdog and arms PMID boost. The chip handles
  external-input pass-through versus battery boost autonomously.
- `set_pmid_enabled()` changes `EN_OTG` without a settling delay.
  `resync_pmid()` repeats the initialization sequence with settling and readback.
- `set_adc_enabled()` controls conversions; telemetry reads re-enable them
  when needed. Invalid NTC results remain invalid battery-temperature readings.
- `request_system_power_reset()` requests BATFET reset while preserving
  unrelated charger-control bits.

The caller must supply `BQ25629_NTC_Config` values matching its circuit; there
are no board-specific defaults:

| Field | Unit | Purpose |
|---|---|---|
| `pullup_ohm` | Ω | Thermistor-divider pull-up resistance |
| `pulldown_ohm` | Ω | Thermistor-divider pull-down resistance |
| `r25_ohm` | Ω | Thermistor resistance at 25 °C |
| `beta_k` | K | Thermistor beta for temperature conversion |

Watchdog cadence, charging limits, recovery triggers, and sleep/shutdown policy
belong to the caller. See [the adapter API](drivers/bq25629/bq25629_bms.h)
for supported controls.

### BQ27427 Driver

The BQ27427 driver is a thin chip-primitive layer that implements
`FuelGaugeDevice` for the runtime poll surface and exposes Data Memory
operations as non-virtual methods. It owns the I2C bus protocol, the
chip-specific access constraints, and `DEVICE_TYPE` verification on
`init()`.

Key invariants enforced internally:

- BlockData reads always start at register `0x40` (reads at higher
  offsets return stale zeros)
- UNSEAL (`Control(0x8000)` twice) runs before every Data Memory write
- CFGUPDATE enter/exit with `Flags()` bit-4 polling and `SOFT_RESET`
- `Control(DEVICE_TYPE) == 0x0427` checked inside `init()`

Polling constraints per TRM SLUUCD5 section 6.3.1.3:

- Standard Commands have a per-command 2 Hz cap. The caller must choose
  a polling cadence that respects this limit.
- `t(BUF) >= 66 us` inter-packet bus-free time at 400 kHz. The ESP-IDF
  I2C driver's transaction framing provides this implicitly.

The driver does not know what cell the product uses, what counts as a
corrupted value, or whether `write_cell_config` should run. Those
decisions live in product-level board init code.

### BQ27742 Driver

Implements `FuelGaugeDevice` runtime reads. Power is calculated from voltage
and average current; flags expose only DSG, CHG, and FC mapped to `FgFlags`.
Failed reads leave outputs unchanged.

`init()` preserves pack configuration; runtime reads do not require a
cell-configuration write. To change settings, call `unseal()`, then
`read_cell_config()` / `write_cell_config()` with `FgCellConfig`, and `seal()`
afterward, including on failure. A custom unseal key can be supplied.
Writes validate ranges, preserve unrelated settings and the energy scale,
skip unchanged blocks, and verify readback. The three flash blocks commit
independently; a failure can leave a partial update.

`read_voltage_config()` / `write_voltage_config()` access nominal cell voltage
and the charging-voltage reference used for full-charge detection, with the
same access, preservation, and readback rules. `disable_full_charge_cutoff()`
clears only `FCFET`; temperature and fault protection settings are preserved.

`read_cc_sense_mohm()` / `write_cc_sense_mohm()` access CC Gain and CC Delta
using TI's F4 encoding. Writes require an unsealed gauge, validate both fields,
verify the whole flash block, and skip identical values. Other calibration
fields and learned data are preserved.

`read_design_capacity_mah()` also works while sealed. Chemistry,
protection provisioning, and the BQ27427 learning workflow remain outside
this driver's scope. BQ27427-specific learning methods return `false`. See
[TI's technical reference, chapter 5](https://www.ti.com/lit/ug/sluuax0c/sluuax0c.pdf).

## Usage

```cpp
// The caller initializes the bus and supplies charger/NTC configuration.
BQ25629Bms bms(i2c_bus, config, address);
if (!bms.init()) { return; }

BmsTelemetry telemetry{};
if (bms.read_telemetry(telemetry) && telemetry.is_battery_voltage_valid()) {
    consume_voltage(telemetry.battery_voltage);
}
```

See [`drivers/bq25629/bq25629_bms.h`](drivers/bq25629/bq25629_bms.h),
[`drivers/bq27427/bq27427.h`](drivers/bq27427/bq27427.h),
[`drivers/bq27742/bq27742.h`](drivers/bq27742/bq27742.h), and
[`drivers/bq25xx/bq25xx.h`](drivers/bq25xx/bq25xx.h) for full APIs.

## Dependencies

- `components/airgradient-common/` — RTOS abstraction (`RTOS::delay_ms`,
  `RTOS::get_time_ms`)
- `components/bq25629/` — BQ25629 vendor driver wrapped by the
  single-cell adapter
- `esp_driver_i2c` — I2C master bus access (concrete drivers only)

There is no dependency on `airgradient-sensors`.

## Tests

Host tests live under `components/airgradient-bms/tests/` and run through
the [tests runner](../../tests/README.md). Current coverage focuses on
sentinel and validation behavior of the public BMS types.

The BQ27427 and BQ27742 drivers have no automated host tests; bus-protocol
verification is performed on hardware. Callers can mock `BmsDevice` and
`FuelGaugeDevice` to test policy independently of the physical devices.
