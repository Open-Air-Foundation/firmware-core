"""Pytest configuration and shared fixtures for AGo BLE integration tests.

Provides session-scoped BLE connection and per-test notification subscriptions.
"""

from __future__ import annotations

import asyncio
import logging
from collections.abc import Callable
from typing import Any, AsyncGenerator

import cbor2
import pytest
import pytest_asyncio
from bleak import BleakClient, BleakScanner
from bleak.backends.device import BLEDevice

import ago_protocol as proto

logger = logging.getLogger("ago_ble_test")

# Suppress noisy bleak internals so --log-cli-level=DEBUG only shows ago_ble_test
logging.getLogger("bleak").setLevel(logging.WARNING)

# Default timeouts (seconds)
DEFAULT_SCAN_TIMEOUT = 10.0
DEFAULT_NOTIFICATION_TIMEOUT = 30.0


# ---------------------------------------------------------------------------
# Payload logging helpers
# ---------------------------------------------------------------------------

# Map from lowercase UUID string to a short human-readable name
_CHAR_NAMES: dict[str, str] = {
    proto.CHAR_MEASURES_UUID: "Measures",
    proto.CHAR_STATUS_UUID:   "Status",
    proto.CHAR_CONFIG_UUID:   "Config",
    proto.CHAR_HISTORY_UUID:  "History",
}


def _char_name(char_specifier: Any) -> str:
    """Return a short label for a characteristic UUID."""
    return _CHAR_NAMES.get(str(char_specifier).lower(), str(char_specifier)[:8])


def _decode_payload(data: bytes) -> str:
    """Human-readable representation of a raw BLE payload.

    Tries CBOR decoding first; falls back to a hex dump for binary data.
    """
    try:
        return repr(cbor2.loads(data))
    except Exception:
        return f"<binary {len(data)}B> {data.hex()}"


def _decode_history_notification(data: bytes) -> str:
    """Decode a History characteristic notification for display.

    History notifications are prefixed with a 1-byte tag (0x00 = CBOR,
    0x01 = binary route points).  Plain CBOR decoding of the full buffer
    sees that tag byte as a CBOR integer and returns 0 or 1, which hides the
    actual content.  This decoder strips the tag first.
    """
    try:
        tag, payload = proto.parse_history_notification(data)
        if tag == proto.HISTORY_TAG_CBOR:
            return repr(payload)
        # Binary chunk: payload is (first_point_index, [route_point_dicts])
        point_index, points = payload
        return f"<binary pts={len(points)} idx={point_index}>"
    except Exception:
        return _decode_payload(data)


# ---------------------------------------------------------------------------
# Logging BleakClient proxy
# ---------------------------------------------------------------------------

class _LoggingBleakClient:
    """Thin proxy around BleakClient that logs every read and write at DEBUG level.

    All other attribute accesses are forwarded to the underlying BleakClient,
    so callers can use this object as a drop-in replacement.
    """

    def __init__(self, inner: BleakClient) -> None:
        self._inner = inner

    def __getattr__(self, name: str) -> Any:
        return getattr(self._inner, name)

    async def read_gatt_char(self, char_specifier: Any, **kwargs: Any) -> bytearray:
        data = await self._inner.read_gatt_char(char_specifier, **kwargs)
        name = _char_name(char_specifier)
        logger.debug("READ  %-8s  %s", name, _decode_payload(bytes(data)))
        return data

    async def write_gatt_char(
        self,
        char_specifier: Any,
        data: bytes | bytearray,
        **kwargs: Any,
    ) -> None:
        name = _char_name(char_specifier)
        logger.debug("WRITE %-8s  %s", name, _decode_payload(bytes(data)))
        await self._inner.write_gatt_char(char_specifier, data, **kwargs)


# ---------------------------------------------------------------------------
# CLI options
# ---------------------------------------------------------------------------

