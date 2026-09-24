/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "sd_log_capture.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr const char *TAG = "SdLog";

namespace {

// Room for a few seconds of a busy boot while the e-paper holds the bus.
constexpr size_t RING_BYTES = 16 * 1024;
// The first bench run reported 1348 B left of 4096 after a sync. The deepest
// path is deeper than that: an I2C error while moving the chip-select logs
// from inside the expander, inside the SD host, inside FATFS, and that log
// line runs a full printf formatter on this stack. Sized for that, not for
// the path that happened to be measured.
constexpr uint32_t WRITER_STACK_BYTES = 6144;
constexpr UBaseType_t WRITER_PRIORITY = 2;
constexpr uint32_t RECEIVE_WAIT_MS = 250;
// Lines reach the card's sector buffer as they come, but the directory entry
// — the file size — only moves on a sync. Power lost between syncs costs at
// most this much log; syncing every line would put the two expander writes of
// every card command on every line instead.
constexpr uint32_t SYNC_PERIOD_MS = 2000;
// The directory holds the mount point plus "/logs"; a file path adds
// "/boot-", up to ten digits and ".log", so the two are sized together.
constexpr size_t DIR_BYTES = 48;
constexpr size_t PATH_BYTES = DIR_BYTES + 32;
constexpr size_t HEADER_BYTES = 192;

enum class State : uint8_t { Idle, Running, Stopped };

std::atomic<State> s_state{State::Idle};
std::atomic<vprintf_like_t> s_console{nullptr};
std::atomic<bool> s_accepting{false};
std::atomic<int> s_inflight{0};
std::atomic<uint32_t> s_dropped{0};
std::atomic<bool> s_stop_requested{false};

RingbufHandle_t s_ring = nullptr;
SemaphoreHandle_t s_writer_done = nullptr;
SdCardStorage *s_card = nullptr;
bool s_new_file = true;
char s_header[HEADER_BYTES] = {};

uint32_t now_ms() { return static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS); }

// Runs on whichever task logged, possibly several at once: esp_log_va() calls
// the print function without holding any lock. Formats in place in the ring
// rather than into a stack buffer first — the length is measured, a slot of
// exactly that size is taken, and the line is formatted straight into it. A
// line buffer here would sit on the stack of every task that logs, on top of
// the formatter it is already running, and a task with a tight stack would
// then overflow only when a card happens to be in the slot.
int tee_vprintf(const char *fmt, va_list args) {
  s_inflight.fetch_add(1);
  if (s_accepting.load() && !xPortInIsrContext()) {
    va_list measure;
    va_copy(measure, args);
    const int len = vsnprintf(nullptr, 0, fmt, measure);
    va_end(measure);
    if (len > 0) {
      void *slot = nullptr;
      // +1 for the terminator vsnprintf always writes; the writer drops it.
      if (xRingbufferSendAcquire(s_ring, &slot, static_cast<size_t>(len) + 1, 0) == pdTRUE) {
        va_list write;
        va_copy(write, args);
        vsnprintf(static_cast<char *>(slot), static_cast<size_t>(len) + 1, fmt, write);
        va_end(write);
        xRingbufferSendComplete(s_ring, slot);
      } else {
        s_dropped.fetch_add(1);
      }
    }
  }
  s_inflight.fetch_sub(1);

  // Default to the stock sink for the instant between installing this hook
  // and learning what it replaced.
  const vprintf_like_t console = s_console.load();
  return (console != nullptr ? console : &vprintf)(fmt, args);
}

