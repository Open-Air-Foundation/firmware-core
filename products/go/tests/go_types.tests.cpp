/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <type_traits>

#include <catch2/catch_test_macros.hpp>

#include "go_type_helpers.hpp"

namespace {

constexpr uint64_t MILLISECONDS_PER_SECOND = 1000;
constexpr uint32_t MAX_TIME_S = TRACKING_TIME_INVALID_S - 1;

void check_same_timing(const TrackingTiming &actual, const TrackingTiming &expected) {
  CHECK(actual.session_started_s == expected.session_started_s);
  CHECK(actual.recording_accumulated_s == expected.recording_accumulated_s);
  CHECK(actual.recording_started_s == expected.recording_started_s);
  CHECK(actual.last_record_s == expected.last_record_s);
}

} // namespace

TEST_CASE("tracking session predicate remains a constexpr helper", "[go][tracking][timing]") {
  STATIC_REQUIRE_FALSE(tracking_session_active(TrackingState::Idle));
  STATIC_REQUIRE(tracking_session_active(TrackingState::Recording));
  STATIC_REQUIRE(tracking_session_active(TrackingState::Paused));
  STATIC_REQUIRE_FALSE(tracking_session_active(static_cast<TrackingState>(255)));
}

TEST_CASE("tracking timing defaults are unknown and layout is compact", "[go][tracking][timing]") {
  STATIC_REQUIRE(sizeof(TrackingTiming) == 16);
  STATIC_REQUIRE(std::is_trivially_copyable_v<TrackingTiming>);
  STATIC_REQUIRE(std::is_standard_layout_v<TrackingTiming>);
  STATIC_REQUIRE(sizeof(RtcAppState) == 28);
  STATIC_REQUIRE(std::is_trivially_copyable_v<RtcAppState>);
  check_same_timing(RtcAppState{}.tracking_timing, TrackingTiming{});

  const TrackingTiming timing;
  CHECK(timing.session_started_s == TRACKING_TIME_INVALID_S);
  CHECK(timing.recording_accumulated_s == TRACKING_TIME_INVALID_S);
  CHECK(timing.recording_started_s == TRACKING_TIME_INVALID_S);
  CHECK(timing.last_record_s == TRACKING_TIME_INVALID_S);
  CHECK(tracking_elapsed_s(timing, 0) == TRACKING_TIME_INVALID_S);
  CHECK(tracking_recording_s(timing, 0) == TRACKING_TIME_INVALID_S);
  CHECK(tracking_last_record_age_s(timing, 0) == TRACKING_TIME_INVALID_S);
}

TEST_CASE("tracking seconds conversion divides before narrowing", "[go][tracking][timing]") {
  CHECK(tracking_seconds_from_ms(0) == 0);
  CHECK(tracking_seconds_from_ms(999) == 0);
  CHECK(tracking_seconds_from_ms(1000) == 1);
  CHECK(tracking_seconds_from_ms(1999) == 1);

  constexpr uint64_t AFTER_32_BIT_MS = static_cast<uint64_t>(UINT32_MAX) + 1;
  CHECK(tracking_seconds_from_ms(AFTER_32_BIT_MS) == 4294967);

  constexpr uint64_t LAST_VALID_MS =
      static_cast<uint64_t>(MAX_TIME_S) * MILLISECONDS_PER_SECOND + 999;
  CHECK(tracking_seconds_from_ms(LAST_VALID_MS) == MAX_TIME_S);
  CHECK(tracking_seconds_from_ms(LAST_VALID_MS + 1) == TRACKING_TIME_INVALID_S);
  CHECK(tracking_seconds_from_ms(UINT64_MAX) == TRACKING_TIME_INVALID_S);
}

TEST_CASE("tracking timing accepts timestamp zero and a point at start", "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 0));
  CHECK(timing.session_started_s == 0);
  CHECK(timing.recording_accumulated_s == 0);
  CHECK(timing.recording_started_s == 0);
  CHECK(tracking_elapsed_s(timing, 0) == 0);
  CHECK(tracking_recording_s(timing, 0) == 0);
  CHECK(tracking_last_record_age_s(timing, 0) == TRACKING_TIME_INVALID_S);

  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 0));
  CHECK(timing.last_record_s == 0);
  CHECK(tracking_last_record_age_s(timing, 0) == 0);
}

