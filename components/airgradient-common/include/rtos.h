/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef RTOS_H
#define RTOS_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// Typed handle aliases
//
// On hardware, these resolve to actual FreeRTOS pointer types, giving
// compile-time safety against mixing up task/queue/semaphore handles.
// Under TEST_HOST, they fall back to void * (no FreeRTOS available).
// ---------------------------------------------------------------------------

#ifndef TEST_HOST
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

using RtosTaskHandle = TaskHandle_t;
using RtosQueueHandle = QueueHandle_t;
using RtosSemaphoreHandle = SemaphoreHandle_t;
#else
using RtosTaskHandle = void *;
using RtosQueueHandle = void *;
using RtosSemaphoreHandle = void *;
#endif

/**
 * @brief RTOS abstraction interface for platform-independent time and delay
 * operations
 *
 * This interface provides abstraction for FreeRTOS/ESP-IDF timing functions to
 * enable host-side testing without hardware dependencies.
 *
 * Uses static facade pattern: static methods delegate to mockable virtual
 * implementations.
 */
class RTOS {
public:
  virtual ~RTOS() = default;

  /**
   * @brief Block the calling task for at least @p ms milliseconds.
   *
   * @warning Tick-quantized (CONFIG_FREERTOS_HZ), not ms-accurate. Floors to
   * whole ticks, so at 100 Hz any request < 10 ms truncates to 0 ticks, and a
   * 0-tick delay is a bare yield (no sleep) that only gives way to equal/higher
   * priority tasks. Polling with a sub-tick delay from a high-priority task
   * busy-spins and starves lower-priority tasks on a single core. Pass >= one
   * tick for a real delay; use a busy-wait us primitive for precise short
   * timing.
   *
   * @param ms Requested delay; rounded down to whole ticks (may be zero).
   */
  static void delay_ms(uint32_t ms);

  /**
   * @brief Get current time in milliseconds since boot (static facade)
   * @return Time in milliseconds
   */
  static uint64_t get_time_ms();

  /**
   * @brief Get monotonic RTC-domain time in milliseconds.
   *
   * Unlike get_time_ms(), this clock continues across deep sleep while the RTC
   * domain remains powered. It is not GPS-adjustable wall time.
   *
   * @return Retained monotonic time in milliseconds, or 0 from the default
   *         host implementation.
   */
  static uint64_t get_retained_time_ms();

  /**
   * @brief Set singleton instance (primarily for testing)
   * @param rtos Pointer to RTOS implementation
   */
  static void set_instance(RTOS *rtos);

  /**
   * @brief Create and start a FreeRTOS task.
   *
   * Blocking: no
   * Allocates: yes (FreeRTOS task stack)
   * No-op in TEST_HOST mode (returns false).
   *
   * @param func        Task entry function.
   * @param name        Task name string (debug only).
   * @param stack_depth Stack depth in bytes (ESP-IDF xTaskCreate takes the
   *                    depth in bytes, not words like vanilla FreeRTOS).
   * @param param       Argument passed to func.
   * @param priority    FreeRTOS task priority.
   * @param out_handle  Receives the opaque task handle; may be nullptr.
   * @return true on success; false on failure or in TEST_HOST mode.
   */
  static bool task_create(void (*func)(void *), const char *name, uint32_t stack_depth, void *param,
                          uint32_t priority, RtosTaskHandle *out_handle);

  /**
   * @brief Delete a FreeRTOS task.
   *
   * No-op in TEST_HOST mode.
   *
   * @param handle Opaque task handle from task_create(); nullptr deletes the
   * calling task.
   */
  static void task_delete(RtosTaskHandle handle);

  /**
   * @brief Create a FreeRTOS queue.
   *
   * Allocates: yes (FreeRTOS queue storage)
   * Returns nullptr in TEST_HOST mode.
   *
   * @param length    Maximum number of items the queue can hold.
   * @param item_size Size in bytes of each item.
   * @return Opaque queue handle, or nullptr on failure / in TEST_HOST mode.
   */
  static RtosQueueHandle queue_create(uint32_t length, uint32_t item_size);

  /**
   * @brief Delete a FreeRTOS queue created with queue_create().
   *
   * No-op in TEST_HOST mode or when queue_handle is nullptr.
   *
   * @param queue_handle Opaque queue handle from queue_create().
   */
  static void queue_delete(RtosQueueHandle queue_handle);

  /**
   * @brief Send an item to a FreeRTOS queue.
   *
   * Thread-safe: yes (FreeRTOS queue)
   * TEST_HOST delegates to the installed RTOS instance.
   *
   * @param queue_handle Opaque queue handle (QueueHandle_t on hardware).
   * @param item         Pointer to the item to copy into the queue.
   * @param timeout_ms   Milliseconds to wait if the queue is full; 0 = return
   *                     immediately.
   * @return true if the item was admitted; false for invalid arguments, a full
   *         queue, timeout, or no TEST_HOST instance.
   */
  static bool queue_send(RtosQueueHandle queue_handle, const void *item, uint32_t timeout_ms = 0);

