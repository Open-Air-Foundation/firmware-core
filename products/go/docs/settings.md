# Settings Service

Persistent device configuration for AirGradient Go, stored in NVS via
`ConfigStore`. `GoSettings` owns durable values and validation; the orchestrator
owns candidate activation and applying changes to running services.

## Files

| File | Purpose |
|---|---|
| [`products/go/main/go_settings.h`](../main/go_settings.h) | `GoSettings` struct and load/save declarations |
| [`products/go/main/go_settings.cpp`](../main/go_settings.cpp) | NVS key constants, validation functions, load/save implementations |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `ConfigStore` | `airgradient-config` (`hal/config_store.h`) | Typed key-value persistence interface |
| `NvsConfigStore` | `airgradient-config` (`backends/nvs_config_store.h`) | ESP-IDF NVS-backed implementation injected at construction |
| `Metric`, `OperatingMode`, `GpsMode` | product (`go_types.h`) | Enums serialized as int and reconstructed on load |
| `ConfigurationControl`, `GoConfigSource` | product (`go_config_types.h`) | Persisted remote-writer authority and shared source gate |
| `WifiStaticIpConfig` | `airgradient-wifi` (`types/wifi_types.h`) | Five-uint32 static-IP record persisted alongside the other settings |

## Public API

| Function | Returns | Purpose |
|---|---|---|
| `load_go_settings(store)` | `GoSettings` | Read all keys from NVS, fall back to defaults for missing or invalid values. Never fails. |
| `GoSettings::active_measure_interval_seconds()` | `int` | Select the saved interval for the current operating mode. |
| `save_go_settings(store, settings)` | `bool` | Validate every field, then write all keys and commit. Returns `false` if validation fails or any write/commit fails. Multi-key persistence is not transactional. |

See [`go_settings.h`](../main/go_settings.h) for full signatures.

## GoSettings Fields

### Measurement and Sensor Preferences

| Field | NVS Key | Type | Default | Valid Range | Notes |
|---|---|---|---|---|---|
| `measure_interval_seconds` | `"mi"` | `int` | `10` | 1 .. 3600 | Shared cadence for Portable and Stationary |
| `energy_saving_measure_interval_seconds` | `"es_mi"` | `int` | `300` | 300 .. 3600 | Separate Energy Saving cadence |
| `co2_abc_days` | `"cad"` | `int` | `7` | -1 or 1 .. 200 | Automatic background calibration period. `-1` disables it; positive values are applied as 24 hours per day to supported S12 and SCD41 CO2 sensors. |
| `tvoc_learning_offset` | `"tlo"` | `int` | `12` | 1 .. 1000 | Sensirion VOC gas-index learning-time offset in whole hours. |
| `nox_learning_offset` | `"nlo"` | `int` | `12` | 1 .. 1000 | Sensirion NOx gas-index learning-time offset in whole hours. |

### Display and Feedback

| Field | NVS Key | Type | Default | Valid Range | Notes |
|---|---|---|---|---|---|
| `use_fahrenheit` | `"uf"` | `bool` | `false` | — | Temperature display unit (false=C, true=F) |
| `use_feet` | `"auf"` | `bool` | `false` | — | Altitude display unit (false=m, true=ft); stored altitude remains meters |
| `pm_use_usaqi` | `"pmu"` | `bool` | `false` | — | PM display format (false=µg/m³, true=USAQI) |
| `pinned_chart` | `"pchart"` | `int` (stored) / `Metric` (in struct) | `Pm25` (1) | 0 .. 8 | Pinned Chart choice shared by UI, BLE, and Local Server; None (0) disables the automatic chart |
| `back_led_indicator` | `"bli"` | `int` (stored) / `BackLedIndicator` (in struct) | `Aqi` (0) | 0 .. 1 | Back LED indicator: AQI / CO2 |
| `back_led_brightness` | `"blb"` | `int` (stored) / `LedBrightness` (in struct) | `Off` (0) | 0 .. 3 | Back LED brightness: Off / Dim / Mid / Bright |
| `touch_led_intensity` | `"tlb"` | `int` (stored) / `TouchLedIntensity` (in struct) | `Off` (0) | 0 .. 2 | Touch feedback LED intensity: Off / Dim / Bright |
| `buzzer_enabled` | `"bv"` | `bool` | `false` | — | Enable ordinary audible feedback; diagnostics can temporarily override it |

### Operating and Connectivity Preferences

