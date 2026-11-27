#!/usr/bin/env python3
# /// script
# requires-python = ">=3.11"
# dependencies = ["bleak>=0.22", "cbor2>=5.6"]
# ///
"""Download Go measurement logs over BLE, one CSV per log.

Run:
    uv run scripts/ago_export_measurements.py --address AA:BB:CC:DD:EE:FF -o exports

Unlock Go to make it connectable. Pair with the host first, or use --pair
and enter Go's displayed passkey in the OS pairing prompt. On macOS, the OS
handles pairing when the protected characteristic is accessed.

Uses History kind="measurements"; does not change settings or delete logs.
CSV readings remain raw. Invalid readings are blank; timestamp 0 means unknown.
Existing CSV files are preserved unless --overwrite is supplied.
"""

from __future__ import annotations

import argparse
import asyncio
import csv
from datetime import datetime, timezone
import logging
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
from typing import Any

HISTORY_UUID = "d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1"
RECORD = struct.Struct("<q5f3hfB")  # Version 1: 39 bytes, no GPS fields.
MAX_FILL = 50
MAX_WRITE = 256
MAX_FILL_ROUNDS = 5
FIELDS = (
    "timestamp", "temperature_c", "humidity_pct", "pm1_ugm3", "pm25_ugm3",
    "pm10_ugm3", "co2_ppm", "tvoc_index", "nox_index", "pressure_hpa",
    "battery_pct",
)
SENTINELS = (-1001.0, -1.0, -1.0, -1.0, -1.0, -1, -1, -1, -1001.0, 255)
logger = logging.getLogger("ago_export_measurements")


