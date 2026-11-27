"""Tests for the History characteristic (write/notify, CBOR + binary).

These tests exercise the full history download protocol: list sessions,
start download, receive binary data, fill gaps, and end download.

Tests that require stored session data are gracefully skipped when the
device reports zero sessions.
"""

from __future__ import annotations

import asyncio

import pytest
from bleak import BleakClient

import ago_protocol as proto
from conftest import NotificationCollector


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

async def _list_sessions(
    client: BleakClient,
    collector: NotificationCollector,
    timeout: float,
) -> list[dict]:
    """Send a 'list' command and return the assembled sessions array.

    The device sends paginated responses — one notification per page.
    Collect pages until ``pg == tpg``.
    """
    write_data = proto.encode_history_list()
    await client.write_gatt_char(
        proto.CHAR_HISTORY_UUID, write_data, response=True,
    )

    all_sessions: list[dict] = []
    deadline = asyncio.get_event_loop().time() + timeout

    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            break

        notif_data = await collector.wait_for(timeout=remaining)
        tag, payload = proto.parse_history_notification(notif_data)
        assert tag == proto.HISTORY_TAG_CBOR, f"Expected CBOR tag, got 0x{tag:02x}"
        assert payload.get("type") == "sessions", (
            f"Expected type='sessions', got '{payload.get('type')}'"
        )

        all_sessions.extend(payload.get("sessions", []))

        # Last page reached when pg == tpg
        pg = payload.get("pg", 1)
        tpg = payload.get("tpg", 1)
        if pg >= tpg:
            break

    return all_sessions


async def _collect_download(
    collector: NotificationCollector,
    timeout: float,
) -> tuple[dict | None, list[tuple[int, list[dict]]], dict | None]:
    """Collect all notifications from a download stream.

    Returns (started_msg, binary_chunks, done_msg) where:
      - started_msg: the CBOR 'started' response (or None)
      - binary_chunks: list of (point_index, [route_point_dicts])
      - done_msg: the CBOR 'done' response (or None)
    """
    started = None
    chunks: list[tuple[int, list[dict]]] = []
    done = None

    deadline = asyncio.get_event_loop().time() + timeout

    while True:
        remaining = deadline - asyncio.get_event_loop().time()
        if remaining <= 0:
            break

        try:
            data = await collector.wait_for(timeout=remaining)
        except asyncio.TimeoutError:
            break

        tag, payload = proto.parse_history_notification(data)

        if tag == proto.HISTORY_TAG_CBOR:
            msg_type = payload.get("type")
            if msg_type == "started":
                started = payload
            elif msg_type == "done":
                done = payload
                break  # Download complete
            elif msg_type == "error":
                pytest.fail(f"History error from device: {payload}")
        elif tag == proto.HISTORY_TAG_BINARY:
            point_index, points = payload
            chunks.append((point_index, points))

    return started, chunks, done


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

