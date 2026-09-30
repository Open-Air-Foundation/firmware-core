#include "accel_service.h"

#include "ag_log.h"

#include <cinttypes>

static constexpr const char *TAG = "AccelService";

namespace {
const char *result_name(ShakeDetector::Result result) {
  switch (result) {
  case ShakeDetector::Result::None:
    return "sample";
  case ShakeDetector::Result::Peak:
    return "peak";
  case ShakeDetector::Result::Shake:
    return "SHAKE";
  case ShakeDetector::Result::Rejected:
    return "reject";
  case ShakeDetector::Result::Invalid:
    return "invalid";
  case ShakeDetector::Result::OffAxis:
    return "skip-axis";
  }
  return "invalid";
}
} // namespace

AccelService::AccelService(AccelSensor *sensor, const gpio::Hal &gpio, RtosQueueHandle event_queue,
                           const Config &config)
    : _sensor(sensor), _gpio(gpio), _event_queue(event_queue), _config(config) {}

AccelService::~AccelService() { stop(); }

bool AccelService::start() {
  if (_task != nullptr) {
    if (_running.load()) {
      return true;
    }
    stop(); // Reap a worker that exited on a sensor error.
  }
  if (_sensor == nullptr || _event_queue == nullptr || _config.pin_int == gpio::INVALID_PIN ||
      _interrupt_handler_attached) {
    AG_LOGW(TAG, "shake detection unavailable");
    return false;
  }
  _work_queue = RTOS::queue_create(WORK_QUEUE_DEPTH, sizeof(Command));
  _reply_queue = RTOS::queue_create(REPLY_QUEUE_DEPTH, sizeof(Reply));
  if (_work_queue == nullptr || _reply_queue == nullptr || !_done.create()) {
    stop();
    return false;
  }
  _running.store(true);
  if (!RTOS::task_create(task_entry, "accel_task", _config.task_stack_size, this,
                         _config.task_priority, &_task)) {
    stop();
    AG_LOGE(TAG, "failed to start shake worker");
    return false;
  }
  return true;
}

void AccelService::stop() {
  _running.store(false);
  if (_work_queue != nullptr) {
    const auto command = Command::Stop;
    RTOS::queue_send(_work_queue, &command, 0);
  }
  if (_task != nullptr) {
    _done.take();
    _task = nullptr;
  }
  cleanup_capture();
  _done.destroy();
  RTOS::queue_delete(_reply_queue);
  _reply_queue = nullptr;
  // A failed ISR detach must never leave a callback pointing at freed memory.
  // Its argument is the queue itself, so retaining it is safe even on destruction.
  if (!_interrupt_handler_attached) {
    RTOS::queue_delete(_work_queue);
    _work_queue = nullptr;
  }
}

bool AccelService::begin_hardware_test(TestReading &out) {
  return request_test(Command::BeginTest, out);
}

bool AccelService::read_hardware_test(TestReading &out) {
  return request_test(Command::ReadTest, out);
}

bool AccelService::end_hardware_test() {
  TestReading unused;
  return request_test(Command::EndTest, unused);
}

bool AccelService::request_test(Command command, TestReading &out) {
  out = {};
  if (!_running.load() || _work_queue == nullptr || _reply_queue == nullptr) {
    return false;
  }
  if (!RTOS::queue_send(_work_queue, &command, IDLE_WAIT_MS)) {
    return false;
  }
  // Single orchestrator caller: at most one request awaits a reply. No stack
  // pointers cross tasks. Keep checking worker health if it exits on a fault.
  Reply reply;
  while (true) {
    if (RTOS::queue_receive(_reply_queue, &reply, IDLE_WAIT_MS)) {
      out = reply.reading;
      return reply.ok;
    }
    if (!_running.load()) {
      return false;
    }
  }
}

AccelService::TestReading AccelService::read_test_sample() {
  TestReading out;
  if (_sensor != nullptr && (_mode == Mode::HardwareTest || _mode == Mode::HardwareTestError)) {
    out.who_am_i = _sensor->who_am_i();
    out.expected_who_am_i = _sensor->expected_who_am_i();
    if (_mode == Mode::HardwareTest) {
      out.result = _sensor->read(out.sample);
    }
  }
  return out;
}

