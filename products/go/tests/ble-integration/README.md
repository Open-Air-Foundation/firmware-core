# AirGradient Go BLE Integration Tests

Hardware integration tests for the AirGradient Go BLE service. Connects to a
real AGo device over Bluetooth Low Energy and verifies the GATT protocol:
service discovery, CBOR payloads, data types, notification flows, and the
history download state machine.

## Prerequisites

- Python >= 3.11
- Linux with BlueZ (or macOS with CoreBluetooth)
- An unlocked AGo device advertising the Go data service
- The device must already be **bonded/paired** with the host machine
- Finish Getting Started and Wi-Fi setup before running the full suite; do not
  run provisioning, History export from another client, or OTA during the tests

## Install

Run from the repository root in a Python virtual environment:

```sh
pip install -e products/go/tests/ble-integration
```

## Run

The full suite changes device state; read [Device Side Effects](#device-side-effects)
before running it. Auto-scan chooses the first matching device, so select an
explicit address when more than one Go is nearby.

```sh
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
| `--ago-address` | _(auto-scan)_ | BLE address or platform device identifier; passed directly to Bleak, without name lookup |
| `--ago-scan-timeout` | `10` | Seconds to scan before giving up |
| `--ago-notify-timeout` | `30` | Max seconds to wait for a notification |
| `--ago-board-variant` | `v1` | Expected board: `v1` or `v2`; does not change firmware board detection |

Run configuration-write tests with UV from the repository root:

```sh
uv run --project products/go/tests/ble-integration pytest \
  products/go/tests/ble-integration/test_config.py::TestConfigWrite \
  -v --ago-board-variant=v2
```

Touch LED cases verify V1 updates and restores the setting. On V2 they require
an initial Off value, rejection of Off/Dim/Bright writes with `unsupported`,
unchanged configuration, and no Config delta. Use `v1` for a V1 board.

### Viewing BLE Payloads

All reads, writes, and notifications are logged at `DEBUG` level under the
`ago_ble_test` logger. Pass `--log-cli-level=DEBUG` to print them live:

```sh
pytest products/go/tests/ble-integration/ -v --log-cli-level=DEBUG
```

Each line is prefixed with the direction and characteristic name, and the
payload is shown as a decoded CBOR value (or a hex dump for binary data):

```text
DEBUG ago_ble_test:conftest.py WRITE Config   {'op': 'set', 'temp_f': True}
DEBUG ago_ble_test:conftest.py NOTIFY Config   {'type': 'config', 'temp_f': True, ...}
DEBUG ago_ble_test:conftest.py READ   Status   {'bat_pct': 87, 'used_kb': 8192, ...}
DEBUG ago_ble_test:conftest.py NOTIFY History  <binary idx=2 record_bytes=56>
```

## Device Side Effects

The BLE suite has **no opt-in gates** for mutations. Use a dedicated device,
export any routes and measurement logs you need to keep, and run one test process at a time:

- Config write tests change settings, including `log_m`, and attempt restoration
  in cleanup. An interrupted run or lost connection can leave changed settings behind.
- `TestConfigCommand` in `test_config.py` requests physical CO2 calibration.
  Run it only under the sensor's calibration conditions. Use
  `--ago-notify-timeout=90` for these cases; the default 30 seconds is shorter
  than the firmware's approximately 60-second calibration polling window.
- `test_status_notify.py` and `test_tracking_duration.py` stop any existing
  tracking session, create test sessions, exercise pause/resume, and leave
  tracking idle. Each temporarily
  sets GPS Off and restores the preference afterward to avoid a GPS-triggered
  restart during notification tests. The original session is not restored.
- `TestHistoryDelete::test_delete_session` deletes an existing non-active
  route. Deleted data is not restored.
- `test_measurement_history.py::test_delete_closed_log_ends_export` deletes one
  existing measurement log when logging is already disabled. Other measurement
  History cases use existing logs and do not change the logging preference.
- Toggling `log_m` can create logs and trigger automatic oldest-log deletion if
  storage is low; restoring the preference does not undo those file changes.

For an initial read/notification check without those mutations:

```sh
pytest products/go/tests/ble-integration/test_service_discovery.py \
  products/go/tests/ble-integration/test_measures.py \
  products/go/tests/ble-integration/test_status.py \
  products/go/tests/ble-integration/test_device_info.py -v \
  --ago-address AA:BB:CC:DD:EE:FF
```

Choose a notification timeout longer than `idle_int` (default 300 seconds), or
`tracking_int` while Recording or Paused,
when checking Measures. After a failed mutation test, read back settings and
tracking state before retrying; a timeout does not undo a device action.

## Test Overview

### `test_service_discovery.py` — GATT Profile

Verifies the AGo service UUID and all four characteristic UUIDs are present
with the correct properties (read/write/notify). Also checks that provisioning
and its Scan and Credentials/Status characteristics share this connection.

### `test_measures.py` — Measures Characteristic

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

### `test_status.py` — Status Characteristic

Reads the Status characteristic once (module-scoped fixture), then validates
the payload across all tests:

- Payload is an 11-key CBOR map with all expected keys, including `trk` and `stop`
- Field types match the spec (uint, float, str, bool)
- `trk` is an integer enum: 0 = Idle, 1 = Recording, 2 = Paused; the legacy
  `tracking` boolean and session ID agree with that state
- `stop` is an integer: 0 = None, 1 = User, 2 = Duration; active sessions use 0
- `"charging"` is a known enum string
- `"bat_pct"` is in 0-100 range
- `"bat_v"` is non-negative

The firmware version is no longer part of Status; it is validated via DIS
(see `test_device_info.py`).

### `test_status_notify.py` — Status Notifications

Verifies tracking transitions through the existing Status and Config
characteristics. Each tracking push is a **delta** carrying
`{tracking, session, trk, stop}`, while Status READ returns the full 11-key snapshot:

- `start_tracking` → `tracking=true`, `session>0`, `trk=1`, `stop=0`; matching Config
  `cmd_result`; READ returns all 11 keys
- `stop_tracking` → `tracking=false`, `session=0`, `trk=0`, `stop=1`; READ agrees
- `pause_tracking` / `resume_tracking` → `trk=2` / `trk=1`; `tracking` remains
  true and the session ID stays unchanged in both NOTIFY and READ; each command
  has a matching successful Config result
- a redundant `start_tracking` (already tracking) sends no spurious Status NOTIFY
- `release_ble` while Recording or Paused returns `busy`, preserves the session,
  and keeps the BLE connection

Clients should use `trk` for the full tracking state;
the tests continue checking `tracking` because the current firmware retains it
for compatibility. See the [BLE client guide](../../go_ble_client.md#6-status-characteristic).

### `test_tracking_duration.py` — Tracking Duration

Checks duration bounds and CBOR types, missing/duplicate/misplaced arguments,
Idle rejection, and unchanged settings after rejected writes. Duration updates
and cancellation produce only a Config `cmd_result`, without a preference delta
or tracking transition.

Six countdown cases take about seven minutes altogether. They verify expiry
while Recording or Paused, resume preserving the deadline, replacement counting
from the update, cancellation, and manual Stop clearing the timer before a new
Start. Expiry must notify `stop=2` without a command result; the next Start resets
`stop=0`. Charging deltas are ignored while waiting for tracking transitions.

Run only these tests with `pytest products/go/tests/ble-integration/test_tracking_duration.py -v`.
The one-minute waits use their own deadline rather than `--ago-notify-timeout`.

### `test_config.py` — Config Operations

Covers read, write, delta-notify, and command operations:

- **Read** (sync): reads Config once (module-scoped fixture), then validates the
  expected config keys present with correct types; versioned correction
  maps and their value arrays, `bledi`, and `gps_mode` use valid fields and enum
  values. The 20-key snapshot includes `idle_int`, `tracking_int`, `wifi`, and `log_m`
- **Set config** (async): writes a single-field `{"op": "set", ...}`, verifies
  the device sends a Config **delta** notification — `"type": "config"` plus
  only the changed key
- **Compact device fields** (async): round-trips buzzer, CO2 ABC, TVOC learning,
  NOx learning, Pinned Chart (`pchart`), LED indicator (`bledi`: `aqi`/`co2`),
  and LED brightness (`bled`) values, verifies exact deltas, then
  restores each value
- **Intervals** (async): round-trips `idle_int` and `tracking_int` independently,
  verifies exact deltas and preservation of the other interval, then restores
  the changed preference
- **Compatibility and validation** (async): retired `op_mode` / `meas_int`
  writes, including mixed requests, return `unknown_config_key` without changing
  settings. Invalid intervals and Wi-Fi types return `invalid_config_value`.
  Unchanged `wifi` writes are checked without opening setup
- **LED indicator validation** (async): invalid names and CBOR types return
  `invalid_config_value` and preserve the config snapshot. LED round trips also
  verify that changing indicator or brightness preserves the other setting.
- **Measurement logging** (async): `log_m` is a boolean in the 20-key snapshot;
  toggling it emits an exact delta and restores the original preference.
  Non-boolean values are rejected without a delta or configuration change
- **No-op set** (async): writing an unchanged value, including `log_m`, emits
  no notification
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

### `test_history.py` — History Characteristic

Exercises the full download protocol. Download tests select a session with at
least one point and at most 65,535 points; they skip when no eligible session exists.

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

### `test_measurement_history.py` — Measurement Logs

Checks `kind:"measurements"` cursor pagination, version-1 39-byte records,
`started.total` snapshot bounds, deliberate gap recovery with `fill`, empty
fills, end cleanup, and missing-log errors. Download cases skip without stored
logs. Deletion ends an active export, removes only the chosen log, and preserves
route IDs; it skips while `log_m` is enabled.

### `test_protocol_offline.py` — Offline Protocol Checks

Checks command encoding, binary offsets, signed 64-bit UTC (including unknown
time), invalid sentinels, malformed packets, route compatibility, and download
helper behavior with simulated notifications. No device fixtures are used.

### `test_device_info.py` — Device Information Service

Verifies the standard DIS (`0x180A`) exposes valid
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
  test_tracking_duration.py   Duration validation and real countdowns
  test_config.py              Config read/write/notify tests
  test_history.py             Route History download flow tests
  test_measurement_history.py  Measurement History protocol tests
  test_protocol_offline.py     Offline decoder and helper checks
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
- **Config cleanup**: write tests attempt to restore original settings; this
  does not reverse calibration or route deletion.
- **No security handling**: assumes the device is already bonded. If not bonded,
  authenticated reads/writes may trigger platform pairing or fail; the suite
  does not drive the passkey exchange.

## Coverage Limits

These are physical-device protocol tests, separate from the
[native host tests](../../../../tests/README.md). The authoritative wire contract
is the [BLE client reference](../../go_ble_client.md); test constants live in
[`ago_protocol.py`](ago_protocol.py). Exact key-set assertions target the current
firmware and are stricter than the forward-compatible parsing expected of apps.

The suite does not cover sleep-cycle broadcasts, provisioning requests, Wi-Fi
enable/setup transitions, successful `release_ble` disconnects, OTA transfer,
fresh pairing, charging/disconnect notifications, or GPS-restart reconnect
recovery. Route History tests skip downloads without an eligible route. Measurement
History tests discard one received record to exercise gap filling but do not
simulate radio loss. They do not establish persistence across reboot, storage
reclamation under pressure, live-writer deletion rejection, or the 60-second
export timeout. Passing schema and notification checks does not establish physical sensor accuracy or calibration quality.

To inspect collection without connecting to a device:

```sh
pytest products/go/tests/ble-integration/ --collect-only -q
```

Collection confirms test discovery only. Record the firmware revision, board,
host platform, selected tests, skips, and results for a hardware verification run.

Run the offline checks separately, without Bluetooth access:

```sh
uv run --project products/go/tests/ble-integration pytest \
  products/go/tests/ble-integration/test_protocol_offline.py -q
```
