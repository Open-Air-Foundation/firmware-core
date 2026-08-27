# AGo BLE Integration Tests

Hardware integration tests for the AirGradient Go BLE service. Connects to a
real AGo device over Bluetooth Low Energy and verifies the GATT protocol:
service discovery, CBOR payloads, data types, notification flows, and the
history download state machine.

## Prerequisites

- Python >= 3.11
- Linux with BlueZ (or macOS with CoreBluetooth)
- An AGo device advertising in **Portable** mode
- The device must already be **bonded/paired** with the host machine

## Install

```bash
pip install -e products/go/tests/ble-integration
```

## Run

```bash
# Auto-scan for a device whose name starts with "AirGradient Go "
pytest products/go/tests/ble-integration/ -v

# Specify a device address explicitly
pytest products/go/tests/ble-integration/ -v --ago-address AA:BB:CC:DD:EE:FF

# Run a single test file
pytest products/go/tests/ble-integration/test_measures.py -v
```

### CLI Options

| Option | Default | Description |
|---|---|---|
| `--ago-address` | _(auto-scan)_ | BLE address or name of the AGo device |
| `--ago-scan-timeout` | `10` | Seconds to scan before giving up |
| `--ago-notify-timeout` | `30` | Max seconds to wait for a notification |

### Viewing BLE Payloads

All reads, writes, and notifications are logged at `DEBUG` level under the
`ago_ble_test` logger. Pass `--log-cli-level=DEBUG` to print them live:

```bash
pytest products/go/tests/ble-integration/ -v --log-cli-level=DEBUG
```

Each line is prefixed with the direction and characteristic name, and the
payload is shown as a decoded CBOR value (or a hex dump for binary data):

```text
DEBUG ago_ble_test:conftest.py WRITE Config   {'op': 'set', 'temp_f': True}
DEBUG ago_ble_test:conftest.py NOTIFY Config   {'type': 'config', 'temp_f': True, ...}
DEBUG ago_ble_test:conftest.py READ   Status   {'bat_pct': 87, 'used_kb': 8192, ...}
DEBUG ago_ble_test:conftest.py NOTIFY History  <binary 58B> 0102...
```

## Test Overview

### `test_service_discovery.py` — GATT Profile (3 tests)

Verifies the AGo service UUID and all four characteristic UUIDs are present
with the correct properties (read/write/notify).

### `test_measures.py` — Measures Characteristic (6 tests)

Subscribes to Measures notifications once (module-scoped fixture), captures a
single notification, then validates it across all tests:

- A notification arrives within the timeout
- Payload is valid CBOR decoding to a map
- `"ts"` (unix timestamp) is always present
- All keys belong to the known set (`t`, `h`, `pm1`, `pm25`, `pm10`, `co2`,
  `tvoc`, `nox`, `pres`, `lat`, `lon`, `alt`, `fix`, `sat`, `ts`)
- Each field has the correct CBOR type (float, uint, float64)
- GPS fields follow the grouping rule: `fix`+`sat` always appear together;
  position fields only appear when the fix group is present

### `test_status.py` — Status Characteristic (6 tests)

Reads the Status characteristic once (module-scoped fixture), then validates
the payload across all tests:

- Payload is a 9-key CBOR map with all expected keys
- Field types match the spec (uint, float, str, bool)
- `"charging"` is a known enum string
- `"bat_pct"` is in 0-100 range
- `"bat_v"` is non-negative

The firmware version is no longer part of Status; it is validated via DIS
(see `test_device_info.py`).

### `test_status_notify.py` — Status NOTIFY (tracking transitions)

Verifies the device pushes Status only on urgent tracking transitions, and that
each push is a **delta** carrying just `{tracking, session}` while a Status READ
still returns the full 9-key snapshot:

- `start_tracking` → Status delta `tracking=true`, `session>0`; matching Config
  `cmd_result`; READ returns all 9 keys
- `stop_tracking` → Status delta `tracking=false`, `session=0`; READ returns all
  9 keys
- a redundant `start_tracking` (already tracking) sends no spurious Status NOTIFY

### `test_config.py` — Config Characteristic (read/write/notify/command)

Covers read, write, delta-notify, and command operations:

- **Read** (sync): reads Config once (module-scoped fixture), then validates the
  17 config keys present with correct types; versioned correction
  arrays, `gps_mode`, and `op_mode` use valid fields and enum values
