/**
 * AirGradient Go — Storage Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_storage.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ag_log.h"
#include "rtos.h"

#ifdef TEST_HOST
#include <sys/statvfs.h>
#else
#include "esp_err.h"
#include "esp_vfs_fat.h"
#endif

static constexpr const char *TAG = "StorageService";

// Maximum buffer size for the routes directory path.
static constexpr size_t MAX_PATH_LEN = 256;

// Maximum buffer size for a fully qualified route file path.
// Worst case: 255-char directory path + '/' + 255-char file name + '\0'.
static constexpr size_t MAX_FILE_PATH_LEN = 512;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

StorageService::StorageService(PayloadCache &cache, NandStorage &nand)
    : _cache(cache), _nand(nand) {}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool StorageService::init() {
  if (!_nand.init()) {
    AG_LOGE(TAG, "init: NAND mount failed");
    return false;
  }

  AG_LOGI(TAG, "init: NAND mounted at %s", _nand.mount_path());
  return true;
}

bool StorageService::is_mounted() const { return _nand.is_mounted(); }

const char *StorageService::mount_path() const { return _nand.mount_path(); }

// ---------------------------------------------------------------------------
// Temporary (chart) operations
// ---------------------------------------------------------------------------

void StorageService::cache_measurement(const MeasuresAGo &m) { _cache.push(m); }

uint16_t StorageService::read_cached_field(CacheField field, float *out, uint16_t max_count) const {
  const uint16_t count = std::min(cached_count(), max_count);
  for (uint16_t i = 0; i < count; ++i) {
    MeasuresAGo entry{};
    _cache.peek_at_index(i, entry);

    switch (field) {
    case CacheField::Temperature:
      out[i] = entry.temp_hum_a.is_temp_valid() ? entry.temp_hum_a.temperature
                                                : MeasuresInvalid::TEMPERATURE;
      break;
    case CacheField::Humidity:
      out[i] =
          entry.temp_hum_a.is_hum_valid() ? entry.temp_hum_a.humidity : MeasuresInvalid::HUMIDITY;
      break;
    case CacheField::PM01:
      out[i] = entry.pm_a.is_pm_01_valid() ? entry.pm_a.pm_01 : MeasuresInvalid::PM;
      break;
    case CacheField::PM25:
      out[i] = entry.pm_a.is_pm_25_valid() ? entry.pm_a.pm_25 : MeasuresInvalid::PM;
      break;
    case CacheField::PM10:
      out[i] = entry.pm_a.is_pm_10_valid() ? entry.pm_a.pm_10 : MeasuresInvalid::PM;
      break;
    case CacheField::CO2:
      out[i] = entry.co2.is_valid() ? static_cast<float>(entry.co2.co2)
                                    : static_cast<float>(MeasuresInvalid::CO2);
      break;
    case CacheField::TvocIndex:
      out[i] = entry.tvoc_nox.is_tvoc_index_valid() ? static_cast<float>(entry.tvoc_nox.tvoc_index)
                                                    : static_cast<float>(MeasuresInvalid::TVOC);
      break;
    case CacheField::NoxIndex:
      out[i] = entry.tvoc_nox.is_nox_index_valid() ? static_cast<float>(entry.tvoc_nox.nox_index)
                                                   : static_cast<float>(MeasuresInvalid::NOX);
      break;
    }
  }
  return count;
}

uint16_t StorageService::read_cache(MeasuresAGo *out, uint16_t max_count) const {
  const uint16_t count = std::min(cached_count(), max_count);
  for (uint16_t i = 0; i < count; ++i) {
    _cache.peek_at_index(i, out[i]);
  }
  return count;
}

uint16_t StorageService::cached_count() const { return _cache.get_size(); }

void StorageService::backup_cache() const { _cache.backup(); }

void StorageService::restore_cache() { _cache.restore(); }

void StorageService::clear_cache() { _cache.clean(); }

// ---------------------------------------------------------------------------
// Persistent (route) operations
// ---------------------------------------------------------------------------

bool StorageService::create_route(uint32_t session_id) {
  if (_route_file != nullptr) {
    AG_LOGW(TAG, "create_route: a route is already active");
    return false;
  }

  if (!_nand.is_mounted()) {
    AG_LOGE(TAG, "create_route: NAND not mounted");
    return false;
  }

  if (!ensure_route_dir()) {
    AG_LOGE(TAG, "create_route: failed to ensure routes directory");
    return false;
  }

  char path[MAX_PATH_LEN];
  format_route_path(session_id, path, sizeof(path));

  // Refuse to "wb"-open over an existing file — that would silently
  // truncate the prior session. Only proceed when stat() confirms the
  // file is absent; any other errno could mask a corrupted directory
  // entry and lose real data.
  struct stat st{};
  if (stat(path, &st) == 0) {
    AG_LOGE(TAG, "create_route: file exists for session %" PRIu32, session_id);
    return false;
  }
  if (errno != ENOENT) {
    AG_LOGE(TAG, "create_route: stat probe failed for %s (errno=%d)", path, errno);
    return false;
  }

  _route_file = fopen(path, "wb");
  if (_route_file == nullptr) {
    AG_LOGE(TAG, "create_route: fopen failed for %s (errno=%d)", path, errno);
    return false;
  }

  // fsync the empty file so the directory entry is durable before
  // start_tracking() tells the user / phone the session exists.
  if (!flush_and_sync_route_file()) {
    AG_LOGE(TAG, "create_route: initial fsync failed (errno=%d)", errno);
    fclose(_route_file);
    _route_file = nullptr;
    return false;
  }

  _current_session_id = session_id;
  _current_point_count = 0;
  _last_fsync_ms = FSYNC_ANCHOR_NONE; // first append syncs unconditionally

  AG_LOGI(TAG, "create_route: opened %s", path);
  return true;
}

bool StorageService::resume_route(uint32_t session_id) {
  if (_route_file != nullptr) {
    AG_LOGW(TAG, "resume_route: a route is already active");
    return false;
  }

  if (!_nand.is_mounted()) {
    AG_LOGE(TAG, "resume_route: NAND not mounted");
    return false;
  }

  if (!ensure_route_dir()) {
    AG_LOGE(TAG, "resume_route: failed to ensure routes directory");
    return false;
  }

  char path[MAX_PATH_LEN];
  format_route_path(session_id, path, sizeof(path));

  struct stat st{};
  if (stat(path, &st) != 0) {
    AG_LOGE(TAG, "resume_route: stat failed for %s (errno=%d)", path, errno);
    return false;
  }

  // Drop a torn trailing record from a prior boot's mid-fwrite crash.
  // ftruncate() on an open fd is portable across ESP-IDF FATFS VFS;
  // path-based truncate() is not.
  const off_t rem = st.st_size % static_cast<off_t>(sizeof(RoutePoint));
  if (rem != 0) {
    FILE *trunc_fp = fopen(path, "rb+");
    if (trunc_fp == nullptr) {
      AG_LOGE(TAG, "resume_route: truncate-open failed for %s (errno=%d)", path, errno);
      return false;
    }
    if (ftruncate(fileno(trunc_fp), st.st_size - rem) != 0) {
      AG_LOGE(TAG, "resume_route: ftruncate failed for %s (errno=%d)", path, errno);
      fclose(trunc_fp);
      return false;
    }
    fclose(trunc_fp);
    AG_LOGW(TAG, "resume_route: dropped %ld torn trailing bytes from %s", static_cast<long>(rem),
            path);
    st.st_size -= rem;
  }

  _route_file = fopen(path, "ab");
  if (_route_file == nullptr) {
    AG_LOGE(TAG, "resume_route: fopen failed for %s (errno=%d)", path, errno);
    return false;
  }

  _current_session_id = session_id;
  _current_point_count = static_cast<uint32_t>(st.st_size / sizeof(RoutePoint));
  _last_fsync_ms = FSYNC_ANCHOR_NONE; // first append syncs unconditionally

  AG_LOGI(TAG, "resume_route: opened %s (%" PRIu32 " points existing)", path, _current_point_count);
  return true;
}

bool StorageService::route_file_exists(uint32_t session_id) const {
  if (!_nand.is_mounted()) {
    return false;
  }

  char path[MAX_PATH_LEN];
  format_route_path(session_id, path, sizeof(path));

  struct stat st{};
  return stat(path, &st) == 0;
}

bool StorageService::append_route_point(const RoutePoint &point) {
  if (_route_file == nullptr) {
    AG_LOGE(TAG, "append_route_point: no active route");
    return false;
  }

  RoutePoint stored_point = point;
  if (!is_fix_valid(stored_point.gps.fix)) {
    // The GPS driver can retain coordinates after losing its fix.
    stored_point.gps.position.latitude = GPS_LATITUDE_INVALID;
    stored_point.gps.position.longitude = GPS_LONGITUDE_INVALID;
    stored_point.gps.altitude_m = GPS_ALTITUDE_INVALID;
  }

  const size_t written = fwrite(&stored_point, sizeof(RoutePoint), 1, _route_file);
  if (written != 1) {
    AG_LOGE(TAG, "append_route_point: fwrite failed (errno=%d)", errno);
    return false;
  }

  // Count what fwrite buffered; the next successful sync will commit it.
  _current_point_count++;

  // Durability budget: sync at most every CONFIG_TRACKING_FSYNC_INTERVAL_MS.
  // FSYNC_ANCHOR_NONE forces the first post-open append to sync, so the
  // first point reaches NAND regardless of cadence or clock value.
  const uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
  const bool sync_due = _last_fsync_ms == FSYNC_ANCHOR_NONE ||
                        (now_ms - _last_fsync_ms) >= CONFIG_TRACKING_FSYNC_INTERVAL_MS;
  if (sync_due) {
    if (!flush_and_sync_route_file()) {
      AG_LOGE(TAG, "append_route_point: flush/sync failed (errno=%d)", errno);
      return false; // anchor preserved so the next attempt still crosses
    }
    _last_fsync_ms = now_ms;
  }
  return true;
}

bool StorageService::end_route() {
  if (_route_file == nullptr) {
    return true;
  }

  const bool synced = flush_and_sync_route_file();
  const bool closed = fclose(_route_file) == 0;
  if (!closed) {
    AG_LOGE(TAG, "end_route: fclose failed (errno=%d)", errno);
  }
  _route_file = nullptr;

  AG_LOGI(TAG, "end_route: session %" PRIu32 " closed (%" PRIu32 " points total)",
          _current_session_id, _current_point_count);

  _current_point_count = 0;
  _current_session_id = 0;
  _last_fsync_ms = FSYNC_ANCHOR_NONE;
  return synced && closed;
}

bool StorageService::flush_and_sync_route_file() {
  if (_route_file == nullptr) {
    return false;
  }

#ifdef TEST_HOST
  const int flush_rc =
      _test_seam ? (++_test_seam->fflush_count, _test_seam->fflush_return) : fflush(_route_file);
#else
  const int flush_rc = fflush(_route_file);
#endif
  if (flush_rc != 0) {
    return false;
  }

#ifdef TEST_HOST
  const int sync_rc = _test_seam ? (++_test_seam->fsync_count, _test_seam->fsync_return)
                                 : fsync(fileno(_route_file));
#else
  const int sync_rc = fsync(fileno(_route_file));
#endif
  return sync_rc == 0;
}

bool StorageService::is_route_active() const { return _route_file != nullptr; }

uint32_t StorageService::current_route_point_count() const { return _current_point_count; }

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

void StorageService::format_route_path(uint32_t session_id, char *out, size_t out_len) const {
  snprintf(out, out_len, "%s/routes/route_%05" PRIu32 ".bin", _nand.mount_path(), session_id);
}

bool StorageService::ensure_route_dir() const {
  if (!_nand.is_mounted()) {
    return false;
  }

  char dir_path[MAX_PATH_LEN];
  snprintf(dir_path, sizeof(dir_path), "%s/routes", _nand.mount_path());

  struct stat st{};
  if (stat(dir_path, &st) == 0) {
    // Path already exists — assume it is a directory.
    return true;
  }

  // Directory does not exist; create it.
  if (mkdir(dir_path, 0755) != 0) {
    AG_LOGE(TAG, "ensure_route_dir: mkdir failed for %s (errno=%d)", dir_path, errno);
    return false;
  }

  AG_LOGI(TAG, "ensure_route_dir: created %s", dir_path);
  return true;
}

// ---------------------------------------------------------------------------
// Persistent (route) read operations — BLE history export
// ---------------------------------------------------------------------------

/// Filename prefix and format for route files: "route_NNNNN.bin"
static constexpr const char *ROUTE_FILE_PREFIX = "route_";
static constexpr const char *ROUTE_FILE_SUFFIX = ".bin";
static constexpr size_t ROUTE_PREFIX_LEN = 6; // strlen("route_")
static constexpr size_t ROUTE_ID_DIGITS = 5;

uint16_t StorageService::session_count() const {
  if (!_nand.is_mounted()) {
    return 0;
  }

  char dir_path[MAX_PATH_LEN];
  snprintf(dir_path, sizeof(dir_path), "%s/routes", _nand.mount_path());

  DIR *dir = opendir(dir_path);
  if (dir == nullptr) {
    return 0;
  }

  uint16_t count = 0;
  struct dirent *entry = nullptr;

  while ((entry = readdir(dir)) != nullptr) {
    if (strncmp(entry->d_name, ROUTE_FILE_PREFIX, ROUTE_PREFIX_LEN) != 0) {
      continue;
    }
    if (strstr(entry->d_name, ROUTE_FILE_SUFFIX) != nullptr) {
      count++;
    }
  }

  closedir(dir);
  AG_LOGI(TAG, "session_count: %u", static_cast<unsigned>(count));
  return count;
}

uint16_t StorageService::list_sessions(uint32_t *out, uint16_t max_count) const {
  if (out == nullptr || max_count == 0 || !_nand.is_mounted()) {
    return 0;
  }

  char dir_path[MAX_PATH_LEN];
  snprintf(dir_path, sizeof(dir_path), "%s/routes", _nand.mount_path());

  DIR *dir = opendir(dir_path);
  if (dir == nullptr) {
    return 0;
  }

  uint16_t count = 0;
  struct dirent *entry = nullptr;

  while ((entry = readdir(dir)) != nullptr && count < max_count) {
    // Match "route_NNNNN.bin" pattern
    if (strncmp(entry->d_name, ROUTE_FILE_PREFIX, ROUTE_PREFIX_LEN) != 0) {
      continue;
    }

    const char *suffix = strstr(entry->d_name, ROUTE_FILE_SUFFIX);
    if (suffix == nullptr) {
      continue;
    }

    // Parse the 5-digit session ID between prefix and suffix
    uint32_t session_id = 0;
    if (sscanf(entry->d_name + ROUTE_PREFIX_LEN, "%05" SCNu32, &session_id) == 1) {
      out[count++] = session_id;
    }
  }

  closedir(dir);

  // Sort ascending
  std::sort(out, out + count);

  return count;
}

uint32_t StorageService::get_session_point_count(uint32_t session_id) const {
  if (!_nand.is_mounted()) {
    return 0;
  }

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "%s/routes/route_%05" PRIu32 ".bin", _nand.mount_path(), session_id);

  struct stat st{};
  if (stat(path, &st) != 0) {
    return 0;
  }

  return static_cast<uint32_t>(st.st_size / sizeof(RoutePoint));
}

uint16_t StorageService::read_route_points(uint32_t session_id, uint32_t offset, RoutePoint *out,
                                           uint16_t count) const {
  if (out == nullptr || count == 0 || !_nand.is_mounted()) {
    return 0;
  }

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "%s/routes/route_%05" PRIu32 ".bin", _nand.mount_path(), session_id);

  FILE *f = fopen(path, "rb");
  if (f == nullptr) {
    return 0;
  }

  // Seek to the requested offset
  long byte_offset = static_cast<long>(offset) * static_cast<long>(sizeof(RoutePoint));
  if (fseek(f, byte_offset, SEEK_SET) != 0) {
    fclose(f);
    return 0;
  }

  size_t read = fread(out, sizeof(RoutePoint), count, f);
  fclose(f);

  return static_cast<uint16_t>(read);
}

time_t StorageService::get_session_start_time(uint32_t session_id) const {
  RoutePoint first{};
  if (read_route_points(session_id, 0, &first, 1) == 0) {
    return 0;
  }
  return first.timestamp;
}

bool StorageService::delete_route(uint32_t session_id) {
  if (!_nand.is_mounted()) {
    AG_LOGE(TAG, "delete_route: NAND not mounted");
    return false;
  }

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "%s/routes/route_%05" PRIu32 ".bin", _nand.mount_path(), session_id);

  struct stat st{};
  if (stat(path, &st) != 0) {
    AG_LOGW(TAG, "delete_route: file not found for session %" PRIu32, session_id);
    return false;
  }

  if (unlink(path) != 0) {
    AG_LOGE(TAG, "delete_route: unlink failed for %s (errno=%d)", path, errno);
    return false;
  }

  return true;
}

uint32_t StorageService::current_route_session_id() const { return _current_session_id; }

bool StorageService::clear_routes() {
  if (_route_file != nullptr) {
    end_route();
  }

  if (!_nand.is_mounted()) {
    AG_LOGE(TAG, "clear_routes: NAND not mounted");
    return false;
  }

  char dir_path[MAX_PATH_LEN];
  snprintf(dir_path, sizeof(dir_path), "%s/routes", _nand.mount_path());

  DIR *dir = opendir(dir_path);
  if (dir == nullptr) {
    if (errno == ENOENT) {
      return true;
    }

    AG_LOGE(TAG, "clear_routes: opendir failed for %s (errno=%d)", dir_path, errno);
    return false;
  }

  bool success = true;
  struct dirent *entry = nullptr;

  while ((entry = readdir(dir)) != nullptr) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }

    char path[MAX_FILE_PATH_LEN];
    const int written = snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
    if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) {
      AG_LOGE(TAG, "clear_routes: path too long for entry %s", entry->d_name);
      success = false;
      continue;
    }

    struct stat st{};
    if (stat(path, &st) != 0) {
      AG_LOGE(TAG, "clear_routes: stat failed for %s (errno=%d)", path, errno);
      success = false;
      continue;
    }

    if (!S_ISREG(st.st_mode)) {
      AG_LOGW(TAG, "clear_routes: skipping non-regular entry %s", path);
      continue;
    }

    if (unlink(path) != 0) {
      AG_LOGE(TAG, "clear_routes: unlink failed for %s (errno=%d)", path, errno);
      success = false;
    }
  }

  closedir(dir);
  return success;
}

uint32_t StorageService::total_capacity_kb() const {
  if (!_nand.is_mounted()) {
    return 0;
  }

#ifdef TEST_HOST
  struct statvfs fs_stats{};
  if (statvfs(_nand.mount_path(), &fs_stats) != 0) {
    AG_LOGW(TAG, "total_capacity_kb: statvfs failed for %s (errno=%d)", _nand.mount_path(), errno);
    return 0;
  }

  const uint64_t total_bytes =
      static_cast<uint64_t>(fs_stats.f_blocks) * static_cast<uint64_t>(fs_stats.f_frsize);
#else
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  const esp_err_t err = esp_vfs_fat_info(_nand.mount_path(), &total_bytes, &free_bytes);
  if (err != ESP_OK) {
    AG_LOGW(TAG, "total_capacity_kb: esp_vfs_fat_info failed for %s (%s)", _nand.mount_path(),
            esp_err_to_name(err));
    return 0;
  }
#endif

  return static_cast<uint32_t>(total_bytes / 1024ULL);
}

uint32_t StorageService::used_kb() const {
  if (!_nand.is_mounted()) {
    return 0;
  }

#ifdef TEST_HOST
  struct statvfs fs_stats{};
  if (statvfs(_nand.mount_path(), &fs_stats) != 0) {
    AG_LOGW(TAG, "used_kb: statvfs failed for %s (errno=%d)", _nand.mount_path(), errno);
    return 0;
  }

  const uint64_t used_blocks =
      static_cast<uint64_t>(fs_stats.f_blocks) - static_cast<uint64_t>(fs_stats.f_bfree);
  const uint64_t used_bytes = used_blocks * static_cast<uint64_t>(fs_stats.f_frsize);
#else
  uint64_t total_bytes = 0;
  uint64_t free_bytes = 0;
  const esp_err_t err = esp_vfs_fat_info(_nand.mount_path(), &total_bytes, &free_bytes);
  if (err != ESP_OK) {
    AG_LOGW(TAG, "used_kb: esp_vfs_fat_info failed for %s (%s)", _nand.mount_path(),
            esp_err_to_name(err));
    return 0;
  }

  const uint64_t used_bytes = total_bytes - free_bytes;
#endif

  return static_cast<uint32_t>(used_bytes / 1024ULL);
}