void AccelService::process_command(Command command) {
  Reply reply;
  switch (command) {
  case Command::Interrupt:
    if (_mode == Mode::Capture && !_burst_active) {
      _interrupt_pending = true;
    }
    return;
  case Command::Stop:
    _running.store(false);
    return;
  case Command::BeginTest:
    _burst_active = false;
    _interrupt_pending = false;
    _detector.reset_capture();
    _mode = Mode::HardwareTestError;
    // Keep the task and ISR registration; mask GPIO while the sensor's filter
    // and interrupt output are reconfigured by this same worker.
    if (_sensor != nullptr && _gpio.disable_interrupt(_config.pin_int) &&
        _sensor->configure(HARDWARE_TEST_CONFIG)) {
      _mode = Mode::HardwareTest;
      reply.ok = true;
    }
    reply.reading = read_test_sample();
    AG_LOGI(TAG, "Hardware Test: +/-4 g unfiltered; configured=%d", reply.ok);
    break;
  case Command::ReadTest:
    reply.reading = read_test_sample();
    reply.ok = _mode == Mode::HardwareTest;
    break;
  case Command::EndTest:
    reply.ok = _mode == Mode::Capture || _mode == Mode::Cooldown;
    if (!reply.ok) {
      _burst_active = false;
      _interrupt_pending = false;
      _detector.reset_capture();
      // A failed restore leaves commands available, but capture cannot poll
      // until a later EndTest request successfully restores the configuration.
      _mode = Mode::CaptureUnavailable;
      reply.ok = _sensor != nullptr && _gpio.disable_interrupt(_config.pin_int) &&
                 _sensor->configure(CAPTURE_CONFIG);
      if (reply.ok) {
        if (_detector.in_cooldown(static_cast<uint32_t>(RTOS::get_time_ms()))) {
          _mode = Mode::Cooldown;
        } else {
          reply.ok = _gpio.enable_interrupt(_config.pin_int);
          if (reply.ok) {
            _mode = Mode::Capture;
          }
        }
      }
    }
    AG_LOGI(TAG, "Hardware Test finished; interrupt capture restored=%d", reply.ok);
    break;
  }
  if (!RTOS::queue_send(_reply_queue, &reply, 0)) {
    AG_LOGE(TAG, "Hardware Test reply delivery failed; stopping worker");
    // This is a service fault, not a failed test. Release the waiting caller.
    _running.store(false);
  }
}

void AccelService::on_interrupt(void *arg) {
  const auto command = Command::Interrupt;
  RTOS::queue_send_from_isr(static_cast<RtosQueueHandle>(arg), &command);
}

void AccelService::task_entry(void *arg) {
  static_cast<AccelService *>(arg)->run();
  RTOS::task_delete(nullptr);
}

void AccelService::run() {
  if (!initialize_capture()) {
    AG_LOGE(TAG, "motion setup failed; disabling shake detection");
    _running.store(false);
  }
  while (_running.load()) {
    Command command;
    if (RTOS::queue_receive(_work_queue, &command, _burst_active ? 0 : IDLE_WAIT_MS) &&
        _running.load()) {
      process_command(command);
    }
    if (!_running.load()) {
      break;
    }
    if (!poll_capture()) {
      AG_LOGE(TAG, "capture failed; disabling shake detection");
      _running.store(false);
      break;
    }
    if (_burst_active) {
      RTOS::delay_ms(SAMPLE_INTERVAL_MS);
    }
  }
  cleanup_capture();
  _done.give(); // Last access to this instance before the task self-deletes.
}

bool AccelService::initialize_capture() {
  if (_sensor == nullptr) {
    return false;
  }
  _sensor_cleanup_pending = true;
  _mode = Mode::CaptureUnavailable;
  if (!_sensor->configure(CAPTURE_CONFIG)) {
    return false;
  }
  _detector.reset_capture();
  _burst_active = false;
  _interrupt_pending = false;
  if (!_gpio.configure(_config.pin_int, gpio::Mode::Input, gpio::PullMode::Floating,
                       gpio::InterruptType::RisingEdge) ||
      !_gpio.add_interrupt_handler(_config.pin_int, on_interrupt, _work_queue)) {
    return false;
  }
  _interrupt_handler_attached = true;
  if (!_gpio.enable_interrupt(_config.pin_int)) {
    return false;
  }
  _mode = Mode::Capture;
  AG_LOGI(TAG, "sensor: %u Hz +/- %u g high-pass=%d; IRQ=%u mg / %u ms axes=0x%x latched=%d",
          CAPTURE_CONFIG.sample_rate_hz, static_cast<unsigned>(CAPTURE_CONFIG.range),
          CAPTURE_CONFIG.high_pass, CAPTURE_CONFIG.interrupt.threshold_mg,
          CAPTURE_CONFIG.interrupt.duration_ms, CAPTURE_CONFIG.interrupt.high_axes,
          CAPTURE_CONFIG.interrupt.latched);
  AG_LOGI(TAG, "interrupt-burst: GPIO%d; Y peak=400 release=150 mg, 6 peaks, 50..400 ms",
          _config.pin_int);
  AG_LOGI(TAG, "window=1600 cooldown=1000 burst=5000 ms max; waiting for IRQ, XYZ idle");
  return true;
}

