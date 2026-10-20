#include "go_thermal_comp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using Catch::Matchers::WithinAbs;
using thermal_comp::alpha_for_band;
using thermal_comp::BandSelector;
using thermal_comp::correct_humidity;
using thermal_comp::correct_temperature;
using thermal_comp::ThermalBand;

TEST_CASE("alpha_for_band maps every band to its constant") {
  CHECK(alpha_for_band(ThermalBand::Cold) == thermal_comp::ALPHA_COLD);
  CHECK(alpha_for_band(ThermalBand::Normal) == thermal_comp::ALPHA_NORMAL);
  CHECK(alpha_for_band(ThermalBand::Hot) == thermal_comp::ALPHA_HOT);
}

TEST_CASE("band selection crosses edges only past the hysteresis margin") {
  BandSelector sel;
  CHECK(sel.band() == ThermalBand::Normal);
  // Sitting on the cold edge stays Normal; crossing the margin flips.
  CHECK(sel.update(thermal_comp::BAND_COLD_MAX_C) == ThermalBand::Normal);
  CHECK(sel.update(thermal_comp::BAND_COLD_MAX_C - thermal_comp::BAND_HYSTERESIS_C - 0.1f) ==
        ThermalBand::Cold);
  // Inside the hysteresis window Cold is sticky; past it Normal returns.
  CHECK(sel.update(thermal_comp::BAND_COLD_MAX_C + thermal_comp::BAND_HYSTERESIS_C) ==
        ThermalBand::Cold);
  CHECK(sel.update(thermal_comp::BAND_COLD_MAX_C + thermal_comp::BAND_HYSTERESIS_C + 0.1f) ==
        ThermalBand::Normal);
  // Same shape on the hot side.
  CHECK(sel.update(thermal_comp::BAND_HOT_MIN_C) == ThermalBand::Normal);
  CHECK(sel.update(thermal_comp::BAND_HOT_MIN_C + thermal_comp::BAND_HYSTERESIS_C + 0.1f) ==
        ThermalBand::Hot);
  CHECK(sel.update(thermal_comp::BAND_HOT_MIN_C - thermal_comp::BAND_HYSTERESIS_C) ==
        ThermalBand::Hot);
  CHECK(sel.update(thermal_comp::BAND_HOT_MIN_C - thermal_comp::BAND_HYSTERESIS_C - 0.1f) ==
        ThermalBand::Normal);
}

TEST_CASE("a hot excursion never lands directly in Cold") {
  BandSelector sel;
  sel.update(40.0f);
  REQUIRE(sel.band() == ThermalBand::Hot);
  // A single wild low estimate first returns to Normal, not Cold.
  CHECK(sel.update(5.0f) == ThermalBand::Normal);
  CHECK(sel.update(5.0f) == ThermalBand::Cold);
}

TEST_CASE("board_heats_tab gates the model on the heat-flow direction") {
  CHECK(thermal_comp::board_heats_tab(29.9f, 34.0f));
  CHECK_FALSE(thermal_comp::board_heats_tab(29.9f, 27.0f));
  CHECK_FALSE(thermal_comp::board_heats_tab(27.0f, 27.0f));
}

TEST_CASE("correct_temperature inverts the coupling model in every band") {
  const float amb = 27.0f;
  const float board = 33.0f;
  for (const ThermalBand band : {ThermalBand::Cold, ThermalBand::Normal, ThermalBand::Hot}) {
    const float alpha = alpha_for_band(band);
    const float sht = amb + alpha * (board - amb) + thermal_comp::INTERCEPT_C;
    CHECK_THAT(correct_temperature(sht, board, alpha), WithinAbs(amb, 0.01f));
  }
}

TEST_CASE("correct_temperature matches golden values") {
  // (29.35 - 0.50 * 35.16 + 0.13) / 0.50
  CHECK_THAT(correct_temperature(29.35f, 35.16f, thermal_comp::ALPHA_NORMAL),
             WithinAbs(23.80f, 0.02f));
  // Uniform-temperature edge: with the slightly negative fitted intercept,
  // zero-excess input over-reads by |INTERCEPT_C|/(1-alpha).
  CHECK_THAT(correct_temperature(25.0f, 25.0f, thermal_comp::ALPHA_NORMAL),
             WithinAbs(25.26f, 0.02f));
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

TEST_CASE("dps_plausible accepts the board gradient and rejects a calibration offset") {
  using namespace thermal_comp;
  CHECK(dps_plausible(35.45f, 35.45f));
  CHECK(dps_plausible(36.75f, 35.45f)); // charging gradient, NTC above DPS
  CHECK(dps_plausible(46.72f, 47.42f)); // hot box, DPS above NTC
  CHECK(dps_plausible(29.0f, 29.0f + DPS_NTC_MAX_DIFF_C));
  CHECK_FALSE(dps_plausible(29.0f, 29.0f + DPS_NTC_MAX_DIFF_C + 0.01f));
  CHECK_FALSE(dps_plausible(35.45f, 27.59f)); // unit 1164: DPS 7.9 degC low
  CHECK_FALSE(dps_plausible(27.59f, 35.45f)); // same offset, other sign
}

TEST_CASE("DpsCheck decides on the first pair and latches") {
  using namespace thermal_comp;
  SECTION("good unit stays Ok even if a later pair looks off") {
    DpsCheck check;
    CHECK(check.state() == DpsCheck::State::Pending);
    CHECK_FALSE(check.rejected());
    CHECK(check.update(30.0f, 30.5f) == DpsCheck::State::Ok);
    CHECK(check.update(30.0f, 40.0f) == DpsCheck::State::Ok);
    CHECK_FALSE(check.rejected());
  }
  SECTION("bad unit stays Rejected even if a later pair agrees") {
    DpsCheck check;
    CHECK(check.update(35.45f, 27.59f) == DpsCheck::State::Rejected);
    CHECK(check.update(30.0f, 30.0f) == DpsCheck::State::Rejected);
    CHECK(check.rejected());
  }
}
