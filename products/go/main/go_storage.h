/**
 * AirGradient Go — Storage Service
 *
 * Two-tier storage management for the Go product:
 *
 * - Temporary (chart data): PayloadCache with RtcPayloadCacheStorage backend.
 *   Survives deep sleep (RTC memory); lost on power-off. Ring-buffer
 *   semantics — oldest entry is overwritten when the buffer is full.
 *
 * - Persistent (route data): POSIX file I/O on an FATFS filesystem provided
 *   by NandStorage. Survives power-off. One binary file per tracking session;
 *   route files are stored at <mount_path>/routes/route_NNNNN.bin, where
 *   NNNNN is the 5-digit session ID (10000–99999).
 *
 * No independent task — all methods are called synchronously by the
 * orchestrator.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "measures_types.h"
#include "nand_storage.h"
#include "services/payload_cache.h"
#include "gps/gps_types.h"

#include <cstdint>
#include <cstdio>
#include <ctime>

// ---------------------------------------------------------------------------
// RoutePoint
// ---------------------------------------------------------------------------

/// A single data point in a route log, combining GPS and sensor data with a
/// timestamp.  Written sequentially to a binary route file — no header, no
/// framing.  Fixed size allows O(1) seeks to the Nth point.
struct RoutePoint {
  time_t timestamp;                 ///< System time at this point (synced from GPS)
  GpsData gps;                      ///< GPS position and fix metadata
  MeasuresAGo sensors;              ///< Sensor readings at this point
  float battery_percentage = -1.0f; ///< Battery SOC (0–100 %), -1 = invalid
};

// ---------------------------------------------------------------------------
// CacheField
// ---------------------------------------------------------------------------

/// Identifies a single measurable field within MeasuresAGo.
/// Used with StorageService::read_cached_field() to retrieve the full
/// history of one field across all cached entries.
enum class CacheField : uint8_t {
  Temperature, ///< TempHumData::temperature (°C)
  Humidity,    ///< TempHumData::humidity (%)
  PM01,        ///< PMData::pm_01 — atmospheric PM1.0 (µg/m³)
  PM25,        ///< PMData::pm_25 — atmospheric PM2.5 (µg/m³)
  PM10,        ///< PMData::pm_10 — atmospheric PM10  (µg/m³)
  CO2,         ///< CO2Data::co2 (ppm, cast to float)
  TvocIndex,   ///< TVOCNOxData::tvoc_index (cast to float)
  NoxIndex,    ///< TVOCNOxData::nox_index  (cast to float)
};

// ---------------------------------------------------------------------------
// StorageService
// ---------------------------------------------------------------------------

#ifdef TEST_HOST
/// Host-test seam for `fflush` / `fsync` interception. Host tests install
/// one to verify durability-budget cadence and inject sync failures.
struct StorageTestSeam {
  int fflush_count = 0;
  int fsync_count = 0;
  int fflush_return = 0; ///< Non-zero = simulate fflush failure.
  int fsync_return = 0;  ///< Non-zero = simulate fsync failure.
};
#endif

class StorageService {
public:
  StorageService(PayloadCache &cache, NandStorage &nand);

  /// Mount NAND and scan for existing route files.  Call once during
  /// initialization.  PayloadCache::restore() should be called separately
  /// before this if RTC data needs to be recovered after deep sleep.
  ///
  /// Returns false if NAND mount fails.  Temporary cache still works
  /// independently of this return value.
  bool init();

  /// True when the NAND filesystem is mounted (after a successful init()).
  bool is_mounted() const;

  /// NAND FATFS mount path (e.g. "/nand"). Valid only when mounted.
  const char *mount_path() const;

  // --- Temporary (chart) operations ---

  /// Push a measurement into the temporary chart cache.
  /// Overwrites the oldest entry when the cache is full.
  void cache_measurement(const MeasuresAGo &m);

  /// Read the full history for one measurement field, oldest-first.
  ///
  /// Iterates all cached entries and extracts the value for @p field from
  /// each.  Invalid readings (per the field's own validator) are written as
  /// the corresponding MeasuresInvalid sentinel so the caller can identify
  /// and skip them during rendering.
  ///
  /// @param field      Which field to extract.
  /// @param out        Caller-owned buffer; must hold at least @p max_count
  ///                   floats.
  /// @param max_count  Capacity of @p out[].
  /// @return           Number of values written
  ///                   (= min(cached_count(), max_count)).
  uint16_t read_cached_field(CacheField field, float *out, uint16_t max_count) const;

  /// Copy all cached entries (oldest first) into a caller-provided buffer.
  ///
  /// @param out        Caller-owned buffer; must hold at least @p max_count
  ///                   entries.
  /// @param max_count  Capacity of @p out[].
  /// @return           Number of entries written
  ///                   (= min(cached_count(), max_count)).
  uint16_t read_cache(MeasuresAGo *out, uint16_t max_count) const;

  /// Number of cached measurements currently held in the ring buffer.
  uint16_t cached_count() const;

  /// Backup the cache ring buffer to RTC memory.  Call before deep sleep.
  void backup_cache() const;

  /// Restore the cache ring buffer from RTC memory.  Call after deep sleep
  /// wake, before init().
  void restore_cache();

  /// Clear the in-memory and RTC-backed temporary chart cache.
  void clear_cache();

  // --- Persistent (route) operations ---

  /// Open a fresh route file. Refuses if the file already exists (no
  /// truncating-open), if NAND is unmounted, or if a route is already
  /// active. On success the empty file is fsync'd so its directory entry
  /// is durable before this returns. On failure no state leaks: ids and
  /// counts stay 0, no half-open handle.
  [[nodiscard]] bool create_route(uint32_t session_id);

  /// Reopen an existing route file in append mode after deep-sleep wake.
  /// Truncates any torn trailing record to the nearest `RoutePoint`
  /// boundary before opening. Same failure post-condition as
  /// `create_route()`.
  [[nodiscard]] bool resume_route(uint32_t session_id);

  /// True if a route file already exists for @p session_id. Used by the
  /// orchestrator's session-ID collision probe. Returns false when NAND
  /// is not mounted.
  bool route_file_exists(uint32_t session_id) const;

  /// Append one data point to the current route file.
  /// Without a valid GPS fix, store invalid position/altitude sentinels.
  /// The caller's point and all other fields are preserved.
  /// Returns false if no route is active or the write fails.
  [[nodiscard]] bool append_route_point(const RoutePoint &point);

  /// Finish the current route tracking session.  Flushes and closes the
  /// route file.  Safe to call when no route is active (no-op).
  /// Returns false on final sync/close failure; the file is closed either way.
  bool end_route();

  /// Returns true if a route file is currently open.
  bool is_route_active() const;

  /// Number of points written to the currently open route file.
  /// Returns 0 when no route is active.
  uint32_t current_route_point_count() const;

  // --- Persistent (route) read operations for BLE history export ---

  /// Count the total number of route sessions on NAND.
  /// Scans the routes/ directory and counts matching files.
  /// Returns 0 when NAND is not mounted or no sessions exist.
  uint16_t session_count() const;

  /// List all route session IDs on NAND.  Scans the routes/ directory.
  /// Writes session IDs to out[], sorted ascending.  Returns the number
  /// found (up to max_count).
  uint16_t list_sessions(uint32_t *out, uint16_t max_count) const;

  /// Get the number of route points in a session file.
  /// Returns 0 if the session does not exist or the file is empty.
  uint32_t get_session_point_count(uint32_t session_id) const;

  /// Read route points from a session file.
  /// Reads up to count points starting at point index offset.
  /// Returns the number of points actually read.
  uint16_t read_route_points(uint32_t session_id, uint32_t offset, RoutePoint *out,
                             uint16_t count) const;

  /// Get the timestamp of the first route point in a session.
  /// Returns 0 if the session is empty or does not exist.
  time_t get_session_start_time(uint32_t session_id) const;

  /// Delete a single route file by session ID.
  /// Returns false if NAND is not mounted, the file does not exist,
  /// or the unlink fails.
  bool delete_route(uint32_t session_id);

  /// Delete all persisted route files from NAND.
  /// Returns true when all route data is removed or no route directory exists.
  bool clear_routes();

  /// Session ID of the currently open route file, or 0 if no route is active.
  uint32_t current_route_session_id() const;

  /// Total mounted NAND filesystem capacity in kilobytes.
  /// Returns 0 when NAND is not mounted or filesystem stats are unavailable.
  uint32_t total_capacity_kb() const;

  /// Used NAND filesystem space in kilobytes.
  /// Returns 0 when NAND is not mounted or filesystem stats are unavailable.
  uint32_t used_kb() const;

#ifdef TEST_HOST
  /// Install (or clear, with nullptr) a test seam. Caller-owned.
  void set_test_seam(StorageTestSeam *seam) { _test_seam = seam; }
#endif

private:
  PayloadCache &_cache;
  NandStorage &_nand;

  FILE *_route_file = nullptr;
  uint32_t _current_session_id = 0;
  uint32_t _current_point_count = 0;

  /// Timestamp (ms) of the last successful fsync. `FSYNC_ANCHOR_NONE` =
  /// no anchor yet, forces the next append to sync unconditionally.
  static constexpr uint32_t FSYNC_ANCHOR_NONE = UINT32_MAX;
  uint32_t _last_fsync_ms = FSYNC_ANCHOR_NONE;

#ifdef TEST_HOST
  StorageTestSeam *_test_seam = nullptr;
#endif

  /// fflush + fsync the route file.
  bool flush_and_sync_route_file();

  /// Ensure `<mount_path>/routes/` exists; false on mount or mkdir failure.
  bool ensure_route_dir() const;

  /// Format `<mount_path>/routes/route_NNNNN.bin` into @p out.
  void format_route_path(uint32_t session_id, char *out, size_t out_len) const;
};
