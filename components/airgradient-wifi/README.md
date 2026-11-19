# airgradient-wifi

Shared Wi-Fi foundation for AirGradient products: a `WifiManager` service
that owns the radio, plus a thin `WifiHal` so product code never depends
directly on the ESP-IDF Wi-Fi stack.

## Status

`Stable`.

## Scope

This component owns:

- Wi-Fi mode control (Off / STA / AP / APSTA)
- STA connection lifecycle with hybrid auto-retry and exponential backoff
- A self-managed store of up to `WIFI_MAX_SAVED_NETWORKS` (3) saved
  networks in a dedicated NVS namespace, via an injected `ConfigStore`
- Auto-connect (empty SSID): scan, intersect visible APs with saved
  networks, rank by RSSI, and fail over single-attempt across candidates
- Explicit-SSID transient connect (never writes the store)
- Async Wi-Fi scan (only valid while STA is disconnected)
- Soft-AP control with caller-provided SSID / password
- Retained mDNS profiles with automatic STA-IP or explicit manual lifecycle
- Static IP configuration as an alternative to DHCP
- Disconnect-reason normalisation from raw ESP-IDF codes
- Power-save mode selection (`None` / `MinModem` / `MaxModem`)

This component does not own:

- Provisioning UX / transport (BLE provisioning, SmartConfig, …)
- Sockets, HTTP, MQTT, or any application-level protocol
- Channel-biased scanning, regulatory / country configuration
- Dynamic mDNS service add / remove after initial configuration
- AP SSID auto-generation from MAC

## Directory Layout

```text
components/airgradient-wifi/
  hal/
  types/
  services/
  drivers/
  tests/
  CMakeLists.txt
  Kconfig
  idf_component.yml
  README.md
```

- `hal/` — abstract `WifiHal` interface (DI seam, mocked in host tests)
- `types/` — public enums, structs, callbacks, sentinels
- `services/` — `WifiManager` (pure C++, host-testable)
- `drivers/` — `EspWifiHal` (ESP-IDF backed implementation)
- `tests/` — host-side tests owned by this component

## Public Includes

```cpp
#include "hal/wifi_hal.h"
#include "types/wifi_types.h"
#include "services/wifi_manager.h"
#include "drivers/esp_wifi_hal.h"
```

Guideline:

- include from `types/` for shared enums, structs, callbacks
- include from `hal/` only when implementing or mocking the HAL
- include from `services/` for the product-facing API (`WifiManager`)
- include from `drivers/` only when instantiating `EspWifiHal`

## Design

```mermaid
flowchart LR
    Product[Product code] --> Mgr[WifiManager<br/>pure C++]
    Mgr --> Hal[WifiHal<br/>abstract]
    Hal --> Esp[EspWifiHal<br/>ESP-IDF driver]
    Esp --> Stack[ESP-IDF Wi-Fi / netif / mDNS]
```

`WifiManager` owns: mode state machine, connection retry with exponential
backoff, retained mDNS profile lifecycle, disconnect-reason normalisation,
DHCP acquisition timeout. The HAL stays thin — translate ESP-IDF events
into typed callbacks, drive timers on behalf of the manager, and forward
mode / scan / connect calls. Driver-side code never makes scheduling
decisions.

## Usage

```cpp
EspWifiHal hw;
NvsConfigStore creds(WIFI_CREDS_NVS_NAMESPACE);
WifiManager wifi(hw, creds);

wifi.set_on_connected([] { /* L2 link up */ });
wifi.set_on_got_ip([](uint32_t ip) { /* full connectivity */ });
wifi.set_on_disconnected([](WifiDisconnectReason r) {
    // long-term recovery: sleep, switch to AP, reprovisioning, ...
});

wifi.set_mode(WifiMode::Sta);
WifiStaConfig cfg;
std::strncpy(cfg.ssid, "MyNetwork", sizeof(cfg.ssid) - 1);
std::strncpy(cfg.password, "secret", sizeof(cfg.password) - 1);
wifi.connect(cfg);
```

### Saved Networks And Auto-Connect

