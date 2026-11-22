# airgradient-ota

Transport-agnostic Over-The-Air firmware update component. It separates the
universal flash-write core (`OtaImageWriter`) from the transport that delivers
the image bytes, and ships the WiFi pull path and the BLE push path.

## Status

`Stable`.

## Scope

This component owns:

- the universal flash-write core (`OtaImageWriter` + `EspOtaImageWriter`)
- the pull transport seam (`OtaImageSource`) shared by all HTTP transports
- the blocking pull orchestrator (`OtaUpdater`) that owns the read/write loop,
  progress throttling, and abort-on-error
- the AirGradient firmware URL builder (`ota_url`)
- the WiFi pull source (`WifiHttpOtaSource`) over `esp_http_client`
- the BLE push service (`OtaBleService`) that owns the OTA GATT flow
  (Control / Data / Status) on a borrowed `AgBleServer`, flashing each
  phone-pushed Data write directly in the write callback and driving
  begin/finish/abort from a single product-called `run()` that returns
  immediately when idle and blocks through the transfer once started
- the BLE connection-interval window: `OtaBleService` requests the fast OTA
  window when a transfer begins and restores the relaxed window on a
  non-success terminal (success reboots, so no restore is needed)
- typed results (`OtaStatus`) and struct-based progress reporting

This component does not own:

- rebooting after an update (the product decides from the returned `OtaStatus`,
  including the terminal `OtaStatus` that `OtaBleService::run()` returns)
- marking the new image valid / rollback / anti-rollback policy
- transport security (WiFi downloads are plain HTTP; the BLE link relies on the
  product-configured authenticated pairing; HTTPS and signed images are future
  work)
- background task creation on the pull path (`run()` is blocking on a
  product-provided task)
- BLE server lifecycle (`OtaBleService` borrows an init'd/secured server and
  never `init()`/`deinit()`s it or drives advertising)
- the cellular transport (seam only; future work)

## Directory Layout

```text
components/airgradient-ota/
  hal/
    ota_image_writer.h       # universal flash-write interface
    ota_image_source.h       # pull transport seam
  types/
    ota_types.h              # OtaStatus, OtaState, OtaProgress, OtaRequest
  backends/
    esp/                     # EspOtaImageWriter (esp_ota_ops)
    wifi/                    # WifiHttpOtaSource (esp_http_client streaming)
  services/
    ota_updater.{h,cpp}      # pull orchestrator (host-testable)
    ota_url.{h,cpp}          # AG-server URL builder (host-testable)
    ota_ble_service.{h,cpp}  # BLE push service: GATT flow + run (host-testable core)
    ota_ble_protocol.h       # BLE CBOR key/op constants + frozen wire constants
  tests/
  idf_component.yml          # espressif/cbor managed dependency
  CMakeLists.txt
  Kconfig
  README.md
```

- `hal/` — public interfaces every transport depends on
- `types/` — result/progress/request value types
- `backends/` — concrete drivers grouped by transport family (firmware only)
- `services/` — host-testable orchestration and URL logic
- `tests/` — host-side tests owned by this component

## Public Includes

```cpp
#include "hal/ota_image_writer.h"
#include "hal/ota_image_source.h"
#include "services/ota_updater.h"
#include "services/ota_ble_service.h"
#include "backends/esp/esp_ota_image_writer.h"
#include "backends/wifi/wifi_http_ota_source.h"
```

Guideline:

- include from `hal/` when depending on an interface
- include from `services/` when driving an update
- include from `backends/` only when instantiating a concrete driver

## Design

The `OtaImageWriter` is the single universal piece every transport terminates
at. Pull transports share the `OtaImageSource` seam and are driven by
`OtaUpdater`; the BLE push service owns its own GATT flow and feeds the
writer directly. "Is an update available?" semantics live only in the pull
path. Reboot is never performed here.

```mermaid
flowchart TB
    U[OtaUpdater] -->|OtaImageSource| WS[WifiHttpOtaSource]
    U -->|write bytes| W[(OtaImageWriter<br/>EspOtaImageWriter)]
    Phone --> BLE[OtaBleService]
    BLE -->|write bytes| W
    WS -->|stream GET| Net[esp_http_client]
    W -.->|status only| PR[Product decides reboot]
```

`OtaUpdater::run()` is a single blocking call: `open -> begin -> loop(read ->
write, throttled progress) -> finish`. It aborts the writer on any read/write
error and always closes the source. The URL builder centralises the AirGradient
URL conventions, mapping the Kconfig-selected device model to the path segment
and serial format at build time.

### Wi-Fi Availability Check

`WifiHttpOtaSource::check_available() const` performs a separate blocking GET
against the firmware URL with `&offset=0&length=0`. The endpoint returns an
empty response, and the method completes the HTTP request with
`esp_http_client_perform()` before releasing the client. It does not open an
image writer, modify the download URL, or invoke `OtaUpdater`.

| Response | Result |
|---|---|
| HTTP 200 | `OtaStatus::Ok`: an image is available; nothing has been installed |
| HTTP 304 | `OtaStatus::UpToDate` |
| HTTP 400 / 404 | `OtaStatus::Declined` |
| Other HTTP status, including redirects | `OtaStatus::ServerError` |
| Client initialization or request failure | `OtaStatus::TransportError` |
| Invalid request or URL exceeding the configured buffer after appending the query | `OtaStatus::InvalidArgument` |

