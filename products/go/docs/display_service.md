# Display Service

Product-specific display service for AirGradient Go. Manages e-paper rendering
via u8g2, three-tier refresh decisions (full/fast/partial), and an async worker
task for SPI hardware refresh. The orchestrator calls the Display Service API
which returns immediately; the slow EPD refresh runs in a dedicated task.

## Files

| File | Purpose |
|---|---|
| `products/go/main/go_display.h` | `DisplayService` class, `DisplayValues` struct, `RtcDisplaySnapshot` struct, `Screen`/`Metric` enums, free functions |
| `products/go/main/go_display.cpp` | Rendering pipeline, refresh logic, worker task, display driver, RTC snapshot storage, session-screen draw routines |
| `products/go/main/go_text_wrap.h` | Pure host-testable word-wrap helper (`compute_wrapped_lines`) used by `_draw_info()` and the auto-wrapped Provisioning status line |
| `products/go/main/go_text_wrap.cpp` | Implementation of the word-wrap helper |

## Dependencies

| Dependency | Source | Usage |
|---|---|---|
| `u8g2` | `components/u8g2/` | Software rendering (fonts, primitives, framebuffer) |
| `go_types.h` | product | `OperatingMode`, `LockState` enums |
| `measures_types.h` | `airgradient-common` | `MeasuresInvalid` sentinel values |
| ESP-IDF SPI driver | ESP-IDF | `spi_device_*` API for EPD communication |
| ESP-IDF GPIO driver | ESP-IDF | CS, DC, RST, BUSY pin control |
| RTOS task/notification | `airgradient-common` | Worker task, frame-ready signaling |

## Display Hardware

**Panel:** Good Display GDEY0213B74, 128x250 px, 1-bit monochrome (SSD1680).
**Interface:** SPI half-duplex, mode 0, 4 MHz, CS managed manually.
**SPI bus:** Shared with NAND flash on SPI2_HOST; uses
`spi_device_acquire_bus(portMAX_DELAY)` for exclusive access during display
operations (ESP-IDF only supports infinite wait for bus acquisition).

All pin assignments come from `board_config.h` via `Config` struct members.

## Public API

### DisplayService::Config

| Field | Type | Default | Purpose |
|---|---|---|---|
| `spi_host` | `spi_host_device_t` | (required) | SPI bus host |
| `pin_cs` | `int` | (required) | Chip select GPIO |
| `pin_dc` | `int` | (required) | Data/command GPIO |
| `pin_rst` | `int` | (required) | Hardware reset GPIO |
| `pin_busy` | `int` | (required) | Busy status GPIO (input) |
| `clock_hz` | `int` | 4000000 | SPI clock frequency |
| `task_stack_size` | `uint16_t` | 4096 | Worker task stack size |
| `task_priority` | `uint8_t` | 4 | Worker task RTOS priority |
| `max_partial_ops` | `uint8_t` | 20 | Differential op limit before forced full GC |

### Methods

| Method | Blocking | Description |
|---|---|---|
| `init(initial, defer_refresh=false)` | Depends | Init driver + u8g2, render initial frame. See below. |
| `update(values, wait)` | No* | Render frame, signal worker. `wait=true` blocks until any prior worker job clears so the new frame queues without being dropped (does **not** wait for the new frame to finish painting; use `flush()` for that). |
| `update_sync(values)` | Yes | Render + SPI inline; for fast-path boot without worker |
| `flush()` | Yes | Spin until `_worker_busy` clears so the most-recently-queued frame is fully painted. Cheap polling with `RTOS::delay_ms(1)`, mirroring `clear()` / `stop()`. Used by every session transition that gates a fixed-duration on-screen dwell (the 500 ms STA `Connected!` hold, the 1.5 s provisioning `Connected!` hold, the `SwitchingTransport` ack, both leave-session renders) and by shutdown before the post-paint power-off dwell. MUST NOT be called from the display worker task itself. |
| `clear()` | Yes | Clear display to white via full refresh |
| `deep_sleep()` | Yes | Put SSD1680 into deep sleep mode 1 (~100 µA → <1 µA) |
| `stop()` | Yes | Stop worker task; call before ESP deep sleep |

#### `init()` — `defer_refresh` parameter

```cpp
bool init(const DisplayValues &initial, bool defer_refresh = false);
```

