"""Pytest configuration and fixtures for AGo Local Server integration tests."""

from __future__ import annotations

import logging
import math
import threading
import time
from collections.abc import Callable
from typing import Generator
from urllib.parse import urlsplit

import httpx
import pytest
from zeroconf import IPVersion, ServiceBrowser, ServiceInfo, ServiceListener, Zeroconf

import ago_local_api as api

logger = logging.getLogger("ago_local_api_test")
logging.getLogger("zeroconf").setLevel(logging.WARNING)
logging.getLogger("httpx").setLevel(logging.WARNING)

DEFAULT_DISCOVERY_TIMEOUT = 10.0
DEFAULT_HTTP_TIMEOUT = 5.0
DEFAULT_CONVERGENCE_TIMEOUT = 20.0
RESTORE_STABILITY_SECONDS = 1.0


class _ServiceCollector(ServiceListener):
    def __init__(self) -> None:
        self.names: set[str] = set()
        self.dirty: set[str] = set()
        self.removed: set[str] = set()
        self.changed = threading.Event()
        self.lock = threading.Lock()

    def add_service(self, _zeroconf: Zeroconf, _service_type: str, name: str) -> None:
        with self.lock:
            self.names.add(name)
            self.dirty.add(name)
            self.removed.discard(name)
        self.changed.set()

    def update_service(
        self, _zeroconf: Zeroconf, _service_type: str, name: str
    ) -> None:
        self.add_service(_zeroconf, _service_type, name)

    def remove_service(
        self, _zeroconf: Zeroconf, _service_type: str, name: str
    ) -> None:
        with self.lock:
            self.names.discard(name)
            self.dirty.discard(name)
            self.removed.add(name)
        self.changed.set()

    def drain_changes(self) -> tuple[set[str], set[str], set[str]]:
        with self.lock:
            self.changed.clear()
            names = set(self.names)
            dirty = set(self.dirty)
            removed = set(self.removed)
            self.dirty.clear()
            self.removed.clear()
        return names, dirty, removed


def pytest_addoption(parser: pytest.Parser) -> None:
    group = parser.getgroup("ago", "AirGradient Go Local Server options")
    group.addoption(
        "--ago-board-variant",
        choices=("v1", "v2"),
        default="v1",
        help="Board under test; V2 rejects touch LED settings (default: v1).",
    )
    group.addoption(
        "--ago-url",
        default=None,
        help="Explicit Local Server base URL, for example http://192.168.1.20.",
    )
    group.addoption(
        "--ago-serial",
        default=None,
        help="Expected serial number and optional mDNS selection filter.",
    )
    group.addoption(
        "--ago-discovery-timeout",
        default=DEFAULT_DISCOVERY_TIMEOUT,
        type=float,
        help=f"mDNS discovery timeout in seconds (default: {DEFAULT_DISCOVERY_TIMEOUT}).",
    )
    group.addoption(
        "--ago-http-timeout",
        default=DEFAULT_HTTP_TIMEOUT,
        type=float,
        help=f"HTTP request timeout in seconds (default: {DEFAULT_HTTP_TIMEOUT}).",
    )
    group.addoption(
        "--ago-convergence-timeout",
        default=DEFAULT_CONVERGENCE_TIMEOUT,
        type=float,
        help=f"Config convergence timeout in seconds (default: {DEFAULT_CONVERGENCE_TIMEOUT}).",
    )
    group.addoption(
        "--ago-allow-config-write",
        action="store_true",
        help="Enable persisted config round-trip and restoration tests.",
    )
    group.addoption(
        "--ago-allow-calibration",
        action="store_true",
        help="Enable the physical CO2 calibration action test.",
    )
    group.addoption(
        "--ago-ota-active",
        action="store_true",
        help="Confirm that a committed foreground OTA is active for OTA tests.",
    )


def pytest_configure(config: pytest.Config) -> None:
    """Reject parallel execution because config restoration is stateful."""
    workers = config.getoption("numprocesses", default=None)
    if workers not in (None, 0, "0"):
        raise pytest.UsageError(
            "Local Server integration tests require serial execution; remove pytest-xdist -n"
        )