`WifiManager` owns a self-managed credential store (up to
`WIFI_MAX_SAVED_NETWORKS` = 3 SSID/password pairs) persisted through a
required injected `ConfigStore`: `WifiManager(hal, store)`. The store's NVS
namespace is component-owned — wire the backend on `WIFI_CREDS_NVS_NAMESPACE`
(`wifi_creds`); products do not choose the value.

Credential API (newest-first ordering; the oldest entry is evicted on
overflow; re-adding an SSID refreshes its password and marks it newest):

- `add_network(ssid, password)` — validate, insert at the front
- `remove_network(ssid)` — drop by SSID
- `list_networks(out, max)` — copy SSIDs newest-first, returns the count
- `has_saved_networks()` — true when at least one network is saved
- `clear_networks()` — erase all (factory reset / reprovision)

`connect()` resolves the SSID:

- **Explicit `config.ssid`** — one-shot transient connect using the normal
  retry / backoff path. Never writes the store.
- **Empty `config.ssid` (auto-connect)** —
  - `0` saved → `NotFound` (no driver call).
  - `1` saved → connect it directly with the caller's retry / backoff (no
    scan, no single-attempt sweep).
  - `>1` saved → start an internal scan, intersect visible SSIDs with the
    saved list, rank by RSSI (tie-break newest-first), then sweep
    candidates with a **single attempt each**, failing over on failure.
    On the first got-IP the sweep ends and the winning AP is latched into
    `_sta_config` so a later link drop reconnects under normal retry /
    backoff. A `start_scan()` failure returns `Failed`.

A retriable disconnect during a non-sweep connect (explicit SSID or the
single-saved-network path) arms the caller's retry / backoff. `no_ap_found`
is treated as **retriable**: a single connect-time channel sweep can miss
a present AP's probe response, so spending the retry budget avoids failing
terminally on one unlucky sweep. `auth_failed`, `assoc_failed`, and
`dhcp_failed` stay non-retriable (the caller decides: reprovision, fall
back, …). The single-attempt sweep across multiple candidates is
unaffected — it never retries a candidate regardless of reason.

A `connect()` issued while a connect or auto cycle is already running
returns `AlreadyInProgress`. While an auto cycle owns the radio a
product-initiated `start_scan()` returns `InvalidState`, and internal
scan results are consumed (not forwarded to `on_scan_complete`).

Hidden APs report an empty SSID in scan results and can never be matched
by the SSID-intersect: a single saved hidden network still connects
(scan skipped), but with more than one saved network a hidden one is
never an auto-connect candidate.

ESP-IDF never persists STA credentials to its own Wi-Fi NVS:
`EspWifiHal::init()` forces `WIFI_STORAGE_RAM`.

### mDNS Profiles

`set_mdns_profile()` validates and retains one profile. The manager copies
the hostname and service records; service-type strings, TXT pointer arrays,
and TXT strings remain caller-owned and must outlive the retained profile. A
rejected profile leaves the previous profile unchanged.

`WifiMdnsLifecycle::StaIpAuto` starts the profile when STA gets an IP and stops
it when STA disconnects. The profile remains retained for the next got-IP.
`WifiMdnsLifecycle::Manual` only changes through `start_mdns()`, `stop_mdns()`,
and `clear_mdns_profile()`; STA connect and disconnect events do not affect it.
Both lifecycles stop when Wi-Fi mode becomes `Off`.

Explicit start and stop are idempotent. A failed start remains retryable, and a
failed stop keeps the profile marked running so the stop can be retried.
`clear_mdns_profile()` stops a running profile before forgetting it and retains
the profile if that stop fails. `set_mdns_config()` remains as a compatibility
wrapper that installs a `StaIpAuto` profile.

## Configuration

The component exposes one Kconfig knob under **AirGradient Wi-Fi** in
`menuconfig` (see `components/airgradient-wifi/Kconfig`):

| Symbol | Default | Purpose |
|---|---|---|
| `CONFIG_AG_WIFI_DHCP_TIMEOUT_MS` | `15000` | DHCP acquisition timeout after L2 association. Bypassed when a static IP is configured. |

## Dependencies

- `components/airgradient-common/` — shared types and RTOS abstraction
- `components/airgradient-config/` — `ConfigStore` interface backing the
  saved-networks credential store
- `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash`, `lwip` — private,
  only consumed by the ESP-IDF driver
