#!/usr/bin/env python3
"""Download every AirGradient Go measurement log as a separate CSV over BLE.

Install: pip install bleak cbor2
Run:     python scripts/ago_export_measurements.py
Optional: --address AA:BB:CC:DD:EE:FF --output-dir ./measurements

Pair Go with the computer first. Keep Go unlocked throughout the export,
including between files. Exit WiFi provisioning setup before connecting.
Logs remain on the device; settings and tracking are not changed.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import math
import struct
import sys
from contextlib import suppress
from datetime import datetime, timezone
from pathlib import Path

import cbor2
from bleak import BleakClient, BleakScanner

HISTORY_UUID = "d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1"
CONFIG_UUID = "d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1"
RECORD = struct.Struct("<q5f3hfB")  # Version 1, 39 bytes; no GPS fields.
FILL_BATCH_SIZE = 50
MAX_FILL_ROUNDS = 5
NOTIFY_TIMEOUT = 30.0
COLUMNS = (
    "timestamp", "temperature_c", "humidity_pct", "pm1_ugm3", "pm25_ugm3",
    "pm10_ugm3", "co2_ppm", "tvoc_index", "nox_index", "pressure_hpa", "battery_pct",
)
INVALID = {
    "temperature_c": -1000.0, "humidity_pct": -1.0,
    "pm1_ugm3": -1.0, "pm25_ugm3": -1.0, "pm10_ugm3": -1.0,
    "co2_ppm": -1, "tvoc_index": -1, "nox_index": -1,
    "pressure_hpa": -1.0, "battery_pct": 255,
}


class History:
    def __init__(self, client: BleakClient, timeout: float = NOTIFY_TIMEOUT):
        self.client = client
        self.timeout = timeout
        self.queue: asyncio.Queue[bytes] = asyncio.Queue()

    def notify(self, _sender, data: bytearray):
        self.queue.put_nowait(bytes(data))

    async def send(self, op: str, **fields):
        await self.client.write_gatt_char(
            HISTORY_UUID, cbor2.dumps({"op": op, **fields}), response=True,
        )

    async def receive(self):
        try:
            packet = await asyncio.wait_for(self.queue.get(), self.timeout)
        except TimeoutError as exc:
            raise RuntimeError(
                "BLE response timed out. Keep Go unlocked and within range."
            ) from exc
        if not packet:
            raise RuntimeError("Empty history notification")
        if packet[0] == 0:
            message = cbor2.loads(packet[1:])
            if not isinstance(message, dict):
                raise RuntimeError("Invalid history response")
            if message.get("type") == "error":
                raise RuntimeError(f"Go reported: {message.get('err')}")
            return message
        if packet[0] == 1 and len(packet) > 3 and (len(packet) - 3) % RECORD.size == 0:
            index = struct.unpack_from("<H", packet, 1)[0]
            return index, packet[3:]
        raise RuntimeError("Invalid measurement-log packet or unsupported record format")

    async def control(self, expected: str):
        message = await self.receive()
        if not isinstance(message, dict) or message.get("type") != expected:
            raise RuntimeError(f"Expected '{expected}', received {message!r}")
        return message

    async def list_all(self):
        logs = []
        after = 0
        while True:
            await self.send("list", kind="measurements", after=after)
            page = await self.control("list")
            entries = page.get("logs")
            cursor = page.get("next")
            if page.get("kind") != "measurements" or not isinstance(entries, list):
                raise RuntimeError("Invalid measurement-log listing")
            if type(cursor) is not int or cursor < 0 or cursor > 0xFFFFFFFF:
                raise RuntimeError("Invalid measurement-log cursor")
            if cursor == 0:
                if entries:
                    raise RuntimeError("Unexpected entries at the end of the log listing")
                return logs
            if cursor <= after or not entries:
                raise RuntimeError("Measurement-log listing did not advance")
            previous = after
            for entry in entries:
                sequence = entry.get("session")
                if type(sequence) is not int or not previous < sequence <= cursor:
                    raise RuntimeError("Invalid measurement-log sequence")
                if entry.get("version") != 1:
                    raise RuntimeError("Unsupported measurement-log version")
                logs.append(entry)
                previous = sequence
            if previous != cursor:
                raise RuntimeError("Measurement-log cursor does not match its entries")
            after = cursor

    async def collect_pass(self, total: int, records: dict[int, bytes]):
        while True:
            message = await self.receive()
            if isinstance(message, dict):
                if message.get("type") != "done":
                    raise RuntimeError(f"Unexpected download response: {message!r}")
                return
            first, payload = message
            for offset in range(0, len(payload), RECORD.size):
                index = first + offset // RECORD.size
                if index >= total:
                    raise RuntimeError("Received record outside the download snapshot")
                record = payload[offset:offset + RECORD.size]
                if index in records and records[index] != record:
                    raise RuntimeError("A record changed during download")
                records[index] = record

    async def download(self, sequence: int):
        records: dict[int, bytes] = {}
        ended = False
        try:
            await self.send("start", kind="measurements", session=sequence)
            started = await self.control("started")
            if (started.get("kind") != "measurements" or started.get("version") != 1
                    or started.get("pt_size") != RECORD.size
                    or started.get("session") != sequence):
                raise RuntimeError("Unsupported measurement-log download format")
            total = started.get("total")
            if type(total) is not int or not 0 <= total <= 65535:
                raise RuntimeError("Invalid download record count")
            await self.collect_pass(total, records)
            for _ in range(MAX_FILL_ROUNDS):
                missing = [i for i in range(total) if i not in records]
                if not missing:
                    break
                for offset in range(0, len(missing), FILL_BATCH_SIZE):
                    await self.send("fill", pts=missing[offset:offset + FILL_BATCH_SIZE])
                    await self.collect_pass(total, records)
            if len(records) != total:
                raise RuntimeError(f"Log {sequence}: {total - len(records)} records still missing")
            await self.send("end")
            await self.control("ended")
            ended = True
            return [records[i] for i in range(total)]
        finally:
            if not ended and self.client.is_connected:
                # Release the device's file protection even after a failed export.
                with suppress(Exception):
                    await self.send("end")


def write_csv(directory: Path, sequence: int, records: list[bytes]) -> Path:
    path = directory / f"AGo_measurements_{sequence:010d}.csv"
    temporary = path.with_suffix(".csv.tmp")
    try:
        with temporary.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(
                output, fieldnames=["log_sequence", "record_index", "datetime_utc", *COLUMNS],
            )
            writer.writeheader()
            for index, record in enumerate(records):
                row = dict(zip(COLUMNS, RECORD.unpack(record)))
                timestamp = row["timestamp"]
                utc = ""
                if timestamp != 0:
                    with suppress(ValueError, OverflowError, OSError):
                        utc = datetime.fromtimestamp(timestamp, timezone.utc).isoformat()
                for field, value in row.items():
                    if value == INVALID.get(field) or (isinstance(value, float) and not math.isfinite(value)):
                        row[field] = ""
                writer.writerow({"log_sequence": sequence, "record_index": index,
                                 "datetime_utc": utc, **row})
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)
    return path


async def run(address: str | None, directory: Path):
    target = address
    if target is None:
        print("Scanning for AirGradient Go…", flush=True)
        target = await BleakScanner.find_device_by_filter(
            lambda device, adv: (adv.local_name or device.name or "").startswith("AirGradient Go "),
            timeout=10.0,
        )
        if target is None:
            raise RuntimeError("No Go found. Unlock Go, or supply --address.")
    print("Connecting… (Go must already be paired with this computer)", flush=True)
    async with BleakClient(target, timeout=30.0) as client:
        config = cbor2.loads(bytes(await client.read_gatt_char(CONFIG_UUID)))
        if not isinstance(config, dict) or config.get("log_v") != 1:
            raise RuntimeError("This firmware does not support measurement-log format v1")
        history = History(client)
        await client.start_notify(HISTORY_UUID, history.notify)
        try:
            logs = await history.list_all()
            print(f"Found {len(logs)} measurement log(s).", flush=True)
            if not logs:
                return
            directory.mkdir(parents=True, exist_ok=True)
            for number, log in enumerate(logs, 1):
                sequence = log["session"]
                print(f"[{number}/{len(logs)}] Downloading log {sequence}…", flush=True)
                records = await history.download(sequence)
                path = write_csv(directory, sequence, records)
                print(f"  Saved {len(records)} measurements: {path}", flush=True)
            print("All measurement logs downloaded.")
        finally:
            if client.is_connected:
                with suppress(Exception):
                    await client.stop_notify(HISTORY_UUID)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--address", help="BLE address or platform identifier; otherwise auto-scan")
    parser.add_argument("-o", "--output-dir", type=Path,
                        default=Path(".export") / "measurements" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"),
                        help="CSV destination (default: a new folder under .export/measurements)")
    args = parser.parse_args()
    try:
        asyncio.run(run(args.address, args.output_dir))
    except KeyboardInterrupt:
        print("Export interrupted.", file=sys.stderr)
        return 1
    except Exception as exc:
        print(f"Export failed: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
