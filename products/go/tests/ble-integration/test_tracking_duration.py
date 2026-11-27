"""Tracking duration commands and real countdowns on a bonded Go device."""

import asyncio

import cbor2
import pytest
import pytest_asyncio

import ago_protocol as proto
from conftest import NotificationCollector
from test_status_notify import (
    _read_status,
    tracking_without_gps_restart,  # Reuse the GPS/Idle cleanup fixture.
)

MINUTE_SECONDS = 60
TIMER_TOLERANCE_SECONDS = 5
RESET_DELAY_SECONDS = 15


async def _next_tracking_status(collector: NotificationCollector, timeout: float) -> dict:
    """Ignore independent charging deltas without extending the timeout."""
    async with asyncio.timeout(timeout):
        while True:
            status = proto.decode_cbor(await collector.wait_for(timeout))
            if set(status) == proto.STATUS_CHARGING_KEYS:
                continue
            assert set(status) == proto.STATUS_NOTIFY_KEYS
            assert type(status["stop"]) is int
            assert status["stop"] in proto.STOP_REASONS.values()
            return status


@pytest.fixture
def tracking_command(ago_client, config_notifications, ago_notify_timeout):
    async def send(command, *, error=None, raw=None, **arguments):
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID,
            raw if raw is not None else proto.encode_command(command, **arguments),
            response=True,
        )
        result = proto.decode_cbor(await config_notifications.wait_for(ago_notify_timeout))
        expected = {"type": "cmd_result", "cmd": command, "ok": error is None}
        if error is not None:
            expected["err"] = error
        assert result == expected
    return send


@pytest_asyncio.fixture(autouse=True)
async def idle_before_duration_test(
    tracking_without_gps_restart, ago_client, config_notifications, status_notifications,
    tracking_command, ago_notify_timeout,
):
    if (await _read_status(ago_client))["tracking"]:
        await tracking_command("stop_tracking")
        assert await _next_tracking_status(status_notifications, ago_notify_timeout) == {
            "tracking": False, "session": 0, "trk": 0, "stop": 1,
        }
    config_notifications.drain()
    status_notifications.drain()


async def _assert_unchanged(client, statuses, results, baseline_status, baseline_config):
    await asyncio.sleep(0.2)
    current = await _read_status(client)
    assert {key: current[key] for key in proto.STATUS_NOTIFY_KEYS} == baseline_status
    assert proto.decode_cbor(bytes(await client.read_gatt_char(proto.CHAR_CONFIG_UUID))) == baseline_config
    assert results.drain_pending() == []  # No unread Config delta or command result.
    for raw in statuses.drain_pending():
        assert set(proto.decode_cbor(raw)) == proto.STATUS_CHARGING_KEYS


@pytest.mark.parametrize("command", ["start_tracking", "tracking_duration"])
@pytest.mark.parametrize("value", [-1, 1441, 1.5, 30.0, True, "30", None, [], {}])
async def test_invalid_duration_preserves_session(
    ago_client, tracking_command, status_notifications, config_notifications,
    ago_notify_timeout, command, value,
):
    if command == "tracking_duration":
        await tracking_command("start_tracking")
        await _next_tracking_status(status_notifications, ago_notify_timeout)
    status = await _read_status(ago_client)
    baseline = {key: status[key] for key in proto.STATUS_NOTIFY_KEYS}
    config = proto.decode_cbor(bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)))
    await tracking_command(command, duration_min=value, error="invalid_config_value")
    await _assert_unchanged(ago_client, status_notifications, config_notifications, baseline, config)


@pytest.mark.parametrize("command,raw", [
    ("start_tracking", proto.encode_command("start_tracking", duration_min=0)),
    ("tracking_duration", proto.encode_command("tracking_duration")),
    ("stop_tracking", proto.encode_command("stop_tracking", duration_min=1)),
    ("set", proto.encode_config_set(duration_min=30)),
    # An indefinite map preserves duplicate keys, unlike a Python dict.
    ("tracking_duration", b"\xbf" + b"".join(cbor2.dumps(value) for value in (
        "op", "cmd", "cmd", "tracking_duration", "duration_min", 1, "duration_min", 2,
    )) + b"\xff"),
])
async def test_duration_argument_rejections(
    ago_client, tracking_command, status_notifications, config_notifications,
    ago_notify_timeout, command, raw,
):
    await tracking_command("start_tracking")
    baseline = await _next_tracking_status(status_notifications, ago_notify_timeout)
    config = proto.decode_cbor(bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)))
    await tracking_command(command, raw=raw, error="invalid_config_value")
    await _assert_unchanged(ago_client, status_notifications, config_notifications, baseline, config)


