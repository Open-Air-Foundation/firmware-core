"""Measurement History integration tests; requires a paired, connectable Go.

Downloads use existing logs. The delete case removes one existing log only
when logging is already disabled. Other cases preserve device data/settings.
"""

from __future__ import annotations

import pytest

import ago_protocol as proto
from test_history import _list_sessions


async def _control(collector, timeout: float, expected: str) -> dict:
    raw = await collector.wait_for(timeout=timeout)
    tag, payload = proto.parse_history_notification(raw, kind="measurements")
    assert tag == proto.HISTORY_TAG_CBOR, f"Expected {expected}, got binary data"
    assert payload.get("type") == expected, payload
    return payload


async def _write(client, data: bytes) -> None:
    assert len(data) <= 256, "Request exceeds firmware write buffer"
    await client.write_gatt_char(proto.CHAR_HISTORY_UUID, data, response=True)


async def _list_logs(client, collector, timeout: float) -> list[dict]:
    logs = []
    after = 0
    while True:
        await _write(client, proto.encode_measurement_list(after))
        response = await _control(collector, timeout, "list")
        assert set(response) == {"type", "kind", "next", "logs"}
        assert response["kind"] == "measurements"
        assert type(response["next"]) is int
        assert isinstance(response["logs"], list)
        assert len(response["logs"]) <= 1
        if not response["logs"]:
            assert response["next"] == 0
            return logs
        entry = response["logs"][0]
        assert set(entry) == proto.MEASUREMENT_LOG_ENTRY_KEYS
        assert all(type(entry[key]) is int for key in proto.MEASUREMENT_LOG_ENTRY_KEYS)
        assert after < entry["session"] <= 0xFFFFFFFF
        assert response["next"] == entry["session"]
        assert 0 <= entry["count"] <= proto.MEASUREMENT_LOG_RECORD_LIMIT
        assert entry["boundary"] in {1, 2, 3, 4}
        assert entry["version"] == proto.MEASUREMENT_LOG_VERSION
        # Unknown UTC is valid. Synchronization can move time in either direction.
        assert 0 <= entry["first_ts"] < 2**63
        assert 0 <= entry["last_ts"] < 2**63
        logs.append(entry)
        after = response["next"]


async def _collect_pass(collector, timeout: float, total: int, expected_indices: set[int]) -> dict:
    records = {}
    while True:
        raw = await collector.wait_for(timeout=timeout)
        tag, payload = proto.parse_history_notification(raw, kind="measurements")
        if tag == proto.HISTORY_TAG_CBOR:
            assert payload == {"type": "done", "sent": len(expected_indices)}, payload
            return records
        first, points = payload
        for offset, point in enumerate(points):
            index = first + offset
            assert 0 <= index < total
            assert index in expected_indices, f"Unexpected record {index} in this pass"
            assert index not in records, f"Duplicate record {index} within a pass"
            assert set(point) == set(proto.MEASUREMENT_RECORD_FIELDS)
            assert point["timestamp"] >= 0
            assert point["battery_percentage"] == 255 or 0 <= point["battery_percentage"] <= 100
            records[index] = point


async def _download(client, collector, timeout: float, sequence: int) -> tuple[int, dict]:
    await _write(client, proto.encode_measurement_start(sequence))
    started = await _control(collector, timeout, "started")
    assert set(started) == {"type", "session", "total", "pt_size", "kind", "version"}
    assert started["session"] == sequence
    assert started["kind"] == "measurements"
    assert started["version"] == proto.MEASUREMENT_LOG_VERSION
    assert started["pt_size"] == proto.MEASUREMENT_RECORD_WIRE_SIZE
    total = started["total"]
    assert type(total) is int and 0 <= total <= proto.MEASUREMENT_LOG_RECORD_LIMIT
    # Use this snapshot, not the potentially older count from list.
    return total, await _collect_pass(collector, timeout, total, set(range(total)))


@pytest.fixture
async def measurement_log(ago_client, history_notifications, ago_notify_timeout):
    logs = await _list_logs(ago_client, history_notifications, ago_notify_timeout)
    if not logs:
        pytest.skip("No measurement logs; enable Log Measurement and allow an Idle sample first")
    return logs[0]


async def test_list_measurement_logs(ago_client, history_notifications, ago_notify_timeout):
    await _list_logs(ago_client, history_notifications, ago_notify_timeout)
    await _write(ago_client, proto.encode_measurement_list(0xFFFFFFFF))
    assert await _control(history_notifications, ago_notify_timeout, "list") == {
        "type": "list", "kind": "measurements", "next": 0, "logs": [],
    }


