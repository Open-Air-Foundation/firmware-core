"""Tests for Status characteristic NOTIFY semantics.

The device pushes a Status notification on every urgent tracking state
transition (start, stop, pause, resume). These tests verify the contract
the AGo BLE Client Spec exposes to the phone app.

BLE-issued tracking commands produce two notifications per transition: one on
Status (state change) and one on Config (cmd_result). Both are checked.
"""

from __future__ import annotations

import asyncio

from bleak import BleakClient

import ago_protocol as proto
from conftest import NotificationCollector


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

async def _read_status(client: BleakClient) -> dict:
    raw = await client.read_gatt_char(proto.CHAR_STATUS_UUID)
    return proto.decode_cbor(bytes(raw))


async def _ensure_tracking_idle(client: BleakClient) -> None:
    """If a tracking session is active, stop it and wait for the device to clear.

    Idempotent — leaves the device idle whether or not a session was open.
    Bounded poll so a stuck device does not hang the suite.
    """
    if not (await _read_status(client)).get("tracking"):
        return

    await client.write_gatt_char(
        proto.CHAR_CONFIG_UUID,
        proto.encode_command("stop_tracking"),
        response=True,
    )

    # Poll Read until tracking clears (max 5 s).
    deadline = asyncio.get_event_loop().time() + 5.0
    while asyncio.get_event_loop().time() < deadline:
        if not (await _read_status(client)).get("tracking"):
            return
        await asyncio.sleep(0.1)

    raise RuntimeError("Could not clear pre-existing tracking session")


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestStatusNotify:
    """Status NOTIFY must fire on urgent tracking transitions."""

    async def test_start_tracking_pushes_status_notify(
        self,
        ago_client: BleakClient,
        ago_notify_timeout: float,
        status_notifications: NotificationCollector,
        config_notifications: NotificationCollector,
    ):
        """start_tracking command -> Status NOTIFY with tracking=true, session>0.

        Also confirms the matching Config cmd_result arrives. Cleanup
        stops the session so other tests start from idle.
        """
        await _ensure_tracking_idle(ago_client)
        status_notifications.drain()
        config_notifications.drain()

        try:
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID,
                proto.encode_command("start_tracking"),
                response=True,
            )

            # Status NOTIFY: tracking transition delta — {tracking, session, trk}.
            status_raw = await status_notifications.wait_for(
                timeout=ago_notify_timeout
            )
            status = proto.decode_cbor(status_raw)
            assert isinstance(status, dict), (
                f"Status notify is not a CBOR map: {status!r}"
            )
            assert set(status.keys()) == proto.STATUS_NOTIFY_KEYS, (
                f"Status NOTIFY is not the transition delta.\n"
                f"  Expected: {proto.STATUS_NOTIFY_KEYS}\n"
                f"  Got:      {set(status.keys())}"
            )
            assert status["tracking"] is True, (
                f"Status NOTIFY did not report tracking=true: {status!r}"
            )
            assert isinstance(status["session"], int) and status["session"] > 0, (
                f"Status NOTIFY session id invalid: {status['session']!r}"
            )
            assert type(status["trk"]) is int
            assert status["trk"] == proto.TRACKING_STATES["recording"]

            # READ remains the full 10-key snapshot.
            read_back = await _read_status(ago_client)
            assert set(read_back.keys()) == proto.STATUS_ALL_KEYS, (
                f"Status READ is not the full snapshot.\n"
                f"  Expected: {proto.STATUS_ALL_KEYS}\n"
                f"  Got:      {set(read_back.keys())}"
            )
            assert read_back["tracking"] is True
            assert read_back["session"] == status["session"]
            assert type(read_back["trk"]) is int
            assert read_back["trk"] == proto.TRACKING_STATES["recording"]

            # Config cmd_result: command acknowledgement on the issuer's
            # characteristic. Distinct event, distinct characteristic.
            cmd_raw = await config_notifications.wait_for(
                timeout=ago_notify_timeout
            )
            cmd = proto.decode_cbor(cmd_raw)
            assert cmd.get("type") == "cmd_result", (
                f"Expected cmd_result, got type={cmd.get('type')!r}"
            )
            assert cmd.get("cmd") == "start_tracking", (
                f"cmd mismatch: {cmd.get('cmd')!r}"
            )
            assert cmd.get("ok") is True, f"cmd_result not ok: {cmd!r}"
        finally:
            await _ensure_tracking_idle(ago_client)

    async def test_stop_tracking_pushes_status_notify(
        self,
        ago_client: BleakClient,
        ago_notify_timeout: float,
        status_notifications: NotificationCollector,
        config_notifications: NotificationCollector,
    ):
        """stop_tracking command -> Status NOTIFY with tracking=false, session=0."""
        await _ensure_tracking_idle(ago_client)

        # Open a session so we have something to stop. Drain both
        # collectors after so the next wait_for() returns the stop notify
        # rather than the start one.
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID,
            proto.encode_command("start_tracking"),
            response=True,
        )
        await status_notifications.wait_for(timeout=ago_notify_timeout)
        await config_notifications.wait_for(timeout=ago_notify_timeout)
        status_notifications.drain()
        config_notifications.drain()

        try:
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID,
                proto.encode_command("stop_tracking"),
                response=True,
            )

            status_raw = await status_notifications.wait_for(
                timeout=ago_notify_timeout
            )
            status = proto.decode_cbor(status_raw)
            assert set(status.keys()) == proto.STATUS_NOTIFY_KEYS, (
                f"Status NOTIFY is not the transition delta.\n"
                f"  Expected: {proto.STATUS_NOTIFY_KEYS}\n"
                f"  Got:      {set(status.keys())}"
            )
            assert status["tracking"] is False, (
                f"Status NOTIFY did not report tracking=false: {status!r}"
            )
            assert status["session"] == 0, (
                f"Status NOTIFY did not clear session: {status['session']!r}"
            )
            assert type(status["trk"]) is int
            assert status["trk"] == proto.TRACKING_STATES["idle"]

            # READ remains the full 10-key snapshot.
            read_back = await _read_status(ago_client)
            assert set(read_back.keys()) == proto.STATUS_ALL_KEYS, (
                f"Status READ is not the full snapshot: {set(read_back.keys())}"
            )
            assert read_back["tracking"] is False
            assert read_back["session"] == 0
            assert type(read_back["trk"]) is int
            assert read_back["trk"] == proto.TRACKING_STATES["idle"]

            cmd_raw = await config_notifications.wait_for(
                timeout=ago_notify_timeout
            )
            cmd = proto.decode_cbor(cmd_raw)
            assert cmd.get("type") == "cmd_result"
            assert cmd.get("cmd") == "stop_tracking"
            assert cmd.get("ok") is True, f"cmd_result not ok: {cmd!r}"
        finally:
            await _ensure_tracking_idle(ago_client)

    async def test_pause_resume_preserves_session(
        self,
        ago_client: BleakClient,
        ago_notify_timeout: float,
        status_notifications: NotificationCollector,
        config_notifications: NotificationCollector,
    ):
        """Pause/resume changes trk while keeping tracking=true and the same ID."""
        await _ensure_tracking_idle(ago_client)
        status_notifications.drain()
        config_notifications.drain()
        session_id = None

        try:
            for command, state in (
                ("start_tracking", proto.TRACKING_STATES["recording"]),
                ("pause_tracking", proto.TRACKING_STATES["paused"]),
                ("resume_tracking", proto.TRACKING_STATES["recording"]),
            ):
                await ago_client.write_gatt_char(
                    proto.CHAR_CONFIG_UUID,
                    proto.encode_command(command),
                    response=True,
                )
                status_raw = await status_notifications.wait_for(
                    timeout=ago_notify_timeout
                )
                status = proto.decode_cbor(status_raw)
                assert set(status.keys()) == proto.STATUS_NOTIFY_KEYS
                assert status["tracking"] is True
                assert type(status["trk"]) is int and status["trk"] == state
                assert type(status["session"]) is int and status["session"] > 0
                if session_id is None:
                    session_id = status["session"]
                assert status["session"] == session_id, (
                    f"Session changed after {command}: {status!r}"
                )

                read_back = await _read_status(ago_client)
                assert set(read_back.keys()) == proto.STATUS_ALL_KEYS
                assert read_back["tracking"] is True
                assert type(read_back["trk"]) is int and read_back["trk"] == state
                assert read_back["session"] == session_id

                cmd_raw = await config_notifications.wait_for(
                    timeout=ago_notify_timeout
                )
                cmd = proto.decode_cbor(cmd_raw)
                assert cmd.get("type") == "cmd_result"
                assert cmd.get("cmd") == command
                assert cmd.get("ok") is True, f"cmd_result not ok: {cmd!r}"
        finally:
            await _ensure_tracking_idle(ago_client)

    async def test_already_tracking_does_not_push_status_notify(
        self,
        ago_client: BleakClient,
        ago_notify_timeout: float,
        status_notifications: NotificationCollector,
        config_notifications: NotificationCollector,
    ):
        """A redundant start_tracking must not push a spurious Status NOTIFY.

        The orchestrator rejects the second start with err=already_tracking
        on Config, but the actual state did not change — so Status must
        stay silent.
        """
        await _ensure_tracking_idle(ago_client)

        # First start: produces both notifies. Consume them.
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID,
            proto.encode_command("start_tracking"),
            response=True,
        )
        await status_notifications.wait_for(timeout=ago_notify_timeout)
        await config_notifications.wait_for(timeout=ago_notify_timeout)
        status_notifications.drain()
        config_notifications.drain()

        try:
            # Second start: must fail with already_tracking on Config and
            # leave Status silent.
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID,
                proto.encode_command("start_tracking"),
                response=True,
            )

            cmd_raw = await config_notifications.wait_for(
                timeout=ago_notify_timeout
            )
            cmd = proto.decode_cbor(cmd_raw)
            assert cmd.get("ok") is False, (
                f"Redundant start_tracking unexpectedly succeeded: {cmd!r}"
            )
            assert cmd.get("err") == "already_tracking", (
                f"Expected err=already_tracking, got {cmd.get('err')!r}"
            )

            # Brief settle so a slow notify would have time to arrive.
            await asyncio.sleep(0.5)
            assert status_notifications.count == 0, (
                f"Spurious Status NOTIFY on redundant start_tracking: "
                f"{status_notifications.all_received!r}"
            )
        finally:
            await _ensure_tracking_idle(ago_client)
