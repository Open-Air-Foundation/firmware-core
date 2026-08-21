/**
 * AirGradient Go — CloudService unit tests
 *
 * Drives the cloud task body deterministically via the friend
 * CloudServiceTestAccess helper.  The real RTOS task / queue plumbing is
 * a no-op under TEST_HOST, so all tests work against the synchronous
 * `_run_iteration(now)` driver and clock injection.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>

#include "go_cloud.h"
#include "go_config_types.h"
#include "go_events.h"
#include "go_wifi.h"
#include "retained_uptime.h"
#include "rtos.h"
#include "services/ag_client.h"

// ============================================================================
// External cloud_spy state (defined in go_cloud_stubs.cpp)
// ============================================================================

namespace cloud_spy {
extern uint32_t post_call_count;
extern uint32_t fetch_call_count;
extern MeasuresAGo last_post_snapshot;
extern int last_post_signal;
extern uint32_t last_post_boot;
extern char *last_fetch_buf;
extern size_t last_fetch_buf_size;
extern AgClientResult next_post_result;
extern AgClientResult next_fetch_result;
extern size_t fetch_bytes_to_write;
extern const char *fetch_body_to_write;
extern void (*on_post_hook)();
extern void (*on_fetch_hook)();
extern int wifi_rssi;
extern void reset();
} // namespace cloud_spy

// ============================================================================
// CloudServiceTestAccess — friend helper exposing private internals
// ============================================================================

class CloudServiceTestAccess {
public:
  static uint32_t run_once(CloudService &c, uint32_t now) { return c._run_iteration(now); }

  static uint32_t post_due(const CloudService &c) { return c._post_due; }
  static uint32_t fetch_due(const CloudService &c) { return c._fetch_due.load(); }
  static bool was_armed(const CloudService &c) { return c._was_armed; }
  static uint32_t done_signal_count(const CloudService &c) { return c._test_done_signal_count; }
  static bool config_fetch_enabled(const CloudService &c) { return c._config_fetch_enabled.load(); }

  static void set_armed(CloudService &c, bool v) { c._armed.store(v); }
  static void set_disable_cloud(CloudService &c, bool v) { c._disable_cloud.store(v); }
  static void set_fire_now_pending(CloudService &c, bool v) { c._fire_now_pending.store(v); }
  static void set_shutdown_pending(CloudService &c, bool v) { c._shutdown_pending.store(v); }

  static void set_post_due(CloudService &c, uint32_t v) { c._post_due = v; }
  static void set_fetch_due(CloudService &c, uint32_t v) { c._fetch_due.store(v); }
  static void set_was_armed(CloudService &c, bool v) { c._was_armed = v; }

  // Inject the fetch buffer so _do_fetch has a place to write into.
  // The real start() allocates this; tests do not call start() because
  // task_create / queue_create / semaphore_create are no-ops under
  // TEST_HOST.
  static void set_fetch_buf(CloudService &c, char *buf) { c._fetch_buf = buf; }

  // Direct snapshot read for the snapshot test — exercises the same
  // mutex-protected accessor the task uses.
  static MeasuresAGo snapshot_copy(CloudService &c) { return c._snapshot_copy(); }

  // Create the done-semaphore so the shutdown branch's _done_sem.give()
  // does not hit an uncreated handle.  is_created() == true under
  // TEST_HOST returns the sentinel handle.
  static void create_done_sem(CloudService &c) { c._done_sem.create(); }
};

using A = CloudServiceTestAccess;

// ============================================================================
// MockRTOS — controls the clock and records task_notify_wait calls
// ============================================================================

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);

  uint64_t get_retained_time_ms_impl() override { return retained_time_ms; }

  // Capture the last timeout passed to task_notify_take().
  uint32_t last_notify_take_ms = UINT32_MAX;
  uint32_t notify_take_calls = 0;

  bool task_notify_wait_impl(uint32_t * /*out_value*/, uint32_t timeout_ms) override {
    last_notify_take_ms = timeout_ms;
    notify_take_calls += 1;
    return false; // simulate timeout — tests drive the loop manually
  }

  bool queue_send_impl(RtosQueueHandle /*qh*/, const void *item, uint32_t /*timeout_ms*/) override {
    if (item != nullptr) {
      last_event = *static_cast<const Event *>(item);
      events_posted += 1;
      return true;
    }
    return false;
  }

  Event last_event{};
  uint32_t events_posted = 0;
  uint64_t retained_time_ms = 0;
};

// ============================================================================
// Fixture
// ============================================================================

struct CloudFixture {
  MockRTOS mock_rtos;

  AgClient ag_client;
  WifiService wifi_service;
  CloudService cloud;

  // Persistent buffer for fetch_config so _do_fetch has somewhere to
  // write into.  Sized to match FETCH_BUFFER_BYTES.
  static constexpr size_t FETCH_BUF_SIZE = 2048;
  char fetch_buf[FETCH_BUF_SIZE] = {};