class TestHistoryList:
    """Verify the session list command and response format."""

    async def test_list_sessions(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Writing 'list' must return a CBOR response with type='sessions' and pagination fields."""
        write_data = proto.encode_history_list()
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, write_data, response=True,
        )

        notif_data = await history_notifications.wait_for(timeout=ago_notify_timeout)
        tag, payload = proto.parse_history_notification(notif_data)

        assert tag == proto.HISTORY_TAG_CBOR, f"Expected CBOR tag, got 0x{tag:02x}"
        assert isinstance(payload, dict), f"Expected dict, got {type(payload).__name__}"
        assert payload.get("type") == "sessions", (
            f"Expected type='sessions', got '{payload.get('type')}'"
        )
        assert "sessions" in payload, "'sessions' key missing from list response"
        assert isinstance(payload["sessions"], list), (
            f"'sessions' must be a list, got {type(payload['sessions']).__name__}"
        )
        # Pagination fields
        assert "pg" in payload, "'pg' (page) key missing from list response"
        assert "tpg" in payload, "'tpg' (total pages) key missing from list response"
        assert "cnt" in payload, "'cnt' (count) key missing from list response"
        assert isinstance(payload["pg"], int), f"'pg' must be int, got {type(payload['pg']).__name__}"
        assert isinstance(payload["tpg"], int), f"'tpg' must be int, got {type(payload['tpg']).__name__}"
        assert isinstance(payload["cnt"], int), f"'cnt' must be int, got {type(payload['cnt']).__name__}"
        assert payload["pg"] >= 1, f"'pg' must be >= 1, got {payload['pg']}"
        assert payload["tpg"] >= 1, f"'tpg' must be >= 1, got {payload['tpg']}"
        assert payload["pg"] <= payload["tpg"], (
            f"'pg' ({payload['pg']}) must be <= 'tpg' ({payload['tpg']})"
        )

    async def test_session_entry_format(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Each session entry must have 'id' (uint), 'pts' (uint), 'ts' (uint)."""
        sessions = await _list_sessions(
            ago_client, history_notifications, ago_notify_timeout,
        )

        if not sessions:
            pytest.skip("No sessions on device -- cannot verify entry format")

        for i, entry in enumerate(sessions):
            assert isinstance(entry, dict), (
                f"Session[{i}] is not a dict: {entry!r}"
            )
            missing = proto.SESSION_ENTRY_KEYS - set(entry.keys())
            assert not missing, (
                f"Session[{i}] missing keys: {missing}. Entry: {entry}"
            )
            assert isinstance(entry["id"], int), (
                f"Session[{i}].id is not int: {entry['id']!r}"
            )
            assert isinstance(entry["pts"], int), (
                f"Session[{i}].pts is not int: {entry['pts']!r}"
            )
            assert isinstance(entry["ts"], int), (
                f"Session[{i}].ts is not int: {entry['ts']!r}"
            )


class TestHistoryDownload:
    """Verify the full download flow: start -> stream -> done -> end."""

    @pytest.fixture
    async def first_nonempty_session(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ) -> dict:
        """Get the first non-empty session, skip if none exist."""
        sessions = await _list_sessions(
            ago_client, history_notifications, ago_notify_timeout,
        )
        for session in sessions:
            if 0 < session["pts"] <= 65535:
                return session
        pytest.skip("No non-empty sessions within the 65,535-record export limit")

    async def test_start_download(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
        first_nonempty_session: dict,
    ):
        """Starting a download must return a 'started' CBOR response with
        the correct session ID, total points, and pt_size=56.
        """
        session_id = first_nonempty_session["id"]
        expected_pts = first_nonempty_session["pts"]

        write_data = proto.encode_history_start(session_id)
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, write_data, response=True,
        )

        # Collect the full download stream
        # Use a generous timeout since large sessions take time
        download_timeout = max(ago_notify_timeout, expected_pts * 0.1 + 10)
        started, chunks, done = await _collect_download(
            history_notifications, timeout=download_timeout,
        )

        # Verify 'started' response
        assert started is not None, "No 'started' response received"
        assert started.get("session") == session_id, (
            f"Session ID mismatch: expected {session_id}, "
            f"got {started.get('session')}"
        )
        assert started.get("total") == expected_pts, (
            f"Total points mismatch: expected {expected_pts}, "
            f"got {started.get('total')}"
        )
        assert started.get("pt_size") == proto.ROUTE_POINT_WIRE_SIZE, (
            f"pt_size mismatch: expected {proto.ROUTE_POINT_WIRE_SIZE}, "
            f"got {started.get('pt_size')}"
        )

        # End the download
        end_data = proto.encode_history_end()
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, end_data, response=True,
        )
        # Consume the 'ended' response
        try:
            end_notif = await history_notifications.wait_for(
                timeout=ago_notify_timeout,
            )
            tag, payload = proto.parse_history_notification(end_notif)
            assert tag == proto.HISTORY_TAG_CBOR
            assert payload.get("type") == "ended"
        except asyncio.TimeoutError:
            pass  # Non-critical if the ended response is missed

    async def test_binary_data_format(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
        first_nonempty_session: dict,
    ):
        """Binary data notifications must have correct wire format:
        tag 0x01, uint16_le point index, then N x 56-byte route points.
        """
        session_id = first_nonempty_session["id"]
        expected_pts = first_nonempty_session["pts"]

        write_data = proto.encode_history_start(session_id)
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, write_data, response=True,
        )

        download_timeout = max(ago_notify_timeout, expected_pts * 0.1 + 10)
        started, chunks, done = await _collect_download(
            history_notifications, timeout=download_timeout,
        )

        assert chunks, "No binary chunks received for non-empty session"

        # Verify each binary chunk
        for point_index, points in chunks:
            assert isinstance(point_index, int), (
                f"point_index is not int: {point_index!r}"
            )
            assert point_index >= 0, f"Negative point_index: {point_index}"

            for i, point in enumerate(points):
                assert isinstance(point, dict), (
                    f"Point at index {point_index + i} is not a dict"
                )
                # Must have all expected fields
                assert set(point.keys()) == set(proto.ROUTE_POINT_FIELDS), (
                    f"Point at index {point_index + i} field mismatch.\n"
                    f"  Expected: {set(proto.ROUTE_POINT_FIELDS)}\n"
                    f"  Got:      {set(point.keys())}"
                )
                # Route UTC may be zero before synchronization.
                assert point["timestamp"] >= 0

        # End the download
        end_data = proto.encode_history_end()
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, end_data, response=True,
        )
        try:
            await history_notifications.wait_for(timeout=ago_notify_timeout)
        except asyncio.TimeoutError:
            pass

    async def test_download_done_count(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
        first_nonempty_session: dict,
    ):
        """The 'done' response 'sent' count must match 'total' from 'started'."""
        session_id = first_nonempty_session["id"]
        expected_pts = first_nonempty_session["pts"]

        write_data = proto.encode_history_start(session_id)
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, write_data, response=True,
        )

        download_timeout = max(ago_notify_timeout, expected_pts * 0.1 + 10)
        started, chunks, done = await _collect_download(
            history_notifications, timeout=download_timeout,
        )

        assert started is not None, "No 'started' response"
        assert done is not None, "No 'done' response after download"
        assert done.get("sent") == started.get("total"), (
            f"'done' sent count ({done.get('sent')}) does not match "
            f"'started' total ({started.get('total')})"
        )

        # End the download
        end_data = proto.encode_history_end()
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, end_data, response=True,
        )
        try:
            await history_notifications.wait_for(timeout=ago_notify_timeout)
        except asyncio.TimeoutError:
            pass

    async def test_end_download(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
        first_nonempty_session: dict,
    ):
        """Writing 'end' must return an 'ended' CBOR response."""
        session_id = first_nonempty_session["id"]
        expected_pts = first_nonempty_session["pts"]

        # Start a download first
        write_data = proto.encode_history_start(session_id)
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, write_data, response=True,
        )

        # Consume the entire download stream
        download_timeout = max(ago_notify_timeout, expected_pts * 0.1 + 10)
        await _collect_download(history_notifications, timeout=download_timeout)

        # Drain any leftover notifications
        history_notifications.drain()

        # Send end command
        end_data = proto.encode_history_end()
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, end_data, response=True,
        )

        notif_data = await history_notifications.wait_for(
            timeout=ago_notify_timeout,
        )
        tag, payload = proto.parse_history_notification(notif_data)

        assert tag == proto.HISTORY_TAG_CBOR, f"Expected CBOR tag, got 0x{tag:02x}"
        assert payload.get("type") == "ended", (
            f"Expected type='ended', got '{payload.get('type')}'"
        )


