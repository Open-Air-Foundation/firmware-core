# AirGradient Go Operation and Measurement Logging

> **Status: design specification; not implemented.** The product behavior below
> records the agreed design. Open implementation decisions are listed at the end.
> After implementation, update the owning references linked below and follow the
> [spec lifecycle](../../../docs/GUIDE.md#doc-lifecycle).

AirGradient Go will remove selectable Operating Modes. Tracking state,
connectivity, and interaction will determine runtime behavior. A separate
Log Measurements setting will provide persistent measurement history without
location, with bounded files and BLE download.

## Problem

Portable, Stationary, and Energy Saving currently bundle measurement cadence,
radio availability, GPS, sleep, and recording policy. Users must choose a mode
to obtain features that should be independently controlled. Removing the menu
alone would leave these constraints embedded in boot and service lifecycles.

The existing chart cache is temporary, while persistent history belongs to
tracking sessions. Go needs an optional persistent measurement log independent
of route tracking, without changing temporary chart behavior.

Current behavior is documented in [Architecture](../ARCHITECTURE.md),
[Orchestrator](../docs/orchestrator.md), [Settings](../docs/settings.md), and
[Storage Service](../docs/storage_service.md). Those references remain the
authority for the implementation until this specification is implemented.

## Goals

- Remove selectable Operating Mode and its runtime policy dependencies.
- Derive awake, acquisition, and correction behavior from tracking and Wi-Fi.
- Preserve Recording and Paused as active tracking-session states.
- Add optional session auto-stop without a persistent duration preference.
- Add Log Measurements, disabled by default, with rolling files and BLE export.
- Bound inactive GPS sleep to five minutes, servicing it with ESP wakeups.
- Preserve usable provisioning cancellation and explicit storage-full feedback.
- Preserve measurement validity, raw history, and temporary chart contracts.

## Non-Goals

- A replacement mode selector or a third measurement interval.
- A new general Status page or storage-status BLE flags.
- Continuous background logging during a tracking session, including Pause.
- Automatic deletion of tracking sessions or GPS fields in measurement logs.
- Historical backfill from the temporary chart cache.
- Redesigning cloud upload, adding HTTP log download, or specifying phone UI.

## Design

### 1. Runtime State and Sleep

Remove Portable, Stationary, and Energy Saving as selectable modes. Their names
in this specification identify existing behavior to reuse, not new settings.

Tracking is active when its state is Recording or Paused. Log Measurements is
a durable preference; it is not a tracking session. Wi-Fi running means the
runtime is maintaining the radio for connection, reconnection, or provisioning,
not merely that a saved preference is enabled.

| Tracking State | Wi-Fi Running | Runtime Policy | Measurement Interval |
|---|---|---|---|
| Recording | Either | Stay awake; append tracking records | Tracking |
| Paused | Either | Stay awake; append neither tracking nor measurement logs | Tracking |
| Idle | Yes | Stay awake for Wi-Fi; log scheduled measurements if enabled | Normal |
| Idle | No | Use Energy Saving acquisition and sleep when eligible; log if enabled | Normal |

User interaction and admitted BLE transfers can temporarily prevent sleep.
GPS Always On will also keep Go awake for continuous GPS acquisition, without
selecting the Tracking measurement interval or continuous gas/correction policy.
Unlocking alone will not select the Tracking interval or continuous gas policy.
Preserve existing measurement readiness, display completion, storage sync,
safety, and lock gates before entering sleep. Log Measurements alone will not
prevent sleep. User shutdown and charging-only operation remain distinct from
measurement-cycle sleep.

### 2. Settings and Measurement Intervals

The names in this table are user-facing concepts, not finalized wire or NVS keys.

| Setting or Parameter | Default | Purpose |
|---|---|---|
| Enable Wi-Fi | Disabled, including migration from every old mode | Permit Wi-Fi connection and setup |
| Local Server | Preserve existing preference; currently enabled | Local API/discovery and Wi-Fi power-saving policy |
| Log Measurements | Disabled | Persist scheduled measurements while tracking is Idle |
| Normal interval | Preserve Energy Saving value; otherwise its existing default/range | Measurement cadence without a tracking session |
| Tracking interval | Preserve Portable/Stationary value; otherwise its existing default/range | Measurement cadence while Recording or Paused |
| Session Stop after | Off for each new session unless supplied at Start | Optional auto-stop duration; not a durable setting |
| GPS Mode | Preserve existing preference and default | Always Off, On When Tracking, or Always On |
| Maximum inactive GPS sleep | 300 seconds | Bound receiver sleep and tracking-start wake delay |
| Measurement-log sync interval | Reuse tracking policy; currently 30 seconds | Sync on an append when the interval has elapsed |
| Maximum records per log file | To be selected before implementation | Bound each measurement-log file |
| Storage warning and stop reserve | To be selected before implementation | Warn before exhaustion and retain space for finalization |
| BLE export inactivity timeout | To be selected before implementation | Release abandoned connections and protected files |

Go already stores two intervals, with one UI row editing the current mode's
value. Expose Normal interval and Tracking interval explicitly so both can be
configured before tracking starts. Wi-Fi and logging do not add intervals.
Map the old Energy Saving interval to Normal and the old Portable/Stationary
interval to Tracking. Preserve both numeric preferences, their existing ranges
and defaults, saved credentials, Local Server preference, and GPS preference.
Initialize the new Wi-Fi preference to Off regardless of the old Operating Mode.
This initialization applies once on migration, not on subsequent boots after
the user enables Wi-Fi. New devices also default to Wi-Fi Off.

Persist numeric preferences as their values, not menu enum indices. Existing
measurement intervals remain numeric seconds to preserve sub-minute values;
the new tracking duration is expressed in numeric minutes.

### 3. Wi-Fi, Provisioning, and Local Server

Enable Wi-Fi and Local Server will be independent preferences. Preserve the
existing cloud enable/disable and configuration-authority controls. Local
Server will require Wi-Fi connectivity; keep its saved preference when Wi-Fi
is disabled.

Use the existing initial-connection and runtime-reconnection distinction:

1. On cold boot with Wi-Fi enabled and saved credentials, attempt connection.
   If initial connection fails, show provisioning.
2. When the user enables Wi-Fi without saved credentials, show provisioning.
   With saved credentials, attempt connection and use the initial-failure path.
3. With Wi-Fi already enabled but no credentials at boot, use the same
   provisioning path rather than attempting an empty network configuration.
4. After obtaining a connection, runtime disconnections retry in the background
   using the existing retry delays. Keep Wi-Fi on; do not sleep between retries
   or automatically reopen provisioning.
5. Cancelling provisioning returns Home, disables Wi-Fi durably, and preserves
   saved credentials. It does not cancel a tracking session.

Do not treat a measurement or GPS-maintenance wake as a new cold boot that
repeatedly enters setup. While Wi-Fi runs, preserve sensing and recording
responsibilities; provisioning must not silently end a tracking session.

Local Server enabled will disable Wi-Fi power saving for reliable local access.
Local Server disabled will enable Wi-Fi power saving while leaving Wi-Fi and
eligible cloud activity running. This is modem power saving, not ESP deep sleep.
Preserve existing local/cloud configuration authority and provisioning listener
handoff contracts from [Local Server](../docs/local_server.md) and
[Wi-Fi Service](../docs/wifi_service.md).

### 4. BLE Availability

Connectable BLE will be available when Go is unlocked or a tracking session is
active, including Paused. Preserve authenticated access for the Go data service.
Keep brief measurement broadcasts on eligible measurement wakes; these remain
distinct from the connectable service.

When Go locks without tracking, disconnect an idle BLE connection and stop
ordinary connectable availability. An already admitted history download or
firmware update may keep its existing connection until completion, failure,
disconnect, or timeout. Do not admit successive new operations merely to extend
this exception. Do not disconnect a download before its missing-record recovery
phase ends. Firmware updates must follow the existing safe OTA lifecycle.

Reevaluate the same rule if tracking stops while Go is locked. An abandoned
export must not prevent sleep or protect a rolling-log file indefinitely.
Provisioning radio ownership must satisfy the unlock/setup UI and authentication
contracts; removal of mode-based BLE ownership requires explicit coordination.

### 5. Tracking Lifecycle and Auto-Stop

Preserve Start, Pause, Resume, and Stop, with Recording and Paused both active.
Starting tracking must successfully establish its storage before reporting
Recording. Failed Start leaves Idle; failed Resume leaves Paused. Tracking will
save sensor records before GPS obtains a valid fix, with unavailable location
represented using the existing validity rules.

Stop after will be an option in the active Tracking menu, available in Recording
and Paused. Starting locally will not require a duration prompt. Each session
starts without auto-stop unless its Start request includes a duration.

Offer Off, 15 minutes, 30 minutes, 1 hour, 2 hours, 4 hours, and 8 hours in the
menu. Represent these as numeric minutes: 0, 15, 30, 60, 120, 240, and 480, with
0 meaning Off. BLE may supply any whole-minute duration from 1 through 1440
(24 hours); omission at Start means Off. Do not restrict BLE to menu presets.
The selection is session state, not an NVS preference for future sessions.
Any duration encoded in persistent metadata must likewise use numeric minutes,
not an enum index; metadata must not reactivate auto-stop in a new session.

- Choosing a duration while tracking is active sets a deadline from that moment.
  Choosing 30 minutes after 40 minutes of tracking grants 30 more minutes.
- Show remaining time and allow replacing the duration or cancelling auto-stop.
- Pause does not suspend the countdown or extend the deadline.
- BLE Start may include a duration counted from successful session start.
- Expiry ends Recording or Paused through the normal Stop lifecycle.
- Manual Stop remains available. Session end clears its auto-stop state.
- Returning to Idle resumes eligible measurement logging and reevaluates sleep,
  BLE, GPS, cadence, and sensor policy without changing saved feature settings.

Use elapsed time for the countdown, independent of UTC corrections. After a
cold boot, software restart, crash, or watchdog reset, tracking remains stopped
and its countdown is cleared. Preserve successfully saved records; do not reopen
the session automatically. Ordinary measurement/GPS-maintenance deep-sleep
wakes are not unexpected restarts and continue their normal Idle flow. No
automatic tracking recovery feature is introduced.

### 6. Measurement Acquisition and Corrections

Select continuous gas-index acquisition when tracking is active or Wi-Fi is
running, including reconnection and provisioning. Use the existing gas sampler's
cadence independently of the Normal or Tracking measurement interval.

When neither condition holds, preserve current Energy Saving acquisition and
raw-gas placeholder behavior. A brief BLE connection, unlock, or GPS-only wake
does not by itself enable continuous gas acquisition. Preserve invalid-field
handling and valid-sample counts; unavailable sensors remain optional.

Apply saved temperature/humidity corrections under the same tracking-or-Wi-Fi
condition. Otherwise bypass them as Energy Saving does today. Keep the existing
PM correction policy. Do not erase saved correction preferences on transitions.
Tracking and measurement logs and their BLE exports will store raw measurements;
correction and presentation remain separate from persistent records.

See [Sensor Producer](../docs/sensor_producer.md) and
[Measurement Corrections](../docs/measurement_corrections.md) for existing
acquisition, warmup, and presentation contracts.

### 7. Measurement Logging and Temporary Charts

Log Measurements will save each scheduled measurement without location when
enabled and tracking is Idle. Temporary charts will continue using their own
cache regardless of logging. Do not populate charts from historical log files
after power-off or backfill persistent logs from cached chart samples.

Suspend measurement logging for the entire tracking session, including Paused.
Pause therefore writes neither kind of persistent record. Keep live readings,
temporary chart updates, and session GPS policy as today. The saved logging
preference remains unchanged; logging resumes after session end if enabled.
Disabling logging stops future appends without deleting existing files.

A log segment is simply one bounded measurement-history file. Rotate by record
count, not calendar date. Use fixed-size records within a versioned format so a
record-count limit also bounds file size. Include a header with format version,
persistent sequence, and boundary reason. Never infer chronology from filenames
containing an unavailable wall-clock date.

Create a new file on the first measurement after:

- Reaching the current file's record limit.
- A cold boot.
- A tracking session ends and logging resumes.
- Logging is disabled and subsequently enabled again.

At tracking start, sync and close the current measurement-log file. Do not
create empty successor files merely because a boundary occurred. Ordinary
deep-sleep wakes must reopen and continue the same file. File sequence plus
record position will preserve ordering without valid time; record timestamps
must distinguish unknown time from a valid date. Boundary metadata indicates
discontinuity, not a known duration of the gap.

Reuse tracking's durability policy: sync creation and the first append, then
sync on subsequent appends when the sync interval has elapsed. This is not a
background flush timer or a guaranteed maximum unsynced age. Also sync before
sleep, file rotation, logging disable, tracking start, and controlled shutdown.
Keep buffers bounded. A successful append is not proof of durability until
sync succeeds. Sleeping after each measurement will normally sync each record.
Handle a partial trailing record after interruption before accepting new appends.

### 8. Storage Exhaustion and Feedback

For measurement logging, reclaim the oldest eligible measurement-log files when
space is needed. Never automatically delete tracking sessions. Do not delete
the current writer or a file protected by export; rotate or choose another
eligible file as appropriate. If no eligible file can provide enough space,
stop appending and report the storage condition. Preserve existing data.

Tracking will stop automatically when storage cannot safely accept more records.
Retain a space reserve for flushing and closing; do not wait for every byte to
be exhausted. Reject a new Start or Resume when capacity is insufficient. Do not
silently change tracking into rolling history or reclaim files on its behalf.
Distinguish storage exhaustion from unrelated write or sync failures.

Show a persistent warning icon on Home while storage is almost full or full.
When an active tracking session stops because storage is full, show a sticky
"Tracking stopped: storage full" snackbar. Clear it on unlock. If Go is already
unlocked when the warning appears, the next button interaction acknowledges it.
Keep the Home icon while the storage condition remains. The snackbar must survive
ordinary Home repaint and sleep preparation until that acknowledgement, rather
than disappearing with a normal snackbar timeout. No general Status page is
introduced.

Report the tracking stop reason through BLE. Do not add storage-full or
almost-full flags: clients can derive fullness from the existing flash-used
and flash-size values. Keep those values representative of total filesystem
usage, including both tracking and measurement logs. A client-derived threshold
need not exactly reproduce an internal finalization reserve.

### 9. GPS Sleep and Maintenance Wakes

For inactive GPS, replace the Energy Saving maximum-duration sleep with sleep
ending at the earlier of the next measurement deadline or five minutes. The
maximum is 300 seconds. Use retained elapsed-time deadlines across ESP sleep.

When otherwise eligible to deep-sleep, schedule the ESP wake for the earlier of
the measurement and GPS-maintenance deadlines. A GPS-only wake will service
receiver sleep without taking a measurement, appending records, broadcasting a
measurement, or refreshing the display. If measurement is also due, combine the
work. If GPS is absent or no sleep deadline was established, do not manufacture
a periodic maintenance requirement for a nonexistent receiver.

Interactive operation will service the same GPS deadline without a reboot.
Preserve button-wake handling and thermal/battery safety during maintenance.
Starting tracking will stop requesting inactive sleep and permit acquisition
when the current receiver sleep ends. Sensor recording can start immediately.
Receiver sleep can delay acquisition by up to five minutes, plus time to obtain
a fix; receiver failures can take longer. No mode-change power reset is required.

Retain all existing GPS settings: Always Off, On When Tracking, and Always On.
On When Tracking keeps GPS active for Recording and Paused; Always Off permits
tracking without location. Always On keeps GPS acquisition active even without
tracking and therefore prevents measurement-cycle ESP deep sleep. It does not
enable measurement logging, change the interval, or enable continuous gas
acquisition and temperature/humidity corrections by itself. Apply the bounded
sleep policy only when GPS is inactive, retaining explicit GPS Test overrides.

Verify receiver timing on hardware
rather than assuming an ESP wake proves the receiver is ready. See
[GPS Service](../docs/gps_service.md) and
[Power Management](../docs/power_management.md).

### 10. BLE Measurement-Log Download

Reuse tracking history's authenticated list, start, record transfer, missing-record
recovery, and end pattern. Distinguish measurement-log files from tracking
sessions and use an explicitly versioned measurement-only wire format without
GPS fields. Preserve existing tracking-history compatibility unless an explicit
protocol migration is agreed.

List each measurement file's sequence, boundary reason, record count, and
start/end timestamps when known. Files with unknown time must remain listable
and downloadable in sequence order.

For export of the current writer, sync it and capture a fixed record count at
download start. Export only that snapshot, including for retransmission; later
appends remain available to a subsequent download. Fail export setup if the
snapshot cannot be established reliably.

Protect an exported file from rolling deletion through transfer and missing-record
recovery. Release protection on End, failure, disconnect, or inactivity timeout.
If storage runs out while a file is protected, reclaim another eligible file or
report insufficient space. Never silently remove records from an active export.
Coordinate storage ownership and BLE callbacks through existing service boundaries.

See [BLE History](../go_ble_client.md#8-history-characteristic) for the current
protocol and [BLE Service](../docs/ble_service.md#history-export) for its existing
blocking export behavior. Transfers must allow tracking deadlines, input, and
safety handling to make progress; existing blocking work cannot be assumed safe
for the new auto-stop contract without verification.

## Implementation Plan

1. Resolve the bounded choices in Open Questions and define settings migration,
   capability discovery, file metadata, and BLE payloads. Audit existing mode
   checks, boot routes, setup/onboarding exits, diagnostics, cloud, OTA, and RTC
   state so removing the enum does not leave hidden lifecycle dependencies.
2. Implement host-testable runtime predicates and two-interval selection. Migrate
   settings and replace mode-based decisions in Go orchestration and UI.
3. Separate Wi-Fi enable/provisioning/cancellation, BLE availability, and local
   server control. Establish single ownership of shared BLE and HTTP resources;
   verify simultaneous tracking, BLE, and Wi-Fi operation and OTA eligibility.
4. Implement bounded GPS sleep and a minimal GPS-maintenance wake path through
   existing board, driver, and RTOS interfaces. Preserve watchdog and safety work.
5. Implement session auto-stop, menu controls, BLE duration input, and stop reasons.
   Ensure event-loop scheduling and export work cannot starve the deadline.
6. Implement versioned measurement-log files, boundary metadata, bounded buffering,
   sync/recovery, rolling reclamation, storage reserve, and Home warnings.
7. Add BLE log listing and snapshot export with retransmission, deletion protection,
   timeout cleanup, and the lock exception for existing operations.
8. Update current documentation only as behavior is implemented: Architecture,
   feature overview, settings/UI/display, orchestrator, power/boot/GPS, sensor and
   correction, storage, BLE client/service, Wi-Fi/local/cloud, and OTA references.
   Update affected [VHUB cases](../../../vhub/README.md) for observable behavior.

Product policy belongs in Go services and orchestration. Keep direct ESP-IDF and
FreeRTOS calls in drivers, BSP, or platform implementations; do not add hardware
abstractions merely to make concrete drivers host-testable.

## Testing Strategy

### Host Verification

Add or update application tests using existing service and hardware boundaries:

- All Tracking Idle/Recording/Paused, Wi-Fi, lock, and logging combinations;
  correct cadence, sensor/correction selection, writes, and sleep eligibility.
- Initial Wi-Fi failure versus runtime reconnect, missing credentials, successful
  provisioning, and cancellation preserving credentials while disabling Wi-Fi.
- BLE lock transitions, tracking end while locked, download recovery, OTA ownership,
  timeout cleanup, and prevention of indefinite connection extension.
- Auto-stop from Start and mid-session, numeric-minute presets and BLE limits,
  replacement/cancellation, Pause, UTC jumps, storage-full stop, stopped state
  after unexpected restart, and slow exports crossing expiry.
- Cold-boot and tracking boundaries, deep-sleep continuation, ordered unknown-time
  records, rotation, torn tails, failed sync, and first-append/interval/final sync.
- Rolling reclamation, protected exports, no eligible reclaimable files, reserve
  handling, protected tracking sessions, warning persistence, and BLE stop reason.
- Export snapshots of active logs, appends during export, gap recovery, disconnect,
  timeout, and accurate aggregate flash usage.
- GPS deadline selection, five-minute bound, measurement/maintenance coincidence,
  button wake, absent GPS, all three retained GPS settings, Always On's independent
  awake policy, and no measurement/display work on GPS-only wakes.
- Upgrade/reset cases and unchanged optional-peripheral and measurement validity
  contracts. Do not require concrete hardware drivers to compile with TEST_HOST.

### Firmware and User-Run Hardware Verification

Build Go for production changes and all affected products for shared changes.
Follow [Host-Test Workflow](../../../tests/README.md) and source the installed
ESP-IDF environment in the same shell before every firmware build.

Hardware verification remains user-only. Cover V1 and V2, battery and USB, GPS
sleep/wake latency and maintenance energy, combined BLE/Wi-Fi heap and radio
behavior, Local Server power-save changes, sensor warmup/gas continuity, NAND
power interruption, warning/display transitions, provisioning cancellation,
and phone log downloads. Verify deadlines and safety work during long exports
and OTA; do not infer physical success from host tests or a successful build.

This spec-only change requires Markdown checks, not firmware or hardware tests.
Do not mark future verification as completed when only the specification exists.

## Open Questions

The product choices above are settled. The remaining items are engineering
decisions to resolve and record while implementing the affected part; they do
not require another product brainstorming round. Ask only if a constraint would
require changing agreed user-visible behavior.

1. **Compatibility:** define handling of old BLE/local/cloud mode fields, invalid
   RTC layouts, and firmware downgrade. Implement the agreed one-time Wi-Fi Off
   initialization and interval/GPS preference preservation.
2. **Deadline servicing:** define how auto-stop expires during an OTA operation
   that has temporarily suspended ordinary orchestration, without extending the
   session or compromising update safety.
3. **Storage bounds and format:** choose records per file, format/header encoding,
   sequence recovery/wrap behavior, unknown-time representation, warning threshold,
   and finalization reserve. Define rotation after software/watchdog resets, the
   explicit deletion/Clear Data contract, and recovery from non-capacity I/O errors.
4. **BLE additions:** finalize log operation/type identifiers, pagination, record
   encoding, capability discovery, export timeout, and stop-reason retention and
   reconnect visibility. Keep existing tracking records distinguishable.
5. **UI details:** define exact menu placement and duration labels, and how
   protected system messages interact with ordinary snackbar updates. Preserve
   the agreed unlock/next-button acknowledgement rule.
6. **Concurrent services:** settle BLE provisioning ownership/security and resource
   budgets with tracking plus Wi-Fi, as well as cloud/BLE OTA arbitration and
   existing manufacturing/onboarding paths that currently depend on modes.