def _decode_properties(info: ServiceInfo) -> dict[str, str]:
    properties: dict[str, str] = {}
    for raw_key, raw_value in info.properties.items():
        key = raw_key.decode("utf-8", errors="replace")
        value = "" if raw_value is None else raw_value.decode("utf-8", errors="replace")
        properties[key] = value
    return properties


def _service_from_info(info: ServiceInfo) -> api.DiscoveredService | None:
    addresses = info.parsed_addresses(IPVersion.V4Only)
    if not addresses:
        return None
    address = addresses[0]
    return api.DiscoveredService(
        base_url=f"http://{address}:{info.port}",
        via_mdns=True,
        name=info.name,
        hostname=info.server.rstrip("."),
        address=address,
        port=info.port,
        properties=_decode_properties(info),
    )


def _discover_service(
    timeout: float, expected_serial: str | None
) -> api.DiscoveredService:
    zeroconf = Zeroconf(ip_version=IPVersion.V4Only)
    collector = _ServiceCollector()
    browser = ServiceBrowser(zeroconf, api.SERVICE_TYPE, collector)
    resolved: dict[str, api.DiscoveredService] = {}
    deadline = time.monotonic() + timeout

    logger.info("Discovering %s for %.1fs", api.SERVICE_TYPE, timeout)
    try:
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            collector.changed.wait(timeout=min(remaining, 0.25))
            names, dirty, removed = collector.drain_changes()

            for name in removed:
                resolved.pop(name, None)
            for name in names:
                if name in resolved and name not in dirty:
                    continue
                info = zeroconf.get_service_info(api.SERVICE_TYPE, name, timeout=1000)
                if info is None:
                    resolved.pop(name, None)
                    continue
                service = _service_from_info(info)
                if service is not None:
                    resolved[name] = service

            candidates = [
                service
                for service in resolved.values()
                if service.properties is not None
                and service.properties.get("model") == api.MODEL
                and service.properties.get("api") == "1"
            ]
            if expected_serial is not None:
                matches = [
                    service
                    for service in candidates
                    if service.properties is not None
                    and service.properties.get("serialno") == expected_serial
                ]
                if len(matches) == 1:
                    return matches[0]
        candidates = [
            service
            for service in resolved.values()
            if service.properties is not None
            and service.properties.get("model") == api.MODEL
            and service.properties.get("api") == "1"
        ]
    finally:
        browser.cancel()
        zeroconf.close()

    if expected_serial is not None:
        raise pytest.UsageError(
            f"No {api.MODEL} Local Server with serial {expected_serial!r} found in {timeout}s"
        )
    if not candidates:
        raise pytest.UsageError(
            f"No {api.MODEL} Local Server found in {timeout}s; pass --ago-url explicitly"
        )
    if len(candidates) > 1:
        serials = sorted(
            service.properties.get("serialno", "?")
            for service in candidates
            if service.properties is not None
        )
        raise pytest.UsageError(
            f"Multiple Local Servers found ({', '.join(serials)}); pass --ago-serial or --ago-url"
        )
    return candidates[0]


def _normalize_url(value: str) -> str:
    url = value if "://" in value else f"http://{value}"
    parsed = urlsplit(url)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise pytest.UsageError(f"Invalid --ago-url: {value!r}")
    if parsed.path not in {"", "/"} or parsed.query or parsed.fragment:
        raise pytest.UsageError(
            "--ago-url must be a base URL without a path, query, or fragment"
        )
    return url.rstrip("/")


def _timeout_option(
    request: pytest.FixtureRequest,
    name: str,
    minimum: float = 0,
) -> float:
    value = float(request.config.getoption(name))
    if not math.isfinite(value) or value <= minimum:
        raise pytest.UsageError(f"{name} must be finite and greater than {minimum}")
    return value


@pytest.fixture(scope="session")
def ago_expected_serial(request: pytest.FixtureRequest) -> str | None:
    value = request.config.getoption("--ago-serial")
    return None if value is None else str(value)


@pytest.fixture(scope="session")
def ago_service(
    request: pytest.FixtureRequest,
    ago_expected_serial: str | None,
) -> api.DiscoveredService:
    explicit_url = request.config.getoption("--ago-url")
    if explicit_url is not None:
        return api.DiscoveredService(
            base_url=_normalize_url(str(explicit_url)), via_mdns=False
        )
    timeout = _timeout_option(request, "--ago-discovery-timeout")
    return _discover_service(timeout, ago_expected_serial)


