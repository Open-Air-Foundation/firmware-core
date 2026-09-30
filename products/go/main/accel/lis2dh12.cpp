/**
 * AirGradient Go — LIS2DH12 implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "lis2dh12.h"

#include "ag_log.h"
#include "rtos.h"

namespace {
static constexpr const char *TAG = "LIS2DH12";
constexpr uint8_t REG_WHO_AM_I = 0x0f;
constexpr uint8_t CTRL1 = 0x20;
constexpr uint8_t CTRL2 = 0x21;
constexpr uint8_t CTRL3 = 0x22;
constexpr uint8_t CTRL4 = 0x23;
constexpr uint8_t CTRL5 = 0x24;
constexpr uint8_t CTRL6 = 0x25;
constexpr uint8_t REFERENCE = 0x26;
constexpr uint8_t STATUS = 0x27;
constexpr uint8_t OUT_X_L = 0x28;
constexpr uint8_t FIFO_CTRL = 0x2e;
constexpr uint8_t INT1_CFG = 0x30;
constexpr uint8_t INT1_SRC = 0x31;
constexpr uint8_t INT1_THS = 0x32;
constexpr uint8_t INT1_DURATION = 0x33;
constexpr uint8_t ACT_THS = 0x3e;
constexpr uint8_t AUTO_INCR = 0x80;
constexpr uint8_t ENABLE_XYZ = 0x07;
constexpr uint8_t BDU_HIGH_RESOLUTION = 0x88;
constexpr uint8_t HP_OUTPUT_AND_INT1 = 0x09;
constexpr uint8_t ROUTE_IA1 = 0x40;
constexpr uint8_t LATCH_INT1 = 0x08;
constexpr uint8_t ACTIVE_LOW = 0x02;
constexpr uint8_t INTERRUPT_ACTIVE = 0x40;
constexpr uint8_t XYZ_READY = 0x08;
constexpr uint8_t XYZ_OVERRUN = 0x80;
constexpr uint8_t MAX_INTERRUPT_VALUE = 0x7f;
constexpr uint32_t MS_PER_SECOND = 1000;
constexpr uint32_t HIGH_RES_STARTUP_SAMPLES = 7; // Datasheet table 10: 7/ODR.
constexpr uint32_t FILTER_SETTLE_SAMPLES = 50;
constexpr uint32_t FILTER_RESET_SAMPLES = 3;
constexpr int RAW_ALIGNMENT = 16;
constexpr int16_t RAW_MIN = -2048;
constexpr int16_t RAW_MAX = 2047;

struct Rate {
  uint16_t hz;
  uint8_t bits;
};
constexpr Rate RATES[] = {{1, 0x10},   {10, 0x20},  {25, 0x30}, {50, 0x40},
                          {100, 0x50}, {200, 0x60}, {400, 0x70}};
struct Range {
  AccelRange range;
  uint8_t bits;
  uint8_t mg_per_lsb;
  uint8_t threshold_step_mg;
};
constexpr Range RANGES[] = {{AccelRange::G2, 0x00, 1, 16},
                            {AccelRange::G4, 0x10, 2, 32},
                            {AccelRange::G8, 0x20, 4, 62},
                            {AccelRange::G16, 0x30, 12, 186}};

int16_t decode(const uint8_t *bytes) {
  const auto raw = static_cast<int16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
  return raw / RAW_ALIGNMENT;
}

// INT1_CFG interleaves the low/high event bits for X, Y, and Z.
uint8_t interrupt_axes(uint8_t axes, bool high) {
  uint8_t bits = 0;
  for (unsigned axis = 0; axis < 3; ++axis) {
    if (axes & (1U << axis)) {
      bits |= 1U << (2 * axis + (high ? 1 : 0));
    }
  }
  return bits;
}
} // namespace

LIS2DH12::LIS2DH12(i2c_master_bus_handle_t bus, const Config &config)
    : _config(config), _bus(bus) {}

LIS2DH12::~LIS2DH12() {
  power_down();
  if (_dev != nullptr) {
    i2c_master_bus_rm_device(_dev);
    _dev = nullptr;
  }
}

bool LIS2DH12::init() {
  _identified = false;
  _configured = false;
  if (_bus == nullptr) {
    return false;
  }
  if (_dev == nullptr) {
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = _config.address,
        .scl_speed_hz = _config.scl_speed_hz,
        .scl_wait_us = 0,
        .flags = {.disable_ack_check = 0},
    };
    if (i2c_master_bus_add_device(_bus, &cfg, &_dev) != ESP_OK) {
      AG_LOGE(TAG, "bus_add_device failed at addr 0x%02X", _config.address);
      return false;
    }
  }

  const uint8_t id = who_am_i();
  if (id != WHO_AM_I_EXPECTED) {
    AG_LOGE(TAG, "WHO_AM_I mismatch: got 0x%02X expected 0x%02X", id, WHO_AM_I_EXPECTED);
    return false;
  }
  _identified = true;
  return power_down();
}

uint8_t LIS2DH12::who_am_i() {
  uint8_t value = 0;
  if (!_read_reg(REG_WHO_AM_I, value)) {
    return 0;
  }
  return value;
}

bool LIS2DH12::configure(const AccelConfig &config) {
  if (!_identified) {
    AG_LOGE(TAG, "configure: sensor not identified");
    return false;
  }
  uint8_t rate_bits = 0;
  for (const auto &rate : RATES) {
    if (rate.hz == config.sample_rate_hz) {
      rate_bits = rate.bits;
      break;
    }
  }
  const Range *range = nullptr;
  for (const auto &candidate : RANGES) {
    if (candidate.range == config.range) {
      range = &candidate;
      break;
    }
  }
  const auto &irq = config.interrupt;
  if (rate_bits == 0 || range == nullptr ||
      ((irq.high_axes | irq.low_axes) & ~AccelAxis::ALL) != 0) {
    AG_LOGE(TAG, "configure: invalid rate, range, or interrupt axes");
    return false;
  }
  const uint8_t axes = interrupt_axes(irq.high_axes, true) | interrupt_axes(irq.low_axes, false);
  const uint32_t duration = static_cast<uint32_t>(irq.duration_ms) * config.sample_rate_hz;
  // Reject rounding: accepting another threshold/duration would silently change behavior.
  if ((axes != 0 && irq.threshold_mg == 0) || irq.threshold_mg % range->threshold_step_mg != 0 ||
      irq.threshold_mg / range->threshold_step_mg > MAX_INTERRUPT_VALUE ||
      duration % MS_PER_SECOND != 0 || duration / MS_PER_SECOND > MAX_INTERRUPT_VALUE) {
    AG_LOGE(TAG, "configure: invalid interrupt threshold or duration");
    return false;
  }

  _configured = false;
  uint8_t unused = 0;
  // Disconnect INT1 and stop sampling before changing scale/filter settings.
  // Reset the filtering block on leaving power-down (datasheet table 10).
  if (!_write_reg(CTRL3, 0) || !_write_reg(CTRL1, 0) || !_read_reg(REFERENCE, unused) ||
      !_write_reg(INT1_CFG, 0) || !_write_reg(CTRL2, config.high_pass ? HP_OUTPUT_AND_INT1 : 0) ||
      !_write_reg(CTRL4, BDU_HIGH_RESOLUTION | range->bits) ||
      !_write_reg(CTRL5, irq.latched ? LATCH_INT1 : 0) ||
      !_write_reg(CTRL6, irq.active_low ? ACTIVE_LOW : 0) || !_write_reg(FIFO_CTRL, 0) ||
      !_write_reg(ACT_THS, 0) ||
      !_write_reg(INT1_THS, irq.threshold_mg / range->threshold_step_mg) ||
      !_write_reg(INT1_DURATION, duration / MS_PER_SECOND) || !_write_reg(INT1_CFG, axes) ||
      !_write_reg(CTRL1, rate_bits | ENABLE_XYZ) || !_write_reg(CTRL3, axes != 0 ? ROUTE_IA1 : 0)) {
    AG_LOGE(TAG, "configure: register setup failed");
    power_down();
    return false;
  }
  const uint32_t settle_samples =
      config.high_pass ? FILTER_SETTLE_SAMPLES : HIGH_RES_STARTUP_SAMPLES;
  RTOS::delay_ms((settle_samples * MS_PER_SECOND + config.sample_rate_hz - 1) /
                 config.sample_rate_hz);
  if ((config.high_pass && !_read_reg(REFERENCE, unused)) || !_read_reg(INT1_SRC, unused)) {
    AG_LOGE(TAG, "configure: filter reset or interrupt clear failed");
    power_down();
    return false;
  }
  if (config.high_pass) {
    RTOS::delay_ms((FILTER_RESET_SAMPLES * MS_PER_SECOND + config.sample_rate_hz - 1) /
                   config.sample_rate_hz);
  }
  _mg_per_lsb = range->mg_per_lsb;
  _configured = true;
  AG_LOGI(TAG, "configured: %u Hz range=%u mg/count=%u high-pass=%d INT1=0x%02X",
          config.sample_rate_hz, static_cast<unsigned>(config.range), _mg_per_lsb, config.high_pass,
          axes);
  return true;
}

AccelReadResult LIS2DH12::read(AccelReading &out) {
  out = {};
  uint8_t status = 0;
  if (!_configured || !_read_reg(STATUS, status)) {
    return AccelReadResult::Error;
  }
  if (!(status & XYZ_READY)) {
    return AccelReadResult::NotReady;
  }
  uint8_t bytes[6] = {};
  if (!_read_block(OUT_X_L | AUTO_INCR, bytes, sizeof(bytes))) {
    return AccelReadResult::Error;
  }
  const int16_t x = decode(bytes);
  const int16_t y = decode(bytes + 2);
  const int16_t z = decode(bytes + 4);
  out.x_mg = x * _mg_per_lsb;
  out.y_mg = y * _mg_per_lsb;
  out.z_mg = z * _mg_per_lsb;
  out.overrun = (status & XYZ_OVERRUN) != 0;
  out.clipped =
      x == RAW_MIN || x == RAW_MAX || y == RAW_MIN || y == RAW_MAX || z == RAW_MIN || z == RAW_MAX;
  return AccelReadResult::Ready;
}

bool LIS2DH12::read_interrupt(bool &active) {
  active = false;
  uint8_t source = 0;
  if (!_configured || !_read_reg(INT1_SRC, source)) {
    return false;
  }
  active = (source & INTERRUPT_ACTIVE) != 0;
  return true;
}

bool LIS2DH12::power_down() {
  _configured = false;
  if (!_identified) {
    return true;
  }
  const bool disconnected = _write_reg(CTRL3, 0);
  const bool powered_down = _write_reg(CTRL1, 0);
  return disconnected && powered_down;
}

bool LIS2DH12::_write_reg(uint8_t reg, uint8_t value) {
  if (_dev == nullptr) {
    return false;
  }
  uint8_t buf[2] = {reg, value};
  return i2c_master_transmit(_dev, buf, sizeof(buf), _config.timeout_ms) == ESP_OK;
}

bool LIS2DH12::_read_reg(uint8_t reg, uint8_t &out) {
  if (_dev == nullptr) {
    return false;
  }
  return i2c_master_transmit_receive(_dev, &reg, 1, &out, 1, _config.timeout_ms) == ESP_OK;
}

bool LIS2DH12::_read_block(uint8_t reg, uint8_t *buf, size_t len) {
  if (_dev == nullptr || buf == nullptr || len == 0) {
    return false;
  }
  return i2c_master_transmit_receive(_dev, &reg, 1, buf, len, _config.timeout_ms) == ESP_OK;
}
