// AirGradient Go — interrupt-burst accelerometer producer.
#pragma once

#include "accel_sensor.h"
#include "airgradient_gpio.h"
#include "go_events.h"
#include "rtos.h"
#include "shake_detector.h"

#include <atomic>

class AccelService {
public:
  struct Config {
    int pin_int = gpio::INVALID_PIN;
    uint32_t task_stack_size = 4096;
    uint8_t task_priority = 5;
    bool log_polls = true;
  };

  // Sensor, GPIO HAL, and central queue must outlive the service. The worker
  // owns all sensor access, including Hardware Test configuration and reads.
  AccelService(AccelSensor *sensor, const gpio::Hal &gpio, RtosQueueHandle event_queue,
               const Config &config);
  ~AccelService();
  AccelService(const AccelService &) = delete;
  AccelService &operator=(const AccelService &) = delete;

  /// Configuration runs in the worker task.
  bool start();
  /// Join the worker and release hardware during teardown.
  void stop();

  struct TestReading {
    AccelReading sample{};
    AccelReadResult result = AccelReadResult::Error;
    uint8_t who_am_i = 0;
    uint8_t expected_who_am_i = 0;
  };
  /// Synchronous worker requests; called only by the orchestrator task.
  /// Begin selects unfiltered +/-4 g; end restores filtered interrupt capture.
  bool begin_hardware_test(TestReading &out);
  bool read_hardware_test(TestReading &out);
  bool end_hardware_test();

private:
#ifdef TEST_HOST
  friend class AccelServiceTestAccess;
#endif
  static constexpr AccelConfig CAPTURE_CONFIG{
      .sample_rate_hz = 100,
      .range = AccelRange::G4,
      .high_pass = true,
      .interrupt = {.high_axes = AccelAxis::ALL,
                    .threshold_mg = 640,
                    .duration_ms = 20,
                    .latched = true},
  };
  static constexpr AccelConfig HARDWARE_TEST_CONFIG{
      .sample_rate_hz = CAPTURE_CONFIG.sample_rate_hz,
      .range = CAPTURE_CONFIG.range,
      .high_pass = false,
      .interrupt = {},
  };
  enum class Command : uint8_t { Interrupt, BeginTest, ReadTest, EndTest, Stop };
  enum class Mode { CaptureUnavailable, Capture, HardwareTest, HardwareTestError };
  struct Reply {
    TestReading reading{};
    bool ok = false;
  };
  static constexpr uint32_t SAMPLE_INTERVAL_MS = 1000 / CAPTURE_CONFIG.sample_rate_hz;
  static constexpr uint32_t BURST_MS = 5000;
  static constexpr uint32_t IDLE_WAIT_MS = 100;
  static constexpr uint32_t REARM_DELAY_MS = 30;
  static constexpr uint32_t SENSOR_STALL_MS = 250;
  static constexpr uint32_t WORK_QUEUE_DEPTH = 4;
  static constexpr uint32_t REPLY_QUEUE_DEPTH = 1;

  AccelSensor *_sensor;
  const gpio::Hal &_gpio;
  RtosQueueHandle _event_queue;
  Config _config;
  RtosQueueHandle _work_queue = nullptr;
  RtosQueueHandle _reply_queue = nullptr;
  RtosTaskHandle _task = nullptr;
  RtosBinarySemaphore _done;
  std::atomic<bool> _running{false}; // Shared with the caller; cleared on stop or worker fault.
  Mode _mode = Mode::CaptureUnavailable;
  bool _interrupt_handler_attached = false; // ISR registered, including while GPIO is masked.
  bool _sensor_cleanup_pending = false;     // Configuration attempted; power down during cleanup.
  bool _interrupt_pending = false; // IRQ command received; consumed by the next capture poll.
  bool _burst_active = false;      // Sampling within the capture window.
  uint32_t _burst_start = 0;
  uint32_t _last_sample = 0;
  uint32_t _polls = 0;
  ShakeDetector _detector{ShakeDetector::Config{}};

  static void on_interrupt(void *arg);
  static void task_entry(void *arg);
  void run();
  bool initialize_capture();
  void process_command(Command command);
  bool request_test(Command command, TestReading &out);
  TestReading read_test_sample();
  bool poll_capture();
  // Returns false on an error; _burst_active indicates whether capture started.
  bool start_burst_if_interrupted(bool notified, uint32_t now);
  bool read_burst_sample(uint32_t now);
  void post_shake_event(uint32_t now);
  bool finish_burst(uint32_t now);
  void cleanup_capture();
};
