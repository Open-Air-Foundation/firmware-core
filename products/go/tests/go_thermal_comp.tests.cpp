#include "go_thermal_comp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using thermal_comp::alpha_for_power_source;
using thermal_comp::correct_humidity;
using thermal_comp::correct_temperature;

TEST_CASE("alpha_for_power_source picks the regime constant") {
  CHECK(alpha_for_power_source(false) == thermal_comp::ALPHA_BATTERY);
  CHECK(alpha_for_power_source(true) == thermal_comp::ALPHA_EXTERNAL_POWER);
  // The charger dissipates next to the SHT tab, so external power raises
  // the SHT's share of the board excess.
  CHECK(thermal_comp::ALPHA_EXTERNAL_POWER > thermal_comp::ALPHA_BATTERY);
}

TEST_CASE("correct_temperature inverts the coupling model exactly") {
  // Forward model: sht = amb + alpha*(board - amb) + INTERCEPT_C.
  const float amb = 27.0f;
  const float board = 33.0f;
  for (const float alpha : {thermal_comp::ALPHA_BATTERY, thermal_comp::ALPHA_EXTERNAL_POWER}) {
    const float sht = amb + alpha * (board - amb) + thermal_comp::INTERCEPT_C;
    CHECK_THAT(correct_temperature(sht, board, alpha), WithinAbs(amb, 0.01f));
  }
}

TEST_CASE("correct_temperature matches golden values") {
  CHECK_THAT(correct_temperature(29.35f, 35.16f, thermal_comp::ALPHA_BATTERY),
             WithinAbs(24.02f, 0.02f));
  CHECK_THAT(correct_temperature(29.35f, 35.16f, thermal_comp::ALPHA_EXTERNAL_POWER),
             WithinAbs(23.07f, 0.02f));
  // Uniform-temperature edge: with the slightly negative fitted intercept,
  // zero-excess input over-reads by |INTERCEPT_C|/(1-alpha).
  CHECK_THAT(correct_temperature(25.0f, 25.0f, thermal_comp::ALPHA_BATTERY),
             WithinAbs(25.25f, 0.02f));
}

TEST_CASE("correct_temperature reproduces field references per regime") {
  // Unit C on battery, 2026-08-31 22:45, reference 29.73 degC.
  CHECK_THAT(correct_temperature(33.03f, 36.78f, alpha_for_power_source(false)),
             WithinAbs(29.73f, 0.1f));
  // Unit B charging at 484 mA, 2026-08-31 12:35-13:00, reference 26.70 degC.
  CHECK_THAT(correct_temperature(31.08f, 35.20f, alpha_for_power_source(true)),
             WithinAbs(26.70f, 0.1f));
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
