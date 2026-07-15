#!/usr/bin/env python3
"""AirGradient Go thermal-calibration collector.

Subscribes to the Cal telemetry characteristic of one or two Go units
(DUT in enclosure + bare reference board) and merges every notification
into a single CSV, timestamped on arrival by the host clock so the two
devices need no time sync of their own.

The Cal characteristic (d1c0c0a5-...) is unauthenticated by design: no
pairing/passkey is required, so the screenless reference board works.

Usage:
    pip install bleak cbor2
    python cal_collector.py --dut "AirGradient Go ef0e" --ref "AirGradient Go 12ab"
    python cal_collector.py --dut AA:BB:CC:DD:EE:FF --out run1.csv

Stop with Ctrl-C. Devices that drop are reconnected automatically.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import logging
import sys
from datetime import datetime, timezone
from pathlib import Path

import cbor2
from bleak import BleakClient, BleakScanner

logger = logging.getLogger("cal_collector")

CAL_CHAR_UUID = "d1c0c0a5-6b48-4b2a-9b1d-59f9f2b0a1e1"

# Column order for the CSV; CBOR keys not in this list are appended as extras.
CAL_KEYS = [
    "t", "h", "tdps", "pres", "tfg", "tdie", "tbat",
    "ibat", "ichg", "ibus", "vbus", "vbat", "chg", "gps", "up", "ts",
]
CSV_COLUMNS = ["host_ts", "role", "address"] + CAL_KEYS

RECONNECT_DELAY_S = 5.0
SCAN_TIMEOUT_S = 15.0


class CsvSink:
    """Serialized CSV writer shared by both device tasks."""

    def __init__(self, path: Path) -> None:
        self._path = path
        new_file = not path.exists()
        self._file = path.open("a", newline="")
        self._writer = csv.DictWriter(self._file, fieldnames=CSV_COLUMNS, extrasaction="ignore")
        if new_file:
            self._writer.writeheader()
            self._file.flush()

    def write(self, role: str, address: str, sample: dict) -> None:
        row = {"host_ts": datetime.now(timezone.utc).isoformat(), "role": role, "address": address}
        row.update(sample)
        self._writer.writerow(row)
        self._file.flush()

    def close(self) -> None:
        self._file.close()


def _looks_like_address(target: str) -> bool:
    return ":" in target or ("-" in target and len(target) >= 36)


async def resolve_targets(targets: list[tuple[str, str]]) -> dict[str, str]:
    """Resolve all name targets to BLE addresses with a single shared scan.

    One scan pass at a time (concurrent CoreBluetooth scans misbehave on
    macOS); rescans until every named device has been seen. Targets that
    already look like an address pass through unchanged.
    """
    resolved: dict[str, str] = {}
    pending = {role: t for role, t in targets}
    for role, t in list(pending.items()):
        if _looks_like_address(t):
            resolved[role] = t
            del pending[role]

    while pending:
        logger.info("scanning for %s...", ", ".join(f"'{t}'" for t in pending.values()))
        devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT_S)
        for role, t in list(pending.items()):
            matches = [d for d in devices if d.name and t.lower() in d.name.lower()]
            if len(matches) > 1:
                names = ", ".join(f"{d.name} ({d.address})" for d in matches)
                raise RuntimeError(f"ambiguous name '{t}': {names}")
            if matches:
                resolved[role] = matches[0].address
                logger.info("resolved '%s' -> %s (%s)", t, matches[0].address, matches[0].name)
                del pending[role]
        if pending:
            logger.warning("not found yet: %s — rescanning", ", ".join(pending.values()))
    return resolved


async def stream_device(role: str, address: str, sink: CsvSink) -> None:
    """Connect, subscribe, and re-connect forever. One task per device."""
    while True:
        disconnected = asyncio.Event()
        try:
            def on_disconnect(_client: BleakClient) -> None:
                disconnected.set()

            async with BleakClient(address, disconnected_callback=on_disconnect) as client:
                def on_notify(_char, data: bytearray) -> None:
                    try:
                        sample = cbor2.loads(bytes(data))
                    except Exception:
                        logger.warning("[%s] undecodable notification (%d bytes)", role, len(data))
                        return
                    sink.write(role, address, sample)
                    summary = " ".join(f"{k}={sample[k]}" for k in ("t", "tdps", "chg") if k in sample)
                    logger.info("[%s] %s", role, summary or sample)

                await client.start_notify(CAL_CHAR_UUID, on_notify)
                logger.info("[%s] subscribed to %s", role, address)
                await disconnected.wait()
                logger.warning("[%s] disconnected", role)
        except Exception as exc:
            logger.warning("[%s] connection error: %s", role, exc)

        logger.info("[%s] reconnecting in %.0fs...", role, RECONNECT_DELAY_S)
        await asyncio.sleep(RECONNECT_DELAY_S)


async def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dut", help="DUT device: advertised name (substring) or BLE address")
    parser.add_argument("--ref", help="Reference device: advertised name (substring) or BLE address")
    parser.add_argument("--out", type=Path, default=Path("thermal_cal.csv"), help="Output CSV (appended)")
    args = parser.parse_args()

    targets = [(role, t) for role, t in (("dut", args.dut), ("ref", args.ref)) if t]
    if not targets:
        parser.error("give at least one of --dut / --ref")

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

    sink = CsvSink(args.out)
    logger.info("writing to %s", args.out)
    try:
        addresses = await resolve_targets(targets)
        await asyncio.gather(
            *(stream_device(role, addresses[role], sink) for role, _ in targets))
    except asyncio.CancelledError:
        pass
    finally:
        sink.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(asyncio.run(main()))
    except KeyboardInterrupt:
        print("\nstopped")
