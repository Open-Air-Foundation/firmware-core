# AirGradient Go Power Management Research

Research branch log. Commit code, notes, and captures after each experiment;
this branch is not intended for merge. ESP32-C5, ESP-IDF v5.5.4.

## Status

2026-10-15: BLE modem sleep + DFS measured in both BLE states.
Automatic light sleep with CPU power-down disabled is next.
Baseline source: `03b78b48aaffd8541147be214a44e74609900e27` plus the
temporary SCD4x disable committed with this log.
Current firmware: BLE modem sleep and startup DFS enabled; maximum 240 MHz,
minimum detected main-crystal frequency. Automatic light sleep disabled.

## Test Configuration

- Portable mode; 10-second measurement interval; SPS30 always measuring.
- No tracking; GPS set to tracking-only and receiver stopped.
- **SCD4x measurement and polling disabled** for every run. Production uses S12.
  Driver initialization stops measurement and returns false to skip polling.
- Wi-Fi off. Keep Stationary Wi-Fi power-saving policy unchanged.
- BLE advertising first; connected-idle measured separately.
- Screen locked, device stationary, no user interaction or transfers.
- Other sensors and logging unchanged; USB disconnected during capture.
- No external 32.768 kHz crystal; retain the main crystal for BLE sleep timing.

## Measurement Procedure

1. Use the power supply at **3.9 V** and keep the analyzer measurement point
   unchanged across runs. Actual connection point: not yet recorded.
2. Wait until consumption is stable, exclude boot, then capture **one minute**.
3. Save average current/power, peak current, and trace. Record board/firmware
   revision and analyzer settings. For connected BLE, record connection interval,
   peripheral latency, and notification subscriptions.

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
| BLE modem sleep + DFS | Connected idle | 97.38 mA | 379.78 mW | 303.86 mA | [2026-10-15 capture](ppk-20261015T143554.png) |
| Above + light sleep, CPU power-down off | Advertising | — | — | — | — |
| Above + light sleep, CPU power-down off | Connected idle | — | — | — | — |
| Above + CPU power-down | Advertising | — | — | — | — |
| Above + CPU power-down | Connected idle | — | — | — | — |

Power reduction = 100 × (baseline − new average power) / baseline.
Production battery-life conclusions require a later comparison on S12 hardware.

Connected idle averaged 0.46 mA (0.33%) above advertising in these captures;
average consumption was effectively unchanged. Connection parameters were not recorded.

Displayed BLE modem sleep averages are 21.70 mA (15.54%) below the advertising
baseline and 23.65 mA (16.89%) below the connected-idle baseline. Both recordings
lasted one minute; screenshots show 10-second views with statistics labeled
"WINDOW." Full-record averages are not shown; no new capture is required.

Adding DFS reduced displayed average current by 6.06 mA (5.14%) while advertising
and 19.03 mA (16.35%) while connected. Total reductions from baseline are 19.89%
and 30.47%, respectively. DFS screenshots also show 10-second viewing windows.

Go stops advertising on connection. A quiet connection can require less radio
and CPU activity than advertising; negotiated timing and PM lock residency are
needed to confirm the cause of the larger connected-idle saving with DFS.

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
  at 111.84 mA and connected idle at 97.38 mA. Actual minimum frequency from the
  boot log and negotiated BLE connection parameters are not yet recorded.
- 2026-10-15: Split automatic light sleep and CPU power-down into separate tests.
  The existing CPU power-down option has not affected these awake Portable tests.