class TestHistoryErrors:
    """Verify error handling in the history protocol."""

    async def test_invalid_session_id(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Starting a download with a non-existent session ID must return
        an error response.
        """
        # Use an implausible session ID
        write_data = proto.encode_history_start(999999999)
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, write_data, response=True,
        )

        notif_data = await history_notifications.wait_for(
            timeout=ago_notify_timeout,
        )
        tag, payload = proto.parse_history_notification(notif_data)

        assert tag == proto.HISTORY_TAG_CBOR, f"Expected CBOR tag, got 0x{tag:02x}"
        assert payload.get("type") == "error", (
            f"Expected type='error', got '{payload.get('type')}'"
        )
        assert "err" in payload, f"'err' key missing from error response: {payload}"
        assert payload["err"] == "session_not_found", (
            f"Expected err='session_not_found', got '{payload['err']}'"
        )

    async def test_fill_without_active_download(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Sending 'fill' without an active download must return an error."""
        # Make sure no download is active by sending 'end' first
        # (this is a no-op if nothing is active, but the device may respond)
        end_data = proto.encode_history_end()
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, end_data, response=True,
        )

        # Drain any response from the end command
        try:
            await history_notifications.wait_for(timeout=2.0)
        except asyncio.TimeoutError:
            pass
        history_notifications.drain()

        # Now send fill without an active download
        fill_data = proto.encode_history_fill([0, 1, 2, 3])
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, fill_data, response=True,
        )

        notif_data = await history_notifications.wait_for(
            timeout=ago_notify_timeout,
        )
        tag, payload = proto.parse_history_notification(notif_data)

        assert tag == proto.HISTORY_TAG_CBOR, f"Expected CBOR tag, got 0x{tag:02x}"
        assert payload.get("type") == "error", (
            f"Expected type='error', got '{payload.get('type')}'"
        )
        assert payload.get("err") == "no_active_download", (
            f"Expected err='no_active_download', got '{payload.get('err')}'"
        )


