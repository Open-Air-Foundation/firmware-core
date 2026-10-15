# AirGradient Go Power Management Research

Research branch log. Commit code, notes, and captures after each experiment;
this branch is not intended for merge. ESP32-C5, ESP-IDF v5.5.4.

## Status

2026-10-15: Advertising and connected-idle baselines captured;
DFS and BLE modem sleep experiment next.
Baseline source: `03b78b48aaffd8541147be214a44e74609900e27` plus the
temporary SCD4x disable committed with this log.
Current firmware: 240 MHz CPU, ESP-IDF PM and BLE modem sleep disabled.

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
2. **DFS + BLE modem sleep:** retain 240 MHz maximum, record the selected minimum
   frequency, and keep automatic light sleep disabled; measure.
3. **Automatic light sleep:** verify wake sources, task waits, peripheral clocks,
   watchdog timing, and PM locks; enable tickless idle and light sleep; measure.
   Retain the existing GPIO workaround setting and leave peripheral power-down off.

For each change, run the Go build; host tests are skipped for this research.
Verify BLE discovery,
connection, READ/WRITE, button/shake response, and sensor updates on hardware.
Update relevant VHUB coverage. Flashing and captures are user-operated.

## Results

Compare each result against the matching BLE baseline. Power is calculated from
the 3.9 V supply setting and measured average current.

| Configuration | BLE State | Average Current | Average Power | Peak Current | Trace |
|---|---|---|---|---|---|
| Baseline | Advertising | 139.60 mA | 544.44 mW | 350.19 mA | [2026-10-15 capture](ppk-20261015T081240.png) |
| Baseline | Connected idle | 140.06 mA | 546.23 mW | 311.69 mA | [2026-10-15 capture](ppk-20261015T124049.png) |
| DFS + BLE sleep | Advertising | — | — | — | — |
| DFS + BLE sleep | Connected idle | — | — | — | — |
| Above + light sleep | Advertising | — | — | — | — |
| Above + light sleep | Connected idle | — | — | — | — |

Power reduction = 100 × (baseline − new average power) / baseline.
Production battery-life conclusions require a later comparison on S12 hardware.

Connected idle averaged 0.46 mA (0.33%) above advertising in these captures;
average consumption was effectively unchanged. Connection parameters were not recorded.

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