@pytest.mark.parametrize("minutes", [0, 1, 1440])
async def test_duration_requires_active_session(tracking_command, minutes):
    await tracking_command("tracking_duration", duration_min=minutes, error="not_tracking")


@pytest.mark.parametrize("minutes", [1, 1440])
@pytest.mark.parametrize("paused", [False, True], ids=["recording", "paused"])
async def test_duration_update_has_only_command_result(
    ago_client, tracking_command, status_notifications, config_notifications,
    ago_notify_timeout, minutes, paused,
):
    await tracking_command("start_tracking", duration_min=minutes)
    baseline = await _next_tracking_status(status_notifications, ago_notify_timeout)
    if paused:
        await tracking_command("pause_tracking")
        baseline = await _next_tracking_status(status_notifications, ago_notify_timeout)
    config = proto.decode_cbor(bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)))
    for duration in (minutes, 0):
        await tracking_command("tracking_duration", duration_min=duration)
        await _assert_unchanged(ago_client, status_notifications, config_notifications, baseline, config)


@pytest.mark.parametrize("mode", ["recording", "paused", "resumed", "replaced"])
async def test_duration_expires_once(
    ago_client, tracking_command, status_notifications, config_notifications,
    ago_notify_timeout, mode,
):
    loop = asyncio.get_running_loop()
    before = loop.time()
    await tracking_command("start_tracking", duration_min=1)
    after = loop.time()
    started = await _next_tracking_status(status_notifications, ago_notify_timeout)
    assert started["tracking"] is True and started["trk"] == 1 and started["stop"] == 0
    assert started["session"] > 0
    config = proto.decode_cbor(bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)))
    if mode == "recording":
        # Neither a rejected update nor a second Start may change the deadline.
        await tracking_command("tracking_duration", duration_min=1441, error="invalid_config_value")
        await tracking_command("start_tracking", duration_min=1440, error="already_tracking")
    if mode in ("paused", "resumed"):
        await tracking_command("pause_tracking")
        paused = await _next_tracking_status(status_notifications, ago_notify_timeout)
        assert paused == {**started, "trk": 2}
    if mode in ("resumed", "replaced"):
        await asyncio.sleep(RESET_DELAY_SECONDS)
        if mode == "resumed":
            await tracking_command("resume_tracking")
            assert await _next_tracking_status(status_notifications, ago_notify_timeout) == started
        else:
            before = loop.time()
            await tracking_command("tracking_duration", duration_min=1)
            after = loop.time()
    deadline = after + MINUTE_SECONDS + TIMER_TOLERANCE_SECONDS
    stopped = await _next_tracking_status(status_notifications, deadline - loop.time())
    assert loop.time() >= before + MINUTE_SECONDS - TIMER_TOLERANCE_SECONDS, "Stopped early"
    assert stopped == {"tracking": False, "session": 0, "trk": 0, "stop": 2}
    await _assert_unchanged(ago_client, status_notifications, config_notifications, stopped, config)
    # A new Start clears the retained duration stop reason.
    await tracking_command("start_tracking")
    restarted = await _next_tracking_status(status_notifications, ago_notify_timeout)
    assert restarted["tracking"] is True and restarted["trk"] == 1 and restarted["stop"] == 0


@pytest.mark.parametrize("mode", ["cancel", "manual_stop_then_start"])
async def test_old_deadline_does_not_stop_tracking(
    ago_client, tracking_command, status_notifications, config_notifications,
    ago_notify_timeout, mode,
):
    deadline = asyncio.get_running_loop().time() + MINUTE_SECONDS + TIMER_TOLERANCE_SECONDS
    await tracking_command("start_tracking", duration_min=1)
    baseline = await _next_tracking_status(status_notifications, ago_notify_timeout)
    if mode == "cancel":
        await tracking_command("tracking_duration", duration_min=0)
    else:
        await tracking_command("stop_tracking")
        assert await _next_tracking_status(status_notifications, ago_notify_timeout) == {
            "tracking": False, "session": 0, "trk": 0, "stop": 1,
        }
        await tracking_command("start_tracking")  # Omitted duration means Off.
        baseline = await _next_tracking_status(status_notifications, ago_notify_timeout)
        assert baseline["stop"] == 0
    config = proto.decode_cbor(bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)))
    with pytest.raises(asyncio.TimeoutError):
        await _next_tracking_status(status_notifications, deadline - asyncio.get_running_loop().time())
    await _assert_unchanged(ago_client, status_notifications, config_notifications, baseline, config)