  std::unique_ptr<trompeloeil::expectation> _exp_time;
  std::unique_ptr<trompeloeil::expectation> _exp_delay;

  CloudFixture()
      : wifi_service(nullptr,
                     {*reinterpret_cast<WifiManager *>(_stub_buf),
                      *reinterpret_cast<AgBleServer *>(_stub_buf),
                      *reinterpret_cast<HttpServer *>(_stub_buf),
                      *reinterpret_cast<LocalServer *>(_stub_buf)},
                     WifiService::Config{}),
        cloud(reinterpret_cast<RtosQueueHandle>(0x1), CloudService::Deps{ag_client, wifi_service},
              CloudService::Config{}) {
    cloud_spy::reset();
    RTOS::set_instance(&mock_rtos);
    retained_uptime::reset_state_for_test();
    retained_uptime::init();
    _exp_time = NAMED_ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    _exp_delay = NAMED_ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Plant a fetch buffer so _do_fetch has somewhere to write.
    A::set_fetch_buf(cloud, fetch_buf);

    // Done semaphore must exist so the shutdown branch's give() finds
    // a created handle.  In TEST_HOST this is a non-null sentinel.
    A::create_done_sem(cloud);
  }

  ~CloudFixture() {
    // Detach the buffer so ~CloudService -> stop() does not free our
    // stack array.
    A::set_fetch_buf(cloud, nullptr);
    retained_uptime::reset_state_for_test();
    RTOS::set_instance(nullptr);
  }

private:
  alignas(8) static inline char _stub_buf[64];
};

// ============================================================================
// 1. Transition handling
// ============================================================================

