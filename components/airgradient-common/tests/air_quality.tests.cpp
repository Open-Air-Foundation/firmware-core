#include "air_quality.h"
#include "measures_types.h"

#include <limits>

#include <catch2/catch_test_macros.hpp>

namespace {

void check_rgb(Rgb actual, Rgb expected) {
  CHECK(actual.r == expected.r);
  CHECK(actual.g == expected.g);
  CHECK(actual.b == expected.b);
}

} // namespace

TEST_CASE("Air quality: CO2 category boundaries", "[air_quality][co2]") {
  struct TestCase {
    int ppm;
    air_quality::Co2Category expected;
  };
  const TestCase cases[] = {
      {0, air_quality::Co2Category::Excellent},
      {400, air_quality::Co2Category::Excellent},
      {800, air_quality::Co2Category::Excellent},
      {801, air_quality::Co2Category::Acceptable},
      {1000, air_quality::Co2Category::Acceptable},
      {1001, air_quality::Co2Category::NotIdeal},
      {1500, air_quality::Co2Category::NotIdeal},
      {1501, air_quality::Co2Category::ToBeAvoided},
      {2000, air_quality::Co2Category::ToBeAvoided},
      {2001, air_quality::Co2Category::Unhealthy},
      {3000, air_quality::Co2Category::Unhealthy},
      {3001, air_quality::Co2Category::VeryUnhealthy},
      {10000, air_quality::Co2Category::VeryUnhealthy},
  };
  for (const auto &test : cases) {
    CAPTURE(test.ppm);
    CHECK(air_quality::co2_to_category(test.ppm) == test.expected);
  }
}

TEST_CASE("Air quality: invalid CO2 readings produce an invalid category and black",
          "[air_quality][co2]") {
  for (const int ppm : {std::numeric_limits<int>::min(), MeasuresInvalid::CO2,
                        MeasuresRange::MAX_VALID_CO2 + 1, std::numeric_limits<int>::max()}) {
    CAPTURE(ppm);
    const auto category = air_quality::co2_to_category(ppm);
    CHECK(category == air_quality::Co2Category::Invalid);
    check_rgb(air_quality::category_to_rgb(category), {0, 0, 0});
  }
}

TEST_CASE("Air quality: US AQI category colors preserve the Go palette", "[air_quality][color]") {
  struct TestCase {
    air_quality::UsAqiCategory category;
    Rgb expected;
  };
  const TestCase cases[] = {
      {air_quality::UsAqiCategory::Good, {0, 255, 0}},
      {air_quality::UsAqiCategory::Moderate, {255, 255, 0}},
      {air_quality::UsAqiCategory::UnhealthySensitive, {255, 128, 0}},
      {air_quality::UsAqiCategory::Unhealthy, {255, 0, 0}},
      {air_quality::UsAqiCategory::VeryUnhealthy, {128, 0, 128}},
      {air_quality::UsAqiCategory::Hazardous, {139, 69, 19}},
      {air_quality::UsAqiCategory::Invalid, {0, 0, 0}},
  };
  for (const auto &test : cases) {
    CAPTURE(static_cast<int>(test.category));
    check_rgb(air_quality::category_to_rgb(test.category), test.expected);
  }
}

TEST_CASE("Air quality: CO2 categories map to the agreed indicator colors",
          "[air_quality][co2][color]") {
  struct TestCase {
    air_quality::Co2Category category;
    Rgb expected;
  };
  const TestCase cases[] = {
      {air_quality::Co2Category::Excellent, {0, 255, 0}},
      {air_quality::Co2Category::Acceptable, {255, 255, 0}},
      {air_quality::Co2Category::NotIdeal, {255, 128, 0}},
      {air_quality::Co2Category::ToBeAvoided, {255, 0, 0}},
      {air_quality::Co2Category::Unhealthy, {128, 0, 128}},
      {air_quality::Co2Category::VeryUnhealthy, {139, 69, 19}},
      {air_quality::Co2Category::Invalid, {0, 0, 0}},
  };
  for (const auto &test : cases) {
    CAPTURE(static_cast<int>(test.category));
    check_rgb(air_quality::category_to_rgb(test.category), test.expected);
  }
}

TEST_CASE("Air quality: unknown categories map to black", "[air_quality][color]") {
  check_rgb(air_quality::category_to_rgb(static_cast<air_quality::UsAqiCategory>(255)), {0, 0, 0});
  check_rgb(air_quality::category_to_rgb(static_cast<air_quality::Co2Category>(255)), {0, 0, 0});
}