- **Set config** (async): writes a single-field `{"op": "set", ...}`, verifies
  the device sends a Config **delta** notification — `"type": "config"` plus
  only the changed key
- **Compact device fields** (async): round-trips buzzer, CO2 ABC, TVOC learning,
  and NOx learning values, verifies exact deltas, then restores each value
- **No-op set** (async): writing an unchanged value emits no notification
- **Correction set** (async): writes a complete temperature correction group,
  verifies the nested Config delta without a Measures notification, then
  restores the original group
- **Correction validation** (async): an unsupported algorithm returns
  `invalid_config_value` and leaves the persisted group unchanged
- **Single-field enforcement** (async): a `set` with more than one config key is
  rejected `single_field_only` and applies nothing; an aiding key (`lat`) under
  `op:"set"` is rejected `unknown_config_key`
- **Presentation units** (async): toggles `temp_f` and `alt_ft`, verifies exact
  deltas and readback, then restores the original values
- **Notify field types** (async): triggers a Config delta and type-checks the
  keys present in it
- **Command** (async): writes `{"op": "cmd", "cmd": "co2_cal"}`, verifies the
  `cmd_progress` then `cmd_result` notification formats, and that a Config
  **READ after a command still returns the full config snapshot** (not the
  `cmd_result`)

### `test_history.py` — History Characteristic (10 tests)

Exercises the full download protocol. Download tests select a session with at
least one point and are **skipped** when the device has no non-empty sessions.

- **List**: writes `{"op": "list"}`, verifies `"sessions"` array with
  `id`/`pts`/`ts` per entry
- **Start download**: verifies `"started"` response with `session`, `total`,
  `pt_size=56`
- **Binary format**: validates tag `0x01`, uint16 LE point index, and 56-byte
  RoutePointWire struct layout
- **Done count**: `"done"` response `"sent"` matches `"started"` `"total"`
- **End**: `"ended"` response after `{"op": "end"}`
- **Errors**: invalid session ID returns `"session_not_found"`; `fill` without
  active download returns `"no_active_download"`
- **Delete**: a nonexistent session returns `"session_not_found"`; deleting a
  listed non-active session returns `"deleted"` and removes only that session

### `test_device_info.py` — Device Information Service (5 tests)

Verifies the standard DIS (`0x180A`), present in Portable mode, exposes valid
read-only identity strings over the encrypted bonded link:

- DIS service is present in the GATT table
- Firmware Revision (`0x2A26`) is a non-empty string — the canonical
  firmware-version source that replaced the former Status `"fw"` key
- Model Number (`0x2A24`) is a non-empty string
- Serial Number (`0x2A25`) is a 12-char lowercase hex string
- Manufacturer Name (`0x2A29`) is `"AirGradient"`

## File Structure

```text
products/go/tests/ble-integration/
  pyproject.toml              Dependencies and pytest config
  conftest.py                 Fixtures: scan, connect, notification collectors
  ago_protocol.py             UUIDs, CBOR key sets, type maps, encode/decode
  test_service_discovery.py   GATT service and characteristic verification
  test_measures.py            Measures read + notification tests
  test_status.py              Status read tests
  test_status_notify.py       Status NOTIFY on tracking transitions
  test_config.py              Config read/write/notify tests
  test_history.py             History download flow tests
  test_device_info.py         Device Information Service (DIS) read tests
```

## Design Notes

- **Single connection**: the BLE connection is session-scoped (one connect for
  the entire pytest run, disconnect at teardown).
- **Session-scoped event loop**: all async fixtures and tests share a single
  session-scoped asyncio event loop (`asyncio_default_fixture_loop_scope` and
  `asyncio_default_test_loop_scope` both set to `"session"` in
  `pyproject.toml`). This is required because the BLE client (and its
  underlying BlueZ D-Bus connection) is session-scoped — async operations on
  any other event loop would fail with a loop mismatch error.
- **Shared read fixtures**: read-only tests (Measures, Status, Config read) use
  module-scoped fixtures that fetch data once and share it across all tests in
  the module. This avoids redundant BLE I/O and keeps validators synchronous.
- **Per-test notification fixtures**: interactive tests (Config write, History)
  use function-scoped `NotificationCollector` fixtures that
  subscribe/unsubscribe per test.
- **Non-destructive config tests**: original settings are read, modified, then
  restored after each test.
- **No security handling**: assumes the device is already bonded. If not bonded,
  authenticated reads/writes will fail with a bleak error.
