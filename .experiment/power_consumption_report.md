# AirGradient Go Power Consumption Report

**Test dates:** 19–20 October 2026. **Status:** Firmware experiment, measured on hardware.

Sleep power decreased **61%** from the existing GNSS-stop baseline, from **31.20 mW to 12.09 mW**. Energy per wake cycle decreased **42%**, from **2.03 mWh to 1.19 mWh**, while wake duration shortened by **4.24 seconds**.

## Test Setup

| Item | Setup |
|---|---|
| Device | AirGradient Go, V1 board |
| Supply | External power supply fixed at **3.9 V**; current captured with PPK |
| Operating Mode | Offline, periodic measurement followed by deep sleep |
| Measurement Interval | **5 minutes (300 seconds)**, including wake and sleep; 12 cycles/hour |
| Final Experiment Configuration | PM measurement enabled; SGP41 VOC/NOx heater and measurements disabled; SCD4x CO₂ disabled; GPS tracking/acquisition off |
| Measurement Basis | Sleep figures are reported bench averages; wake figures are averages over selected PPK capture windows |

## Sleep Power Improvements

The baseline is the existing firmware's GNSS-stop behavior, measured at approximately **8 mA**. Subsequent stages replace GNSS stop with actual GPS deep sleep, then add the remaining changes cumulatively. All current and power figures refer to the **3.9 V supply input**.

| Stage | Change | Sleep Current | Sleep Power | Reduction From Previous Stage |
|---|---|---:|---:|---:|
| Baseline | Existing GNSS-stop command | 8.00 mA | 31.20 mW | — |
| 1 | Replace GNSS stop with actual GPS deep sleep (`CFG-SLEEP`) | 6.00 mA | 23.40 mW | 25% |
| 2 | Disable SGP41 heater and measurements | 3.72 mA | 14.51 mW | 38% |
| 3 | Put CAP1203 touch controller into deep sleep | 3.40 mA | 13.26 mW | 9% |
| 4 | Disable charger continuous ADC before system sleep | **3.10 mA** | **12.09 mW** | **9%** |

The latest capture independently indicates approximately **3.24 mA** averaged across the sleep portions before and after the wake selection, consistent with the earlier 3.1 mA reading.

## Wake-Cycle Improvements

Energy includes the entire selected wake cycle, including MCU light sleep while the PM sensor warms up.

| Stage | Change | Duration | Average Current | Energy Per Wake | Energy Reduction From Baseline |
|---|---|---:|---:|---:|---:|
| Baseline | Original wake sequence | 20.46 s | 91.73 mA | 2.03 mWh | — |
| Optimized | Replace discarded PM warmup reads with MCU light sleep; sleep PM before storage/display; remove GPS diagnostic wait; restore GPS baud after sleep | **16.22 s** | **67.57 mA** | **1.19 mWh** | **42%** |

The PM warmup remains **10 seconds**. GPS acquisition retains its existing three-second timeout but was not exercised in these captures.

## Five-Minute Energy Budget

Using the final wake capture and the measured **3.1 mA** sleep baseline:

| Portion | Duration Per Cycle | Energy Per Cycle |
|---|---:|---:|
| Wake / Measurement | 16.22 s | 1.19 mWh |
| Deep Sleep | 283.78 s | 0.95 mWh |
| **Total** | **300 s** | **2.14 mWh** |

This gives an estimated average input power of **25.7 mW** (**6.59 mA at 3.9 V**). A nominal **7.4 Wh battery** would provide approximately **12 days** under this experimental configuration. This is an energy-based estimate, not a battery endurance test; usable capacity and operation across battery voltage remain unverified.

The next timing opportunity is overlapping PM warmup with the approximately **1.7 seconds** currently spent probing and disabling CO₂ sensors. These results do not yet establish battery life with CO₂, VOC/NOx, or GPS tracking enabled.

## Evidence

Wake captures: [baseline](wakeuptime.png) and [optimized](wakeuptime3.png). Firmware log: [optimized](output.log.9).

Calculations use power = voltage × current, and energy = power × duration / 3600. Percentages use unrounded capture values.
