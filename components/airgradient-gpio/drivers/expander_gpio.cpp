/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "expander_gpio.h"

#include "esp_log.h"
#include "native_gpio.h"
#include "tca6408a.h"

static constexpr const char *TAG = "ExpanderGpio";

namespace gpio {
namespace expander {

namespace {

TCA6408A *s_device = nullptr;

bool device_ok(int pin) {
  if (s_device == nullptr || !s_device->ready()) {
    ESP_LOGE(TAG, "pin %d used before expander attached", pin);
    return false;
  }
  return true;
}

bool configure(int pin, Mode mode, PullMode pull, InterruptType interrupt) {
  if (!is_expander_pin(pin)) {
    return native::hal.configure(pin, mode, pull, interrupt);
  }
  if (!device_ok(pin)) {
    return false;
  }
  if (interrupt != InterruptType::Disabled) {
    return false; // ~INT is not routed to the MCU on v2.0
  }
  const bool output = (mode == Mode::Output || mode == Mode::InputOutput);
  return s_device->set_direction(static_cast<uint8_t>(port_of(pin)), output);
}

int get_level(int pin) {
  if (!is_expander_pin(pin)) {
    return native::hal.get_level(pin);
  }
  if (!device_ok(pin)) {
    return 0;
  }
  bool high = false;
  if (!s_device->get_level(static_cast<uint8_t>(port_of(pin)), high)) {
    return 0;
  }
  return high ? 1 : 0;
}

bool set_level(int pin, int level) {
  if (!is_expander_pin(pin)) {
    return native::hal.set_level(pin, level);
  }
  if (!device_ok(pin)) {
    return false;
  }
  return s_device->set_level(static_cast<uint8_t>(port_of(pin)), level != 0);
}

bool add_interrupt_handler(int pin, InterruptHandler handler, void *arg) {
  if (!is_expander_pin(pin)) {
    return native::hal.add_interrupt_handler(pin, handler, arg);
  }
  return false;
}

bool remove_interrupt_handler(int pin) {
  if (!is_expander_pin(pin)) {
    return native::hal.remove_interrupt_handler(pin);
  }
  return false;
}

bool enable_interrupt(int pin) {
  if (!is_expander_pin(pin)) {
    return native::hal.enable_interrupt(pin);
  }
  return false;
}

bool disable_interrupt(int pin) {
  if (!is_expander_pin(pin)) {
    return native::hal.disable_interrupt(pin);
  }
  return false;
}

} // namespace

void attach(TCA6408A *dev) { s_device = dev; }

TCA6408A *device() { return s_device; }

const Hal hal = {
    configure,        get_level,        set_level,         add_interrupt_handler,
    remove_interrupt_handler, enable_interrupt, disable_interrupt,
};

} // namespace expander
} // namespace gpio
