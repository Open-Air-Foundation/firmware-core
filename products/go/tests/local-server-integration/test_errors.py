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


@pytest.mark.parametrize("field,value", [
    ("temperatureUnits", "c"),
    ("duration_min", 30),
    ("tracking_duration", 30),
])
def test_unknown_field(ago_http_client: httpx.Client, field: str, value: object) -> None:
    response = ago_http_client.put(api.CONFIG_PATH, json={field: value})
    api.assert_error(
        response,
        400,
        "unknown_field",
        "unknown field",
        field=field,
    )


@pytest.mark.config_write
@pytest.mark.parametrize("preserved_config_setting", ["idleInterval"], indirect=True)
def test_ble_duration_field_rejects_entire_config_write(
    ago_http_client: httpx.Client,
    preserved_config_setting: tuple[str, object, object],
    ago_convergence_timeout: float,
) -> None:
    field, _original, updated = preserved_config_setting
    baseline = api.get_config(ago_http_client)
    response = ago_http_client.put(
        api.CONFIG_PATH, json={field: updated, "duration_min": 30},
    )
    api.assert_error(response, 400, "unknown_field", "unknown field", field="duration_min")
    deadline = time.monotonic() + ago_convergence_timeout
    while True:
        assert api.get_config(ago_http_client) == baseline
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(CONFIG_REJECTION_POLL_INTERVAL, remaining))


@pytest.mark.config_write
@pytest.mark.parametrize("preserved_config_setting", ["idleInterval"], indirect=True)
@pytest.mark.parametrize("mixed", [False, True])
def test_legacy_interval_is_unsupported_without_changing_config(
    ago_http_client: httpx.Client,
    preserved_config_setting: tuple[str, object, object],
    ago_convergence_timeout: float,
    mixed: bool,
) -> None:
    field, _original, updated = preserved_config_setting
    baseline = api.get_config(ago_http_client)
    request = {"measurementInterval": 17}
    if mixed:
        request[field] = updated
    response = ago_http_client.put(api.CONFIG_PATH, json=request)
    api.assert_error(response, 404, "not_found", "not found", field="measurementInterval")

    deadline = time.monotonic() + ago_convergence_timeout
    while True:
        assert api.get_config(ago_http_client) == baseline
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(CONFIG_REJECTION_POLL_INTERVAL, remaining))


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
        pytest.param(("idleInterval", 0), id="idleInterval-min"),
        pytest.param(("idleInterval", 3601), id="idleInterval-max"),
        pytest.param(("idleInterval", 1.5), id="idleInterval-fraction"),
        pytest.param(("idleInterval", True), id="idleInterval-bool"),
        pytest.param(("trackingInterval", 0), id="trackingInterval-min"),
        pytest.param(("trackingInterval", 3601), id="trackingInterval-max"),
        pytest.param(("trackingInterval", 1.5), id="trackingInterval-fraction"),
        pytest.param(("trackingInterval", True), id="trackingInterval-bool"),
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
@pytest.mark.parametrize("preserved_config_setting", ["buzzerEnabled"], indirect=True)
@pytest.mark.parametrize("intensity", [0, 1, 2, 3])
@pytest.mark.parametrize("mixed", [False, True])
def test_v2_touch_led_is_unsupported_without_changing_config(
    ago_http_client: httpx.Client,
    preserved_config_setting: tuple[str, object, object],
    ago_convergence_timeout: float,
    ago_board_variant: str,
    intensity: int,
    mixed: bool,
) -> None:
    if ago_board_variant != "v2":
        pytest.skip("requires --ago-board-variant=v2")
    field, _original, updated = preserved_config_setting
    baseline = api.get_config(ago_http_client)
    assert baseline["touchLedIntensity"] == 0
    request: dict[str, object] = {"touchLedIntensity": intensity}
    if mixed:
        request[field] = updated
    response = ago_http_client.put(api.CONFIG_PATH, json=request)
    api.assert_error(response, 404, "not_found", "not found", field="touchLedIntensity")

    deadline = time.monotonic() + ago_convergence_timeout
    while True:
        assert api.get_config(ago_http_client) == baseline
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        time.sleep(min(CONFIG_REJECTION_POLL_INTERVAL, remaining))


@pytest.mark.config_write
@pytest.mark.parametrize("preserved_config_setting", ["idleInterval"], indirect=True)
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


@pytest.mark.config_write
@pytest.mark.parametrize("preserved_config_setting", ["logMeasurements"], indirect=True)
@pytest.mark.parametrize("value", [0, 1, 1.0, "true", None, [], {}])
@pytest.mark.parametrize("mixed", [False, True])
def test_invalid_logging_value_rejects_entire_update(
    ago_http_client: httpx.Client,
    preserved_config_setting: tuple[str, object, object],
    ago_convergence_timeout: float,
    value: object,
    mixed: bool,
) -> None:
    baseline = api.get_config(ago_http_client)
    request = {"logMeasurements": value}
    if mixed:
        request["idleInterval"] = api.alternate_config_value("idleInterval", baseline["idleInterval"])
    try:
        response = ago_http_client.put(api.CONFIG_PATH, json=request)
        api.assert_error(response, 400, "invalid_value", "invalid value", field="logMeasurements")
        deadline = time.monotonic() + ago_convergence_timeout
        while True:
            assert api.get_config(ago_http_client) == baseline
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            time.sleep(min(CONFIG_REJECTION_POLL_INTERVAL, remaining))
    finally:
        # The fixture restores logMeasurements; restore the companion too if
        # a regression partially accepted the mixed request.
        if mixed:
            api.put_and_wait(
                ago_http_client, "idleInterval", baseline["idleInterval"],
                ago_convergence_timeout,
            )
