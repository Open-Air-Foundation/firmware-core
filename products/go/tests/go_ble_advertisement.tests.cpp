/**
 * AirGradient Go — Offline BLE measurement payload tests
 */

#include "go_ble_advertisement.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>
#include <string>

using GoBleAdvertisement::encode;
using GoBleAdvertisement::Payload;

TEST_CASE("BLE advertisement: default measurements encode invalid sentinels",
          "[ble][advertisement]") {
  const Payload expected{0x01, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF,
                         0xFF, 0xFF, 0xFF, 0x00, 0x80, 0xFF};
  REQUIRE(encode(MeasuresAGo{}) == expected);
}

TEST_CASE("BLE advertisement: v1 has exact little-endian wire bytes", "[ble][advertisement]") {
  MeasuresAGo m{};
  m.temp_hum_a = {-12.34f, 56.6f};
  m.pm_a.pm_25 = 12.3f;
  m.co2.co2 = 1234;
  m.pressure = {1013.25f, -42.5f};
  m.power.battery_percentage = 87.5f;

  // -1234, 57, 123, 1234, 10133, -43, 88. Other PM and power fields
  // remain invalid, so this also checks field-specific validation.
  const Payload expected{0x01, 0x2E, 0xFB, 0x39, 0x7B, 0x00, 0xD2,
                         0x04, 0x95, 0x27, 0xD5, 0xFF, 0x58};
  REQUIRE(encode(m).size() == 13);
  REQUIRE(encode(m) == expected);
}

TEST_CASE("BLE advertisement: valid range endpoints are preserved", "[ble][advertisement]") {
  MeasuresAGo m{};
  Payload expected{};
  SECTION("minimum values including legitimate zeros") {
    m.temp_hum_a = {-40.0f, 0.0f};
    m.pm_a.pm_25 = 0.0f;
    m.co2.co2 = 0;
    m.pressure = {300.0f, -500.0f};
    m.power.battery_percentage = 0.0f;
    expected = {0x01, 0x60, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0xB8, 0x0B, 0x0C, 0xFE, 0x00};
  }
  SECTION("maximum sensor values and largest encodable PM value") {
    m.temp_hum_a = {125.0f, 100.0f};
    m.pm_a.pm_25 = 6553.4f;
    m.co2.co2 = 10000;
    m.pressure = {1100.0f, 10000.0f};
    m.power.battery_percentage = 100.0f;
    expected = {0x01, 0xD4, 0x30, 0x64, 0xFE, 0xFF, 0x10, 0x27, 0xF8, 0x2A, 0x10, 0x27, 0x64};
  }
  REQUIRE(encode(m) == expected);
}

TEST_CASE("BLE advertisement: rounding uses nearest with ties away from zero",
          "[ble][advertisement]") {
  MeasuresAGo m{};
  // Binary-exact halves exercise rounding without decimal representation noise.
  m.temp_hum_a = {-0.125f, 49.5f};
  m.pm_a.pm_25 = 1.25f;
  m.co2.co2 = 400;
  m.pressure = {1000.25f, 42.5f};
  m.power.battery_percentage = 49.49f;
  const Payload expected{0x01, 0xF3, 0xFF, 0x32, 0x0D, 0x00, 0x90,
                         0x01, 0x13, 0x27, 0x2B, 0x00, 0x31};
  REQUIRE(encode(m) == expected);
}

TEST_CASE("BLE advertisement: rejects sensor values outside valid ranges", "[ble][advertisement]") {
  MeasuresAGo m{};
  SECTION("below minimum") {
    m.temp_hum_a = {-40.01f, -0.01f};
    m.pm_a.pm_25 = -0.01f;
    m.co2.co2 = -1;
    m.pressure = {299.99f, -500.01f};
    m.power.battery_percentage = -0.01f;
  }
  SECTION("above maximum") {
    m.temp_hum_a = {125.01f, 100.01f};
    m.pm_a.pm_25 = 6553.5f; // Would collide with the invalid sentinel.
    m.co2.co2 = 10001;
    m.pressure = {1100.01f, 10000.01f};
    m.power.battery_percentage = 100.01f;
  }
  REQUIRE(encode(m) == encode(MeasuresAGo{}));
}

TEST_CASE("BLE advertisement: nonfinite and overflowing values are invalid",
          "[ble][advertisement]") {
  const float value =
      GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
               -std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max());
  MeasuresAGo m{};
  m.temp_hum_a = {value, value};
  m.pm_a.pm_25 = value;
  m.pressure = {value, value};
  m.power.battery_percentage = value;
  REQUIRE(encode(m) == encode(MeasuresAGo{}));
}

TEST_CASE("BLE advertisement: one failed field preserves the remaining readings",
          "[ble][advertisement]") {
  MeasuresAGo m{};
  m.temp_hum_a.temperature = 0.0f; // Humidity remains invalid.
  m.pressure.altitude = 0.0f;      // Pressure remains invalid.
  m.co2.co2 = 400;
  const Payload expected{0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0x90,
                         0x01, 0xFF, 0xFF, 0x00, 0x00, 0xFF};
  REQUIRE(encode(m) == expected);
}

TEST_CASE("BLE advertisement: wake report is delivered once with a rising sequence",
          "[ble][advertisement]") {
  using GoBleAdvertisement::WakeOutcome;
  using GoBleAdvertisement::WakeReport;
  WakeReport discard{};
  GoBleAdvertisement::take_wake_report(discard);

  WakeReport first{};
  first.outcome = WakeOutcome::SkippedButton;
  GoBleAdvertisement::store_wake_report(first);
  WakeReport second{};
  second.outcome = WakeOutcome::Advertised;
  GoBleAdvertisement::store_wake_report(second);

  WakeReport out{};
  REQUIRE(GoBleAdvertisement::take_wake_report(out));
  CHECK(out.outcome == WakeOutcome::Advertised);
  CHECK(out.sequence > 0);
  CHECK_FALSE(GoBleAdvertisement::take_wake_report(out));

  GoBleAdvertisement::store_wake_report(second);
  WakeReport next{};
  REQUIRE(GoBleAdvertisement::take_wake_report(next));
  CHECK(next.sequence == out.sequence + 1);
}

TEST_CASE("BLE advertisement: wake report formats as one line", "[ble][advertisement]") {
  GoBleAdvertisement::WakeReport report{};
  report.sequence = 7;
  report.outcome = GoBleAdvertisement::WakeOutcome::SetupFailed;
  report.failed_step = GoBleAdvertisement::SetupStep::Start;
  report.awake_ms = 14250;
  report.has_payload = true;
  report.payload = encode(MeasuresAGo{});
  char text[192];
  GoBleAdvertisement::format_wake_report(report, text, sizeof(text));
  CHECK(std::string(text) ==
        "seq=7 outcome=setup-failed failed_step=start adv_start_ms=0 adv_duration_ms=0 "
        "awake_ms=14250 payload=010080ffffffffffffff0080ff");
}