TEST_CASE("Disarmed -> Armed sets both deadlines one interval out", "[CloudService][transition]") {
  CloudFixture f;
  REQUIRE(A::config_fetch_enabled(f.cloud));
  A::set_armed(f.cloud, true);
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  REQUIRE(A::was_armed(f.cloud));
  REQUIRE(A::post_due(f.cloud) == 1000 + 60'000);
  REQUIRE(A::fetch_due(f.cloud) == 1000 + 60'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
  REQUIRE(wake == 60'000);
}

TEST_CASE("Disarmed -> Armed with fire_now fires POST immediately and re-anchors",
          "[CloudService][transition]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_fire_now_pending(f.cloud, true);

  // First iteration: transition snaps both deadlines to now=1000, then
  // the POST-priority branch fires POST immediately because the
  // deadline equals now.  POST re-anchors _post_due = 1000 + 60000.
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(A::post_due(f.cloud) == 1000 + 60'000);
  REQUIRE(A::fetch_due(f.cloud) == 1000); // FETCH not yet run
  REQUIRE(wake == 0);

  // Second iteration: FETCH fires (still due at now), re-anchors to
  // 1001 + 60000.
  A::run_once(f.cloud, /*now=*/1001);
  REQUIRE(cloud_spy::fetch_call_count == 1);
  REQUIRE(A::fetch_due(f.cloud) == 1001 + 60'000);
}

TEST_CASE("fire_now while config Fetch is disabled makes only POST immediate",
          "[CloudService][transition][fetch_gate]") {
  CloudFixture f;
  f.cloud.set_config_fetch_enabled(false);
  A::set_armed(f.cloud, true);
  A::set_fire_now_pending(f.cloud, true);

  REQUIRE(A::run_once(f.cloud, 1000) == 0);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(cloud_spy::fetch_call_count == 0);
  REQUIRE(A::post_due(f.cloud) == 1000 + 60'000);

  const uint32_t wake = A::run_once(f.cloud, 1001);
  REQUIRE(cloud_spy::fetch_call_count == 0);
  REQUIRE(wake == 59'999);
}

TEST_CASE("config Fetch re-enabled while disarmed follows the next arm schedule",
          "[CloudService][transition][fetch_gate]") {
  CloudFixture f;
  f.cloud.set_config_fetch_enabled(false);
  REQUIRE(A::run_once(f.cloud, 500) == UINT32_MAX);

  f.cloud.set_config_fetch_enabled(true);
  REQUIRE(A::run_once(f.cloud, 750) == UINT32_MAX);

  f.cloud.arm(/*fire_now=*/false);
  REQUIRE(A::run_once(f.cloud, 1000) == 60'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
  REQUIRE(A::fetch_due(f.cloud) == 61'000);
}

TEST_CASE("config Fetch re-enable and arm may coalesce before the task runs",
          "[CloudService][transition][fetch_gate]") {
  CloudFixture f;
  f.cloud.set_config_fetch_enabled(false);
  f.cloud.set_config_fetch_enabled(true);
  f.cloud.arm(/*fire_now=*/false);

  REQUIRE(A::run_once(f.cloud, 1000) == 60'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
  REQUIRE(A::fetch_due(f.cloud) == 61'000);
}

TEST_CASE("Arm-while-armed does not reset deadlines", "[CloudService][transition]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::run_once(f.cloud, /*now=*/1000);
  const uint32_t post_before = A::post_due(f.cloud);
  const uint32_t fetch_before = A::fetch_due(f.cloud);

  // arm() again with fire_now=false: no transition observed.
  f.cloud.arm(/*fire_now=*/false);
  A::run_once(f.cloud, /*now=*/2000);

  REQUIRE(A::post_due(f.cloud) == post_before);
  REQUIRE(A::fetch_due(f.cloud) == fetch_before);
}

// ============================================================================
// 2. Null-handle safety
// ============================================================================

TEST_CASE("Cloud state setters are safe before start()", "[CloudService][null_handle]") {
  CloudFixture f;

  // _task_handle is nullptr by default (we never called start()).  The
  // calls below must not crash; the atomics still take the new values.
  f.cloud.arm(true);
  f.cloud.disarm();
  f.cloud.set_disable_cloud(true);
  f.cloud.set_disable_cloud(false);
  f.cloud.set_config_fetch_enabled(false);
  REQUIRE_FALSE(A::config_fetch_enabled(f.cloud));
  f.cloud.set_config_fetch_enabled(true);
  REQUIRE(A::config_fetch_enabled(f.cloud));

  // Drive an iteration to confirm atomics propagated and no crash.
  uint32_t wake = A::run_once(f.cloud, /*now=*/0);
  (void)wake;
  SUCCEED();
}

// ============================================================================
// 3. Snapshot read/write
// ============================================================================

TEST_CASE("update_measures_snapshot round-trips under the mutex", "[CloudService][snapshot]") {
  CloudFixture f;

  MeasuresAGo a{};
  a.co2.co2 = 1234;
  a.temp_hum_a.temperature = 25.5f;
  f.cloud.update_measures_snapshot(a);

  MeasuresAGo back = A::snapshot_copy(f.cloud);
  REQUIRE(back.co2.co2 == 1234);
  REQUIRE(back.temp_hum_a.temperature == 25.5f);

  MeasuresAGo b{};
  b.co2.co2 = 555;
  f.cloud.update_measures_snapshot(b);
  back = A::snapshot_copy(f.cloud);
  REQUIRE(back.co2.co2 == 555);
}

// ============================================================================
// 4. POST and FETCH forwarding to event queue
// ============================================================================

TEST_CASE("POST forwards AgClientResult into PostMeasuresResult event", "[CloudService][post]") {
  CloudFixture f;
  cloud_spy::next_post_result = AgClientResult::ServerError;

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0); // due immediately
  A::set_fetch_due(f.cloud, 999'999'999);

  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(f.mock_rtos.events_posted == 1);
  REQUIRE(f.mock_rtos.last_event.type == EventType::PostMeasuresResult);
  REQUIRE(f.mock_rtos.last_event.cloud_result == static_cast<uint8_t>(AgClientResult::ServerError));
  REQUIRE(wake == 0); // continue immediately to re-sample
}

TEST_CASE("FETCH forwards AgClientResult into FetchConfigResult event", "[CloudService][fetch]") {
  CloudFixture f;
  cloud_spy::next_fetch_result = AgClientResult::Ok;
  cloud_spy::fetch_body_to_write = "{\"key\":\"val\"}";
  cloud_spy::fetch_bytes_to_write = 13;

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);

  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  REQUIRE(cloud_spy::fetch_call_count == 1);
  REQUIRE(cloud_spy::last_fetch_buf == f.fetch_buf);
  REQUIRE(cloud_spy::last_fetch_buf_size == CloudFixture::FETCH_BUF_SIZE);
  REQUIRE(f.mock_rtos.events_posted == 1);
  REQUIRE(f.mock_rtos.last_event.type == EventType::FetchConfigResult);
  REQUIRE(f.mock_rtos.last_event.fetch_config.result == static_cast<uint8_t>(AgClientResult::Ok));
  REQUIRE(f.mock_rtos.last_event.fetch_config.update.update_mask == 0);
  REQUIRE_FALSE(f.mock_rtos.last_event.fetch_config.co2_calibration_requested);
  REQUIRE_FALSE(f.mock_rtos.last_event.fetch_config.led_test_requested);
  REQUIRE_FALSE(f.mock_rtos.last_event.fetch_config.gps_test_requested);
  REQUIRE(wake == 0);
}

TEST_CASE("FETCH parses supported corrections independently", "[CloudService][fetch][correction]") {
  CloudFixture f;
  const char body[] =
      R"({"country":"DE","corrections":{"pm02":{"correctionAlgorithm":"custom_via_pm25_raw","slr":{"intercept":0,"scalingFactorViaPm25":1.08}},"atmp":{"correctionAlgorithm":"custom","slr":{"intercept":-0.4,"scalingFactor":1}},"rhum":{"correctionAlgorithm":"none","slr":null}}})";
  cloud_spy::fetch_body_to_write = body;
  cloud_spy::fetch_bytes_to_write = std::strlen(body);

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);

  const FetchConfigEventPayload &payload = f.mock_rtos.last_event.fetch_config;
  REQUIRE(payload.result == static_cast<uint8_t>(AgClientResult::Ok));
  REQUIRE(has_go_config_field(payload.update.update_mask, GoConfigField::Pm25Correction));
  REQUIRE(has_go_config_field(payload.update.update_mask, GoConfigField::TemperatureCorrection));
  REQUIRE(has_go_config_field(payload.update.update_mask, GoConfigField::HumidityCorrection));
  REQUIRE(payload.update.corrections.pm25.algorithm == Pm25CorrectionAlgorithm::CustomViaPm25Raw);
  REQUIRE(payload.update.corrections.pm25.scaling_factor == 1.08f);
  REQUIRE_FALSE(payload.update.corrections.pm25.use_epa2021);
  REQUIRE(payload.update.corrections.temperature.intercept == -0.4f);
  REQUIRE(payload.update.corrections.humidity.algorithm == LinearCorrectionAlgorithm::None);
}

TEST_CASE("FETCH ignores the retired Go custom PM EPA flag", "[CloudService][fetch][correction]") {
  CloudFixture f;
  const char body[] =
      R"({"corrections":{"pm02":{"correctionAlgorithm":"custom_via_pm25_raw","slr":{"intercept":0,"scalingFactorViaPm25":1.08,"useEpa2021":"ignored"}}}})";
  cloud_spy::fetch_body_to_write = body;
  cloud_spy::fetch_bytes_to_write = std::strlen(body);

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);

  const FetchConfigEventPayload &payload = f.mock_rtos.last_event.fetch_config;
  REQUIRE(has_go_config_field(payload.update.update_mask, GoConfigField::Pm25Correction));
  CHECK(payload.update.corrections.pm25.algorithm == Pm25CorrectionAlgorithm::CustomViaPm25Raw);
  CHECK_FALSE(payload.update.corrections.pm25.use_epa2021);
}

TEST_CASE("FETCH rejects one malformed correction but keeps valid siblings",
          "[CloudService][fetch][correction]") {
  CloudFixture f;
  const char body[] =
      R"({"corrections":{"pm02":{"correctionAlgorithm":"slr_PMS5003_20231030"},"atmp":{"correctionAlgorithm":"custom","slr":{"intercept":2,"scalingFactor":1}},"rhum":{"correctionAlgorithm":"none"}}})";
  cloud_spy::fetch_body_to_write = body;
  cloud_spy::fetch_bytes_to_write = std::strlen(body);

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);

  const GoConfigUpdate &update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::Pm25Correction));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::TemperatureCorrection));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::HumidityCorrection));
}