| `defer_refresh` | Behavior |
|---|---|
| `false` (default) | Synchronous: renders frame, performs full SPI refresh (~3 s), then starts worker. Used by `run_interactive()` and `run_fast_path()`. |
| `true` | Deferred: renders and reserves the framebuffer, marks a full refresh pending, starts the worker, and immediately signals it to run the initial refresh in the background. Returns in ~10 ms. |

When `defer_refresh=true`, `init()` returns before the SPI refresh begins.
The worker task acquires the SPI bus and holds it for the duration of the
refresh (~3 s). Any other SPI device that calls `spi_device_transmit()` during
this window (e.g. NAND flash) blocks until the worker releases the bus —
natural serialization without an explicit semaphore.

`_worker_busy` is set to `true` before the worker starts so that a concurrent
`update()` call will not corrupt the in-progress refresh.

## RTC Display Snapshot

`RtcDisplaySnapshot` is a flat struct of scalar fields (no pointers) stored in
RTC slow memory so it survives deep sleep. It is saved by
`prepare_for_sleep()` just before the device enters deep sleep, and loaded by
`run_button_wake_path()` at the very start of a button-wake boot.

```cpp
struct RtcDisplaySnapshot {
  // Sensor values
  int   co2_ppm;          float pm25_ugm3;
  float temperature_c;    float humidity_pct;
  int   tvoc_index;       int   nox_index;
  float pressure_hpa;     float altitude_m;
  // Battery
  uint8_t battery_pct;  bool is_battery_charging;  bool is_plugged_in;
  // Status flags & rendering settings
  bool gps_enabled;  bool gps_fix;  bool tracking_active;  bool ble_enabled;
  bool use_fahrenheit;  bool pm_use_usaqi;
};
```

Estimated size: ~40 bytes (well within the ~50 B budget; total RTC usage
~1.6 KB of the 8 KB available on ESP32-C5).

### Free functions

```cpp
// Save the last displayed state to RTC memory (go_display.cpp).
// Called from prepare_for_sleep() after the final display update.
void save_rtc_display_snapshot(const DisplayValues &values);

// Load the snapshot saved before the last deep sleep.
// Returns true and fills *snapshot_out when valid; false on first power-on.
bool load_rtc_display_snapshot(RtcDisplaySnapshot *snapshot_out);
```

Both functions are declared in `go_display.h` within the `#ifndef TEST_HOST`
guard, with inline no-op stubs in the `#else` branch so the orchestrator
compiles cleanly in host test builds.

The validity flag (`s_rtc_display_snapshot_valid`) is zero-initialized in RTC
memory on first power-on, so an uninitialized snapshot is never used. When
the snapshot is invalid, `build_wake_values()` fills the `DisplayValues` with
the default invalid sentinels, and the renderer shows dashes.

## Host-Compatible Types

`Screen`, `Metric`, `ListRow`, `DisplayValues`, and `RtcDisplaySnapshot` are
defined outside the `#ifndef TEST_HOST` guard and compile without ESP-IDF
headers. `DisplayService` and the snapshot free functions are
hardware-dependent and excluded from host builds (stubs provided).

### Screen Enum

| Screen | Notes |
|---|---|
| `Home` | Dashboard |
| `MainMenu` | Home with menu overlay |
| `Settings` / `SettingsChoice` / `TagList` / `About` / `Confirm` | Full-screen lists |
| `ShutdownUser` | Goodbye screen for user long-press shutdown ("Powered off" / "Hold button" / "to turn on") |
| `ShutdownDischarge` | Safety-trip shutdown for OverDischarge ("Battery critically low" / "Connect charger" / "Charge before use") |
| `ShutdownTemperature` | High-temperature safety shutdown ("Battery overheated" / "Move device to a" / "cooler location") |
| `ShutdownTemperatureLow` | Low-temperature safety shutdown ("Battery too cold" / "Move device to a" / "warmer location") |
| `PairingPasskey` | Title-as-header + 3 px divider + large 6-digit BLE passkey + hint; no status bar, no snackbar |
| `Info` | Generic single-text presentation surface (cold-boot splash, Stationary bring-up narration); no status bar, no snackbar |
| `Provisioning` | Stationary Wi-Fi provisioning page (QR + status + action rows); no status bar, no snackbar |
| `ProvisioningConfirm` | Yes / No confirmation overlay for Provisioning actions; no status bar, no snackbar |

