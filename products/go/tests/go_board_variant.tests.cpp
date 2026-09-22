/**
 * AirGradient Go — Board variant helper unit tests
 *
 * Pure boot-time decisions in go_board.h: mapping the two I2C probes to a
 * BoardVariant, and the per-variant PM-enable polarity.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include "go_board.h"

TEST_CASE("detect_board_variant prefers the v2 expander over the fuel gauge", "[board]") {
  // The BQ27427 (v1) and BQ27742 (v2) share address 0x55, so only the
  // TCA6408A at 0x20 can identify v2.
  CHECK(detect_board_variant(true, true) == BoardVariant::V2);
  CHECK(detect_board_variant(true, false) == BoardVariant::V2);
  CHECK(detect_board_variant(false, true) == BoardVariant::V1);
  CHECK(detect_board_variant(false, false) == BoardVariant::Prototype);
}

TEST_CASE("pm_power_on_level is active-low on every TMUX121 board", "[board]") {
  CHECK(pm_power_on_level(BoardVariant::Prototype) == 1);
  CHECK(pm_power_on_level(BoardVariant::V1) == 0);
  CHECK(pm_power_on_level(BoardVariant::V2) == 0);
}

TEST_CASE("board_variant_str names every variant", "[board]") {
  CHECK(std::string(board_variant_str(BoardVariant::Prototype)) == "Prototype");
  CHECK(std::string(board_variant_str(BoardVariant::V1)) == "V1");
  CHECK(std::string(board_variant_str(BoardVariant::V2)) == "V2");
}

TEST_CASE("touch_tuning_for keeps rev 1 on what it shipped with", "[board][touch]") {
  // Rev 1's pad has never been measured. Every unit in the field runs 128x
  // with a threshold of 64, and moving it on a guess could leave them with no
  // working touch at all.
  CHECK(touch_tuning_for(BoardVariant::V1).delta_sense == 0);
  CHECK(touch_tuning_for(BoardVariant::V1).threshold == 64);
  CHECK(touch_tuning_for(BoardVariant::Prototype).delta_sense == 0);
  CHECK(touch_tuning_for(BoardVariant::Prototype).threshold == 64);
}

TEST_CASE("touch_tuning_for gives rev 2.0 the gain its pads were measured at",
          "[board][touch]") {
  // 4 is 8x, where the strongest pad reads 60 and the weakest 43 — both clear
  // of the register ceiling and far above the noise.
  CHECK(touch_tuning_for(BoardVariant::V2).delta_sense == 4);
  CHECK(touch_tuning_for(BoardVariant::V2).threshold == 21);
}

TEST_CASE("the rev 2.0 threshold sits between the noise and a real touch",
          "[board][touch]") {
  // Measured at 16x with the enclosure fitted: idle stays within one count and
  // the weakest pad, CS2, peaks at 87 — 43 once the gain is halved to the 8x
  // this ships at.
  constexpr uint8_t IDLE_NOISE = 1;
  constexpr uint8_t WEAKEST_PEAK = 43;
  const uint8_t threshold = touch_tuning_for(BoardVariant::V2).threshold;
  CHECK(threshold > IDLE_NOISE * 8);    // nowhere near tripping on noise
  CHECK(threshold <= WEAKEST_PEAK / 2); // and reached at half a press
}

