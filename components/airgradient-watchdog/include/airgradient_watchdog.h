/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AIRGRADIENT_WATCHDOG_H
#define AIRGRADIENT_WATCHDOG_H

#include "airgradient_gpio.h"

namespace watchdog {

// Configures the pin as a floating output with interrupts disabled.
// Returns false if GPIO configuration fails.
bool init(const gpio::Hal &hal, int pin);

// Feeds the external watchdog: HIGH, RTOS delay of 20 ms, then LOW.
// Task context only. Caller serializes feeds and controls their interval.
// GPIO write failures are not reported, preserving the existing helper behavior.
void feed(const gpio::Hal &hal, int pin);

} // namespace watchdog

#endif // AIRGRADIENT_WATCHDOG_H
