"""AirGradient Go BLE protocol constants, CBOR helpers, and binary parsers.

All AGo-specific protocol knowledge lives here so test modules only import
this single file for UUIDs, key sets, type expectations, encoding, and
decoding.
"""

from __future__ import annotations

import struct
from typing import Any

import cbor2

# ---------------------------------------------------------------------------
# GATT UUIDs
# ---------------------------------------------------------------------------

SERVICE_UUID = "d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1"

CHAR_MEASURES_UUID = "d1c0c0a1-6b48-4b2a-9b1d-59f9f2b0a1e1"
CHAR_STATUS_UUID = "d1c0c0a2-6b48-4b2a-9b1d-59f9f2b0a1e1"
CHAR_CONFIG_UUID = "d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1"
CHAR_HISTORY_UUID = "d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1"

# Standard Device Information Service (0x180A) — present in Portable mode.
# 16-bit SIG UUIDs expand against the Bluetooth base UUID.
DIS_SERVICE_UUID = "0000180a-0000-1000-8000-00805f9b34fb"
DIS_MODEL_NUMBER_UUID = "00002a24-0000-1000-8000-00805f9b34fb"
DIS_SERIAL_NUMBER_UUID = "00002a25-0000-1000-8000-00805f9b34fb"
DIS_FIRMWARE_REVISION_UUID = "00002a26-0000-1000-8000-00805f9b34fb"
DIS_MANUFACTURER_NAME_UUID = "00002a29-0000-1000-8000-00805f9b34fb"

# ---------------------------------------------------------------------------
# Expected characteristic properties
# ---------------------------------------------------------------------------

EXPECTED_PROPERTIES: dict[str, set[str]] = {
    CHAR_MEASURES_UUID: {"read", "notify"},
    CHAR_STATUS_UUID: {"read", "notify"},
    CHAR_CONFIG_UUID: {"read", "write", "notify"},
    CHAR_HISTORY_UUID: {"write", "notify"},
}

# ---------------------------------------------------------------------------
# Measures characteristic
# ---------------------------------------------------------------------------

MEASURES_ALL_KEYS = {
    "t", "h", "pm1", "pm25", "pm10",
    "co2", "tvoc", "nox", "pres",
    "lat", "lon", "alt", "fix", "sat",
    "ts",
}

# Fields that are always present when included
MEASURES_ALWAYS_PRESENT = {"ts"}

# GPS fields that appear as a group when fix is valid
MEASURES_GPS_FIX_GROUP = {"fix", "sat"}
MEASURES_GPS_POSITION = {"lat", "lon", "alt"}
MEASURES_GPS_ALL = MEASURES_GPS_FIX_GROUP | MEASURES_GPS_POSITION

# Expected CBOR types per key (Python types after cbor2 decode)
MEASURES_FIELD_TYPES: dict[str, tuple[type, ...]] = {
    "t": (float,),
    "h": (float,),
    "pm1": (float,),
    "pm25": (float,),
    "pm10": (float,),
    "co2": (int,),
    "tvoc": (int,),
    "nox": (int,),
    "pres": (float,),
    "lat": (float,),          # float64, cbor2 decodes as Python float
    "lon": (float,),
    "alt": (float,),
    "fix": (int,),
    "sat": (int,),
    "ts": (int,),
}

# ---------------------------------------------------------------------------
# Status characteristic
# ---------------------------------------------------------------------------

# Firmware version is intentionally absent here — it lives in the DIS
# Firmware Revision characteristic (0x2A26), see test_device_info.py.
#
# STATUS_ALL_KEYS is the READ shape (full snapshot). A Status NOTIFY carries
# only the transition delta (STATUS_NOTIFY_KEYS); the other keys come via Read.
STATUS_ALL_KEYS = {
    "gps_fix", "gps_sat", "bat_pct", "bat_v", "charging",
    "tracking", "session", "flash_kb", "used_kb",
}

# Status NOTIFY carries only the two keys that change on a tracking transition.
STATUS_NOTIFY_KEYS = {"tracking", "session"}

STATUS_FIELD_TYPES: dict[str, tuple[type, ...]] = {
    "gps_fix": (int,),
    "gps_sat": (int,),
    "bat_pct": (int,),
    "bat_v": (float,),
    "charging": (str,),
    "tracking": (bool,),
    "session": (int,),
    "flash_kb": (int,),
    "used_kb": (int,),
}

CHARGING_STATES = {"none", "trickle", "pre", "fast", "taper", "topoff", "done", "unknown"}

# ---------------------------------------------------------------------------
# Config characteristic
# ---------------------------------------------------------------------------

CONFIG_READ_KEYS = {
    "meas_int",
    "temp_f", "alt_ft", "pm_aqi",
    "gps_mode",
    "auto_lock",
    "op_mode",
    "fled", "bled", "tled",
    "buz", "abc", "tlo", "nlo",
    "pm25_corr", "temp_corr", "hum_corr",
}