  /**
   * @brief Receive an item from a FreeRTOS queue.
   *
   * Thread-safe: yes (FreeRTOS queue)
   * Always returns false in TEST_HOST mode.
   *
   * @param queue_handle Opaque queue handle (QueueHandle_t on hardware).
   * @param item         Buffer to receive the item into.
   * @param timeout_ms   Maximum time to wait; 0 = non-blocking,
   *                     UINT32_MAX = wait indefinitely (portMAX_DELAY).
   * @return true if an item was received; false on timeout or TEST_HOST.
   */
  static bool queue_receive(RtosQueueHandle queue_handle, void *item, uint32_t timeout_ms);

  /**
   * @brief Send an item to a FreeRTOS queue from ISR context.
   *
   * ISR-safe: yes
   * No-op in TEST_HOST mode.
   * Calls portYIELD_FROM_ISR() internally when a higher-priority task is
   * woken.
   *
   * @param queue_handle Opaque queue handle (QueueHandle_t on hardware).
   * @param item         Pointer to the item to copy into the queue.
   */
  static void queue_send_from_isr(RtosQueueHandle queue_handle, const void *item);

  // -----------------------------------------------------------------------
  // Task notifications
  // -----------------------------------------------------------------------

  /**
   * @brief Lightweight signal to a task (give pattern).
   *
   * Increments the receiving task's notification value.
   * No-op in TEST_HOST mode.
   *
   * @param task_handle Opaque task handle from task_create().
   */
  static void task_notify_give(RtosTaskHandle task_handle);

  /**
   * @brief Wait for a lightweight signal (take pattern).
   *
   * Blocks the calling task until the notification count is non-zero, then
   * clears it.  No-op in TEST_HOST mode (returns false).
   *
   * @param timeout_ms Maximum wait time; UINT32_MAX = wait indefinitely.
   * @return true if a notification was received; false on timeout or
   *         TEST_HOST.
   */
  static bool task_notify_take(uint32_t timeout_ms);

  /**
   * @brief Send a 32-bit value to a task via notification.
   *
   * Overwrites any pending notification value.
   * Delegates to the installed mock in TEST_HOST mode (default no-op).
   *
   * @param task_handle Opaque task handle from task_create().
   * @param value       32-bit value to deliver.
   */
  static void task_notify_send(RtosTaskHandle task_handle, uint32_t value);

  /**
   * @brief Wait for a 32-bit value notification.
   *
   * Blocks the calling task until a notification arrives, then writes the
   * value into *out_value.  No-op in TEST_HOST mode (returns false).
   *
   * @param out_value   Receives the 32-bit notification value.
   * @param timeout_ms  Maximum wait time; UINT32_MAX = wait indefinitely.
   * @return true if a notification was received; false on timeout or
   *         TEST_HOST.
   */
  static bool task_notify_wait(uint32_t *out_value, uint32_t timeout_ms);

  // -----------------------------------------------------------------------
  // System clock
  // -----------------------------------------------------------------------

  /**
   * @brief Apply a UTC epoch timestamp to the platform system clock.
   *
   * Delegates to set_system_time_from_epoch_impl() in TEST_HOST mode when a
   * test RTOS instance is installed.
   *
   * @param epoch_seconds POSIX epoch (seconds since 1970-01-01T00:00:00Z).
   */
  static void set_system_time_from_epoch(int64_t epoch_seconds);

  /**
   * @brief Virtual implementation of system clock setting (mockable).
   * @param epoch_seconds POSIX epoch (seconds since 1970-01-01T00:00:00Z).
   */
  virtual void set_system_time_from_epoch_impl(int64_t epoch_seconds);

  /**
   * @brief Virtual implementation of delay (mockable)
   * @param ms Milliseconds to delay
   */
  virtual void delay_ms_impl(uint32_t ms) = 0;

  /**
   * @brief Virtual implementation of get_time_ms (mockable)
   * @return Time in milliseconds
   */
  virtual uint64_t get_time_ms_impl() = 0;

  /**
   * @brief Virtual implementation of get_retained_time_ms (mockable).
   * @return Retained monotonic time in milliseconds; defaults to 0 on host.
   */
  virtual uint64_t get_retained_time_ms_impl();

  /// Host hook for value notifications; default is a no-op.
  virtual void task_notify_send_impl(RtosTaskHandle task_handle, uint32_t value);