def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("ago", "AirGradient Go BLE options")
    group.addoption(
        "--ago-address",
        default=None,
        help="BLE address (or name) of the AGo device. "
             "If omitted, auto-scans for a device whose name starts with "
             "'AirGradient Go '.",
    )
    group.addoption(
        "--ago-scan-timeout",
        default=DEFAULT_SCAN_TIMEOUT,
        type=float,
        help=f"BLE scan timeout in seconds (default: {DEFAULT_SCAN_TIMEOUT}).",
    )
    group.addoption(
        "--ago-notify-timeout",
        default=DEFAULT_NOTIFICATION_TIMEOUT,
        type=float,
        help=f"Max seconds to wait for a BLE notification (default: {DEFAULT_NOTIFICATION_TIMEOUT}).",
    )


# ---------------------------------------------------------------------------
# Auto-scan helper
# ---------------------------------------------------------------------------

_AGO_NAME_PREFIX = "AirGradient Go "


async def _scan_for_ago(timeout: float) -> BLEDevice:
    """Scan for a BLE device whose name matches the AGo prefix.

    Returns the first match. Raises RuntimeError if none found.
    """
    logger.info("Scanning for AGo device (timeout=%.1fs) ...", timeout)

    found: BLEDevice | None = None
    event = asyncio.Event()

    def _on_detect(device: BLEDevice, adv: Any) -> None:
        nonlocal found
        name = adv.local_name or device.name or ""
        if found is None and name.startswith(_AGO_NAME_PREFIX):
            logger.info("Found AGo device: %s [%s]", name, device.address)
            found = device
            event.set()

    scanner = BleakScanner(detection_callback=_on_detect)
    await scanner.start()
    try:
        await asyncio.wait_for(event.wait(), timeout=timeout)
    except asyncio.TimeoutError:
        pass
    finally:
        await scanner.stop()

    if found is None:
        raise RuntimeError(
            f"No AGo device found after {timeout}s scan. "
            "Make sure the device is advertising in Portable mode, "
            "or pass --ago-address explicitly."
        )
    return found


# ---------------------------------------------------------------------------
# Session-scoped fixtures
# ---------------------------------------------------------------------------

@pytest.fixture(scope="session")
def ago_scan_timeout(request: pytest.FixtureRequest) -> float:
    return float(request.config.getoption("--ago-scan-timeout"))


@pytest.fixture(scope="session")
def ago_notify_timeout(request: pytest.FixtureRequest) -> float:
    return float(request.config.getoption("--ago-notify-timeout"))


@pytest_asyncio.fixture(scope="session")
async def ago_device(request: pytest.FixtureRequest, ago_scan_timeout: float) -> BLEDevice:
    """Resolve the BLE device to connect to (scan or explicit address)."""
    address = request.config.getoption("--ago-address")
    if address is not None:
        # Use the address directly; bleak will resolve it.
        logger.info("Using explicit device address: %s", address)
        return BLEDevice(address=address, name=address, details={})
    return await _scan_for_ago(ago_scan_timeout)


@pytest_asyncio.fixture(scope="session")
async def ago_client(ago_device: BLEDevice) -> AsyncGenerator[_LoggingBleakClient, None]:
    """Session-scoped BLE connection to the AGo device.

    Connects once, yields a logging-enabled client for all tests, disconnects
    at teardown.  Reads and writes are logged at DEBUG level; enable them with
    ``--log-cli-level=DEBUG``.
    """
    logger.info("Connecting to %s ...", ago_device.address)
    client = BleakClient(ago_device)
    await client.connect(timeout=15.0)
    logger.info("Connected (MTU=%d)", client.mtu_size)

    yield _LoggingBleakClient(client)

    if client.is_connected:
        logger.info("Disconnecting ...")
        await client.disconnect()
    logger.info("Disconnected")


# ---------------------------------------------------------------------------
# Notification collector
# ---------------------------------------------------------------------------

