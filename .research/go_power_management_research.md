# AirGradient Go Power Management Research

Research branch log. Commit code, notes, and captures after each experiment;
this branch is not intended for merge. ESP32-C5, ESP-IDF v5.5.4.

## Status

2026-10-16: With custom GPIO sleep callbacks disabled, BLE stays connected for
one minute. Advertising average is 97.33 mA; Connected idle is 96.79 mA. Automatic light
sleep and CPU power-down remain enabled. The previous `0x208` timeout is no longer
reproduced in this run, implicating our callbacks; the exact mechanism is unknown.
User retested all touch events: working with callbacks disabled.
Baseline source: `03b78b48aaffd8541147be214a44e74609900e27` plus the
temporary SCD4x disable committed with this log.
Current firmware: BLE modem sleep and startup DFS enabled; maximum 240 MHz,
minimum 48 MHz (confirmed in boot log). Runtime automatic light sleep starts after
the first completed measurement; tickless idle and PM profiling remain enabled,
including after BLE disconnects. Custom GPIO sleep callbacks are disabled;
ESP-IDF Bluetooth wake handling remains unchanged.
CPU power-down is enabled; peripheral power-down remains disabled.

Advertising and Connected idle captures are complete for this configuration.

## Test Configuration

- Portable mode; 10-second measurement interval; SPS30 always measuring.
- No tracking; GPS set to tracking-only and receiver stopped.
- **SCD4x measurement and polling disabled** for every run. Production uses S12.
  Driver initialization stops measurement and returns false to skip polling.
- Wi-Fi off. Keep Stationary Wi-Fi power-saving policy unchanged.
- BLE advertising first; connected-idle measured separately.
- Screen locked, device stationary, no user interaction or transfers.
- Keep the case fitted consistently for subsequent functional and power tests.
- **USB data connected, cable VCC disconnected, in every test so far.**
- Other sensors unchanged; revised light-sleep firmware adds PM diagnostics.
- No external 32.768 kHz crystal; retain the main crystal for BLE sleep timing.

## Measurement Procedure

1. Use the power supply at **3.9 V** and keep the analyzer measurement point
   unchanged across runs. Actual connection point: not yet recorded.
2. Wait until consumption is stable, exclude boot, then capture **one minute**.
3. Save average current/power, peak current, and trace. Record board/firmware
   revision and analyzer settings. For connected BLE, record connection interval,
   and peripheral latency.

## Experiment Sequence

1. **Baseline:** disable SCD4x; keep ESP-IDF PM disabled; measure.
2. **BLE modem sleep only:** enable `CONFIG_BT_LE_SLEEP_ENABLE`, keep the CPU at
   240 MHz, and leave `CONFIG_PM_ENABLE` disabled; measure.
3. **Add DFS:** retain BLE modem sleep and the 240 MHz maximum, record the selected
   minimum frequency, and keep automatic light sleep disabled; measure.
4. **Add automatic light sleep:** verify wake sources, task waits, peripheral clocks,
   watchdog timing, and PM locks; enable tickless idle and runtime light sleep.
   Set `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=n`; measure.
   Retain the existing GPIO workaround setting and leave peripheral power-down off.
5. **Add CPU power-down:** set `CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y`,
   keeping all other settings from stage 4; measure.

For each change, run the Go build; host tests are skipped for this research.
Verify BLE discovery, connection, READ/WRITE, button/shake response, and sensor
updates on hardware.
Update relevant VHUB coverage. Flashing and captures are user-operated.

## Light-Sleep Checkpoint Before Callback Isolation

- First measurement enables sleep after startup and warmup. GPIO wake covers
  both buttons, touch INT, and motion INT; normal edge interrupts resume on wake.
- `CONFIG_PM_PROFILING=y` adds cumulative lock statistics, successful sleep count,
  and rejected sleep count every 60 seconds, checked after measurements.
  `CONFIG_PM_LIGHT_SLEEP_CALLBACKS=y` enables the GPIO transitions around sleep.
- Check repeated cold boots, buttons, touch, shake, BLE READ/WRITE, and sensor
  updates first. Then repeat the stable one-minute advertising capture at 3.9 V.