TEST_CASE("tracking timing separates active segments from session elapsed time",
          "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 100));
  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 110));
  CHECK(tracking_recording_s(timing, 120) == 20);

  REQUIRE(tracking_timing_pause(timing, 130));
  CHECK(timing.recording_accumulated_s == 30);
  CHECK(timing.recording_started_s == TRACKING_TIME_INVALID_S);
  CHECK(tracking_recording_s(timing, 150) == 30);
  CHECK(tracking_elapsed_s(timing, 150) == 50);
  CHECK(tracking_last_record_age_s(timing, 150) == 40);

  REQUIRE(tracking_timing_resume(timing, 150));
  CHECK(timing.session_started_s == 100);
  CHECK(timing.recording_accumulated_s == 30);
  CHECK(timing.recording_started_s == 150);
  CHECK(timing.last_record_s == 110);
  CHECK(tracking_recording_s(timing, 170) == 50);
  CHECK(tracking_elapsed_s(timing, 170) == 70);

  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 170));
  REQUIRE(tracking_timing_pause(timing, 180));
  CHECK(timing.recording_accumulated_s == 60);
  REQUIRE(tracking_timing_resume(timing, 200));
  CHECK(tracking_recording_s(timing, 210) == 70);
  CHECK(tracking_elapsed_s(timing, 210) == 110);
  CHECK(tracking_last_record_age_s(timing, 210) == 40);
}

TEST_CASE("tracking pause and resume are idempotent", "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 10));
  auto previous = timing;
  REQUIRE(tracking_timing_resume(timing, 20));
  check_same_timing(timing, previous);
  CHECK(tracking_recording_s(timing, 20) == 10);

  REQUIRE(tracking_timing_pause(timing, 30));
  previous = timing;
  REQUIRE(tracking_timing_pause(timing, 40));
  check_same_timing(timing, previous);
  CHECK(tracking_recording_s(timing, 50) == 20);
}

TEST_CASE("tracking timing copied across sleep keeps the active segment",
          "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 10));
  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 20));

  SECTION("Recording sleep is included without periodic counter updates") {
    const TrackingTiming restored = timing;
    CHECK(tracking_elapsed_s(restored, 5010) == 5000);
    CHECK(tracking_recording_s(restored, 5010) == 5000);
    CHECK(tracking_last_record_age_s(restored, 5010) == 4990);
  }

  SECTION("Paused sleep does not increase recording time") {
    REQUIRE(tracking_timing_pause(timing, 30));
    TrackingTiming restored = timing;
    CHECK(tracking_elapsed_s(restored, 5030) == 5020);
    CHECK(tracking_recording_s(restored, 5030) == 20);
    CHECK(tracking_last_record_age_s(restored, 5030) == 5010);
    REQUIRE(tracking_timing_resume(restored, 5030));
    CHECK(tracking_recording_s(restored, 5040) == 30);
  }
}

TEST_CASE("tracking timing records differences of whole-second anchors", "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, tracking_seconds_from_ms(1900)));
  REQUIRE(tracking_timing_pause(timing, tracking_seconds_from_ms(2100)));
  // One second boundary was crossed; no subsecond remainder is retained.
  CHECK(timing.recording_accumulated_s == 1);
  REQUIRE(tracking_timing_resume(timing, tracking_seconds_from_ms(2200)));
  REQUIRE(tracking_timing_pause(timing, tracking_seconds_from_ms(2900)));
  CHECK(timing.recording_accumulated_s == 1);
}

TEST_CASE("tracking restart and reset discard previous session timing", "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 100));
  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 110));
  REQUIRE(tracking_timing_pause(timing, 120));
  REQUIRE(tracking_timing_start(timing, 200));
  CHECK(timing.session_started_s == 200);
  CHECK(timing.recording_accumulated_s == 0);
  CHECK(timing.recording_started_s == 200);
  CHECK(timing.last_record_s == TRACKING_TIME_INVALID_S);

  timing = TrackingTiming{};
  check_same_timing(timing, TrackingTiming{});
}