TEST_CASE("FETCH rejects wrong aliases, malformed roots, and trailing data",
          "[CloudService][fetch][correction]") {
  SECTION("wrong PM alias") {
    CloudFixture f;
    const char body[] =
        R"({"corrections":{"pm02":{"correctionAlgorithm":"custom_via_pm25_raw","slr":{"intercept":0,"scalingFactor":1,"useEpa2021":false}}}}})";
    cloud_spy::fetch_body_to_write = body;
    cloud_spy::fetch_bytes_to_write = std::strlen(body);
    A::set_armed(f.cloud, true);
    A::set_was_armed(f.cloud, true);
    A::set_post_due(f.cloud, 999'999'999);
    A::set_fetch_due(f.cloud, 0);
    A::run_once(f.cloud, 1000);
    REQUIRE(f.mock_rtos.last_event.fetch_config.update.update_mask == 0);
  }

  SECTION("trailing non-whitespace") {
    CloudFixture f;
    const char body[] = R"({"corrections":{}} trailing)";
    cloud_spy::fetch_body_to_write = body;
    cloud_spy::fetch_bytes_to_write = std::strlen(body);
    A::set_armed(f.cloud, true);
    A::set_was_armed(f.cloud, true);
    A::set_post_due(f.cloud, 999'999'999);
    A::set_fetch_due(f.cloud, 0);
    A::run_once(f.cloud, 1000);
    REQUIRE(f.mock_rtos.last_event.fetch_config.update.update_mask == 0);
  }
}

