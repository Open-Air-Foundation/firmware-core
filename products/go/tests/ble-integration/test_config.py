"""Tests for the Config characteristic (read/write/notify, CBOR payload).

TestConfigRead uses a module-scoped fixture that reads Config once — all read
tests are synchronous validators of that shared payload.

TestConfigWrite and TestConfigCommand are async because they perform interactive
write/notify cycles that genuinely need per-test BLE I/O.
"""

from __future__ import annotations

import asyncio
import math

import pytest
import pytest_asyncio
from bleak import BleakClient

import ago_protocol as proto
from conftest import NotificationCollector


# ---------------------------------------------------------------------------
# Module-scoped fixture: read Config once
# ---------------------------------------------------------------------------

@pytest_asyncio.fixture(scope="module")
async def config_payload(ago_client: BleakClient) -> dict:
    """Read the Config characteristic once and return the decoded CBOR map."""
    data = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
    return proto.decode_cbor(bytes(data))


# ---------------------------------------------------------------------------
# Read tests — pure data validation, no async
# ---------------------------------------------------------------------------

class TestConfigRead:
    """Verify reading the Config characteristic returns a valid 17-key map."""

    def test_read_config(self, config_payload: dict):
        """Reading Config must return valid CBOR map."""
        assert isinstance(config_payload, dict), (
            f"Expected CBOR map, got {type(config_payload).__name__}"
        )

    def test_all_keys_present(self, config_payload: dict):
        """Config read must contain exactly the 17 expected keys."""
        missing = proto.CONFIG_READ_KEYS - set(config_payload.keys())
        extra = set(config_payload.keys()) - proto.CONFIG_READ_KEYS
        assert not missing, f"Missing Config keys: {missing}"
        assert not extra, f"Unexpected Config keys: {extra}"

    def test_field_types(self, config_payload: dict):
        """Each Config field must have the expected type."""
        for key in proto.CONFIG_READ_KEYS:
            assert key in config_payload, f"Config key '{key}' missing"
            value = config_payload[key]
            expected_types = proto.CONFIG_FIELD_TYPES[key]
            assert type(value) in expected_types, (
                f"Config['{key}']: expected {expected_types}, "
                f"got {type(value).__name__} = {value!r}"
            )

    def test_gps_mode_valid(self, config_payload: dict):
        """'gps_mode' must be one of the known enum strings."""
        gps_mode = config_payload.get("gps_mode")
        assert gps_mode in proto.GPS_MODES, (
            f"Unknown gps_mode: '{gps_mode}'. Expected one of: {proto.GPS_MODES}"
        )

    def test_operating_mode_valid(self, config_payload: dict):
        """'op_mode' must be one of the known enum strings."""
        op_mode = config_payload.get("op_mode")
        assert op_mode in proto.OPERATING_MODES, (
            f"Unknown op_mode: '{op_mode}'. Expected one of: {proto.OPERATING_MODES}"
        )

    def test_compact_device_fields_valid(self, config_payload: dict):
        """Compact buzzer and sensor config fields must use valid ranges."""
        assert type(config_payload["buz"]) is bool
        assert type(config_payload["abc"]) is int
        assert config_payload["abc"] == proto.CO2_ABC_DAYS_DISABLED or (
            proto.CO2_ABC_DAYS_MIN
            <= config_payload["abc"]
            <= proto.CO2_ABC_DAYS_MAX
        )
        assert type(config_payload["tlo"]) is int
        assert (
            proto.LEARNING_OFFSET_HOURS_MIN
            <= config_payload["tlo"]
            <= proto.LEARNING_OFFSET_HOURS_MAX
        )
        assert type(config_payload["nlo"]) is int
        assert (
            proto.LEARNING_OFFSET_HOURS_MIN
            <= config_payload["nlo"]
            <= proto.LEARNING_OFFSET_HOURS_MAX
        )

    def test_correction_maps_valid(self, config_payload: dict):
        """Correction maps must expose the versioned compact value arrays."""
        for key in proto.CORRECTION_MAP_KEYS:
            correction = config_payload[key]
            assert set(correction) == {"s", "v"}, (
                f"Config['{key}'] keys mismatch: expected {{'s', 'v'}}, "
                f"got {set(correction)}"
            )
            assert correction["s"] == proto.CORRECTION_SCHEMA_VERSION
            values = correction["v"]
            expected_length = 4 if key == "pm25_corr" else 3
            assert isinstance(values, list)
            assert len(values) == expected_length

            algorithm = values[0]
            assert type(algorithm) is int
            if key == "pm25_corr":
                assert algorithm in proto.PM25_CORRECTION_ALGORITHMS.values()
                assert type(values[3]) is int
                assert values[3] == 0, "Config['pm25_corr'] reserved flags must be clear"
            else:
                assert algorithm in proto.LINEAR_CORRECTION_ALGORITHMS.values()
            assert isinstance(values[1], float)
            assert isinstance(values[2], float)

            assert math.isfinite(values[1])
            assert math.isfinite(values[2])

