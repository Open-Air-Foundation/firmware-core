/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <limits>

#include "gps/gps_types.h"

TEST_CASE("gps_types public types expose invalid sentinels", "[gps][types]") {
  const GpsData data;

  REQUIRE(data.position.latitude == GPS_LATITUDE_INVALID);
  REQUIRE(data.position.longitude == GPS_LONGITUDE_INVALID);
  REQUIRE(data.altitude_m == GPS_ALTITUDE_INVALID);
  REQUIRE(data.fix.fix_type == GpsFixType::NoFix);
  REQUIRE(data.fix.satellite_count == GPS_SATELLITE_COUNT_INVALID);
  REQUIRE(data.fix.hdop == GPS_DOP_INVALID);
  REQUIRE(data.fix.pdop == GPS_DOP_INVALID);
  REQUIRE(data.fix.vdop == GPS_DOP_INVALID);
  REQUIRE_FALSE(data.timestamp.valid);
}

TEST_CASE("gps_types validation helpers reject sentinel values", "[gps][types]") {
  REQUIRE_FALSE(is_latitude_valid(GPS_LATITUDE_INVALID));
  REQUIRE_FALSE(is_longitude_valid(GPS_LONGITUDE_INVALID));
  REQUIRE_FALSE(is_altitude_valid(GPS_ALTITUDE_INVALID));
  REQUIRE_FALSE(is_satellite_count_valid(GPS_SATELLITE_COUNT_INVALID));
}

TEST_CASE("gps_types validation helpers accept in-range values", "[gps][types]") {
  REQUIRE(is_latitude_valid(0.0));
  REQUIRE(is_latitude_valid(90.0));
  REQUIRE(is_latitude_valid(-90.0));
  REQUIRE(is_longitude_valid(0.0));
  REQUIRE(is_longitude_valid(180.0));
  REQUIRE(is_longitude_valid(-180.0));
  REQUIRE(is_altitude_valid(0.0f));
  REQUIRE(is_altitude_valid(-430.0f)); // Dead Sea
  REQUIRE(is_satellite_count_valid(0));
  REQUIRE(is_satellite_count_valid(12));
}

TEST_CASE("GpsAidingData default-initializes to no-injection state", "[gps][types]") {
  const GpsAidingData aid;
  REQUIRE_FALSE(has_aiding_position(aid));
  REQUIRE_FALSE(has_aiding_time(aid));
  REQUIRE(aid.latitude == GPS_LATITUDE_INVALID);
  REQUIRE(aid.longitude == GPS_LONGITUDE_INVALID);
  REQUIRE(aid.altitude_m == GPS_ALTITUDE_INVALID);
  REQUIRE(aid.pos_acc_m == 0);
  REQUIRE(aid.epoch_s == 0);
  REQUIRE(aid.time_acc_ms == 0);
}

TEST_CASE("HDOP validation requires a finite positive field", "[gps][types]") {
  for (float invalid :
       {GPS_DOP_INVALID, 0.0f, -0.0f, -1.0f, std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()})
    CHECK_FALSE(is_hdop_valid(invalid));
  for (float valid :
       {0.1f, 1.0f, std::numeric_limits<float>::min(), std::numeric_limits<float>::max()})
    CHECK(is_hdop_valid(valid));
}

TEST_CASE("GPS display time validation leaves synchronization predicates unchanged",
          "[gps][types]") {
  GpsTimestamp ts{};
  CHECK(is_gps_time_of_day_valid(ts)); // Midnight; valid flag and date checked separately.
  CHECK_FALSE(is_gps_timestamp_valid(ts));
  ts = {2026, 10, 6, 23, 59, 59, true};
  CHECK(is_gps_time_of_day_valid(ts));
  for (int GpsTimestamp::*field :
       {&GpsTimestamp::hour, &GpsTimestamp::minute, &GpsTimestamp::second}) {
    const int original = ts.*field;
    for (int invalid : {-1, field == &GpsTimestamp::hour ? 24 : 60, std::numeric_limits<int>::min(),
                        std::numeric_limits<int>::max()}) {
      ts.*field = invalid;
      CHECK_FALSE(is_gps_time_of_day_valid(ts));
      CHECK(is_gps_timestamp_valid(ts)); // Existing helper still checks only the flag.
    }
    ts.*field = original;
  }
  GpsFix fix{};
  CHECK_FALSE(is_fix_valid(fix));
  for (GpsFixType type : {GpsFixType::Fix2D, GpsFixType::Fix3D, static_cast<GpsFixType>(255)}) {
    fix.fix_type = type;
    CHECK(is_fix_valid(fix)); // Existing helper still accepts any non-NoFix enum.
  }
}
