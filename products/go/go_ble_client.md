# AirGradient Go — BLE Client Integration Spec

Client-side specification for mobile app developers integrating with the
AirGradient Go (AGo) BLE peripheral. Covers discovery, pairing, GATT
operations, payload decoding, and the history download protocol.

---

## Table of Contents

1. [Discovery and Connection](#1-discovery-and-connection)
2. [Pairing and Security](#2-pairing-and-security)
3. [GATT Service Overview](#3-gatt-service-overview)
4. [Serialization](#4-serialization)
5. [Measures Characteristic](#5-measures-characteristic)
6. [Status Characteristic](#6-status-characteristic)
7. [Config Characteristic](#7-config-characteristic)
8. [History Characteristic](#8-history-characteristic)
9. [Device Information Service](#9-device-information-service)
10. [Wi-Fi Provisioning (Portable)](#10-wi-fi-provisioning-portable)
11. [Firmware Update (OTA, Portable)](#11-firmware-update-ota-portable)
12. [MTU Considerations](#12-mtu-considerations)
13. [Error Reference](#13-error-reference)
14. [Appendix: RoutePointWire Binary Format](#appendix-routepointwire-binary-format)
15. [Appendix: CBOR Quick Reference](#appendix-cbor-quick-reference)
16. [Appendix: Invalid Sentinel Values](#appendix-invalid-sentinel-values)

---

## 1. Discovery and Connection

### Advertised Name

The device advertises as **`AirGradient Go <suffix>`** where `<suffix>` is the
last four lowercase hex chars of the device serial (the bottom two bytes of
the device's Wi-Fi MAC address).

Example: MAC `aa:bb:cc:dd:ef:0e` advertises as `AirGradient Go ef0e`.

The total advertised name is 19 characters. The suffix provides ~65 000
distinct values, which is sufficient to disambiguate multiple devices in the
same household; collisions are possible in larger fleets, so clients should
not treat the suffix as a globally unique identifier — use the BLE address
or the device's reported serial for that purpose.

### Advertised Service UUID

The 128-bit service UUID is included in the advertising payload:

```text
d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1
```

The complete local name is placed in the scan response data.

### Scanning Recommendations

- **Preferred**: filter by the service UUID above for reliable discovery.
  This is robust to future name format changes.
- **Fallback**: filter by name prefix `"AirGradient Go "` (note the trailing
  space).
- The device supports a single concurrent connection. While a client is
  connected, the device stops advertising. Advertising resumes after
  disconnect.

### Connection Parameters

The device is a BLE peripheral (Broadcaster + Peripheral roles). It does not
initiate connections.

---

## 2. Pairing and Security

### Pairing Model

The device uses **Passkey Entry** pairing with **Display Only** IO capability.
The device generates and displays a 6-digit numeric passkey (000000–999999)
on its e-paper screen. The user must enter this passkey on the phone to
complete pairing.

Security features:

- **Bonding**: Enabled. After successful pairing, the bond is stored and
  subsequent connections skip passkey entry.
- **MITM protection**: Enabled.

### Pairing Flow

```text
Phone                              Device
  |                                  |
  |------- connect ----------------->|
  |                                  |
  |------- initiate pairing -------->|
  |                                  |
  |                         (generates passkey)
  |                         (shows passkey on display)
  |                                  |
  |  (user reads passkey from device)|
  |                                  |
  |------- enter passkey ----------->|
  |                                  |
  |<------- pairing complete --------|
  |                                  |
  |  (bonded — future connections    |
  |   skip passkey entry)            |
```

### Authenticated Characteristics

When security is enabled (production builds), the following characteristics
require an authenticated (paired) connection:

| Characteristic | Read | Write | Notify / Subscribe |
|---|---|---|---|
| Measures | Authenticated | — | Authenticated |
| Status | Authenticated | — | — |
| Config | Authenticated | Authenticated | — |
| History | — | Authenticated | — |
| OTA Control / Data (firmware update) | — | Authenticated | — |
| OTA Status (firmware update) | — | — | Authenticated |
| Wi-Fi Scan (provisioning) | — | Encrypted | Encrypted |
| Credentials/Status (provisioning) | Encrypted | Encrypted | Encrypted |
| Device Information (DIS) | Encrypted | — | — |

The provisioning and DIS characteristics require an **encrypted** link rather
than an explicitly authenticated one. On the Portable link the bonded MITM
pairing above already provides encryption, so no extra step is needed (see §9
and §10).

Attempting to read or subscribe to an authenticated characteristic without
pairing will usually trigger the BLE stack's pairing flow automatically on
supported platforms.

### Development Builds

Development firmware may disable BLE security. In this case, all
characteristics are accessible without pairing. The GATT structure remains
identical.

---

## 3. GATT Service Overview

### Service

| Field | Value |
|---|---|
| UUID | `d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1` |
| Type | Primary Service |

### Characteristics

| Name | UUID | Properties | Description |
|---|---|---|---|
| Measures | `d1c0c0a1-...` | Read, Notify | Live sensor + GPS data |
| Status | `d1c0c0a2-...` | Read, Notify | Device status snapshot |
| Config | `d1c0c0a3-...` | Read, Write, Notify | Get/set config, execute commands |
| History | `d1c0c0a4-...` | Write, Notify | Stored route data export |

All UUIDs share the base `d1c0c0aX-6b48-4b2a-9b1d-59f9f2b0a1e1` where `X`
is the characteristic index (1–4).

When BLE security is enabled, the Measures characteristic requires an
authenticated connection for both read access and notification delivery.

### Additional Services (Portable mode)

In Portable mode the bonded link also exposes three more services — alongside
the AGo data service above — for Wi-Fi provisioning, firmware update, and device
identity:

| Service | UUID | Purpose |
|---|---|---|
| AirGradient Provisioning | `acbcfea8-e541-4c40-9bfd-17820f16c95c` | Wi-Fi scan + credentials + live status (§10) |
| AirGradient OTA | `ab9a0001-1e3c-4f5a-9b6d-0a1b2c3d4e5f` | Firmware update push (§11) |
| Device Information (DIS) | `0x180A` | Model / Serial / Firmware / Manufacturer (§9) |

Neither UUID is added to the advertising payload (the client is already
connected on the bonded link); discover them via GATT service discovery after
connecting.

---

## 4. Serialization

All characteristic payloads use **CBOR** (RFC 8949, Concise Binary Object
Representation). CBOR is a compact binary encoding similar to JSON in
structure.

### Conventions

- **Map keys**: Short lowercase ASCII strings (e.g., `"t"`, `"pm25"`,
  `"co2"`).
- **Missing keys**: If a sensor reading is unavailable or invalid, the key
  is omitted entirely. A missing key means "data not available" — not zero.
- **Numeric types**:
  - Unsigned integers (`uint`): CO2, TVOC, NOx, timestamps, counts.
  - Float32 (`float`): Temperature, humidity, PM, pressure, altitude.
  - Float64 (`double`): GPS latitude and longitude (for precision).
  - Boolean: Config toggles, tracking state.
  - Text string: Enum values (charging state, GPS mode, etc.).

### Payload Sizes and Transport

The device's preferred ATT MTU is **512 bytes**, but the final MTU is negotiated
with the phone and may be lower. For notifications and Write Without Response,
the usable ATT payload is `MTU - 3` bytes. A characteristic Read Response has an
`MTU - 1` byte payload; a larger characteristic value is retrieved with
Read-Long / Read Blob operations.

Notifications are single ATT PDUs and are never fragmented by the application.
The Config snapshot is therefore not a notification payload: it is a Read-Long
value and is typically about 205 bytes with correction schema version 1. Clients
must support Read-Long and must not assume that one Read Response contains the
full snapshot.

| Characteristic | Typical Size | Max/Limit | Transport |
|---|---:|---:|---|
| Measures | ~120 B | ~135 B | One notification when MTU is at least 138 |
| Status Read | ~95 B | ~115 B | Read; notifications carry small deltas |
| Config Read | ~205 B | <512 B | Read-Long / Read Blob |
| Config Notify | — | <180 B | One notification when MTU is at least 185 |
| History control | ~40 B | ~180 B | One notification per response |
| History data | 227 B | 227 B | One notification when MTU is at least 230 |
| OTA data | — | `MTU - 3` (509 B at MTU 512) | Sequential Write Without Response |

---

## 5. Measures Characteristic

**UUID**: `d1c0c0a1-6b48-4b2a-9b1d-59f9f2b0a1e1`
**Properties**: Read, Notify
**Direction**: Device -> Phone

### Read

Read this characteristic at any time to get the latest sensor measurements.
The device updates the characteristic value on every measurement cycle
regardless of connection state, so a phone can read immediately after
connecting without waiting for the first notification.

### Subscription

Subscribe to notifications on this characteristic to receive live sensor
data. The device sends a notification each time a new measurement cycle
completes (typically every few seconds, configurable via Config).

In production builds, both reads and subscription require an authenticated
(paired) connection. If the client reads or enables notifications before
pairing, the BLE stack may trigger the passkey pairing flow automatically.

Notifications are only sent while the device is in **Portable** operating
mode and the BLE client is connected.

Measures contain the device's raw sensor values. The correction settings in the
Config characteristic do not alter Measures reads or notifications; clients may
apply those settings locally or use their own correction policy.

Temperature and humidity custom correction is `scale * raw + intercept`. PM
custom preserves exact raw zero; otherwise it computes
`max(0, scale * raw + intercept)` and can then apply EPA using raw humidity. If
EPA requires invalid humidity, PM2.5 falls back to raw. See
[Measurement Corrections](docs/measurement_corrections.md) for exact EPA
equations, field-validity behavior, and consumer ownership.

### Payload

CBOR map. All keys are optional except `"ts"`.

| Key | Type | Unit | Description |
|---|---|---|---|
| `"t"` | float32 | Celsius | Temperature |
| `"h"` | float32 | % | Relative humidity |
| `"pm1"` | float32 | ug/m3 | PM 1.0 |
| `"pm25"` | float32 | ug/m3 | PM 2.5 |
| `"pm10"` | float32 | ug/m3 | PM 10 |
| `"co2"` | uint | ppm | CO2 concentration |
| `"tvoc"` | uint | index | TVOC index (1–500) |
| `"nox"` | uint | index | NOx index (1–500) |
| `"pres"` | float32 | hPa | Atmospheric pressure |
| `"lat"` | float64 | degrees | GPS latitude |
| `"lon"` | float64 | degrees | GPS longitude |
| `"alt"` | float32 | meters MSL | GPS altitude |
| `"fix"` | uint | enum | GPS fix type: 0=none, 2=2D, 3=3D |
| `"sat"` | uint | count | Satellite count |
| `"ts"` | uint | unix seconds | Measurement timestamp (**always present**) |

### GPS Field Rules

GPS fields (`"lat"`, `"lon"`, `"alt"`, `"fix"`, `"sat"`) are only included
when the device has a valid GPS fix. If the device is idle (not tracking) or
has no GPS fix, all GPS keys are absent.

When GPS is included:

- `"fix"` and `"sat"` are always present in the group.
- `"lat"`, `"lon"`, `"alt"` are individually omitted if that specific
  reading is invalid.

### Examples

**All sensors active, GPS has 3D fix:**

```json
{
  "t": 23.5, "h": 45.2,
  "pm1": 5.0, "pm25": 8.3, "pm10": 12.1,
  "co2": 450, "tvoc": 120, "nox": 5,
  "pres": 1013.2,
  "lat": 47.376887, "lon": 8.541694, "alt": 408.0,
  "fix": 3, "sat": 12,
  "ts": 1711234567
}
```

**Idle, CO2 sensor warming up:**

```json
{
  "t": 23.5, "h": 45.2,
  "pm1": 5.0, "pm25": 8.3, "pm10": 12.1,
  "tvoc": 120, "nox": 5,
  "pres": 1013.2,
  "ts": 1711234567
}
```

GPS keys absent (not tracking). `"co2"` key absent (sensor not ready).

---

## 6. Status Characteristic

**UUID**: `d1c0c0a2-6b48-4b2a-9b1d-59f9f2b0a1e1`
**Properties**: Read, Notify
**Direction**: Device -> Phone

### Usage

Read this characteristic at any time to get a snapshot of the device's
current status. The device refreshes the value internally on battery
polls, GPS fix changes, history-delete reconciliation, and on every
tracking state transition.

### Notifications

Subscribe to receive an immediate push on **urgent transitions**. The device
fires NOTIFY only for the events listed below — _not_ on every periodic Status
refresh. Clients that want live GPS / flash-usage in between should keep polling
via Read (see [Polling the non-urgent fields](#polling-the-non-urgent-fields)).

A Status **NOTIFY carries only a delta** — never the full snapshot. There are
**three delta shapes**, distinguished by which keys are present (there is **no**
`"type"` discriminator on Status notifications):

- **Tracking transition** — `{"tracking": <bool>, "session": <uint>}`.
- **Charging transition** — `{"charging": <text>, "bat_pct": <uint>, "bat_v":
  <float32>}`, pushed when the user plugs in, unplugs, or the battery finishes
  charging.
- **Disconnect notice** — `{"disc": <text>}`, pushed just before the device drops
  the BLE link (see [Disconnect notice](#disconnect-notice-disc) below).

The shapes have **disjoint keys**, so just **merge whichever keys arrive** into
your local model. A notification is a single ATT PDU and is kept bounded by
sending only what changed. The **Read** value remains the full 9-key snapshot
(see Payload); re-read Status for the full state on connect.

| Event | NOTIFY? | Payload reflects |
|---|---|---|
| `start_tracking` succeeded | Yes | `tracking: true`, `session: N` |
| `start_tracking` failed at storage open | Yes | `tracking: false`, `session: 0` |
| `stop_tracking` (manual) | Yes | `tracking: false`, `session: 0` |
| Charging transition (plug in / unplug / charge complete) | Yes | `charging`, `bat_pct`, `bat_v` |
| Device about to drop the link (shutdown / leaving Portable) | Yes | `disc` |
| Resume-after-sleep failed in firmware init | No | BLE is not up yet; client picks it up via Read on the next connect |
| Periodic BMS / GPS refresh (no charging change) | No | — |

#### Disconnect notice (`disc`)

Before the device tears down the BLE link, it pushes a single-key
`{"disc": <text>}` notice so the client can treat the imminent disconnect as
**expected** rather than an error. BLE is only available in Portable mode, so a
`disc` notice always means the link is about to go away — either the device is
shutting down or it is leaving Portable mode. After it, expect a disconnect (and,
for a mode change, no reconnect on the AGo service until the device is Portable
again).

`disc` is **NOTIFY-only** — it never appears in the Status Read snapshot.
Delivery is best-effort; if the device disconnects without a `disc` (lost
notification), treat the disconnect itself as the signal.

| `"disc"` value | Meaning |
|---|---|
| `"overheat"` | Safety shutdown — battery temperature outside the discharge range, hot or cold. This legacy value is shared by both temperature shutdown reasons |
| `"low_batt"` | Safety shutdown — battery over-discharge (critically low) |
| `"user"` | User-initiated shutdown (long-press power) |
| `"op_stationary"` | Operating mode changing to Stationary |
| `"op_offline"` | Operating mode changing to Offline |

#### Read is authoritative

NimBLE silently drops notifications to peers that have not yet enabled
the CCCD, and notifications can be lost in transit. Treat Status NOTIFY
as best-effort. Clients that just connected MUST issue a Read on Status
before relying on subsequent notifies to learn the current state.

#### Tracking started via BLE produces two notifications

When the client issues `{"op":"cmd","cmd":"start_tracking"}` (or
`"stop_tracking"`), it will receive **two** notifications on different
characteristics for the same logical event:

1. **Status NOTIFY** (this characteristic) — broadcasts the state change
   (`tracking`, `session`).
2. **Config NOTIFY** (`type: "cmd_result"`) — the response to the issued
   command, carrying `ok` and optionally `err`.

The two are distinct protocol events on distinct characteristics, not
redundant transport. Handle them in their own listeners.

#### Reconciling unexpected `tracking: true -> false`

If you receive a NOTIFY with `tracking: false` that did **not** follow
a client-issued `stop_tracking` command, treat it as "session ended on
device" and refresh local state from the device (re-list History,
re-read Status). Do not try to infer the cause from the payload — the
on-device snackbar carries the human-readable reason. The most common
causes are a manual stop on the device's own menu and (rarely) a NAND
fault at the storage layer.

### Payload

The **Read** value is a CBOR map with **all 9 keys always present**. A **NOTIFY**
payload carries only one of the delta shapes (tracking transition, charging
transition, or the NOTIFY-only `disc` notice); every snapshot key is available
via Read. The `disc` key is never part of the Read snapshot.

| Key | Type | In NOTIFY? | Description |
|---|---|---|---|
| `"gps_fix"` | uint | No (Read-only) | GPS fix type: `0` = none, `2` = 2D, `3` = 3D |
| `"gps_sat"` | uint | No (Read-only) | Satellite count (0 if unavailable) |
| `"bat_pct"` | uint | Yes (charging delta) | Battery percentage, 0–100 |
| `"bat_v"` | float32 | Yes (charging delta) | Battery voltage in Volts |
| `"charging"` | text | Yes (charging delta) | Charging state (see table below) |
| `"tracking"` | bool | Yes (tracking delta) | `true` if a tracking session is active |
| `"session"` | uint | Yes (tracking delta) | Current tracking session ID (0 if not tracking) |
| `"flash_kb"` | uint | No (Read-only) | Total flash storage capacity in KB |
| `"used_kb"` | uint | No (Read-only) | Used flash storage in KB |

The firmware version is **not** in this payload. Read it from the Device
Information Service Firmware Revision characteristic (`0x2A26`, see §9).

#### Polling the non-urgent fields

The four Read-only fields above (`gps_fix`, `gps_sat`, `flash_kb`, `used_kb`)
are **never pushed** via NOTIFY. They change slowly and are not urgent, so the
device leaves them to the client to poll. While a screen that displays any of
them is visible, **poll the Status characteristic with a Read** at a sensible
cadence — roughly every **30 seconds**, aligned with the device's internal
~30 s battery-refresh rate. Polling faster yields no fresher data; stop polling
when no such screen is visible to save power. Battery and charging state arrive
promptly via the charging-transition NOTIFY, so they do not require polling.

### Charging State Values

| Value | Meaning |
|---|---|
| `"none"` | Not charging |
| `"trickle"` | Trickle charge |
| `"pre"` | Pre-charge |
| `"fast"` | Fast charge |
| `"taper"` | Taper charge |
| `"topoff"` | Top-off timer active |
| `"done"` | Charge complete |
| `"unknown"` | Unknown state |

### Example

```json
{
  "gps_fix": 3,
  "gps_sat": 12,
  "bat_pct": 72,
  "bat_v": 3.85,
  "charging": "none",
  "tracking": true,
  "session": 10042,
  "flash_kb": 262144,
  "used_kb": 8192
}
```

---

## 7. Config Characteristic

**UUID**: `d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1`
**Properties**: Read, Write, Notify
**Direction**: Bidirectional

This characteristic supports three operations:

1. **Read** — Get current device configuration.
2. **Write** — Set config values or execute commands.
3. **Notify** — Receive config change confirmations and command results.

### 7.1 Read: Get Current Config

Read the characteristic to receive the full device configuration.

#### Payload (16-key CBOR map)

| Key | Type | Description |
|---|---|---|
| `"meas_int"` | uint | Measurement interval in seconds (1–3600). All sensors measured together at this cadence. |
| `"temp_f"` | bool | `true` = Fahrenheit, `false` = Celsius |
| `"pm_aqi"` | bool | `true` = US AQI for PM, `false` = raw ug/m3 |
| `"gps_mode"` | text | GPS mode (see table below) |
| `"auto_lock"` | uint | Auto-lock timeout (seconds) |
| `"op_mode"` | text | Operating mode (see table below) |
| `"fled"` | uint | Front (display) LED brightness: 0=Off, 1=Dim, 2=Mid, 3=Bright |
| `"bled"` | uint | Back (AQI) LED brightness: 0=Off, 1=Dim, 2=Mid, 3=Bright |
| `"tled"` | uint | Touch LED intensity: 0=Off, 1=Dim, 2=Bright |
| `"buz"` | bool | Buzzer enabled |
| `"abc"` | int | CO2 ABC period: `-1` disables, otherwise 1–200 days |
| `"tlo"` | uint | TVOC learning-time offset: 1–1000 hours |
| `"nlo"` | uint | NOx learning-time offset: 1–1000 hours |
| `"pm25_corr"` | map | PM2.5 correction (`s`, `v`) |
| `"temp_corr"` | map | Temperature correction (`s`, `v`) |
| `"hum_corr"` | map | Humidity correction (`s`, `v`) |

Correction maps contain schema version `s` and a positional value array `v`.
Schema version 1 uses `[algorithm, scale, intercept]` for temperature and
humidity, and `[algorithm, scale, intercept, flags]` for PM2.5. All coefficients
are finite float32 values. Algorithm enums are `0 = none`, `1 = custom` for
temperature/humidity, and `0 = none`, `1 = epa_2021`, `2 = custom_via_pm25_raw`
for PM2.5. PM2.5 flag bit 0 is reserved for compatibility. Go always emits it
clear and ignores it on `custom_via_pm25_raw` writes.
Canonical `none` uses scale `1.0`, intercept `0.0`, and clear flags. The current
decoder accepts other finite coefficients for `none`; correction math ignores
them and persisted loading canonicalizes them.

#### GPS Mode Values

| Value | Meaning |
|---|---|
| `"off"` | GPS always off |
| `"tracking"` | GPS on only during tracking |
| `"always"` | GPS always on |

#### Operating Mode Values

| Value | Meaning |
|---|---|
| `"portable"` | Portable mode (BLE active, battery powered) |
| `"stationary"` | Stationary mode (Wi-Fi connected, wall powered) |
| `"offline"` | Offline mode (no connectivity) |

#### Example

```json
{
  "meas_int": 10,
  "temp_f": false,
  "pm_aqi": false,
  "gps_mode": "tracking",
  "auto_lock": 60,
  "op_mode": "portable",
  "fled": 3,
  "bled": 3,
  "tled": 2,
  "buz": true,
  "abc": 7,
  "tlo": 12,
  "nlo": 12,
  "pm25_corr": {"s": 1, "v": [2, 1.08, -0.2, 0]},
  "temp_corr": {"s": 1, "v": [0, 1.0, 0.0]},
  "hum_corr": {"s": 1, "v": [0, 1.0, 0.0]}
}
```

### 7.2 Write: Set Config

Write a CBOR map to update device configuration. Include `"op": "set"` and
**exactly one** config key to change. Omitted keys retain their current values.

#### Format

```json
{"op": "set", "meas_int": 30}
```

All config keys from the Read payload are supported. The `"op"` key is
required.

**Single field per write.** A `set` may carry **only one** recognized config
key. This keeps the resulting Config NOTIFY (a delta — see 7.4) within a single
ATT PDU regardless of how many config fields exist. A write carrying more than
one recognized config key (or the same key twice) is rejected before any value
is applied. To change several settings, send one `set` per field.

Deprecated keys (`"pm_int"`, `"other_int"`, `"disp_int"`) are accepted and
silently ignored for backward compatibility. They do not modify any setting.

#### Config key types for writes

| Key | Expected Type | Notes |
|---|---|---|
| `"meas_int"` | uint | 1–3600 seconds |
| `"temp_f"` | bool | |
| `"pm_aqi"` | bool | |
| `"gps_mode"` | text | `"off"`, `"tracking"`, or `"always"` |
| `"auto_lock"` | uint | |
| `"op_mode"` | text | `"portable"`, `"stationary"`, or `"offline"` |
| `"fled"` | uint | 0–3 (front LED brightness) |
| `"bled"` | uint | 0–3 (back LED brightness) |
| `"tled"` | uint | 0–2 (touch LED intensity) |
| `"buz"` | bool | Buzzer enabled |
| `"abc"` | int | `-1` disables ABC; otherwise 1–200 days |
| `"tlo"` | uint | 1–1000 hours |
| `"nlo"` | uint | 1–1000 hours |
| `"pm25_corr"` | map | Complete PM2.5 correction group |
| `"temp_corr"` | map | Complete temperature correction group |
| `"hum_corr"` | map | Complete humidity correction group |

Correction writes replace one complete correction group and count as one
recognized config key:

```json
{"op": "set", "pm25_corr": {"s": 1, "v": [2, 1.08, -0.2, 1]}}
```

The device validates the schema version, array length, algorithm enum, flags, and
finite coefficients. A successful correction write persists the settings and
updates the device's display and PM AQI LED. It does not send a Measures
notification because the raw measurement data did not change. Measures and
History remain raw; clients may apply the Config corrections locally.

#### Response

After applying the config change, the device sends a **Config notification**
(see 7.4 below). No progress notification is sent for config set operations.

The write is rejected before any value is applied if it contains an
**unrecognized config key** (`unknown_config_key`, checked first), an invalid
interval, GPS mode, LED, buzzer, ABC, learning-offset, or correction value
(`invalid_config_value`), or **more than one recognized config key**
(`single_field_only`). A settings persistence failure returns
`config_save_failed`. On rejection no settings are modified and the device
sends an error notification instead:

```json
{"type": "cmd_result", "cmd": "set", "ok": false, "err": "unknown_config_key"}
```

```json
{"type": "cmd_result", "cmd": "set", "ok": false, "err": "single_field_only"}
```

Aiding keys (`lat`, `lon`, `alt`, `pos_acc`, `epoch`, `time_acc`) are valid only
under `op:"cmd"` (`set_aiding`); under `op:"set"` they are rejected as
`unknown_config_key`.

#### Leaving `"portable"` mode disconnects BLE

Any change of the operating mode away from Portable tears down the Portable BLE
link — whether **you** set `op_mode` to `"stationary"`/`"offline"`, or the user
changes the mode **on the device** itself. In both cases the device announces the
imminent disconnect with a **Status `disc` notice** (`"op_stationary"` or
`"op_offline"` — see [§6 Disconnect notice](#disconnect-notice-disc)), waits a
short settle window (~200 ms) so the notification can drain, then disconnects and
switches mode. So an `op_mode` set is **not** confirmed with a Config delta;
instead, watch for the `disc` notice on **Status** and treat the following
disconnect as completion of the switch. Because notifications are best-effort,
the `disc` may occasionally be missed; if a disconnect follows (with no reconnect
on the AGo service), assume the device left Portable mode.

### 7.3 Write: Execute Command

Write a CBOR map to execute a device command.

#### Format

```json
{"op": "cmd", "cmd": "co2_cal"}
```

#### Supported Commands

| `"cmd"` Value | Action |
|---|---|
| `"co2_cal"` | Trigger CO2 sensor background calibration |
| `"clear_data"` | Erase all stored route data and clear chart cache |
| `"factory_rst"` | Reset to factory defaults: clears data, restores default settings, deletes BLE bonds, and reboots the device |
| `"start_tracking"` | Begin GPS + sensor route logging session |
| `"stop_tracking"` | End the current route logging session |
| `"set_aiding"` | Inject A-GNSS aiding data (position and/or time) to speed up GPS fix |

#### Response

Long-running commands (`"co2_cal"`, `"clear_data"`, `"factory_rst"`) send
a **command progress notification** (see 7.5 below) immediately when the
command is accepted, followed by a **command result notification** (see 7.6
below) when the operation completes or fails.

Treat `cmd_progress` only as acknowledgement that the command was accepted. Keep
the command pending until a `cmd_result` arrives; the GATT write response and
progress notification are not completion signals.

Other commands (`"start_tracking"`, `"stop_tracking"`, `"set_aiding"`) respond
with a command result notification only (no progress notification).

**Notes**:

- After `"factory_rst"`, the device reboots. The BLE connection will
  drop, and all bond information is erased. The phone will need to re-pair on
  the next connection.
- `"start_tracking"` fails with `"err": "already_tracking"` if a tracking
  session is already active, or with `"err": "flash_error"` if the storage
  layer cannot open the route file (NAND unmounted, session-id space
  exhausted, or fsync failure on the empty file). The device additionally
  surfaces this on-screen via a `"Storage error — can't track"` snackbar.
- `"stop_tracking"` fails with `"err": "not_tracking"` if no tracking
  session is active.
- After a successful `"start_tracking"` or `"stop_tracking"`, the device
  also pushes a **Status NOTIFY** with the new `"tracking"` / `"session"`
  values. Clients subscribed to Status will therefore see two
  notifications per tracking transition: one Config `cmd_result` and one
  Status NOTIFY (see §6).
- `"set_aiding"` accepts optional position and/or time fields (see below).
  At least one useful piece of data must be present; otherwise the command
  fails with `"err": "no_aiding_data"`.
- Keep at most one `"co2_cal"` request outstanding. The firmware has no shared
  duplicate/busy gate and the sensor task keeps only the latest unconsumed
  request, so repeated writes can coalesce. A `co2_cal` result has neither a
  request ID nor an origin and can result from BLE, local HTTP, or UI
  calibration. Wait for a result before retrying, but do not assume it belongs
  to a particular BLE write.

#### Set Aiding Payload

The `"set_aiding"` command carries optional aiding data fields in the same
CBOR map. The device forwards valid data to the GPS module to reduce
cold-start time-to-first-fix (TTFF).

```json
{
  "op": "cmd",
  "cmd": "set_aiding",
  "lat": 47.376887,
  "lon": 8.541694,
  "alt": 408.0,
  "pos_acc": 50.0,
  "epoch": 1711234567,
  "time_acc": 2000
}
```

| Key | Type | Unit | Description | Required |
|---|---|---|---|---|
| `"lat"` | float64 | decimal degrees | Approximate latitude | No |
| `"lon"` | float64 | decimal degrees | Approximate longitude | No |
| `"alt"` | float32 | meters MSL | Approximate altitude | No |
| `"pos_acc"` | float32 | meters (1-sigma) | Position accuracy estimate | No |
| `"epoch"` | uint | POSIX seconds | Current UTC time | No |
| `"time_acc"` | uint | milliseconds | Time accuracy estimate | No |

**Position injection** requires both `"lat"` and `"lon"` to be present and
valid. `"alt"` and `"pos_acc"` are optional refinements.

**Time injection** requires `"epoch"` to be non-zero.

The phone should send this command shortly after connecting, using the
phone's own location and clock as the data source. It can also be re-sent
whenever the phone obtains a significantly updated position.

**System clock side-effect**: If the device's system clock has not yet been
synced (no GPS timestamp received), and the aiding data includes a valid
`"epoch"`, the device also sets its internal clock from the aiding epoch.
This provides meaningful timestamps for route-point data before the first
GPS fix arrives. The aiding epoch is approximate, so the device does not
mark the clock as synced — when a real GPS timestamp arrives, it overwrites
with the authoritative time.

### 7.4 Notify: Config Changed

Subscribe to Config notifications to receive confirmation when any
configuration change is applied (whether from this BLE client, another
source, or the device's own UI).

#### Payload (delta CBOR map)

A Config NOTIFY is a **delta**, not the full snapshot: `"type": "config"` plus
**only the field(s) that changed**. A single-field `set` (and the device UI,
which normally changes one setting at a time) yields a 2-key map:

```json
{"type": "config", "meas_int": 10}
```

Merge the changed key(s) into your local model. Production sends no Config
notification for a no-op write. The full config is always available via **Read /
Read-Long** (no `"type"` key) — re-read it on connect to establish the baseline.
The snapshot is typically about 205 bytes with schema version 1, so clients must
collect Read-Long fragments when the negotiated MTU cannot carry the complete
value. The `"type"` key distinguishes this from command notifications (all
arrive on the same characteristic; Read always returns the config snapshot
regardless of which notification kind was last sent).

If a notification **fails to CBOR-decode**, do not guess — re-Read the
characteristic. A notification is a single ATT PDU and is never fragmented, so a
payload that exceeds the negotiated MTU is truncated by the stack; a failed
decode is the client's signal to fall back to the authoritative Read. Keeping a
negotiated MTU ≥ 185 B (see §12) makes this path unnecessary in practice.

Decoupling NOTIFY from the Read snapshot keeps every notification within one ATT
PDU, independent of how many config fields the snapshot grows to carry.

### 7.5 Notify: Command Progress

For long-running commands (`"co2_cal"`, `"clear_data"`, `"factory_rst"`),
the device sends a progress notification immediately when the command is
accepted, before the actual work begins. This lets the client show a
loading indicator while waiting for the final result.

```json
{"type": "cmd_progress", "cmd": "co2_cal"}
```

2 keys: `"type"`, `"cmd"`. No `"ok"` or `"err"` keys.

The `"cmd"` value echoes the command string from the write request.

**Client handling**: When a `"cmd_progress"` notification arrives, display a
progress/loading state for the indicated command. The final outcome will
arrive as a separate `"cmd_result"` notification. Clients that do not
recognize `"cmd_progress"` can safely ignore it — the `"cmd_result"`
notification is unchanged and self-contained.

For `"co2_cal"`, do not clear the loading state or issue another calibration
when progress arrives. Wait for `{"type":"cmd_result","cmd":"co2_cal",...}`;
if the result times out or the BLE link drops first, treat the outcome as
unknown. Reconnecting does not provide a calibration-status query.

**Timing**: The progress notification arrives promptly after the write.
Unsupported sensors or command failures may return immediately. Otherwise the
sensor manager polls every 5 seconds for up to 12 attempts, so a result can take
about 60 seconds. Use a timeout exceeding 60 seconds plus notification margin;
if no result arrives, treat the outcome as unknown rather than issuing
overlapping retries.

### 7.6 Notify: Command Result

After executing a command, the device sends a result notification.

#### Success

```json
{"type": "cmd_result", "cmd": "co2_cal", "ok": true}
```

3 keys: `"type"`, `"cmd"`, `"ok"`.

#### Failure

```json
{"type": "cmd_result", "cmd": "co2_cal", "ok": false, "err": "calibration_failed"}
```

4 keys: `"type"`, `"cmd"`, `"ok"`, `"err"`.

The `"err"` key is only present when `"ok"` is `false` and an error
description is available.

#### Command Error Strings

| Error String | Command | Cause |
|---|---|---|
| `"unsupported"` | `co2_cal` | No CO2 sensor is selected or the selected driver does not support manual calibration; S12 and SCD4x support it, while the current STCC4 driver does not |
| `"calibration_failed"` | `co2_cal` | CO2 calibration procedure failed |
| `"clear_failed"` | `clear_data` | Route data erase did not complete fully |
| `"factory_reset_failed"` | `factory_rst` | Settings save, data clear, or bond delete failed |
| `"already_tracking"` | `start_tracking` | Tracking session was already active |
| `"flash_error"` | `start_tracking` | Storage layer could not open the route file: NAND unmounted, session-id collision exhaustion, or fsync failure. Device shows `"Storage error — can't track"` on-screen. |
| `"not_tracking"` | `stop_tracking` | No tracking session was active |
| `"no_aiding_data"` | `set_aiding` | No valid position or time data in the payload |
| `"unknown_command"` | (any) | Unrecognised `"cmd"` string |
| `"unknown_config_key"` | `set` | Config write contained an unrecognised key (or an aiding key under `op:"set"`); entire write rejected |
| `"single_field_only"` | `set` | Config write carried more than one recognized config key (or a duplicate); entire write rejected |

### 7.7 Notification Dispatch

Config notifications always contain a `"type"` key. Use it to dispatch:

| `"type"` Value | Notification Kind |
|---|---|
| `"config"` | Config changed — delta (changed key(s) only); merge into local model |
| `"cmd_progress"` | Command accepted — show loading state |
| `"cmd_result"` | Command finished — check `"ok"` and `"err"` |

---

## 8. History Characteristic

**UUID**: `d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1`
**Properties**: Write, Notify
**Direction**: Bidirectional

Bulk export of stored route session data from the device's flash storage.
This is a stateful download protocol using CBOR for control messages and
packed binary for bulk data transfer.

History points are exported from the raw route data without applying the device's
correction settings. Clients may apply the current Config corrections or another
policy locally. Route points do not contain a correction-version identifier.

### 8.1 Notification Format

Every History notification starts with a **1-byte type tag**:

| Tag | Content | Meaning |
|---|---|---|
| `0x00` | CBOR map | Control response (session list, started, done, ended, error) |
| `0x01` | Binary data | Route point data chunk |

**Important**: Strip the first byte before decoding. CBOR payloads start at
byte index 1.

### 8.2 Write Commands (Phone -> Device)

All writes are CBOR maps with an `"op"` key.

#### List Sessions

```json
{"op": "list"}
```

Request the list of stored route sessions. Can be sent at any time.

#### Start Download

```json
{"op": "start", "session": 10042}
```

Start downloading all points for the specified session. The `"session"` value
is a session ID obtained from the session list. Existing sessions with zero
points are valid download targets.

#### Fill Missing Points

```json
{"op": "fill", "pts": [12, 13, 14, 15, 78]}
```

Request retransmission of specific point indices that were missed during the
initial download. The `"pts"` array contains zero-based point indices. Can
only be sent after a download has completed (after receiving `"done"`).

The device's write buffer (256 bytes) limits the `"pts"` array to
approximately 50 indices per request. For larger gaps, send multiple `fill`
requests.

#### End Download

```json
{"op": "end"}
```

Signal that the download is complete. Cleans up server-side state.

#### Delete Session

```json
{"op": "delete", "session": 10042}
```

Delete a single route session from the device's flash storage. The
`"session"` value is a session ID obtained from the session list.

The device rejects deletion of the currently active tracking session
(returns `"session_active"` error). If the session is currently being
downloaded, the device silently ends the export before deleting.

After a successful delete, the device updates its Status characteristic
internally so the next read reflects the reduced `"used_kb"`.

Can be sent at any time (does not require an active download). Listed sessions
with zero points can be deleted normally.

### 8.3 Notify Responses (Device -> Phone)

All CBOR responses are prefixed with tag byte `0x00`. Decode the CBOR
starting at byte index 1.

#### Session List

Sent in response to `{"op": "list"}`. The response is **paginated** — the
device sends one notification per page. Each notification is self-contained
with pagination metadata. Collect pages until `"pg" == "tpg"`.

```json
{
  "type": "sessions",
  "sessions": [
    {"id": 10001, "pts": 150, "ts": 1737000000},
    {"id": 10002, "pts": 300, "ts": 1737100000}
  ],
  "pg": 1,
  "tpg": 3,
  "cnt": 13
}
```

| Field | Type | Description |
|---|---|---|
| `"id"` | uint | Session ID |
| `"pts"` | uint | Number of route points in this session |
| `"ts"` | uint | Session start time (unix seconds), or 0 when the session has no points |
| `"pg"` | uint | Current page number (1-based) |
| `"tpg"` | uint | Total number of pages |
| `"cnt"` | uint | Total number of sessions across all pages |

Sessions are paginated in groups of 6 per notification. There is no hard
cap on the total number of sessions — all sessions stored on the device
are included. If the device has no sessions, a single page is sent with
an empty `"sessions"` array and `"cnt": 0`.

A route stopped before its first measurement is still a valid stored session.
It is included with `"pts": 0` and `"ts": 0` and remains downloadable and
deletable.

#### Download Started

Sent at the beginning of a `start` operation, before binary data streaming
begins.

```json
{"type": "started", "session": 10042, "total": 300, "pt_size": 56}
```

| Field | Type | Description |
|---|---|---|
| `"session"` | uint | Session ID being downloaded |
| `"total"` | uint | Total number of points in this session |
| `"pt_size"` | uint | Bytes per route point (always 56) |

The `"pt_size"` field allows the client to verify wire format compatibility.
If `"pt_size"` is not 56, the client should abort — the binary format is
incompatible.

For an empty session, `"total"` is 0. The device sends no binary chunks and
immediately follows `"started"` with `{"type": "done", "sent": 0}`. The
client should still send `{"op": "end"}` when it finishes the download.

#### Binary Data Chunks

Sent after `"started"`. Tagged with `0x01`.

Binary layout:

```text
[0x01] [uint16_le point_index] [RoutePointWire...]
```

| Offset | Size | Type | Description |
|---|---|---|---|
| 0 | 1 | uint8 | Tag: `0x01` |
| 1 | 2 | uint16_le | Index of the first point in this chunk |
| 3 | N*56 | bytes | Packed RoutePointWire data |

Each notification contains up to **4 route points** (4 x 56 = 224 bytes of
point data, plus 3 bytes header = 227 bytes total).

Points are streamed sequentially starting from index 0. The `point_index`
field tells the client which points are in this notification. Use it to
detect gaps (missed notifications).

#### Download Done

Sent after all binary data has been streamed (for both `start` and `fill`
operations).

```json
{"type": "done", "sent": 300}
```

| Field | Type | Description |
|---|---|---|
| `"sent"` | uint | Number of points actually sent |

#### Download Ended

Sent in response to `{"op": "end"}`.

```json
{"type": "ended"}
```

#### Session Deleted

Sent in response to a successful `{"op": "delete"}`.

```json
{"type": "deleted", "session": 10042}
```

| Field | Type | Description |
|---|---|---|
| `"session"` | uint | Session ID that was deleted |

#### Error

Sent when an operation fails.

```json
{"type": "error", "err": "session_not_found"}
```

| Error String | Cause |
|---|---|
| `"session_not_found"` | The requested session ID does not exist |
| `"no_active_download"` | `fill` was sent without a prior `start` |
| `"flash_error"` | Flash storage read failed during streaming |
| `"delete_failed"` | Flash storage delete failed |
| `"session_active"` | Cannot delete the active tracking session |
| `"busy"` | Provisioning Wi-Fi radio is active; export refused (see §10) |

### 8.4 Download Protocol Flow

#### Complete Download

```text
Phone                              Device
  |                                  |
  |---- {"op": "list"} ------------>|
  |<-- [0x00] sessions list --------|
  |                                  |
  |---- {"op": "start", session: N} |
  |<-- [0x00] started --------------|
  |<-- [0x01] points 0-3 ----------|  (streaming begins)
  |<-- [0x01] points 4-7 ----------|
  |     ...                          |
  |<-- [0x01] points 296-299 ------|
  |<-- [0x00] done (sent: 300) ----|  (streaming complete)
  |                                  |
  |---- {"op": "end"} ------------>|
  |<-- [0x00] ended ----------------|
```

#### With Gap Recovery

BLE notifications are unreliable — they can be lost. The client should track
which point indices it received and use `fill` to request any gaps.

```text
Phone                              Device
  |                                  |
  |---- {"op": "start", session: N} |
  |<-- [0x00] started --------------|
  |<-- [0x01] points 0-3 ----------|
  |<-- [0x01] points 4-7 ----------|
  |     ... (points 8-11 lost) ...   |
  |<-- [0x01] points 12-15 --------|
  |     ...                          |
  |<-- [0x00] done (sent: 300) ----|
  |                                  |
  |  (client detects gap: 8-11)      |
  |                                  |
  |---- {"op": "fill", pts:[8,9,10,11]} |
  |<-- [0x01] point 8 -------------|
  |<-- [0x01] point 9 -------------|
  |<-- [0x01] point 10 ------------|
  |<-- [0x01] point 11 ------------|
  |<-- [0x00] done (sent: 4) ------|
  |                                  |
  |  (verify all points received)    |
  |                                  |
  |---- {"op": "end"} ------------>|
  |<-- [0x00] ended ----------------|
```

#### Delete a Session

```text
Phone                              Device
  |                                  |
  |---- {"op": "list"} ------------>|
  |<-- [0x00] sessions list --------|
  |                                  |
  |  (user selects session to delete)|
  |                                  |
  |---- {"op": "delete", session: N}|
  |<-- [0x00] deleted (session: N) -|
  |                                  |
  |  (optionally re-list to confirm) |
  |                                  |
  |---- {"op": "list"} ------------>|
  |<-- [0x00] sessions list --------|
```

If the session is currently being tracked:

```text
Phone                              Device
  |                                  |
  |---- {"op": "delete", session: N}|
  |<-- [0x00] error: session_active |
```

### 8.5 Client Implementation Guide

1. **Subscribe** to History notifications before sending any write commands.
2. **List sessions** with `{"op": "list"}` to discover available data.
3. **Start download** with `{"op": "start", "session": <id>}`.
4. **Buffer incoming notifications** — dispatch by first byte tag:
   - `0x00`: Decode CBOR, handle control message.
   - `0x01`: Extract `point_index` (bytes 1-2, little-endian uint16), then
     parse route point data starting at byte 3.
5. **Track received indices** to detect gaps after `"done"` is received.
6. **Fill gaps** if any point indices are missing, using one or more `fill`
   commands.
7. **End download** with `{"op": "end"}` when all data is received.
8. **Delete sessions** with `{"op": "delete", "session": <id>}` to remove
   individual route files from the device. Check the Status characteristic's
   `"tracking"` and `"session"` fields first to avoid attempting to delete
   the active tracking session.

**Gap detection**: Compare the expected set of indices `[0, total-1]` with
the indices actually received. Each binary notification's `point_index` tells
you the starting index. With 4 points per notification, a notification with
`point_index = 8` contains points 8, 9, 10, 11.

---

## 9. Device Information Service

Standard BLE Device Information Service (`0x180A`) exposing read-only device
identity. All characteristics are encrypted (require the bonded connection).

| Characteristic | UUID | Type | Value |
|---|---|---|---|
| Model Number | `0x2A24` | UTF-8 | `"P-1PSG"` |
| Serial Number | `0x2A25` | UTF-8 | 12-char lowercase hex device serial |
| Firmware Revision | `0x2A26` | UTF-8 | Firmware version string |
| Manufacturer Name | `0x2A29` | UTF-8 | `"AirGradient"` |

Read these once after connecting to identify the device. The same service is
also present during standalone Stationary provisioning.

The **Firmware Revision** characteristic (`0x2A26`) is the canonical source for
the running firmware version; it is not duplicated in the Status characteristic
(§6).

---

## 10. Wi-Fi Provisioning (Portable)

Configure the device's Wi-Fi credentials over the **already-bonded Portable
link** — no operating-mode switch and no re-pair. The device runs the standard
AirGradient provisioning protocol (Wi-Fi scan, credential submit, live status)
on a dedicated GATT service that sits alongside the data service.

> The same provisioning service is also used standalone in Stationary mode
> (over a Just Works connection); this section documents the Portable
> bonded-link flow.

Unlike the rest of this spec, provisioning payloads are **JSON** (UTF-8), not
CBOR.

### Service and Characteristics

| Field | Value |
|---|---|
| Service UUID | `acbcfea8-e541-4c40-9bfd-17820f16c95c` |

| Characteristic | UUID | Properties | Direction |
|---|---|---|---|
| Wi-Fi Scan | `467a080f-e50f-42c9-b9b2-a2ab14d82725` | Write, Notify | Bidirectional |
| Credentials/Status | `703fa252-3d2a-4da9-a05c-83b0d9cacb8e` | Read, Write, Notify | Bidirectional |

Both characteristics are encrypted; on the Portable link the existing passkey
bond satisfies that, so no extra pairing step is needed.

### Availability and the on-demand radio

- The provisioning service is present on **every Portable boot** and stays idle
  until the client writes a scan or credentials request.
- The Wi-Fi radio is **off by default** and powered only for the few seconds of
  an active scan or connect, then dropped. Two consequences for the client:
  - A scan can take several seconds (the radio time-shares the antenna with
    BLE).
  - The radio auto-drops after ~90 s of provisioning inactivity. Treat a stale
    scan list as expired and re-scan before submitting if the user lingered.

### 10.1 Wi-Fi Scan

Write **any** non-empty value to the Wi-Fi Scan characteristic to trigger a
scan. Results arrive as one or more **paginated** notifications on the same
characteristic (3 networks per page).

Page payload:

```json
{
  "wifi": [
    {"s": "HomeWiFi", "r": -45, "o": 0},
    {"s": "Cafe", "r": -67, "o": 1}
  ],
  "page": 1,
  "tpage": 2,
  "found": 5
}
```

| Key | Type | Description |
|---|---|---|
| `"wifi"` | array | Up to 3 networks for this page |
| `"s"` | text | SSID |
| `"r"` | int | RSSI (dBm) |
| `"o"` | uint | `1` = open network, `0` = secured |
| `"page"` | uint | Current page (1-based) |
| `"tpage"` | uint | Total pages |
| `"found"` | uint | Total networks found across all pages |

Empty result (no networks): a single notification `{"found": 0}`.

Collect pages until `"page" == "tpage"`. Results are deduplicated and
RSSI-sorted by the device.

### 10.2 Submit Credentials

Write a JSON object to the Credentials/Status characteristic:

```json
{
  "ssid": "HomeWiFi",
  "password": "mypassword",
  "disableCloud": false,
  "staticIp": {
    "ip": "192.168.1.50",
    "netmask": "255.255.255.0",
    "gateway": "192.168.1.1",
    "dns": "192.168.1.1"
  }
}
```

| Key | Type | Required | Notes |
|---|---|---|---|
| `"ssid"` | text | Yes | Target network SSID |
| `"password"` | text | No | 8–63 chars; omit or empty for an open network |
| `"disableCloud"` | bool | No | Persisted; suppresses cloud upload in Stationary |
| `"staticIp"` | object | No | All four sub-fields required together; omit for DHCP |

`"staticIp"` sub-fields (`"ip"`, `"netmask"`, `"gateway"`, `"dns"`) are dotted
IPv4 strings. If `"staticIp"` is present it must be complete and valid,
otherwise it is ignored and the device uses DHCP.

### 10.3 Status Notifications

After credentials are submitted, the device powers the radio, attempts a single
STA connection, and notifies status on the Credentials/Status characteristic:

```json
{"status": 0}
```

| Code | Meaning |
|---|---|
| `0` | `WIFI_CONNECTED` — connected and credentials saved |
| `10` | `WIFI_CONNECT_FAILED` — could not connect; re-prompt and retry |

> In Portable, only codes `0` and `10` are emitted. The server-reachability
> codes (`1`, `2`, `3`, `11`, `12`, `13`) defined for the provisioning service
> are produced only during Stationary onboarding, where the device performs a
> cloud check; Portable provisioning does not.

### Verify-then-drop semantics (important)

`WIFI_CONNECTED` means the credentials were **verified and saved** — it does
**not** mean the device is online to the cloud. After a successful connect the
device drops the Wi-Fi radio and **stays in Portable**. The saved credentials
are used the next time the device enters Stationary mode. The session stays
open, so the client can scan / submit again to reconfigure.

A failed connect leaves the device listening; submit corrected credentials to
retry.

### History export contention

While the provisioning radio is active (during a scan/connect), History export
requests (`list` / `start` / `fill`) are rejected with
`{"type": "error", "err": "busy"}`. Do not interleave a History download with
provisioning; retry the export once provisioning is idle.

### Flow

```text
Phone                              Device (Portable, bonded)
  |                                  |
  |-- write Scan char -------------->|  (powers Wi-Fi, scans)
  |<-- notify scan page 1/2 ---------|
  |<-- notify scan page 2/2 ---------|
  |                                  |
  |-- write Credentials char ------->|  (single STA connect)
  |<-- notify {"status": 0} ---------|  (saved + verified, radio dropped)
  |                                  |
  |  (creds used on next Stationary entry; device stays Portable)
```

### Upgrade note: GATT cache

The provisioning and DIS services are new in this firmware. A phone that bonded
to the device **before** this firmware may have a cached GATT layout without
them. If the services are missing after upgrade, refresh the GATT cache (the
device sends a Service Changed indication, `0x1801`) or remove and re-pair the
device.

---

## 11. Firmware Update (OTA, Portable)

Push a firmware image to the device over the **already-bonded Portable link**.
The device exposes a dedicated OTA GATT service alongside the data service; the
phone drives the whole transfer (the device only flashes what it receives). OTA
over BLE is available **only in Portable mode** — in Stationary the device
updates itself over Wi-Fi without phone involvement.

> The image is the raw application binary (`airgradient-go.bin`). It is written
> to the passive OTA partition; the device reboots into it on success.

### Service and Characteristics

| Field | Value |
|---|---|
| Service UUID | `ab9a0001-1e3c-4f5a-9b6d-0a1b2c3d4e5f` |

| Characteristic | UUID | Properties | Direction |
|---|---|---|---|
| Control | `ab9a0002-1e3c-4f5a-9b6d-0a1b2c3d4e5f` | Write | Phone -> Device |
| Data | `ab9a0003-1e3c-4f5a-9b6d-0a1b2c3d4e5f` | Write Without Response | Phone -> Device |
| Status | `ab9a0004-1e3c-4f5a-9b6d-0a1b2c3d4e5f` | Notify | Device -> Phone |

All three require an authenticated (bonded MITM) link — the same pairing the
data service uses, so no second bond. Status is **NOTIFY-only** (no readable
value); subscribe to its CCCD before starting. Control payloads are **CBOR**;
Data payloads are **raw image bytes** (not CBOR).

Neither UUID is advertised; discover the service via GATT discovery after
connecting. A phone bonded before this firmware may need a GATT-cache refresh
(see §10 "Upgrade note").

### 11.1 Control (Phone -> Device)

CBOR map with an `"op"` key.

#### Start

```json
{"op": "start", "total": 1837008, "fw": "3.4.0"}
```

| Key | Type | Required | Notes |
|---|---|---|---|
| `"op"` | text | Yes | `"start"` |
| `"total"` | uint | Yes | Image size in bytes (1 .. 2^32-1) |
| `"fw"` | text | No | Informational version string (≤ 32 bytes) |

The device validates the request, then **erases the OTA partition** (takes
several seconds) and emits the **ready** Status NOTIFY (`state = downloading`).
**Do not send Data until the ready NOTIFY arrives.** Allow a generous
`start -> ready` timeout (≥ ~10 s): the device may finish other foreground work
before it erases.

A `start` while a transfer is already active is ignored. A malformed or
oversized `start` is answered with a `Failed / InvalidArgument` Status NOTIFY and
the device stays idle.

#### End

```json
{"op": "end"}
```

Send after the last Data chunk. The device verifies the received byte count
equals `total`, validates the image, sets it as the boot partition, emits
`Applying` then `Done`, and reboots. A byte-count mismatch yields
`Failed / TransportError`.

#### Abort

```json
{"op": "abort"}
```

Cancel an in-flight transfer. The device discards the partial image and emits
`Failed / Aborted`.

### 11.2 Data (Phone -> Device)

After the ready NOTIFY, stream the **raw image bytes in order** via
**Write-Without-Response** to the Data characteristic. No framing, no CBOR —
just the file bytes split into chunks.

- Each write must be ≤ **`MTU − 3`** bytes (the ATT Write-Command payload) and
  ≤ **512** bytes (the device's hard cap). Negotiate MTU 512 (the device
  prefers 512) for a 509-byte chunk.
- Send chunks sequentially; the device flashes each as it arrives.
- Write-Without-Response has no ATT ACK — pace sends so the local TX queue does
  not overrun (a sub-millisecond inter-chunk gap is enough in practice).
- Sending Data before the ready NOTIFY, an empty write, an oversized write, or
  more bytes than `total` aborts the transfer with `TransportError`.

### 11.3 Status (Device -> Phone)

Subscribe for progress and the terminal result. CBOR map, always 3 keys:

```json
{"state": 1, "result": 0, "bytes": 524288}
```

| Key | Type | Description |
|---|---|---|
| `"state"` | uint | Transfer state (see below) |
| `"result"` | uint | Result code (see below); `0` while in progress |
| `"bytes"` | uint | Bytes the device has accepted/flashed (true progress) |

Drive the progress bar from `"bytes"`, **not** your own send count — with
Write-Without-Response the send count runs ahead of what the device has flashed.

#### State values

| Value | State | Meaning |
|---|---|---|
| `1` | Downloading | Partition ready / streaming (the **ready** signal and each progress tick) |
| `2` | Applying | All bytes received; validating + setting the boot partition |
| `3` | Done | Image applied; the device reboots into it |
| `4` | Failed | Transfer failed (see `result`) |

#### Result values

| Value | Result | Meaning |
|---|---|---|
| `0` | Ok | In progress, or success (`state = done`) |
| `1` | FlashError | Flash write/finish failure |
| `2` | InvalidImage | Image failed validation at finish |
| `3` | TransportError | Disconnect, stall, truncation, or protocol violation |
| `4` | Aborted | Phone sent `abort` |
| `5` | InvalidArgument | Malformed / oversized `start` |

A mid-transfer **disconnect** ends the transfer as `TransportError` (not
`Aborted`); only an explicit `abort` yields `Aborted`. If no Data arrives for
~10 s the device aborts the stall with `TransportError`.

### 11.4 Flow

```text
Phone                              Device (Portable, bonded)
  |-- subscribe Status CCCD ------->|
  |-- write Control {op:start} ---->|  (validate, erase partition — seconds)
  |<-- notify {state:1,result:0} ---|  (ready)
  |-- Data chunk (WnR) ------------>|  (flash)
  |-- Data chunk (WnR) ------------>|
  |     ...                          |
  |<-- notify {state:1,bytes:N} ----|  (periodic progress)
  |-- write Control {op:end} ------>|
  |<-- notify {state:2} ------------|  (applying)
  |<-- notify {state:3,result:0} ---|  (done)
  |        (device reboots into the new image)
```

### 11.5 Constraints

- **One transfer at a time**, Portable only.
- **Do not interleave** an OTA `start` with an active History export (§8) — the
  two share the device's single foreground task. Finish or abort the export
  first.
- Negotiate **MTU 512** before starting for best throughput (see §12).

### 11.6 Connection Parameters

When the transfer begins the device requests a **fast connection interval**
(15–30 ms, slave latency 0, ~2 s supervision timeout) to maximize Data
throughput, and restores a relaxed window (30–50 ms) on a non-success terminal
(a successful update reboots, so no restore is needed).

This is a peripheral-initiated connection-parameter-update request — the central
(phone) may accept, clamp, or reject it, and iOS / Android each enforce their own
interval policy. For best throughput, let the OS honor a short interval during
the transfer and avoid forcing a long one. The transfer is correct at any
negotiated interval; only its speed is affected.

---

## 12. MTU Considerations

### Negotiation

- The Go firmware sets its preferred ATT MTU to **512 bytes**. This is a local
  preference, not a guarantee: the final value is negotiated with the phone and
  is the lower value supported by both peers.
- Request the highest MTU supported by the phone immediately after connecting,
  preferably 512, and wait for the MTU exchange to complete before starting
  history or OTA transfers. Use the reported negotiated value for all chunk
  sizes; do not hard-code 512-byte writes.
- For notifications and Write Without Response, the usable payload is `MTU - 3`.
  The default ATT MTU of 23 therefore provides only 20 application bytes. A
  notification larger than the negotiated payload is not application-fragmented;
  the client should treat a failed CBOR decode as a reason to re-Read the
  characteristic.

### Required MTUs by operation

- **Config Read**: the full 16-key snapshot is bounded by a 512-byte
  characteristic buffer. Use Read-Long / Read Blob and
  collect all fragments. Config Read does not require MTU 512, but it does
  require a client API that supports long reads.
- **Config and Status notifications**: notifications contain deltas, not the
  full Config or Status snapshot. Keep the negotiated MTU at **185 bytes or
  higher** for the documented notification budget (up to 180 application
  bytes). Re-Read the characteristic if a notification cannot be decoded.
- **Measures notifications**: the maximum Measures payload is approximately
  135 bytes, requiring an MTU of at least 138 bytes. MTU 185 or higher is
  recommended for normal operation.
- **History binary notifications**: each chunk is 227 bytes (3-byte header plus
  four 56-byte points), so the negotiated MTU must be at least **230 bytes** to
  carry one complete chunk. The current firmware always sends four points per
  chunk; a larger MTU does not increase that count.
- **OTA Data writes**: use Write Without Response with chunks no larger than
  `MTU - 3`, and no larger than the device's 512-byte write cap. At the preferred
  MTU 512, the maximum ATT payload is 509 bytes.

---

## 13. Error Reference

### Config Command Errors

Command result notifications (`"type": "cmd_result"`) may include an `"err"`
field. Error strings are operation-specific and defined by the device
firmware.

### History Protocol Errors

| Error | Meaning | Recovery |
|---|---|---|
| `"session_not_found"` | Session ID doesn't exist | Re-list sessions |
| `"no_active_download"` | `fill` sent without `start` | Send `start` first |
| `"flash_error"` | Storage read failed | Retry or skip session |
| `"delete_failed"` | Flash delete failed | Retry or ignore |
| `"session_active"` | Cannot delete the active tracking session | Stop tracking first via Config `"stop_tracking"` command, then retry |
| `"busy"` | Provisioning Wi-Fi radio is active | Wait until provisioning is idle (radio drops after a scan/connect or ~90 s), then retry |

### Connection Errors

- If the device disconnects during a history download, the download state is
  automatically cleaned up on the device side. The client must restart the
  download from `start` after reconnecting.
- The device is a single-connection peripheral. Only one phone can connect
  at a time.

---

## Appendix: RoutePointWire Binary Format

Each route point is 56 bytes, packed little-endian. Use this layout to parse
binary data from History notifications.

| Offset | Size | Type | Field | Invalid Sentinel |
|---|---|---|---|---|
| 0 | 4 | uint32_le | timestamp (unix seconds) | — |
| 4 | 8 | float64_le | latitude (degrees) | `91.0` |
| 12 | 8 | float64_le | longitude (degrees) | `181.0` |
| 20 | 4 | float32_le | altitude (meters MSL) | `-10000.0` |
| 24 | 1 | uint8 | gps_fix (0=none, 2=2D, 3=3D) | — |
| 25 | 4 | float32_le | temperature (Celsius) | `-1001.0` |
| 29 | 4 | float32_le | humidity (%) | `-1.0` |
| 33 | 4 | float32_le | pm1.0 (ug/m3) | `-1.0` |
| 37 | 4 | float32_le | pm2.5 (ug/m3) | `-1.0` |
| 41 | 4 | float32_le | pm10 (ug/m3) | `-1.0` |
| 45 | 2 | int16_le | co2 (ppm) | `-1` |
| 47 | 2 | int16_le | tvoc_index | `-1` |
| 49 | 2 | int16_le | nox_index | `-1` |
| 51 | 4 | float32_le | pressure (hPa) | `-1001.0` |
| 55 | 1 | uint8 | battery_percentage (0–100 %) | `255` |

**Total**: 56 bytes per point.

### Parsing Notes

- All multi-byte fields are **little-endian**.
- Check each field against its sentinel value to determine validity. If a
  field equals its sentinel, treat it as "not available."
- The `timestamp` field is always valid (no sentinel).
- The `gps_fix` field is always valid (0 means no fix).

---

## Appendix: CBOR Quick Reference

CBOR (RFC 8949) is a binary serialization format. Most platforms have CBOR
libraries available:

| Platform | Library |
|---|---|
| iOS/Swift | `SwiftCBOR`, `CBORCoding` |
| Android/Kotlin | `cbor-java`, `jackson-dataformat-cbor` |
| JavaScript/TypeScript | `cbor-x`, `cbor-web` |
| Python | `cbor2` |
| Dart/Flutter | `cbor` |

### Decoding Example (Pseudocode)

```text
// Measures notification (no tag prefix)
raw_bytes = notification.value
map = cbor_decode(raw_bytes)
if "t" in map:
    temperature = map["t"]   // float32
if "co2" in map:
    co2 = map["co2"]         // uint

// History notification (has tag prefix)
raw_bytes = notification.value
tag = raw_bytes[0]
if tag == 0x00:
    map = cbor_decode(raw_bytes[1:])
    handle_control(map)
elif tag == 0x01:
    point_index = uint16_le(raw_bytes[1:3])
    point_data = raw_bytes[3:]
    parse_route_points(point_index, point_data)
```

---

## Appendix: Invalid Sentinel Values

These sentinel values indicate "data not available" in the binary route point
format. In CBOR payloads, unavailable fields are simply omitted (key absent).

| Field | Sentinel Value | Notes |
|---|---|---|
| latitude | `91.0` | Outside valid range [-90, 90] |
| longitude | `181.0` | Outside valid range [-180, 180] |
| altitude | `-10000.0` | Below minimum plausible altitude |
| temperature | `-1001.0` | Below absolute zero in any unit |
| humidity | `-1.0` | Below valid range [0, 100] |
| pm1.0 / pm2.5 / pm10 | `-1.0` | Negative PM is impossible |
| co2 | `-1` (int16) | Negative CO2 is impossible |
| tvoc_index | `-1` (int16) | Negative index is impossible |
| nox_index | `-1` (int16) | Negative index is impossible |
| pressure | `-1001.0` | Below valid atmospheric range |
| battery_percentage | `255` (uint8) | Outside valid range [0, 100] |
