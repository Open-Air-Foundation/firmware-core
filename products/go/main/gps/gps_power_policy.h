#pragma once

#include "go_types.h"
#include <cstdint>

inline constexpr uint32_t GPS_IDLE_SLEEP_MS = 30000;
inline constexpr uint32_t GPS_ENERGY_SAVING_SLEEP_MS = UINT32_MAX;

inline uint32_t gps_inactive_sleep_ms(OperatingMode mode) {
  return mode == OperatingMode::Offline ? GPS_ENERGY_SAVING_SLEEP_MS : GPS_IDLE_SLEEP_MS;
}
