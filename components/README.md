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
| [`ads1115`](ads1115/README.md) | Generic ADS1115 ADC helper used by sensor drivers such as AlphaSense |
| [`airgradient-ble`](airgradient-ble/README.md) | BLE peripheral HAL and NimBLE-backed GATT server, characteristic management, and advertising control |
| [`airgradient-bms`](airgradient-bms/README.md) | Battery management HAL, public BMS types, and concrete charger / PMIC drivers (e.g. BQ25XX) |
| [`airgradient-cellular`](airgradient-cellular/README.md) | Cellular modem HAL, shared cellular types, AT-command service, and modem drivers (scaffold) |
| [`airgradient-client`](airgradient-client/README.md) | AirGradient cloud HTTP client, payload serialization, and response mapping |
| [`airgradient-common`](airgradient-common/README.md) | Shared measurement types, air-quality classification/colors, RTOS abstraction, and retained uptime |
| [`airgradient-config`](airgradient-config/README.md) | Typed key-value persistence interface and reusable backends (NVS) |
| [`airgradient-gpio`](airgradient-gpio/README.md) | Context-aware GPIO HAL, pin bindings, native GPIO, and TCA6408A expander driver |
| [`airgradient-http-server`](airgradient-http-server/README.md) | HTTP listener, routes, request/response abstractions, and ESP-IDF backend |
| [`airgradient-local-server`](airgradient-local-server/README.md) | Versioned local API routes and wire schema over product-supplied providers |
| [`airgradient-nand-storage`](airgradient-nand-storage/README.md) | SPI NAND flash HAL providing FATFS mount/unmount lifecycle for application POSIX I/O |
| [`airgradient-ota`](airgradient-ota/README.md) | Shared image writer, Wi-Fi pull updates, and BLE push updates |
| [`airgradient-payload-cache`](airgradient-payload-cache/README.md) | Bounded FIFO payload queue and RTC-retained storage backend for `Measures`-derived payloads |
| [`airgradient-provisioning`](airgradient-provisioning/README.md) | Wi-Fi onboarding over BLE or a captive portal, including attached BLE sessions |
| [`airgradient-sensors`](airgradient-sensors/README.md) | Environmental sensor HALs, concrete drivers, and shared orchestration (`SensorManager`) |
| [`airgradient-serial`](airgradient-serial/README.md) | Serial transport abstractions (UART, I2C-to-UART bridge) used by sensors and other peripherals |
| [`airgradient-touch`](airgradient-touch/README.md) | Capacitive touch HAL and CAP1203 driver with noise detection and recalibration |
| [`airgradient-watchdog`](airgradient-watchdog/README.md) | Stateless external watchdog initialization and GPIO feed pulse |
| [`airgradient-wifi`](airgradient-wifi/README.md) | Wi-Fi HAL and manager, saved networks, auto-connect, and mDNS profiles |

Each first-party component README follows the
[component README template](../docs/templates/component_readme.md).

## Vendor / Third-Party Components

These live alongside first-party components but are upstream-owned. Their
docs are not subject to `docs/GUIDE.md` and are intentionally excluded from
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

Use each component README as its entry point. Detailed topic documents, when
needed, live in that component's `docs/` directory and are linked from its README.
Follow the [documentation guide](../docs/GUIDE.md) when adding or updating them.