- USB logs can still disappear during light sleep. Read diagnostics through UART0
  at 115200 if needed; USB reconnect is not guaranteed to recover the console.
  Leave `CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION=n`: enabling it prevents automatic
  sleep while the host is connected. See [Espressif USB sleep notes](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32c5/api-guides/usb-serial-jtag-console.html#sleep-mode-considerations).
- Compare counter changes between dumps; mode percentages show PM mode occupancy,
  not exact physical sleep residency. Profiling and dumps add overhead; disable
  profiling for the final consumption comparison after diagnosing sleep behavior.

## BLE Display Diagnostic

- UART is not accessible on Go and native USB logging stops during sleep.
  Show the first BLE disconnect reason as `BLE err 0x...` in the Home screen's
  bottom message area. The raw NimBLE code is retained in RAM until reboot.
- Use existing display refreshes; the message survives measurements and later
  connection attempts. It temporarily overrides other Home messages.
- Automatic light sleep and CPU power-down remain enabled. No recovery action,
  connection-policy change, or configuration change is added.
- Flash, stay on Home, wait for the first measurement, then attempt BLE connection.
  Let it disconnect and send the displayed code. Reboot before another fresh
  attempt; manual disconnections also populate the diagnostic.
- This is a temporary research display, not production UX or a power comparison.

## GPIO Callback Isolation

- Disable only registration of our GPIO sleep entry/exit callbacks using the
  temporary source constant `POWER_RESEARCH_GPIO_SLEEP_CALLBACKS=false`.
  Retain global GPIO wake-source enablement and all menuconfig settings.
- Keep automatic light sleep, CPU power-down, BLE modem sleep, DFS, profiling,
  and the disconnect-code display as in the previous test.
- After flashing, stay on Home, wait for the first measurement, then connect BLE.
  Report whether it stays connected and READ/WRITE works, or the displayed code.
  No power capture needed.
- These research callbacks were added to arm button/touch/motion GPIO wake before
  sleep and restore normal edge interrupts afterward. They change GPIO interrupt
  and wake configuration on each sleep attempt; they are not BLE callbacks.
- Result: BLE stayed connected for one minute (Connected idle). Disabling
  the callbacks removes the observed timeout in this run; the exact failing
  callback operation has not been isolated. User retested all touch events and
  confirmed they work with the callbacks disabled.

## Further Idle-Time Improvements

Automatic light sleep uses available idle gaps. Further savings may require
targeted code changes to create longer uninterrupted idle periods, rather than
configuration changes alone. A full redesign is not yet justified.

- Measure actual sleep frequency/duration and identify task, timer, and PM-lock
  activity before choosing changes; mode percentages alone are insufficient.
- Investigate unnecessary polling, opportunities for interrupts/blocking waits,
  combining timer-driven work, and shortening PM-lock duration.
- Review avoidable display refreshes, logging, and background work while idle.
- Preserve input responsiveness and sensor timing. Investigate the BLE disconnect
  separately; more idle time is not an established fix.

CPU power-down adds savings only during sleep. Short sleep periods and transition
overhead may limit its benefit; this has not been measured yet. SPS30 and other
board loads remain, so CPU improvements alone cannot eliminate their consumption.
These are research directions, not implemented changes or product decisions.

## Results

Compare each stage with the previous stage to isolate its additional saving,
and with the baseline for total saving. Match BLE state in both comparisons.
Power is calculated from the 3.9 V supply setting and measured average current.

| Configuration | BLE State | Average Current | Average Power | Peak Current | Trace |
|---|---|---|---|---|---|
| Baseline | Advertising | 139.60 mA | 544.44 mW | 350.19 mA | [2026-10-15 capture](ppk-20261015T081240.png) |
| Baseline | Connected idle | 140.06 mA | 546.23 mW | 311.69 mA | [2026-10-15 capture](ppk-20261015T124049.png) |
| BLE modem sleep only | Advertising | 117.90 mA | 459.81 mW | 349.40 mA | [2026-10-15 capture](ppk-20261015T125711.png) |
| BLE modem sleep only | Connected idle | 116.41 mA | 454.00 mW | 308.56 mA | [2026-10-15 capture](ppk-20261015T125919.png) |
| BLE modem sleep + DFS | Advertising | 111.84 mA | 436.18 mW | 312.47 mA | [2026-10-15 capture](ppk-20261015T143253.png) |
| BLE modem sleep + DFS | Connected idle | 109.97 mA | 428.88 mW | 303.86 mA | [2026-10-15 capture](ppk-20261015T143554.png) |
| Above + light sleep, CPU power-down off (initial trial) | Advertising | ~113 mA | ~441 mW | — | [Startup log](result.log) |
| Above + light sleep, revised wake handling + profiling | Advertising | 100.26 mA | 391.01 mW | 321.88 mA | [2026-10-15 capture](ppk-20261015T154620.png) |
| Above + light sleep, revised wake handling + profiling | Connected idle | Unavailable: disconnects immediately | — | — | — |
| Above + CPU power-down | Advertising | 100.99 mA | 393.86 mW | 313.26 mA | [CPU power-down capture](ppk-20261015T170750.png) |
| Above + CPU power-down | Connected idle | Unavailable: cannot connect | — | — | — |
| Above + custom GPIO callbacks disabled | Advertising | 97.33 mA | 379.59 mW | 385.75 mA | [Callback isolation capture](ppk-20261015T175032.png) |
| Above + custom GPIO callbacks disabled | Connected idle | 96.79 mA | 377.48 mW | 307.78 mA | [Callback isolation capture](ppk-20261015T173552.png) |

Power reduction = 100 × (baseline − new average power) / baseline.
Production battery-life conclusions require a later comparison on S12 hardware.

Connected idle averaged 0.46 mA (0.33%) above advertising in these captures;
average consumption was effectively unchanged. Connection parameters were not recorded.

Displayed BLE modem sleep averages are 21.70 mA (15.54%) below the advertising
baseline and 23.65 mA (16.89%) below the connected-idle baseline. Both recordings
lasted one minute; screenshots show 10-second views with statistics labeled
"WINDOW." Full-record averages are not shown; no new capture is required.

Adding DFS reduced displayed average current by 6.06 mA (5.14%) while advertising
and 6.44 mA (5.53%) while connected. Total reductions from baseline are 19.89%
and 21.48%, respectively. DFS screenshots also show 10-second viewing windows.

Connected idle with DFS is 1.87 mA below advertising. Go stops advertising on
connection; the cause of this small difference has not been established.

CPU power-down displayed average is 0.73 mA (0.73%) above the preceding trial.
This screenshot shows a 10-second viewing window; the preceding 100.26 mA value
covers a one-minute window. These observations do not establish a power saving
or a repeatable increase. BLE connection still fails with CPU power-down enabled.

With custom GPIO callbacks disabled, advertising is 97.33 mA and Connected idle
is 96.79 mA: a 0.54 mA difference. Advertising is 3.66 mA (3.62%) below the prior
100.99 mA advertising capture with callbacks enabled. Total reductions from the
matching baselines are 30.28% and 30.89%, respectively. Both one-minute captures
show 10-second viewing windows. BLE and all touch events work in the user tests;
the exact callback failure mechanism remains unknown.

## Activity Log

- 2026-10-14: Reviewed firmware and agreed on the test configuration.
- 2026-10-14: Added a temporary early return after SCD4x stop measurement;
  bypassed retained-measurement resume. No ESP-IDF PM settings changed.
  Go firmware build and Markdown lint passed; host tests skipped as requested.
- 2026-10-15: Captured the stable baseline using a 3.9 V power supply for one
  minute, excluding boot. Analyzer reports 139.60 mA average, 350.19 mA maximum,
  and 8.38 C charge. Research notes and captures moved into `.research/`.
- 2026-10-15: Track code, research notes, and captures in Git; use separate
  commits for subsequent experiments.
- 2026-10-15: Connected-idle baseline at 3.9 V for one minute: 140.06 mA average,
  311.69 mA maximum, and 8.40 C charge. Firmware unchanged from `bdca1e6`.
- 2026-10-15: Split BLE modem sleep and DFS into separate measurement stages.
- 2026-10-15: User enabled `CONFIG_BT_LE_SLEEP_ENABLE` and tested advertising.
  CPU remains 240 MHz, PM disabled, BLE clock main XTAL. The displayed window
  reports 117.90 mA average, 349.40 mA maximum, and 1.18 C over 10 seconds.
- 2026-10-15: BLE modem sleep connected idle: displayed 10-second window reports
  116.41 mA average, 308.56 mA maximum, and 1.16 C. Firmware unchanged from `e667ffa`.
- 2026-10-15: User confirmed both BLE modem sleep recordings lasted one minute;
  corrected the capture duration, keeping viewing-window statistics distinct.
- 2026-10-15: User built and flashed with `CONFIG_PM_ENABLE=y` and
  `CONFIG_PM_DFS_INIT_AUTO=y`; tickless idle remains disabled. Recorded advertising
  at 111.84 mA and connected idle at 109.97 mA. Actual minimum frequency from the
  boot log and negotiated BLE connection parameters are not yet recorded.
- 2026-10-15: Split automatic light sleep and CPU power-down into separate tests.
  The existing CPU power-down option has not affected these awake Portable tests.
- 2026-10-15: Corrected the misread DFS connected-idle average to 109.97 mA,
  as confirmed by the user; recalculated power and savings.
- 2026-10-15: User enabled tickless idle. Added runtime light-sleep enablement in
  `GoHardwareBoard::init_core()`, preserving startup DFS limits. Disabled CPU
  power-down for this stage. Existing timers and GPIO interrupt handling remain
  unchanged for the initial experiment; button/shake wake behavior is unverified.
  Idle threshold remains three ticks (30 ms at 100 Hz).
  Go firmware build passed; host tests skipped as agreed. After flashing, expect
  a PM configuration log with `Light sleep: ENABLED` during hardware initialization.

- 2026-10-15: Initial light-sleep trial: ~113 mA advertising, sluggish response,
  repeated power cycles, and USB log loss. [Log](result.log) contains USB
  disconnects but no captured panic. USB data was connected in every test,
  with cable VCC disconnected.
- 2026-10-15: Deferred automatic sleep until the first completed measurement;
  added GPIO wake callbacks and PM profiling. Revised one-minute advertising
  capture: 100.26 mA average, 321.88 mA peak, 6.02 C. Reduction versus DFS:
  11.58 mA (10.35%); versus baseline: 28.18%. User observed peaks during display
  refresh and menu navigation. BLE connected then immediately disconnected.
- 2026-10-16: Runtime light-sleep-off control restored BLE communication.
  PM dump at ~74.5 seconds since boot: 48 MHz 76%, 80 MHz 14%, 240 MHz 9%;
  cumulative figures include startup. Initial touch concerns were due to
  comparing with/without the case; double tap works in the control build.
- 2026-10-16: Re-enabled runtime light sleep, retaining other settings.
  With the case fitted, double tap works; BLE still immediately disconnects.
  The separate DFS reference build was not needed.
- 2026-10-16: Removed the untested BLE-disconnect diagnostics, including the
  proposed sleep-disable response and reason retention. Returned firmware to
  the last tested checkpoint; retained PM profiling and GPIO wake handling.
  Go build, formatting, and Markdown lint passed; host tests skipped as agreed.
- Next experiment: user enables `PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` through
  menuconfig, keeping peripheral power-down and other settings unchanged.
  Configuration edits remain user-operated. Compare advertising against
  100.26 mA with the same case/supply setup; BLE disconnect is a known issue.
- 2026-10-16: User enabled `PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP` in menuconfig.
  Active config changes only this option and its legacy alias
  `ESP_SYSTEM_PM_POWER_DOWN_CPU`; `sdkconfig.old` is the previous-config backup.
  Peripheral power-down remains off; BLE sleep, DFS, tickless idle, PM profiling,
  and GPIO wake callbacks remain unchanged. Go build and Markdown lint passed;
  hardware test pending. No manual config edits or commit by the agent.
- 2026-10-16: User supplied CPU power-down capture: displayed average 100.99 mA,
  peak 313.26 mA, charge 1.01 C in the 10-second viewing window. Recording uses
  the agreed one-minute procedure; window duration is not recording duration.
  User reports BLE still cannot connect. No firmware/configuration changes made
  after this result; CPU power-down remains enabled.
- 2026-10-16: Added raw BLE disconnect reason to its app event and a persistent
  Home display overlay for the first reason. No PM/BLE policy changes. Go build,
  formatting, and Markdown lint passed; host tests skipped as agreed. Hardware
  test pending. Configuration untouched; no commit.
- 2026-10-16: Display diagnostic returned `0x208`. Installed NimBLE headers map
  this to HCI base `0x200` + `BLE_ERR_CONN_SPVN_TMO` (`0x08`): connection
  supervision timeout. The controller reports link loss through missing received
  packets, not an explicit local/remote termination reason. With the sleep-off
  control working, sleep/wake timing or controller restoration is a suspect;
  the code does not identify the root cause. No firmware/config changes made.
  Mapping: [NimBLE return codes](https://mynewt.apache.org/v1_5_0/network/ble_hs/ble_hs_return_codes.html).
- 2026-10-16: Disabled custom GPIO sleep callback registration for isolation.
  Global GPIO wake-source enablement, runtime sleep, CPU power-down, and the BLE
  display diagnostic retained. No menuconfig changes. Go build, formatting, and
  Markdown lint passed; hardware result pending. Host tests skipped as agreed.
  Nothing committed.
- 2026-10-16: User confirmed BLE stays connected for one minute after disabling
  custom GPIO sleep callbacks. Connected idle capture: displayed average
  96.79 mA, peak 307.78 mA, 0.97 C in the 10-second view. Automatic light sleep and
  CPU power-down remain enabled. Prior run was advertising, so the 4.20 mA drop
  cannot be attributed solely to callback removal. The exact timeout mechanism
  remains unresolved. No further firmware/config change.
- 2026-10-16: User retested all touch events with callbacks disabled: all working.
- 2026-10-16: Completed advertising capture with callbacks disabled and settings
  unchanged: 97.33 mA average, 385.75 mA peak, 379.59 mW at 3.9 V. Trace:
  [Advertising capture](ppk-20261015T175032.png). No firmware/config changes.
