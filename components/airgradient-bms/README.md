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

Fuel gauges: `BQ27427` (GO v1.0) and `BQ27742` (GO v2.0, bq27742-G1 with
integrated hardware protection). Both implement `FuelGaugeDevice`; the
BQ27742 normalises its `Flags()` to the `FgFlags` layout, reads data flash
through the unsealed block interface (no CFGUPDATE mode on that part), and
additionally exposes `SafetyStatus`, `ProtectorStatus` and `ProtectorState`.

Guideline:

- include from `hal/` when depending on the abstract interface
- include from `types/` when depending on BMS data types only
  (`FgCellConfig`, sentinels) — no ESP-IDF dependency
- include from `drivers/` only when instantiating a concrete driver

## Design

```text
caller -> BmsDevice&       -> BQ25629Bms -> i2c_master -> charger IC
caller -> FuelGaugeDevice& -> BQ27427    -> i2c_master -> fuel gauge IC
```

`BmsDevice` models a charger / PMIC with telemetry reads, status
queries, and power-path control. `FuelGaugeDevice` models a fuel gauge
with runtime SOC, voltage, current, and capacity reads.

Public BMS types live in `types/` and never carry ESP-IDF symbols, so
host tests can construct and validate them without the I2C master.

### BmsDevice Virtuals

| Method | Purpose |
|---|---|
| `init()` | Initialize hardware |
| `read_telemetry(out)` | ADC snapshot (voltages, currents, temps) |
| `read_status(out)` | Charging state + power source + fault flags |
| `get_charging_state(state)` | Lightweight single-register read |
| `get_battery_percentage(output)` | SOC estimate (0-100%) |
| `update_watchdog()` | Reset HW watchdog |
| `feature_ship_available()` | Query ship-mode capability |
| `enter_ship_mode()` | Power off (should not return) |
| `set_pmid_enabled(enabled)` | Enable/disable PMID boost converter (EN_OTG) |
| `resync_pmid()` | Re-apply full PMID configuration sequence + verify (recovery) |
| `set_charge_enable(enabled)` | Enable/disable battery charging current path |
| `set_charge_current_ma(mA)` | Set fast-charge current limit (CC mode) |
| `set_watchdog_timeout_ms(ms)` | Configure chip-level watchdog timeout |

The `set_pmid_enabled` primitive is retained for explicit PMID lifecycle
control (e.g. shutdown sequencing). It is no longer driven by
per-measurement PM cycling on AGo: `BQ25629Bms::init()` arms `EN_OTG=1`
once for the session and the chip handles buck↔boost transitions
autonomously.

`resync_pmid()` re-applies the full PMID configuration sequence (the
same preamble + arm + verify used by `init()`). Used by the product
layer when an external observation suggests the chip may have
autonomously cleared `EN_OTG` (BAT_OTGZ / OTG hiccup / TS faults per
datasheet §8.3.10.3-4). Blocking call; includes a settle delay.

See `products/go/docs/power_management.md` for the cell-OCP rationale
behind session-armed PMID and the PM-invalid-hint recovery mechanism.

### FuelGaugeDevice Virtuals

| Method | Purpose |
|---|---|
| `ready()` | True when chip is attached and identified |
| `read_soc_percent(out)` | Predicted state-of-charge (0-100%) |
| `read_voltage_mv(out)` | Battery terminal voltage |
| `read_average_current_ma(out)` | Average current (signed) |
| `read_average_power_mw(out)` | Average power (signed) |
| `read_remaining_capacity_mah(out)` | Filtered remaining capacity |
| `read_full_charge_capacity_mah(out)` | Filtered full-charge capacity |
| `read_internal_temperature_c(out)` | Die temperature in degrees C |
| `read_flags(out)` | Chip flags register |

Only the runtime-poll surface is virtual. Boot-time Data Memory
operations (read/write cell config, factory reset) are non-virtual
methods on the concrete `BQ27427` class — product-level board init
calls those directly.

### Drivers

| Driver | IC | Notes |
|---|---|---|
| `drivers/bq25xx` | TI BQ25672 / BQ25798 | Multi-cell charger family |
| `drivers/bq25629` | TI BQ25629 | Single-cell charger; wraps the vendor `bq25629` component |
| `drivers/bq27427` | TI BQ27427 | Single-cell Impedance Track fuel gauge; implements `FuelGaugeDevice` |

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

- Per-command 2 Hz cap on Standard Commands (per-command, not
  aggregate). At the product's 30 s outer cadence each command runs at
  0.033 Hz — well below the ceiling.
- `t(BUF) >= 66 us` inter-packet bus-free time at 400 kHz. The ESP-IDF
  I2C driver's transaction framing provides this implicitly.

The driver does not know what cell the product uses, what counts as a
corrupted value, or whether `write_cell_config` should run. Those
decisions live in product-level board init code.

## Usage

```cpp
BQ25629Bms bms(i2c_bus, config, address);
if (!bms.init()) { /* handle */ }

BmsTelemetry telemetry;
bms.read_telemetry(telemetry);
```

See [`drivers/bq25629/bq25629_bms.h`](drivers/bq25629/bq25629_bms.h),
[`drivers/bq27427/bq27427.h`](drivers/bq27427/bq27427.h), and
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

The BQ27427 driver has no automated host tests (bus protocol — validated
at hardware-in-the-loop time). Product-level fuel-gauge logic
(`evaluate_fg_state`, SOC source switching) is tested in the Go product
test suite.