| Field | NVS Key | Type | Default | Valid Range | Notes |
|---|---|---|---|---|---|
| `gps_mode` | `"gpm"` | `int` (stored) / `GpsMode` (in struct) | `OnWhenTracking` (1) | 0 .. 2 | GPS operating mode: AlwaysOff / OnWhenTracking / AlwaysOn |
| `operating_mode` | `"opm"` | `int` (stored) / `OperatingMode` (in struct) | `Portable` (0) | 0 .. 2 | Serialized as int; cast to `OperatingMode` on load |
| `auto_lock_seconds` | `"als"` | `int` | `10` | 0, 10, 30, 60 | Runtime auto-lock timeout; `0` = disabled |
| `disable_cloud` | `"dc"` | `bool` | `false` | — | Outbound cloud transport kill switch. Suppresses POST, FETCH, and Stationary OTA checks; does not disable the local API. |
| `local_server_enabled` | `"lsrv"` | `bool` | `true` | — | Local API and discovery; editable through Operations in Stationary only. |
| `configuration_control` | `"cc"` | `int` (stored) / `ConfigurationControl` (in struct) | `Both` (2) | 0 .. 2 | Remote configuration authority: `Cloud`, `Local`, or `Both`. Does not control measurement POST. |
| `onboarding_done` | `"obd"` | `bool` | `false` | — | First-boot guide latch. `false` shows the one-time Getting Started screen after the boot splash; flips `true` on first real engagement (Menu hold on the guide, BLE pair/bond, or any operating-mode change). Cleared by factory reset. |

### Static IP Record

| Field | NVS Key | Type | Default | Valid Range | Notes |
|---|---|---|---|---|---|
| `static_ip.ip` | `"sip"` | `uint32_t` (stored as `int`) | `0` (DHCP) | — | Static-IP address (network byte order). Zero means DHCP and skips the other static-IP fields on load. |
| `static_ip.netmask` | `"snm"` | `uint32_t` (stored as `int`) | `0` | — | Loaded only when `static_ip.ip != 0`. |
| `static_ip.gateway` | `"sgw"` | `uint32_t` (stored as `int`) | `0` | — | Loaded only when `static_ip.ip != 0`. |
| `static_ip.dns_primary` | `"sd1"` | `uint32_t` (stored as `int`) | `0` | — | Loaded only when `static_ip.ip != 0`. |
| `static_ip.dns_secondary` | `"sd2"` | `uint32_t` (stored as `int`) | `0` | — | Loaded only when `static_ip.ip != 0`. |

Missing or invalid `bli` defaults to AQI. Indicator and brightness persist
independently.

Touch LED intensity defaults to Off. On V2, its UI control is hidden, BLE and
local-server writes are rejected, and cloud updates ignore the field. The NVS
key and read fields remain for compatibility; valid values saved by older
firmware are still loaded, with no automatic reset to Off.

### Measurement Interval Presentation

Portable and Stationary share `measure_interval_seconds`; Energy Saving uses
`energy_saving_measure_interval_seconds`. Switching modes selects the saved
preference without overwriting the other. Both survive restart. A missing or
invalid `es_mi` loads as 300 seconds, preserving the existing `mi` value.

The same Settings > Operations > Measurement Interval menu edits the active
preference. Portable and Stationary offer 3s, 10s, 30s, 60s, 5m, 15m, and 1h;
Energy Saving offers 5m, 15m, and 1h. Valid non-fixed values retain their exact
seconds and appear as `Custom (<seconds>s)`; the menu has no custom-value input.
Unrelated settings preserve both intervals; choosing a fixed value replaces
only the active preference.

### Pinned Chart