class History:
    def __init__(self, client: Any, cbor: Any, timeout: float, mtu: int) -> None:
        self.client = client
        self.cbor = cbor
        self.timeout = timeout
        self.write_limit = min(MAX_WRITE, mtu - 3)
        self.queue: asyncio.Queue = asyncio.Queue()

    def notify(self, _sender: Any, data: bytearray) -> None:
        self.queue.put_nowait(bytes(data))

    def disconnected(self, _client: Any) -> None:
        self.queue.put_nowait(RuntimeError("BLE disconnected; rerun to retry the download"))

    async def send(self, op: str, **fields: Any) -> None:
        data = self.cbor.dumps({"op": op, **fields})
        if len(data) > self.write_limit:
            raise RuntimeError("History command exceeds negotiated write size")
        logger.debug("WRITE %s %s", op, fields)
        await asyncio.wait_for(
            self.client.write_gatt_char(HISTORY_UUID, data, response=True),
            self.timeout,
        )

    async def receive(self) -> dict | bytes:
        try:
            data = await asyncio.wait_for(self.queue.get(), self.timeout)
        except TimeoutError as exc:
            raise RuntimeError("History notification timed out; download incomplete") from exc
        if isinstance(data, Exception):
            raise data
        if not data:
            raise RuntimeError("Empty History notification")
        if data[0] == 1:
            return data
        if data[0] != 0:
            raise RuntimeError(f"Unknown History tag: {data[0]}")
        message = self.cbor.loads(data[1:])
        if not isinstance(message, dict):
            raise RuntimeError("Expected a History CBOR map")
        logger.debug("NOTIFY %s", message)
        if message.get("type") == "error":
            raise RuntimeError(f"Device error: {message.get('err')}")
        return message

    async def control(self, expected: str) -> dict:
        message = await self.receive()
        if not isinstance(message, dict) or message.get("type") != expected:
            raise RuntimeError(f"Expected History {expected}, got {message!r}")
        return message

    async def list_logs(self) -> list[dict]:
        logs = []
        after = 0
        while True:
            await self.send("list", kind="measurements", after=after)
            message = await self.control("list")
            entries = message.get("logs")
            cursor = message.get("next")
            if message.get("kind") != "measurements" or not isinstance(entries, list):
                raise RuntimeError("Invalid measurement list response")
            if not entries and cursor == 0:
                return logs
            if (type(cursor) is not int or not after < cursor <= 0xFFFFFFFF
                    or len(entries) != 1 or not isinstance(entries[0], dict)
                    or entries[0].get("session") != cursor):
                raise RuntimeError("Invalid measurement list cursor")
            logs.extend(entries)
            after = cursor

    async def collect(self, records: dict[int, tuple], total: int) -> None:
        """Collect one pass through done; time out on stalled notifications."""
        while True:
            message = await self.receive()
            if isinstance(message, dict):
                if message.get("type") != "done":
                    raise RuntimeError(f"Unexpected History response: {message}")
                return
            if len(message) < 3 + RECORD.size or (len(message) - 3) % RECORD.size:
                raise RuntimeError("Malformed measurement record notification")
            first = int.from_bytes(message[1:3], "little")
            for offset in range(3, len(message), RECORD.size):
                index = first + (offset - 3) // RECORD.size
                if index >= total:
                    raise RuntimeError("Record index exceeds download snapshot")
                record = RECORD.unpack_from(message, offset)
                if index in records and RECORD.pack(*records[index]) != message[offset:offset + RECORD.size]:
                    raise RuntimeError(f"Conflicting data for record {index}")
                records[index] = record

    def fill_batches(self, indices: list[int]):
        batch: list[int] = []
        for index in indices:
            candidate = batch + [index]
            if len(candidate) > MAX_FILL or len(self.cbor.dumps({"op": "fill", "pts": candidate})) > self.write_limit:
                yield batch
                batch = [index]
            else:
                batch = candidate
        if batch:
            yield batch

    async def end(self) -> None:
        await self.send("end")
        # A failed pass can leave queued data before the end acknowledgement.
        async with asyncio.timeout(self.timeout):
            while True:
                message = await self.receive()
                if isinstance(message, dict) and message.get("type") == "ended":
                    return

    async def download(self, sequence: int) -> list[tuple]:
        try:
            await self.send("start", kind="measurements", session=sequence)
            started = await self.control("started")
            total = started.get("total")
            if (started.get("kind") != "measurements" or started.get("version") != 1
                    or started.get("pt_size") != RECORD.size
                    or started.get("session") != sequence
                    or type(total) is not int or not 0 <= total <= 65535):
                raise RuntimeError(f"Unsupported measurement download format: {started}")
            logger.info("Log %d: downloading %d records", sequence, total)
            records: dict[int, tuple] = {}
            await self.collect(records, total)
            for _ in range(MAX_FILL_ROUNDS):
                missing = [i for i in range(total) if i not in records]
                if not missing:
                    break
                logger.info("Log %d: recovering %d missing records", sequence, len(missing))
                for batch in self.fill_batches(missing):
                    await self.send("fill", pts=batch)
                    await self.collect(records, total)
            if len(records) != total:
                raise RuntimeError(f"Log {sequence}: {total - len(records)} records missing; CSV not saved")
        except BaseException:
            if self.client.is_connected:
                try:
                    await self.end()
                except Exception as exc:
                    logger.warning("Could not end failed download: %s", exc)
            raise
        await self.end()
        return [records[i] for i in range(total)]


def utc_text(timestamp: int) -> str:
    if timestamp == 0:
        return ""
    try:
        return datetime.fromtimestamp(timestamp, timezone.utc).isoformat()
    except (ValueError, OverflowError, OSError):
        return ""


