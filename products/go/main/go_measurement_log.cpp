#include "go_storage.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rtos.h"
#include "ag_log.h"

namespace {
constexpr const char *TAG = "MeasurementLog";
constexpr uint32_t LOG_MAGIC = 0x41474c31;
constexpr uint32_t LOG_VERSION = 1;
constexpr size_t LOG_PATH_SIZE = 256;
constexpr int64_t MIN_VALID_UTC = 1577836800; // 2020-01-01
struct LogHeader {
  uint32_t magic = LOG_MAGIC;
  uint32_t version = LOG_VERSION;
  uint32_t record_size = sizeof(MeasurementLogRecord);
  uint32_t sequence = 0;
  MeasurementLogBoundary boundary = MeasurementLogBoundary::ColdBoot;
};
static_assert(sizeof(LogHeader) == 20);

bool valid_header(const LogHeader &h, uint32_t sequence) {
  return h.magic == LOG_MAGIC && h.version == LOG_VERSION &&
         h.record_size == sizeof(MeasurementLogRecord) && h.sequence == sequence && sequence != 0 &&
         h.boundary >= MeasurementLogBoundary::ColdBoot &&
         h.boundary <= MeasurementLogBoundary::RecordLimit;
}
bool sync_file(FILE *file) { return fflush(file) == 0 && fsync(fileno(file)) == 0; }
} // namespace

void StorageService::format_log_path(uint32_t sequence, char *out, size_t capacity) const {
  snprintf(out, capacity, "%s/measures/log_%010" PRIu32 ".bin", mount_path(), sequence);
}

bool StorageService::has_measurement_space() const {
  uint32_t free_kb = 0;
  return measurement_free_kb(free_kb) && free_kb > STORAGE_RESERVE_KB;
}

bool StorageService::set_log_boundary(MeasurementLogBoundary reason) {
  if (reason < MeasurementLogBoundary::ColdBoot || reason > MeasurementLogBoundary::RecordLimit)
    return false;
  const bool ok = close_measurements();
  if (!ok)
    AG_LOGE(TAG, "failed to sync log at boundary");
  _log_sequence = 0;
  _log_boundary = reason;
  return ok;
}

bool StorageService::sync_measurements() {
  if (_log_file == nullptr)
    return true;
#ifdef TEST_HOST
  if (_test_seam != nullptr) {
    ++_test_seam->fflush_count;
    if (_test_seam->fflush_return != 0)
      return false;
    ++_test_seam->fsync_count;
    if (_test_seam->fsync_return != 0)
      return false;
  }
#endif
  return sync_file(_log_file);
}

bool StorageService::close_measurements() {
  if (_log_file == nullptr)
    return true;
  const bool synced = sync_measurements();
  const bool closed = fclose(_log_file) == 0;
  _log_file = nullptr;
  return synced && closed;
}

bool StorageService::measurement_log_info(uint32_t sequence, MeasurementLogInfo &out) const {
  if (!is_mounted() || sequence == 0)
    return false;
  char path[LOG_PATH_SIZE];
  format_log_path(sequence, path, sizeof(path));
  FILE *file = fopen(path, "rb");
  if (file == nullptr)
    return false;
  LogHeader header{};
  struct stat st{};
  const bool valid =
      fread(&header, sizeof(header), 1, file) == 1 && valid_header(header, sequence) &&
      fstat(fileno(file), &st) == 0 && st.st_size >= (off_t)sizeof(header) &&
      (st.st_size - sizeof(header)) / sizeof(MeasurementLogRecord) <= LOG_RECORD_LIMIT;
  fclose(file);
  if (!valid)
    return false;
  out = {};
  out.sequence = sequence;
  out.boundary = header.boundary;
  out.count = (st.st_size - sizeof(header)) / sizeof(MeasurementLogRecord);
  MeasurementLogRecord record{};
  if (out.count != 0) {
    if (!read_measurement(sequence, 0, record))
      return false;
    out.first_timestamp = record.timestamp;
    if (!read_measurement(sequence, out.count - 1, record))
      return false;
    out.last_timestamp = record.timestamp;
  }
  return true;
}