Persisted identifiers are None (0), PM2.5 (1), CO2 (2), Temperature (3), Humidity
(4), TVOC (5), NOx (6), Pressure (7), and Altitude (8). Missing or invalid values
load as PM2.5; invalid choices are rejected on save. The UI exposes this setting
as the first setting under Display & Touch. BLE uses unsigned `pchart` IDs;
the Local Server uses `pinnedChart` names. See the [BLE protocol](../go_ble_client.md#72-write-set-config)
and [local configuration](local_server.md#configuration).

The preference survives interval changes, even when Temperature or Humidity
cannot be shown below 300 seconds. See [Pinned Charts](ui_manager.md#pinned-charts).

### Measurement Corrections

`GoSettings::corrections` contains the complete configured correction set for PM2.5,
temperature, and humidity. Algorithms and coefficients are persisted as a
group per measure. Float coefficients use the shared `ConfigStore` four-byte
IEEE-754 blob accessors.

`effective_measurement_corrections(settings)` selects a copy for rendering and
derived readings. Energy Saving bypasses temperature and humidity corrections;
Portable and Stationary apply the saved algorithms. PM2.5 correction remains
active in all modes. Mode changes never erase saved coefficients. See
[Operating Mode](measurement_corrections.md#operating-mode).

The [supported algorithms](measurement_corrections.md#supported-algorithms)
include factory PM calibration as well as custom corrections. That document
owns transform behavior and interface-specific algorithm names.

Missing or invalid algorithms fall back to `none`. A custom correction is
active only when every required coefficient is present and finite. PM2.5
`none` does not require persisted coefficients and loads with
identity parameters. Ordinary factory reset writes the default all-`none` correction
set; [serial factory reset](serial_command_service.md#lifecycle) preserves active
measurement corrections. Go ignores the retired `useEpa2021` custom-PM option and does not read or
write its former `mc_pe` NVS key.

Wi-Fi SSID and password are owned by `WifiManager`'s saved-networks store
(its own `wifi_creds` NVS namespace, injected at construction). Connection
metadata (`disable_cloud`, `configuration_control`, `static_ip`) and
`local_server_enabled` live in `GoSettings`. Wi-Fi power saving is derived from
Local Server: disabled when the server is enabled, enabled otherwise.

Upgrades without `lsrv` default Local Server to enabled to preserve local
access. The legacy `wps` key is ignored.

## Load Behavior

`load_go_settings(ConfigStore &store)` reads each NVS key in sequence. For
every field:

1. Call the appropriate `store.get_*()` for the key.
2. If the read succeeds **and** the value passes field-level validation: use the
   stored value.
3. If the read fails (key absent) or validation rejects the value: keep the
   struct default.

The function never fails. It always returns a fully populated `GoSettings`
struct with either the stored or the default value for each field.
An absent or out-of-range `"cc"` key retains the `Both` default. Loading is
field-by-field and does not normalize the cross-field `Cloud` plus
`disable_cloud=true` combination; normal writers prevent that combination, and
full validation rejects it before a later save or activation.

## Save Behavior

`save_go_settings(ConfigStore &store, const GoSettings &settings)` validates
all fields up front before writing anything. If **any** field is out of range,
the function returns `false` immediately without touching NVS.

On success, all fields are written with `store.set_*()`, and `store.commit()`
is called. The function returns `true` only when all writes and the commit
succeed. It rejects `configuration_control=Cloud` with `disable_cloud=true`
before the first NVS write.

The multi-key operation has no rollback. A successful write that precedes a
later write or commit failure can remain in the store. Runtime callers avoid a
partially activated in-memory state by replacing the active candidate only
after `save_go_settings()` returns `true`.

## Validation Rules

`is_go_settings_valid()` checks the complete settings object. Shared field
bounds and validators live in `go_config_types.h`; local checks stay in
`go_settings.cpp`.

Scalar bounds are listed once in [GoSettings Fields](#gosettings-fields).
Additional rules and boundaries are:

- `pinned_chart` must be one of the supported metric identifiers.
- `configuration_control=Cloud` cannot be combined with `disable_cloud=true`.
- Booleans need no numeric range check. Static IP fields have no semantic
  validation here; a zero address selects DHCP on load.
- Correction validators check supported algorithm enums. Wire adapters and
  persisted loading check finite coefficients; the shared correction validator
  itself does not establish coefficient finiteness. Callers constructing
  settings directly must preserve that precondition.

## Stationary Networking Fields

`disable_cloud`, `configuration_control`, and `static_ip` are durable Stationary
settings. On every successful provisioning connection, the orchestrator
attempts to persist the payload's `disable_cloud` and `static_ip` values before
teardown or attached verification. A successful DHCP candidate zeros
`static_ip`, clearing previously stored static-IP fields. If persistence fails,
the previous active metadata remains in use, but Stationary provisioning
teardown or Portable attached verification continues.

When provisioning disables cloud while the active authority is `Cloud`, the
orchestrator changes authority to `Local` in the same candidate. This preserves
the invariant that a disabled cloud transport cannot be the only configuration
writer. Factory reset restores `disable_cloud=false`,
`configuration_control=Both`, and DHCP.

## Configuration Authority

`disable_cloud` and `configuration_control` are independent:

- `disable_cloud` controls outbound network work. When true, `CloudService`
  suppresses subsequent measurement POST and config FETCH attempts, Stationary
  OTA checks do not run, and the local API remains available. It does not cancel
  an in-flight cloud request. The local wire field `cloudConnection` is its
  inverse.
- `configuration_control` controls only the two remote configuration writers.
  `Local` disables cloud FETCH without affecting measurement POST. `Cloud` and
  `Both` enable FETCH.

| Active Control | Cloud Fetch | Local Server | BLE, UI, Provisioning, Factory, System |
|---|---|---|---|
| `Cloud` | Allowed | Forbidden, except an exact control-only recovery to `Local` or `Both` | Allowed |
| `Local` | Forbidden | Allowed | Allowed |
| `Both` | Allowed | Allowed | Allowed |

The local server checks authority before queueing a request, and the
orchestrator checks it again before applying the queued update. Cloud results
receive the same consumption-time check, so an in-flight FETCH result is
discarded after authority changes to `Local`. All accepted candidates still
pass full `GoSettings` validation and NVS commit before activation.

Factory reset writes default settings (`disable_cloud=false`,
`configuration_control=Both`, DHCP) and additionally calls
`WifiService::clear_credentials()` to erase all saved networks.

## First-Boot Onboarding Field

`onboarding_done` is the durable latch for the one-time Getting Started
guide. It defaults to `false`, so a fresh unbox shows the guide once when
orchestrator initialization finishes, while sensors warm up. The orchestrator
flips it to `true` through the idempotent `mark_onboarding_done()` helper on
the first real engagement:

- a Menu hold on `Screen::GettingStarted` (boot-gate entry),
- a successful (encrypted) BLE pairing/bond (`on_ble_auth_complete(true)`);
  a failed pair leaves the flag untouched, or
- any operating-mode change (`change_mode()`).

The helper guards redundant NVS commits — once the flag is set, further
calls are no-ops. Because the guide gate is evaluated on the `Interactive`
(`PowerOn`) boot path, which always reloads `GoSettings` from NVS, the flag
does not need an `RtcAppState` mirror. Factory reset clears it (default
`false`) so refurbished / returned units re-show the guide.

## Legacy Tracking Intent

The legacy `trk_start` NVS boolean and its accessors remain for compatibility.
Boot no longer consumes it or creates an Energy Saving route; factory reset
clears it. Operating mode is controlled by the existing `GoSettings` setting.

## Factory Settings

`FactorySettings` is **production-level** state, distinct from user `GoSettings`.
It holds the fuel-gauge learning run state (`fg_learning_stage`,
`fg_learning_cycle`, `fg_learning_itpor_losses`, and `fg_learning_fail_reason`)
and lives under distinct keys (`fs_s` / `fs_c` / `fs_i` / `fs_r`) in the **same `"go"` NVS namespace**. Because
`save_go_settings()` only rewrites the keys it enumerates, these keys are never
touched by `factory_reset()` and therefore **survive it** — a finished
(`Complete` / `Failed`) unit cannot accidentally look normal after a user reset.
Clearing the run state is an explicit `clear_factory_settings()` call, invoked
only by the learning exit gesture (a POWER press).

| Function | Purpose |
|---|---|
| `is_factory_learning_stage_active(stage)` | Boot predicate (true for every stage except `Idle`; both terminals are sticky) |
| `load_factory_settings()` / `save_factory_settings()` | Load all factory fields; save stage, cycle, and ITPOR-loss count only |
| `save_fg_learning_state()` | Write stage, cycle, and ITPOR-loss count, then commit once; no multi-key rollback |
| `save_fg_learning_fail_reason()` | Persist and commit the failure reason separately |
| `clear_factory_settings()` | Explicit clear — **not** called by `factory_reset()` |

See [`fg_learning.md`](fg_learning.md) for the factory learning boot path.

## Relationship to RtcAppState

Some settings fields have a corresponding field or flag in `RtcAppState` (the
RTC-memory cache used to survive deep sleep):

| Setting | RTC Field or Flag | Purpose |
|---|---|---|
| `operating_mode` | `mode` | Settings = durable (NVS, survives power-off). RtcAppState = fast cache (RTC memory, survives deep sleep only). |
| `gps_mode` | `RtcAppFlag::GpsEnabled` | Set in `RtcAppState::flags` when the setting is not AlwaysOff. |

The GPS flag is accessed through `has_flag()` and `set_flag()`. See
[RTC State Persistence](power_management.md#rtc-state-persistence) for the
flag layout and defaults.

**Startup logic:**

- **Fresh power-on:** Load from `GoSettings` (NVS), then initialize
  `RtcAppState` from those values.
- **Deep sleep timer wake:** Restore session state and the GPS wake deadline
  from `RtcAppState`; load settings to apply the current GPS mode.
- **Mode or GPS toggle:** Write both `GoSettings` (NVS) and `RtcAppState`.

## Usage and Activation

Use the board's existing `ConfigStore` in the `"go"` namespace; see
[`go_hardware_board.cpp`](../main/go_hardware_board.cpp). Runtime changes should
use the orchestrator's candidate activation path so successful persistence also
updates the affected services. A direct `save_go_settings()` call only writes
storage; it does not activate runtime policy.

## Verification

[`go_settings.tests.cpp`](../tests/go_settings.tests.cpp) covers defaults,
round-trips, invalid fields, grouped corrections, configuration authority,
write failures, and factory-key isolation. Runtime activation and failure
handling are covered in
[`go_orchestrator.tests.cpp`](../tests/go_orchestrator.tests.cpp).
See the [host-test workflow](../../../tests/README.md). Fake-store tests do not
establish NVS power-loss atomicity or hardware sensor-setting persistence.

## Build-Time Options

BLE link security is not part of `GoSettings`. It is always enabled: the custom
GATT service mandates pairing (Passkey Entry, bonding, MITM) and has no
build-time toggle.
