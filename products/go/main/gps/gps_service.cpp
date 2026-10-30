/**
 * AirGradient Go — GPS Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "gps/gps_service.h"

#include "ag_log.h"
#include "decimal64.h"
#include "go_events.h"
#include "rtos.h"

#include <algorithm>
#include <ctime>

static constexpr const char *TAG = "GpsService";

// Loop yield: tight while draining, ~1 NMEA epoch when idle.
static constexpr uint32_t FAST_DRAIN_YIELD_MS = 10;
static constexpr uint32_t IDLE_YIELD_MS = 1000;
static constexpr uint32_t SLEEP_RETRY_MS = 1000;
static constexpr uint32_t MAX_WAKE_WAIT_MS = 60 * 60 * 1000;
static constexpr uint32_t MIN_WAKE_WAIT_MS = 10; // One tick at Go's 100 Hz RTOS rate.

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GpsService::GpsService(GpsDriver &driver, RtosQueueHandle event_queue, const Config &config)
    : _driver(driver), _event_queue(event_queue), _config(config),
      _baud_rate(config.wake_deadline_ms != 0 ? GpsDriver::MODULE_DEFAULT_BAUD
                                              : config.expected_baud),
      _wake_deadline_ms(config.wake_deadline_ms) {}

GpsService::~GpsService() {
  if (_running || _task_handle != nullptr) {
    stop();
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool GpsService::start() { return start_worker(0); }

bool GpsService::start_sleeping(uint32_t interval_ms) {
  return interval_ms != 0 && start_worker(interval_ms);
}

bool GpsService::start_worker(uint32_t sleep_interval_ms) {
  if (_running && _sleep_interval_ms == sleep_interval_ms) {
    return true; // already running — idempotent
  }
  if (!_mutex.is_valid()) {
    return false;
  }
  if (_config.baud_rate != GpsDriver::MODULE_DEFAULT_BAUD &&
      _config.baud_rate != GpsDriver::MODULE_ACTIVE_BAUD) {
    return false;
  }
  // Reap a task that exited after acquisition setup failed. Create completion
  // before launching so an immediate stop cannot miss the join semaphore.
  stop();
  if (!_done_sem.create()) {
    return false;
  }
  update_latest_fix({});
  _clock_synced = false;
  _sleep_interval_ms = sleep_interval_ms;
  _running = true;
  const bool ok =
      RTOS::task_create(task_entry, "gps_task", static_cast<uint32_t>(_config.task_stack_size),
                        this, static_cast<uint32_t>(_config.task_priority), &_task_handle);
  if (!ok) {
    _running = false;
    _done_sem.destroy();
    return false;
  }
  return true;
}

void GpsService::stop() {
  AG_LOGI(TAG, "stop: task only (GNSS keeps tracking)");
  // Serialize notification with worker exit so its task handle is still live.
  _mutex.lock();
  if (_running.exchange(false) && _task_handle != nullptr) {
    RTOS::task_notify_give(_task_handle);
  }
  _mutex.unlock();
  if (_task_handle != nullptr && _done_sem.is_created()) {
    _done_sem.take();
    _done_sem.destroy();
    _task_handle = nullptr;
  }
  _driver.end();
}

bool GpsService::request_sleep(uint32_t duration_ms) {
  if (duration_ms == 0) {
    return false;
  }
  stop();
  update_latest_fix({});
  return send_sleep(duration_ms);
}

bool GpsService::send_sleep(uint32_t duration_ms) {
  if (sleep_remaining_ms() != 0) {
    return false; // Sleeping GPS cannot accept a replacement interval.
  }
  const int baud =
      _config.fast_path_sleep_probe ? _driver.connect_fast_path() : _driver.connect(_baud_rate);
  if (baud == 0) {
    return false;
  }
  _baud_rate = baud;
  const bool sent = _driver.request_sleep(duration_ms);
  if (sent) {
    _mutex.lock();
    _wake_deadline_ms = RTOS::get_retained_time_ms() + duration_ms;
    _mutex.unlock();
    _baud_rate = GpsDriver::MODULE_DEFAULT_BAUD;
  }
  _driver.end();
  return sent;
}

uint32_t GpsService::sleep_remaining_ms() const {
  const uint64_t deadline_ms = sleep_deadline_ms();
  const uint64_t now_ms = RTOS::get_retained_time_ms();
  return deadline_ms > now_ms ? static_cast<uint32_t>(deadline_ms - now_ms) : 0;
}

uint64_t GpsService::sleep_deadline_ms() const {
  _mutex.lock();
  const uint64_t deadline_ms = _wake_deadline_ms;
  _mutex.unlock();
  return deadline_ms;
}

GpsData GpsService::get_latest_fix() const {
  _mutex.lock();
  const GpsData snapshot = _latest_fix;
  _mutex.unlock();
  return snapshot;
}

void GpsService::set_posting_interval_ms(int interval_ms) {
  _config.posting_interval_ms = interval_ms;
}

void GpsService::set_aiding_data(const GpsAidingData &data) {
  _mutex.lock();
  _aiding_data = data;
  _aiding_pending = true;
  _mutex.unlock();
}

// ---------------------------------------------------------------------------
// Task entry point (static)
// ---------------------------------------------------------------------------

// static
void GpsService::task_entry(void *arg) {
  static_cast<GpsService *>(arg)->run();
  RTOS::task_delete(nullptr);
}

// ---------------------------------------------------------------------------
// Task loop
// ---------------------------------------------------------------------------

bool GpsService::wait_for_wake() {
  while (_running) {
    const uint32_t remaining_ms = sleep_remaining_ms();
    if (remaining_ms == 0) {
      break;
    }
    const uint32_t wait_ms = std::clamp(remaining_ms, MIN_WAKE_WAIT_MS, MAX_WAKE_WAIT_MS);
    uint32_t notification;
    (void)RTOS::task_notify_wait(&notification, wait_ms);
  }
  return _running;
}

bool GpsService::activate_receiver() {
  const uint32_t remaining_ms = sleep_remaining_ms();
  if (remaining_ms != 0) {
    AG_LOGI(TAG, "Waiting for GPS wake: %lu ms remaining",
            static_cast<unsigned long>(remaining_ms));
  }
  if (!wait_for_wake()) {
    return false;
  }

  const int baud = _driver.connect(_baud_rate);
  if (baud == 0) {
    return false;
  }
  _baud_rate = baud;
  if (_config.baud_rate == GpsDriver::MODULE_ACTIVE_BAUD && baud != GpsDriver::MODULE_ACTIVE_BAUD) {
    if (!_driver.switch_to_active_baud()) {
      _driver.end();
      return false;
    }
    _baud_rate = GpsDriver::MODULE_ACTIVE_BAUD;
  }
  _mutex.lock();
  _wake_deadline_ms = 0;
  _mutex.unlock();
  _driver.configure_receiver();
  if (!_running) {
    return false;
  }
  _driver.gnss_start();
  AG_LOGI(TAG, "GNSS receiver started");
  return true;
}

void GpsService::run() {
  if (_sleep_interval_ms != 0) {
    run_sleep_loop();
  } else {
    run_acquisition();
  }
  _mutex.lock();
  _running = false;
  _mutex.unlock();
  if (_done_sem.is_created()) {
    _done_sem.give();
  }
}

void GpsService::run_sleep_loop() {
  while (wait_for_wake()) {
    if (!send_sleep(_sleep_interval_ms)) {
      AG_LOGW(TAG, "GPS sleep failed; will retry");
      if (_running) {
        uint32_t notification;
        (void)RTOS::task_notify_wait(&notification, SLEEP_RETRY_MS);
      }
    }
  }
}

void GpsService::run_acquisition() {
  if (!activate_receiver()) {
    return;
  }

  uint64_t last_post_ms = 0;

  while (_running) {
    // Check for pending aiding data (set via set_aiding_data() from any
    // thread).  Copy under mutex; inject outside mutex to avoid holding the
    // lock during serial I/O.
    {
      GpsAidingData aid_local;
      bool inject = false;
      _mutex.lock();
      if (_aiding_pending) {
        aid_local = _aiding_data;
        _aiding_pending = false;
        inject = true;
      }
      _mutex.unlock();
      if (inject) {
        char epoch_text[DECIMAL64_BUFFER_SIZE];
        AG_LOGI(TAG, "Inject aiding: lat=%.6f lon=%.6f alt=%.1f acc=%.0f epoch=%s tacc=%u",
                aid_local.latitude, aid_local.longitude, aid_local.altitude_m, aid_local.pos_acc_m,
                format_int64_decimal(aid_local.epoch_s, epoch_text),
                static_cast<unsigned>(aid_local.time_acc_ms));
        if (!_clock_synced && has_aiding_time(aid_local)) {
          RTOS::set_system_time_from_epoch(aid_local.epoch_s);
          // Do not set _clock_synced: GPS-derived time from RMC is more accurate
        }
        _driver.inject_aiding(aid_local);
      }
    }

    const bool had_data = _driver.read();
    if (had_data) {
      const GpsData data = _driver.get_data();
      update_latest_fix(data);

      if (!_clock_synced && is_gps_timestamp_valid(data.timestamp)) {
        sync_system_clock(data.timestamp);
        _clock_synced = true;
      }
    }

    const uint64_t now_ms = RTOS::get_time_ms();
    if (now_ms - last_post_ms >= static_cast<uint64_t>(_config.posting_interval_ms)) {
      // Post unconditionally so fix loss (NoFix) reaches the orchestrator.
      post_fix_event();
      last_post_ms = now_ms;
    }

    RTOS::delay_ms(had_data ? FAST_DRAIN_YIELD_MS : IDLE_YIELD_MS);
  }
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void GpsService::update_latest_fix(const GpsData &data) {
  _mutex.lock();
  _latest_fix = data;
  _mutex.unlock();
}

void GpsService::post_fix_event() {
  Event evt{};
  evt.type = EventType::GpsFixUpdate;
  evt.gps_data = _driver.get_data();

  AG_LOGD(TAG, "fix: lat=%.6f lon=%.6f alt=%.1f fix=%d sat=%d hdop=%.1f",
          evt.gps_data.position.latitude, evt.gps_data.position.longitude, evt.gps_data.altitude_m,
          static_cast<int>(evt.gps_data.fix.fix_type), evt.gps_data.fix.satellite_count,
          evt.gps_data.fix.hdop);

  // Non-blocking send: drop the event if the queue is full.
  RTOS::queue_send(_event_queue, &evt, 0);
}

void GpsService::sync_system_clock(const GpsTimestamp &ts) {
  // GPS timestamps are UTC.  On ESP-IDF the default timezone is UTC, so
  // mktime() produces the correct POSIX epoch without timezone adjustment.
  struct tm t{};
  t.tm_year = ts.year - 1900;
  t.tm_mon = ts.month - 1;
  t.tm_mday = ts.day;
  t.tm_hour = ts.hour;
  t.tm_min = ts.minute;
  t.tm_sec = ts.second;
  t.tm_isdst = 0;
  const time_t epoch = mktime(&t);
  if (epoch != static_cast<time_t>(-1)) {
    RTOS::set_system_time_from_epoch(static_cast<int64_t>(epoch));
  }
}