TEST_CASE("FETCH parses supported root scalars and ignores local-only fields",
          "[CloudService][fetch][config]") {
  CloudFixture f;
  const char body[] =
      R"({"pmStandard":"us-aqi","temperatureUnit":"f","altitudeUnit":"ft","measurementInterval":3600,"gpsMode":"always","frontLedBrightness":0,"backLedBrightness":3,"touchLedIntensity":2,"buzzerEnabled":true,"co2CalibrationRequested":true,"ledTestRequested":true,"gpsTestRequested":true,"disableCloudConnection":true,"configurationControl":"local","corrections":[]})";
  cloud_spy::fetch_body_to_write = body;
  cloud_spy::fetch_bytes_to_write = std::strlen(body);

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);

  const GoConfigUpdate &update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::PmStandard));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::TemperatureUnit));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::AltitudeUnit));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::MeasurementInterval));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::GpsMode));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::FrontLedBrightness));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::BackLedBrightness));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::TouchLedIntensity));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::BuzzerEnabled));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::CloudConnection));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::ConfigurationControl));
  REQUIRE(update.pm_use_usaqi);
  REQUIRE(update.use_fahrenheit);
  REQUIRE_FALSE(update.use_feet);
  REQUIRE(update.measure_interval_seconds == MEASURE_INTERVAL_SECONDS_MAX);
  REQUIRE(update.gps_mode == GpsMode::AlwaysOn);
  REQUIRE(update.front_led_brightness == LedBrightness::Off);
  REQUIRE(update.back_led_brightness == LedBrightness::Bright);
  REQUIRE(update.touch_led_intensity == TouchLedIntensity::Bright);
  REQUIRE(update.buzzer_enabled);
  REQUIRE(f.mock_rtos.last_event.fetch_config.co2_calibration_requested);
  REQUIRE(f.mock_rtos.last_event.fetch_config.led_test_requested);
  REQUIRE(f.mock_rtos.last_event.fetch_config.gps_test_requested);
  REQUIRE_FALSE(update.disable_cloud);
  REQUIRE(update.configuration_control == ConfigurationControl::Both);
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::Pm25Correction));

  const char cleared_body[] =
      R"({"co2CalibrationRequested":false,"ledTestRequested":false,"gpsTestRequested":false})";
  cloud_spy::fetch_body_to_write = cleared_body;
  cloud_spy::fetch_bytes_to_write = std::strlen(cleared_body);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1500);
  REQUIRE_FALSE(f.mock_rtos.last_event.fetch_config.co2_calibration_requested);
  REQUIRE_FALSE(f.mock_rtos.last_event.fetch_config.led_test_requested);
  REQUIRE_FALSE(f.mock_rtos.last_event.fetch_config.gps_test_requested);
}

TEST_CASE("FETCH rejects malformed device settings independently",
          "[CloudService][fetch][config]") {
  CloudFixture f;
  const char body[] =
      R"({"temperatureUnit":"c","measurementInterval":0,"gpsMode":"ALWAYS","frontLedBrightness":4,"backLedBrightness":-1,"touchLedIntensity":3,"buzzerEnabled":"true","co2CalibrationRequested":"true","ledTestRequested":1,"gpsTestRequested":"true"})";
  cloud_spy::fetch_body_to_write = body;
  cloud_spy::fetch_bytes_to_write = std::strlen(body);

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);

  const GoConfigUpdate &update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::TemperatureUnit));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::MeasurementInterval));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::GpsMode));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::FrontLedBrightness));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::BackLedBrightness));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::TouchLedIntensity));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::BuzzerEnabled));
  REQUIRE_FALSE(f.mock_rtos.last_event.fetch_config.co2_calibration_requested);
  REQUIRE_FALSE(f.mock_rtos.last_event.fetch_config.led_test_requested);
  REQUIRE_FALSE(f.mock_rtos.last_event.fetch_config.gps_test_requested);
}

TEST_CASE("FETCH parses valid ABC days and rejects malformed values independently",
          "[CloudService][fetch][config]") {
  CloudFixture f;
  const char body[] = R"({"abcDays":200,"temperatureUnit":"f"})";
  cloud_spy::fetch_body_to_write = body;
  cloud_spy::fetch_bytes_to_write = std::strlen(body);

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);

  const GoConfigUpdate &update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::Co2AbcDays));
  REQUIRE(update.co2_abc_days == 200);
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::TemperatureUnit));

  const char disabled_body[] = R"({"abcDays":-1})";
  cloud_spy::fetch_body_to_write = disabled_body;
  cloud_spy::fetch_bytes_to_write = std::strlen(disabled_body);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1500);
  const GoConfigUpdate &disabled_update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE(has_go_config_field(disabled_update.update_mask, GoConfigField::Co2AbcDays));
  REQUIRE(disabled_update.co2_abc_days == CO2_ABC_DAYS_DISABLED);

  const char invalid_body[] = R"({"abcDays":7.5,"temperatureUnit":"c"})";
  cloud_spy::fetch_body_to_write = invalid_body;
  cloud_spy::fetch_bytes_to_write = std::strlen(invalid_body);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 2000);
  const GoConfigUpdate &invalid_update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE_FALSE(has_go_config_field(invalid_update.update_mask, GoConfigField::Co2AbcDays));
  REQUIRE(has_go_config_field(invalid_update.update_mask, GoConfigField::TemperatureUnit));
}

