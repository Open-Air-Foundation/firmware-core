/**
 * AirGradient Go — Sensor Producer implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_sensor_producer.h"

#include "ag_log.h"
#include "go_events.h"
#include "rtos.h"

static constexpr const char *TAG = "SensorProducer";

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SensorProducer::SensorProducer(SensorManager &manager, RtosQueueHandle event_queue,
                               const Config &config)
    : _manager(manager), _event_queue(event_queue), _config(config) {}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool SensorProducer::start() {
  _running = true;

  const bool ok =
      RTOS::task_create(task_entry, "sensor_prod", static_cast<uint32_t>(_config.task_stack_size),
                        this, static_cast<uint32_t>(_config.task_priority), &_task_handle);

  if (!ok) {
    _running = false;
    _task_handle = nullptr;
    return false;
  }
  return true;
}

void SensorProducer::stop() {
  _running = false;
  // Leave the PM sensor asleep on teardown — the task is about to be deleted,
  // so the caller (sole owner during shutdown) sleeps it synchronously.
  _manager.pm_sleep();
  if (_task_handle != nullptr) {
    // Send a zero-value notification to unblock task_notify_wait so the task
    // can check _running and exit the loop cleanly when possible.
    RTOS::task_notify_send(_task_handle, 0);
    RTOS::delay_ms(10);
    // Forcefully delete — safe because SensorManager holds no mutexes.
    // The task entry function blocks forever after run() returns (instead of
    // self-deleting) to avoid a double-delete race with this call.
    RTOS::task_delete(_task_handle);
    _task_handle = nullptr;
  }
}

void SensorProducer::request_measurement(uint8_t iterations, SensorGroup groups) {
  if (_task_handle != nullptr) {
    // Pack iterations (bits 0-7) and group mask (bits 8-15) into one notify value
    uint32_t value = (static_cast<uint32_t>(groups) << 8) | iterations;
    RTOS::task_notify_send(_task_handle, value);
  }
}

void SensorProducer::request_co2_calibration() {
  if (_task_handle != nullptr) {
    RTOS::task_notify_send(_task_handle, NOTIFY_CALIBRATION);
  }
}

void SensorProducer::request_prepare() {
  if (_task_handle != nullptr) {
    RTOS::task_notify_send(_task_handle, NOTIFY_PREPARE);
  }
}

void SensorProducer::request_pm_sleep() {
  if (_task_handle != nullptr) {
    RTOS::task_notify_send(_task_handle, NOTIFY_PM_SLEEP);
  }
}

void SensorProducer::request_self_test() {
  if (_task_handle != nullptr) {
    RTOS::task_notify_send(_task_handle, NOTIFY_SELF_TEST);
  }
}

// ---------------------------------------------------------------------------
// Task entry point (static)
// ---------------------------------------------------------------------------

// static
void SensorProducer::task_entry(void *arg) {
  static_cast<SensorProducer *>(arg)->run();
  // Block indefinitely — stop() will delete this task externally.
  // Must not return: FreeRTOS task functions that return cause undefined
  // behavior.  Self-delete is intentionally avoided here because stop()
  // performs a forced task_delete, and both paths racing would cause a
  // double-delete crash.
  while (true) {
    RTOS::delay_ms(60000);
  }
}

// ---------------------------------------------------------------------------
// Task loop
// ---------------------------------------------------------------------------

void SensorProducer::run() {
  AG_LOGI(TAG, "warming up sensors before first measurement");
  _manager.pm_wake(); // no-op unless resuming from sleep
  _manager.warmup();
  AG_LOGI(TAG, "warmup complete");

  // Enable the gas-index sampler when a TVOC/NOx sensor is wired and
  // algorithm configuration succeeds. The sampler converts the indefinite
  // task_notify_wait into a timeout-driven loop that feeds the Sensirion
  // algorithm at a fixed cadence independent of the measurement interval.
  _sampler_enabled =
      _manager.has_tvoc_nox_sensor() && _manager.configure_tvoc_nox_index(SAMPLER_TICK_MS);

  if (_sampler_enabled) {
    AG_LOGI(TAG, "gas-index sampler enabled (%lu ms)", static_cast<unsigned long>(SAMPLER_TICK_MS));
  }

  uint32_t next_tick_ms = static_cast<uint32_t>(RTOS::get_time_ms()) + SAMPLER_TICK_MS;

  while (_running) {
    uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
    uint32_t timeout = UINT32_MAX;
    if (_sampler_enabled) {
      timeout = (next_tick_ms > now) ? (next_tick_ms - now) : 0;
    }

    uint32_t notify_value = 0;
    const bool notified = RTOS::task_notify_wait(&notify_value, timeout);

    if (!_running) {
      break;
    }

    if (notified) {
      if (notify_value == NOTIFY_CALIBRATION) {
        handle_calibration();
      } else if (notify_value == NOTIFY_PREPARE) {
        handle_prepare();
      } else if (notify_value == NOTIFY_PM_SLEEP) {
        handle_pm_sleep();
      } else if (notify_value == NOTIFY_SELF_TEST) {
        handle_self_test();
      } else {
        handle_measurement(notify_value);
      }
    }

    // Sampler tick — runs after the notification handler so a measurement
    // request never starves it indefinitely. Missed intervals are skipped.
    now = static_cast<uint32_t>(RTOS::get_time_ms());
    if (_sampler_enabled && now >= next_tick_ms) {
      handle_sampler_tick();
      next_tick_ms = now + SAMPLER_TICK_MS;
    }
  }
}

// ---------------------------------------------------------------------------
// Notification handlers
// ---------------------------------------------------------------------------

void SensorProducer::handle_calibration() {
  // NOTE: Blocking!
  Co2CalibrationResult result = _manager.calibrate_co2();

  Event event{};
  event.type = EventType::Co2CalibrationDone;
  event.co2_cal_result = static_cast<uint8_t>(result);
  RTOS::queue_send(_event_queue, &event, 0);
}

void SensorProducer::handle_prepare() {
  AG_LOGI(TAG, "PM prepare: waking and warming up after power-on");
  _manager.pm_wake();
  _manager.warmup();
  AG_LOGI(TAG, "PM prepare: complete");
}

void SensorProducer::handle_pm_sleep() {
  _manager.pm_sleep();

  Event event{};
  event.type = EventType::PmSensorAsleep;
  RTOS::queue_send(_event_queue, &event, 0);
}

void SensorProducer::handle_self_test() {
  // One fresh read of every sensor group, then classify each Go role by field
  // validity. Absent and bad-read collapse into a single FAIL (pass = false).
  // temp_hum_a already honors the CO2 temp/hum fallback, so boards without a
  // dedicated T/H sensor still pass when CO2 supplies it.
  Measures m = _manager.start_measures(1, SensorGroup::All);

  SensorTestResults results{};
  results.co2_pass = m.co2.is_valid();
  results.pm_pass = m.pm_a.is_pm_25_valid();
  results.temp_hum_pass = m.temp_hum_a.is_valid();
  results.tvoc_nox_pass = m.tvoc_nox.is_tvoc_index_valid() && m.tvoc_nox.is_nox_index_valid();
  results.pressure_pass = m.pressure.is_pressure_valid();

  Event event{};
  event.type = EventType::SensorTestDone;
  event.sensor_test_results = results;
  RTOS::queue_send(_event_queue, &event, 0);
}

void SensorProducer::handle_measurement(uint32_t notify_value) {
  // Decode iteration count (bits 0-7) and sensor group mask (bits 8-15)
  uint32_t iterations = notify_value & 0xFF;
  auto requested_groups = static_cast<SensorGroup>((notify_value >> 8) & 0xFF);

  if (iterations == 0) {
    iterations = 1;
  }
  if (requested_groups == SensorGroup::None) {
    requested_groups = SensorGroup::All;
  }

  // When the sampler is active, strip the TvocNox bit so measurement
  // notifications never read SGP41 or advance the algorithm at an
  // irregular cadence — only the sampler tick does that.
  SensorGroup measurement_groups = requested_groups;
  if (_sampler_enabled) {
    measurement_groups = static_cast<SensorGroup>(static_cast<uint8_t>(requested_groups) &
                                                  ~static_cast<uint8_t>(SensorGroup::TvocNox));
  }

  Measures measures = _manager.start_measures(static_cast<int>(iterations), measurement_groups);

  // When the sampler is active, the measurement didn't read SGP41
  // (TvocNox bit was stripped). Splice in the cached TVOC/NOx so the
  // orchestrator receives fresh index values.
  if (_sampler_enabled) {
    measures.tvoc_nox = _last_tvoc_nox;
  }

  // Cache the latest valid temp/hum for compensation push.
  if (measures.temp_hum_a.is_temp_valid() && measures.temp_hum_a.is_hum_valid()) {
    _last_temp_hum = measures.temp_hum_a;
    _last_temp_hum_valid = true;
  }

  // Snapshot the pressure sensor's on-die temperature (driver cache, no bus
  // traffic) for the BLE Cal telemetry stream.
  _last_pressure_temp_hum = _manager.pressure_temp_hum();

  // Map to MeasuresAGo — select the primary sensor channels only.
  MeasuresAGo basic{};
  basic.temp_hum_a = measures.temp_hum_a;
  basic.pm_a = measures.pm_a;
  basic.co2 = measures.co2;
  basic.tvoc_nox = measures.tvoc_nox;
  basic.power = measures.power;
  basic.pressure = measures.pressure;

  Event event{};
  event.type = EventType::SensorDataReady;
  event.sensor_data = basic;
  RTOS::queue_send(_event_queue, &event, 0);
}

void SensorProducer::handle_sampler_tick() {
  if (_last_temp_hum_valid) {
    _manager.set_tvoc_nox_compensation(_last_temp_hum.temperature, _last_temp_hum.humidity);
  }

  Measures sampler = _manager.start_measures(1, SensorGroup::TvocNox);
  _last_tvoc_nox = sampler.tvoc_nox;
}
