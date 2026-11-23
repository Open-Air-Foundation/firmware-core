"""Offline protocol checks; no Bluetooth adapter or device is used."""
import asyncio
import csv
import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

import cbor2

SCRIPT = Path(__file__).resolve().parents[1] / "ago_export_measurements.py"
spec = importlib.util.spec_from_file_location("export_measurements", SCRIPT)
export = importlib.util.module_from_spec(spec)
spec.loader.exec_module(export)


def record(timestamp=0):
    return export.RECORD.pack(timestamp, -1000, -1, -1, 12.5, 18, -1, 0, -1, -1, 255)


class FakeClient:
    is_connected = True

    def __init__(self, recover=True, version=1, silent=False):
        self.history = export.History(self, timeout=0.02)
        self.requests = []
        self.recover = recover
        self.version = version
        self.silent = silent
        self.records = [record(), record(1700000000), record(1700000010)]

    def control(self, message):
        self.history.notify(None, bytearray(b"\x00" + cbor2.dumps(message)))

    def binary(self, index):
        self.history.notify(None, bytearray(b"\x01" + struct.pack("<H", index) + self.records[index]))

    async def write_gatt_char(self, uuid, payload, response):
        assert uuid == export.HISTORY_UUID and response
        request = cbor2.loads(payload)
        self.requests.append(request)
        if self.silent:
            return
        op = request["op"]
        if op == "list":
            after = request["after"]
            sequence = {0: 3, 3: 8, 8: 0}[after]
            # Listing counts deliberately differ from the later download snapshot.
            logs = [{"session": sequence, "version": 1, "count": 2}] if sequence else []
            self.control({"type": "list", "kind": "measurements", "logs": logs, "next": sequence})
        elif op == "start":
            total = 3 if request["session"] == 3 else 0
            self.control({"type": "started", "kind": "measurements", "version": self.version,
                          "session": request["session"], "total": total, "pt_size": 39})
            if total:
                # Missing index 1, out-of-order packets, and a duplicate.
                self.binary(2)
                self.binary(0)
                self.binary(0)
            self.control({"type": "done", "sent": total})
        elif op == "fill":
            if self.recover:
                for index in request["pts"]:
                    self.binary(index)
            self.control({"type": "done", "sent": len(request["pts"]) if self.recover else 0})
        elif op == "end":
            self.control({"type": "ended"})
        else:
            raise AssertionError(f"Unexpected operation: {op}")


class DownloadTests(unittest.IsolatedAsyncioTestCase):
    async def test_all_files_cursor_snapshot_and_gap_recovery(self):
        client = FakeClient()
        logs = await client.history.list_all()
        self.assertEqual([log["session"] for log in logs], [3, 8])
        downloaded = [await client.history.download(log["session"]) for log in logs]
        self.assertEqual(downloaded, [client.records, []])
        self.assertEqual([r["after"] for r in client.requests if r["op"] == "list"], [0, 3, 8])
        self.assertEqual([r["pts"] for r in client.requests if r["op"] == "fill"], [[1]])
        self.assertTrue(all(r.get("kind") == "measurements" for r in client.requests
                            if r["op"] in {"list", "start"}))
        self.assertEqual(sum(r["op"] == "end" for r in client.requests), 2)
        self.assertFalse(any(r["op"] in {"delete", "set", "cmd"} for r in client.requests))

    async def test_missing_records_fail_and_release_export(self):
        client = FakeClient(recover=False)
        with self.assertRaisesRegex(RuntimeError, "records still missing"):
            await client.history.download(3)
        self.assertEqual(client.requests[-1], {"op": "end"})
        self.assertEqual(sum(r["op"] == "fill" for r in client.requests), export.MAX_FILL_ROUNDS)

    async def test_future_format_fails_and_releases_export(self):
        client = FakeClient(version=2)
        with self.assertRaisesRegex(RuntimeError, "Unsupported"):
            await client.history.download(3)
        self.assertEqual(client.requests[-1], {"op": "end"})

    async def test_timeout_releases_export(self):
        client = FakeClient(silent=True)
        with self.assertRaisesRegex(RuntimeError, "timed out"):
            await client.history.download(3)
        self.assertEqual(client.requests[-1], {"op": "end"})

    async def test_repeated_cursor_is_rejected(self):
        client = FakeClient(silent=True)
        for _ in range(2):
            client.control({"type": "list", "kind": "measurements", "next": 3,
                            "logs": [{"session": 3, "version": 1}]})
        with self.assertRaisesRegex(RuntimeError, "did not advance"):
            await client.history.list_all()

    async def test_malformed_binary_rejected(self):
        client = FakeClient()
        client.history.notify(None, bytearray(b"\x01\x00\x00bad"))
        with self.assertRaisesRegex(RuntimeError, "Invalid measurement-log packet"):
            await client.history.receive()


class CsvTests(unittest.TestCase):
    def test_unknown_time_order_and_invalid_fields(self):
        self.assertEqual(export.RECORD.size, 39)
        with tempfile.TemporaryDirectory() as directory:
            path = export.write_csv(Path(directory), 42, [record(), record(1700000000)])
            with path.open(newline="") as source:
                rows = list(csv.DictReader(source))
            self.assertEqual(rows[0]["timestamp"], "0")
            self.assertEqual(rows[0]["datetime_utc"], "")
            self.assertEqual(rows[0]["log_sequence"], "42")
            self.assertEqual(rows[1]["record_index"], "1")
            self.assertEqual(rows[1]["datetime_utc"], "2023-11-14T22:13:20+00:00")
            for field in ("temperature_c", "humidity_pct", "pm1_ugm3", "co2_ppm",
                          "nox_index", "pressure_hpa", "battery_pct"):
                self.assertEqual(rows[0][field], "")
            self.assertEqual(rows[0]["tvoc_index"], "0")
            self.assertEqual(rows[0]["pm25_ugm3"], "12.5")
            self.assertFalse(path.with_suffix(".csv.tmp").exists())


if __name__ == "__main__":
    unittest.main()
