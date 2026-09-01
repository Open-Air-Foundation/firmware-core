#include "go_thermal_comp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using thermal_comp::correct_humidity;
using thermal_comp::correct_temperature;

TEST_CASE("correct_temperature inverts the coupling model exactly") {
  // Forward model: sht = amb + ALPHA*(board - amb) + INTERCEPT_C.
  const float amb = 27.0f;
  const float board = 33.0f;
  const float sht = amb + thermal_comp::ALPHA * (board - amb) + thermal_comp::INTERCEPT_C;
  CHECK_THAT(correct_temperature(sht, board), WithinAbs(amb, 0.01f));
}

TEST_CASE("correct_temperature matches golden values") {
  CHECK_THAT(correct_temperature(29.35f, 35.16f), WithinAbs(23.28f, 0.02f));
  // Uniform-temperature edge: with the slightly negative fitted intercept,
  // zero-excess input over-reads by |INTERCEPT_C|/(1-ALPHA).
  CHECK_THAT(correct_temperature(25.0f, 25.0f), WithinAbs(25.27f, 0.02f));
}

TEST_CASE("correct_humidity rescales RH to the corrected temperature") {
  // Cooler corrected temperature means higher relative humidity.
  CHECK_THAT(correct_humidity(50.0f, 30.404f, 27.0f), WithinAbs(60.91f, 0.1f));
  CHECK(correct_humidity(50.0f, 30.0f, 25.0f) > 50.0f);
}

TEST_CASE("correct_humidity clamps to the physical range") {
  CHECK(correct_humidity(99.0f, 35.0f, 20.0f) == 100.0f);
  CHECK(correct_humidity(0.0f, 30.0f, 25.0f) == 0.0f);
}