TEST_CASE("FETCH parses TVOC and NOx learning offsets independently",
          "[CloudService][fetch][config]") {
  CloudFixture f;
  const char body[] = R"({"tvocLearningOffset":1,"noxLearningOffset":1000})";
  cloud_spy::fetch_body_to_write = body;
  cloud_spy::fetch_bytes_to_write = std::strlen(body);

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);

  const GoConfigUpdate &update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::TvocLearningOffset));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::NoxLearningOffset));
  REQUIRE(update.tvoc_learning_offset == LEARNING_OFFSET_HOURS_MIN);
  REQUIRE(update.nox_learning_offset == LEARNING_OFFSET_HOURS_MAX);

  const char invalid_body[] = R"({"tvocLearningOffset":12.5,"noxLearningOffset":1001})";
  cloud_spy::fetch_body_to_write = invalid_body;
  cloud_spy::fetch_bytes_to_write = std::strlen(invalid_body);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1500);

  const GoConfigUpdate &invalid_update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE_FALSE(has_go_config_field(invalid_update.update_mask, GoConfigField::TvocLearningOffset));
  REQUIRE_FALSE(has_go_config_field(invalid_update.update_mask, GoConfigField::NoxLearningOffset));
}

TEST_CASE("FETCH ignores cloud policy fields when no supported field is present",
          "[CloudService][fetch][config]") {
  CloudFixture f;
  const char body[] = R"({"disableCloudConnection":true,"configurationControl":"local"})";
  cloud_spy::fetch_body_to_write = body;
  cloud_spy::fetch_bytes_to_write = std::strlen(body);

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);

  const GoConfigUpdate &update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE(update.update_mask == 0);
  REQUIRE_FALSE(update.disable_cloud);
  REQUIRE(update.configuration_control == ConfigurationControl::Both);
}

TEST_CASE("FETCH rejects malformed unsupported and case-mismatched scalars independently",
          "[CloudService][fetch][config]") {
  CloudFixture f;
  const char body[] =
      R"({"pmStandard":"metric","temperatureUnit":"f","disableCloudConnection":true,"configurationControl":"local","corrections":{"rhum":{"correctionAlgorithm":"none"}}})";
  cloud_spy::fetch_body_to_write = body;
  cloud_spy::fetch_bytes_to_write = std::strlen(body);

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);

  const GoConfigUpdate &update = f.mock_rtos.last_event.fetch_config.update;
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::PmStandard));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::TemperatureUnit));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::CloudConnection));
  REQUIRE_FALSE(has_go_config_field(update.update_mask, GoConfigField::ConfigurationControl));
  REQUIRE(has_go_config_field(update.update_mask, GoConfigField::HumidityCorrection));
  REQUIRE(update.use_fahrenheit);
}

TEST_CASE("FETCH allocation accepts 2047 bytes but not larger responses",
          "[CloudService][fetch][buffer]") {
  CloudFixture f;
  std::string body(2047, ' ');
  cloud_spy::fetch_body_to_write = body.data();
  cloud_spy::fetch_bytes_to_write = body.size();

  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 999'999'999);
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 1000);
  REQUIRE(cloud_spy::last_fetch_buf_size == 2048);
  REQUIRE(f.mock_rtos.last_event.fetch_config.result == static_cast<uint8_t>(AgClientResult::Ok));

  body.push_back('x');
  cloud_spy::next_fetch_result = AgClientResult::BufferTooSmall;
  cloud_spy::fetch_bytes_to_write = body.size();
  A::set_fetch_due(f.cloud, 0);
  A::run_once(f.cloud, 2000);
  REQUIRE(f.mock_rtos.last_event.fetch_config.result ==
          static_cast<uint8_t>(AgClientResult::BufferTooSmall));
  REQUIRE(f.mock_rtos.last_event.fetch_config.update.update_mask == 0);
}

TEST_CASE("Fetch event update is a trivially copyable value with no buffer pointer",
          "[CloudService][fetch][event]") {
  REQUIRE(std::is_trivially_copyable<GoConfigUpdate>::value);
  REQUIRE(std::is_trivially_copyable<FetchConfigEventPayload>::value);
}

// ============================================================================
// 5. POST priority
// ============================================================================