def _log_request(request: httpx.Request) -> None:
    body = request.content.decode("utf-8", errors="replace") if request.content else ""
    logger.debug("%s %s %s", request.method, request.url, body)


def _log_response(response: httpx.Response) -> None:
    response.read()
    body = response.text if response.content else "<empty>"
    logger.debug("%s %s", response.status_code, body)


@pytest.fixture(scope="session")
def ago_http_client(
    request: pytest.FixtureRequest,
    ago_service: api.DiscoveredService,
) -> Generator[httpx.Client, None, None]:
    timeout = _timeout_option(request, "--ago-http-timeout")
    logger.info("Using Local Server at %s", ago_service.base_url)
    with httpx.Client(
        base_url=ago_service.base_url,
        timeout=timeout,
        trust_env=False,
        headers={"Accept": "application/json"},
        event_hooks={"request": [_log_request], "response": [_log_response]},
    ) as client:
        yield client


@pytest.fixture(scope="session")
def measures_payload(ago_http_client: httpx.Client) -> dict[str, object]:
    payload = api.assert_json_response(ago_http_client.get(api.MEASURES_PATH))
    api.validate_measures(payload)
    return payload


@pytest.fixture
def config_payload(ago_http_client: httpx.Client) -> dict[str, object]:
    return api.get_config(ago_http_client)


@pytest.fixture(scope="session")
def ago_convergence_timeout(request: pytest.FixtureRequest) -> float:
    return _timeout_option(
        request,
        "--ago-convergence-timeout",
        minimum=RESTORE_STABILITY_SECONDS,
    )


@pytest.fixture
def ago_board_variant(request: pytest.FixtureRequest) -> str:
    return request.config.getoption("--ago-board-variant")


@pytest.fixture
def preserved_config_setting(
    request: pytest.FixtureRequest,
    ago_http_client: httpx.Client,
    ago_convergence_timeout: float,
    ago_board_variant: str,
) -> Generator[tuple[str, object, object], None, None]:
    if not request.config.getoption("--ago-allow-config-write"):
        pytest.skip("requires --ago-allow-config-write")

    parameter = request.param
    requested_value: object | None = None
    if isinstance(parameter, tuple) and len(parameter) == 2:
        field, requested_value = parameter
    else:
        field = parameter
    if not isinstance(field, str) or field not in api.CONFIG_ROUND_TRIP_VALUES:
        raise ValueError(f"unsupported config restoration field: {field!r}")
    if field == "touchLedIntensity" and ago_board_variant == "v2":
        pytest.skip("V2 touch LED rejection is covered separately; no setting to restore")

    baseline = api.get_config(ago_http_client)
    if baseline["configurationControl"] != "local":
        pytest.skip("config mutation tests require configurationControl=local")

    original = baseline[field]
    owned_value = (
        api.alternate_config_value(field, original)
        if requested_value is None
        else requested_value
    )

    try:
        yield field, original, owned_value
    finally:
        # Always enqueue the baseline after the test request. If a regression
        # accepted an invalid write asynchronously but GET has not reflected it
        # yet, FIFO admission still leaves the baseline as the final update.
        api.put_and_wait(
            ago_http_client,
            field,
            original,
            ago_convergence_timeout,
            stable_duration=RESTORE_STABILITY_SECONDS,
        )


@pytest.fixture
def require_calibration_opt_in(request: pytest.FixtureRequest) -> None:
    if not request.config.getoption("--ago-allow-calibration"):
        pytest.skip("requires --ago-allow-calibration")


@pytest.fixture
def require_ota_active(request: pytest.FixtureRequest) -> None:
    if not request.config.getoption("--ago-ota-active"):
        pytest.skip("requires --ago-ota-active")


@pytest.fixture
def discover_local_server(
    request: pytest.FixtureRequest,
) -> Callable[[str | None], api.DiscoveredService]:
    timeout = _timeout_option(request, "--ago-discovery-timeout")

    def discover(expected_serial: str | None = None) -> api.DiscoveredService:
        return _discover_service(timeout, expected_serial)

    return discover