bool write_all(int fd, const char *data, size_t len) {
  while (len > 0) {
    const ssize_t n = write(fd, data, len);
    if (n <= 0) {
      return false;
    }
    data += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

/// Highest N among boot-N.log in dir, or 0 when there are none.
unsigned newest_log_number(const char *dir) {
  DIR *d = opendir(dir);
  if (d == nullptr) {
    return 0;
  }
  unsigned newest = 0;
  while (struct dirent *e = readdir(d)) {
    if (strncasecmp(e->d_name, "boot-", 5) != 0) {
      continue;
    }
    char *end = nullptr;
    const unsigned long n = std::strtoul(e->d_name + 5, &end, 10);
    if (end != e->d_name + 5 && strcasecmp(end, ".log") == 0 && n > newest) {
      newest = static_cast<unsigned>(n);
    }
  }
  closedir(d);
  return newest;
}

int open_log(const char *mount, bool new_file, char *path) {
  char dir[DIR_BYTES];
  const int dir_len = std::snprintf(dir, sizeof(dir), "%s/logs", mount);
  if (dir_len < 0 || static_cast<size_t>(dir_len) >= sizeof(dir)) {
    ESP_LOGW(TAG, "mount path %s is too long for the log directory", mount);
    return -1;
  }
  if (mkdir(dir, 0775) != 0 && errno != EEXIST) {
    ESP_LOGW(TAG, "cannot create %s: %s", dir, std::strerror(errno));
    return -1;
  }
  const unsigned newest = newest_log_number(dir);
  const unsigned number = (new_file || newest == 0) ? newest + 1 : newest;
  std::snprintf(path, PATH_BYTES, "%s/boot-%04u.log", dir, number);
  const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0664);
  if (fd < 0) {
    ESP_LOGW(TAG, "cannot open %s: %s", path, std::strerror(errno));
  }
  return fd;
}

/// Stop capturing and give back everything capture took. Safe from the writer
/// task only: it is the one consumer of the ring.
void release_capture() {
  s_accepting.store(false);
  // A hook call that saw s_accepting true before the store above may still be
  // inside the ring; once the count drops to zero, none is, and none can start.
  while (s_inflight.load() != 0) {
    vTaskDelay(1);
  }
  const vprintf_like_t console = s_console.load();
  esp_log_set_vprintf(console != nullptr ? console : &vprintf);
  vRingbufferDelete(s_ring);
  s_ring = nullptr;
}

void writer_task(void *) {
  char path[PATH_BYTES] = {};
  int fd = -1;

  if (!s_card->is_mounted() && !s_card->init()) {
    ESP_LOGW(TAG, "no card mounted — log stays on the console only");
  } else {
    fd = open_log(s_card->mount_path(), s_new_file, path);
  }

  if (fd >= 0) {
    char line[HEADER_BYTES + 16];
    const int n = std::snprintf(line, sizeof(line), "%s==== %s ====\n", s_new_file ? "" : "\n",
                                s_header);
    if (n > 0 && write_all(fd, line, std::min(static_cast<size_t>(n), sizeof(line) - 1))) {
      ESP_LOGI(TAG, "%s %s", s_new_file ? "logging to" : "appending to", path);
    } else {
      ESP_LOGW(TAG, "header write to %s failed: %s", path, std::strerror(errno));
      close(fd);
      fd = -1;
    }
  }

  uint32_t last_sync = now_ms();
  bool unsynced = false;
  bool reported_stack = false;
  while (fd >= 0) {
    size_t size = 0;
    auto *item = static_cast<char *>(xRingbufferReceive(s_ring, &size, pdMS_TO_TICKS(RECEIVE_WAIT_MS)));
    if (item != nullptr) {
      const bool ok = write_all(fd, item, strnlen(item, size));
      vRingbufferReturnItem(s_ring, item);
      if (!ok) {
        ESP_LOGE(TAG, "write to %s failed (%s) — card removed or full; capture stopped", path,
                 std::strerror(errno));
        break;
      }
      unsynced = true;
    }

    const uint32_t dropped = s_dropped.exchange(0);
    if (dropped != 0) {
      char note[64];
      const int n = std::snprintf(note, sizeof(note),
                                  "[sd-log] %lu line(s) lost here: log buffer was full\n",
                                  static_cast<unsigned long>(dropped));
      write_all(fd, note, static_cast<size_t>(n));
      unsynced = true;
    }

    const bool draining_done = item == nullptr && s_stop_requested.load();
    if (unsynced && (draining_done || now_ms() - last_sync >= SYNC_PERIOD_MS)) {
      if (fsync(fd) != 0) {
        ESP_LOGE(TAG, "sync of %s failed (%s) — capture stopped", path, std::strerror(errno));
        break;
      }
      last_sync = now_ms();
      unsynced = false;
      if (!reported_stack) {
        ESP_LOGI(TAG, "writer stack headroom %u B",
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        reported_stack = true;
      }
    }
    if (draining_done) {
      break;
    }
  }

  if (fd >= 0) {
    fsync(fd);
    close(fd);
  }
  release_capture();
  // Unmounting is what leaves FATFS consistent if the card is pulled or power
  // goes; everything above has been synced by now.
  s_card->deinit();
  s_state.store(State::Stopped);
  xSemaphoreGive(s_writer_done);
  vTaskDelete(nullptr);
}

void stop_for_restart() { sd_log::stop(1000); }

} // namespace

namespace sd_log {

bool start(SdCardStorage &card, bool new_file, const char *header) {
  State expected = State::Idle;
  if (!s_state.compare_exchange_strong(expected, State::Running)) {
    return false;
  }

  s_card = &card;
  s_new_file = new_file;
  std::snprintf(s_header, sizeof(s_header), "%s", header != nullptr ? header : "");
  s_stop_requested.store(false);
  s_dropped.store(0);

  s_ring = xRingbufferCreate(RING_BYTES, RINGBUF_TYPE_NOSPLIT);
  s_writer_done = (s_writer_done != nullptr) ? s_writer_done : xSemaphoreCreateBinary();
  if (s_ring == nullptr || s_writer_done == nullptr) {
    ESP_LOGE(TAG, "out of memory for the log buffer — not capturing");
    if (s_ring != nullptr) {
      vRingbufferDelete(s_ring);
      s_ring = nullptr;
    }
    s_state.store(State::Stopped);
    return false;
  }

  // The ring exists before the hook can reach it.
  s_accepting.store(true);
  s_console.store(esp_log_set_vprintf(&tee_vprintf));

  if (xTaskCreate(&writer_task, "sd_log", WRITER_STACK_BYTES, nullptr, WRITER_PRIORITY, nullptr) !=
      pdPASS) {
    ESP_LOGE(TAG, "cannot start the writer task — not capturing");
    s_accepting.store(false);
    while (s_inflight.load() != 0) {
      vTaskDelay(1);
    }
    esp_log_set_vprintf(s_console.load());
    vRingbufferDelete(s_ring);
    s_ring = nullptr;
    s_state.store(State::Stopped);
    return false;
  }

  // esp_restart() runs these first, including the restart after an OTA.
  // A panic does not, so a crash keeps what was synced before it.
  const esp_err_t err = esp_register_shutdown_handler(&stop_for_restart);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGW(TAG, "no restart hook (%s): a software restart may lose the last %lu ms",
             esp_err_to_name(err), static_cast<unsigned long>(SYNC_PERIOD_MS));
  }
  return true;
}

bool stop(uint32_t timeout_ms) {
  if (s_state.load() != State::Running) {
    return true;
  }
  // Freeze what goes to the card at this line, and let any line already on its
  // way into the ring land before the writer is told to finish: it stops at the
  // first empty receive, and a line still being formatted is not in the ring
  // yet.
  s_accepting.store(false);
  while (s_inflight.load() != 0) {
    vTaskDelay(1);
  }
  s_stop_requested.store(true);
  if (xSemaphoreTake(s_writer_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
    ESP_LOGW(TAG, "log writer still busy after %lu ms — the tail may be lost",
             static_cast<unsigned long>(timeout_ms));
    return false;
  }
  return true;
}

} // namespace sd_log