class TestHistoryDelete:
    """Verify single-session delete via the History characteristic."""

    async def test_delete_nonexistent_session(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Deleting a non-existent session ID must return 'session_not_found'."""
        write_data = proto.encode_history_delete(999999999)
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, write_data, response=True,
        )

        notif_data = await history_notifications.wait_for(
            timeout=ago_notify_timeout,
        )
        tag, payload = proto.parse_history_notification(notif_data)

        assert tag == proto.HISTORY_TAG_CBOR, f"Expected CBOR tag, got 0x{tag:02x}"
        assert payload.get("type") == "error", (
            f"Expected type='error', got '{payload.get('type')}'"
        )
        assert payload.get("err") == "session_not_found", (
            f"Expected err='session_not_found', got '{payload.get('err')}'"
        )

    async def test_delete_session(
        self,
        ago_client: BleakClient,
        history_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Delete a stored session and verify it disappears from the list.

        Reads device Status to avoid deleting the active tracking session.
        Skips if no deletable session exists on the device.
        """
        # Read Status to find the active tracking session (if any)
        status_data = await ago_client.read_gatt_char(proto.CHAR_STATUS_UUID)
        status = proto.decode_cbor(bytes(status_data))
        active_session_id = (
            status.get("session", 0) if status.get("tracking") else 0
        )

        # List available sessions
        sessions_before = await _list_sessions(
            ago_client, history_notifications, ago_notify_timeout,
        )
        if not sessions_before:
            pytest.skip("No sessions on device -- cannot test delete")

        # Pick a session that is NOT the active tracking session
        target = None
        for s in reversed(sessions_before):
            if s["id"] != active_session_id:
                target = s
                break

        if target is None:
            pytest.skip(
                "Only session on device is the active tracking session "
                "-- cannot delete without stopping tracking"
            )

        target_id = target["id"]

        # Read used_kb before deletion
        used_kb_before = status.get("used_kb", 0)

        # Delete the session
        write_data = proto.encode_history_delete(target_id)
        await ago_client.write_gatt_char(
            proto.CHAR_HISTORY_UUID, write_data, response=True,
        )

        notif_data = await history_notifications.wait_for(
            timeout=ago_notify_timeout,
        )
        tag, payload = proto.parse_history_notification(notif_data)

        assert tag == proto.HISTORY_TAG_CBOR, f"Expected CBOR tag, got 0x{tag:02x}"
        assert payload.get("type") == "deleted", (
            f"Expected type='deleted', got '{payload.get('type')}'. "
            f"Full response: {payload}"
        )
        assert payload.get("session") == target_id, (
            f"Session ID mismatch: expected {target_id}, "
            f"got {payload.get('session')}"
        )

        # Re-list sessions and verify the deleted one is gone
        sessions_after = await _list_sessions(
            ago_client, history_notifications, ago_notify_timeout,
        )
        remaining_ids = {s["id"] for s in sessions_after}
        assert target_id not in remaining_ids, (
            f"Session {target_id} still present after delete. "
            f"Remaining: {remaining_ids}"
        )
        assert len(sessions_after) == len(sessions_before) - 1, (
            f"Expected {len(sessions_before) - 1} sessions after delete, "
            f"got {len(sessions_after)}"
        )

        # Verify flash usage decreased (or at least did not increase)
        status_data = await ago_client.read_gatt_char(proto.CHAR_STATUS_UUID)
        status_after = proto.decode_cbor(bytes(status_data))
        used_kb_after = status_after.get("used_kb", 0)
        assert used_kb_after <= used_kb_before, (
            f"used_kb did not decrease after delete: "
            f"before={used_kb_before}, after={used_kb_after}"
        )
