"""Local Server parsing, validation, and provider error tests."""

from __future__ import annotations

import time

import httpx
import pytest

import ago_local_api as api

JSON_HEADERS = {"Content-Type": "application/json"}
CONFIG_REJECTION_POLL_INTERVAL = 0.25


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
        pytest.param(("pinnedChart", "PM25"), id="pinnedChart-enum"),
        pytest.param(("pinnedChart", 1), id="pinnedChart-type"),
        pytest.param(("measurementInterval", 0), id="measurementInterval-range"),
        pytest.param(("measurementInterval", 1.5), id="measurementInterval-type"),
        pytest.param(("gpsMode", "sometimes"), id="gpsMode"),
        pytest.param(("backLedIndicator", "pm25-aqi"), id="backLedIndicator-enum"),
        pytest.param(("backLedIndicator", "AQI"), id="backLedIndicator-uppercase-aqi"),
        pytest.param(("backLedIndicator", "CO2"), id="backLedIndicator-uppercase-co2"),
        pytest.param(("backLedIndicator", 1), id="backLedIndicator-number"),
        pytest.param(("backLedIndicator", True), id="backLedIndicator-bool"),
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


@pytest.mark.config_write
@pytest.mark.parametrize("preserved_config_setting", ["measurementInterval"], indirect=True)
@pytest.mark.parametrize("include_valid_setting", [False, True], ids=["epa-only", "mixed"])
def test_epa_correction_is_rejected_without_changing_config(
    ago_http_client: httpx.Client,
    preserved_config_setting: tuple[str, object, object],
    ago_convergence_timeout: float,
    include_valid_setting: bool,
) -> None:
    # The preservation fixture requires opt-in and local control, and restores
    # the interval even if a regression partially accepts the mixed request.
    field, _original, updated = preserved_config_setting
    baseline = api.get_config(ago_http_client)
    request: dict[str, object] = {
        "corrections": {"pm25": {"correctionAlgorithm": "epa_2021", "slr": None}}
    }
    if include_valid_setting:
        request[field] = updated

    restore_corrections = True
    try:
        response = ago_http_client.put(api.CONFIG_PATH, json=request)
        restore_corrections = response.status_code != 400
        api.assert_error(
            response,
            400,
            "invalid_value",
            "invalid value",
            field="corrections.pm25",
        )

        # A single immediate GET can miss an incorrectly queued update. Observe
        # the full convergence window before claiming that nothing changed.
        deadline = time.monotonic() + ago_convergence_timeout
        while True:
            current = api.assert_json_response(ago_http_client.get(api.CONFIG_PATH))
            restore_corrections = current["corrections"] != baseline["corrections"]
            assert current == baseline, "Rejected EPA request changed the configuration"
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(CONFIG_REJECTION_POLL_INTERVAL, remaining))
    finally:
        if restore_corrections:
            # Queue the original values after any mistakenly accepted request.
            # Avoid rewriting unchanged PM calibration on the passing path.
            api.put_and_wait(
                ago_http_client,
                "corrections",
                baseline["corrections"],
                ago_convergence_timeout,
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