bool StorageService::next_measurement_log(uint32_t after, MeasurementLogInfo &out) const {
  if (!is_mounted())
    return false;
  char path[LOG_PATH_SIZE];
  snprintf(path, sizeof(path), "%s/measures", mount_path());
  DIR *dir = opendir(path);
  if (dir == nullptr)
    return false;
  uint32_t next = 0;
  while (auto *entry = readdir(dir)) {
    uint32_t id = 0;
    int length = 0;
    if (sscanf(entry->d_name, "log_%10" SCNu32 ".bin%n", &id, &length) == 1 && length != 0 &&
        entry->d_name[length] == '\0' && id > after && (next == 0 || id < next)) {
      MeasurementLogInfo candidate{};
      if (measurement_log_info(id, candidate)) {
        next = id;
        out = candidate;
      }
    }
  }
  closedir(dir);
  return next != 0;
}

bool StorageService::read_measurement(uint32_t sequence, uint32_t index,
                                      MeasurementLogRecord &out) const {
  if (!is_mounted() || index >= LOG_RECORD_LIMIT)
    return false;
  char path[LOG_PATH_SIZE];
  format_log_path(sequence, path, sizeof(path));
  FILE *file = fopen(path, "rb");
  if (file == nullptr)
    return false;
  LogHeader header{};
  const bool ok = fread(&header, sizeof(header), 1, file) == 1 && valid_header(header, sequence) &&
                  fseek(file, sizeof(header) + index * sizeof(out), SEEK_SET) == 0 &&
                  fread(&out, sizeof(out), 1, file) == 1;
  fclose(file);
  return ok;
}

bool StorageService::delete_measurement_log(uint32_t sequence) {
  if (!is_mounted() || sequence == 0 || sequence == _protected_log || sequence == _log_sequence)
    return false;
  char path[LOG_PATH_SIZE];
  format_log_path(sequence, path, sizeof(path));
  if (unlink(path) != 0)
    return false;
#ifdef TEST_HOST
  if (_test_seam != nullptr) {
    ++_test_seam->deleted_logs;
    if (_test_seam->free_kb >= 0)
      _test_seam->free_kb += _test_seam->reclaimed_kb;
  }
#endif
  return true;
}

bool StorageService::reclaim_measurement_space() {
  uint32_t after = 0;
  while (!has_measurement_space()) {
    uint32_t free_kb = 0;
    if (!measurement_free_kb(free_kb))
      return false;
    MeasurementLogInfo info{};
    if (!next_measurement_log(after, info)) {
      AG_LOGE(TAG, "no eligible measurement log can supply space");
      return false;
    }
    after = info.sequence;
    if (info.sequence == _protected_log || info.sequence == _log_sequence)
      continue;
    if (!delete_measurement_log(info.sequence))
      return false;
  }
  return true;
}

bool StorageService::create_measurement_log() {
  if (!is_mounted())
    return false;
  char path[LOG_PATH_SIZE];
  snprintf(path, sizeof(path), "%s/measures", mount_path());
  if (mkdir(path, 0755) != 0 && errno != EEXIST)
    return false;

  // Persist the sequence before creating a file. A damaged counter fails closed
  // rather than reusing an identifier after old files have been reclaimed.
  snprintf(path, sizeof(path), "%s/measures/sequence.bin", mount_path());
  uint32_t counter[2] = {0, UINT32_MAX};
  FILE *sequence_file = fopen(path, "r+b");
  if (sequence_file != nullptr) {
    struct stat counter_stat{};
    if (fstat(fileno(sequence_file), &counter_stat) != 0 ||
        counter_stat.st_size != sizeof(counter) ||
        fread(counter, sizeof(counter), 1, sequence_file) != 1 || counter[0] != ~counter[1]) {
      fclose(sequence_file);
      return false;
    }
  } else {
    if (errno != ENOENT)
      return false;
    // Missing counter with existing files is not a fresh store. Fail closed.
    char directory[LOG_PATH_SIZE];
    snprintf(directory, sizeof(directory), "%s/measures", mount_path());
    DIR *dir = opendir(directory);
    if (dir == nullptr)
      return false;
    bool empty = true;
    while (auto *entry = readdir(dir)) {
      if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
        empty = false;
    }
    closedir(dir);
    if (!empty)
      return false;
    sequence_file = fopen(path, "w+b");
    if (sequence_file == nullptr)
      return false;
  }
  if (counter[0] == UINT32_MAX) {
    fclose(sequence_file);
    return false;
  }
  const uint32_t next = counter[0] + 1;
  counter[0] = next;
  counter[1] = ~next;
  const bool saved = fseek(sequence_file, 0, SEEK_SET) == 0 &&
                     fwrite(counter, sizeof(counter), 1, sequence_file) == 1 &&
                     sync_file(sequence_file);
  const bool closed = fclose(sequence_file) == 0;
  if (!saved || !closed)
    return false;
  if (!reclaim_measurement_space())
    return false;
  format_log_path(next, path, sizeof(path));
  struct stat st{};
  if (stat(path, &st) == 0 || errno != ENOENT)
    return false;
  _log_file = fopen(path, "wb");
  if (_log_file == nullptr)
    return false;
  LogHeader header{};
  header.sequence = next;
  header.boundary = _log_boundary;
  if (fwrite(&header, sizeof(header), 1, _log_file) != 1 || !sync_measurements()) {
    close_measurements();
    return false;
  }
  _log_sequence = next;
  _log_count = 0;
  _log_sync_ms = FSYNC_ANCHOR_NONE;
  return true;
}

