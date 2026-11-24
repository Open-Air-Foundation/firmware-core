#pragma once

#include <cmath>

#include "go_display.h"

// Cache values must already have presentation corrections applied. The caller
// owns chart_buffer (DISPLAY_CHART_BUF_SIZE floats) through display rendering.
inline void populate_display_chart(DisplayChartData &chart, Metric metric, const MeasuresAGo *cache,
                                   uint8_t cache_count, float *chart_buffer) {
  chart.samples = nullptr;
  chart.count = 0;
  chart.min = 0.0f;
  chart.max = 0.0f;
  if (metric == Metric::None || cache == nullptr || cache_count == 0 || chart_buffer == nullptr) {
    return;
  }

  uint8_t valid_count = 0;
  float min_val = 0.0f;
  float max_val = 0.0f;
  bool first = true;

  for (uint8_t i = 0; i < cache_count && i < DISPLAY_CHART_BUF_SIZE; ++i) {
    float sample = 0.0f;
    bool valid = false;

    switch (metric) {
    case Metric::Pm25:
      if (cache[i].pm_a.is_pm_25_valid()) {
        sample = cache[i].pm_a.pm_25;
        valid = true;
      }
      break;
    case Metric::Co2:
      if (cache[i].co2.is_valid()) {
        sample = static_cast<float>(cache[i].co2.co2);
        valid = true;
      }
      break;
    case Metric::Temp:
      if (cache[i].temp_hum_a.is_temp_valid()) {
        sample = cache[i].temp_hum_a.temperature;
        valid = true;
      }
      break;
    case Metric::Humidity:
      if (cache[i].temp_hum_a.is_hum_valid()) {
        sample = cache[i].temp_hum_a.humidity;
        valid = true;
      }
      break;
    case Metric::Tvoc:
      if (cache[i].tvoc_nox.is_tvoc_index_valid()) {
        sample = static_cast<float>(cache[i].tvoc_nox.tvoc_index);
        valid = true;
      }
      break;
    case Metric::Nox:
      if (cache[i].tvoc_nox.is_nox_index_valid()) {
        sample = static_cast<float>(cache[i].tvoc_nox.nox_index);
        valid = true;
      }
      break;
    case Metric::Pressure:
      if (cache[i].pressure.is_pressure_valid()) {
        sample = cache[i].pressure.pressure;
        valid = true;
      }
      break;
    case Metric::Altitude:
      if (cache[i].pressure.is_altitude_valid()) {
        sample = cache[i].pressure.altitude;
        valid = true;
      }
      break;
    case Metric::None:
    default:
      break;
    }

    if (valid && std::isfinite(sample)) {
      chart_buffer[valid_count] = sample;
      if (first) {
        min_val = sample;
        max_val = sample;
        first = false;
      } else {
        if (sample < min_val)
          min_val = sample;
        if (sample > max_val)
          max_val = sample;
      }
      ++valid_count;
    }
  }

  chart.samples = (valid_count > 0) ? chart_buffer : nullptr;
  chart.count = valid_count;
  chart.min = (valid_count > 0) ? min_val : 0.0f;
  chart.max = (valid_count > 0) ? max_val : 0.0f;
}