TEST_CASE("POST priority: coincident deadlines fire POST first", "[CloudService][priority]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 60'000);
  A::set_fetch_due(f.cloud, 60'000);

  // First iteration at t=60'000: both due, POST runs first.
  A::run_once(f.cloud, /*now=*/60'000);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(cloud_spy::fetch_call_count == 0);

  // Second iteration: POST already re-anchored, FETCH still due.
  A::run_once(f.cloud, /*now=*/60'001);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(cloud_spy::fetch_call_count == 1);
}

// ============================================================================
// 6. Start-time anchoring
// ============================================================================

TEST_CASE("POST deadline anchored to start time, not completion", "[CloudService][anchoring]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  // Drive POST at t=1000; the in-stub hook does nothing (call is "instant").
  A::run_once(f.cloud, /*now=*/1000);

  // Next deadline = post_started_at (1000) + 60'000, NOT completion time.
  REQUIRE(A::post_due(f.cloud) == 1000 + 60'000);
}

// ============================================================================
// 7. Overrun handling — POST priority preserved
// ============================================================================

TEST_CASE("Overrun POST fires next POST before FETCH on re-anchor", "[CloudService][overrun]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);

  // Simulate a >60 s POST: start at t=0, anchor to 60'000, but the
  // "current" clock the test passes to run_once for the next iteration
  // is 70'000 — _post_due (60'000) is already in the past.
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  A::run_once(f.cloud, /*now=*/0);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(A::post_due(f.cloud) == 60'000);

  // Next iteration at t=70'000 with fetch deadline still far in future.
  A::set_fetch_due(f.cloud, 80'000);
  A::run_once(f.cloud, /*now=*/70'000);

  // POST fires again, FETCH still not yet considered.
  REQUIRE(cloud_spy::post_call_count == 2);
  REQUIRE(cloud_spy::fetch_call_count == 0);
}

// ============================================================================
// 8. Disarm during POST gates FETCH out
// ============================================================================

static CloudService *s_disarm_target = nullptr;
static void disarm_during_post() {
  if (s_disarm_target != nullptr) {
    A::set_armed(*s_disarm_target, false);
  }
}

TEST_CASE("Disarm during POST gates FETCH out of the next iteration", "[CloudService][disarm]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 0);

  s_disarm_target = &f.cloud;
  cloud_spy::on_post_hook = disarm_during_post;

  A::run_once(f.cloud, /*now=*/1000);
  REQUIRE(cloud_spy::post_call_count == 1);

  // Next iteration: armed == false at the top, FETCH must NOT fire.
  A::run_once(f.cloud, /*now=*/1001);
  REQUIRE(cloud_spy::fetch_call_count == 0);

  s_disarm_target = nullptr;
  cloud_spy::on_post_hook = nullptr;
}

// ============================================================================
// 9. set_disable_cloud during POST gates FETCH
// ============================================================================

static CloudService *s_disable_target = nullptr;
static void disable_during_post() {
  if (s_disable_target != nullptr) {
    A::set_disable_cloud(*s_disable_target, true);
  }
}

TEST_CASE("set_disable_cloud during POST gates FETCH out of the next iteration",
          "[CloudService][disable]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 0);

  s_disable_target = &f.cloud;
  cloud_spy::on_post_hook = disable_during_post;

  A::run_once(f.cloud, /*now=*/1000);
  REQUIRE(cloud_spy::post_call_count == 1);

  A::run_once(f.cloud, /*now=*/1001);
  REQUIRE(cloud_spy::fetch_call_count == 0);

  s_disable_target = nullptr;
  cloud_spy::on_post_hook = nullptr;
}

TEST_CASE("Disabling config Fetch leaves measurement POST running and skips Fetch HTTP",
          "[CloudService][fetch_gate]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 1000);
  A::set_fetch_due(f.cloud, 1000);
  f.cloud.set_config_fetch_enabled(false);

  REQUIRE(A::run_once(f.cloud, 1000) == 0);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(cloud_spy::fetch_call_count == 0);

  REQUIRE(A::run_once(f.cloud, 1001) == 59'999);
  REQUIRE(cloud_spy::fetch_call_count == 0);
}

TEST_CASE("Enabling config Fetch makes Fetch immediately due without changing POST",
          "[CloudService][fetch_gate]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 100'000);
  A::set_fetch_due(f.cloud, 500);
  f.cloud.set_config_fetch_enabled(false);

  REQUIRE(A::run_once(f.cloud, 1000) == 99'000);
  REQUIRE(cloud_spy::fetch_call_count == 0);

  f.cloud.set_config_fetch_enabled(true);
  REQUIRE(A::run_once(f.cloud, 5000) == 0);
  REQUIRE(A::post_due(f.cloud) == 100'000);
  REQUIRE(A::fetch_due(f.cloud) == 65'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 1);
}

TEST_CASE("Rapid config Fetch disable and re-enable still fires immediately",
          "[CloudService][fetch_gate]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 100'000);
  A::set_fetch_due(f.cloud, 500);

  f.cloud.set_config_fetch_enabled(false);
  f.cloud.set_config_fetch_enabled(true);

  REQUIRE(A::run_once(f.cloud, 5000) == 0);
  REQUIRE(A::post_due(f.cloud) == 100'000);
  REQUIRE(A::fetch_due(f.cloud) == 65'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 1);
}

TEST_CASE("Config Fetch deadline wait is wrap-safe", "[CloudService][fetch_gate][wrap]") {
  CloudFixture f;
  const uint32_t now = UINT32_MAX - 1000;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, now + 90'000);
  A::set_fetch_due(f.cloud, now + 60'000);

  REQUIRE(A::run_once(f.cloud, now) == 60'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
}

TEST_CASE("Disabled config Fetch deadline cannot cause a zero-wait spin",
          "[CloudService][fetch_gate][clamp]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 70'000);
  A::set_fetch_due(f.cloud, 0);
  f.cloud.set_config_fetch_enabled(false);

  REQUIRE(A::run_once(f.cloud, 10'000) == 60'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
}

// ============================================================================
// 10. RSSI translation
// ============================================================================

TEST_CASE("RSSI of WIFI_RSSI_INVALID is translated to -127", "[CloudService][rssi]") {
  CloudFixture f;
  cloud_spy::wifi_rssi = WIFI_RSSI_INVALID; // 0
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  A::run_once(f.cloud, /*now=*/0);
  REQUIRE(cloud_spy::last_post_signal == -127);
}

TEST_CASE("Real RSSI is forwarded unchanged", "[CloudService][rssi]") {
  CloudFixture f;
  cloud_spy::wifi_rssi = -57;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  A::run_once(f.cloud, /*now=*/0);
  REQUIRE(cloud_spy::last_post_signal == -57);
}

// ============================================================================
// 11. Shutdown latch
// ============================================================================

TEST_CASE("Shutdown branch increments done counter, skips AgClient", "[CloudService][shutdown]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 0);

  A::set_shutdown_pending(f.cloud, true);
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  REQUIRE(A::done_signal_count(f.cloud) == 1);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
  REQUIRE(wake == UINT32_MAX);
}

// ============================================================================
// 12. next_wake clamp
// ============================================================================

TEST_CASE("Past deadline returns 0 wake (no UINT32_MAX wrap)", "[CloudService][clamp]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  // Set both deadlines in the past by a tiny amount so the "fire" path
  // does not run them (the run path uses `>= 0` cast comparison; both
  // happen to be in the past so POST fires).  To test the clamp on
  // the "sleep" branch, push the disable flag so we enter the idle
  // branch.
  A::set_disable_cloud(f.cloud, true);
  A::set_post_due(f.cloud, 500);
  A::set_fetch_due(f.cloud, 500);

  // now=1000, both deadlines in the past while disabled — the slide
  // logic moves them forward and the sleep is computed.
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);

  // No wraparound: should be a sensible interval, not a near-UINT32_MAX
  // value.
  REQUIRE(wake <= 60'000);
}

// ============================================================================
// 13. POST metadata and empty snapshot
// ============================================================================

TEST_CASE("First POST sees a default-constructed snapshot", "[CloudService][first_post]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  A::run_once(f.cloud, /*now=*/0);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(cloud_spy::last_post_boot == 0);

  // Every measure field on the snapshot fails its is_*_valid() check
  // because Prereq A made the default sentinels universal.  This is
  // the contract the cloud task relies on for the cold-boot first
  // POST: serializer omits all measure fields, while "wifi" and "boot" remain.
  const MeasuresAGo &s = cloud_spy::last_post_snapshot;
  REQUIRE_FALSE(s.co2.is_valid());
  REQUIRE_FALSE(s.pm_a.is_valid());
  REQUIRE_FALSE(s.temp_hum_a.is_valid());
  REQUIRE_FALSE(s.tvoc_nox.is_valid());
  REQUIRE_FALSE(s.power.is_valid());
  REQUIRE_FALSE(s.pressure.is_valid());
}

TEST_CASE("Cloud POST samples uptime without a new measurement", "[CloudService][boot]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  A::set_post_due(f.cloud, 0);
  A::set_fetch_due(f.cloud, 999'999'999);

  A::run_once(f.cloud, /*now=*/0);
  REQUIRE(cloud_spy::post_call_count == 1);
  REQUIRE(cloud_spy::last_post_boot == 0);

  f.mock_rtos.retained_time_ms = 60'000;
  A::run_once(f.cloud, /*now=*/60'000);
  REQUIRE(cloud_spy::post_call_count == 2);
  REQUIRE(cloud_spy::last_post_boot == 1);
}

// ============================================================================
// 14. Disarmed idle sleeps indefinitely
// ============================================================================

TEST_CASE("Disarmed loop returns UINT32_MAX wake", "[CloudService][idle]") {
  CloudFixture f;
  // _armed stays false; the idle branch returns indefinite wait.
  uint32_t wake = A::run_once(f.cloud, /*now=*/1000);
  REQUIRE(wake == UINT32_MAX);
}

// ============================================================================
// 15. Master disable suppresses both cloud legs
// ============================================================================

TEST_CASE("Master disable suppresses POST and Fetch and slides expired deadlines",
          "[CloudService][disable]") {
  CloudFixture f;
  A::set_armed(f.cloud, true);
  A::set_was_armed(f.cloud, true);
  f.cloud.set_disable_cloud(true);
  A::set_post_due(f.cloud, 500);  // in the past
  A::set_fetch_due(f.cloud, 500); // in the past

  A::run_once(f.cloud, /*now=*/1000);

  // Both deadlines slid one full interval into the future from `now`.
  REQUIRE(A::post_due(f.cloud) == 1000 + 60'000);
  REQUIRE(A::fetch_due(f.cloud) == 1000 + 60'000);
  REQUIRE(cloud_spy::post_call_count == 0);
  REQUIRE(cloud_spy::fetch_call_count == 0);
}