# Config NOTIFY is a DELTA, not the full snapshot: "type":"config" plus only the
# field(s) that changed. A single-field "set" yields {"type", <changed key>}; a
# no-op "set" produces no notification. The full snapshot is only available via
# Read (CONFIG_READ_KEYS, no "type").
CONFIG_NOTIFY_TYPE = "config"

CO2_ABC_DAYS_DISABLED = -1
CO2_ABC_DAYS_MIN = 1
CO2_ABC_DAYS_MAX = 200
LEARNING_OFFSET_HOURS_MIN = 1
LEARNING_OFFSET_HOURS_MAX = 1000

CONFIG_FIELD_TYPES: dict[str, tuple[type, ...]] = {
    "meas_int": (int,),
    "temp_f": (bool,),
    "alt_ft": (bool,),
    "pm_aqi": (bool,),
    "gps_mode": (str,),
    "auto_lock": (int,),
    "op_mode": (str,),
    "fled": (int,),
    "bled": (int,),
    "tled": (int,),
    "buz": (bool,),
    "abc": (int,),
    "tlo": (int,),
    "nlo": (int,),
    "pm25_corr": (dict,),
    "temp_corr": (dict,),
    "hum_corr": (dict,),
    "type": (str,),
}

GPS_MODES = {"off", "tracking", "always"}
OPERATING_MODES = {"portable", "stationary", "offline"}

CORRECTION_MAP_KEYS = {"pm25_corr", "temp_corr", "hum_corr"}
CORRECTION_SCHEMA_VERSION = 1
PM25_CORRECTION_ALGORITHMS = {
    "none": 0,
    "epa_2021": 1,
    "custom_via_pm25_raw": 2,
}
LINEAR_CORRECTION_ALGORITHMS = {"none": 0, "custom": 1}
PM25_CORRECTION_FLAG_USE_EPA = 1 << 0

# Command progress notification (sent before long-running commands)
CMD_PROGRESS_KEYS = {"type", "cmd"}

# Command result notification
CMD_RESULT_KEYS_SUCCESS = {"type", "cmd", "ok"}
CMD_RESULT_KEYS_FAILURE = {"type", "cmd", "ok", "err"}

COMMANDS = {"co2_cal", "clear_data", "factory_rst", "start_tracking", "stop_tracking"}

# Config "set" rejection error strings (cmd_result err values).
ERR_SINGLE_FIELD_ONLY = "single_field_only"
ERR_UNKNOWN_CONFIG_KEY = "unknown_config_key"
ERR_INVALID_CONFIG_VALUE = "invalid_config_value"
ERR_CONFIG_SAVE_FAILED = "config_save_failed"

# ---------------------------------------------------------------------------
# History characteristic
# ---------------------------------------------------------------------------

HISTORY_TAG_CBOR = 0x00
HISTORY_TAG_BINARY = 0x01

ROUTE_POINT_WIRE_SIZE = 56

# struct format for RoutePointWire: little-endian
# uint32 timestamp | float64 lat | float64 lon | float32 alt | uint8 fix |
# float32 temp | float32 hum | float32 pm1 | float32 pm25 | float32 pm10 |
# int16 co2 | int16 tvoc | int16 nox | float32 pressure | uint8 battery_pct
_ROUTE_POINT_FMT = "<I d d f B f f f f f h h h f B"
_ROUTE_POINT_STRUCT = struct.Struct(_ROUTE_POINT_FMT)

assert _ROUTE_POINT_STRUCT.size == ROUTE_POINT_WIRE_SIZE, (
    f"RoutePointWire struct size mismatch: expected {ROUTE_POINT_WIRE_SIZE}, "
    f"got {_ROUTE_POINT_STRUCT.size}"
)

ROUTE_POINT_FIELDS = (
    "timestamp", "latitude", "longitude", "altitude", "gps_fix",
    "temperature", "humidity", "pm_01", "pm_25", "pm_10",
    "co2", "tvoc_index", "nox_index", "pressure", "battery_percentage",
)

# History session list entry keys
SESSION_ENTRY_KEYS = {"id", "pts", "ts"}


# ---------------------------------------------------------------------------
# CBOR decode helpers
# ---------------------------------------------------------------------------

def decode_cbor(data: bytes) -> Any:
    """Decode a CBOR payload. Raises cbor2.CBORDecodeError on invalid data."""
    return cbor2.loads(data)


def decode_cbor_safe(data: bytes) -> tuple[bool, Any]:
    """Decode CBOR, returning (success, result_or_error)."""
    try:
        return True, cbor2.loads(data)
    except Exception as exc:
        return False, exc


# ---------------------------------------------------------------------------
# CBOR encode helpers (for writing to the device)
# ---------------------------------------------------------------------------

def encode_config_set(**overrides: Any) -> bytes:
    """Build a Config write payload for setting config values.

    Example: encode_config_set(buz=True)
    """
    payload: dict[str, Any] = {"op": "set"}
    payload.update(overrides)
    return cbor2.dumps(payload)


