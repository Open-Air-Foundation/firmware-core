/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cmath>
#include <cstdint>

// Invalid sentinel values — used to mark uninitialized or unavailable fields.
// Valid ranges are enforced by the is_*_valid() free functions below.
inline constexpr double GPS_LATITUDE_INVALID = 91.0;     // valid: [-90, 90]
inline constexpr double GPS_LONGITUDE_INVALID = 181.0;   // valid: [-180, 180]
inline constexpr float GPS_ALTITUDE_INVALID = -10000.0f; // valid: > GPS_ALTITUDE_INVALID
inline constexpr int GPS_SATELLITE_COUNT_INVALID = -1;   // valid: >= 0
inline constexpr float GPS_DOP_INVALID = -1.0f;          // valid: > 0

enum class GpsFixType : uint8_t {
  NoFix = 0,
  Fix2D = 2,
  Fix3D = 3,
};

struct GpsPosition {
  double latitude = GPS_LATITUDE_INVALID;   // decimal degrees, + = North, - = South
  double longitude = GPS_LONGITUDE_INVALID; // decimal degrees, + = East,  - = West
};

struct GpsFix {
  GpsFixType fix_type = GpsFixType::NoFix;
  int satellite_count = GPS_SATELLITE_COUNT_INVALID;
  float hdop = GPS_DOP_INVALID;
  float pdop = GPS_DOP_INVALID;
  float vdop = GPS_DOP_INVALID;
};

struct GpsTimestamp {
  int year = 0;   // full year, e.g. 2026
  int month = 0;  // 1–12
  int day = 0;    // 1–31
  int hour = 0;   // 0–23
  int minute = 0; // 0–59
  int second = 0; // 0–59
  bool valid = false;
};

struct GpsData {
  GpsPosition position;
  float altitude_m = GPS_ALTITUDE_INVALID; // meters above mean sea level
  GpsFix fix;
  GpsTimestamp timestamp;
};

// ---------------------------------------------------------------------------
// Field-level validation helpers
// ---------------------------------------------------------------------------

inline bool is_latitude_valid(double lat) { return lat >= -90.0 && lat <= 90.0; }

inline bool is_longitude_valid(double lon) { return lon >= -180.0 && lon <= 180.0; }

inline bool is_position_valid(const GpsPosition &pos) {
  return is_latitude_valid(pos.latitude) && is_longitude_valid(pos.longitude);
}

inline bool is_altitude_valid(float alt) { return alt > GPS_ALTITUDE_INVALID; }

inline bool is_satellite_count_valid(int count) { return count >= 0; }

inline bool is_hdop_valid(float hdop) { return std::isfinite(hdop) && hdop > 0.0f; }

inline bool is_fix_valid(const GpsFix &fix) { return fix.fix_type != GpsFixType::NoFix; }

inline bool is_gps_timestamp_valid(const GpsTimestamp &ts) { return ts.valid; }

/// Validate only the displayed UTC time fields, independently of the date/valid flag.
inline bool is_gps_time_of_day_valid(const GpsTimestamp &ts) {
  return ts.hour >= 0 && ts.hour <= 23 && ts.minute >= 0 && ts.minute <= 59 && ts.second >= 0 &&
         ts.second <= 59;
}

// ---------------------------------------------------------------------------
// A-GNSS aiding data
// ---------------------------------------------------------------------------

/// Aiding data for A-GNSS injection. Provided by the application layer
/// (e.g. from a phone via BLE) and forwarded through GpsService to
/// GpsDriver::inject_aiding().
struct GpsAidingData {
  /// Latitude in decimal degrees.  Set to GPS_LATITUDE_INVALID to skip
  /// position injection.
  double latitude = GPS_LATITUDE_INVALID;

  /// Longitude in decimal degrees.  Set to GPS_LONGITUDE_INVALID to skip
  /// position injection.
  double longitude = GPS_LONGITUDE_INVALID;

  /// Altitude in meters above mean sea level.  Set to GPS_ALTITUDE_INVALID
  /// to skip.  If latitude/longitude are valid but altitude is invalid, the
  /// altitude field in AID-POS is set to 0 (unknown).
  float altitude_m = GPS_ALTITUDE_INVALID;

  /// Position accuracy estimate in meters (1-sigma).
  /// 0 = unknown (receiver uses its own default).
  float pos_acc_m = 0;

  /// UTC time as POSIX epoch seconds.  Set to 0 to skip time injection.
  /// The driver converts to calendar fields (year/month/day/hour/min/sec)
  /// internally via gmtime_r().
  int64_t epoch_s = 0;

  /// Time accuracy estimate in milliseconds.
  /// Used to populate the AID-TIME tacc_s and tacc_ns fields.
  uint32_t time_acc_ms = 0;
};

/// Return true if the aiding data contains a valid position for injection.
inline bool has_aiding_position(const GpsAidingData &d) {
  return is_latitude_valid(d.latitude) && is_longitude_valid(d.longitude);
}

/// Return true if the aiding data contains a valid time for injection.
inline bool has_aiding_time(const GpsAidingData &d) { return d.epoch_s > 0; }