- `espressif/mdns` — managed component declared in `idf_component.yml`;
  IDF 5.x moved mDNS out of the core tree

## Tests

Host tests live in `components/airgradient-wifi/tests/` and run through
the top-level [tests runner](../../tests/README.md). They cover:

- mode state-machine transitions and idempotency
- connect / disconnect / retry backoff (including `no_ap_found` retriable)
- credential store helper (add, dedup-and-refresh-to-newest, evict-oldest,
  list newest-first, clear, load resilience / self-heal, validation,
  commit-failure surfacing)
- auto-connect with mocked scan results: `0` / `1` / `>1` saved branches,
  per-SSID RSSI dedup, best-RSSI selection, deterministic tie-break,
  single-attempt failover, exhaustion, scan-failure, DHCP-timeout-in-sweep
- disconnect-reason mapping (raw ESP-IDF code → `WifiDisconnectReason`)
- mDNS profile validation, explicit start / stop / clear, automatic reconnect,
  manual disconnect survival, and mode-Off teardown
- DHCP timeout policy (treated as `dhcp_failed`, non-retriable)
- mode enforcement on `connect` / `start_scan` / `start_ap`
- scan-only-while-disconnected enforcement
- `set_mode` leaving STA emits `requested_by_user` and swallows the
  driver echo (no phantom retry in non-STA mode)
- `status_snapshot` zeroes STA-only fields when disconnected

Hardware-only behaviour (real STA association, AP DHCP, mDNS resolution,
static IP application, power-save effects, NVS clear) is verified
manually on a real ESP32; see `products/reference/main/test_wifi.cpp`
for the runtime smoke test.

## Notes

ESP-IDF does not expose a direct DHCP-failure event — only
`IP_EVENT_STA_GOT_IP` and `IP_EVENT_STA_LOST_IP`. The HAL therefore
exposes two single-shot timers — `arm_dhcp_timeout` /
`cancel_dhcp_timeout` for the DHCP watchdog, and `arm_retry_timer` /
`cancel_retry_timer` for the connect-retry backoff — so the manager
can keep all timing decisions in pure C++. The driver backs both with
`esp_timer`.

### Active-scan dwell

`EspWifiHal::start_scan` requests a 60 ms per-channel active dwell
(`scan_time.active.max = 60`). The intent is to keep a full 42-channel
AUTO-band scan inside the ~10 s PMF SA-Query tolerance of any client
associated to a co-resident SoftAP, so the captive-portal provisioning
flow does not lose its legitimate client to a SA-Query disassoc while
the scan is in flight.

When BLE is enabled on the same chip, ESP-IDF silently overrides the
requested dwell back to BT-coex-safe defaults (~240 ms/channel) and
logs `"Should use default active scan time parameter for WiFi scan
when Bluetooth is enabled"` at warning level. The 60 ms request is
still honoured for `WifiOnly` provisioning flows and for any path
where BLE has already been deinit'd before the scan starts (e.g. the
`airgradient-provisioning` "first AP client commits" teardown). No
public-API change; `WifiScanConfig` is unchanged.

### PMF on SoftAP is not configurable

`EspWifiHal::start_ap` does not attempt to disable PMF on the
soft-AP. ESP-IDF documents `pmf_cfg.capable` as deprecated and always
forces PMF when the peer advertises support; setting `capable=false`
is silently dropped. Mitigation for the resulting SA-Query disassoc
under BT-coex lives entirely in `airgradient-provisioning` (transport
selector, first-client-wins teardown, shortened scan dwell above).

### Band mode is not pinned

ESP-IDF persists `band_mode` to NVS via `WIFI_STORAGE_FLASH`. A
one-time `esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY)` call
survives reboots, which can silently mask regressions during
diagnosis. Products that want a specific band policy must call
`esp_wifi_set_band_mode()` explicitly on every boot; otherwise a
fresh-NVS unit falls back to AUTO. This driver intentionally does
not pin the band — leaving the radio on AUTO preserves 5 GHz scan
visibility, and product-side mitigation (e.g. transport-aware
teardown in `airgradient-provisioning`) handles the SoftAP / scan
interaction without needing to disable a whole band.
