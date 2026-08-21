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
#include "go_events.h"
#include "rtos.h"

#include <ctime>

static constexpr const char *TAG = "GpsService";

// Loop yield: tight while draining, ~1 NMEA epoch when idle.
static constexpr uint32_t FAST_DRAIN_YIELD_MS = 10;
static constexpr uint32_t IDLE_YIELD_MS = 1000;

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

GpsService::GpsService(GpsDriver &driver, RtosQueueHandle event_queue, const Config &config)
    : _driver(driver), _event_queue(event_queue), _config(config) {}

GpsService::~GpsService() {
  if (_running) {
    stop();
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool GpsService::start() {
  if (_running) {
    return true; // already running — idempotent
  }
  if (!_mutex.is_valid()) {
    return false;
  }
  _clock_synced = false;
  _running = true;
  const bool ok =
      RTOS::task_create(task_entry, "gps_task", static_cast<uint32_t>(_config.task_stack_size),
                        this, static_cast<uint32_t>(_config.task_priority), &_task_handle);
  if (!ok) {
    _running = false;
    return false;
  }
  return true;
}

void GpsService::stop() {
  AG_LOGI(TAG, "stop: task only (GNSS keeps tracking)");
  _running = false;
  if (_task_handle != nullptr && _done_sem.is_created()) {
    _done_sem.take();
    _done_sem.destroy();
    _task_handle = nullptr;
  }
  _driver.end();
}

void GpsService::stop_and_idle_gnss() {
  AG_LOGI(TAG, "stop_and_idle_gnss: stopping task and GNSS receiver");
  if (!_running && _task_handle == nullptr) {
    // Task already stopped — use one-shot path to ensure module receives
    // GNSS stop even when no task was running.
    idle_gnss();
    return;
  }
  _running = false;
  if (_task_handle != nullptr && _done_sem.is_created()) {
    _done_sem.take();
    _done_sem.destroy();
    _task_handle = nullptr;
  }
  // Serial link is still open — send GNSS stop before closing.
  _driver.gnss_stop();
  _driver.end();
  AG_LOGI(TAG, "GNSS receiver stopped");
}

void GpsService::idle_gnss() {
  AG_LOGI(TAG, "idle_gnss: sending GNSS stop (no task)");
  _driver.begin(_config.baud_rate);
  _driver.gnss_stop();
  _driver.end();
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

void GpsService::run() {
  // Create the done semaphore inside the task so it is valid for the entire
  // task lifetime.  stop() blocks on this semaphore before returning.
  _done_sem.create();

  _driver.begin(_config.baud_rate);
  _driver.gnss_start();
  AG_LOGI(TAG, "GNSS receiver started");

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
        AG_LOGI(TAG, "Inject aiding: lat=%.6f lon=%.6f alt=%.1f acc=%.0f epoch=%lld tacc=%u",
                aid_local.latitude, aid_local.longitude, aid_local.altitude_m, aid_local.pos_acc_m,
                static_cast<long long>(aid_local.epoch_s),
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

  // Signal stop()/stop_and_idle_gnss() that the task loop has exited before
  // self-deleting.  The caller controls the shutdown sequence: serial link
  // remains open so the caller can optionally send GNSS stop before end().
  if (_done_sem.is_created()) {
    _done_sem.give();
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

// ---------------------------------------------------------------------------
// One-shot synchronous read (fast-path timer wake)
// ---------------------------------------------------------------------------

GpsData gps_read_once(GpsDriver &driver, int baud_rate, uint32_t timeout_ms,
                      const volatile bool &abort) {
  if (!driver.begin_hot_resume(baud_rate)) {
    AG_LOGE(TAG, "hot-resume: failed to open GPS UART");
    return {};
  }

  AG_LOGI(TAG, "hot-resume: listening at %d baud for %lu ms", baud_rate,
          static_cast<unsigned long>(timeout_ms));

  bool received_sentence = false;
  bool acquired_position = false;
  const uint64_t deadline_ms = RTOS::get_time_ms() + timeout_ms;
  while (RTOS::get_time_ms() < deadline_ms && !abort) {
    if (driver.read()) {
      received_sentence = true;
      const GpsData data = driver.get_data();
      if (is_fix_valid(data.fix) && is_position_valid(data.position)) {
        acquired_position = true;
        break;
      }
    }
    RTOS::delay_ms(10);
  }
  const GpsData data = driver.get_data();
  driver.end();

  if (acquired_position) {
    AG_LOGI(TAG, "hot-resume: acquired valid position");
  } else {
    AG_LOGW(TAG, "hot-resume: no valid position (sentence=%d aborted=%d)", received_sentence,
            static_cast<bool>(abort));
  }
  return data;
}
