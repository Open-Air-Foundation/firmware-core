# Components

Shared AirGradient ESP-IDF components used across product firmware.

Each component owns one clear responsibility and stays reusable across
different product models. Product-specific BSP and application wiring belong
under `products/`, not here.

This folder also hosts third-party or vendor code when that code is used as
part of a shared component. For example, a reusable sensor driver from a
third party can live here if it is part of the shared firmware foundation.

## First-Party Components

| Component | Responsibility |
|---|---|
| [`ads1115`](ads1115/) | Generic ADS1115 ADC helper used by sensor drivers such as AlphaSense |
| [`airgradient-ble`](airgradient-ble/README.md) | BLE peripheral HAL and NimBLE-backed GATT server, characteristic management, and advertising control |
| [`airgradient-bms`](airgradient-bms/README.md) | Battery management HAL, public BMS types, and concrete charger / PMIC drivers (e.g. BQ25XX) |
| [`airgradient-cellular`](airgradient-cellular/README.md) | Cellular modem HAL, shared cellular types, AT-command service, and modem drivers (scaffold) |
| [`airgradient-common`](airgradient-common/) | Shared measurement types, air-quality classification/colors, RTOS abstraction, and retained uptime |
| [`airgradient-config`](airgradient-config/README.md) | Typed key-value persistence interface and reusable backends (NVS) |
| [`airgradient-gpio`](airgradient-gpio/README.md) | GPIO HAL and ESP-IDF-backed driver for pin control and interrupt registration |
| [`airgradient-nand-storage`](airgradient-nand-storage/README.md) | SPI NAND flash HAL providing FATFS mount/unmount lifecycle for application POSIX I/O |
| [`airgradient-payload-cache`](airgradient-payload-cache/README.md) | Bounded FIFO payload queue and RTC-retained storage backend for `Measures`-derived payloads |
| [`airgradient-sensors`](airgradient-sensors/README.md) | Environmental sensor HALs, concrete drivers, and shared orchestration (`SensorManager`) |
| [`airgradient-serial`](airgradient-serial/) | Serial transport abstractions (UART, I2C-to-UART bridge) used by sensors and other peripherals (no README yet) |
| [`airgradient-touch`](airgradient-touch/README.md) | Capacitive touch HAL and CAP1203 driver with noise detection and recalibration |
| [`airgradient-watchdog`](airgradient-watchdog/README.md) | Stateless external watchdog initialization and GPIO feed pulse |

Each first-party component README follows the
[component README template](../docs/templates/component_readme.md).

## Shared Air Quality Helpers

[`air_quality.h`](airgradient-common/include/air_quality.h) provides pure,
hardware-independent helpers in the `air_quality` namespace:

| API | Purpose |
|---|---|
| `pm25_to_us_aqi(float)` | PM2.5 to US AQI using the 2024 breakpoints; negative/NaN returns `INVALID_AQI` |
| `us_aqi_to_category(int)` | US AQI to `UsAqiCategory`; negative values are invalid |
| `co2_to_category(int)` | CO2 ppm to `Co2Category`, using `CO2Data::is_valid()` (0–10000 ppm) |
| `category_to_rgb(category)` | Overloads for both category types; invalid/unknown categories return black |

[`rgb.h`](airgradient-common/include/rgb.h) defines unscaled 8-bit `Rgb`
channels. Brightness and source selection belong to the product. CO2 categories
are independent of the US AQI scale; see Go's
[color tables](../products/go/docs/led_service.md#co2-color-map).
Boundary and palette tests live in
[`air_quality.tests.cpp`](airgradient-common/tests/air_quality.tests.cpp).

## Vendor / Third-Party Components

These live alongside first-party components but are upstream-owned. Their
docs are not subject to `docs/STYLE.md` and are intentionally excluded from
markdownlint enforcement.

| Component | Source | Used by |
|---|---|---|
| `bq25629` | Texas Instruments BQ25629 driver (slated for replacement) | `airgradient-bms` |
| `embedded-i2c-scd4x` | Sensirion SCD4x I2C driver | `airgradient-sensors` |
| `esp-nimble-cpp` | NimBLE C++ wrapper | `airgradient-ble` |
| `sensirion-gas-index-algorithm` | Sensirion gas-index algorithm (float variant, v3.2.0) | `airgradient-sensors` (`SensorManager`) |
| `libnmea-esp32` | NMEA parser library | `products/go` GPS service |
| `u8g2` | u8g2 graphics library | `products/go` display service |

## Guidelines

- Keep product-specific BSP and application wiring out of this folder.
- Place shared capability code here only when it is reusable across
  products.
- Prefer capability-focused components over generic catch-all components.
- Vendor or third-party code may live here if it belongs to a shared
  component responsibility.

As the firmware grows, new shared capabilities such as connectivity,
display, or power can follow the same pattern and live alongside these
components.