TEST_CASE("tracking mutations reject unknown time without changing state",
          "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 100));
  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 110));
  const auto previous = timing;

  CHECK_FALSE(tracking_timing_start(timing, TRACKING_TIME_INVALID_S));
  CHECK_FALSE(tracking_timing_pause(timing, TRACKING_TIME_INVALID_S));
  CHECK_FALSE(tracking_timing_resume(timing, TRACKING_TIME_INVALID_S));
  CHECK_FALSE(
      tracking_timing_record_accepted(timing, TrackingState::Recording, TRACKING_TIME_INVALID_S));
  check_same_timing(timing, previous);
  CHECK(tracking_elapsed_s(timing, TRACKING_TIME_INVALID_S) == TRACKING_TIME_INVALID_S);
  CHECK(tracking_recording_s(timing, TRACKING_TIME_INVALID_S) == TRACKING_TIME_INVALID_S);
  CHECK(tracking_last_record_age_s(timing, TRACKING_TIME_INVALID_S) == TRACKING_TIME_INVALID_S);
}

TEST_CASE("tracking mutations require an initialized session", "[go][tracking][timing]") {
  TrackingTiming timing;
  CHECK_FALSE(tracking_timing_pause(timing, 10));
  CHECK_FALSE(tracking_timing_resume(timing, 10));
  CHECK_FALSE(tracking_timing_record_accepted(timing, TrackingState::Recording, 10));
  check_same_timing(timing, TrackingTiming{});
}

TEST_CASE("tracking record acceptance requires Recording state and ordered timestamp",
          "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 100));
  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 110));
  auto previous = timing;
  CHECK_FALSE(tracking_timing_record_accepted(timing, TrackingState::Recording, 109));
  check_same_timing(timing, previous);
  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 110));
  check_same_timing(timing, previous);

  REQUIRE(tracking_timing_pause(timing, 120));
  previous = timing;
  CHECK_FALSE(tracking_timing_record_accepted(timing, TrackingState::Paused, 130));
  CHECK_FALSE(tracking_timing_record_accepted(timing, TrackingState::Idle, 130));
  check_same_timing(timing, previous);
}

TEST_CASE("tracking timing rejects time before known anchors", "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 100));
  const auto previous = timing;
  CHECK(tracking_elapsed_s(timing, 99) == TRACKING_TIME_INVALID_S);
  CHECK(tracking_recording_s(timing, 99) == TRACKING_TIME_INVALID_S);
  CHECK_FALSE(tracking_timing_pause(timing, 99));
  CHECK_FALSE(tracking_timing_resume(timing, 99));
  CHECK_FALSE(tracking_timing_record_accepted(timing, TrackingState::Recording, 99));
  check_same_timing(timing, previous);

  REQUIRE(tracking_timing_pause(timing, 120));
  REQUIRE(tracking_timing_resume(timing, 150));
  const auto resumed = timing;
  CHECK(tracking_recording_s(timing, 149) == TRACKING_TIME_INVALID_S);
  CHECK_FALSE(tracking_timing_pause(timing, 149));
  check_same_timing(timing, resumed);
}

TEST_CASE("tracking timing rejects inconsistent accumulated duration", "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 100));

  SECTION("Unknown accumulator") { timing.recording_accumulated_s = TRACKING_TIME_INVALID_S; }
  SECTION("Segment predates session") { timing.recording_started_s = 99; }
  SECTION("Segment is in the future") { timing.recording_started_s = 201; }
  SECTION("Accumulated duration overlaps the active segment") {
    timing.recording_started_s = 150;
    timing.recording_accumulated_s = 51;
  }
  SECTION("Paused duration exceeds session elapsed") {
    timing.recording_started_s = TRACKING_TIME_INVALID_S;
    timing.recording_accumulated_s = 101;
  }
  SECTION("Corrupt accumulator would overflow with the live segment") {
    timing.recording_accumulated_s = MAX_TIME_S;
  }

  const auto previous = timing;
  CHECK(tracking_recording_s(timing, 200) == TRACKING_TIME_INVALID_S);
  CHECK_FALSE(tracking_timing_pause(timing, 200));
  CHECK_FALSE(tracking_timing_resume(timing, 200));
  check_same_timing(timing, previous);
  CHECK(tracking_elapsed_s(timing, 200) == 100); // Independent field remains usable.
}

