#pragma once

#include "go_types.h"
#include <cstdint>

inline constexpr uint32_t GPS_IDLE_SLEEP_MS = 30000;
// Provisional until reset-to-GPS-ready time is measured on the bench.
inline constexpr uint32_t GPS_RESET_THRESHOLD_MS = 30000;
inline constexpr uint32_t GPS_RESET_CONFIRM_MS = 1000;

inline uint32_t gps_inactive_sleep_ms(OperatingMode mode, uint32_t next_wake_ms) {
  if (mode != OperatingMode::Offline) {
    return GPS_IDLE_SLEEP_MS;
  }
  return next_wake_ms != 0 ? next_wake_ms : GPS_IDLE_SLEEP_MS;
}
