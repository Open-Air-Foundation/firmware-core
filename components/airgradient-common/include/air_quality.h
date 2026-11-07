/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AIRGRADIENT_AIR_QUALITY_H
#define AIRGRADIENT_AIR_QUALITY_H

#include <cstdint>

#include "rgb.h"

namespace air_quality {

/// Sentinel returned by pm25_to_us_aqi() for invalid input.
inline constexpr int INVALID_AQI = -1;

/// PM2.5 (ug/m3) -> US AQI. Uses the 2024 US EPA PM2.5 breakpoints.
/// Returns INVALID_AQI for negative or NaN input.
int pm25_to_us_aqi(float pm25_ugm3);

/// US EPA AQI category bands.
enum class UsAqiCategory : uint8_t {
  Invalid = 0,
  Good = 1,               ///< 0..50
  Moderate = 2,           ///< 51..100
  UnhealthySensitive = 3, ///< 101..150
  Unhealthy = 4,          ///< 151..200
  VeryUnhealthy = 5,      ///< 201..300
  Hazardous = 6,          ///< 301..500
};

/// AQI value -> category. Negative -> Invalid; > 500 -> Hazardous.
UsAqiCategory us_aqi_to_category(int aqi);

/// Category -> unscaled indicator color. Invalid or unknown categories -> black.
Rgb category_to_rgb(UsAqiCategory category);

/// CO2 concentration bands, independent of the numerical US AQI scale.
enum class Co2Category : uint8_t {
  Invalid = 0,
  Excellent = 1,     ///< <= 800 ppm, within the valid measurement range
  Acceptable = 2,    ///< 801..1000 ppm
  NotIdeal = 3,      ///< 1001..1500 ppm
  ToBeAvoided = 4,   ///< 1501..2000 ppm
  Unhealthy = 5,     ///< 2001..3000 ppm
  VeryUnhealthy = 6, ///< > 3000 ppm, within the valid measurement range
};

/// CO2 ppm -> category. Uses CO2Data validity (currently 0..10000 ppm).
Co2Category co2_to_category(int ppm);

/// Category -> unscaled indicator color. Invalid or unknown categories -> black.
Rgb category_to_rgb(Co2Category category);

} // namespace air_quality

#endif // AIRGRADIENT_AIR_QUALITY_H
