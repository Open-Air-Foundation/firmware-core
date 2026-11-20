# AirGradient Go

Firmware for the [AirGradient Go](https://www.airgradient.com/portable/)
portable air quality monitor with GPS, e-paper display, BLE, and battery.

## Sensors

- **PM** — Sensirion SPS30 (PM1.0, PM2.5, PM10 mass + particle counts)
- **CO2** — SenseAir S12 / Sensirion SCD4x / Sensirion STCC4 (probed in
  order at boot; first detected wins)
- **TVOC / NOx** — Sensirion SGP41
- **Temp / Humidity** — SHT40 probed on both V1 and V2, with fallback to
  CO2-integrated temperature/humidity, then pressure-sensor temperature
- **Pressure** — DPS368 (V1) or SPL07-003 (V2)
- **Motion** — LIS2DH12; V1 shake detection, both boards support diagnostics
- **Battery** — BQ25629 charger driver; BQ27427 (V1) or BQ27742 (V2) fuel gauge

## Hardware Notes

The firmware selects **V1 or V2** from eFuse at runtime. Blank, unknown, or
unreadable IDs select V1; peripheral failures never change the selection.
V2 uses a TCA6408A expander, four back LEDs, and no touch LEDs or shake detection.
See [Hardware Initialization](docs/hardware_init.md#board-variants) for wiring,
board-ID tooling, and display initialization prerequisites.

The SPS30 depends on the charger-managed PMID rail. EN_PM isolates its I2C
bus; it does not switch off the supply. See
[PM Power Management](docs/power_management.md#why-pmid-is-session-armed)
before changing power sequencing.

See [Sensor Selection](docs/sensor_producer.md#ago-sensor-wiring) for
temperature/humidity fallback, and [Power Management](docs/power_management.md)
for battery safety, gauge telemetry, and shutdown behavior.

On battery, release the power button after a long press to turn off; holding it
through shutdown restarts the device. With USB attached, user shutdown enters
[charging-only operation](docs/power_management.md#charging-only-light-sleep).

For production setup, see the [manufacturing shortcut](docs/orchestrator.md#input-handling)
and [USB command service](docs/serial_command_service.md), available before onboarding.

## Documentation

- [Architecture](ARCHITECTURE.md) — system structure, boot, tasks, and data flow;
  start here before reading the detailed topic docs
- [Feature Overview](feature_overview.md) — product-facing behavior
- [Hardware Initialization](docs/hardware_init.md) and
  [board configuration](main/board_config.h) — board selection, wiring, and boot sequencing
- [BLE Client Reference](go_ble_client.md) — discovery, pairing, GATT, payloads,
  history, provisioning, and OTA
- [Local HTTP API](../../docs/local_http_api.md) — shared wire contract;
  [Go Local Server](docs/local_server.md) owns Go support, ranges, and policy
- [Hardware Test](docs/hardware_test.md) — on-device diagnostics;
  [Fuel-Gauge Learning](docs/fg_learning.md) describes the V1 factory routine
- [Host-Test Workflow](../../tests/README.md) and [Go tests](tests) — automated verification
- [BLE integration tests](tests/ble-integration/README.md) and
  [Local Server integration tests](tests/local-server-integration/README.md) —
  physical-device verification