TEST_CASE("tracking last-record validation is independent of duration fields",
          "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 100));
  SECTION("No point") { timing.last_record_s = TRACKING_TIME_INVALID_S; }
  SECTION("Point predates session") { timing.last_record_s = 99; }
  SECTION("Point is in the future") { timing.last_record_s = 201; }

  CHECK(tracking_last_record_age_s(timing, 200) == TRACKING_TIME_INVALID_S);
  CHECK(tracking_elapsed_s(timing, 200) == 100);
  CHECK(tracking_recording_s(timing, 200) == 100);
}

TEST_CASE("tracking timing supports the last representable second without wrapping",
          "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 0));
  CHECK(tracking_elapsed_s(timing, MAX_TIME_S) == MAX_TIME_S);
  CHECK(tracking_recording_s(timing, MAX_TIME_S) == MAX_TIME_S);
  REQUIRE(tracking_timing_pause(timing, MAX_TIME_S - 10));
  REQUIRE(tracking_timing_resume(timing, MAX_TIME_S - 5));
  CHECK(tracking_recording_s(timing, MAX_TIME_S) == MAX_TIME_S - 5);
  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, MAX_TIME_S));
  CHECK(tracking_last_record_age_s(timing, MAX_TIME_S) == 0);
  REQUIRE(tracking_timing_pause(timing, MAX_TIME_S));
  CHECK(timing.recording_accumulated_s == MAX_TIME_S - 5);

  const auto previous = timing;
  CHECK_FALSE(tracking_timing_resume(timing, TRACKING_TIME_INVALID_S));
  check_same_timing(timing, previous);
}

TEST_CASE("tracking timing snapshots do not mutate the timing value", "[go][tracking][timing]") {
  TrackingTiming timing;
  REQUIRE(tracking_timing_start(timing, 10));
  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 20));
  const auto previous = timing;
  CHECK(tracking_elapsed_s(timing, 30) == 20);
  CHECK(tracking_recording_s(timing, 30) == 20);
  CHECK(tracking_last_record_age_s(timing, 30) == 10);
  check_same_timing(timing, previous);
}

TEST_CASE("tracking last-record time can recover independently of active duration",
          "[go][tracking][timing]") {
  TrackingTiming timing{100, TRACKING_TIME_INVALID_S, TRACKING_TIME_INVALID_S, 110};
  REQUIRE(tracking_timing_record_accepted(timing, TrackingState::Recording, 160));
  CHECK(tracking_elapsed_s(timing, 170) == 70);
  CHECK(tracking_recording_s(timing, 170) == TRACKING_TIME_INVALID_S);
  CHECK(tracking_last_record_age_s(timing, 170) == 10);
}

TEST_CASE("tracking timing restoration follows the authoritative state", "[go][tracking][timing]") {
  const TrackingTiming recording{100, 20, 140, 150};
  const TrackingTiming paused{100, 30, TRACKING_TIME_INVALID_S, 120};
  check_same_timing(tracking_timing_for_state(recording, TrackingState::Recording), recording);
  check_same_timing(tracking_timing_for_state(paused, TrackingState::Paused), paused);
  check_same_timing(tracking_timing_for_state(recording, TrackingState::Paused),
                    {100, TRACKING_TIME_INVALID_S, TRACKING_TIME_INVALID_S, 150});
  check_same_timing(tracking_timing_for_state(paused, TrackingState::Recording),
                    {100, TRACKING_TIME_INVALID_S, TRACKING_TIME_INVALID_S, 120});
  check_same_timing(tracking_timing_for_state(recording, TrackingState::Idle), TrackingTiming{});
  check_same_timing(tracking_timing_for_state(recording, static_cast<TrackingState>(255)),
                    TrackingTiming{});
}
