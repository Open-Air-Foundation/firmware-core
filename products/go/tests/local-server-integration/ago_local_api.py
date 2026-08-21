"""AirGradient Go Local Server protocol constants and assertions."""

from __future__ import annotations

import math
import re
import time
from dataclasses import dataclass
from typing import Any

import httpx

MEASURES_PATH = "/api/v1/measures"
CONFIG_PATH = "/api/v1/config"
CALIBRATE_CO2_PATH = "/api/v1/actions/calibrate-co2"
TEST_LEDS_PATH = "/api/v1/actions/test-leds"
TEST_GPS_PATH = "/api/v1/actions/test-gps"

MODEL = "P-1PSG"
SERVICE_TYPE = "_airgradient._tcp.local."

MEASURES_REQUIRED_KEYS = {"serialNumber", "model", "firmware", "boot"}
MEASURES_OPTIONAL_KEYS = {
    "wifiRssi",
    "co2",
    "pm01",
    "pm25",
    "pm10",
    "pm003Count",
    "pm005Count",
    "pm01Count",
    "pm02Count",
    "pm50Count",
    "pm10Count",
    "temperature",
    "humidity",
    "tvocIndex",
    "tvocRaw",
    "noxIndex",
    "noxRaw",
    "battPercent",
    "battVolt",
    "chargeVolt",
}
MEASURES_KEYS = MEASURES_REQUIRED_KEYS | MEASURES_OPTIONAL_KEYS

CONFIG_KEYS = {
    "pmStandard",
    "temperatureUnit",
    "altitudeUnit",
    "cloudConnection",
    "configurationControl",
    "measurementInterval",
    "gpsMode",
    "frontLedBrightness",
    "backLedBrightness",
    "touchLedIntensity",
    "buzzerEnabled",
    "co2AbcDays",
    "tvocLearningOffset",
    "noxLearningOffset",
    "corrections",
}
CORRECTION_KEYS = {"pm25", "temperature", "humidity"}

CONFIG_ROUND_TRIP_VALUES: dict[str, tuple[object, object]] = {
    "temperatureUnit": ("c", "f"),
    "altitudeUnit": ("m", "ft"),
    "measurementInterval": (1, 2),
    "gpsMode": ("off", "tracking"),
    "frontLedBrightness": (0, 1),
    "backLedBrightness": (0, 1),
    "touchLedIntensity": (0, 1),
    "buzzerEnabled": (False, True),
    "co2AbcDays": (0, 1),
    "tvocLearningOffset": (1, 2),
    "noxLearningOffset": (1, 2),
}

_SERIAL_PATTERN = re.compile(r"^[0-9a-f]{12}$")
_MISSING = object()


@dataclass(frozen=True)
class DiscoveredService:
    """Resolved Local Server endpoint and optional mDNS metadata."""

    base_url: str
    via_mdns: bool
    name: str = ""
    hostname: str = ""
    address: str = ""
    port: int = 0
    properties: dict[str, str] | None = None


def assert_json_response(
    response: httpx.Response, status_code: int = 200
) -> dict[str, Any]:
    """Assert a JSON object response and return its decoded payload."""
    assert response.status_code == status_code, response.text
    content_type = response.headers.get("content-type", "")
    assert content_type.split(";", 1)[0].strip().lower() == "application/json"
    payload = response.json()
    assert isinstance(payload, dict)
    return payload


def assert_empty_response(response: httpx.Response, status_code: int) -> None:
    """Assert a status-only response with no body."""
    assert response.status_code == status_code, response.text
    assert response.content == b""


def assert_error(
    response: httpx.Response,
    status_code: int,
    code: str,
    message: str,
    field: str | object = _MISSING,
) -> None:
    """Assert the Local Server structured error envelope."""
    payload = assert_json_response(response, status_code)
    expected: dict[str, str] = {"code": code, "message": message}
    if field is not _MISSING:
        assert isinstance(field, str)
        expected["field"] = field
    assert payload == {"error": expected}


def _is_number(value: object) -> bool:
    return (
        isinstance(value, (int, float))
        and not isinstance(value, bool)
        and math.isfinite(value)
    )


def _assert_integer(
    value: object, minimum: int | None = None, maximum: int | None = None
) -> None:
    assert isinstance(value, int) and not isinstance(value, bool)
    if minimum is not None:
        assert value >= minimum
    if maximum is not None:
        assert value <= maximum


def _assert_number(value: object, minimum: float, maximum: float | None = None) -> None:
    assert _is_number(value)
    assert float(value) >= minimum
    if maximum is not None:
        assert float(value) <= maximum


