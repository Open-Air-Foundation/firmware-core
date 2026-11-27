"""Tests for the standard Device Information Service (DIS, 0x180A).

The device exposes DIS alongside the AGo data service. It
carries read-only UTF-8 identity strings: Model Number, Serial Number,
Firmware Revision, and Manufacturer Name. The firmware version lives here
(0x2A26) and is intentionally not duplicated in the Status characteristic.

All DIS characteristics require an encrypted link; the bonded BLE
connection satisfies that.
"""

from __future__ import annotations

from bleak import BleakClient

import ago_protocol as proto


async def _read_text(client: BleakClient, char_uuid: str) -> str:
    data = await client.read_gatt_char(char_uuid)
    return bytes(data).decode("utf-8")


class TestDeviceInformation:
    """Verify the DIS service exposes valid device identity strings."""

    def test_dis_service_present(self, ago_client: BleakClient):
        """The standard DIS service (0x180A) must be present on the data connection."""
        uuids = {s.uuid.lower() for s in ago_client.services}
        assert proto.DIS_SERVICE_UUID in uuids, (
            f"DIS service {proto.DIS_SERVICE_UUID} not found. Available: {uuids}"
        )

    async def test_firmware_revision(self, ago_client: BleakClient):
        """Firmware Revision (0x2A26) must be a non-empty string.

        This is the canonical firmware-version source — it replaced the
        former Status "fw" key.
        """
        fw = await _read_text(ago_client, proto.DIS_FIRMWARE_REVISION_UUID)
        assert len(fw) > 0, "Firmware Revision is empty"

    async def test_model_number(self, ago_client: BleakClient):
        """Model Number (0x2A24) must be a non-empty string."""
        model = await _read_text(ago_client, proto.DIS_MODEL_NUMBER_UUID)
        assert len(model) > 0, "Model Number is empty"

    async def test_serial_number(self, ago_client: BleakClient):
        """Serial Number (0x2A25) must be a 12-char lowercase hex string."""
        serial = await _read_text(ago_client, proto.DIS_SERIAL_NUMBER_UUID)
        assert len(serial) == 12, f"Serial Number not 12 chars: {serial!r}"
        assert all(c in "0123456789abcdef" for c in serial), (
            f"Serial Number not lowercase hex: {serial!r}"
        )

    async def test_manufacturer_name(self, ago_client: BleakClient):
        """Manufacturer Name (0x2A29) must be 'AirGradient'."""
        manufacturer = await _read_text(ago_client, proto.DIS_MANUFACTURER_NAME_UUID)
        assert manufacturer == "AirGradient", (
            f"Unexpected Manufacturer Name: {manufacturer!r}"
        )
