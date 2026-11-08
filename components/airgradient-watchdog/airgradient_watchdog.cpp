/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "airgradient_watchdog.h"

#include "rtos.h"

namespace watchdog {

static constexpr uint32_t FEED_PULSE_MS = 20;

bool init(const gpio::Hal &hal, int pin) {
  return hal.configure(pin, gpio::Mode::Output, gpio::PullMode::Floating,
                       gpio::InterruptType::Disabled);
}

void feed(const gpio::Hal &hal, int pin) {
  hal.set_level(pin, 1);
  RTOS::delay_ms(FEED_PULSE_MS);
  hal.set_level(pin, 0);
}

} // namespace watchdog
