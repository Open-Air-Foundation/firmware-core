#include "spl07.h"

#include "ag_log.h"
#include "rtos.h"
#include <cmath>

static constexpr const char *TAG = "SPL07";

namespace {
constexpr uint8_t REG_DATA = 0x00;
constexpr uint8_t REG_PRESSURE_CONFIG = 0x06;
constexpr uint8_t REG_TEMPERATURE_CONFIG = 0x07;
constexpr uint8_t REG_MEASUREMENT = 0x08;
constexpr uint8_t REG_CONFIG = 0x09;
constexpr uint8_t REG_RESET = 0x0C;
constexpr uint8_t REG_ID = 0x0D;
constexpr uint8_t REG_COEFFICIENTS = 0x10;
constexpr uint8_t PRODUCT_ID = 0x11;
constexpr uint8_t SOFT_RESET = 0x09;
constexpr uint8_t STARTUP_READY = 0xC0;
constexpr uint8_t DATA_READY = 0x30;
constexpr uint8_t OVERSAMPLING_16X = 0x04; // 1 Hz, for both pressure and temperature
constexpr uint8_t SHIFT_BOTH = 0x0C;
constexpr uint8_t CONTINUOUS_BOTH = 0x07; // Internal ASIC temperature, TMP_EXT=0
constexpr size_t COEFFICIENT_BYTES = 21;
constexpr int TIMEOUT_MS = 100;
constexpr uint32_t CLOCK_HZ = 100000;
constexpr uint32_t RESET_DELAY_MS = 50;
constexpr uint32_t READY_DELAY_MS = 10;
constexpr unsigned READY_ATTEMPTS = 20;
constexpr float SCALE_16X = 253952.0f;
constexpr float SEA_LEVEL_HPA = 1013.25f;
constexpr float MIN_PRESSURE_HPA = 300.0f;
constexpr float MAX_PRESSURE_HPA = 1100.0f;
constexpr float ALTITUDE_FACTOR_M = 44330.0f;
constexpr float ALTITUDE_EXPONENT = 0.1903f;

int32_t signed_bits(uint32_t value, unsigned bits) {
  const uint32_t sign = 1u << (bits - 1);
  return (value & sign) ? static_cast<int32_t>(value) - static_cast<int32_t>(1u << bits)
                        : static_cast<int32_t>(value);
}
} // namespace

SPL07::~SPL07() {
  if (_dev != nullptr)
    i2c_master_bus_rm_device(_dev);
}

bool SPL07::init() {
  _ready = false;
  _temperature = {};
  if (_dev == nullptr) {
    i2c_device_config_t cfg{};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = _address;
    cfg.scl_speed_hz = CLOCK_HZ;
    if (i2c_master_bus_add_device(_bus, &cfg, &_dev) != ESP_OK)
      return false;
  }
  uint8_t id = 0;
  if (!read_register(REG_ID, &id, 1) || id != PRODUCT_ID) {
    AG_LOGE(TAG, "unexpected ID 0x%02x at 0x%02x", id, _address);
    return false;
  }
  if (!write_register(REG_RESET, SOFT_RESET))
    return false;
  RTOS::delay_ms(RESET_DELAY_MS);
  bool initialized = false;
  for (unsigned i = 0; i < READY_ATTEMPTS; ++i) {
    uint8_t status = 0;
    if (!read_register(REG_MEASUREMENT, &status, 1))
      return false;
    if ((status & STARTUP_READY) == STARTUP_READY) {
      initialized = true;
      break;
    }
    RTOS::delay_ms(READY_DELAY_MS);
  }
  if (!initialized || !read_coefficients() ||
      !write_register(REG_PRESSURE_CONFIG, OVERSAMPLING_16X) ||
      !write_register(REG_TEMPERATURE_CONFIG, OVERSAMPLING_16X) ||
      !write_register(REG_CONFIG, SHIFT_BOTH) || !write_register(REG_MEASUREMENT, CONTINUOUS_BOTH))
    return false;
  _ready = true;
  AG_LOGI(TAG, "continuous pressure/temperature ready");
  return true;
}

bool SPL07::read(PressureData &out) {
  out = {};
  _temperature = {};
  uint8_t status = 0;
  if (!_ready || !read_register(REG_MEASUREMENT, &status, 1) || (status & DATA_READY) != DATA_READY)
    return false;
  uint8_t data[6];
  if (!read_register(REG_DATA, data, sizeof(data)))
    return false;
  const float p =
      signed_bits((uint32_t(data[0]) << 16) | (uint32_t(data[1]) << 8) | data[2], 24) / SCALE_16X;
  const float t =
      signed_bits((uint32_t(data[3]) << 16) | (uint32_t(data[4]) << 8) | data[5], 24) / SCALE_16X;
  TempHumData temperature{};
  temperature.temperature = _c0 * 0.5f + _c1 * t;
  if (!temperature.is_temp_valid())
    return false;
  // SPL07 adds c40 and c31 to the DPS368 polynomial (Goermicro datasheet 4.6.1).
  const float pa = _c00 + p * (_c10 + p * (_c20 + p * (_c30 + p * _c40))) +
                   t * (_c01 + p * (_c11 + p * (_c21 + p * _c31)));
  PressureData result{};
  result.pressure = pa / 100.0f;
  if (!result.is_pressure_valid() || result.pressure < MIN_PRESSURE_HPA ||
      result.pressure > MAX_PRESSURE_HPA)
    return false;
  result.altitude =
      ALTITUDE_FACTOR_M * (1.0f - std::pow(result.pressure / SEA_LEVEL_HPA, ALTITUDE_EXPONENT));
  out = result;
  _temperature = temperature;
  return true;
}

bool SPL07::read_coefficients() {
  uint8_t c[COEFFICIENT_BYTES];
  if (!read_register(REG_COEFFICIENTS, c, sizeof(c)))
    return false;
  _c0 = signed_bits((uint32_t(c[0]) << 4) | (c[1] >> 4), 12);
  _c1 = signed_bits((uint32_t(c[1] & 0x0F) << 8) | c[2], 12);
  _c00 = signed_bits((uint32_t(c[3]) << 12) | (uint32_t(c[4]) << 4) | (c[5] >> 4), 20);
  _c10 = signed_bits((uint32_t(c[5] & 0x0F) << 16) | (uint32_t(c[6]) << 8) | c[7], 20);
  _c01 = signed_bits((uint32_t(c[8]) << 8) | c[9], 16);
  _c11 = signed_bits((uint32_t(c[10]) << 8) | c[11], 16);
  _c20 = signed_bits((uint32_t(c[12]) << 8) | c[13], 16);
  _c21 = signed_bits((uint32_t(c[14]) << 8) | c[15], 16);
  _c30 = signed_bits((uint32_t(c[16]) << 8) | c[17], 16);
  _c31 = signed_bits((uint32_t(c[18]) << 4) | (c[19] >> 4), 12);
  _c40 = signed_bits((uint32_t(c[19] & 0x0F) << 8) | c[20], 12);
  return true;
}

bool SPL07::read_register(uint8_t reg, uint8_t *data, size_t len) {
  return _dev != nullptr &&
         i2c_master_transmit_receive(_dev, &reg, 1, data, len, TIMEOUT_MS) == ESP_OK;
}

bool SPL07::write_register(uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  return _dev != nullptr && i2c_master_transmit(_dev, data, sizeof(data), TIMEOUT_MS) == ESP_OK;
}
