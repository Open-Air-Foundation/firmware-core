/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AIRGRADIENT_RGB_H
#define AIRGRADIENT_RGB_H

#include <cstdint>

/// Unscaled RGB color; each channel ranges from 0 to 255.
struct Rgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

#endif // AIRGRADIENT_RGB_H