Redirects are not followed, preserving the zero-length request semantics.
The check uses `CONFIG_AG_OTA_HTTP_TIMEOUT_MS` and
`CONFIG_AG_OTA_URL_BUFFER_SIZE`. Callers own scheduling and serialization with
other network work. A successful check does not reserve an image; a later
download still handles its own response normally. Existing `open()` and
`OtaUpdater::run()` behavior is unchanged, including rejecting an empty HTTP
200 response when downloading an image.

### BLE Push

`OtaBleService` registers Control, Data, and Status characteristics on a
borrowed, secured BLE server before advertising. Data writes execute on the
NimBLE host task; the product calls `run()` to prepare, finish, or abort the
transfer on its own task. The product owns sleep/coexistence policy and reboot.

See [BLE Push OTA](docs/ble_push.md) for the current protocol, threading,
failure handling, and design rationale.

## Usage

```cpp
OtaRequest req{serial, current_fw, "hw.airgradient.com"};
WifiHttpOtaSource source(req);
EspOtaImageWriter writer;
OtaUpdater updater(source, writer);
updater.set_on_progress([](const OtaProgress &p) {
  AG_LOGI("App", "ota %u%% (%u bytes)", p.percent, (unsigned)p.bytes_written);
});

OtaStatus st = updater.run(); // single blocking call
if (st == OtaStatus::Ok) {
  reboot(); // product decides
}
```

For BLE setup and lifecycle, follow [BLE Push OTA](docs/ble_push.md#ownership-and-integration)
and the [service header](services/ota_ble_service.h). In particular, an idle
`run()` returns `Ok` without performing an update; use progress events to
identify a completed transfer before deciding to reboot.

## Configuration

The component exposes Kconfig knobs under **AirGradient OTA** in `menuconfig`
(see `components/airgradient-ota/Kconfig`):

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_AG_DEVICE_MODEL_ONE_OPEN_AIR` | `y` | Use the One / Open Air firmware URL shape |
| `CONFIG_AG_DEVICE_MODEL_MAX` | `n` | Use the Max firmware URL shape |
| `CONFIG_AG_DEVICE_MODEL_GO` | `n` | Use the Go firmware URL shape |
| `CONFIG_AG_OTA_HTTP_TIMEOUT_MS` | `15000` | HTTP connect/read timeout |
| `CONFIG_AG_OTA_READ_BUFFER_SIZE` | `1024` | Per-read download/flash buffer |
| `CONFIG_AG_OTA_PROGRESS_INTERVAL_MS` | `1000` | Minimum gap between progress callbacks |
| `CONFIG_AG_OTA_URL_BUFFER_SIZE` | `256` | Max built firmware URL length |
| `CONFIG_AG_OTA_BLE_DATA_MAX_BYTES` | `512` | Max accepted single BLE Data write; larger writes are rejected |
| `CONFIG_AG_OTA_BLE_CONTROL_MAX_BYTES` | `64` | Max accepted BLE Control write size |
| `CONFIG_AG_OTA_BLE_FW_MAX_LEN` | `32` | Max BLE `fw` string length |
| `CONFIG_AG_OTA_BLE_STALL_TIMEOUT_MS` | `10000` | BLE silent-phone byte-progress watchdog window |
| `CONFIG_AG_OTA_BLE_PROGRESS_INTERVAL_MS` | `5000` | BLE `run()` tick: progress log + progress NOTIFY cadence, stall granularity |

The device-model symbols are mutually exclusive members of the
shared `AG_DEVICE_MODEL` choice from `airgradient-common`. `OtaRequest` contains
only per-request values; callers cannot change the model at runtime.

The OTA connection-interval window is owned by `OtaBleService`: it requests the
fast window (15–30 ms, latency 0, 2 s supervision) on `begin()` and restores the
relaxed window (30–50 ms) on a non-success terminal. These are fixed constants
in `ota_ble_service.cpp`, not Kconfig. The preferred MTU (512) remains a
product / BLE-stack concern (`CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU`).

## Dependencies

- `components/airgradient-common/` — `RTOS` timing and `AG_LOG` macros
- `app_update` — `esp_ota_ops` flash API
- `esp_http_client` — WiFi streaming download
- `components/airgradient-ble/` — `AgBleServer` / characteristic HAL (BLE push)
- `espressif/cbor` (TinyCBOR) — BLE Control/Status CBOR encode/decode
  (managed component via `idf_component.yml`)

## Tests

Host tests live in `components/airgradient-ota/tests/` and run through the
top-level [tests runner](../../tests/README.md). They cover the `ota_url`
mapping for the default One / Open Air model and the `OtaUpdater` flow
(ordering, skip/abort/truncation paths,
byte accounting, progress state sequence, and callback throttling) against a
Trompeloeil mock source and a host fake writer, plus the `OtaBleService`
protocol/state core (CBOR Control decode + bounds, wire constants, the
state machine, begin/write/finish sequencing, byte-count/framing rules,
NOTIFY-only Status emission, rejection rules, the progress callback, the
connection-interval bracketing (fast request on `begin()`, relaxed restore only
on a non-success terminal), and the `run()` / `is_active` lifecycle) against a
mock `AgBleServer` and the host fake writer.
`EspOtaImageWriter` and `WifiHttpOtaSource`, including the availability check,
are verified by HIL rather than host tests; so are the blocking BLE `run()`
loop and the live silent-phone stall watchdog.

## Notes

- WiFi downloads use plain HTTP and provide no transport authentication; the
  BLE push path relies on the product-configured authenticated pairing
  (`WRITE_AUTHEN` + `BOND | MITM | SC`). HTTPS and signed images are explicit
  future improvements.
- The cellular pull source remains a seam but is not implemented here.
