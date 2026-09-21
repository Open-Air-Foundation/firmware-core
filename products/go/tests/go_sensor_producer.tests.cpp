/**
 * AirGradient Go — SensorProducer tests
 *
 * Tests the notification handlers and the run() loop of SensorProducer
 * using mock sensors (via real SensorManager) and mockable RTOS primitives
 * (task_notify_wait_impl, queue_send_impl, get_time_ms_impl, delay_ms_impl).
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include "go_sensor_producer.h"
#include "hal/co2_sensor.h"
#include "hal/pm_sensor.h"
#include "hal/pressure_sensor.h"
#include "hal/temp_hum_sensor.h"
#include "hal/tvoc_nox_sensor.h"
#include "measures_types.h"
#include "rtos.h"
#include "services/sensor_manager.h"

namespace {

uint8_t event_queue_sentinel = 0;

} // namespace

// ===========================================================================
// Mock sensors (same pattern as sensor_manager.tests.cpp)
// ===========================================================================

class MockTempHumSensor : public trompeloeil::mock_interface<TempHumSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
};

class MockCO2Sensor : public trompeloeil::mock_interface<CO2Sensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_CONST_MOCK0(supports_temp_hum);
  IMPLEMENT_MOCK0(temp_hum_data);
  IMPLEMENT_CONST_MOCK0(supports_calibration);
  IMPLEMENT_MOCK1(do_baseline_calibration);
  IMPLEMENT_MOCK0(is_baseline_calibration_done);
  IMPLEMENT_CONST_MOCK0(supports_abc_period_configuration);
  IMPLEMENT_MOCK1(set_abc_period_days);
};

class MockPMSensor : public trompeloeil::mock_interface<PMSensor> {
public:
  IMPLEMENT_MOCK1(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_CONST_MOCK0(supports_temp_hum);
  IMPLEMENT_MOCK0(temp_hum_data);
  IMPLEMENT_MOCK0(sleep);
  IMPLEMENT_MOCK0(wake);
};

class MockTVOCNOxSensor : public trompeloeil::mock_interface<TVOCNOxSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_MOCK0(run_conditioning);
  IMPLEMENT_MOCK2(set_compensation);
};

class MockPressureSensor : public trompeloeil::mock_interface<PressureSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_CONST_MOCK0(supports_temp_hum);
  IMPLEMENT_MOCK0(temp_hum_data);
};

// ===========================================================================
// Mock RTOS with task_notify_wait + queue_send support
// ===========================================================================

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);
  IMPLEMENT_MOCK2(task_notify_wait_impl);
  IMPLEMENT_MOCK3(queue_send_impl);
};

// ===========================================================================
// Test access friend — exposes private members for direct testing
// ===========================================================================

class SensorProducerTestAccess {
public:
  explicit SensorProducerTestAccess(SensorProducer &p) : _p(p) {}

  void set_sampler_enabled(bool v) { _p._sampler_enabled = v; }
  bool sampler_enabled() const { return _p._sampler_enabled; }

  void set_last_tvoc_nox(const TVOCNOxData &d) { _p._last_tvoc_nox = d; }
  TVOCNOxData last_tvoc_nox() const { return _p._last_tvoc_nox; }

  void set_last_temp_hum(const TempHumData &d, bool valid) {
    _p._last_temp_hum = d;
    _p._last_temp_hum_valid = valid;
  }
  TempHumData last_temp_hum() const { return _p._last_temp_hum; }
  bool last_temp_hum_valid() const { return _p._last_temp_hum_valid; }

  void set_running(bool v) { _p._running = v; }

  void handle_calibration() { _p.handle_calibration(); }
  void handle_prepare() { _p.handle_prepare(); }
  void handle_pm_sleep() { _p.handle_pm_sleep(); }
  void handle_self_test() { _p.handle_self_test(); }
  void set_co2_abc_days(int days) { _p._config.co2_abc_days = days; }
  void handle_co2_abc_period() { _p.handle_co2_abc_period(); }
  void set_tvoc_nox_learning_offsets(int tvoc_learning_offset, int nox_learning_offset) {
    _p._config.tvoc_learning_offset = tvoc_learning_offset;
    _p._config.nox_learning_offset = nox_learning_offset;
  }
  void handle_tvoc_nox_learning_offsets() { _p.handle_tvoc_nox_learning_offsets(); }
  void handle_measurement(uint32_t v) { _p.handle_measurement(v); }
  void handle_sampler_tick() { _p.handle_sampler_tick(); }
  void run() { _p.run(); }

  static uint32_t encode_notify(uint8_t iterations, SensorGroup groups) {
    return (static_cast<uint32_t>(groups) << 8) | iterations;
  }

private:
  SensorProducer &_p;
};

// ===========================================================================
// Helpers
// ===========================================================================

#define EXPECT_READ(mock, data, status)                                                            \
  REQUIRE_CALL((mock), read(trompeloeil::_)).LR_SIDE_EFFECT(_1 = (data)).RETURN(status)

// ===========================================================================
// Handler tests
// ===========================================================================

TEST_CASE("SensorProducer handlers", "[SensorProducer]") {
  MockTempHumSensor mock_temp_hum;
  MockCO2Sensor mock_co2;
  MockPMSensor mock_pm;
  MockTVOCNOxSensor mock_tvoc_nox;
  MockPressureSensor mock_pressure;

  ALLOW_CALL(mock_co2, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_pm, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_pressure, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_pm, wake()).RETURN(true); // prepare/run wake PM before warmup

  Sensors sensors{};
  sensors.temp_hum = &mock_temp_hum;
  sensors.co2 = &mock_co2;
  sensors.pms_a = &mock_pm;
  sensors.tvoc_nox = &mock_tvoc_nox;
  sensors.pressure = &mock_pressure;

  SensorManager manager(sensors);

  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
      .RETURN(true);

  SensorProducer producer(manager, &event_queue_sentinel, {});
  SensorProducerTestAccess access(producer);

  SECTION("stop keeps PM measuring when requested") { producer.stop(false); }

  SECTION("stop sleeps PM when requested") {
    REQUIRE_CALL(mock_pm, sleep()).RETURN(true);

    producer.stop(true);
  }

  // -----------------------------------------------------------------------
  // handle_calibration
  // -----------------------------------------------------------------------

  SECTION("handle_calibration calls calibrate_co2 and posts event") {
    REQUIRE_CALL(mock_co2, supports_calibration()).RETURN(true);
    REQUIRE_CALL(mock_co2, do_baseline_calibration(trompeloeil::_)).RETURN(true);
    REQUIRE_CALL(mock_co2, is_baseline_calibration_done()).RETURN(true);

    // Capture the posted event
    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    access.handle_calibration();

    CHECK(captured.type == EventType::Co2CalibrationDone);
    CHECK(captured.co2_cal_result == static_cast<uint8_t>(Co2CalibrationResult::Success));
  }

  SECTION("handle_co2_abc_period applies the requested period") {
    access.set_co2_abc_days(7);
    REQUIRE_CALL(mock_co2, supports_abc_period_configuration()).RETURN(true);
    REQUIRE_CALL(mock_co2, set_abc_period_days(7)).RETURN(true);

    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    access.handle_co2_abc_period();

    CHECK(captured.type == EventType::Co2AbcPeriodDone);
    CHECK(captured.co2_abc_result == static_cast<uint8_t>(Co2AbcPeriodResult::Success));
  }

  SECTION("handle_co2_abc_period forwards the disabled sentinel") {
    access.set_co2_abc_days(-1);
    REQUIRE_CALL(mock_co2, supports_abc_period_configuration()).RETURN(true);
    REQUIRE_CALL(mock_co2, set_abc_period_days(-1)).RETURN(true);

    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    access.handle_co2_abc_period();

    CHECK(captured.type == EventType::Co2AbcPeriodDone);
    CHECK(captured.co2_abc_result == static_cast<uint8_t>(Co2AbcPeriodResult::Success));
  }

  SECTION("handle_tvoc_nox_learning_offsets applies both offsets and posts completion") {
    REQUIRE(manager.configure_tvoc_nox_index(10000));
    access.set_tvoc_nox_learning_offsets(24, 48);

    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    access.handle_tvoc_nox_learning_offsets();

    CHECK(captured.type == EventType::TvocNoxLearningOffsetDone);
    CHECK(captured.tvoc_nox_learning_offset_result ==
          static_cast<uint8_t>(TvocNoxLearningOffsetResult::Success));
  }

  // -----------------------------------------------------------------------
  // handle_prepare
  // -----------------------------------------------------------------------

  SECTION("handle_prepare wakes PM then warms up") {
    REQUIRE_CALL(mock_pm, wake()).RETURN(true);
    // warmup() calls warmup_step() in a loop — expect conditioning + PM reads
    REQUIRE_CALL(mock_tvoc_nox, run_conditioning()).TIMES(AT_LEAST(1)).RETURN(true);
    ALLOW_CALL(mock_pm, read(trompeloeil::_)).RETURN(false);

    access.handle_prepare();
  }

  SECTION("handle_pm_sleep sleeps PM and posts PmSensorAsleep") {
    REQUIRE_CALL(mock_pm, sleep()).RETURN(true);

    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    access.handle_pm_sleep();

    CHECK(captured.type == EventType::PmSensorAsleep);
  }

  // -----------------------------------------------------------------------
  // handle_self_test — bulk AQ self-test (single measurement -> pass/fail)
  // -----------------------------------------------------------------------

  SECTION("handle_self_test all valid -> all roles pass, posts SensorTestDone") {
    PMData pm_ok{};
    pm_ok.pm_25 = 5.0f;
    TVOCNOxData tvoc_ok{100, 25000, 1, 18000};
    PressureData pressure_ok{};
    pressure_ok.pressure = 1013.0f;

    EXPECT_READ(mock_temp_hum, (TempHumData{22.5f, 55.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm, pm_ok, true);
    EXPECT_READ(mock_tvoc_nox, tvoc_ok, true);
    EXPECT_READ(mock_pressure, pressure_ok, true);

    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    access.handle_self_test();

    CHECK(captured.type == EventType::SensorTestDone);
    CHECK(captured.sensor_test_results.co2_pass);
    CHECK(captured.sensor_test_results.pm_pass);
    CHECK(captured.sensor_test_results.temp_hum_pass);
    CHECK(captured.sensor_test_results.tvoc_nox_pass);
    CHECK(captured.sensor_test_results.pressure_pass);
    CHECK(captured.sensor_test_results.all_pass());
  }

  SECTION("handle_self_test failed reads -> those roles fail") {
    // CO2 + temp/hum valid; PM read fails; TVOC/NOx + pressure invalid values.
    EXPECT_READ(mock_temp_hum, (TempHumData{22.5f, 55.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm, (PMData{}), false);            // read failure -> pm fail
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{}), true);  // valid read, invalid indices
    EXPECT_READ(mock_pressure, (PressureData{}), true); // valid read, invalid pressure

    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    access.handle_self_test();

    CHECK(captured.type == EventType::SensorTestDone);
    CHECK(captured.sensor_test_results.co2_pass);
    CHECK(captured.sensor_test_results.temp_hum_pass);
    CHECK_FALSE(captured.sensor_test_results.pm_pass);
    CHECK_FALSE(captured.sensor_test_results.tvoc_nox_pass);
    CHECK_FALSE(captured.sensor_test_results.pressure_pass);
    CHECK_FALSE(captured.sensor_test_results.all_pass());
  }

  // -----------------------------------------------------------------------
  // handle_measurement — sampler active
  // -----------------------------------------------------------------------

  SECTION("sampler active: strips TvocNox from All, splices cache") {
    access.set_sampler_enabled(true);

    // Pre-set the TVOC/NOx cache with known values
    TVOCNOxData cached{.tvoc_index = 150, .tvoc_raw = 25000, .nox_index = 42, .nox_raw = 18000};
    access.set_last_tvoc_nox(cached);

    // TvocNox stripped → SGP41 must NOT be read
    FORBID_CALL(mock_tvoc_nox, read(trompeloeil::_));

    // Other sensors should be read (Other + PM groups)
    EXPECT_READ(mock_temp_hum, (TempHumData{22.5f, 55.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm, (PMData{}), false);
    EXPECT_READ(mock_pressure, (PressureData{}), false);

    // Capture the posted event to verify splice
    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    uint32_t notify = SensorProducerTestAccess::encode_notify(1, SensorGroup::All);
    access.handle_measurement(notify);

    // The posted event should carry the spliced cache, not invalid sentinels
    CHECK(captured.sensor_data.measures.tvoc_nox.tvoc_index == 150);
    CHECK(captured.sensor_data.measures.tvoc_nox.tvoc_raw == 25000);
    CHECK(captured.sensor_data.measures.tvoc_nox.nox_index == 42);
    CHECK(captured.sensor_data.measures.tvoc_nox.nox_raw == 18000);

    // Other fields from real reads
    CHECK(captured.sensor_data.measures.co2.co2 == 400);
  }

  // -----------------------------------------------------------------------
  // handle_measurement — sampler inactive
  // -----------------------------------------------------------------------

  SECTION("sampler inactive: preserves original mask, reads SGP41") {
    access.set_sampler_enabled(false);

    // All groups active → SGP41 SHOULD be read
    EXPECT_READ(mock_tvoc_nox,
                (TVOCNOxData{MeasuresInvalid::TVOC, 30000, MeasuresInvalid::NOX, 20000}), true);
    EXPECT_READ(mock_temp_hum, (TempHumData{22.5f, 55.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm, (PMData{}), false);
    EXPECT_READ(mock_pressure, (PressureData{}), false);

    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    uint32_t notify = SensorProducerTestAccess::encode_notify(1, SensorGroup::All);
    access.handle_measurement(notify);

    // TVOC/NOx should come from SensorManager (not spliced cache)
    CHECK(captured.sensor_data.measures.tvoc_nox.tvoc_raw == 30000);
    CHECK(captured.sensor_data.measures.tvoc_nox.nox_raw == 20000);
  }

  // -----------------------------------------------------------------------
  // handle_measurement — defaults
  // -----------------------------------------------------------------------

  SECTION("zero iterations defaults to 1") {
    access.set_sampler_enabled(false);

    // Exactly one read expected (1 iteration)
    EXPECT_READ(mock_temp_hum, (TempHumData{22.5f, 55.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm, (PMData{}), false);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{}), false);
    EXPECT_READ(mock_pressure, (PressureData{}), false);

    uint32_t notify = SensorProducerTestAccess::encode_notify(0, SensorGroup::All);
    access.handle_measurement(notify);
  }

  SECTION("None group defaults to All") {
    access.set_sampler_enabled(false);

    // All sensors should be read
    EXPECT_READ(mock_temp_hum, (TempHumData{22.5f, 55.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm, (PMData{}), false);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{}), false);
    EXPECT_READ(mock_pressure, (PressureData{}), false);

    uint32_t notify = SensorProducerTestAccess::encode_notify(1, SensorGroup::None);
    access.handle_measurement(notify);
  }

  // -----------------------------------------------------------------------
  // handle_measurement — temp/hum caching
  // -----------------------------------------------------------------------

  SECTION("caches valid temp_hum_a for compensation") {
    access.set_sampler_enabled(false);

    EXPECT_READ(mock_temp_hum, (TempHumData{24.0f, 60.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm, (PMData{}), false);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{}), false);
    EXPECT_READ(mock_pressure, (PressureData{}), false);

    uint32_t notify = SensorProducerTestAccess::encode_notify(1, SensorGroup::All);
    access.handle_measurement(notify);

    CHECK(access.last_temp_hum_valid());
    CHECK(access.last_temp_hum().temperature == 24.0f);
    CHECK(access.last_temp_hum().humidity == 60.0f);
  }

  SECTION("does not cache invalid temp_hum_a") {
    access.set_sampler_enabled(false);

    // Return invalid temp/hum
    EXPECT_READ(mock_temp_hum,
                (TempHumData{MeasuresInvalid::TEMPERATURE, MeasuresInvalid::HUMIDITY}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm, (PMData{}), false);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{}), false);
    EXPECT_READ(mock_pressure, (PressureData{}), false);

    uint32_t notify = SensorProducerTestAccess::encode_notify(1, SensorGroup::All);
    access.handle_measurement(notify);

    CHECK_FALSE(access.last_temp_hum_valid());
  }

  // -----------------------------------------------------------------------
  // handle_sampler_tick
  // -----------------------------------------------------------------------

  SECTION("sampler tick pushes compensation when valid") {
    access.set_last_temp_hum({22.0f, 55.0f}, true);

    REQUIRE_CALL(mock_tvoc_nox, set_compensation(22.0f, 55.0f));
    EXPECT_READ(mock_tvoc_nox,
                (TVOCNOxData{MeasuresInvalid::TVOC, 25000, MeasuresInvalid::NOX, 18000}), true);

    access.handle_sampler_tick();

    CHECK(access.last_tvoc_nox().tvoc_raw == 25000);
    CHECK(access.last_tvoc_nox().nox_raw == 18000);
  }

  SECTION("sampler tick skips compensation when not valid") {
    access.set_last_temp_hum({}, false);

    FORBID_CALL(mock_tvoc_nox, set_compensation(trompeloeil::_, trompeloeil::_));
    EXPECT_READ(mock_tvoc_nox,
                (TVOCNOxData{MeasuresInvalid::TVOC, 25000, MeasuresInvalid::NOX, 18000}), true);

    access.handle_sampler_tick();

    CHECK(access.last_tvoc_nox().tvoc_raw == 25000);
  }
}

// ===========================================================================
// run() integration tests
// ===========================================================================

TEST_CASE("SensorProducer run()", "[SensorProducer]") {
  MockTVOCNOxSensor mock_tvoc_nox;
  MockPMSensor mock_pm;

  ALLOW_CALL(mock_pm, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_pm, wake()).RETURN(true); // run() wakes PM before warmup

  Sensors sensors{};
  sensors.tvoc_nox = &mock_tvoc_nox;
  sensors.pms_a = &mock_pm;

  SensorManager manager(sensors);

  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);
  ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
      .RETURN(true);

  SensorProducer producer(manager, &event_queue_sentinel, {});
  SensorProducerTestAccess access(producer);

  SECTION("run enables sampler when SGP41 is wired") {
    // run() wakes PM before the initial warmup
    REQUIRE_CALL(mock_pm, wake()).RETURN(true);
    // Warmup: conditioning + PM reads
    ALLOW_CALL(mock_tvoc_nox, run_conditioning()).RETURN(true);
    ALLOW_CALL(mock_pm, read(trompeloeil::_)).RETURN(false);

    // get_time_ms: warmup loop + run loop init
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);

    // First task_notify_wait: stop the loop immediately
    REQUIRE_CALL(mock_rtos, task_notify_wait_impl(trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(access.set_running(false))
        .RETURN(false);

    access.set_running(true);
    access.run();

    CHECK(access.sampler_enabled());
  }

  SECTION("run: measurement notification dispatches and posts event") {
    ALLOW_CALL(mock_tvoc_nox, run_conditioning()).RETURN(true);
    ALLOW_CALL(mock_pm, read(trompeloeil::_)).RETURN(false);

    static uint64_t fake_time = 0;
    fake_time = 0;
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).LR_RETURN(fake_time);

    trompeloeil::sequence seq;

    // 1st wait: deliver a measurement notification (All, 1 iteration)
    uint32_t notify_all = SensorProducerTestAccess::encode_notify(1, SensorGroup::All);
    REQUIRE_CALL(mock_rtos, task_notify_wait_impl(trompeloeil::_, trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(*_1 = notify_all)
        .RETURN(true);

    // SGP41 should NOT be read (sampler strips TvocNox)
    FORBID_CALL(mock_tvoc_nox, read(trompeloeil::_));

    // Capture the posted event
    Event captured{};
    REQUIRE_CALL(mock_rtos, queue_send_impl(trompeloeil::_, trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(captured = *static_cast<const Event *>(_2))
        .RETURN(true);

    // 2nd wait: stop loop
    REQUIRE_CALL(mock_rtos, task_notify_wait_impl(trompeloeil::_, trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(access.set_running(false))
        .RETURN(false);

    access.set_running(true);
    access.run();

    CHECK(captured.type == EventType::SensorDataReady);
    // TVOC should be from cache (initial sentinels since no sampler tick yet)
    CHECK(captured.sensor_data.measures.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
  }

  SECTION("run: timeout fires sampler tick and caches TVOC") {
    ALLOW_CALL(mock_tvoc_nox, run_conditioning()).RETURN(true);
    ALLOW_CALL(mock_pm, read(trompeloeil::_)).RETURN(false);

    // Time advances past the sampler tick interval
    static uint64_t fake_time = 0;
    fake_time = 0;
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).LR_RETURN(fake_time);

    trompeloeil::sequence seq;

    // 1st wait: timeout (no notification) — time has advanced past tick
    REQUIRE_CALL(mock_rtos, task_notify_wait_impl(trompeloeil::_, trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(fake_time = SGP41_INDEX_SAMPLING_INTERVAL_MS + 1)
        .RETURN(false);

    // The sampler tick should read SGP41
    EXPECT_READ(mock_tvoc_nox,
                (TVOCNOxData{MeasuresInvalid::TVOC, 25000, MeasuresInvalid::NOX, 18000}), true);

    // 2nd wait: stop loop
    REQUIRE_CALL(mock_rtos, task_notify_wait_impl(trompeloeil::_, trompeloeil::_))
        .IN_SEQUENCE(seq)
        .LR_SIDE_EFFECT(access.set_running(false))
        .RETURN(false);

    access.set_running(true);
    access.run();

    // Cache should be updated with the sampler tick result
    CHECK(access.last_tvoc_nox().tvoc_raw == 25000);
    CHECK(access.last_tvoc_nox().nox_raw == 18000);
  }

  SECTION("run: sampler disabled when no TVOC sensor") {
    // Create a manager without TVOC/NOx
    Sensors no_tvoc{};
    no_tvoc.pms_a = &mock_pm;
    SensorManager no_tvoc_mgr(no_tvoc);
    SensorProducer no_tvoc_producer(no_tvoc_mgr, nullptr, {});
    SensorProducerTestAccess no_tvoc_access(no_tvoc_producer);

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_pm, read(trompeloeil::_)).RETURN(false);

    // Loop exits immediately
    REQUIRE_CALL(mock_rtos, task_notify_wait_impl(trompeloeil::_, trompeloeil::_))
        .LR_SIDE_EFFECT(no_tvoc_access.set_running(false))
        .RETURN(false);

    no_tvoc_access.set_running(true);
    no_tvoc_access.run();

    CHECK_FALSE(no_tvoc_access.sampler_enabled());
  }
}
