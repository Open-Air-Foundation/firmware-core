#include "go_thermal_comp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using thermal_comp::correct_humidity;
using thermal_comp::correct_temperature;

TEST_CASE("board_heats_tab gates the model on the heat-flow direction") {
  CHECK(thermal_comp::board_heats_tab(29.9f, 34.0f));
  CHECK_FALSE(thermal_comp::board_heats_tab(29.9f, 27.0f));
  CHECK_FALSE(thermal_comp::board_heats_tab(27.0f, 27.0f));
}

TEST_CASE("correct_temperature inverts the coupling model") {
  const float amb = 27.0f;
  const float board = 35.0f;
  const float sht = amb + thermal_comp::ALPHA * (board - amb) + thermal_comp::INTERCEPT_C;
  CHECK_THAT(correct_temperature(sht, board), WithinAbs(amb, 0.01f));
}

TEST_CASE("correct_temperature matches golden values") {
  // (29.35 - 0.42 * 35.16 - 0.38) / 0.58
  CHECK_THAT(correct_temperature(29.35f, 35.16f), WithinAbs(24.49f, 0.02f));
  // Uniform-temperature edge: the positive fitted intercept is the running
  // device's self-heating floor, so zero-excess input under-reads by
  // INTERCEPT_C/(1-ALPHA).
  CHECK_THAT(correct_temperature(25.0f, 25.0f), WithinAbs(24.34f, 0.02f));
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
