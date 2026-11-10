#include <catch2/catch_test_macros.hpp>

#include "go_display_geometry.h"
#include "go_display_refresh.h"

namespace {
constexpr uint32_t COOLDOWN_MS = DisplayRefreshPolicy::DEFAULT_COOLDOWN_MS;
constexpr uint8_t MAX_PARTIAL_OPS = DisplayRefreshPolicy::DEFAULT_MAX_PARTIAL_OPS;

DisplayRefreshDecision home_update(const DisplayRefreshPolicy &policy, uint32_t now_ms,
                                   bool header_changed = false, bool menu_cleanup_due = false) {
  return policy.home_update(header_changed, menu_cleanup_due, now_ms, MAX_PARTIAL_OPS, COOLDOWN_MS);
}
} // namespace

TEST_CASE("Display refresh: unplug followed by lock uses Fast then full-canvas Partial",
          "[display][refresh]") {
  DisplayRefreshPolicy policy;
  CHECK(home_update(policy, 1000, true).mode == RefreshMode::Fast);
  policy.completed(RefreshMode::Fast, 2500);

  const auto lock = home_update(policy, 2501, true);
  REQUIRE(lock.mode == RefreshMode::Partial);
  REQUIRE(lock.full_canvas_partial);
  const auto region = go_display_geometry::partial_region(lock.full_canvas_partial);
  CHECK(region.y == 0);
  CHECK(region.byte_count == go_display_geometry::PHYSICAL_BYTES);
  policy.completed(lock.mode, 3001);

  // The Partial did not move the cooldown; it is measured from Fast completion.
  CHECK(home_update(policy, 2500 + COOLDOWN_MS - 1, true).mode == RefreshMode::Partial);
  CHECK(home_update(policy, 2500 + COOLDOWN_MS, true).mode == RefreshMode::Fast);
  CHECK(policy.differential_ops() == 2);
}

TEST_CASE("Display refresh: measurement after Fast retains due Full cleanup until cooldown ends",
          "[display][refresh]") {
  DisplayRefreshPolicy policy;
  policy.completed(RefreshMode::Full, 0);
  for (uint8_t i = 0; i < MAX_PARTIAL_OPS - 1; ++i) {
    policy.completed(RefreshMode::Partial, 1000 + i);
  }
  REQUIRE(home_update(policy, 6000, true).mode == RefreshMode::Fast);
  policy.completed(RefreshMode::Fast, 7500);
  REQUIRE(policy.differential_ops() == MAX_PARTIAL_OPS);

  const auto measurement = home_update(policy, 8000);
  CHECK(measurement.mode == RefreshMode::Partial);
  CHECK_FALSE(measurement.full_canvas_partial);
  policy.completed(measurement.mode, 8500);
  CHECK(policy.differential_ops() == MAX_PARTIAL_OPS + 1);
  CHECK(home_update(policy, 7500 + COOLDOWN_MS - 1).mode == RefreshMode::Partial);
  CHECK(home_update(policy, 7500 + COOLDOWN_MS).mode == RefreshMode::Full);

  // Selection/submission alone cannot clear the debt or start a cooldown.
  CHECK(policy.differential_ops() == MAX_PARTIAL_OPS + 1);
  CHECK(home_update(policy, 7500 + COOLDOWN_MS + 1).mode == RefreshMode::Full);
  policy.completed(RefreshMode::Full, 15000);
  CHECK(policy.differential_ops() == 0);
  CHECK(home_update(policy, 15001, true).mode == RefreshMode::Partial);
}

TEST_CASE("Display refresh: routine measurements stay body-only Partial after cooldown",
          "[display][refresh]") {
  DisplayRefreshPolicy policy;
  policy.completed(RefreshMode::Fast, 1000);
  for (const auto now : {1001u, 1000 + COOLDOWN_MS}) {
    CAPTURE(now);
    const auto measurement = home_update(policy, now);
    CHECK(measurement.mode == RefreshMode::Partial);
    CHECK_FALSE(measurement.full_canvas_partial);
    CHECK(go_display_geometry::partial_region(measurement.full_canvas_partial).y ==
          go_display_geometry::BODY_Y);
  }
}

TEST_CASE("Display refresh: deferred menu header is included while cleanup waits",
          "[display][refresh]") {
  DisplayRefreshPolicy policy;
  policy.completed(RefreshMode::Fast, 1000);
  // Header values can match the last menu submission without having been painted.
  const auto update = home_update(policy, 2000, false, true);
  CHECK(update.mode == RefreshMode::Partial);
  CHECK(update.full_canvas_partial);
  policy.completed(update.mode, 2500);
  CHECK(home_update(policy, 1000 + COOLDOWN_MS, false, true).mode == RefreshMode::Fast);
}

TEST_CASE("Display refresh: initialization Full starts cooldown even at timestamp zero",
          "[display][refresh]") {
  DisplayRefreshPolicy policy;
  REQUIRE(home_update(policy, 0, true).mode == RefreshMode::Fast);
  policy.completed(RefreshMode::Full, 0);
  CHECK(home_update(policy, COOLDOWN_MS - 1, true).mode == RefreshMode::Partial);
  CHECK(home_update(policy, COOLDOWN_MS, true).mode == RefreshMode::Fast);
}

TEST_CASE("Display refresh: cooldown survives the millisecond clock wrapping",
          "[display][refresh]") {
  DisplayRefreshPolicy policy;
  constexpr uint32_t completed_ms = UINT32_MAX - 1000;
  policy.completed(RefreshMode::Fast, completed_ms);
  CHECK(home_update(policy, completed_ms + COOLDOWN_MS - 1, true).mode == RefreshMode::Partial);
  CHECK(home_update(policy, completed_ms + COOLDOWN_MS, true).mode == RefreshMode::Fast);
}

TEST_CASE("Display refresh: long menu sessions saturate cleanup debt instead of losing it",
          "[display][refresh]") {
  DisplayRefreshPolicy policy;
  policy.completed(RefreshMode::Full, 0);
  for (uint32_t i = 0; i < 300; ++i) {
    policy.completed(RefreshMode::Partial, i);
  }
  CHECK(policy.differential_ops() == UINT8_MAX);
  CHECK(home_update(policy, COOLDOWN_MS - 1, false, true).mode == RefreshMode::Partial);
  CHECK(home_update(policy, COOLDOWN_MS, false, true).mode == RefreshMode::Full);
  policy.completed(RefreshMode::Full, COOLDOWN_MS + 2500);
  CHECK(policy.differential_ops() == 0);
}

TEST_CASE("Display refresh: limits remain configurable", "[display][refresh]") {
  DisplayRefreshPolicy policy;
  policy.completed(RefreshMode::Fast, 1000);
  CHECK(policy.home_update(true, false, 1001, MAX_PARTIAL_OPS, 0).mode == RefreshMode::Fast);
  CHECK(policy.home_update(true, false, 1999, MAX_PARTIAL_OPS, 1000).mode == RefreshMode::Partial);
  CHECK(policy.home_update(true, false, 2000, MAX_PARTIAL_OPS, 1000).mode == RefreshMode::Fast);
  CHECK(policy.home_update(false, false, 2000, 1, 1000).mode == RefreshMode::Full);
}
