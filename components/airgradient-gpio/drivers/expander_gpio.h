/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef EXPANDER_GPIO_H
#define EXPANDER_GPIO_H

#include "airgradient_gpio.h"

class TCA6408A;

namespace gpio {
namespace expander {

// Virtual pin numbers for the I2C expander.  Pins below PIN_BASE are native
// ESP32 GPIOs; pin(n) addresses expander port n.
inline constexpr int PIN_BASE = 100;
inline constexpr int PIN_COUNT = 8;

constexpr int pin(int port) { return PIN_BASE + port; }
constexpr bool is_expander_pin(int p) { return p >= PIN_BASE && p < PIN_BASE + PIN_COUNT; }
constexpr int port_of(int p) { return p - PIN_BASE; }

// Bind the expander driver.  Must be called (with an initialised driver)
// before any expander pin is used through the HAL.
void attach(TCA6408A *device);
TCA6408A *device();

// gpio::Hal that routes native pins to gpio::native::hal and expander pins to
// the attached TCA6408A.  Expander pins support Input/Output direction and
// level access only: pull modes are ignored (the board provides pull-ups) and
// interrupt registration returns false, so callers fall back to polling.
extern const Hal hal;

} // namespace expander
} // namespace gpio

#endif // EXPANDER_GPIO_H
