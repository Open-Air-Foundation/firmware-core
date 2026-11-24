/**
 * AirGradient Go — CloudService
 *
 * Stationary cloud transport: periodic POST of MeasuresAGo and FETCH of
 * device config via AgClient on a dedicated low-priority task.
 *
 * State changes (arm/disarm/cloud gates) use atomics — no command queue,
 * no silent drops.  Heap is claimed lazily by start() and freed by stop();
 * Portable/Offline boots pay zero heap cost.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_CLOUD_H
#define GO_CLOUD_H

#include <atomic>
#include <cstdint>

#include "measures_types.h"
#include "rtos.h"

class AgClient;
class WifiService;
class OtaService;

class CloudService {
public:
  struct Deps {
    AgClient &client;
    WifiService &wifi;
    OtaService &ota;
  };

  struct Config {
    uint32_t post_interval_ms = 60'000;
    uint32_t fetch_interval_ms = 60'000;
    bool disable_cloud = false;
    bool config_fetch_enabled = true;
    uint32_t ota_check_interval_ms = 60 * 60 * 1000;
  };

  CloudService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg);
  ~CloudService();

  CloudService(const CloudService &) = delete;
  CloudService &operator=(const CloudService &) = delete;

  // -----------------------------------------------------------------
  // Lifecycle
  // -----------------------------------------------------------------

  /// Spawn the cloud task and allocate heap resources.  Idempotent.
  /// Returns false on allocation failure (self-cleans so caller can
  /// retry).  This is the only call that claims real heap.
  bool start();

  /// Drain in-flight HTTP, delete task, free heap.  Bounded by one
  /// WifiHttpClient timeout (~15 s).  Idempotent.
  void stop();

  /// Nonblocking handoff: request shutdown, then poll until resources are freed.
  void request_stop();
  bool poll_stop();

  // -----------------------------------------------------------------
  // State actions (lock-free; safe to call any time)
  // -----------------------------------------------------------------

  /// Enable periodic POST/FETCH.  @p fire_now makes the first cycle
  /// immediate; consumed only on Disarmed→Armed transition.
  /// Deadlines are start-anchored (not completion-anchored).
  void arm(bool fire_now);

  /// Disable periodic ticks.  In-flight HTTP still drains.
  void disarm();

  /// Push the disable_cloud flag; takes effect next iteration.
  void set_disable_cloud(bool disable);

  /// Enable or disable config Fetch independently of measurement POST.
  /// Enabling makes config Fetch immediately due without changing POST cadence.
  void set_config_fetch_enabled(bool enabled);

  /// Replace the cached snapshot (mutex hold ~µs).
  void update_measures_snapshot(const MeasuresAGo &m);

#ifdef TEST_HOST
  friend class CloudServiceTestAccess;
#endif

private:
  void _run();
  static void _task_entry(void *arg);

  /// Single iteration of the run loop.  Returns ms to wait before next
  /// iteration (0 = continue now, UINT32_MAX = wait indefinitely).
  uint32_t _run_iteration(uint32_t now);

  void _wake();
  void _do_post(uint32_t now_ms);
  void _do_fetch(uint32_t now_ms);
  void _check_ota(uint32_t now_ms);
  MeasuresAGo _snapshot_copy();

  RtosQueueHandle _event_queue;
  AgClient &_client;
  WifiService &_wifi;
  OtaService &_ota;
  Config _cfg;

  // Orchestrator-writable state (no locks needed)
  std::atomic<bool> _armed{false};
  std::atomic<bool> _disable_cloud;
  std::atomic<bool> _config_fetch_enabled;
  std::atomic<bool> _fire_now_pending{false};
  std::atomic<bool> _shutdown_pending{false};
  std::atomic<bool> _stop_ready{false};
  void _finish_stop();

  // Snapshot
  MeasuresAGo _latest_snapshot{};
  RtosMutex _snapshot_mtx;

  // Deadlines (task-owned; friend-readable for tests)
  uint32_t _post_due = 0;
  std::atomic<uint32_t> _fetch_due{0};
  bool _was_armed = false;
  uint32_t _ota_due = 0;

  // Task lifecycle (heap-allocated only by start())
  RtosTaskHandle _task_handle = nullptr;
  RtosBinarySemaphore _done_sem;
  char *_fetch_buf = nullptr;

#ifdef TEST_HOST
  uint32_t _test_done_signal_count = 0;
#endif
};

#endif // GO_CLOUD_H