@pytest.mark.parametrize("operation,error", [("start", "flash_error"), ("delete", "delete_failed")])
async def test_missing_measurement_log(
    ago_client, history_notifications, ago_notify_timeout, operation, error,
):
    # Log sequences start at 1, so 0 is a guaranteed absent ID.
    encode = proto.encode_measurement_start if operation == "start" else proto.encode_measurement_delete
    await _write(ago_client, encode(0))
    assert await _control(history_notifications, ago_notify_timeout, "error") == {
        "type": "error", "err": error,
    }


async def test_download_fill_and_end(
    ago_client, history_notifications, ago_notify_timeout, measurement_log,
):
    total, records = await _download(
        ago_client, history_notifications, ago_notify_timeout, measurement_log["session"],
    )
    # Deliberately drop one received record as well as recovering actual gaps.
    # Compare its retransmission to prove fill preserves the snapshot's contents.
    dropped = next(iter(records), None)
    expected = records.pop(dropped) if dropped is not None else None
    for _ in range(5):
        missing = sorted(set(range(total)) - records.keys())
        if not missing:
            break
        for start in range(0, len(missing), proto.MAX_FILL_POINTS):
            batch = missing[start:start + proto.MAX_FILL_POINTS]
            await _write(ago_client, proto.encode_history_fill(batch))
            records.update(await _collect_pass(
                history_notifications, ago_notify_timeout, total, set(batch),
            ))
    assert set(records) == set(range(total)), "Records still missing after fill"
    if dropped is not None:
        assert records[dropped] == expected

    # A mixed valid/out-of-snapshot fill is rejected as a whole; the active
    # snapshot survives the rejection, including if logging has appended data.
    indices = [0, total] if total else [total]
    await _write(ago_client, proto.encode_history_fill(indices))
    assert await _control(history_notifications, ago_notify_timeout, "error") == {
        "type": "error", "err": "invalid_index",
    }
    await _write(ago_client, proto.encode_history_fill([]))
    assert await _collect_pass(history_notifications, ago_notify_timeout, total, set()) == {}
    if total:
        await _write(ago_client, proto.encode_history_fill([0]))
        repeated = await _collect_pass(history_notifications, ago_notify_timeout, total, {0})
        assert repeated == {0: records[0]}
    await _write(ago_client, proto.encode_history_end())
    assert await _control(history_notifications, ago_notify_timeout, "ended") == {"type": "ended"}
    await _write(ago_client, proto.encode_history_fill([0]))
    assert await _control(history_notifications, ago_notify_timeout, "error") == {
        "type": "error", "err": "no_active_download",
    }


async def test_failed_start_releases_previous_log(
    ago_client, history_notifications, ago_notify_timeout, measurement_log,
):
    await _download(ago_client, history_notifications, ago_notify_timeout, measurement_log["session"])
    await _write(ago_client, proto.encode_measurement_start(0))
    assert await _control(history_notifications, ago_notify_timeout, "error") == {
        "type": "error", "err": "flash_error",
    }
    await _write(ago_client, proto.encode_history_fill([0]))
    assert await _control(history_notifications, ago_notify_timeout, "error") == {
        "type": "error", "err": "no_active_download",
    }


async def test_delete_closed_log_ends_export(
    ago_client, history_notifications, ago_notify_timeout,
):
    config = proto.decode_cbor(bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)))
    if config["log_m"]:
        pytest.skip("Disable Log Measurement first so the delete target is closed")
    logs = await _list_logs(ago_client, history_notifications, ago_notify_timeout)
    if not logs:
        pytest.skip("No measurement logs to delete")
    routes_before = await _list_sessions(ago_client, history_notifications, ago_notify_timeout)
    target = logs[0]["session"]
    await _download(ago_client, history_notifications, ago_notify_timeout, target)
    await _write(ago_client, proto.encode_measurement_delete(target))
    assert await _control(history_notifications, ago_notify_timeout, "deleted") == {
        "type": "deleted", "session": target,
    }
    await _write(ago_client, proto.encode_history_fill([0]))
    assert await _control(history_notifications, ago_notify_timeout, "error") == {
        "type": "error", "err": "no_active_download",
    }
    remaining = await _list_logs(ago_client, history_notifications, ago_notify_timeout)
    assert {entry["session"] for entry in remaining} == {entry["session"] for entry in logs} - {target}
    routes_after = await _list_sessions(ago_client, history_notifications, ago_notify_timeout)
    assert {entry["id"] for entry in routes_after} == {entry["id"] for entry in routes_before}
