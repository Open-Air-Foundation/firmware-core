# Reference Product

ESP-IDF component integration and hardware smoke-test application. It provides
small experiments for shared components without requiring a shipping product's
full application lifecycle.

## Status

A developer test bed, not a shipping product or a future product scaffold.
The build compiles the smoke-test sources; `app_main()` dispatches one selected
test. It has no release tag or product VHUB template.

## Sensors

There is no fixed production sensor configuration. The selected test defines
which peripherals it exercises. For example, [sensor smoke tests](main/test_sensors.cpp)
construct specific sensor and charger drivers, while the
[accelerometer diagnostic](main/test_accel.cpp) exercises LIS2DH12 and its
interrupt path. Read the selected test's setup before connecting hardware.

## Hardware Notes

[board_config.h](main/board_config.h) defines the compile-time `BOARD_GO` and
`BOARD_MAX` pin maps. The checked-in selection is `BOARD_GO`; exactly one board
macro must be defined. Match the pins, buses, addresses, and power rails to the
actual board revision. This test bed does not use Go's runtime V1/V2 board
selection, so the `BOARD_GO` label alone is not proof of compatible wiring.

[main.cpp](main/main.cpp) initializes NVS and conditionally initializes power
GPIOs and I2C for the chosen test. Some tests initialize their own buses or
radios. Sensor tests include charger configuration; NAND tests can format after
a mount failure and overwrite their test file. Choose hardware and stored data
appropriate for the selected test's actions.

## Select a Test

1. In [main.cpp](main/main.cpp), find the test-selection block and leave exactly
   one `RUN_TEST_*` definition enabled. The checked-in selection is
   `RUN_TEST_ACCEL`. If no selector is defined, the source falls back to
   `RUN_TEST_SENSORS`.
2. Choose the board definition in [board_config.h](main/board_config.h) and
   verify its pins against the connected board.
3. Read the selected `test_*.cpp` for additional selectors, peripherals,
   credentials, endpoints, and expected results. Keep real credentials and
   device identifiers out of commits.
4. Build, then have the operator flash the firmware and inspect serial output.
   Flashing and serial monitoring are user-only operations in this repository.

Adding a compiler define does not remove the selectors already defined in
source. Multiple active selectors can initialize an unintended combination of
resources and dispatch only the first matching branch. Use the source selection
block consistently rather than relying on an extra define to override it.

### Test Entry Points

The complete selector list is in `main.cpp`. These groups provide a reading path:

| Area | Test Sources | Setup to Check |
|---|---|---|
| Sensors and motion | [Sensors](main/test_sensors.cpp), [accelerometer](main/test_accel.cpp) | Concrete devices, I2C addresses, rails, interrupt wiring |
| Board interfaces | [GPIO](main/test_gpio.cpp), [serial](main/test_serial.cpp), [touch](main/test_touch.cpp) | Selected pin map, serial bridge channels, touch controller |
| Persistence | [Config](main/test_config.cpp), [payload cache](main/test_payload_cache.cpp), [NAND](main/test_nand_storage.cpp) | NVS use, retained state, SPI wiring, filesystem writes |
| Connectivity | [BLE](main/test_ble.cpp), [Wi-Fi](main/test_wifi.cpp), [provisioning](main/test_provisioning.cpp) | BLE settings, network inputs, transport-specific selectors |
| HTTP and cloud | [HTTP server](main/test_http_server.cpp), [local server](main/test_local_server.cpp), [cloud client](main/test_airgradient_client.cpp) | Network access, endpoints, credentials, registered test device |
| Firmware update | [OTA](main/test_ota.cpp) | `TEST_OTA_BLE`, image/partition setup, transport and apply options |

The OTA source currently defines `TEST_OTA_BLE`; selecting `RUN_TEST_OTA`
therefore exercises BLE push. Remove that local selector to exercise its Wi-Fi
pull branch. Review the product partition configuration before attempting an
image write: the checked-in defaults use a single factory application partition,
not an OTA partition layout.

## Build

Initialize submodules once per checkout:

```sh
git submodule update --init --recursive
```

Use the ESP-IDF version pinned by [firmware CI](../../.github/workflows/firmware-build.yml),
currently v5.5.6. CI builds reference for ESP32-C5. From the repository root,
with the export path adjusted to your installation:

```sh
. "$HOME/Tools/esp/esp-idf/export.sh"
idf.py -C products/reference -DIDF_TARGET=esp32c5 build
```

The application sources and dependencies are listed in
[main/CMakeLists.txt](main/CMakeLists.txt). All listed smoke-test sources compile
even though only one runs. Changing `RUN_TEST_*` does not exclude unrelated
sources from compilation. [sdkconfig.defaults](sdkconfig.defaults) supplies
initial configuration, including NimBLE; existing `sdkconfig` values can differ.

## Interpret Results

Confirm the boot banner names the selected test, then inspect that test's
results and the physical behavior it exercises. There is no common automated
pass/fail result for the whole reference application.

- Tests with assertions or result summaries require all intended cases to pass.
  For example, NAND reports a read-back `PASS` or `FAIL`.
- Interactive diagnostics can stay running indefinitely. Verify the expected
  activity rather than waiting for a completion banner.
- A generic `Test complete` message only means the selected function returned.
  A test can return after an initialization failure or a skip.
- Missing hardware, disabled BLE, or skipped cases do not establish a pass.
  Record the missing prerequisite separately from an observed failure.
- An initialization success is narrower than full functional coverage. For
  example, the serial smoke test checks backend initialization; it does not
  prove a sensor protocol exchange or validate every baud rate.

Record the selected test, board/revision, relevant configuration, observed
result, and focused logs when reporting a hardware check. Keep local selection
and credential edits separate from the component change being verified.

## Documentation

- [Component index](../../components/README.md) — shared contracts and detailed
  component documentation.
- [Host-test workflow](../../tests/README.md) — repeatable native logic tests.
- [main.cpp](main/main.cpp) — selection, shared initialization, and dispatch.
- [board_config.h](main/board_config.h) — compile-time hardware bindings.
- [Documentation guide](../../docs/GUIDE.md) — documentation ownership and style.

This README is the reference application's entry point. Add detailed topic docs
only when a substantial bring-up procedure needs its own explanation; shipping
product architecture and release-verification requirements belong to those
products.
