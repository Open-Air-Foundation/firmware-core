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

## Quick start (tester)

You need: the two Go boards (DUT + bare reference), a USB-C cable, a laptop
with BLE, ESP-IDF v5.5.x, Python 3.10+.

**1. Flash the DUT (fully populated board):**

```bash
git switch temperature_adjust
. "$HOME/esp/<your-idf>/esp-idf/export.sh"
idf.py -C products/go build
idf.py -C products/go -p <PORT> flash
```

**2. Flash the reference (bare board):**

```bash
git switch temperature_adjust-ref
idf.py -C products/go build          # full rebuild — same build dir!
idf.py -C products/go -p <PORT> flash
```

Boot log must show `THERMAL-CAL REFERENCE BUILD: forcing GPS off, 10 s
measure interval`. Always rebuild after switching branches — both branches
share `products/go/build`, so the binary left there is whatever branch built
last.

Note each board's advertised BLE name (`AirGradient Go xxxx`; the suffix is
the last 4 hex chars of the serial — shown in the About screen, or derive it
from the MAC esptool prints while flashing).

**3. Run the collector (laptop):**

```bash
pip install bleak cbor2
python3 products/go/tools/thermal_cal/cal_collector.py \
    --dut "AirGradient Go <dut-suffix>" --ref "AirGradient Go <ref-suffix>" \
    --out run1.csv
```

Both devices stream into one CSV, host-timestamped on arrival (`role` column
distinguishes them). Reconnects automatically; append-safe. Expect a `[dut]`
and a `[ref]` line every ~10 s once the first measurement cycle completes
(~1–2 min after boot).

**macOS**: if the script dies with `zsh: abort`, your terminal app has no
Bluetooth permission. Run it from Apple's stock Terminal.app (it prompts —
click Allow), or add your terminal via System Settings → Privacy & Security →
Bluetooth → **+**, then fully restart the terminal.

No pairing/passkey is ever required — if you get a pairing prompt you are
poking an authenticated characteristic, not the Cal one.

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

## Cold-soak sweep (automated temperature-range characterization)

Covers the low-temperature range and yields the sensors' thermal time
constant in one unattended run.

**Condensation warning:** a 0 degC board brought into tropical room air
(dew point ~19-20 degC) WILL condense. Seal each board in a zip-lock bag
(desiccant pack if available) before the soak and keep it sealed until the
board is back at room temperature. Never power a visibly damp board.

1. Charge both boards, start the collector (`--out coldsoak.csv`), confirm
   both stream.
2. Bag the boards, put both in the chamber/fridge at 0 degC for >= 45 min.
   BLE will likely drop inside — that's fine, the collector reconnects.
3. Take them out, place on the bench (still bagged, still on battery),
   let them warm to room temperature undisturbed for 1-2 h.
4. Stop the collector and run the fitter:

```bash
pip install pandas numpy matplotlib
python3 products/go/tools/thermal_cal/cal_fit.py coldsoak.csv run1.csv --out cal_report/
```

Feed it every CSV you have (soak + scenario runs together) — it segments
automatically:

- **self-heating fits** — `t_dut - t_ref = k*(T_int - t_dut) + b` for each
  candidate internal sensor (`tdps`/`tfg`/`tdie`), split into all / charging /
  quiet segments, with R2 and residuals — pick the regressor with the best R2
  and stable k across segments.
- **quiet offsets by temperature bin** (1 degC) — shows whether the offsets
  drift with absolute temperature across the swept range.
- **warm-up tau** per sensor — the exponential recovery constant, which sets
  the low-pass filter for the firmware compensation.

Outputs `cal_report/report.html` (self-contained report — tables, model
comparison with the best R² highlighted, embedded plots; shareable as one
file), `cal_fit.json` (machine-readable coefficients for the firmware step),
`bins.csv`, and PNG plots.
