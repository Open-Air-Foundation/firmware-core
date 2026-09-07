/**
 * AirGradient Go — go_airflow_detect host tests
 *
 * Exercises the jitter-based airflow detector with synthetic series and with
 * golden sample sequences taken verbatim from the 2026-08-28 validation log
 * (~/ntc_comp_test.log): one still-air stretch and one direct-fan stretch,
 * both at the real 10 s cadence.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

#include "go_airflow_detect.h"

namespace {

constexpr float START_S = 100.0f;
constexpr float CADENCE_S = 10.0f;

float feed(airflow_detect::Detector &det, const float *vals, size_t n, float t0 = START_S) {
  float t = t0;
  for (size_t i = 0; i < n; ++i) {
    det.add_sample(vals[i], t);
    t += CADENCE_S;
  }
  return t;
}

// Still air, idle, minutes 170–176 of the validation log.
// Window jitters (reference analysis): 13 / 20 / 9 mK.
constexpr float STILL_AIR[] = {29.44f, 29.48f, 29.48f, 29.48f, 29.49f, 29.51f, 29.51f, 29.51f,
                               29.51f, 29.53f, 29.53f, 29.55f, 29.52f, 29.55f, 29.55f, 29.56f,
                               29.56f, 29.57f, 29.60f, 29.57f, 29.58f, 29.56f, 29.57f, 29.57f,
                               29.58f, 29.57f, 29.56f, 29.56f, 29.57f, 29.57f, 29.57f, 29.56f,
                               29.56f, 29.58f, 29.57f, 29.55f, 29.56f, 29.57f};

// Direct desk fan, charging, minutes 116–122 of the validation log.
// Window jitters (reference analysis): 42 / 61 / 47 mK.
constexpr float FAN[] = {26.93f, 26.91f, 26.96f, 26.94f, 26.94f, 26.95f, 26.93f, 26.96f,
                         27.03f, 27.03f, 27.03f, 26.93f, 26.91f, 26.98f, 26.91f, 26.86f,
                         26.84f, 26.77f, 26.79f, 26.81f, 26.68f, 26.56f, 26.59f, 26.52f,
                         26.58f, 26.52f, 26.48f, 26.53f, 26.56f, 26.62f, 26.64f, 26.68f,
                         26.73f, 26.77f, 26.81f, 26.82f, 26.83f, 26.83f};

} // namespace

TEST_CASE("no window closes before enough diffs accumulate") {
  airflow_detect::Detector det;
  const float flat[airflow_detect::WINDOW_DIFFS] = {}; // 11 samples = 10 diffs
  feed(det, flat, airflow_detect::WINDOW_DIFFS);
  CHECK(det.last_window_jitter_mk() < 0.0f);
  CHECK_FALSE(det.airflow());
}

TEST_CASE("constant series reads as still air") {
  airflow_detect::Detector det;
  float t = START_S;
  for (int i = 0; i < 40; ++i) {
    det.add_sample(25.0f, t);
    t += CADENCE_S;
  }
  CHECK(det.last_window_jitter_mk() >= 0.0f);
  CHECK(det.last_window_jitter_mk() < 1.0f);
  CHECK_FALSE(det.airflow());
}

TEST_CASE("sustained fluctuation raises the flag, sustained quiet clears it") {
  airflow_detect::Detector det;
  float t = START_S;
  // Alternating +/-0.05 °C -> diffs of +/-0.1 °C -> jitter ~100 mK.
  const int loud_samples = 1 + airflow_detect::WINDOW_DIFFS * airflow_detect::ENTER_WINDOWS;
  for (int i = 0; i < loud_samples; ++i) {
    det.add_sample(25.0f + ((i % 2 != 0) ? 0.05f : -0.05f), t);
    t += CADENCE_S;
  }
  CHECK(det.airflow());
  CHECK(det.last_window_jitter_mk() >= airflow_detect::ENTER_JITTER_MK);

  // One quiet window is not enough to clear...
  for (int i = 0; i < airflow_detect::WINDOW_DIFFS; ++i) {
    det.add_sample(25.0f, t);
    t += CADENCE_S;
  }
  CHECK(det.airflow());

  // ...but the full exit dwell is.
  for (int i = 0; i < airflow_detect::WINDOW_DIFFS * (airflow_detect::EXIT_WINDOWS - 1); ++i) {
    det.add_sample(25.0f, t);
    t += CADENCE_S;
  }
  CHECK_FALSE(det.airflow());
}

TEST_CASE("ambiguous windows hold the exit streak, loud windows reset it") {
  auto raise_flag = [](airflow_detect::Detector &det, float &t) {
    const int n = 1 + airflow_detect::WINDOW_DIFFS * airflow_detect::ENTER_WINDOWS;
    for (int i = 0; i < n; ++i) {
      det.add_sample(25.0f + ((i % 2 != 0) ? 0.05f : -0.05f), t);
      t += CADENCE_S;
    }
    REQUIRE(det.airflow());
  };
  auto feed_windows = [](airflow_detect::Detector &det, float &t, int windows, float half_step) {
    for (int i = 0; i < airflow_detect::WINDOW_DIFFS * windows; ++i) {
      det.add_sample(25.0f + ((i % 2 != 0) ? half_step : -half_step), t);
      t += CADENCE_S;
    }
  };

  SECTION("a mid-band window between quiet ones does not restart the dwell") {
    airflow_detect::Detector det;
    float t = START_S;
    raise_flag(det, t);
    feed_windows(det, t, 3, 0.0f);   // 3 quiet windows (~0 mK)
    feed_windows(det, t, 1, 0.013f); // ~26 mK: ambiguous, holds
    CHECK(det.airflow());
    feed_windows(det, t, 2, 0.0f); // 2 more quiet -> 5 total
    CHECK_FALSE(det.airflow());
  }

  SECTION("a loud window restarts the dwell") {
    airflow_detect::Detector det;
    float t = START_S;
    raise_flag(det, t);
    feed_windows(det, t, 4, 0.0f);
    feed_windows(det, t, 1, 0.05f); // ~100 mK: wind is back, reset
    feed_windows(det, t, 4, 0.0f);  // only 4 quiet since the reset
    CHECK(det.airflow());
  }
}

TEST_CASE("a single loud window between quiet ones never raises the flag") {
  airflow_detect::Detector det;
  float t = START_S;
  for (int i = 0; i < airflow_detect::WINDOW_DIFFS + 1; ++i) {
    det.add_sample(25.0f, t);
    t += CADENCE_S;
  }
  for (int i = 0; i < airflow_detect::WINDOW_DIFFS; ++i) {
    det.add_sample(25.0f + ((i % 2 != 0) ? 0.05f : -0.05f), t);
    t += CADENCE_S;
  }
  for (int i = 0; i < airflow_detect::WINDOW_DIFFS; ++i) {
    det.add_sample(25.0f, t);
    t += CADENCE_S;
  }
  CHECK_FALSE(det.airflow());
}

TEST_CASE("a diff spanning an irregular gap is dropped") {
  airflow_detect::Detector det;
  float t = START_S;
  for (int i = 0; i < 6; ++i) {
    det.add_sample(25.0f, t);
    t += CADENCE_S;
  }
  // Sensor outage: the next sample lands 60 s later and 5 °C away. The pair
  // must not enter the statistics.
  t += 50.0f;
  det.add_sample(30.0f, t);
  t += CADENCE_S;
  for (int i = 0; i < 12; ++i) {
    det.add_sample(30.0f, t);
    t += CADENCE_S;
  }
  CHECK(det.last_window_jitter_mk() >= 0.0f);
  CHECK(det.last_window_jitter_mk() < 1.0f);
  CHECK_FALSE(det.airflow());
}

TEST_CASE("golden still-air stretch stays flagless") {
  airflow_detect::Detector det;
  feed(det, STILL_AIR, sizeof(STILL_AIR) / sizeof(STILL_AIR[0]));
  CHECK_FALSE(det.airflow());
  // Last completed window of this stretch measured ~9 mK in the reference
  // analysis; allow slack for float accumulation.
  CHECK(det.last_window_jitter_mk() >= 0.0f);
  CHECK(det.last_window_jitter_mk() < airflow_detect::EXIT_JITTER_MK);
}

TEST_CASE("golden fan stretch raises the flag") {
  airflow_detect::Detector det;
  feed(det, FAN, sizeof(FAN) / sizeof(FAN[0]));
  CHECK(det.airflow());
  // Windows measured 42 / 61 / 47 mK in the reference analysis.
  CHECK(det.last_window_jitter_mk() >= airflow_detect::ENTER_JITTER_MK);
}
