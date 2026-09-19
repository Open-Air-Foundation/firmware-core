"""Tests for the Status characteristic (read-only, CBOR payload).

A single module-scoped fixture reads the Status characteristic once.
All tests validate different aspects of that same captured payload.
"""

from __future__ import annotations

import pytest_asyncio
from bleak import BleakClient

import ago_protocol as proto


# ---------------------------------------------------------------------------
# Module-scoped fixture: read Status once
# ---------------------------------------------------------------------------

@pytest_asyncio.fixture(scope="module")
async def status_payload(ago_client: BleakClient) -> dict:
    """Read the Status characteristic once and return the decoded CBOR map."""
    data = await ago_client.read_gatt_char(proto.CHAR_STATUS_UUID)
    return proto.decode_cbor(bytes(data))


# ---------------------------------------------------------------------------
# Tests — pure data validation, no async
# ---------------------------------------------------------------------------

class TestStatus:
    """Verify the Status characteristic returns a valid 10-key CBOR map."""

    def test_read_status(self, status_payload: dict):
        """Reading the Status characteristic must return valid CBOR map."""
        assert isinstance(status_payload, dict), (
            f"Expected CBOR map, got {type(status_payload).__name__}"
        )

    def test_all_keys_present(self, status_payload: dict):
        """The Status payload must contain exactly the 10 expected keys."""
        missing = proto.STATUS_ALL_KEYS - set(status_payload.keys())
        extra = set(status_payload.keys()) - proto.STATUS_ALL_KEYS
        assert not missing, f"Missing Status keys: {missing}"
        assert not extra, f"Unexpected Status keys: {extra}"

    def test_field_types(self, status_payload: dict):
        """Each Status field must have the expected type."""
        for key, expected_types in proto.STATUS_FIELD_TYPES.items():
            assert key in status_payload, f"Status key '{key}' missing"
            value = status_payload[key]
            assert type(value) in expected_types, (
                f"Status['{key}']: expected {expected_types}, "
                f"got {type(value).__name__} = {value!r}"
            )

    def test_tracking_state_valid(self, status_payload: dict):
        """The enum must agree with the legacy active flag and session ID."""
        state = status_payload["trk"]
        assert type(state) is int and state in proto.TRACKING_STATES.values(), (
            f"Invalid tracking state: {state!r}"
        )
        active = state != proto.TRACKING_STATES["idle"]
        assert status_payload["tracking"] is active
        session = status_payload["session"]
        assert type(session) is int
        assert session > 0 if active else session == 0

    def test_charging_state_valid(self, status_payload: dict):
        """The 'charging' field must be one of the known charging state strings."""
        charging = status_payload.get("charging")
        assert charging in proto.CHARGING_STATES, (
            f"Unknown charging state: '{charging}'. "
            f"Expected one of: {proto.CHARGING_STATES}"
        )

    def test_battery_percentage_range(self, status_payload: dict):
        """The 'bat_pct' field must be in 0-100 range."""
        bat_pct = status_payload.get("bat_pct")
        assert isinstance(bat_pct, int), f"bat_pct is not int: {bat_pct!r}"
        assert 0 <= bat_pct <= 100, f"bat_pct out of range: {bat_pct}"

    def test_battery_voltage_non_negative(self, status_payload: dict):
        """The 'bat_v' field must be non-negative (clamped to 0.0 by firmware)."""
        bat_v = status_payload.get("bat_v")
        assert isinstance(bat_v, (int, float)), f"bat_v is not numeric: {bat_v!r}"
        assert bat_v >= 0, f"bat_v is negative: {bat_v}"
