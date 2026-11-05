/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "rtos.h"

#ifndef TEST_HOST
#include <ctime>
#include <sys/time.h>

#include "esp_rtc_time.h"
#include "esp_timer.h"
#else
#include <cstdlib>
#include <cstring>
#endif

// ---------------------------------------------------------------------------
// RTOS singleton
// ---------------------------------------------------------------------------

// Singleton instance storage
static RTOS *s_rtos_instance = nullptr;

RTOS *RTOS::get_instance() {
#ifndef TEST_HOST
  if (s_rtos_instance == nullptr) {
    // Auto-create default implementation if not set (production only)
    static FreeRTOS default_impl;
    s_rtos_instance = &default_impl;
  }
#endif
  return s_rtos_instance;
}

void RTOS::set_instance(RTOS *rtos) { s_rtos_instance = rtos; }

// ---------------------------------------------------------------------------
// Timing facade (mockable via virtual singleton)
// ---------------------------------------------------------------------------

// Static facade methods - delegate to singleton instance
void RTOS::delay_ms(uint32_t ms) { get_instance()->delay_ms_impl(ms); }

uint64_t RTOS::get_time_ms() { return get_instance()->get_time_ms_impl(); }

uint64_t RTOS::get_retained_time_ms() {
  RTOS *rtos = get_instance();
  return rtos != nullptr ? rtos->get_retained_time_ms_impl() : 0;
}

uint64_t RTOS::get_retained_time_ms_impl() { return 0; }

// FreeRTOS implementation
void FreeRTOS::delay_ms_impl(uint32_t ms) {
#ifndef TEST_HOST
  vTaskDelay(pdMS_TO_TICKS(ms));
#endif
}

uint64_t FreeRTOS::get_time_ms_impl() {
#ifndef TEST_HOST
  return esp_timer_get_time() / 1000;
#else
  return 0;
#endif
}

uint64_t FreeRTOS::get_retained_time_ms_impl() {
#ifndef TEST_HOST
  return esp_rtc_get_time_us() / 1000ULL;
#else
  return 0;
#endif
}

// ---------------------------------------------------------------------------
// Task lifecycle (no-op stubs in TEST_HOST)
// ---------------------------------------------------------------------------

bool RTOS::task_create(void (*func)(void *), const char *name, uint32_t stack_depth, void *param,
                       uint32_t priority, RtosTaskHandle *out_handle) {
#ifndef TEST_HOST
  RtosTaskHandle handle = nullptr;
  const BaseType_t ret = xTaskCreate(func, name, static_cast<configSTACK_DEPTH_TYPE>(stack_depth),
                                     param, static_cast<UBaseType_t>(priority), &handle);
  if (out_handle != nullptr) {
    *out_handle = handle;
  }
  return ret == pdPASS;
#else
  (void)func;
  (void)name;
  (void)stack_depth;
  (void)param;
  (void)priority;
  (void)out_handle;
  return false;
#endif
}

void RTOS::task_delete(RtosTaskHandle handle) {
#ifndef TEST_HOST
  vTaskDelete(handle);
#else
  (void)handle;
#endif
}

// ---------------------------------------------------------------------------
// Queue (no-op stubs in TEST_HOST)
// ---------------------------------------------------------------------------

RtosQueueHandle RTOS::queue_create(uint32_t length, uint32_t item_size) {
#ifndef TEST_HOST
  return xQueueCreate(static_cast<UBaseType_t>(length), static_cast<UBaseType_t>(item_size));
#else
  RTOS *rtos = get_instance();
  if (rtos != nullptr) {
    return rtos->queue_create_impl(length, item_size);
  }
  return nullptr;
#endif
}

void RTOS::queue_delete(RtosQueueHandle queue_handle) {
#ifndef TEST_HOST
  if (queue_handle != nullptr) {
    vQueueDelete(queue_handle);
  }
#else
  RTOS *rtos = get_instance();
  if (rtos != nullptr) {
    rtos->queue_delete_impl(queue_handle);
  }
#endif
}