  /**
   * @brief Virtual implementation of task_notify_wait (mockable).
   *
   * Called by the static task_notify_wait() in TEST_HOST mode when a
   * singleton instance is installed. Default returns false (timeout).
   */
  virtual bool task_notify_wait_impl(uint32_t *out_value, uint32_t timeout_ms);

  /**
   * @brief Virtual implementation of queue_create (mockable).
   *
   * Called by the static queue_create() in TEST_HOST mode when a singleton
   * instance is installed. Default allocates a simple in-memory queue.
   */
  virtual RtosQueueHandle queue_create_impl(uint32_t length, uint32_t item_size);

  /**
   * @brief Virtual implementation of queue_delete (mockable).
   *
   * Called by the static queue_delete() in TEST_HOST mode when a singleton
   * instance is installed. Default frees the in-memory queue.
   */
  virtual void queue_delete_impl(RtosQueueHandle queue_handle);

  /**
   * @brief Virtual implementation of queue_send (mockable).
   *
   * Called by the static queue_send() in TEST_HOST mode when a singleton
   * instance is installed. Default pushes into the in-memory queue and returns
   * whether the item was admitted.
   */
  virtual bool queue_send_impl(RtosQueueHandle queue_handle, const void *item, uint32_t timeout_ms);

  /**
   * @brief Virtual implementation of queue_receive (mockable).
   *
   * Called by the static queue_receive() in TEST_HOST mode when a singleton
   * instance is installed. Default pops from the in-memory queue (non-blocking).
   */
  virtual bool queue_receive_impl(RtosQueueHandle queue_handle, void *item, uint32_t timeout_ms);

private:
  static RTOS *get_instance();
};

/**
 * @brief FreeRTOS implementation of RTOS interface
 *
 * Uses vTaskDelay() and esp_timer_get_time() for ESP-IDF platform.
 */
class FreeRTOS : public RTOS {
public:
  void delay_ms_impl(uint32_t ms) override;
  uint64_t get_time_ms_impl() override;
  uint64_t get_retained_time_ms_impl() override;
};

/**
 * @brief RAII mutex wrapper.
 *
 * Wraps a FreeRTOS mutex semaphore.  The mutex is created in the constructor
 * and deleted in the destructor.  In TEST_HOST mode all operations are no-ops
 * and is_valid() returns false.
 *
 * Thread-safe: yes (FreeRTOS mutex semantics on hardware)
 * Blocking: lock() waits indefinitely until the mutex is available.
 * Not copyable or movable.
 */
class RtosMutex {
public:
  /// Create the underlying mutex.  is_valid() returns false if creation fails
  /// or when running under TEST_HOST.
  RtosMutex();
  ~RtosMutex();

  RtosMutex(const RtosMutex &) = delete;
  RtosMutex &operator=(const RtosMutex &) = delete;

  /// Acquire the mutex, waiting indefinitely.
  /// @return true on success; always true in TEST_HOST mode.
  bool lock();

  /// Release the mutex.
  void unlock();

  /// @return true if the underlying handle was created successfully.
  bool is_valid() const;

private:
  RtosSemaphoreHandle _handle; ///< Typed semaphore handle; nullptr in TEST_HOST.
};

/**
 * @brief RAII binary semaphore wrapper with deferred creation.
 *
 * Unlike RtosMutex, the semaphore handle is not created in the constructor.
 * Call create() before any take()/give() calls.  This matches the FreeRTOS
 * pattern where a completion semaphore is created inside the task that will
 * later signal it.
 *
 * In TEST_HOST mode all operations are no-ops.
 * Not copyable or movable.
 */
class RtosBinarySemaphore {
public:
  RtosBinarySemaphore() = default;
  ~RtosBinarySemaphore();

  RtosBinarySemaphore(const RtosBinarySemaphore &) = delete;
  RtosBinarySemaphore &operator=(const RtosBinarySemaphore &) = delete;

  /// Create the underlying binary semaphore.
  /// @return true on success; always true in TEST_HOST mode.
  bool create();

  /// Wait indefinitely for the semaphore to be given.
  /// @return true on success; always true in TEST_HOST mode.
  bool take();

  /// Wait up to @p timeout_ms for the semaphore to be given.
  /// @param timeout_ms Maximum wait time; UINT32_MAX = wait indefinitely
  ///                   (portMAX_DELAY).
  /// @return true if acquired; false on timeout; always true in TEST_HOST mode.
  bool take(uint32_t timeout_ms);

  /// Signal (give) the semaphore.
  /// @return true on success; always true in TEST_HOST mode.
  bool give();

  /// Delete the underlying semaphore and reset to the uncreated state.
  void destroy();

  /// @return true if create() has been called and the handle is live.
  bool is_created() const;

private:
  /// Typed semaphore handle; non-null sentinel after create() in TEST_HOST.
  RtosSemaphoreHandle _handle = nullptr;
};

#endif // RTOS_H
