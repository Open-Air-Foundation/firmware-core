#!/usr/bin/env python3
"""Listen for AirGradient Go Offline BLE measurements until Ctrl+C.

Requirements:
    python3 -m pip install bleak

Usage:
    python3 scripts/ago_listen_measurements.py
    python3 scripts/ago_listen_measurements.py --address AA:BB:CC:DD:EE:FF

Prints one JSON object per matching scanner report, for format version 2
(Offline service UUID list plus manufacturer data) and the earlier version 1
(Service Data). Measurements are raw, invalid values are null, and received_at
is the client's reception time. No connection, pairing, or bonding is
performed.
"""

from __future__ import annotations

import argparse
import asyncio
from datetime import datetime
import json
import logging
import struct
from typing import Any

logger = logging.getLogger("ago_measurements")

# Matches products/go/main/go_ble_advertisement.h.
SERVICE_UUID = "992f1fe6-4519-4d4f-8ca7-546d36126ee8"
VERSION = 1
PAYLOAD = struct.Struct("<BhBHHHhB")
INVALID_U8 = 0xFF
INVALID_U16 = 0xFFFF
INVALID_I16 = -32768

# Version 2: 9-byte bit-packed manufacturer data payload, least significant bit first.
COMPANY_ID = 0xFFFF  # Bluetooth SIG test ID until an assigned ID replaces it.
V2_SIZE = 9
V2_VERSION = 2
V2_FIELDS = {  # name: (first bit, width)
    "version": (0, 3),
    "temperature": (3, 11),
    "humidity": (14, 7),
    "pm25": (21, 14),
    "co2": (35, 14),
    "pressure": (49, 13),
    "battery": (62, 7),
}
SEA_LEVEL_PRESSURE_HPA = 1013.25


def decode_measurements(data: bytes) -> dict[str, int | float | None]:
    """Decode the 13-byte Service Data body (Bleak already removes the UUID)."""
    if len(data) != PAYLOAD.size:
        raise ValueError(f"expected {PAYLOAD.size} bytes, received {len(data)}")
    version, temp, humidity, pm25, co2, pressure, altitude, battery = PAYLOAD.unpack(data)
    if version != VERSION:
        raise ValueError(f"unsupported payload version {version}")
    return {
        "version": version,
        "temperature_c": None if temp == INVALID_I16 else temp / 100,
        "humidity_percent": None if humidity == INVALID_U8 else humidity,
        "pm25_ug_m3": None if pm25 == INVALID_U16 else pm25 / 10,
        "co2_ppm": None if co2 == INVALID_U16 else co2,
        "pressure_hpa": None if pressure == INVALID_U16 else pressure / 10,
        "barometric_altitude_m": None if altitude == INVALID_I16 else altitude,
        "battery_percent": None if battery == INVALID_U8 else battery,
    }


def _bits(data: bytes, first: int, width: int) -> int:
    return sum(((data[(first + i) // 8] >> ((first + i) % 8)) & 1) << i for i in range(width))


def decode_measurements_v2(data: bytes) -> dict[str, int | float | None]:
    """Decode the 9-byte version 2 payload (Bleak already removes the company ID)."""
    if len(data) != V2_SIZE:
        raise ValueError(f"expected {V2_SIZE} bytes, received {len(data)}")
    raw = {name: _bits(data, *field) for name, field in V2_FIELDS.items()}
    if raw["version"] != V2_VERSION:
        raise ValueError(f"unsupported payload version {raw['version']}")

    def value(name: str) -> int | None:
        return None if raw[name] == (1 << V2_FIELDS[name][1]) - 1 else raw[name]

    temperature, pm25, pressure = value("temperature"), value("pm25"), value("pressure")
    pressure_hpa = None if pressure is None else round(pressure / 10 + 300, 1)
    return {
        "version": V2_VERSION,
        "temperature_c": None if temperature is None else round(temperature / 10 - 40, 1),
        "humidity_percent": value("humidity"),
        "pm25_ug_m3": None if pm25 is None else pm25 / 10,
        "co2_ppm": value("co2"),
        "pressure_hpa": pressure_hpa,
        "barometric_altitude_m": None if pressure_hpa is None else round(
            44330 * (1 - (pressure_hpa / SEA_LEVEL_PRESSURE_HPA) ** 0.1903)),
        "battery_percent": value("battery"),
    }


async def listen(address: str | None = None) -> None:
    from bleak import BleakScanner

    def on_detect(device: Any, advertisement: Any) -> None:
        if address and device.address.casefold() != address.casefold():
            return
        v1 = advertisement.service_data.get(SERVICE_UUID)
        v2 = (advertisement.manufacturer_data.get(COMPANY_ID)
              if SERVICE_UUID in (u.lower() for u in advertisement.service_uuids) else None)
        if v1 is None and v2 is None:
            return
        try:
            readings = decode_measurements(v1) if v1 is not None else decode_measurements_v2(v2)
        except ValueError as exc:
            logger.warning("Ignoring packet from %s: %s", device.address, exc)
            return
        print(json.dumps({
            "received_at": datetime.now().astimezone().isoformat(timespec="milliseconds"),
            "device": device.address,
            "rssi_dbm": advertisement.rssi,
            **readings,
        }), flush=True)

    # Match in the callback rather than with a scan filter so version 1 (Service
    # Data only) still matches. Preserve repeated reports from the scanner.
    async with BleakScanner(detection_callback=on_detect):
        logger.info("Listening for Go measurements. Press Ctrl+C to stop.")
        await asyncio.Event().wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--address", help="Only print this device's Bluetooth address or macOS UUID")
    args = parser.parse_args()
    try:
        from bleak.exc import BleakError
    except ImportError:
        parser.exit(1, "Missing dependency: install with python3 -m pip install bleak\n")

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    logging.getLogger("bleak").setLevel(logging.WARNING)
    try:
        asyncio.run(listen(args.address))
    except KeyboardInterrupt:
        logger.info("Stopped.")
    except (BleakError, OSError) as exc:
        logger.error("Bluetooth error: %s", exc)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