def encode_pm25_correction(
    algorithm: str,
    scale: float,
    intercept: float,
    use_epa: bool,
) -> bytes:
    """Build a Config write payload for the complete PM2.5 correction group."""
    if algorithm not in PM25_CORRECTION_ALGORITHMS:
        raise ValueError(f"Unsupported PM2.5 correction algorithm: {algorithm}")
    return _encode_correction_set(
        "pm25_corr",
        PM25_CORRECTION_ALGORITHMS[algorithm],
        scale,
        intercept,
        PM25_CORRECTION_FLAG_USE_EPA if use_epa else 0,
    )


def encode_linear_correction(key: str, algorithm: str, scale: float, intercept: float) -> bytes:
    """Build a Config write payload for a complete linear correction group."""
    if key not in {"temp_corr", "hum_corr"}:
        raise ValueError(f"Unsupported linear correction key: {key}")
    if algorithm not in LINEAR_CORRECTION_ALGORITHMS:
        raise ValueError(f"Unsupported linear correction algorithm: {algorithm}")
    return _encode_correction_set(
        key,
        LINEAR_CORRECTION_ALGORITHMS[algorithm],
        scale,
        intercept,
        None,
    )


def _encode_correction_set(
    key: str,
    algorithm: int,
    scale: float,
    intercept: float,
    flags: int | None,
) -> bytes:
    """Encode a correction write with float32 CBOR coefficients."""
    values = bytearray([0x84 if flags is not None else 0x83])
    values.extend(cbor2.dumps(algorithm))
    values.extend(b"\xfa")
    values.extend(struct.pack(">f", scale))
    values.extend(b"\xfa")
    values.extend(struct.pack(">f", intercept))
    if flags is not None:
        values.extend(cbor2.dumps(flags))

    correction = bytearray(b"\xa2")
    correction.extend(cbor2.dumps("s"))
    correction.extend(cbor2.dumps(CORRECTION_SCHEMA_VERSION))
    correction.extend(cbor2.dumps("v"))
    correction.extend(values)

    payload = bytearray(b"\xa2")
    payload.extend(cbor2.dumps("op"))
    payload.extend(cbor2.dumps("set"))
    payload.extend(cbor2.dumps(key))
    payload.extend(correction)
    return bytes(payload)


def encode_command(cmd: str) -> bytes:
    """Build a Config write payload for executing a command.

    Example: encode_command("co2_cal")
    """
    return cbor2.dumps({"op": "cmd", "cmd": cmd})


def encode_history_list() -> bytes:
    """Build a History write payload: list sessions."""
    return cbor2.dumps({"op": "list"})


def encode_history_start(session_id: int) -> bytes:
    """Build a History write payload: start download."""
    return cbor2.dumps({"op": "start", "session": session_id})


def encode_history_fill(point_indices: list[int]) -> bytes:
    """Build a History write payload: fill missing points."""
    return cbor2.dumps({"op": "fill", "pts": point_indices})


def encode_history_end() -> bytes:
    """Build a History write payload: end download."""
    return cbor2.dumps({"op": "end"})


def encode_history_delete(session_id: int) -> bytes:
    """Build a History write payload: delete a single session."""
    return cbor2.dumps({"op": "delete", "session": session_id})


# ---------------------------------------------------------------------------
# History notification parsers
# ---------------------------------------------------------------------------

def parse_history_notification(data: bytes) -> tuple[int, Any]:
    """Parse a History characteristic notification.

    Returns (tag, payload) where:
      - tag == HISTORY_TAG_CBOR:   payload is the decoded CBOR dict
      - tag == HISTORY_TAG_BINARY: payload is (point_index, [route_point_dicts])
    """
    if len(data) < 1:
        raise ValueError("Empty history notification")

    tag = data[0]
    body = data[1:]

    if tag == HISTORY_TAG_CBOR:
        return tag, cbor2.loads(body)

    if tag == HISTORY_TAG_BINARY:
        return tag, _parse_binary_chunk(body)

    raise ValueError(f"Unknown history tag: 0x{tag:02x}")


def _parse_binary_chunk(body: bytes) -> tuple[int, list[dict[str, Any]]]:
    """Parse the body of a binary history notification (after the tag byte).

    Returns (first_point_index, [route_point_dicts]).
    """
    if len(body) < 2:
        raise ValueError(f"Binary chunk too short for header: {len(body)} bytes")

    point_index = struct.unpack_from("<H", body, 0)[0]
    payload = body[2:]

    if len(payload) % ROUTE_POINT_WIRE_SIZE != 0:
        raise ValueError(
            f"Binary payload size {len(payload)} is not a multiple of "
            f"{ROUTE_POINT_WIRE_SIZE}"
        )

    points: list[dict[str, Any]] = []
    offset = 0
    while offset < len(payload):
        points.append(parse_route_point_wire(payload, offset))
        offset += ROUTE_POINT_WIRE_SIZE

    return point_index, points


def parse_route_point_wire(data: bytes, offset: int = 0) -> dict[str, Any]:
    """Decode a single 55-byte RoutePointWire from *data* at *offset*.

    Returns a dict keyed by ROUTE_POINT_FIELDS.
    """
    values = _ROUTE_POINT_STRUCT.unpack_from(data, offset)
    return dict(zip(ROUTE_POINT_FIELDS, values))