bool StorageService::resume_measurements(uint32_t sequence) {
  if (_log_file != nullptr)
    return sequence == _log_sequence;
  _log_sequence = sequence; // Failed continuation must not silently create a successor.
  MeasurementLogInfo info{};
  if (!measurement_log_info(sequence, info))
    return false;
  char path[LOG_PATH_SIZE];
  format_log_path(sequence, path, sizeof(path));
  FILE *file = fopen(path, "r+b");
  if (file == nullptr)
    return false;
  const off_t length = sizeof(LogHeader) + info.count * sizeof(MeasurementLogRecord);
  if (ftruncate(fileno(file), length) != 0 || fseek(file, length, SEEK_SET) != 0) {
    fclose(file);
    return false;
  }
  _log_file = file;
  _log_sequence = sequence;
  _log_count = info.count;
  _log_boundary = info.boundary;
  _log_sync_ms = FSYNC_ANCHOR_NONE;
  return true;
}

bool StorageService::append_measurement(const MeasurementLogRecord &record) {
  if (_log_file == nullptr && _log_sequence != 0 && !resume_measurements(_log_sequence))
    return false;
  if (_log_file != nullptr && _log_count >= LOG_RECORD_LIMIT &&
      !set_log_boundary(MeasurementLogBoundary::RecordLimit))
    return false;
  if (!has_measurement_space() && _log_file != nullptr) {
    // Close the writer so older records in this file can also become reclaimable.
    if (!set_log_boundary(MeasurementLogBoundary::RecordLimit))
      return false;
  }
  if (_log_file == nullptr && !create_measurement_log())
    return false;
  MeasurementLogRecord stored = record;
  if (stored.timestamp < MIN_VALID_UTC)
    stored.timestamp = 0;
  size_t bytes = sizeof(stored);
#ifdef TEST_HOST
  if (_test_seam != nullptr && _test_seam->write_bytes >= 0)
    bytes = std::min(bytes, static_cast<size_t>(_test_seam->write_bytes));
#endif
  const size_t written = fwrite(&stored, 1, bytes, _log_file);
  if (written != sizeof(stored)) {
    // Never append another record after a partial write without repairing the tail.
    close_measurements();
    return false;
  }
  ++_log_count;
  const uint32_t now = RTOS::get_time_ms();
  if (_log_sync_ms == FSYNC_ANCHOR_NONE ||
      now - _log_sync_ms >= CONFIG_TRACKING_FSYNC_INTERVAL_MS) {
    if (!sync_measurements())
      return false;
    _log_sync_ms = now;
  }
  return true;
}

bool StorageService::protect_measurement_log(uint32_t sequence, MeasurementLogInfo &snapshot) {
  if (_protected_log != 0 || !sync_measurements() || !measurement_log_info(sequence, snapshot))
    return false;
  _protected_log = sequence;
  return true;
}

bool StorageService::clear_measurements() {
  if (_protected_log != 0 || !close_measurements())
    return false;
  _log_sequence = 0;
  _log_boundary = MeasurementLogBoundary::Enabled;
  char path[LOG_PATH_SIZE];
  snprintf(path, sizeof(path), "%s/measures", mount_path());
  DIR *dir = opendir(path);
  if (dir == nullptr)
    return errno == ENOENT;
  bool ok = true;
  while (auto *entry = readdir(dir)) {
    uint32_t id = 0;
    int length = 0;
    if (sscanf(entry->d_name, "log_%10" SCNu32 ".bin%n", &id, &length) == 1 && length != 0 &&
        entry->d_name[length] == '\0') {
      if (!delete_measurement_log(id))
        ok = false;
    }
  }
  closedir(dir);
  return ok;
}
