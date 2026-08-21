"""Local Server parsing, validation, and provider error tests."""

from __future__ import annotations

import httpx
import pytest

import ago_local_api as api

JSON_HEADERS = {"Content-Type": "application/json"}


@pytest.mark.parametrize("body", [b"", b"{", b"[]", b"{} trailing"])
def test_invalid_bodies(ago_http_client: httpx.Client, body: bytes) -> None:
    response = ago_http_client.put(api.CONFIG_PATH, content=body, headers=JSON_HEADERS)
    api.assert_error(response, 400, "invalid_body", "invalid request body")


def test_unknown_field(ago_http_client: httpx.Client) -> None:
    response = ago_http_client.put(api.CONFIG_PATH, json={"temperatureUnits": "c"})
    api.assert_error(
        response,
        400,
        "unknown_field",
        "unknown field",
        field="temperatureUnits",
    )


def test_invalid_enum(ago_http_client: httpx.Client) -> None:
    response = ago_http_client.put(api.CONFIG_PATH, json={"temperatureUnit": "kelvin"})
    api.assert_error(
        response,
        400,
        "invalid_value",
        "invalid value",
        field="temperatureUnit",
    )


@pytest.mark.config_write
@pytest.mark.parametrize(
    "preserved_config_setting",
    [
        pytest.param(("altitudeUnit", "yards"), id="altitudeUnit-enum"),
        pytest.param(("altitudeUnit", True), id="altitudeUnit-type"),
        pytest.param(("measurementInterval", 0), id="measurementInterval-range"),
        pytest.param(("measurementInterval", 1.5), id="measurementInterval-type"),
        pytest.param(("gpsMode", "sometimes"), id="gpsMode"),
        pytest.param(("frontLedBrightness", 4), id="frontLedBrightness"),
        pytest.param(("backLedBrightness", 4), id="backLedBrightness"),
        pytest.param(("touchLedIntensity", 3), id="touchLedIntensity"),
        pytest.param(("buzzerEnabled", 1), id="buzzerEnabled-type"),
        pytest.param(("co2AbcDays", -1), id="co2AbcDays"),
        pytest.param(("tvocLearningOffset", 0), id="tvocLearningOffset"),
        pytest.param(("noxLearningOffset", 0), id="noxLearningOffset"),
    ],
    indirect=True,
)
def test_invalid_extended_config_value(
    ago_http_client: httpx.Client,
    preserved_config_setting: tuple[str, object, object],
) -> None:
    field, _original, value = preserved_config_setting

    response = ago_http_client.put(api.CONFIG_PATH, json={field: value})
    api.assert_error(
        response,
        400,
        "invalid_value",
        "invalid value",
        field=field,
    )


def test_unknown_nested_correction_field(ago_http_client: httpx.Client) -> None:
    response = ago_http_client.put(
        api.CONFIG_PATH,
        json={"corrections": {"temperature": {"bogus": 1}}},
    )
    api.assert_error(
        response,
        400,
        "unknown_field",
        "unknown field",
        field="corrections.temperature.bogus",
    )


def test_known_unsupported_field(
    ago_http_client: httpx.Client,
    config_payload: dict[str, object],
) -> None:
    if config_payload["configurationControl"] == "cloud":
        pytest.skip("configuration source policy takes precedence over product support")
    response = ago_http_client.put(api.CONFIG_PATH, json={"country": "US"})
    api.assert_error(response, 404, "not_found", "not found", field="country")
