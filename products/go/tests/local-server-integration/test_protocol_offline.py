"""Offline configuration-schema checks; no HTTP or mDNS fixtures."""

import pytest

import ago_local_api as api


@pytest.fixture
def example_config():
    return {
        "pmStandard": "ugm3", "temperatureUnit": "c", "altitudeUnit": "m",
        "pinnedChart": "pm25", "cloudConnection": False, "configurationControl": "local",
        "idleInterval": 300, "trackingInterval": 10, "logMeasurements": False,
        "gpsMode": "tracking", "backLedIndicator": "aqi", "backLedBrightness": 3,
        "touchLedIntensity": 0, "buzzerEnabled": True, "co2AbcDays": 7,
        "tvocLearningOffset": 12, "noxLearningOffset": 12,
        "corrections": {name: {"correctionAlgorithm": "none", "slr": None}
                        for name in ("pm25", "temperature", "humidity")},
    }


@pytest.mark.parametrize("enabled", [False, True])
def test_logging_boolean_schema(example_config, enabled):
    example_config["logMeasurements"] = enabled
    api.validate_config(example_config)
    assert api.alternate_config_value("logMeasurements", enabled) is not enabled


@pytest.mark.parametrize("invalid", [0, 1, 1.0, "true", None, [], {}])
def test_nonboolean_logging_schema_rejected(example_config, invalid):
    example_config["logMeasurements"] = invalid
    with pytest.raises(AssertionError):
        api.validate_config(example_config)


def test_logging_field_required(example_config):
    del example_config["logMeasurements"]
    with pytest.raises(AssertionError):
        api.validate_config(example_config)
