#pragma once
#include <algorithm>
#include <cstdint>
inline constexpr uint32_t GPS_MAX_SLEEP_MS = 300000;
inline uint32_t gps_inactive_sleep_ms(uint32_t measurement_interval_ms) {
  return std::max<uint32_t>(1, std::min(measurement_interval_ms, GPS_MAX_SLEEP_MS));
}