# ---------------------------------------------------------------------------
# Write tests — async, interactive BLE I/O per test
# ---------------------------------------------------------------------------

class TestConfigWrite:
    """Verify writing config changes and receiving notifications."""

    @pytest.mark.parametrize("key", ["temp_f", "alt_ft"])
    async def test_set_presentation_config_notification(
        self,
        key: str,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Writing a 'set' op must trigger a Config DELTA notification.

        We toggle one display unit and then restore it. The notification is a
        delta: the 'type' discriminator plus only the single changed key.
        """
        # Read current config to know original value
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))
        original_value = original[key]
        new_value = not original_value

        try:
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID,
                proto.encode_config_set(**{key: new_value}),
                response=True,
            )

            notif_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
            payload = proto.decode_cbor(notif_data)

            assert payload.get("type") == proto.CONFIG_NOTIFY_TYPE, (
                f"Expected type='config', got '{payload.get('type')}'"
            )
            assert set(payload.keys()) == {"type", key}, (
                f"Config delta keys mismatch.\n"
                f"  Expected: {{'type', '{key}'}}\n"
                f"  Got:      {set(payload.keys())}"
            )
            assert payload[key] == new_value, (
                f"{key} not updated in notification: "
                f"expected {new_value}, got {payload[key]}"
            )
        finally:
            current = proto.decode_cbor(
                bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID))
            )
            if current[key] != original_value:
                await ago_client.write_gatt_char(
                    proto.CHAR_CONFIG_UUID,
                    proto.encode_config_set(**{key: original_value}),
                    response=True,
                )
                await config_notifications.wait_for(timeout=ago_notify_timeout)

    @pytest.mark.parametrize("key", ["temp_f", "alt_ft"])
    async def test_set_presentation_config_roundtrip(
        self,
        key: str,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Setting a config value and re-reading must reflect the change."""
        # Read current config
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))
        original_value = original[key]
        new_value = not original_value

        try:
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID,
                proto.encode_config_set(**{key: new_value}),
                response=True,
            )

            await config_notifications.wait_for(timeout=ago_notify_timeout)

            updated = proto.decode_cbor(
                bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID))
            )
            assert updated[key] == new_value, (
                f"Config roundtrip failed: wrote {key}={new_value}, "
                f"read back {updated[key]}"
            )
        finally:
            current = proto.decode_cbor(
                bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID))
            )
            if current[key] != original_value:
                await ago_client.write_gatt_char(
                    proto.CHAR_CONFIG_UUID,
                    proto.encode_config_set(**{key: original_value}),
                    response=True,
                )
                await config_notifications.wait_for(timeout=ago_notify_timeout)

    @pytest.mark.parametrize("key", ["buz", "abc", "tlo", "nlo"])
    async def test_set_compact_device_field_roundtrip(
        self,
        key: str,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Compact device config fields must persist and emit exact deltas."""
        original = proto.decode_cbor(
            bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID))
        )
        original_value = original[key]
        if key == "buz":
            new_value = not original_value
        elif key == "abc":
            new_value = (
                proto.CO2_ABC_DAYS_MIN
                if original_value == proto.CO2_ABC_DAYS_DISABLED
                else proto.CO2_ABC_DAYS_DISABLED
            )
        else:
            new_value = (
                proto.LEARNING_OFFSET_HOURS_MIN + 1
                if original_value == proto.LEARNING_OFFSET_HOURS_MIN
                else proto.LEARNING_OFFSET_HOURS_MIN
            )

        try:
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID,
                proto.encode_config_set(**{key: new_value}),
                response=True,
            )
            payload = proto.decode_cbor(
                await config_notifications.wait_for(timeout=ago_notify_timeout)
            )
            assert payload == {"type": proto.CONFIG_NOTIFY_TYPE, key: new_value}

            updated = proto.decode_cbor(
                bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID))
            )
            assert updated[key] == new_value
        finally:
            current = proto.decode_cbor(
                bytes(await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID))
            )
            if current[key] != original_value:
                await ago_client.write_gatt_char(
                    proto.CHAR_CONFIG_UUID,
                    proto.encode_config_set(**{key: original_value}),
                    response=True,
                )
                await config_notifications.wait_for(timeout=ago_notify_timeout)

    async def test_set_temperature_correction_updates_config_without_measures_notify(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """A BLE correction write updates Config without a Measures notify."""
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))
        original_correction = original["temp_corr"]
        new_intercept = float(original_correction["v"][2]) + 0.25
        new_correction = {"s": 1, "v": [1, 1.0, new_intercept]}

        config_notifications.drain()
        try:
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID,
                proto.encode_linear_correction("temp_corr", "custom", 1.0, new_intercept),
                response=True,
            )

            config_payload = proto.decode_cbor(
                await config_notifications.wait_for(timeout=ago_notify_timeout)
            )
            assert set(config_payload) == {"type", "temp_corr"}
            received = config_payload["temp_corr"]
            assert received["s"] == new_correction["s"]
            assert received["v"][0] == new_correction["v"][0]
            assert received["v"][1] == pytest.approx(new_correction["v"][1], abs=1e-6)
            assert received["v"][2] == pytest.approx(new_correction["v"][2], abs=1e-6)

        finally:
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID,
                proto.encode_linear_correction(
                    "temp_corr",
                    next(
                        name
                        for name, value in proto.LINEAR_CORRECTION_ALGORITHMS.items()
                        if value == original_correction["v"][0]
                    ),
                    float(original_correction["v"][1]),
                    float(original_correction["v"][2]),
                ),
                response=True,
            )
            await config_notifications.wait_for(timeout=ago_notify_timeout)

    async def test_invalid_correction_is_rejected(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Unsupported correction algorithms must not change persisted settings."""
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))

        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID,
            proto._encode_correction_set("temp_corr", 99, 1.0, 0.0, None),
            response=True,
        )

        payload = proto.decode_cbor(
            await config_notifications.wait_for(timeout=ago_notify_timeout)
        )
        assert payload == {
            "type": "cmd_result",
            "cmd": "set",
            "ok": False,
            "err": proto.ERR_INVALID_CONFIG_VALUE,
        }

        updated = proto.decode_cbor(
            await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        )
        assert updated["temp_corr"] == original["temp_corr"]

    async def test_config_notify_field_types(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Config delta fields must have the correct types.

        Toggles temp_f (a real change) so the delta carries the field, then
        type-checks only the keys present in the delta.
        """
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))
        original_temp_f = original["temp_f"]

        try:
            write_data = proto.encode_config_set(temp_f=not original_temp_f)
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID, write_data, response=True,
            )

            notif_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
            payload = proto.decode_cbor(notif_data)

            assert set(payload.keys()) == {"type", "temp_f"}, (
                f"Expected delta {{'type', 'temp_f'}}, got {set(payload.keys())}"
            )
            # Type-check every key present in the delta.
            for key in payload:
                value = payload[key]
                expected_types = proto.CONFIG_FIELD_TYPES[key]
                assert type(value) in expected_types, (
                    f"Config delta['{key}']: expected {expected_types}, "
                    f"got {type(value).__name__} = {value!r}"
                )
        finally:
            # Restore original value
            restore_data = proto.encode_config_set(temp_f=original_temp_f)
            await ago_client.write_gatt_char(
                proto.CHAR_CONFIG_UUID, restore_data, response=True,
            )
            await config_notifications.wait_for(timeout=ago_notify_timeout)

    async def test_noop_set_emits_no_notification(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """A 'set' that changes nothing emits no Config notification."""
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))

        # Write the current value back — no field changes.
        write_data = proto.encode_config_set(temp_f=original["temp_f"])
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        with pytest.raises(asyncio.TimeoutError):
            await config_notifications.wait_for(timeout=min(ago_notify_timeout, 1.0))

    async def test_multi_field_set_rejected(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """A 'set' with more than one config key is rejected single_field_only.

        No value is applied and no config delta is sent — only a cmd_result
        error.
        """
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        original = proto.decode_cbor(bytes(raw))

        write_data = proto.encode_config_set(
            meas_int=original["meas_int"] + 1,
            temp_f=not original["temp_f"],
        )
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        notif_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(notif_data)

        assert payload.get("type") == "cmd_result", (
            f"Expected cmd_result, got '{payload.get('type')}'"
        )
        assert payload.get("ok") is False, f"Expected ok=false, got {payload!r}"
        assert payload.get("err") == proto.ERR_SINGLE_FIELD_ONLY, (
            f"Expected err='single_field_only', got '{payload.get('err')}'"
        )

        # Nothing changed — re-read confirms both fields untouched.
        raw2 = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        after = proto.decode_cbor(bytes(raw2))
        assert after["meas_int"] == original["meas_int"]
        assert after["temp_f"] == original["temp_f"]

    async def test_aiding_key_under_set_rejected(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """An aiding key under op:'set' is rejected as unknown_config_key.

        Aiding keys are command arguments (op:'cmd' / set_aiding), meaningless
        under 'set'.
        """
        write_data = proto.encode_config_set(lat=47.376887)
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        notif_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(notif_data)

        assert payload.get("type") == "cmd_result"
        assert payload.get("ok") is False
        assert payload.get("err") == proto.ERR_UNKNOWN_CONFIG_KEY, (
            f"Expected err='unknown_config_key', got '{payload.get('err')}'"
        )


# ---------------------------------------------------------------------------
# Command tests — async, interactive BLE I/O
# ---------------------------------------------------------------------------

class TestConfigCommand:
    """Verify command execution via Config writes."""

    async def test_command_progress_and_result_format(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """Writing a long-running command must trigger cmd_progress then cmd_result.

        We use 'co2_cal' as the test command. The device sends a cmd_progress
        notification immediately (acknowledging the command), followed by a
        cmd_result notification when calibration completes. The result may be
        success or failure (sensor may not support calibration), but both
        notification formats must be correct.
        """
        write_data = proto.encode_command("co2_cal")
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        # --- First notification: cmd_progress ---
        progress_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
        progress = proto.decode_cbor(progress_data)

        assert progress.get("type") == "cmd_progress", (
            f"Expected type='cmd_progress', got '{progress.get('type')}'"
        )
        assert set(progress.keys()) == proto.CMD_PROGRESS_KEYS, (
            f"cmd_progress keys mismatch.\n"
            f"  Expected: {proto.CMD_PROGRESS_KEYS}\n"
            f"  Got:      {set(progress.keys())}"
        )
        assert progress["cmd"] == "co2_cal", (
            f"cmd mismatch: expected 'co2_cal', got '{progress['cmd']}'"
        )
        assert "ok" not in progress, "cmd_progress must not contain 'ok' key"
        assert "err" not in progress, "cmd_progress must not contain 'err' key"

        # --- Second notification: cmd_result ---
        result_data = await config_notifications.wait_for(timeout=ago_notify_timeout)
        payload = proto.decode_cbor(result_data)

        assert payload.get("type") == "cmd_result", (
            f"Expected type='cmd_result', got '{payload.get('type')}'"
        )

        # Must have cmd and ok keys
        assert "cmd" in payload, f"'cmd' key missing from cmd_result: {payload}"
        assert "ok" in payload, f"'ok' key missing from cmd_result: {payload}"

        assert payload["cmd"] == "co2_cal", (
            f"cmd mismatch: expected 'co2_cal', got '{payload['cmd']}'"
        )
        assert isinstance(payload["ok"], bool), (
            f"'ok' must be bool, got {type(payload['ok']).__name__}"
        )

        # If failure, 'err' key may be present
        if not payload["ok"]:
            assert set(payload.keys()) <= proto.CMD_RESULT_KEYS_FAILURE, (
                f"Failure cmd_result has unexpected keys: "
                f"{set(payload.keys()) - proto.CMD_RESULT_KEYS_FAILURE}"
            )
            if "err" in payload:
                assert isinstance(payload["err"], str), (
                    f"'err' must be str, got {type(payload['err']).__name__}"
                )
        else:
            assert set(payload.keys()) == proto.CMD_RESULT_KEYS_SUCCESS, (
                f"Success cmd_result keys mismatch: "
                f"expected {proto.CMD_RESULT_KEYS_SUCCESS}, "
                f"got {set(payload.keys())}"
            )

    async def test_read_after_command_returns_config_snapshot(
        self,
        ago_client: BleakClient,
        config_notifications: NotificationCollector,
        ago_notify_timeout: float,
    ):
        """A Config READ after a command returns the config snapshot, not cmd_result.

        Command notifications go out via notify(data,len) without touching the
        stored value, so the Config characteristic always reads as the full
        config snapshot regardless of the last notification kind.
        """
        write_data = proto.encode_command("co2_cal")
        await ago_client.write_gatt_char(
            proto.CHAR_CONFIG_UUID, write_data, response=True,
        )

        # Drain cmd_progress then cmd_result so they don't leak into later tests.
        await config_notifications.wait_for(timeout=ago_notify_timeout)  # progress
        await config_notifications.wait_for(timeout=ago_notify_timeout)  # result

        # READ must return the config snapshot, not the cmd_result.
        raw = await ago_client.read_gatt_char(proto.CHAR_CONFIG_UUID)
        payload = proto.decode_cbor(bytes(raw))

        assert payload.get("type") is None, (
            f"Config READ leaked a notification payload: {payload!r}"
        )
        assert set(payload.keys()) == proto.CONFIG_READ_KEYS, (
            f"Config READ is not the full snapshot.\n"
            f"  Expected: {proto.CONFIG_READ_KEYS}\n"
            f"  Got:      {set(payload.keys())}"
        )