The three Stationary setup screens (`Info`, `Provisioning`,
`ProvisioningConfirm`) form one logical "setup session". They share a
distinct refresh policy and own the full canvas (no status bar drawn).
See [Setup Session Refresh Policy](#setup-session-refresh-policy) below.

### DisplayValues

Flat snapshot of everything needed to render one frame. Built by the UI Manager
on every update; the Display Service diffs against the previous snapshot
internally to select the refresh tier (Full/Fast/Partial).

Key points:

- TVOC/NOx are `int` (SGP41 algorithm index output, not raw resistance)
- Sensor readings come from channel A (`pm_a`, `temp_hum_a`)
- `ListRow::text` is `char[48]` (owned by struct, not a pointer)
- `is_plugged_in` (`bool`): true when the charger reports external USB input; drives the plug icon in the status bar
- `wifi_enabled` (`bool`): show the Wi-Fi status-bar icon (true for the whole Stationary session)
- `wifi_connected` (`bool`): selects the connected vs disconnected Wi-Fi glyph (`WifiService::is_online()`)
- Invalid sentinels from `MeasuresInvalid`; `0xFF` for battery
- `ble_passkey` (`uint32_t`): 6-digit passkey for PairingPasskey screen
- `info_text` (`const char *`): caller-owned ASCII string for `Screen::Info`; null or empty renders a blank canvas
- `provisioning_status` (`const char *`): transport-aware status text for `Screen::Provisioning`; auto-wrapped to at most 2 lines so long strings (`Connected! 192.168.x.y`, `Connect failed - try again`) stay inside the canvas
- `provisioning_transport` (`uint8_t`): `ProvisioningTransport` value driving transport-specific labels and captions
- `provisioning_connected_ip` (`uint32_t`): network-byte-order IPv4 (low byte = first octet) matching `WifiGotIpCallback`, `WifiStaticIpConfig`, and `format_ipv4_be`. Non-zero overrides the status line with the formatted `Connected!` text via `UIManager::provisioning_status_text()`
- `provisioning_confirm_kind` (`uint8_t`): `0` switch transport, `1` cancel setup
- `provisioning_confirm_index` (`uint8_t`): `0` No (default), `1` Yes
- `provisioning_ap_ssid` (`const char *`): captive-portal AP SSID (`airgradient-<MAC>`) rendered as the Wi-Fi instruction line
- `provisioning_ap_password` (`const char *`): captive-portal AP password rendered as the Wi-Fi instruction line (sourced from `UIManager::Config::ap_password`, matches `WifiService::Config::ap_password`)
- `qr` (`const AirgradientProvisioning::QrCode *`): borrowed pointer to the QR matrix shown on the session screens that render one — the Provisioning page (UIManager re-encodes on session entry and transport switch: BleOnly → companion-app URL, WifiOnly → `WIFI:` join descriptor) and the Getting Started page (setup landing-page URL). Those screens are mutually exclusive, so one pointer serves both. Null or empty matrix skips the QR area

## Architecture

### Framebuffer Ownership

| Buffer | Size | Context | Purpose |
|---|---|---|---|
| `_render_buf[4096]` | 4096 B | Orchestrator, then worker | u8g2 render target and immutable worker source |
| DMA bounce buffer | 4096 B | Display driver | DMA-capable source for polling SPI transfers |

`update()` claims the framebuffer before rendering and transfers ownership to
the worker through its task notification. Until the worker clears
`_worker_busy`, another non-waiting update is rejected and a waiting update
blocks. This keeps the framebuffer immutable while Full and Fast refreshes
read it for both SSD1680 RAM planes.

Partial updates use a direct full-width framebuffer span. Body updates pass
bytes 288–3999 (rows 18–249, 3712 bytes); setup-session updates pass bytes
0–3999 (the full 128×250 canvas). The driver copies either span into the DMA
bounce buffer before its synchronous SPI transfer, so no intermediate region
buffer is required.

### Async Worker Task

The worker task waits on an RTOS task notification, then drives the SPI
hardware. The orchestrator signals frame-ready via `RTOS::task_notify_give()`.
An atomic `_worker_busy` flag owns the framebuffer across tasks
(`wait=false` returns false if busy; `wait=true` waits until it can claim the
buffer).

In the deferred-refresh mode (`defer_refresh=true`), `init()` itself signals
the worker with the initial full-refresh job before returning. This means the
first `RTOS::task_notify_give()` comes from the main task inside `init()`,
not from `update()`. The worker processes this exactly like any other full
refresh, acquiring and releasing the SPI bus normally.

While waiting on the SSD1680 `BUSY` line, the worker polls every `BUSY_POLL_MS`
(10 ms, one scheduler tick). This must stay `>= 1` tick: a sub-tick delay
degrades to `vTaskDelay(0)` (a bare yield), so the priority-4 worker would
busy-spin and starve the lower-priority (eg. LED, buzzer) tasks for the whole
multi-second refresh.

### Display Driver

The SSD1680 display driver is file-local (anonymous namespace in
`go_display.cpp`). It follows the SPI protocol documented in
`UI-IMPLEMENTATION.md` sections 2.3-2.4:

- `driver_init()` — GPIO setup, SPI device attachment, DMA bounce buffer
- `driver_hw_init_full()` — Full SSD1680 init (SW reset, gate config, RAM window)
- `driver_set_basemap()` — Write frame to both RAM planes + full update trigger (`0xF7`)
- `driver_hw_init_fast()` — Fast refresh init: SW reset + temperature override trick (forces 100 °C OTP LUT for faster waveform)
- `driver_fast_write()` — Write full frame to both RAM planes (basemap coherence for subsequent partials)
- `driver_fast_commit()` — Trigger fast update (`0xC7` waveform, non-differential, no flash)
- `driver_part_begin/write_region/commit()` — Partial update protocol (`0xFF` waveform, differential)
- `driver_deep_sleep()` — SSD1680 deep sleep mode 1
- `driver_bus_acquire/release()` — Exclusive SPI bus access

## Three-Tier Display Refresh

The display uses three refresh tiers to balance image quality, speed, and
user experience. See `products/go/specs/display_refresh_tiers.md` for the
full design rationale.

### RefreshMode Enum

```cpp
enum class RefreshMode : uint8_t {
    Full,     ///< Full GC waveform (flash). Resets basemap in both RAM planes.
    Fast,     ///< Full-screen 0xC7 waveform (no flash). Temperature-override trick.
    Partial,  ///< Body-only differential 0xFF (no flash). Writes body region only.
};
```

### Refresh Tiers

| Tier | SSD1680 Waveform | Duration | Flash | When |
|---|---|---|---|---|
| **Full** | `0xF7` (GC, both RAM planes) | ~2–3 s | Yes | Init from deep sleep; anti-ghosting after 20 differential ops |
| **Fast** | `0xC7` (non-differential, 100 °C OTP LUT) | ~1–1.5 s | No | Transitions to/from PairingPasskey or any `Shutdown*` screen; navigable screen transitions with header change |
| **Partial** | `0xFF` (differential, body region only) | ~0.3–0.5 s | No | Menu navigation between navigable screens (header unchanged); same list screen updates |

### Decision Logic

The `update()` method selects the refresh mode using this priority:

1. **Full** — crossing the setup-session boundary (any non-session screen
   ↔ `Info` / `Provisioning` / `ProvisioningConfirm`). Resets
   `_diff_count` and `_menu_exited` so the next session starts with a
   fresh partial budget.
2. **Partial** — in-session transition (both previous and next are
   session screens). The partial-op counter is **not** consulted inside
   the session. Covers `Info` text updates, Provisioning status updates,
   `Provisioning ↔ ProvisioningConfirm`, and No ↔ Yes toggling.
3. **Partial** — menu-navigation transition (either previous or next is
   a menu-navigation screen, and the next screen is not a `Shutdown*`
   variant or PairingPasskey)
4. **Full** — `_diff_count >= max_partial_ops` (anti-ghosting, default 20)
5. **Fast** — `_menu_exited` is set (post-menu cleanup)
6. **Partial** — both screens "navigable" and header unchanged, OR same
   list screen
7. **Fast** — everything else (fallback)

A screen is **navigable** if the user reaches it through normal menu
interaction: Home, MainMenu, Settings, SettingsChoice, TagList, Confirm,
About. PairingPasskey and the `Shutdown*` variants are not navigable —
transitions involving them always use Fast for clear visual indication.

A screen is **menu-navigation** if it is navigable and not Home (MainMenu,
Settings, SettingsChoice, TagList, Confirm, About). Transitions where either
side is a menu-navigation screen force body-only Partial regardless of header
changes or anti-ghosting counter. This keeps menu interaction responsive and
flash-free. The anti-ghosting Full refresh is deferred, not skipped.

`_menu_exited` is set during any menu-navigation Partial and cleared when a
full-screen refresh (Full or Fast) executes. After the user leaves the menu,
the first non-menu update triggers Fast (or Full if the anti-ghosting
threshold was reached during the menu session) to clean up accumulated
artifacts and refresh the status bar.

All non-`Shutdown*` screens share the same status bar at Y=0..17. Partial
updates write the body region only (Y=18..249, 232 px height, full 128 px
width), so the status bar is physically unchanged on the display. Header
changes during menu navigation and same-list-screen interactions are silently
deferred; the header self-corrects at the post-menu cleanup refresh or the
next Full refresh (anti-ghosting).

### Anti-Ghosting Counter

`_diff_count` counts all differential operations (both Fast and Partial).
Both waveform types accumulate ghosting equally on the GDEY0213B74 panel.
The counter resets to 0 on Full refresh and uses saturating increment on Fast
or Partial (capped at `UINT8_MAX` to prevent wrap during long menu sessions).
`Config::max_partial_ops` (default 20) controls the limit.

During menu navigation, the anti-ghosting threshold may be reached or
exceeded, but the menu-navigation rule (tier 1) overrides it. The counter
keeps incrementing. When a later non-menu update runs, `_diff_count >=
max_partial_ops` (tier 2) promotes it to Full — an even stronger cleanup
than the Fast from `_menu_exited`.

### Fast Refresh: Temperature Override

The Fast tier uses a vendor-documented technique for the GDEY0213B74 panel:

1. SW reset clears all SSD1680 registers
2. Read the built-in temperature sensor, then override the temperature
   register to 100 °C (`0x64`)
3. Reload the OTP LUT — the controller selects a faster, more aggressive
   waveform designed for high-temperature operation
4. Re-establish display geometry (data entry mode, RAM windows, cursor)
5. Write full frame to both RAM planes (0x24 and 0x26 — ensures basemap
   coherence for subsequent Partial refreshes)
6. Trigger with `0xC7` (skips LUT loading since it was done in step 3)

Writing both RAM planes ensures that after a Fast refresh, the basemap
(RAM 0x26) matches the displayed content. Subsequent Partial refreshes
(which diff 0x24 vs 0x26) produce correct results.

### Decision Matrix

| Condition | Refresh Mode |
|---|---|
| Crossing the setup-session boundary (non-session ↔ Info / Provisioning / ProvisioningConfirm) | Full |
| In-session transition (Info ↔ Provisioning ↔ ProvisioningConfirm, including same-screen text updates) | Partial |
| Menu-navigation transition (prev or next is menu-nav screen) | Partial |
| Menu-navigation transition, even if `diff_count >= max_partial_ops` | Partial |
| Menu-navigation transition with header change | Partial |
| Transition **to** any `Shutdown*` variant or PairingPasskey from menu | Fast/Full (existing) |
| `diff_count >= max_partial_ops` (non-menu, non-session) | Full |
| Post-menu cleanup (`_menu_exited` set, non-menu update) | Fast |
| Both navigable, header unchanged (non-menu, non-session) | Partial |
| Same list screen (any header state) | Partial |
| Screen transition involving PairingPasskey (non-menu) | Fast |
| Screen transition involving any `Shutdown*` variant (non-menu) | Fast |
| Navigable screen transition, header changed (non-menu, non-session) | Fast |

### Setup Session Refresh Policy

`Screen::Info`, `Screen::Provisioning`, and `Screen::ProvisioningConfirm`
are treated as one logical setup session. The refresh policy has two
rules layered on top of the general matrix:

- **Crossing the session boundary in either direction forces Full.**
  Any non-session screen entering a session screen, or any session
  screen returning to Home / Portable, runs a full GC waveform. This
  prevents the prior layout from ghosting under the new one. The
  partial-op counter and `_menu_exited` flag are reset at the boundary.
- **All intra-session transitions are Partial, regardless of layout
  change.** This includes `Info` text updates
  (`Connecting to saved Wi-Fi...` → `Trying default Wi-Fi...` →
  `Connected!\n<ip>`), Provisioning status updates, the
  `Provisioning ↔ ProvisioningConfirm` overlay, the No ↔ Yes toggle in
  the confirmation overlay, and the in-session `Info → Provisioning`
  jump. The partial worker writes the **full canvas** (y = 0..249) for
  session screens, so even visually-disjoint layouts (Info's centered
  text block versus Provisioning's full-canvas QR layout) clear
  cleanly without falling back to the Full waveform's ~3 s flash.

Non-session partials still write only the body region (y = 18..249,
232 px tall) to preserve the status bar without rewriting it. The
selection between body-only and full-canvas partial happens in the
worker loop based on `is_session_screen(_prev_values.screen)`.

### Session-Screen Drawing

Session screens skip `_draw_status_bar()` and `_draw_snackbar()`:

- `_draw_info()` renders a centered word-wrapped text block in the
  body region (y ≥ 18 clamp). Uses `compute_wrapped_lines()` with a
  `u8g2_GetStrWidth` closure as the `StrWidthFn`, ASCII only, font
  `u8g2_font_helvB12_tf`. Multi-line text honours explicit `\n` as
  hard breaks and word-wraps each paragraph at the last space
  boundary that fits.
- `_draw_provisioning()` renders the title (`Connect to Wi-Fi`),
  transport-specific QR code (drawn from `v.qr` —
  encoded by UIManager: companion-app URL for BLE, Wi-Fi `WIFI:` join
  descriptor for the captive-portal AP), caption, instructions
  (SSID + password from `v.provisioning_ap_ssid` /
  `v.provisioning_ap_password` in Wi-Fi), status line (auto-wrapped to
  at most 2 lines via the same word-wrap helper), helper text, and
  two action rows. Labels switch on `provisioning_transport`. Module
  pixel size and quiet zone are product-local; the QR matrix data
  comes from `airgradient-provisioning`.
- `_draw_provisioning_confirm()` renders the question text from
  `v.rows[0].text` (provided by `UIManager::populate_provisioning_confirm_rows()`)
  and two filled / framed buttons (`No` index 0, `Yes` index 1)
  driven by `provisioning_confirm_index`.

### Display Update Suppression

Background display updates are suppressed in two cases:

- **Menu-navigation screens** (MainMenu, Settings, SettingsChoice,
  TagList, Confirm, About). Background events — sensor data, BLE
  connect/disconnect/auth/config writes, BMS charging-status changes,
  and snackbar expiry — do not trigger display updates while the user
  is interacting with a menu.
- **Setup session screens** (`Info`, `Provisioning`,
  `ProvisioningConfirm`). The orchestrator's
  `_setup_session_active` flag short-circuits
  `request_background_display_update()` entirely so sensor / BMS /
  BLE-status events that arrive during the session do not race the
  explicit `update_display(wait=true)` calls that drive session state
  transitions.

Both cases are enforced by the orchestrator's
`request_background_display_update()`, which checks
`_setup_session_active` first, then delegates to
`UIManager::is_on_menu_screen()`. Background data is still cached
internally and pushed to BLE clients; only the e-paper refresh is
suppressed. The display catches up on the next user-initiated repaint
or on the next deliberate session render. See `docs/orchestrator.md`
for the full call-site classification.

## Rendering Pipeline

Frame assembly order:

1. Clear buffer to 0xFF (white)
2. Set draw color to 0 (black)
3. If any `Shutdown*` variant or `PairingPasskey`: dedicated draw (no status bar, no snackbar)
4. Else: `draw_status_bar()` + screen-specific draw + `draw_snackbar()`

Screen dispatch:

- **FG learning dashboard:** when `DisplayValues::show_fg_dashboard` is set, the
  pipeline routes to the dedicated full-canvas `_draw_fg_learning_dashboard()`
  (no status bar, no snackbar) before any other screen dispatch. It renders the
  `FgLearningDashboardData` aggregate — phase banner, cycle, SOC / voltage /
  signed current, FCC drift, FG temperature, and the `FC` / `CHG` / `DSG` /
  `QMAX_UP` / `RES_UP` / `OCVTAKEN` flags — or `FG: NO DATA` when invalid. Built
  directly by `FgLearningRunner` (no `UIManager`). See
  [`fg_learning.md`](fg_learning.md).
- **Home:** Hero blocks (PM2.5, CO2 with dual-font labels centered via
  `u8g2_GetStrWidth()`) + 3-row grid (Temp/Humidity, TVOC/NOx or
  Min/Max, Pressure/Altitude or chart). Grid dividers span full 128 px;
  1st divider is always 2 px thick, 3rd is 2 px thick when chart is
  visible. Selection rects for PM2.5, CO2, Temp, and Humidity use full
  128 px width. Logo removed from home page (kept for display-off only).
- **MainMenu:** Home screen (metric cleared to None) + overlay at y=162.
  The 2 px-thick 1st grid divider is preserved as the menu top border.
  Menu rows use full 128 px-wide selection rects.
- **Settings/SettingsChoice/TagList/Confirm/About:** Full-screen list with
  full 128 px-wide selection rects and vertically centered text. A
  separator line between the header rows (Exit/Back) and content rows
  uses a 2 px content offset to avoid touching.
- **PairingPasskey:** "Bluetooth Pairing" title (`helvB14_tf`, baseline
  y=35), 3 px-thick divider at y=49 (shared chrome with `Shutdown*`),
  6-digit passkey (`logisoso32_tr`, baseline y=145), and "Enter on
  phone" hint (`helvR12_tr`, baseline y=215). No status bar, no
  snackbar.
- **ShutdownUser / ShutdownDischarge / ShutdownTemperature /
  ShutdownTemperatureLow:** Unified template — `"AirGradient"` brand header
  (`helvB14_tf`, baseline y=34),
  3 px-thick divider at y=49, reason-specific icon centred at
  (`SCREEN_W / 2`, y=94), and a title/action/detail text block
  (`helvB14_tf` titles at y=151/169, `helvR12_tr` action at y=198,
  `helvR08_tr` detail at y=214). Icons are drawn from u8g2 primitives
  (power circle, battery body, thermometer with heat-wave lines). No
  status bar, no snackbar. The renderer dispatches on the Screen
  variant. The temperature icons use heat-wave lines for high temperature and
  a snowflake for low temperature.

### Fonts

| Font | Usage |
|---|---|
| `u8g2_font_logisoso32_tr` | Hero section values (PM2.5, CO2), Pairing passkey |
| `u8g2_font_logisoso16_tr` | Hero section metric name labels |
| `u8g2_font_helvB14_tf` | Shutdown brand header / title lines, Pairing title |
| `u8g2_font_helvR12_tr` | Hero section unit labels, Shutdown action line, Pairing hint |
| `u8g2_font_helvR08_tr` | Grid cell labels, About page info text, Shutdown detail line |
| `u8g2_font_helvB08_tf` | Grid cell values, About page title |
| `u8g2_font_6x10_tr` | Menu/list row text, logo text |
| `u8g2_font_siji_t_6x10` | Battery glyph, plug glyph (57410), Wi-Fi glyphs (connected 57419 / disconnected 57879), GPS glyph |
| `u8g2_font_open_iconic_all_1x_t` | Lock icon (glyph 0xCA) |
| `u8g2_font_open_iconic_thing_1x_t` | Unlock icon (glyph 0x44) |

### Status Bar — Wi-Fi Icon

The Wi-Fi icon is shown for the whole Stationary session (`wifi_enabled`
= Stationary mode), so a dropped link stays visible instead of vanishing.
`wifi_connected` (`WifiService::is_online()`) selects the glyph: connected
(57419) when online, disconnected (57879) otherwise. In Portable mode the
icon is hidden. `_is_header_changed()` includes `wifi_connected` so the
connect/disconnect transition triggers a status-bar redraw.

### Status Bar — Battery and Plug Icons

The battery icon is right-pinned at `BATTERY_X` (116). Three visual
states:

| State | Icons |
|---|---|
| On battery | Battery level glyph only |
| Charging | Charging bolt glyph (57914) |
| Plugged, not charging | Plug glyph (57410) + battery level glyph |

The plug icon appears left of the battery glyph when
`is_plugged_in && !is_battery_charging` (full-charge pause or charge
termination done while USB is connected).

When the plug icon is visible and tracking is active, the tracking dot
shifts left to avoid overlapping the plug glyph.

`_is_header_changed()` includes `is_plugged_in` so plug/unplug
transitions trigger a status bar redraw.

### Value Formatting

| Metric | Rules |
|---|---|
| PM2.5 | <100: one decimal; >=100: integer; >999: "999+"; USAQI: EPA breakpoints |
| CO2 | Integer ppm; >9999: "9999+" |
| Temperature | One decimal, C or F; conversion: `F = C * 9/5 + 32` |
| Humidity | Integer percent |
| TVOC/NOx | Integer (index values) |
| Pressure | Integer hPa; >9999: "9999+ hPa" |
| Altitude | Integer meters; >9999: "9999+ m" |
| Invalid | "-" for any metric |