bool AccelService::poll_capture() {
  const bool notified = _interrupt_pending;
  _interrupt_pending = false;
  if (!_running.load() || _sensor == nullptr) {
    return false;
  }
  if (_mode != Mode::Capture && _mode != Mode::Cooldown) {
    return true;
  }
  const uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  if (_mode == Mode::Cooldown) {
    if (_detector.in_cooldown(now)) {
      return true;
    }
    return rearm_interrupt();
  }
  if (!_burst_active) {
    if (!start_burst_if_interrupted(notified, now)) {
      return false;
    }
    if (!_burst_active) {
      return true;
    }
  }

  if (!read_burst_sample(now)) {
    return false;
  }
  if (_burst_active && now - _burst_start >= BURST_MS) {
    return finish_burst(now);
  }
  return true;
}

bool AccelService::start_burst_if_interrupted(bool notified, uint32_t now) {
  const int level = _gpio.get_level(_config.pin_int);
  if (level < 0) {
    return false;
  }
  if (!notified && level != 1) {
    return true;
  }
  AG_LOGI(TAG, "IRQ received: via=%s GPIO%d=%d service_ms=%" PRIu32,
          notified ? "ISR" : "latched-level", _config.pin_int, level, now);
  bool active = false;
  if (!_gpio.disable_interrupt(_config.pin_int) || !_sensor->read_interrupt(active)) {
    return false;
  }
  if (!active) {
    AG_LOGW(TAG, "IRQ inactive; no capture");
    return _gpio.enable_interrupt(_config.pin_int);
  }
  _burst_active = true;
  _burst_start = _last_sample = now;
  _detector.reset_capture();
  AG_LOGI(TAG, "Burst started: duration=%" PRIu32 " ms; GPIO interrupt masked", BURST_MS);
  return true;
}

bool AccelService::read_burst_sample(uint32_t now) {
  AccelReading sample;
  ++_polls;
  const auto read = _sensor->read(sample);
  if (read == AccelReadResult::Error) {
    AG_LOGE(TAG, "POLL #%" PRIu32 ": I2C error", _polls);
    return false;
  }
  if (read != AccelReadResult::Ready) {
    if (_config.log_polls) {
      AG_LOGI(TAG, "POLL #%" PRIu32 " not-ready", _polls);
    }
    if (now - _last_sample >= SENSOR_STALL_MS) {
      AG_LOGE(TAG, "no fresh motion samples for %" PRIu32 " ms", SENSOR_STALL_MS);
      return false;
    }
    return true;
  }

  _last_sample = now;
  const auto result = _detector.update(sample, now);
  if (_config.log_polls) {
    AG_LOGD(TAG, "POLL #%" PRIu32 " xyz_mg=%d,%d,%d ov=%d clip=%d peaks=%u %s", _polls, sample.x_mg,
            sample.y_mg, sample.z_mg, sample.overrun, sample.clipped, _detector.peaks(),
            result_name(result));
  }
  if (result == ShakeDetector::Result::Shake) {
    post_shake_event(now);
    return finish_burst(now);
  }
  return true;
}

void AccelService::post_shake_event(uint32_t now) {
  Event event{};
  event.type = EventType::ShakeDetected;
  event.shake_detected_ms = now;
  if (_running.load() && RTOS::queue_send(_event_queue, &event, 0)) {
    AG_LOGI(TAG, "SHAKE,%" PRIu32, now);
  } else {
    AG_LOGW(TAG, "SHAKE event dropped (stopping or queue full)");
  }
}

bool AccelService::finish_burst(uint32_t now) {
  AG_LOGI(TAG, "BURST_END,%" PRIu32 ",remaining_peaks=%u", now, _detector.peaks());
  _burst_active = false;
  if (_detector.in_cooldown(now)) {
    _mode = Mode::Cooldown;
    AG_LOGI(TAG, "shake cooldown; GPIO%d masked, XYZ polling idle", _config.pin_int);
    return true;
  }
  return rearm_interrupt();
}

bool AccelService::rearm_interrupt() {
  bool active = false;
  if (!_sensor->read_interrupt(active)) {
    return false;
  }
  RTOS::delay_ms(REARM_DELAY_MS);
  if (!_running.load()) {
    return true;
  }
  if (!_gpio.enable_interrupt(_config.pin_int)) {
    return false;
  }
  _mode = Mode::Capture;
  AG_LOGI(TAG, "GPIO%d rearmed; XYZ polling idle", _config.pin_int);
  return true;
}

void AccelService::cleanup_capture() {
  if (_interrupt_handler_attached) {
    _gpio.disable_interrupt(_config.pin_int);
    if (_gpio.remove_interrupt_handler(_config.pin_int)) {
      _interrupt_handler_attached = false;
    } else {
      AG_LOGE(TAG, "GPIO interrupt detach failed; retaining ISR queue");
    }
  }
  if (_sensor_cleanup_pending && _sensor != nullptr) {
    if (!_sensor->power_down()) {
      AG_LOGE(TAG, "motion power-down failed");
    }
    _sensor_cleanup_pending = false;
  }
  _burst_active = false;
  _interrupt_pending = false;
  _mode = Mode::CaptureUnavailable;
}
