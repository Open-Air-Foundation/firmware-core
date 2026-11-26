#pragma once

#include <algorithm>
#include <cstdint>

inline constexpr uint32_t GPS_MAX_INACTIVE_SLEEP_MS = 300000;

inline uint32_t gps_inactive_sleep_ms(uint32_t next_measurement_ms) {
  return std::min(next_measurement_ms, GPS_MAX_INACTIVE_SLEEP_MS);
}
