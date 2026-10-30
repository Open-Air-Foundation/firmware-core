/**
 * AirGradient Go — GPS Service
 *
 * Runs as an independent RTOS task that calls GpsDriver::read() in a loop,
 * updates the latest fix, syncs the system clock on the first valid timestamp,
 * and posts GpsFixUpdate events to the orchestrator event queue at the
 * configured interval.
 *
 * NMEA parsing and serial I/O are fully delegated to GpsDriver.  This service
 * only orchestrates the task lifecycle and event posting.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "gps/gps_driver.h"
#include "gps/gps_types.h"
#include "rtos.h"

#include <cstdint>
#include <atomic>

inline constexpr int GPS_POSTING_INTERVAL_MS_DEFAULT = 5000;

class GpsService {
public:
  struct Config {
    int baud_rate = GpsDriver::MODULE_ACTIVE_BAUD;
    int posting_interval_ms = GPS_POSTING_INTERVAL_MS_DEFAULT;
    uint16_t task_stack_size = 4096;
    uint8_t task_priority = 3; // must be below display worker (4)
    int expected_baud = GpsDriver::MODULE_DEFAULT_BAUD;
    uint64_t wake_deadline_ms = 0; // restored from RtcAppState after deep sleep
  };

  /// Construct the service.  Does not start the task.
  ///
  /// @param driver      GPS driver (GpsDriver).  Must outlive this service.
  /// @param event_queue  Orchestrator event queue.  Must be created before
  ///                     start() is called.
  /// @param config       Runtime configuration (baud rate, interval, ...).
  GpsService(GpsDriver &driver, RtosQueueHandle event_queue, const Config &config);
  ~GpsService();

  /// Start GPS task and GNSS receiver. Returns true if running or started.
  /// Idempotent: returns true without creating a second task if already running.
  /// If sleeping, the task waits for the expected wake before acquisition setup.
  /// stop() cancels that wait. Success means task creation, not GPS readiness.
  /// Lifecycle calls (start/stop/sleep) require one owning caller.
  bool start();

  /// Keep GPS inactive while ESP is awake. The worker sends sleep, waits for
  /// its deadline, and repeats. start() changes back to acquisition; stop()
  /// cancels maintenance. A sleeping receiver keeps its existing deadline.
  bool start_sleeping(uint32_t interval_ms);

  /// Stop GPS task and close UART. Does not send GNSS stop.
  /// Used when entering ESP32 deep sleep while GPS is active (preserves
  /// TAU1113 tracking for hot-start).
  void stop();

  /// Stop acquisition, transmit CFG-SLEEP at the expected baud and close UART.
  /// Zero duration is rejected without changing anything. An already sleeping
  /// GPS keeps its original deadline; pending activation is cancelled.
  /// True means TX completed, not confirmed module sleep. False means no new
  /// request or transport failure. A failed request does not create a deadline.
  [[nodiscard]] bool request_sleep(uint32_t duration_ms);

  /// Expected time remaining, not hardware-confirmed sleep status.
  uint32_t sleep_remaining_ms() const;
  uint64_t sleep_deadline_ms() const;

  /// Return the most recent parsed fix (thread-safe copy).
  GpsData get_latest_fix() const;

  /// Update the posting interval at runtime (e.g. when settings change).
  void set_posting_interval_ms(int interval_ms);

  /// Provide A-GNSS aiding data for injection.  Thread-safe: may be called
  /// before start() or while the task is running (e.g. when new data arrives
  /// via BLE).  The data is forwarded to GpsDriver::inject_aiding() on the
  /// next task-loop iteration.
  void set_aiding_data(const GpsAidingData &data);

#ifdef TEST_HOST
  friend class GpsServiceTestAccess;
#endif

private:
  GpsDriver &_driver;
  RtosQueueHandle _event_queue;
  Config _config;

  GpsData _latest_fix; // protected by _mutex
  std::atomic<bool> _running{false};
  RtosTaskHandle _task_handle = nullptr;
  mutable RtosMutex _mutex;
  RtosBinarySemaphore _done_sem; // signalled by task before self-delete
  bool _clock_synced = false;
  GpsAidingData _aiding_data;   // protected by _mutex
  bool _aiding_pending = false; // protected by _mutex
  // Worker owns baud while running; lifecycle caller joins before accessing it.
  int _baud_rate;
  uint64_t _wake_deadline_ms;      // protected by _mutex
  uint32_t _sleep_interval_ms = 0; // zero: acquire; nonzero: repeat sleep

  static void task_entry(void *arg); // RTOS task entry point
  void run();                        // actual task loop
  bool start_worker(uint32_t sleep_interval_ms);
  void run_acquisition();
  void run_sleep_loop();
  bool wait_for_wake();
  bool send_sleep(uint32_t duration_ms); // caller owns UART; never joins worker
  bool activate_receiver();              // cancellable wait, then acquisition setup

  void update_latest_fix(const GpsData &data);    // writes under mutex
  void post_fix_event();                          // post GpsData to event queue
  void sync_system_clock(const GpsTimestamp &ts); // set ESP32 RTC from GPS time
};

/// Synchronous one-shot GPS read. Blocks until a valid fix is parsed,
/// timeout_ms expires, or abort becomes true. For use in the fast-path
/// timer-wake boot path only — does not require a running RTOS task.
///
/// Tries the expected active baud, then the other baud if GPS is silent.
/// A receiver still within its sleep deadline is left alone.
///
/// @param driver     GPS driver; UART is closed before returning.
/// @param baud_rate  Active receiver baud rate.
/// @param timeout_ms Maximum time to wait for a valid position, in milliseconds.
/// @param abort      When true, the polling loop exits early. Intended for
///                   ISR-driven abort flags (volatile bool set by ISR).
///                   Default: a static false constant (no abort support).
/// @return           Latest GpsData; validate both fix and position before use.
inline const volatile bool gps_no_abort = false;

GpsData gps_read_once(GpsDriver &driver, int baud_rate, uint32_t timeout_ms,
                      const volatile bool &abort = gps_no_abort, uint64_t wake_deadline_ms = 0);
