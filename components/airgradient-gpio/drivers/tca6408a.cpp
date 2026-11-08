/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "tca6408a.h"

#include "esp_log.h"
static constexpr const char *TAG = "TCA6408A";

namespace gpio::tca6408a {
namespace {
constexpr uint8_t REG_INPUT = 0x00;
constexpr uint8_t REG_OUTPUT = 0x01;
constexpr uint8_t REG_POLARITY = 0x02;
constexpr uint8_t REG_CONFIGURATION = 0x03;
constexpr uint8_t ALL_PINS = 0xFF;

bool valid_pin(int pin) { return pin >= 0 && pin < PIN_COUNT; }
bool valid_level(int level) { return level == 0 || level == 1; }

class Lock {
public:
  explicit Lock(RtosMutex &mutex) : _mutex(mutex), _locked(mutex.is_valid() && mutex.lock()) {}
  ~Lock() {
    if (_locked) {
      _mutex.unlock();
    }
  }
  Lock(const Lock &) = delete;
  Lock &operator=(const Lock &) = delete;
  bool acquired() const { return _locked; }

private:
  RtosMutex &_mutex;
  bool _locked;
};

// Register helpers require the context mutex to be held by the caller.
bool read_register(Context &context, uint8_t reg, uint8_t &value) {
  if (context.device == nullptr) {
    context.output_latch_valid = false;
    return false;
  }
  uint8_t result = ALL_PINS;
  const esp_err_t error = i2c_master_transmit_receive(context.device, &reg, sizeof(reg), &result,
                                                      sizeof(result), context.config.timeout_ms);
  if (error != ESP_OK) {
    context.output_latch_valid = false;
    ESP_LOGE(TAG, "register 0x%02X read failed: %s", reg, esp_err_to_name(error));
    return false;
  }
  value = result;
  if (reg == REG_OUTPUT) {
    context.output_latch = result;
    context.output_latch_valid = true;
  }
  return true;
}

bool write_register(Context &context, uint8_t reg, uint8_t value) {
  if (context.device == nullptr) {
    context.output_latch_valid = false;
    return false;
  }
  const uint8_t data[] = {reg, value};
  const esp_err_t error =
      i2c_master_transmit(context.device, data, sizeof(data), context.config.timeout_ms);
  if (error != ESP_OK) {
    // A failed transfer may still have reached the chip. Refresh before the
    // next partial output update rather than trusting the previous latch.
    context.output_latch_valid = false;
    ESP_LOGE(TAG, "register 0x%02X write failed: %s", reg, esp_err_to_name(error));
    return false;
  }
  if (reg == REG_OUTPUT) {
    context.output_latch = value;
    context.output_latch_valid = true;
  }
  return true;
}

bool update_register(Context &context, uint8_t reg, uint8_t mask, uint8_t value) {
  uint8_t previous = ALL_PINS;
  if (reg == REG_OUTPUT && context.output_latch_valid) {
    previous = context.output_latch;
  } else if (!read_register(context, reg, previous)) {
    return false;
  }
  const uint8_t updated = static_cast<uint8_t>((previous & ~mask) | (value & mask));
  return previous == updated || write_register(context, reg, updated);
}

bool remove_device(Context &context) {
  context.output_latch_valid = false;
  if (context.device != nullptr) {
    const esp_err_t error = i2c_master_bus_rm_device(context.device);
    if (error != ESP_OK) {
      ESP_LOGE(TAG, "device removal failed: %s", esp_err_to_name(error));
      return false;
    }
    context.device = nullptr;
  }
  context.bus = nullptr;
  return true;
}

bool configure(void *opaque, int pin, gpio::Mode mode, gpio::PullMode pull,
               gpio::InterruptType interrupt, int initial_level) {
  auto &context = *static_cast<Context *>(opaque);
  if (!valid_pin(pin) || (mode != gpio::Mode::Input && mode != gpio::Mode::Output) ||
      pull != gpio::PullMode::Floating || interrupt != gpio::InterruptType::Disabled ||
      (mode == gpio::Mode::Output && initial_level != gpio::KEEP_OUTPUT_LEVEL &&
       !valid_level(initial_level))) {
    return false;
  }
  Lock lock(context.mutex);
  if (!lock.acquired() || context.device == nullptr) {
    return false;
  }
  const uint8_t mask = static_cast<uint8_t>(1u << pin);
  if (mode == gpio::Mode::Input) {
    return update_register(context, REG_CONFIGURATION, mask, mask);
  }
  // Keep this sequence under one lock. A failed direction write may leave the
  // latch updated; report failure rather than claiming an atomic reconfiguration.
  return (initial_level == gpio::KEEP_OUTPUT_LEVEL ||
          update_register(context, REG_OUTPUT, mask, initial_level ? mask : 0)) &&
         update_register(context, REG_CONFIGURATION, mask, 0);
}

int get_level(void *opaque, int pin) {
  auto &context = *static_cast<Context *>(opaque);
  if (!valid_pin(pin)) {
    return -1;
  }
  uint8_t levels = ALL_PINS;
  return read_port(context, levels) ? static_cast<int>((levels >> pin) & 1u) : -1;
}

bool set_level(void *opaque, int pin, int level) {
  auto &context = *static_cast<Context *>(opaque);
  if (!valid_pin(pin) || !valid_level(level)) {
    return false;
  }
  Lock lock(context.mutex);
  if (!lock.acquired() || context.device == nullptr) {
    return false;
  }
  const uint8_t mask = static_cast<uint8_t>(1u << pin);
  return update_register(context, REG_OUTPUT, mask, level ? mask : 0);
}

const Operations operations{
    configure,
    get_level,
    set_level,
    // INT routing and MCU ISR registration belong to board integration.
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};
} // namespace

Hal hal(Context &context) { return {operations, &context}; }

bool init(Context &context, i2c_master_bus_handle_t bus, const Config &config) {
  if (bus == nullptr || (config.address != ADDRESS_LOW && config.address != ADDRESS_HIGH) ||
      config.scl_speed_hz == 0 || config.scl_speed_hz > MAX_SCL_SPEED_HZ ||
      config.timeout_ms <= 0) {
    return false;
  }
  Lock lock(context.mutex);
  if (!lock.acquired()) {
    return false;
  }
  if (context.device != nullptr) {
    // A live handle retains its original bus, address, and clock configuration.
    if (context.bus != bus || context.config.address != config.address ||
        context.config.scl_speed_hz != config.scl_speed_hz ||
        context.config.timeout_ms != config.timeout_ms) {
      return false;
    }
  } else {
    context.output_latch_valid = false;
    context.bus = bus;
    context.config = config;
    if (i2c_master_probe(bus, config.address, config.timeout_ms) != ESP_OK) {
      ESP_LOGE(TAG, "probe failed at 0x%02X", config.address);
      return false;
    }
    i2c_device_config_t device_config{};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = config.address;
    device_config.scl_speed_hz = config.scl_speed_hz;
    if (i2c_master_bus_add_device(bus, &device_config, &context.device) != ESP_OK) {
      ESP_LOGE(TAG, "device registration failed");
      return false;
    }
  }
  context.output_latch_valid = false;
  uint8_t configuration = ALL_PINS;
  uint8_t output_latch = ALL_PINS;
  if (!read_register(context, REG_CONFIGURATION, configuration) ||
      !read_register(context, REG_OUTPUT, output_latch)) {
    remove_device(context);
    return false;
  }
  return true;
}

bool deinit(Context &context) {
  Lock lock(context.mutex);
  return lock.acquired() && remove_device(context);
}

bool read_port(Context &context, uint8_t &levels) {
  Lock lock(context.mutex);
  return lock.acquired() && read_register(context, REG_INPUT, levels);
}

bool write_port(Context &context, uint8_t levels) {
  Lock lock(context.mutex);
  return lock.acquired() && write_register(context, REG_OUTPUT, levels);
}

bool set_input_inversion(Context &context, uint8_t mask) {
  Lock lock(context.mutex);
  return lock.acquired() && write_register(context, REG_POLARITY, mask);
}

} // namespace gpio::tca6408a
