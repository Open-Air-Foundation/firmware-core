# AGo Local Server Integration Tests

Hardware integration tests for the AirGradient Go Local Server API. The suite
connects to a real device in Stationary mode and verifies mDNS discovery, HTTP
routes, JSON schemas, configuration admission, structured errors, actions, and
OTA access policy.

## Prerequisites

- Python >= 3.11
- An AirGradient Go connected to the same network as the test host
- The device running in **Stationary** mode
- Multicast DNS available between the host and device, or the device IP address

## Install

```sh
pip install -e products/go/tests/local-server-integration
```

## Run

Auto-discover one Go device through `_airgradient._tcp.local.`:

```sh
pytest products/go/tests/local-server-integration/ -v
```

Target a device by URL or serial number:

```sh
pytest products/go/tests/local-server-integration/ -v \
  --ago-url http://192.168.1.20

pytest products/go/tests/local-server-integration/ -v \
  --ago-serial aabbccddeeff
```

Run one test file:

```sh
pytest products/go/tests/local-server-integration/test_measures.py -v \
  --ago-url http://192.168.1.20
```

### CLI Options

| Option | Default | Description |
|---|---|---|
| `--ago-url` | _(mDNS)_ | Explicit Local Server base URL |
| `--ago-serial` | _(any)_ | Select and validate one serial number |
| `--ago-discovery-timeout` | `10` | mDNS discovery timeout in seconds |
| `--ago-http-timeout` | `5` | Per-request HTTP timeout in seconds |
| `--ago-convergence-timeout` | `20` | Config convergence timeout in seconds |
| `--ago-allow-config-write` | off | Enable persisted config round trips and restoration |
| `--ago-allow-calibration` | off | Enable physical CO2 calibration |
| `--ago-ota-active` | off | Confirm committed OTA is active |

### Viewing HTTP Traffic

Requests and responses are logged at `DEBUG` level under the
`ago_local_api_test` logger:

```sh
pytest products/go/tests/local-server-integration/ -v \
  --ago-url http://192.168.1.20 --log-cli-level=DEBUG
```

## Safety

The default suite does not change durable configuration. It submits only an
empty configuration update, malformed requests, and the LED and GPS test
actions. The GPS action leaves the live GPS Test screen open for the operator to
exit.

Persisted mutation tests require `--ago-allow-config-write`. They round-trip the
temperature and altitude units, measurement and GPS settings, three LED levels,
buzzer, CO2 ABC period, and TVOC/NOx learning offsets. Each parameterized case
changes one field, polls the asynchronous GET snapshot for convergence, and
restores that field during fixture teardown. They run only when
`configurationControl` is `local`, preventing cloud updates from racing with
restoration.

Extended invalid-value cases use the same opt-in and restoration fixture. If a
firmware regression accepts and persists an invalid value, teardown restores the
baseline by enqueuing the original value after the test request.

These tests can briefly change measurement cadence, GPS operation, indicator
brightness, buzzer enablement, and sensor algorithm configuration. Run them only
on a dedicated device and do not use `pytest-xdist`.

CO2 calibration is physical and fire-and-forget over HTTP. It runs only with
`--ago-allow-calibration`; the HTTP response confirms queue admission, not
dispatch to the sensor, calibration start, or completion. Do not run it without
appropriate calibration conditions.

The suite rejects parallel `pytest-xdist` execution because configuration tests
assume exclusive access to one device.

## Test Overview

### `test_discovery.py` — mDNS Profile

When mDNS selects the endpoint, verifies the `_airgradient._tcp.local.` service,
Go model, API version, hostname, TXT identity, port, and agreement with
`GET /api/v1/measures`. Discovery assertions skip when `--ago-url` selects the
endpoint directly.

### `test_measures.py` — Measures Snapshot

Validates required identity fields, optional measurement fields, numeric ranges,
unknown-key rejection, and omission of invalid values rather than JSON `null`.

### `test_config.py` — Configuration

Validates the complete Go configuration schema and safe empty-update admission.
Opt-in parameterized cases round-trip and restore the display-unit, timing, GPS,
LED, buzzer, CO2 ABC, and gas-learning settings covered by the suite.

### `test_errors.py` — Error Contract

Exercises empty, malformed, non-object, and trailing request bodies; unknown and
invalid fields; config enum, type, and range rejection; nested dotted error
paths; and known fields unsupported by Go.

### `test_actions.py` — Actions

Verifies that `test-leds` and `test-gps` return empty `200` fire-and-forget
responses. The opt-in calibration test verifies the same response contract for
`calibrate-co2`.

### `test_ota.py` — OTA Policy

These tests require an operator or dedicated test build to stage and hold a
committed Stationary Wi-Fi OTA. The suite does not start OTA itself. Run only
this file while OTA is active:

```sh
pytest products/go/tests/local-server-integration/test_ota.py -v \
  --ago-serial aabbccddeeff --ago-ota-active
```

They verify cached GET availability, read-only PUT/action policy, and
malformed-body precedence for an endpoint the operator asserts is in committed
OTA. The file always performs a fresh mDNS discoverability check during that
state, even when `--ago-url` selected the primary endpoint; it does not observe
retention across the transition into OTA. A successful OTA may reboot before
the suite completes, so use an abortable or intentionally failing OTA for this
check. Add `--ago-allow-calibration` only when explicitly testing the
calibration route's OTA policy; this protects against accidental calibration if
OTA ends before that request arrives.

## File Structure

```text
products/go/tests/local-server-integration/
  pyproject.toml       Dependencies and pytest configuration
  conftest.py          Discovery, HTTP, opt-in, and restoration fixtures
  ago_local_api.py     Routes, schema validators, and response assertions
  test_discovery.py    mDNS and identity tests
  test_measures.py     Measures schema tests
  test_config.py       Config read, no-op, and round-trip tests
  test_errors.py       Parsing and provider errors
  test_actions.py      Fire-and-forget action tests
  test_ota.py          Operator-gated OTA policy tests
```

## Design Notes

- **One device per run:** mDNS discovery fails on ambiguity unless
  `--ago-serial` or `--ago-url` selects the target.
- **Synchronous requests:** the firmware owns one HTTP task, so tests avoid
  unnecessary client concurrency.
- **Asynchronous configuration:** `202` means admitted. Mutating tests poll GET
  for convergence before proceeding.
- **Explicit interaction:** physical calibration and OTA tests are skipped
  unless their opt-in flags are present.
- **No action completion over HTTP:** BLE and UI report calibration completion;
  the Local Server API remains fire-and-forget.

## Coverage Limits

The suite currently has collection evidence only; no physical AirGradient Go run
has been recorded. The native host suite passed
`1240/1240` tests and the Go firmware build passed at revision `c17f2d3`; those
checks do not replace hardware execution.

No physical-device integration-suite evidence is available yet for concurrent
PUT behavior, provisioning listener handoff, transient reconnect, the mDNS
lifecycle across disconnect/reconnect, Home Assistant interoperability, or heap
headroom during listener, local GET, cloud TLS, and OTA activity. Host tests
cover policy and lifecycle logic, but not on-device behavior. The existing mDNS
and OTA test cases describe intended hardware checks, but collection alone does
not prove those paths on a device.

The suite also does not prove deterministic four-entry FIFO saturation or
hardware `503` pressure, HTTP responsiveness during calibration or TLS work,
correction numerical accuracy, persistence across reboot, physical calibration
completion, or compatibility with `python-airgradient`. Configuration mutation
and calibration remain explicit opt-ins, and OTA tests trust the operator's
`--ago-ota-active` assertion rather than detecting or starting OTA themselves.
