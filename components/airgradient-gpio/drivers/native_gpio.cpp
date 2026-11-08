/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "native_gpio.h"

#include "driver/gpio.h"

namespace {

gpio_mode_t to_gpio_mode(gpio::Mode mode) {
  switch (mode) {
  case gpio::Mode::Input:
    return GPIO_MODE_INPUT;
  case gpio::Mode::Output:
    return GPIO_MODE_OUTPUT;
  case gpio::Mode::InputOutput:
    return GPIO_MODE_INPUT_OUTPUT;
  }

  return GPIO_MODE_DISABLE;
}

gpio_int_type_t to_interrupt_type(gpio::InterruptType interrupt) {
  switch (interrupt) {
  case gpio::InterruptType::Disabled:
    return GPIO_INTR_DISABLE;
  case gpio::InterruptType::RisingEdge:
    return GPIO_INTR_POSEDGE;
  case gpio::InterruptType::FallingEdge:
    return GPIO_INTR_NEGEDGE;
  case gpio::InterruptType::AnyEdge:
    return GPIO_INTR_ANYEDGE;
  case gpio::InterruptType::LowLevel:
    return GPIO_INTR_LOW_LEVEL;
  case gpio::InterruptType::HighLevel:
    return GPIO_INTR_HIGH_LEVEL;
  }

  return GPIO_INTR_DISABLE;
}

void apply_pull_mode(gpio_config_t &config, gpio::PullMode pull) {
  switch (pull) {
  case gpio::PullMode::Floating:
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    break;
  case gpio::PullMode::PullUp:
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    break;
  case gpio::PullMode::PullDown:
    config.pull_up_en = GPIO_PULLUP_DISABLE;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    break;
  case gpio::PullMode::PullUpDown:
    config.pull_up_en = GPIO_PULLUP_ENABLE;
    config.pull_down_en = GPIO_PULLDOWN_ENABLE;
    break;
  }
}

bool is_valid_pin(int pin) { return GPIO_IS_VALID_GPIO(pin); }

bool install_isr_service_once() {
  static bool installed = false;
  if (installed) {
    return true;
  }
  const esp_err_t err = gpio_install_isr_service(0);
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    installed = true;
    return true;
  }
  return false;
}

bool configure(void *, int pin, gpio::Mode mode, gpio::PullMode pull, gpio::InterruptType interrupt,
               int initial_level) {
  if (!is_valid_pin(pin)) {
    return false;
  }

  const gpio_num_t gpio_num = static_cast<gpio_num_t>(pin);
  const bool output = mode == gpio::Mode::Output || mode == gpio::Mode::InputOutput;
  if (output && (!GPIO_IS_VALID_OUTPUT_GPIO(pin) || (initial_level != gpio::KEEP_OUTPUT_LEVEL &&
                                                     initial_level != 0 && initial_level != 1))) {
    return false;
  }
  if (gpio_reset_pin(gpio_num) != ESP_OK) {
    return false;
  }
  if (output && initial_level != gpio::KEEP_OUTPUT_LEVEL &&
      gpio_set_level(gpio_num, initial_level) != ESP_OK) {
    return false;
  }

  gpio_config_t config = {};
  config.pin_bit_mask = (1ULL << pin);
  config.mode = to_gpio_mode(mode);
  config.intr_type = to_interrupt_type(interrupt);
  apply_pull_mode(config, pull);

  return gpio_config(&config) == ESP_OK;
}

int get_level(void *, int pin) {
  if (!is_valid_pin(pin)) {
    return -1;
  }

  return gpio_get_level(static_cast<gpio_num_t>(pin));
}

bool set_level(void *, int pin, int level) {
  if (!is_valid_pin(pin)) {
    return false;
  }

  return gpio_set_level(static_cast<gpio_num_t>(pin), level) == ESP_OK;
}

bool add_interrupt_handler(void *, int pin, gpio::InterruptHandler handler, void *arg) {
  if (!is_valid_pin(pin) || handler == nullptr) {
    return false;
  }

  if (!install_isr_service_once()) {
    return false;
  }

  return gpio_isr_handler_add(static_cast<gpio_num_t>(pin), handler, arg) == ESP_OK;
}

bool remove_interrupt_handler(void *, int pin) {
  if (!is_valid_pin(pin)) {
    return false;
  }

  return gpio_isr_handler_remove(static_cast<gpio_num_t>(pin)) == ESP_OK;
}

bool enable_interrupt(void *, int pin) {
  if (!is_valid_pin(pin)) {
    return false;
  }

  return gpio_intr_enable(static_cast<gpio_num_t>(pin)) == ESP_OK;
}

bool disable_interrupt(void *, int pin) {
  if (!is_valid_pin(pin)) {
    return false;
  }

  return gpio_intr_disable(static_cast<gpio_num_t>(pin)) == ESP_OK;
}

const gpio::Operations operations = {
    configure,        get_level,         set_level, add_interrupt_handler, remove_interrupt_handler,
    enable_interrupt, disable_interrupt,
};

} // namespace

const gpio::Hal gpio::native::hal{operations};