def write_csv(path: Path, sequence: int, records: list[tuple], overwrite: bool) -> None:
    """Publish only complete CSV files; preserve existing exports by default."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", newline="", encoding="utf-8",
                                         dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            writer = csv.writer(stream)
            writer.writerow(["session", "record_index", "timestamp", "datetime_utc", *FIELDS[1:]])
            for index, record in enumerate(records):
                values = ["" if value == sentinel or not math.isfinite(value) else value
                          for value, sentinel in zip(record[1:], SENTINELS)]
                writer.writerow([sequence, index, record[0], utc_text(record[0]), *values])
        if overwrite:
            os.replace(temporary, path)
        else:
            os.link(temporary, path)  # Fails without overwriting if destination exists.
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


async def run(args: argparse.Namespace) -> None:
    import cbor2
    from bleak import BleakClient, BleakScanner

    device = args.address
    if not device:
        logger.info("Scanning for connectable Go devices...")
        found = await BleakScanner.discover(timeout=args.scan_timeout, return_adv=True)
        matches = [device for device, adv in found.values()
                   if (adv.local_name or device.name or "").startswith("AirGradient Go ")]
        if len(matches) != 1:
            addresses = ", ".join(device.address for device in matches)
            raise RuntimeError(f"Found {len(matches)} Go devices ({addresses}); specify --address")
        device = matches[0]

    history = None

    def disconnected(client):
        if history is not None:
            history.disconnected(client)

    async with BleakClient(device, disconnected_callback=disconnected, timeout=30) as client:
        if args.pair and sys.platform != "darwin":
            logger.info("Pairing: enter Go's passkey in the OS prompt")
            await client.pair()
        # Same BlueZ workaround as ago_ota_ble.py: mtu_size otherwise reports 23.
        acquire = getattr(getattr(client, "_backend", None), "_acquire_mtu", None)
        if acquire is not None:
            await acquire()
        mtu = client.mtu_size
        if mtu < 185:
            raise RuntimeError(f"MTU {mtu} is too small for this exporter; pair and reconnect (need >=185)")
        history = History(client, cbor2, args.notify_timeout, mtu)
        await client.start_notify(HISTORY_UUID, history.notify)
        try:
            logs = await history.list_logs()
            if args.list:
                for log in logs:
                    print(f"session={log['session']} count={log.get('count')} "
                          f"first_ts={log.get('first_ts')} last_ts={log.get('last_ts')} "
                          f"version={log.get('version')} boundary={log.get('boundary')}")
                logger.info("%d measurement logs", len(logs))
                return
            if args.session is not None:
                logs = [log for log in logs if log["session"] == args.session]
                if not logs:
                    raise RuntimeError(f"Measurement log {args.session} not found")
            if not logs:
                logger.info("No measurement logs to download")
                return
            output = Path(args.output_dir).expanduser()
            output.mkdir(parents=True, exist_ok=True)
            paths = [output / f"AGo_measurements_{log['session']:010d}.csv" for log in logs]
            if not args.overwrite:
                for path in paths:
                    if path.exists():
                        raise FileExistsError(f"{path} exists; choose another directory or use --overwrite")
            for log, path in zip(logs, paths):
                records = await history.download(log["session"])
                write_csv(path, log["session"], records, args.overwrite)
                logger.info("Saved %d records to %s", len(records), path)
        finally:
            if client.is_connected:
                await client.stop_notify(HISTORY_UUID)


def positive_seconds(value: str) -> float:
    result = float(value)
    if not math.isfinite(result) or result <= 0:
        raise argparse.ArgumentTypeError("must be a positive number of seconds")
    return result


def session_id(value: str) -> int:
    result = int(value)
    if not 1 <= result <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("must be a file sequence from 1 to 4294967295")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--address", help="BLE address or macOS device UUID; otherwise scan")
    parser.add_argument("--pair", action="store_true", help="request OS pairing before downloading")
    select = parser.add_mutually_exclusive_group()
    select.add_argument("--list", action="store_true", help="list logs without downloading")
    select.add_argument("--session", type=session_id, help="download only this file sequence")
    parser.add_argument("-o", "--output-dir", default=".", help="CSV destination directory")
    parser.add_argument("--overwrite", action="store_true", help="replace existing CSV files")
    parser.add_argument("--scan-timeout", type=positive_seconds, default=10.0)
    parser.add_argument("--notify-timeout", type=positive_seconds, default=30.0,
                        help="seconds without a notification before failing (default: 30)")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(message)s")
    logging.getLogger("bleak").setLevel(logging.WARNING)
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        logger.error("Export failed: %s", exc)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
