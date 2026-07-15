/**
 * AirGradient Go — Sensor Producer
 *
 * Wraps the shared SensorManager in an independent RTOS task.  The
 * orchestrator signals it (via task notification) to run a measurement cycle; the
 * task calls SensorManager::start_measures() — which blocks for the full
 * averaging duration — then posts a SensorDataReady event to the orchestrator
 * event queue.
 *
 * AGo sensor wiring (owned by main.cpp wiring layer, not by this class):
 *   sensors.temp_hum     = &sht40_driver
 *   sensors.co2          = &co2_driver       // S8 or Sunlight
 *   sensors.pms_a        = &pms5003_driver
 *   sensors.pms_b        = nullptr
 *   sensors.tvoc_nox     = &sgp41_driver
 *   sensors.o3_no2       = nullptr
 *
 * SensorProducer holds only a SensorManager reference.  It has no knowledge
 * of which sensors are wired — that is the product wiring layer's
 * responsibility.
 */

#pragma once

#include "go_events.h"
#include "measures_types.h"
#include "rtos.h"
#include "services/sensor_manager.h"

#include <cstdint>

class SensorProducerTestAccess; // forward declaration for test friend

class SensorProducer {
  friend class SensorProducerTestAccess;

public:
  struct Config {
    uint16_t task_stack_size = 4096;
    uint8_t task_priority = 5;
  };

  /// Construct the producer.  Does not start the task.
  ///
  /// @param manager      SensorManager to drive.  Must outlive this producer.
  /// @param event_queue  Orchestrator event queue.  Must be created before
  ///                     start() is called.
  /// @param config       Task stack size and priority.
  SensorProducer(SensorManager &manager, RtosQueueHandle event_queue, const Config &config);

  /// Start the sensor task.  Call once during initialization.
  /// @return true if the task was created successfully.
  bool start();

  /// Stop the sensor task.  Sets _running = false then deletes the task.
  /// Safe because SensorManager holds no mutexes.
  void stop();

  /// Trigger one measurement cycle with the given iteration count.
  /// Non-blocking: returns immediately after signalling the task via
  /// RTOS::task_notify_send().
  /// @param iterations Number of averaging iterations (minimum 1 enforced
  ///                   inside the task).
  /// @param groups     Which sensor groups to poll.
  void request_measurement(uint8_t iterations, SensorGroup groups);

  /// Trigger a CO2 background calibration.
  /// Non-blocking: returns immediately.  The task runs the blocking
  /// SensorManager::calibrate_co2() and posts a Co2CalibrationDone event
  /// when finished.
  void request_co2_calibration();

  /// Trigger PM sensor warmup after a power cycle.
  /// Non-blocking: returns immediately.  The task runs blocking
  /// SensorManager::warmup() (~10 s) in its own context.
  ///
  /// The orchestrator sends this notification `CONFIG_SENSOR_WARMUP_DURATION_MS`
  /// before the next measurement deadline.  The subsequent measurement
  /// notification latches while warmup is running and is consumed
  /// immediately after warmup completes.
  void request_prepare();

  /// Put the PM sensor into low-power sleep, then post PmSensorAsleep so the
  /// orchestrator can isolate the bus. Non-blocking: returns immediately.
  void request_pm_sleep();

  /// Pressure-sensor on-die temperature snapshotted at the end of the last
  /// measurement cycle. Invalid sentinels before the first cycle or when no
  /// pressure sensor is wired. Read by the orchestrator for Cal telemetry;
  /// benign cross-task read (single aligned-float fields, 10s cadence).
  TempHumData last_pressure_temp_hum() const { return _last_pressure_temp_hum; }

  /// Trigger the bulk AQ hardware self-test. Non-blocking: returns
  /// immediately. The task runs one measurement (start_measures with a single
  /// iteration) in its own context, classifies each Go sensor role by field
  /// validity, and posts a single SensorTestDone event, then resumes normal
  /// posting.
  void request_self_test();

private:
  SensorManager &_manager;
  RtosQueueHandle _event_queue;
  Config _config;

  volatile bool _running = false;
  RtosTaskHandle _task_handle = nullptr;

  /// Cached TVOC/NOx from the most recent sampler tick. Spliced into
  /// measurement results when the sampler is active so the orchestrator
  /// always receives fresh index values without reading SGP41 at
  /// irregular measurement cadence.
  TVOCNOxData _last_tvoc_nox{
      .tvoc_index = MeasuresInvalid::TVOC,
      .tvoc_raw = MeasuresInvalid::TVOC,
      .nox_index = MeasuresInvalid::NOX,
      .nox_raw = MeasuresInvalid::NOX,
  };

  /// Last valid temp/hum for compensation push to SGP41 driver.
  TempHumData _last_temp_hum{};
  bool _last_temp_hum_valid = false;

  /// Pressure-sensor on-die temp snapshot for last_pressure_temp_hum().
  TempHumData _last_pressure_temp_hum{};

  // A flag that indicate TVOC and NOx sampling enabled or not
  bool _sampler_enabled = false;

  /// Sentinel notification value that triggers calibration instead of measurement.
  static constexpr uint32_t NOTIFY_CALIBRATION = UINT32_MAX;

  /// Sentinel notification value that triggers PM warmup after power cycle.
  static constexpr uint32_t NOTIFY_PREPARE = UINT32_MAX - 1;

  /// Sentinel notification value that triggers PM sleep.
  static constexpr uint32_t NOTIFY_PM_SLEEP = UINT32_MAX - 2;

  /// Sentinel notification value that triggers the AQ self-test sweep.
  static constexpr uint32_t NOTIFY_SELF_TEST = UINT32_MAX - 3;

  /// Sampler cadence derived from Kconfig choice. Only active when the
  /// algorithm is successfully configured.
  static constexpr uint32_t SAMPLER_TICK_MS = SGP41_INDEX_SAMPLING_INTERVAL_MS;

  static void task_entry(void *arg); ///< RTOS task entry point
  void run();                        ///< Actual task loop

  // Notification handlers — called from run()
  void handle_calibration();
  void handle_prepare();
  void handle_pm_sleep();
  void handle_self_test();
  void handle_measurement(uint32_t notify_value);
  void handle_sampler_tick();
};
