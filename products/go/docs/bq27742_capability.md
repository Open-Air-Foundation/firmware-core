# BQ27742-G1 Gauge and Protector Reference

The AirGradient Go main board rev 2.0 carries a TI BQ27742-G1 (U3) as its
fuel gauge and single-cell battery protector. The part owns the CHG/DSG FETs,
runs Impedance Track, and keeps every setting that matters for safety in its
own data flash. This page collects what the firmware needs from the TRM and
datasheet in one place: the I²C rules the ESP32-C5 must respect, the
data-flash map the driver reads and writes, the protector checksum that can
lock the pack, and the status registers that explain why a FET is open.

Sources are TI literature SLUUAX0C Rev. C (Technical Reference Manual, "TRM")
and SLUSBV9D (datasheet, "DS"). Page numbers are PDF pages. Board facts come
from the `airgradient-go-main` v2.0 schematic. Where a value has been
confirmed on a bench unit it is marked "bench".

## Scope

- BQ27742-G1 only. The BQ25628 charger has its own JEITA and NTC path; see
  [`power_management.md`](power_management.md).
- Board rev 2.0 only. Rev 1 boards use a BQ27427 without a protector; see
  [`fg_learning.md`](fg_learning.md) for that gauge's learning path.
- Chip behaviour and the firmware that exists today. Planned data-flash
  changes are listed under [Firmware Coverage](#firmware-coverage) as
  "not written" so the map stays honest.

## Board Facts

| Item | Value | Source |
|---|---|---|
| Part | BQ27742YZFR-G1, LCSC C2842397 | schematic U3 |
| I²C address | 7-bit `0x55` (`0xAA` write / `0xAB` read) | TRM p.40 §3.4 |
| I²C clock on Go v2.0 | 100 kHz on the gauge's own device handle; the other 11 devices stay at 400 kHz | [`go_hardware_board.cpp`](../main/go_hardware_board.cpp) |
| Bus pull-ups | R30 / R26 = 10 kΩ | schematic |
| Sense resistor | R8 = 5 mΩ across SRP/SRN | schematic |
| Pack thermistor (NTC2) | Murata NCP15XH103F, 103AT-class model | schematic, DS |
| Cell | Cowon INR18490NP, 2600 mAh, 4.20 V, charge 0–45 °C, discharge −20–60 °C | cell datasheet |
| RC2 interrupt pin | no-connect, polling only | schematic |
| REG25 | 2.5 V internal LDO for the gauge core only, 2.3–2.6 V | DS p.7 §6.6, bench 2.5 V |
| Identity | DEVICE_TYPE `0x0742`, FW_VERSION `0x0103` | TRM p.46, bench |

## I2C Interface

### Command Formats

The gauge supports four transaction shapes (TRM p.41 Figure 3-1): 1-byte
write, quick read, 1-byte read, and incremental read. The address pointer
auto-increments on every acknowledged byte, so a 2-byte standard command is
one incremental read of two bytes. A `Control()` subcommand is two writes of
one byte each to `0x00` and `0x01` (low byte first), followed after a wait by
a read of `0x00`/`0x01`. The driver sends the two bytes as two 1-byte writes
to match the figure exactly. Figure 3-1 lists no multi-byte write, yet the
gauge acknowledges a 33-byte block write (`0x40` plus 32 data bytes) in one
transaction: that is how every data-flash block on the board is committed
and read back (bench). The 1-byte split for `Control()` is therefore a
precaution, not a requirement.

### Waiting Times

TRM p.42 §3.4.2 sets host-side waits that are independent of bus speed:

| Operation | Required wait |
|---|---|
| `Control()` subcommand write → read result | 66 ms |
| Consecutive reads of the same result | 66 ms |
| Checksum subcommands (`PROTECTOR_CHKSUM`, `ALL_DF_CHKSUM`, …) → read result | 100 ms |
| Read/write standard commands → value updated | 2 s minimum |
| Read-only standard commands | no wait, but do not sweep the standard command set more than twice per second (watchdog reset risk) |

Electrical timing (DS p.11 §6.21): `f_SCL` ≤ 400 kHz, `t_r` and `t_f` ≤ 300 ns,
`t_BUF` ≥ 66 µs between STOP and the next START. The 66 µs bus-free time and
the 66 ms subcommand wait are unrelated numbers.

### Clock Stretching

The gauge stretches SCL whenever its CPU is busy, including after the start
bit, after ACK/NACK, and after the first data bit of a host read
(TRM p.42 §3.4.3). Typical stretches are ≤ 4 ms. The long cases:

| Event | Maximum stretch |
|---|---|
| Ra table update during discharge | 24 ms |
| Data-flash block write | 72 ms |
| Block write recovery after an interrupted write | 116 ms |
| End-of-discharge update with an Ra update in flight | 144 ms |

The host I²C timeout must therefore allow at least 150 ms per transaction.

### Why the Gauge Runs at 100 kHz

At 400 kHz on the shared bus, the ESP32-C5's incremental 2-byte read of a
`Control()` result returns a corrupted high byte while the low byte and every
1-byte read are correct: DEVICE_TYPE reads `0x0372`, `0x0072`, `0x0C72`, or
`0x3272` depending on the boot. `Voltage()` and `AtRate()` traffic is
unaffected. At 100 kHz the same read returns `0x0742` every time (bench,
2026-09). Because unseal, `IT_ENABLE`, and every data-flash operation go
through `Control()`, none of them work on a rev 2.0 board at 400 kHz. The
gauge therefore gets its own device handle at 100 kHz. Whether the 400 kHz
failure is a rise-time problem (10 kΩ pull-ups, 12 devices) or a
clock-stretch handling problem is not yet established; bring-up item P3-02
measures `t_r` at R30/R26.

## Command Map

Standard and extended commands the firmware uses (TRM Tables 4-1, 4-6):

| Command | Address | Width | Notes |
|---|---|---|---|
| `Control()` | `0x00`/`0x01` | 2 | subcommand register, see below |
| `AtRate()` | `0x02` | 2 | the one read/write standard command |
| `Temperature()` | `0x06` | 2 | pack thermistor on NTC2, 0.1 K |
| `Voltage()` | `0x08` | 2 | mV |
| `Flags()` | `0x0A` | 2 | `CHG_INH`, `CHG_SUS`, `FC`, … |
| `RemainingCapacity()` | `0x10` | 2 | mAh |
| `FullChargeCapacity()` | `0x12` | 2 | mAh |
| `AverageCurrent()` | `0x14` | 2 | signed mA |
| `TimeToEmpty()` | `0x16` | 2 | minutes |
| `SafetyStatus()` | `0x1A`/`0x1B` | 2 | firmware-layer faults, see [Status Registers](#status-registers) |
| `InternalTemperature()` | `0x28` | 2 | die temperature, 0.1 K |
| `StateOfCharge()` | `0x2C` | 2 | % |
| `DesignCapacity()` | `0x3C` | 2 | readable while sealed |
| `DataFlashClass()` | `0x3E` | 1 | unsealed only |
| `DataFlashBlock()` | `0x3F` | 1 | 32-byte block index |
| `BlockData()` | `0x40`–`0x5F` | 32 | data-flash window |
| `BlockDataCheckSum()` | `0x60` | 1 | writing it commits the block |
| `BlockDataControl()` | `0x61` | 1 | `0x00` selects general data flash |
| `ProtectorStatus()` | `0x6D` | 1 | AFESTAT1, hardware faults and FET state |
| `AveragePower()` | `0x76` | 2 | signed mW; the driver still derives V × I instead |
| `ProtectorState()` | `0x78` | 1 | AFESTATE, protector state machine |

`Control()` subcommands (TRM p.45 Table 4-2):

| Subcommand | Code | Sealed | Returns / effect |
|---|---|---|---|
| `CONTROL_STATUS` | `0x0000` | yes | status bits; low byte bit 0 = `QEN`, `SS` = sealed |
| `DEVICE_TYPE` | `0x0001` | yes | `0x0742` |
| `FW_VERSION` | `0x0002` | yes | `0x0103` on the bench units |
| `HW_VERSION` | `0x0003` | yes | hardware revision |
| `PROTECTOR_VERSION` | `0x0004` | yes | protector revision |
| `PREV_MACWRITE` | `0x0007` | yes | previous subcommand written |
| `PROTECTOR_CHKSUM` | `0x001A` | yes | live sum of Prot OV Config + Prot OC Config, bit 15 set on mismatch |
| `SEALED` | `0x0020` | no | seals data flash |
| `IT_ENABLE` | `0x0021` | no | starts Impedance Track, sets `QEN` (one-way) |
| `START_FET_TEST` | `0x0024` | no | test-only FET override with `FETTest()` `0x74` |
| `RESET` | `0x0041` | no | full reset; briefly opens both FETs |

Unseal is not a subcommand: write the two key words to `Control()` back to
back with nothing in between. For the default key `0x36720414` the driver
sends `Control(0x0414)` then `Control(0x3672)` (TRM p.59 §5.1.4, p.105
§5.9.1). This sequence unseals the bench units and is what every data-flash
write on the board relies on. The full-access key (`0xFFFFFFFF`) is only
needed to change the keys themselves.

## Data Flash Access

### Block Transfer

Data flash is read and written one 32-byte block at a time through the
command window (TRM p.58 §5.1.1, p.54 §4.2.3–4.2.7). Read:

1. Unseal (idempotent).
2. Write `0x00` to `BlockDataControl()`.
3. Write the subclass ID to `DataFlashClass()`.
4. Write `offset / 32` to `DataFlashBlock()`.
5. Read 32 bytes from `BlockData()`. Field offset `n` sits at `0x40 + n % 32`.

Write: perform the read, change only the target bytes in the host copy,
write all 32 bytes back to `BlockData()`, then write the checksum to
`BlockDataCheckSum()`:

```text
checksum = 255 - (sum(block[0..31]) mod 256)
```

Writing `0x60` is what programs the flash. A block write takes up to 72 ms;
the driver waits 100 ms before touching the gauge again. The gauge keeps a
copy buffer so a power loss mid-write is recovered on the next access
(TRM p.42), but the TRM does not promise which value survives, so the driver
reads the block back after every write.

### Rules the Gauge Does Not Enforce

- Values are not range-checked: "the values are not rejected by the fuel
  gauge. Writing an incorrect value may result in hardware failure"
  (TRM p.58). The driver validates every field against the TRM min/max before
  it writes.
- A wrong `BlockDataCheckSum()` is not reported. Read-back is the only check.
- Data flash cannot be programmed while `Voltage()` is below Flash Update OK
  Voltage (subclass 68 offset 0, default 2800 mV) unless a charger is present
  (TRM p.38 §2.8.3). With UV Prot at 2700 mV the gauge can sit in a band where
  it cannot update its own Ra/Qmax; the firmware writes happen at boot on USB
  so they are unaffected.

## Data Flash Map

Only the subclasses the firmware touches or plans to touch. "Go v2.0" is the
value the firmware writes at boot today; "—" means the factory default is
kept.

### Subclass 2 Safety

Firmware-layer protection, evaluated once per second against `Voltage()` and
`Temperature()` (TRM p.60, p.66–67). The gauge opens the CHG or DSG FET
itself on these; the hardware protector below is the second, coarser level.

| Offset | Field | Type | Min | Max | Default | Unit | Go v2.0 |
|---|---|---|---|---|---|---|---|
| 0 | OV Prot Threshold | I2 | 4200 | 4600 | 4390 | mV | 4250 |
| 2 | OV Prot Delay | U1 | 0 | 5 | 1 | s | — |
| 3 | OV Prot Recovery | I2 | 4100 | 4500 | 4290 | mV | 4150 |
| 5 | UV Prot Threshold | I2 | 2300 | 3100 | 2800 | mV | 2600 |
| 7 | UV Prot Delay | U1 | 0 | 5 | 1 | s | — |
| 8 | UV Prot Recovery | I2 | 2400 | 3200 | 2900 | mV | 3100 |
| 10 | Body Diode Threshold | I2 | 0 | 100 | 60 | mA | — |
| 12 | OT Chg | I2 | 0 | 1200 | 550 | 0.1 °C | 450 |
| 14 | OT Chg Time | U1 | 0 | 60 | 5 | s | — |
| 15 | OT Chg Recovery | I2 | 0 | 1200 | 500 | 0.1 °C | 400 |
| 17 | OT Dsg | I2 | 0 | 1200 | 600 | 0.1 °C | 600 |
| 19 | OT Dsg Time | U1 | 0 | 60 | 5 | s | — |
| 20 | OT Dsg Recovery | I2 | 0 | 1200 | 550 | 0.1 °C | 550 |

UV Prot is not where the device stops. PowerService ships the unit at 2.8 V so
the user gets a reason on screen and storage closes cleanly, and the gauge's
trip is the backstop for when that does not happen. 2600 mV keeps 260 mV over
the hardware UVP (2340 mV once OVP code 000 is programmed) while leaving 200 mV
under the firmware's own trip, so the gauge cannot cut the pack out from under a
shutdown already in progress. It also stays 100 mV above the cell's 2500 mV
end-of-discharge rating.

Recovery sits far above the trip on purpose. This layer re-closes the DSG FET on
`Voltage()` alone, with no charger required:

> "The FET is re-enabled and the flag cleared after Voltage() rises back above
> UV Prot Recovery." (TRM p.67 §5.3.1.2)

A narrow band therefore oscillates: the trip fires at the loaded voltage, the
unloaded cell springs back past recovery, the FET closes, the load returns and
it trips again. Each cycle is a brownout of the host. 3100 mV is past where an
empty cell rests, which makes the trip a latch until the pack is charged.

### Subclass 39 JEITA

Temperature ranges used by charge inhibit / suspend (TRM p.61, §2.6.1).

| Offset | Field | Type | Default | Unit | Go v2.0 |
|---|---|---|---|---|---|
| 0 | T1 Temp | I1 | 0 | °C | — |
| 1 | T2 Temp | I1 | 10 | °C | — |
| 2 | T3 Temp | I1 | 45 | °C | — |
| 3 | T4 Temp | I1 | 50 | °C | — |
| 4 | T5 Temp | I1 | 60 | °C | — |
| 5 | Temp Hys | I1 | 1 | °C | — |
| 6–12 | T1–T2 … T4–T5 Chg Voltage | I2 | 4350 / 4350 / 4300 / 4250 | mV | — |
| 14–17 | T1–T2 … T4–T5 Chg Current | U1 | 50 / 80 / 80 / 80 | % | — |

### Subclass 48 Data

| Offset | Field | Type | Default | Unit | Go v2.0 |
|---|---|---|---|---|---|
| 8 | Cycle Count | U2 | 0 | count | — |
| 12 | Design Capacity | I2 | 1000 | mAh | 2600 |
| 14 | Design Energy | I2 | 3800 | mWh | 9620 |

### Subclass 57 Integrity Data

| Offset | Field | Type | Max | Default | Go v2.0 |
|---|---|---|---|---|---|
| 6 | All DF Checksum | H2 | 0x7FFF | 0x0000 | — |
| 8 | Static Chem DF Checksum | H2 | 0x7FFF | 0x7C23 | — |
| 10 | Static DF Checksum | H2 | 0x7FFF | 0x0000 | — |
| 12 | Prot Checksum | H2 | 0x7FFF | 0x0011 | 0x000A (see [Protector Checksum](#protector-checksum)) |

### Subclass 64 Registers

All six fields share block 0 (TRM p.62, p.81–84).

| Offset | Field | Type | Default | Go v2.0 |
|---|---|---|---|---|
| 0 | Pack Configuration (A) | H2 | 0x097F | — |
| 2 | Pack Configuration B | H1 | 0xA7 | — |
| 3 | Pack Configuration C | H1 | 0xB9 | — |
| 4 | Pack Configuration D | H1 | 0x83 | 0xB3 |
| 5 | Prot OC Config | H1 | 0x0A | — |
| 6 | Prot OV Config | H1 | 0x07 | 0x00 |

### Subclass 68 Power

| Offset | Field | Type | Default | Unit | Go v2.0 |
|---|---|---|---|---|---|
| 0 | Flash Update OK Voltage | I2 | 2800 | mV | — |
| 2 | Sleep Current | I2 | 15 | mA | 50 |

### Subclass 80 IT Cfg

| Offset | Field | Type | Default | Unit | Go v2.0 |
|---|---|---|---|---|---|
| 0 | Load Select | U1 | 1 | — | — |
| 1 | Load Mode | U1 | 1 | — | — |
| 64 | Terminate Voltage | I2 | 3000 | mV | 3000 |

### Subclass 82 State

| Offset | Field | Type | Min | Max | Default | Unit | Go v2.0 |
|---|---|---|---|---|---|---|---|
| 0 | Qmax Cell 0 | I2 | 0 | 14500 | 1000 | mAh | — (target 2600, not written) |
| 2 | Update Status | H1 | 0x00 | 0x06 | 0x00 | — | — (gauge-owned) |
| 3 | V at Chg Term | I2 | 0 | 5000 | 4350 | mV | — |

### Subclass 88 Ra0

Offset 0 holds the table flag, offsets 2–30 hold Ra 0…14 as I2. The driver
reads the table for learning diagnostics and never writes it.

### Subclass 112 Codes

Offset 0 is the sealed-to-unsealed key (H4, default `0x36720414`), offset 4
the unsealed-to-full key (`0xFFFFFFFF`). Neither is changed on the board.

## Pack Configuration D

Subclass 64 offset 4, default `0x83` (TRM p.83 Table 5-18):

| Bit | Name | Default | Meaning |
|---|---|---|---|
| 7 | OTFET | 1 | `SafetyStatus()[OTC]` opens CHG FET, `[OTD]` opens DSG FET |
| 6 | FCFET | 0 | `Flags()[FC]` opens CHG FET at charge termination |
| 5 | CIFET | 0 | `Flags()[CHG_INH]` opens CHG FET (charge inhibit) |
| 4 | CSFET | 0 | `Flags()[CHG_SUS]` opens CHG FET (charge suspend) |
| 3 | SMSYNCEN | 0 | SOC smoothing equalises immediately in relax |
| 2 | RSVD | 0 | must stay 0 |
| 1 | IMAXRESRVEN | 1 | Reserve Capacity counts in `Imax()` |
| 0 | IMAXEN | 1 | `Imax()` reporting enabled |

Charge inhibit and charge suspend are JEITA conditions (TRM p.35 §2.6.2–3):

- Inhibit (`CHG_INH`): `Temperature()` < T1 or > T4 while not charging.
- Suspend (`CHG_SUS`): `Temperature()` < T1 or > T5 while charging.

With the factory value both bits are 0, so outside T1–T5 the gauge only
reports: it sets the flag and forces `ChargingCurrent()` and
`ChargingVoltage()` to 0, and relies on the charger to obey. The charger on
this board does not read those registers. `0xB3` (CIFET + CSFET set,
everything else unchanged) makes the gauge open the CHG FET itself below 0 °C
and above T4/T5, so the pack refuses a cold charge even if the charger's own
NTC path is wrong.

## Hardware Protector

### Overvoltage Code and Paired Undervoltage

Prot OV Config, subclass 64 offset 6. Bits 7:3 are reserved, bits 2:0 are
`OVP[2:0]`. The undervoltage threshold is not separately programmable; each
OVP code fixes a UVP pair (TRM p.20 Table 2-1, DS p.4–5 Table 1).

| OVP code | V_OVP | Paired V_UVP |
|---|---|---|
| 000 | 4.275 V | 2.340 V |
| 001 | 4.300 V | 2.354 V |
| 010 | 4.325 V | 2.368 V |
| 011 | 4.350 V | 2.381 V |
| 100 | 4.375 V | 2.395 V |
| 101 | 4.400 V | 2.409 V |
| 110 | 4.425 V | 2.422 V |
| 111 (default) | 4.450 V | 2.438 V |

Release levels are fixed offsets: V_OVPREL = V_OVP − 0.215 V, V_UVPREL =
V_UVP + 0.105 V (DS p.8–9 §6.9–6.10). Code 000 is the lowest available and is
the target for a 4.20 V cell.

### Overcurrent Codes

Prot OC Config, subclass 64 offset 5, default `0x0A`: bits 7:6 reserved,
bit 5 `SCD`, bits 4:2 `OCD[2:0]`, bits 1:0 `OCC[1:0]` (TRM p.83 Table 5-19).
Currents are for the board's 5 mΩ sense resistor.

| Field | Code | Threshold | Current at 5 mΩ |
|---|---|---|---|
| OCC | 00 / 01 / **10** / 11 | 6 / 13 / **18** / 28 mV | 1.2 / 2.6 / **3.6** / 5.6 A |
| OCD | 000 / 001 / **010** / 011 | 14 / 24 / **34** / 44 mV | 2.8 / 4.8 / **6.8** / 8.8 A |
| OCD | 100 / 101 / 110 / 111 | 53 / 63 / 73 / 83 mV | 10.6 / 12.6 / 14.6 / 16.6 A |
| SCD | **0** / 1 | **73** / 148 mV | **14.6** / 29.6 A |

Bold is the factory default. The datasheet's combined threshold table shifts
the OCC/OCD/SCD millivolt values by about ±1 mV per OVP row; the numbers
above are the default-OVP row. Retuning these codes waits on a 4-wire check
of R8 and a measured system peak current.

### Fixed Delays

None of the protector delays is a data-flash field (DS p.5 Table 2):

| Fault | Delay |
|---|---|
| OVP | 1.00 s |
| UVP | 31.25 ms |
| OCC | 7.81 ms |
| OCD | 31.25 ms |
| SCD | 312.5 µs |

### Protector State Machine

TRM p.21–24 §2.2.2.2, DS p.18–20 §7.4:

| State | Entered when | FET action | Leaves when |
|---|---|---|---|
| NORMAL | reset done, no fault | both closed | — |
| OVP fault | V_BAT > V_OVP for t_OVP | CHG open | charger removed (PACK+ > 300 mV below cell) and cell < V_OVPREL |
| UVP fault | V_BAT < V_UVP for t_UVP, or power-on reset | DSG open | charger detected and cell > V_UVPREL |
| OCC fault | V_SRP − V_SRN > V_OCC for t_OCC | CHG open | charger removed |
| OCD / SCD fault | V_SRN − V_SRP > V_OCD or V_SCD | DSG open | load removed (PACK+ within 300 mV of cell) |
| Shutdown wait → analog shutdown | host `SET_SHUTDOWN`, or V_BAT below Shutdown V in sleep | both open, then LDO off | charger attached |
| Low-voltage charging | leaving analog shutdown with V_LVDET < V_BAT < POR | CHG gate tied to PACK+, DSG open | cell reaches POR |

Power-on reset lands in the UVP fault state, so a freshly inserted cell does
not discharge until a charger has been seen once. There is no register that
disables this; it belongs in the production and repair procedure.

### Two Protection Levels

The firmware Safety subclass acts first and the hardware protector is the
backstop ("two levels of safety", DS p.17 §7.3.4.1). Firmware cannot force a
FET closed while the hardware protector holds it open (TRM p.22). On the
board:

The host shuts down before any of this: PowerService requests ship mode at
2.8 V on a board whose gauge has a protector, 2.9 V on one whose gauge does not.

| Direction | Firmware trips at | Hardware trips at |
|---|---|---|
| Overvoltage | 4250 mV after 1 s | 4450 mV after 1 s (4275 mV once OVP code 000 is written) |
| Undervoltage | 2600 mV after 1 s | 2438 mV after 31.25 ms (2340 mV once OVP code 000 is written) |
| Over-temperature charge | 45.0 °C after 5 s | none (temperature is firmware-only) |

### Status Registers

`SafetyStatus()` `0x1A`/`0x1B`, firmware faults only (TRM p.51 Table 4-5).
High byte reserved. Low byte:

| Bit 7 | Bit 6 | Bit 5 | Bit 4 | Bit 3 | Bit 2 | Bit 1 | Bit 0 |
|---|---|---|---|---|---|---|---|
| INV_PROT_CHKSUM | RSVD | ISD | TDD | OTC | OTD | OVP | UVP |

`ProtectorStatus()` `0x6D`, register AFESTAT1, hardware faults and FET
override state (TRM p.55 Table 4-7):

| Bit 7 | Bit 6 | Bit 5 | Bit 4 | Bit 3 | Bit 2 | Bit 1 | Bit 0 |
|---|---|---|---|---|---|---|---|
| CHG_OFF | DSG_OFF | CVM | UVP | OVP | SCD | OCD | OCC |

`ProtectorState()` `0x78`, register AFESTATE, one bit per active state
(TRM p.57 Table 4-9):

| Bit 7 | Bit 6 | Bit 5 | Bit 4 | Bit 3 | Bit 2 | Bit 1 | Bit 0 |
|---|---|---|---|---|---|---|---|
| RSVD | RSVD | SHUTDWNW | OVP | OCD_SCD | OCC | NORMAL | HOLD |

`LT AFE Status` (subclass 60 offset 2) polls `ProtectorStatus()` every second
and ORs the hardware fault bits for the life of the gauge (TRM p.81). There is
no equivalent sticky record for firmware faults. A healthy bench boot reads
`SafetyStatus() = 0x0000`, `ProtectorStatus() = 0x00`, `ProtectorState()`
NORMAL.

## Protector Checksum

Prot Checksum (subclass 57 offset 12) guards Prot OV Config and Prot OC
Config. The gauge compares it every second against the plain 16-bit sum of
the two bytes; on a mismatch it sets `SafetyStatus()[INV_PROT_CHKSUM]` and
holds both FETs open until the stored value is corrected (TRM p.20 §2.2.2.1,
p.79 §5.3.8.4). The factory value checks out: `0x0A + 0x07 = 0x11`. This is a
different mechanism from the one's-complement `BlockDataCheckSum()` that
commits a block.

`Control(0x001A)` recomputes the sum, masks it to 15 bits, and returns it with
bit 15 set when it differs from the stored field (TRM p.48 §4.1.1.18). Wait
100 ms before reading the result.

### Changing Subclass 64

The sequence the chip requires when Pack Config D or Prot OV Config changes:

1. Read subclass 64 block 0 and subclass 57 block 0 and keep both as a
   baseline.
2. Modify only bytes 4 and 6 of the subclass 64 copy. Byte 5 must read back
   unchanged before the block is committed.
3. Write the block and its `BlockDataCheckSum()`; wait 100 ms; read it back.
4. Compute `Prot OC Config + Prot OV Config` and write it to subclass 57
   offset 12 through the same read-modify-write, preserving offsets 6, 8, 10.
5. Send `Control(0x001A)`, wait 100 ms, read: bit 15 must be 0 and bits 14:0
   must equal the value written.
6. Confirm `SafetyStatus()` bit 7 is clear and `ProtectorStatus()` reports
   neither `CHG_OFF` nor `DSG_OFF`.

Keep USB attached for the whole sequence: if the checksum is wrong the pack
opens both FETs within a second and the MCU survives only on VBUS.

| Failure | Gauge behaviour | Reference |
|---|---|---|
| Wrong `BlockDataCheckSum()` | not reported; block may not commit | TRM p.54, p.58 |
| Prot Checksum stale | both FETs held open within 1 s, `INV_PROT_CHKSUM` set | TRM p.20, p.79 |
| Power lost mid-write | copy buffer restores on next access, +44 ms stretch; end value not guaranteed | TRM p.42 |
| Gauge sealed | `0x3E` and `0x61` unavailable, sequence cannot start | TRM p.53 |
| `Voltage()` < Flash Update OK Voltage, no charger | write silently skipped | TRM p.38 |

## Impedance Track Learning

Update Status (subclass 82 offset 2) is owned by the gauge (TRM p.97
§5.5.3.2):

| Bit | Value | Meaning |
|---|---|---|
| 2 | 0x04 | Impedance Track enabled |
| 1 | 0x02 | optimised Qmax and Ra learned (full cycle done) |
| 0 | 0x01 | initial Qmax learned (charge + rest done) |

A learning cycle moves it `0x00 → 0x04` (`IT_ENABLE`) `→ 0x05` (after charge
and relaxation) `→ 0x06` (after discharge and relaxation). A golden image is
taken at `0x06`; TI's tooling then clears bit 2 so shipped units read `0x02`.
`IT_ENABLE` also sets `CONTROL_STATUS[QEN]`, which cannot be cleared, so Qmax
Cell 0 and every protection field must be final before it is sent. Do not
write Update Status by hand.

Bench state: Update Status `0x00`, FCC 747 mAh, no learned data. SOC from
this gauge is not meaningful until a learning cycle completes. The cycle
procedure and the runner rewrite are tracked in the v2.0 bring-up plan.

## Firmware Coverage

Driver: [`bq27742.h`](../../../components/airgradient-bms/drivers/bq27742/bq27742.h).
Board wiring: [`go_hardware_board.cpp`](../main/go_hardware_board.cpp).

| Capability | Driver API | Board rev 2.0 today |
|---|---|---|
| Protector config | `read_protector_config()`, `write_protector_config()` | written from `init_bms()` when an adapter is present and the values differ; verified by read-back, `PROTECTOR_CHKSUM`, then FET state |
| Identity probe | `init()` checks DEVICE_TYPE; `_log_identity_diagnostics()` on mismatch | runs every boot |
| Runtime reads | `read_soc_percent()`, `read_voltage_mv()`, `read_average_current_ma()`, `read_temperature_c()`, `read_flags()` | polled every 30 s |
| Average power | `read_average_power_mw()` derives V × I | `AveragePower()` `0x76` not used yet |
| Safety subclass | `read_protection_config()`, `write_protection_config()` | written at boot when different; U1 delay fields preserved; out-of-range rejected; read back and logged |
| Cell config | `read_cell_config()`, `write_cell_config()` | written at boot when different (2600 / 9620 / 3000 / 50); no `RESET` |
| Status | `read_safety_status()`, `read_protector_status()`, `read_protector_state()` | logged at boot |
| Learning reads | `read_qmax_cell0()`, `read_ra_table()`, `read_update_status()`, `read_control_status()` | available; runner still targets BQ27427 |
| `IT_ENABLE` | `set_update_status_learning(true)` | not sent on rev 2.0 |
| `PROTECTOR_CHKSUM` subcommand | `control_subcommand(0x001A, …)` | used to verify the protector write |
| Qmax Cell 0 write | none | not written |

All three boot-time writes are idempotent: on the next boot the board logs
"already correct — preserved" for each and does not touch flash.

The protector write runs from `init_bms()`, not `init_fuel_gauge()`. Every
boot path brings the gauge up before the charger, so that is the first point
where the adapter state the write depends on can be read at all.
