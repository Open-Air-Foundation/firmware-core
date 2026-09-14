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
