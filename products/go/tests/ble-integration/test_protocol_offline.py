"""Offline wire-format checks: no device fixtures, scanning, or BLE connection."""

import asyncio
import struct

import cbor2
import pytest

import ago_protocol as proto
from conftest import NotificationCollector
from test_measurement_history import _collect_pass, _download
from test_tracking_duration import _next_tracking_status


@pytest.mark.parametrize("arguments", [{}, {"duration_min": 0}, {"duration_min": 1},
                                      {"duration_min": 1440}, {"duration_min": None}])
def test_command_duration_encoding(arguments):
    # Explicit zero/null must not be silently omitted by the helper.
    assert cbor2.loads(proto.encode_command("start_tracking", **arguments)) == {
        "op": "cmd", "cmd": "start_tracking", **arguments,
    }


@pytest.mark.parametrize("stop", [0, 1, 2])
async def test_tracking_collector_skips_charging_delta(stop):
    collector = NotificationCollector()
    status = {"tracking": False, "session": 0, "trk": 0, "stop": stop}
    collector.callback(None, cbor2.dumps({"charging": "fast", "bat_pct": 75, "bat_v": 4.0}))
    collector.callback(None, cbor2.dumps(status))
    assert await _next_tracking_status(collector, 0.01) == status
    with pytest.raises(asyncio.TimeoutError):
        await _next_tracking_status(collector, 0.01)


@pytest.mark.parametrize("stop", [True, False, "user", 1.0, -1, 3])
async def test_tracking_collector_rejects_invalid_stop(stop):
    collector = NotificationCollector()
    collector.callback(None, cbor2.dumps({"tracking": False, "session": 0, "trk": 0, "stop": stop}))
    with pytest.raises(AssertionError):
        await _next_tracking_status(collector, 0.01)


async def test_tracking_collector_rejects_old_status_shape():
    collector = NotificationCollector()
    collector.callback(None, cbor2.dumps({"tracking": False, "session": 0, "trk": 0}))
    with pytest.raises(AssertionError):
        await _next_tracking_status(collector, 0.01)


def measurement_bytes(timestamp=0):
    # Construct individual fields independently of the decoder's struct layout.
    return (
        timestamp.to_bytes(8, "little", signed=True)
        + b"".join(struct.pack("<f", value) for value in (-1001.0, 45.5, 1.0, 2.5, 10.0))
        + b"".join(value.to_bytes(2, "little", signed=True) for value in (450, -1, 123))
        + struct.pack("<f", 1013.25) + bytes([255])
    )


@pytest.mark.parametrize("timestamp", [0, 2**40, -1])
def test_measurement_wire_offsets_and_signed_utc(timestamp):
    raw = bytes([1]) + (258).to_bytes(2, "little") + measurement_bytes(timestamp)
    tag, (index, points) = proto.parse_history_notification(raw, kind="measurements")
    assert tag == proto.HISTORY_TAG_BINARY
    assert index == 258
    assert points == [{
        "timestamp": timestamp, "temperature": -1001.0, "humidity": 45.5,
        "pm_01": 1.0, "pm_25": 2.5, "pm_10": 10.0, "co2": 450,
        "tvoc_index": -1, "nox_index": 123, "pressure": 1013.25,
        "battery_percentage": 255,
    }]


def test_multiple_measurements_and_route_compatibility():
    raw = b"\x01\x07\x00" + measurement_bytes(1) + measurement_bytes(2)
    _, (index, points) = proto.parse_history_notification(raw, kind="measurements")
    assert index == 7
    assert [point["timestamp"] for point in points] == [1, 2]
    route = struct.pack("<IddfB5f3hfB", 123, 1.5, -2.5, 100, 3,
                        25, 40, 1, 2, 3, 400, 5, 6, 1000, 80)
    _, (index, points) = proto.parse_history_notification(b"\x01\x00\x00" + route)
    assert index == 0
    assert set(points[0]) == set(proto.ROUTE_POINT_FIELDS)
    assert points[0]["latitude"] == 1.5
    assert points[0]["battery_percentage"] == 80
    with pytest.raises(ValueError):
        proto.parse_history_notification(raw)  # No implicit measurement detection.


@pytest.mark.parametrize("data", [b"", b"\x02", b"\x01", b"\x01\x00", b"\x01\x00\x00",
                                      b"\x01\x00\x00" + measurement_bytes()[:-1]])
def test_malformed_history_is_rejected(data):
    with pytest.raises(ValueError):
        proto.parse_history_notification(data, kind="measurements")


def test_unsupported_measurement_version():
    with pytest.raises(ValueError, match="Unsupported"):
        proto.parse_history_notification(b"\x01\x00\x00" + measurement_bytes(),
                                         kind="measurements", version=2)


def test_measurement_commands_keep_route_defaults():
    assert cbor2.loads(proto.encode_measurement_list(42)) == {
        "op": "list", "kind": "measurements", "after": 42,
    }
    assert cbor2.loads(proto.encode_measurement_start(42)) == {
        "op": "start", "kind": "measurements", "session": 42,
    }
    assert cbor2.loads(proto.encode_measurement_delete(42)) == {
        "op": "delete", "kind": "measurements", "session": 42,
    }
    assert cbor2.loads(proto.encode_history_list()) == {"op": "list"}
    assert cbor2.loads(proto.encode_history_start(42)) == {"op": "start", "session": 42}
    assert cbor2.loads(proto.encode_history_fill([0, 2])) == {"op": "fill", "pts": [0, 2]}
    assert cbor2.loads(proto.encode_config_set(log_m=True)) == {"op": "set", "log_m": True}
    assert "log_m" in proto.CONFIG_READ_KEYS
    assert proto.CONFIG_FIELD_TYPES["log_m"] == (bool,)


async def test_collector_keeps_gaps_and_does_not_accept_timeout_as_done():
    collector = NotificationCollector()
    collector.callback(None, b"\x01\x01\x00" + measurement_bytes())
    collector.callback(None, b"\x00" + cbor2.dumps({"type": "done", "sent": 3}))
    records = await _collect_pass(collector, 0.01, 3, {0, 1, 2})
    assert set(records) == {1}  # Device sent count is not receipt coverage.
    with pytest.raises(asyncio.TimeoutError):
        await _collect_pass(collector, 0.01, 3, {0})


@pytest.mark.parametrize("first,total", [(3, 3), (0, 0)])
async def test_collector_rejects_records_outside_snapshot(first, total):
    collector = NotificationCollector()
    collector.callback(None, b"\x01" + first.to_bytes(2, "little") + measurement_bytes())
    with pytest.raises(AssertionError):
        await _collect_pass(collector, 0.01, total, set(range(total)))


async def test_download_uses_started_count_and_checks_format():
    collector = NotificationCollector()

    class Client:
        async def write_gatt_char(self, uuid, data, response):
            assert uuid == proto.CHAR_HISTORY_UUID and response is True
            assert cbor2.loads(data) == {"op": "start", "kind": "measurements", "session": 42}

    started = {"type": "started", "session": 42, "kind": "measurements",
               "total": 1, "version": 1, "pt_size": 39}
    collector.callback(None, b"\x00" + cbor2.dumps(started))
    collector.callback(None, b"\x01\x00\x00" + measurement_bytes())
    collector.callback(None, b"\x00" + cbor2.dumps({"type": "done", "sent": 1}))
    total, records = await _download(Client(), collector, 0.01, 42)
    assert total == 1 and set(records) == {0}
    started["version"] = 2
    collector.callback(None, b"\x00" + cbor2.dumps(started))
    with pytest.raises(AssertionError):
        await _download(Client(), collector, 0.01, 42)