bool RTOS::queue_send(RtosQueueHandle queue_handle, const void *item, uint32_t timeout_ms) {
  if (queue_handle == nullptr || item == nullptr) {
    return false;
  }
#ifndef TEST_HOST
  return xQueueSend(queue_handle, item, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
#else
  RTOS *rtos = get_instance();
  if (rtos != nullptr) {
    return rtos->queue_send_impl(queue_handle, item, timeout_ms);
  }
  return false;
#endif
}

bool RTOS::queue_receive(RtosQueueHandle queue_handle, void *item, uint32_t timeout_ms) {
#ifndef TEST_HOST
  const TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xQueueReceive(queue_handle, item, ticks) == pdTRUE;
#else
  RTOS *rtos = get_instance();
  if (rtos != nullptr) {
    return rtos->queue_receive_impl(queue_handle, item, timeout_ms);
  }
  return false;
#endif
}

void RTOS::queue_send_from_isr(RtosQueueHandle queue_handle, const void *item) {
#ifndef TEST_HOST
  BaseType_t woken = pdFALSE;
  xQueueSendFromISR(queue_handle, item, &woken);
  portYIELD_FROM_ISR(woken);
#else
  (void)queue_handle;
  (void)item;
#endif
}

// ---------------------------------------------------------------------------
// Task notifications (no-op stubs in TEST_HOST)
// ---------------------------------------------------------------------------

void RTOS::task_notify_give(RtosTaskHandle task_handle) {
#ifndef TEST_HOST
  xTaskNotifyGive(task_handle);
#else
  (void)task_handle;
#endif
}

bool RTOS::task_notify_take(uint32_t timeout_ms) {
#ifndef TEST_HOST
  const TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return ulTaskNotifyTake(pdTRUE, ticks) != 0;
#else
  (void)timeout_ms;
  return false;
#endif
}

void RTOS::task_notify_send(RtosTaskHandle task_handle, uint32_t value) {
#ifndef TEST_HOST
  xTaskNotify(task_handle, value, eSetValueWithOverwrite);
#else
  RTOS *rtos = get_instance();
  if (rtos != nullptr) {
    rtos->task_notify_send_impl(task_handle, value);
  }
#endif
}

bool RTOS::task_notify_wait(uint32_t *out_value, uint32_t timeout_ms) {
#ifndef TEST_HOST
  const TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xTaskNotifyWait(0, 0, out_value, ticks) == pdTRUE;
#else
  RTOS *rtos = get_instance();
  if (rtos != nullptr) {
    return rtos->task_notify_wait_impl(out_value, timeout_ms);
  }
  return false;
#endif
}

// ---------------------------------------------------------------------------
// System clock
// ---------------------------------------------------------------------------

void RTOS::set_system_time_from_epoch(int64_t epoch_seconds) {
#ifndef TEST_HOST
  const struct timeval tv = {static_cast<time_t>(epoch_seconds), 0};
  settimeofday(&tv, nullptr);
#else
  RTOS *rtos = get_instance();
  if (rtos != nullptr) {
    rtos->set_system_time_from_epoch_impl(epoch_seconds);
  }
#endif
}

void RTOS::set_system_time_from_epoch_impl(int64_t epoch_seconds) { (void)epoch_seconds; }

void RTOS::task_notify_send_impl(RtosTaskHandle task_handle, uint32_t value) {
  (void)task_handle;
  (void)value;
}

bool RTOS::task_notify_wait_impl(uint32_t *out_value, uint32_t timeout_ms) {
  (void)out_value;
  (void)timeout_ms;
  return false;
}

// ---------------------------------------------------------------------------
// Test queue -- simple ring buffer used by default _impl methods
// ---------------------------------------------------------------------------

#ifdef TEST_HOST

/// In-memory ring buffer backing test queues.
struct TestQueue {
  uint8_t *buf;
  uint32_t item_size;
  uint32_t capacity;
  uint32_t head;
  uint32_t tail;
  uint32_t count;
};

static TestQueue *as_tq(RtosQueueHandle h) { return static_cast<TestQueue *>(h); }

#endif // TEST_HOST

RtosQueueHandle RTOS::queue_create_impl(uint32_t length, uint32_t item_size) {
#ifdef TEST_HOST
  auto *tq = static_cast<TestQueue *>(std::malloc(sizeof(TestQueue)));
  if (tq == nullptr) {
    return nullptr;
  }
  tq->buf = static_cast<uint8_t *>(std::malloc(length * item_size));
  if (tq->buf == nullptr) {
    std::free(tq);
    return nullptr;
  }
  tq->item_size = item_size;
  tq->capacity = length;
  tq->head = 0;
  tq->tail = 0;
  tq->count = 0;
  return static_cast<RtosQueueHandle>(tq);
#else
  (void)length;
  (void)item_size;
  return nullptr;
#endif
}

void RTOS::queue_delete_impl(RtosQueueHandle queue_handle) {
#ifdef TEST_HOST
  if (queue_handle != nullptr) {
    auto *tq = as_tq(queue_handle);
    std::free(tq->buf);
    std::free(tq);
  }
#else
  (void)queue_handle;
#endif
}

bool RTOS::queue_send_impl(RtosQueueHandle queue_handle, const void *item, uint32_t timeout_ms) {
#ifdef TEST_HOST
  (void)timeout_ms;
  if (queue_handle == nullptr || item == nullptr) {
    return false;
  }
  auto *tq = as_tq(queue_handle);
  if (tq->count >= tq->capacity) {
    return false;
  }
  std::memcpy(tq->buf + tq->head * tq->item_size, item, tq->item_size);
  tq->head = (tq->head + 1) % tq->capacity;
  tq->count++;
  return true;
#else
  (void)queue_handle;
  (void)item;
  (void)timeout_ms;
  return false;
#endif
}

bool RTOS::queue_receive_impl(RtosQueueHandle queue_handle, void *item, uint32_t timeout_ms) {
#ifdef TEST_HOST
  (void)timeout_ms;
  if (queue_handle == nullptr || item == nullptr) {
    return false;
  }
  auto *tq = as_tq(queue_handle);
  if (tq->count == 0) {
    return false;
  }
  std::memcpy(item, tq->buf + tq->tail * tq->item_size, tq->item_size);
  tq->tail = (tq->tail + 1) % tq->capacity;
  tq->count--;
  return true;
#else
  (void)queue_handle;
  (void)item;
  (void)timeout_ms;
  return false;
#endif
}

// ---------------------------------------------------------------------------
// RtosMutex
// ---------------------------------------------------------------------------

RtosMutex::RtosMutex() : _handle(nullptr) {
#ifndef TEST_HOST
  _handle = xSemaphoreCreateMutex();
#endif
}

RtosMutex::~RtosMutex() {
#ifndef TEST_HOST
  if (_handle != nullptr) {
    vSemaphoreDelete(_handle);
    _handle = nullptr;
  }
#endif
}

bool RtosMutex::lock() {
#ifndef TEST_HOST
  return xSemaphoreTake(_handle, portMAX_DELAY) == pdTRUE;
#else
  return true;
#endif
}

void RtosMutex::unlock() {
#ifndef TEST_HOST
  xSemaphoreGive(_handle);
#endif
}

bool RtosMutex::is_valid() const { return _handle != nullptr; }

// ---------------------------------------------------------------------------
// RtosBinarySemaphore
// ---------------------------------------------------------------------------

RtosBinarySemaphore::~RtosBinarySemaphore() { destroy(); }

bool RtosBinarySemaphore::create() {
#ifndef TEST_HOST
  _handle = xSemaphoreCreateBinary();
  return _handle != nullptr;
#else
  // Use a non-null sentinel so is_created() returns true on TEST_HOST.
  // The value is never dereferenced outside of #ifndef TEST_HOST guards.
  _handle = reinterpret_cast<RtosSemaphoreHandle>(1);
  return true;
#endif
}

bool RtosBinarySemaphore::take() {
#ifndef TEST_HOST
  return xSemaphoreTake(_handle, portMAX_DELAY) == pdTRUE;
#else
  return true;
#endif
}

bool RtosBinarySemaphore::take(uint32_t timeout_ms) {
#ifndef TEST_HOST
  const TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTake(_handle, ticks) == pdTRUE;
#else
  (void)timeout_ms;
  return true;
#endif
}

bool RtosBinarySemaphore::give() {
#ifndef TEST_HOST
  return xSemaphoreGive(_handle) == pdTRUE;
#else
  return true;
#endif
}

void RtosBinarySemaphore::destroy() {
#ifndef TEST_HOST
  if (_handle != nullptr) {
    vSemaphoreDelete(_handle);
  }
#endif
  _handle = nullptr;
}

bool RtosBinarySemaphore::is_created() const { return _handle != nullptr; }
