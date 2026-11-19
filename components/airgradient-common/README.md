# airgradient-common

Shared measurement types, validation, air-quality helpers, and platform
abstractions used by components and products.

## Status

Implemented and used by shared components and product firmware. Pure data
transforms and selected RTOS behavior have native host coverage.

## Scope

Owns measurement representations and invalid sentinels, correction transforms,
air-quality classification and colors, RTOS wrappers, retained uptime, logging,
device-model selection, and small platform utilities.

Sensor acquisition belongs to `airgradient-sensors`. Products own scheduling,
which measurement view to display or transmit, persistence policy, and hardware
wiring. Keep new capability-specific services in their own components.

## Directory Layout

```text
components/airgradient-common/
  include/                    public types and platform interfaces
  tests/                      native transform and RTOS tests
  air_quality.cpp             classification and palette helpers
  measurement_corrections.cpp derived measurement views
  rtos.cpp                    firmware backend and host seams
  retained_uptime.cpp          session uptime across retained sleep
  common.cpp                  identity and platform helpers
  ag_i2c.cpp                  ESP-IDF I2C helpers
  Kconfig
```

## Public Includes

| Header | Responsibility |
|---|---|
| [`measures_types.h`](include/measures_types.h) | Measurement groups, units, sentinels, field validation |
| [`measurement_corrections.h`](include/measurement_corrections.h) | Correction configuration, validation, derived views |
| [`air_quality.h`](include/air_quality.h), [`rgb.h`](include/rgb.h) | Classification and unscaled colors |
| [`rtos.h`](include/rtos.h) | Timing, tasks, queues, notifications, synchronization |
| [`retained_uptime.h`](include/retained_uptime.h) | Completed session uptime minutes |
| [`common.h`](include/common.h), [`ag_log.h`](include/ag_log.h) | Identity, formatting, reset, logging |
| [`device_model.h`](include/device_model.h) | Build-selected device model |
| [`ag_i2c.h`](include/ag_i2c.h) | Hardware-dependent I2C helpers |

Include the header for the capability needed; concrete hardware helpers are
not substitutes for product BSP ownership.

## Design

### Measurement Validity

`Measures`, `MeasuresBasic`, and `MeasuresAGo` compose field groups with default
invalid values. A default-constructed measurement is not a successful zero
reading. Validate each consumed field using its specific helper, such as
`is_temp_valid()` or `is_pm_25_valid()`. A combined `is_valid()` can reject a
group whose other fields remain usable.

Callers that average samples must keep separate valid-sample counts per field.
The shared types do not perform acquisition or averaging. See the
[sensor component](../airgradient-sensors/README.md) for those operations.

### Corrections and Air Quality

`apply_measurement_corrections()` derives a `MeasuresAGo` view without modifying
the raw input. Validate correction settings with the dedicated helpers before
accepting them. Products decide which consumers receive raw or corrected data;
see [Go's correction policy](../../products/go/docs/measurement_corrections.md).

The `air_quality` namespace provides pure, hardware-independent helpers:

| API | Contract |
|---|---|
| `pm25_to_us_aqi(float)` | PM2.5 to US AQI using the 2024 breakpoints; negative/NaN input returns `INVALID_AQI` |
| `us_aqi_to_category(int)` | Negative values are invalid; values above 500 remain hazardous |
| `co2_to_category(int)` | Uses `CO2Data::is_valid()`; categories are independent of the US AQI scale |
| `category_to_rgb(category)` | Unscaled 8-bit `Rgb` channels; invalid/unknown categories return black |

Brightness and indicator source selection belong to the product. For Go's
presentation, see the [LED service](../../products/go/docs/led_service.md).

### RTOS and Retained Time

Firmware timing calls use the default `FreeRTOS` implementation. Host tests
that call `RTOS::delay_ms()` or `RTOS::get_time_ms()` must install an instance
with `RTOS::set_instance()` and clear it before destroying the mock. Task
creation is a stub under `TEST_HOST`; host tests do not run firmware tasks.
Queue and timing seams are described in [`rtos.h`](include/rtos.h).

`RtosMutex` owns its underlying mutex; check `is_valid()` after construction.
Locking waits indefinitely on firmware. Host synchronization behavior is not
proof of task scheduling, interrupt safety, or hardware race freedom.

`RTOS::get_time_ms()` measures the current boot. Retained time uses the RTC
clock across deep sleep. `retained_uptime` keeps the session start in retained
memory and reports completed minutes, saturating at the `uint32_t` maximum.
Uninitialized state or a backward clock restarts the session. This is uptime,
not wall-clock time or persisted history across loss of the RTC domain.

## Usage

```cpp
MeasuresBasic measures{};
// Acquisition fills whichever fields are available.
if (measures.temp_hum_a.is_temp_valid()) {
    consume_temperature(measures.temp_hum_a.temperature);
}
```

## Configuration

The `AG_DEVICE_MODEL` Kconfig choice selects one model for shared endpoint and
model behavior. Product defaults should select the intended member.

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_AG_DEVICE_MODEL_ONE_OPEN_AIR` | `y` | One / Open Air model |
| `CONFIG_AG_DEVICE_MODEL_MAX` | `n` | Max model |
| `CONFIG_AG_DEVICE_MODEL_GO` | `n` | Go model |

## Dependencies

Firmware uses ESP-IDF FreeRTOS, timers, system and heap services, application
metadata, drivers, and RTC support. Pure measurement headers and selected
implementations compile with `TEST_HOST`; I2C hardware helpers require ESP-IDF.
See [`CMakeLists.txt`](CMakeLists.txt) for the build boundary.

## Tests

Run through the [host-test workflow](../../tests/README.md). Component tests
cover [air quality](tests/air_quality.tests.cpp),
[corrections](tests/measurement_corrections.tests.cpp), and
[RTOS/retained uptime](tests/rtos.tests.cpp). Hardware-dependent utilities and
actual FreeRTOS scheduling require firmware or hardware verification.