class NotificationCollector:
    """Async-safe container that accumulates BLE notifications."""

    def __init__(
        self,
        name: str = "",
        decoder: Callable[[bytes], str] | None = None,
    ) -> None:
        self._name = name
        self._decoder: Callable[[bytes], str] = decoder or _decode_payload
        self._queue: asyncio.Queue[bytes] = asyncio.Queue()
        self._all: list[bytes] = []

    @property
    def all_received(self) -> list[bytes]:
        """All notifications received so far (oldest first)."""
        return list(self._all)

    @property
    def count(self) -> int:
        return len(self._all)

    def callback(self, _sender: Any, data: bytearray) -> None:
        """bleak notification callback — safe to call from any thread."""
        raw = bytes(data)
        self._all.append(raw)
        self._queue.put_nowait(raw)
        logger.debug(
            "NOTIFY %-8s  %s",
            self._name or "?",
            self._decoder(raw),
        )

    async def wait_for(self, timeout: float) -> bytes:
        """Wait for the next notification, raising TimeoutError if none arrives."""
        return await asyncio.wait_for(self._queue.get(), timeout=timeout)

    async def wait_for_n(self, n: int, timeout: float) -> list[bytes]:
        """Wait until *n* total notifications have been collected."""
        deadline = asyncio.get_event_loop().time() + timeout
        while self.count < n:
            remaining = deadline - asyncio.get_event_loop().time()
            if remaining <= 0:
                raise asyncio.TimeoutError(
                    f"Timed out waiting for {n} notifications "
                    f"(got {self.count} in {timeout}s)"
                )
            try:
                await asyncio.wait_for(self._queue.get(), timeout=remaining)
            except asyncio.TimeoutError:
                raise asyncio.TimeoutError(
                    f"Timed out waiting for {n} notifications "
                    f"(got {self.count} in {timeout}s)"
                ) from None

        return list(self._all[:n])

    def drain(self) -> list[bytes]:
        """Return and clear all collected notifications."""
        result = list(self._all)
        self._all.clear()
        # Drain the queue too
        while not self._queue.empty():
            try:
                self._queue.get_nowait()
            except asyncio.QueueEmpty:
                break
        return result


# ---------------------------------------------------------------------------
# Per-test notification subscription fixtures
# ---------------------------------------------------------------------------

@pytest_asyncio.fixture
async def measures_notifications(
    ago_client: BleakClient,
) -> AsyncGenerator[NotificationCollector, None]:
    """Subscribe to Measures notifications for the duration of a test."""
    collector = NotificationCollector(name="Measures")
    await ago_client.start_notify(proto.CHAR_MEASURES_UUID, collector.callback)
    yield collector
    await ago_client.stop_notify(proto.CHAR_MEASURES_UUID)


@pytest_asyncio.fixture
async def status_notifications(
    ago_client: BleakClient,
) -> AsyncGenerator[NotificationCollector, None]:
    """Subscribe to Status notifications for the duration of a test.

    The device pushes Status on urgent tracking and charging transitions.
    Steady-state polls update the characteristic value silently — clients
    see those via Read.
    """
    collector = NotificationCollector(name="Status")
    await ago_client.start_notify(proto.CHAR_STATUS_UUID, collector.callback)
    yield collector
    await ago_client.stop_notify(proto.CHAR_STATUS_UUID)


@pytest_asyncio.fixture
async def config_notifications(
    ago_client: BleakClient,
) -> AsyncGenerator[NotificationCollector, None]:
    """Subscribe to Config notifications for the duration of a test."""
    collector = NotificationCollector(name="Config")
    await ago_client.start_notify(proto.CHAR_CONFIG_UUID, collector.callback)
    yield collector
    await ago_client.stop_notify(proto.CHAR_CONFIG_UUID)


@pytest_asyncio.fixture
async def history_notifications(
    ago_client: BleakClient,
) -> AsyncGenerator[NotificationCollector, None]:
    """Subscribe to History notifications for the duration of a test."""
    collector = NotificationCollector(name="History", decoder=_decode_history_notification)
    await ago_client.start_notify(proto.CHAR_HISTORY_UUID, collector.callback)
    yield collector
    await ago_client.stop_notify(proto.CHAR_HISTORY_UUID)
