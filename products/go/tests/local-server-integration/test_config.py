"""GET and PUT /api/v1/config integration tests."""

from __future__ import annotations

import httpx
import pytest

import ago_local_api as api


def _require_local_control(config: dict[str, object]) -> None:
    if config["configurationControl"] == "cloud":
        pytest.skip("local config writes are disabled by configurationControl=cloud")


def test_config_schema(config_payload: dict[str, object]) -> None:
    api.validate_config(config_payload)


def test_empty_config_is_accepted(
    ago_http_client: httpx.Client,
    config_payload: dict[str, object],
) -> None:
    _require_local_control(config_payload)
    api.assert_empty_response(ago_http_client.put(api.CONFIG_PATH, json={}), 202)


@pytest.mark.config_write
@pytest.mark.parametrize(
    "preserved_config_setting",
    tuple(api.CONFIG_ROUND_TRIP_VALUES),
    indirect=True,
)
def test_config_field_round_trip(
    ago_http_client: httpx.Client,
    preserved_config_setting: tuple[str, object, object],
    ago_convergence_timeout: float,
) -> None:
    field, _original, updated = preserved_config_setting
    baseline = api.get_config(ago_http_client)
    api.put_and_wait(
        ago_http_client,
        field,
        updated,
        ago_convergence_timeout,
    )
    if field in {"idleInterval", "trackingInterval"}:
        companion = "trackingInterval" if field == "idleInterval" else "idleInterval"
        assert api.get_config(ago_http_client)[companion] == baseline[companion]


@pytest.mark.config_write
@pytest.mark.parametrize("preserved_config_setting", ["logMeasurements"], indirect=True)
def test_logging_noop_preserves_config(
    ago_http_client: httpx.Client,
    preserved_config_setting: tuple[str, object, object],
    ago_convergence_timeout: float,
) -> None:
    baseline = api.get_config(ago_http_client)
    api.put_and_wait(
        ago_http_client, "logMeasurements", baseline["logMeasurements"],
        ago_convergence_timeout,
    )
    assert api.get_config(ago_http_client) == baseline
