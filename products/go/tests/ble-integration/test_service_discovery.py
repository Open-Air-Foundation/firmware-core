"""Tests for BLE GATT service and characteristic discovery."""

from __future__ import annotations

import pytest
from bleak import BleakClient

import ago_protocol as proto


class TestServiceDiscovery:
    """Verify the AGo GATT profile is correctly advertised."""

    def _find_service(self, client: BleakClient):
        for service in client.services:
            if service.uuid == proto.SERVICE_UUID:
                return service
        return None

    def _find_char(self, client: BleakClient, char_uuid: str):
        for service in client.services:
            for char in service.characteristics:
                if char.uuid == char_uuid:
                    return char
        return None

    def test_service_exists(self, ago_client: BleakClient):
        """The AGo primary service UUID must be present in the GATT table."""
        service = self._find_service(ago_client)
        assert service is not None, (
            f"Service {proto.SERVICE_UUID} not found. "
            f"Available: {[s.uuid for s in ago_client.services]}"
        )

    @pytest.mark.parametrize("char_uuid", [
        proto.CHAR_MEASURES_UUID,
        proto.CHAR_STATUS_UUID,
        proto.CHAR_CONFIG_UUID,
        proto.CHAR_HISTORY_UUID,
    ])
    def test_characteristic_exists(self, ago_client: BleakClient, char_uuid: str):
        """Each expected characteristic UUID must exist under the AGo service."""
        char = self._find_char(ago_client, char_uuid)
        assert char is not None, f"Characteristic {char_uuid} not found"

    @pytest.mark.parametrize("char_uuid,expected_props", [
        (proto.CHAR_MEASURES_UUID, proto.EXPECTED_PROPERTIES[proto.CHAR_MEASURES_UUID]),
        (proto.CHAR_STATUS_UUID, proto.EXPECTED_PROPERTIES[proto.CHAR_STATUS_UUID]),
        (proto.CHAR_CONFIG_UUID, proto.EXPECTED_PROPERTIES[proto.CHAR_CONFIG_UUID]),
        (proto.CHAR_HISTORY_UUID, proto.EXPECTED_PROPERTIES[proto.CHAR_HISTORY_UUID]),
    ])
    def test_characteristic_properties(
        self,
        ago_client: BleakClient,
        char_uuid: str,
        expected_props: set[str],
    ):
        """Each characteristic must expose at least the expected properties."""
        char = self._find_char(ago_client, char_uuid)
        assert char is not None, f"Characteristic {char_uuid} not found"

        actual_props = set(char.properties)
        missing = expected_props - actual_props
        assert not missing, (
            f"{char_uuid}: missing properties {missing}. "
            f"Has: {actual_props}"
        )

    @pytest.mark.parametrize("char_uuid,expected_props", [
        (proto.CHAR_WIFI_SCAN_UUID, {"write", "notify"}),
        (proto.CHAR_WIFI_CREDENTIALS_UUID, {"read", "write", "notify"}),
    ])
    def test_provisioning_service_is_available(
        self,
        ago_client: BleakClient,
        char_uuid: str,
        expected_props: set[str],
    ):
        """Provisioning shares the data connection, independent of Wi-Fi preference."""
        service = ago_client.services.get_service(proto.PROVISIONING_SERVICE_UUID)
        assert service is not None, "Provisioning service missing from the data connection"
        char = service.get_characteristic(char_uuid)
        assert char is not None, f"Provisioning characteristic {char_uuid} missing"
        assert expected_props <= set(char.properties)
