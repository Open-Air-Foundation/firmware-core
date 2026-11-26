#pragma once

#include "go_settings.h"

// Product policy is independent of radio lifetime and user interaction.
struct GoRuntimePolicy {
  static constexpr int TEMP_HUM_CORRECTION_BYPASS_MIN_SECONDS = 300;

  const GoSettings &settings;
  TrackingState tracking = TrackingState::Idle;
  bool wifi_fallback = false;

  bool tracking_active() const { return tracking != TrackingState::Idle; }
  int interval_seconds() const { return settings.active_measure_interval_seconds(tracking); }
  bool gps_active() const {
    return settings.gps_mode == GpsMode::AlwaysOn ||
           (settings.gps_mode == GpsMode::OnWhenTracking && tracking_active());
  }
  bool wifi_allowed() const { return settings.wifi_enabled && !tracking_active(); }
  bool sleep_allowed() const {
    return !tracking_active() && (!settings.wifi_enabled || wifi_fallback);
  }
  bool bypass_temp_hum_corrections() const {
    return sleep_allowed() &&
           settings.idle_interval_seconds >= TEMP_HUM_CORRECTION_BYPASS_MIN_SECONDS;
  }
};
