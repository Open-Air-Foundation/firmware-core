# Go Thermal Calibration (temperature_adjust)

Self-heating from the BQ25629 charger and other components conducts along the
PCB into the SHT40 stem, biasing temperature (and therefore RH) even though
the stem protrudes from the enclosure. This toolkit collects the raw data to
fit a compensation model:

```
T_ambient = T_sht - k * (T_internal - T_sht) - b        (steady state)
RH_ambient = RH at T_ambient assuming constant dew point (Magnus)
```

`T_internal` candidates are all streamed so the best regressor (or a
combination) can be chosen offline: DPS368 (main PCB), fuel-gauge internal,
BQ25629 die, battery NTC.

## Cal BLE characteristic

UUID `d1c0c0a5-6b48-4b2a-9b1d-59f9f2b0a1e1` (Read + Notify, **no pairing
required** — the screenless reference board cannot display a passkey).
Notified once per measurement cycle (default 10 s) with a CBOR map;
invalid/missing sources are omitted:

| Key | Value |
|---|---|
| `t`, `h` | SHT40 temperature (°C) / humidity (%) — external stem |
| `tdps` | DPS368 temperature (°C) — main PCB |
| `pres` | pressure (hPa) |
| `tfg` | fuel-gauge internal temperature (°C) |
| `tdie` | BQ25629 die temperature (°C) |
| `tbat` | battery NTC temperature (°C) |
| `ibat` | FG battery current (mA, +charge) |
| `ichg` | charger battery current (mA) |
| `ibus` | input/VBUS current (mA) |
| `vbus`, `vbat` | VBUS / battery voltage (V) |
| `chg` | charging state string (`none`/`fast`/`taper`/...) |
| `gps` | GPS receiver active (0/1) |
| `up` | uptime (s) |
| `ts` | device epoch timestamp |

BMS-sourced fields refresh on the 30 s full BMS poll; sensor fields are fresh
each cycle. Fine for thermal time constants (minutes).

## Boards

- **DUT** — normal firmware from branch `temperature_adjust`, fully populated,
  in enclosure. Runs its ordinary duty cycle; its self-heating is the signal.
- **Reference** — firmware from branch `temperature_adjust-ref` on a board
  with no AQ sensors populated (no PM / CO2 / SGP41), SHT40 + DPS368 only,
  outside the enclosure. GPS is forced off and the measure interval forced to
  10 s at boot. Run it on battery only — **never plugged in** — with a fully
  charged battery. Its own `tdps - t` spread is the cleanliness self-check:
  it must stay small and constant; discard any interval where it jumps.

## Collector

```
pip install bleak cbor2
python cal_collector.py --dut "AirGradient Go ef0e" --ref "AirGradient Go 12ab" --out run1.csv
```

Both devices stream into one CSV, host-timestamped on arrival (`role` column
distinguishes them). Reconnects automatically; append-safe.

## Suggested run matrix (still air, 10–20 cm apart, no direct sun)

| Scenario | Duration | Purpose |
|---|---|---|
| idle, screen off | 1–2 h | baseline offset `b` |
| GPS on | 1–2 h | GPS heat term |
| plug in, full-rate charge | 1–2 h | max ΔT → dominates `k` |
| taper / top-off | 1 h | low-current k |
| unplug, cool-down | 1–2 h | step response → filter time constant |

Repeat at a second room temperature (AC on/off) to confirm k is
temperature-independent. Spot-check the reference against a trusted
thermometer once per run.