def validate_measures(payload: dict[str, Any]) -> None:
    """Validate the complete Go measures payload contract."""
    assert MEASURES_REQUIRED_KEYS <= payload.keys()
    assert payload.keys() <= MEASURES_KEYS
    assert all(value is not None for value in payload.values())

    assert isinstance(payload["serialNumber"], str)
    assert _SERIAL_PATTERN.fullmatch(payload["serialNumber"])
    assert payload["model"] == MODEL
    assert isinstance(payload["firmware"], str) and payload["firmware"]
    _assert_integer(payload["boot"], 0, 0xFFFFFFFF)

    if "wifiRssi" in payload:
        _assert_integer(payload["wifiRssi"])
    if "co2" in payload:
        _assert_integer(payload["co2"], 0, 10000)
    for field in ("pm01", "pm25", "pm10"):
        if field in payload:
            _assert_number(payload[field], 0)
    for field in (
        "pm003Count",
        "pm005Count",
        "pm01Count",
        "pm02Count",
        "pm50Count",
        "pm10Count",
    ):
        if field in payload:
            _assert_integer(payload[field], 0)
    if "temperature" in payload:
        _assert_number(payload["temperature"], -40, 125)
    if "humidity" in payload:
        _assert_number(payload["humidity"], 0, 100)
    for field in ("tvocIndex", "tvocRaw", "noxIndex", "noxRaw"):
        if field in payload:
            _assert_integer(payload[field], 0)
    if "battPercent" in payload:
        _assert_integer(payload["battPercent"], 0, 100)
    for field in ("battVolt", "chargeVolt"):
        if field in payload:
            _assert_number(payload[field], 0)


def _validate_correction(entry: object, measure: str) -> None:
    assert isinstance(entry, dict)
    assert set(entry) == {"correctionAlgorithm", "slr"}
    algorithm = entry["correctionAlgorithm"]
    slr = entry["slr"]

    if measure == "pm25":
        assert algorithm in {"none", "epa_2021", "custom_via_pm25_raw"}
        custom = algorithm == "custom_via_pm25_raw"
    else:
        assert algorithm in {"none", "custom"}
        custom = algorithm == "custom"

    if not custom:
        assert slr is None
        return

    assert isinstance(slr, dict)
    expected = {"intercept", "scalingFactor"}
    assert set(slr) == expected
    assert _is_number(slr["intercept"])
    assert _is_number(slr["scalingFactor"])


def validate_config(payload: dict[str, Any]) -> None:
    """Validate the complete Go configuration payload contract."""
    assert set(payload) == CONFIG_KEYS
    assert payload["pmStandard"] in {"ugm3", "us-aqi"}
    assert payload["temperatureUnit"] in {"c", "f"}
    assert payload["altitudeUnit"] in {"m", "ft"}
    assert isinstance(payload["cloudConnection"], bool)
    assert payload["configurationControl"] in {"cloud", "local", "both"}
    _assert_integer(payload["measurementInterval"], 1, 3600)
    assert payload["gpsMode"] in {"off", "tracking", "always"}
    _assert_integer(payload["frontLedBrightness"], 0, 3)
    _assert_integer(payload["backLedBrightness"], 0, 3)
    _assert_integer(payload["touchLedIntensity"], 0, 2)
    assert isinstance(payload["buzzerEnabled"], bool)
    _assert_integer(payload["co2AbcDays"], 0, 200)
    assert payload["co2AbcDays"] == 0 or payload["co2AbcDays"] >= 1
    _assert_integer(payload["tvocLearningOffset"], 1, 1000)
    _assert_integer(payload["noxLearningOffset"], 1, 1000)

    corrections = payload["corrections"]
    assert isinstance(corrections, dict)
    assert set(corrections) == CORRECTION_KEYS
    for measure in CORRECTION_KEYS:
        _validate_correction(corrections[measure], measure)


def get_config(client: httpx.Client) -> dict[str, Any]:
    """Read and validate the current Go configuration."""
    payload = assert_json_response(client.get(CONFIG_PATH))
    validate_config(payload)
    return payload


def put_and_wait(
    client: httpx.Client,
    field: str,
    value: object,
    convergence_timeout: float,
    stable_duration: float = 0,
) -> None:
    """Submit one config field and wait for the asynchronous snapshot update."""
    response = client.put(CONFIG_PATH, json={field: value})
    assert_empty_response(response, 202)

    deadline = time.monotonic() + convergence_timeout
    stable_since: float | None = None
    while time.monotonic() < deadline:
        now = time.monotonic()
        payload = assert_json_response(client.get(CONFIG_PATH))
        if payload.get(field) == value:
            validate_config(payload)
            if stable_since is None:
                stable_since = now
            if now - stable_since >= stable_duration:
                return
        else:
            stable_since = None
        time.sleep(0.25)
    raise AssertionError(
        f"{field} did not converge to {value!r} within {convergence_timeout}s"
    )


def alternate_config_value(field: str, current: object) -> object:
    """Return a valid test value that differs from the current field value."""
    first, second = CONFIG_ROUND_TRIP_VALUES[field]
    return second if current == first else first
