#include "test_accel.h"

#include "driver/ledc.h"
#include "esp_log.h"

#include "board_config.h"
#include "rtos.h"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <limits>

static constexpr const char *TAG = "test_accel";

namespace {
#if defined(BOARD_GO)

// Reference-only diagnostic. Bench side-to-side traces are strongest on Y;
// compare deliberate shakes with backpack/cycling traces before product use.
// GPIO3 tests are awake-only; SHAKE is logged without starting an AQ measurement.
enum class Axis : uint8_t { X, Y, Z };

// LIS2DH12 high-resolution samples at +/-4 g: 2 mg per signed 12-bit LSB.
struct Sample {
  static constexpr int16_t INVALID = std::numeric_limits<int16_t>::min();
  static constexpr int MAX_MG = 4096;
  int16_t x_mg = INVALID;
  int16_t y_mg = INVALID;
  int16_t z_mg = INVALID;
  bool overrun = false;
  bool clipped = false;

  static bool field_valid(int16_t value) { return value >= -MAX_MG && value < MAX_MG; }
  bool is_x_valid() const { return field_valid(x_mg); }
  bool is_y_valid() const { return field_valid(y_mg); }
  bool is_z_valid() const { return field_valid(z_mg); }
  bool is_valid() const { return is_x_valid() && is_y_valid() && is_z_valid(); }
};

class Lis2dh12Probe {
public:
  static constexpr uint8_t IDENTITY = 0x33;
  static constexpr uint32_t SAMPLE_INTERVAL_MS = 10; // 100 Hz
  static constexpr uint16_t THRESHOLD_STEP_MG = 32;  // +/-4 g interrupt threshold
  static constexpr uint32_t FILTER_SETTLE_MS = 500;
  static constexpr uint8_t INTERRUPT_ACTIVE = 0x40;
  static constexpr uint8_t AXIS_X = 0x01;
  static constexpr uint8_t AXIS_Y = 0x02;
  static constexpr uint8_t AXIS_Z = 0x04;
  static constexpr uint8_t ALL_AXES = AXIS_X | AXIS_Y | AXIS_Z;

  struct Config {
    uint16_t threshold_mg = 640;
    uint32_t duration_ms = 20;
    uint8_t axes = ALL_AXES;
    // Applies to both INT1 and XYZ outputs. Disable only for raw-data capture.
    bool high_pass = true;
  };

  enum class ReadResult { Ready, NotReady, Error };

  explicit Lis2dh12Probe(i2c_master_bus_handle_t bus);
  ~Lis2dh12Probe();
  Lis2dh12Probe(const Lis2dh12Probe &) = delete;
  Lis2dh12Probe &operator=(const Lis2dh12Probe &) = delete;
  bool ready() const { return _device != nullptr; }
  bool configure(const Config &config);
  bool log_registers();
  bool reset_filter();
  bool interrupt_source(uint8_t &source);
  ReadResult read(Sample &sample);
  bool stop();

private:
  i2c_master_dev_handle_t _device = nullptr;
  bool read_reg(uint8_t reg, uint8_t *data, size_t size);
  bool write_reg(uint8_t reg, uint8_t value);
  bool _identified = false;
};

// Consumes high-pass-filtered XYZ, in mg. Pure logic; no bus or RTOS access.
class ShakeDetector {
public:
  struct Config {
    static constexpr uint32_t MAX_WINDOW_MS = 10000;
    static constexpr uint32_t MAX_COOLDOWN_MS = 60000;
    static constexpr int MAX_DOMINANCE_PERCENT = 1000;
    Axis axis = Axis::Y;
    int peak_mg = 400;
    int release_mg = 150;
    int dominance_percent = 150;
    uint8_t required_peaks = 6; // Three left/right pairs, either starting direction.
    uint32_t min_peak_ms = 50;
    uint32_t max_peak_ms = 400;
    uint32_t window_ms = 1600;
    uint32_t max_sample_gap_ms = 50;
    uint32_t quiet_lead_ms = 0;
    uint32_t cooldown_ms = 3000;
    uint32_t rearm_quiet_ms = 300;
  };
  enum class Result { None, Peak, Shake, Rejected, Invalid, OffAxis };

  explicit ShakeDetector(const Config &config) : _config(config) {}
  static bool valid_config(const Config &config);
  Result update(const Sample &sample, uint32_t now);
  // A new capture cannot continue an old sequence. Cooldown survives captures.
  void reset_capture();
  uint8_t peaks() const { return _peaks; }

private:
  Config _config;
  uint8_t _peaks = 0;
  int _last_sign = 0;
  int _zone = 0;
  uint32_t _first_peak = 0;
  uint32_t _last_peak = 0;
  uint32_t _last_sample = 0;
  uint32_t _quiet_start = 0;
  uint32_t _last_quiet = 0;
  uint32_t _last_shake = 0;
  bool _have_sample = false;
  bool _quiet = false;
  bool _lead_ready = false;
  bool _cooling = false;
  bool _rearm_ready = false;

  void reset_sequence();
};

namespace cfg {
enum class CaptureMode { Continuous, InterruptBurst };
// ESP32 stays awake; GPIO3 starts a bounded XYZ capture instead of continuous polling.
constexpr CaptureMode CAPTURE_MODE = CaptureMode::InterruptBurst;

// Starting hypotheses for bench testing, not validated product thresholds.
constexpr Lis2dh12Probe::Config SENSOR{
    .threshold_mg = 640, // Original IRQ settings verified with continuous diagnostic capture.
    .duration_ms = 20,   // Multiple of 10 ms at 100 Hz.
    .axes = Lis2dh12Probe::ALL_AXES,
    .high_pass = true,
};
constexpr ShakeDetector::Config GESTURE{
    .axis = Axis::Y, // Side-to-side bench traces; opposite-direction peaks are often <800 mg.
    .peak_mg = 400,
    .release_mg = 150,
    .dominance_percent = 150,
    .required_peaks = 6,
    .min_peak_ms = 50, // Natural forearm arcs include qualifying reversals 50..70 ms apart.
    .max_peak_ms = 400,
    .window_ms = 1600,
    .max_sample_gap_ms = 50,
    .quiet_lead_ms = 0,
    .cooldown_ms = 3000,
    .rearm_quiet_ms = 300,
};
constexpr uint32_t BURST_MS = 5000; // Includes time to observe quiet after a shake.
constexpr uint32_t SUMMARY_INTERVAL_MS = 10000;
constexpr uint32_t IRQ_DIAGNOSTIC_INTERVAL_MS = 1000;
constexpr uint32_t IDLE_WAIT_MS = 100;
constexpr uint32_t REARM_DELAY_MS = 30; // Allow latched INT1 to deassert (>2 ODR periods).
constexpr uint32_t WATCHDOG_INTERVAL_MS = 1000;
constexpr uint32_t WATCHDOG_PULSE_MS = 20;
constexpr uint32_t SENSOR_STALL_MS = 250;
// Reference-only sound switch; no product settings are loaded by this test.
constexpr bool BUZZER_ACK_ENABLED = true;
constexpr uint32_t BUZZER_ACK_FREQ_HZ = 2700; // Same frequency as Go's buzzer driver.
constexpr uint32_t BUZZER_ACK_MS = 100;

// A burst has no samples preceding its IRQ. Quiet lead-in needs continuous
// capture until a later FIFO-based implementation can supply that history.
static_assert(CAPTURE_MODE == CaptureMode::Continuous || GESTURE.quiet_lead_ms == 0,
              "Quiet lead-in is supported in continuous capture only");
static_assert(BURST_MS > GESTURE.window_ms);
} // namespace cfg

constexpr uint32_t BUS_SPEED_HZ = 400000;
constexpr int IO_TIMEOUT_MS = 50;

Lis2dh12Probe::Lis2dh12Probe(i2c_master_bus_handle_t bus) {
  if (bus == nullptr)
    return;
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = I2C_ADDR_ACCEL_TEST;
  config.scl_speed_hz = BUS_SPEED_HZ;
  if (i2c_master_bus_add_device(bus, &config, &_device) != ESP_OK)
    _device = nullptr;
}

Lis2dh12Probe::~Lis2dh12Probe() {
  stop();
  if (_device != nullptr)
    i2c_master_bus_rm_device(_device);
}

bool Lis2dh12Probe::read_reg(uint8_t reg, uint8_t *data, size_t size) {
  return _device != nullptr && data != nullptr && size != 0 &&
         i2c_master_transmit_receive(_device, &reg, 1, data, size, IO_TIMEOUT_MS) == ESP_OK;
}

bool Lis2dh12Probe::write_reg(uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  return _device != nullptr &&
         i2c_master_transmit(_device, data, sizeof(data), IO_TIMEOUT_MS) == ESP_OK;
}

constexpr uint8_t WHO_AM_I = 0x0f;
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
constexpr uint8_t AUTO_INCREMENT = 0x80;
constexpr uint8_t ODR_100HZ_XYZ = 0x57;
constexpr uint8_t BDU_HR_4G = 0x98;
constexpr uint8_t HP_OUTPUT_AND_INT1 = 0x09; // Normal HP, cutoff 2 Hz at 100 Hz ODR.
constexpr uint8_t ROUTE_IA1 = 0x40;
constexpr uint8_t LATCH_INT1 = 0x08;
constexpr uint8_t XYZ_READY = 0x08;
constexpr uint8_t XYZ_OVERRUN = 0x80;
constexpr uint8_t THRESHOLD_MAX = 127;
constexpr uint8_t X_HIGH = 0x02;
constexpr uint8_t Y_HIGH = 0x08;
constexpr uint8_t Z_HIGH = 0x20;
constexpr int16_t RAW_MIN = -2048;
constexpr int16_t RAW_MAX = 2047;
constexpr int RAW_ALIGNMENT = 16;
constexpr int MG_PER_LSB = 2;

int16_t decode(const uint8_t *bytes) {
  const auto raw = static_cast<int16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
  return raw / RAW_ALIGNMENT;
}

bool Lis2dh12Probe::configure(const Config &config) {
  // Reject rather than silently round tuning values.
  if (config.threshold_mg == 0 || config.threshold_mg % THRESHOLD_STEP_MG != 0 ||
      config.threshold_mg / THRESHOLD_STEP_MG > THRESHOLD_MAX ||
      config.duration_ms % SAMPLE_INTERVAL_MS != 0 ||
      config.duration_ms / SAMPLE_INTERVAL_MS > THRESHOLD_MAX || config.axes == 0 ||
      (config.axes & ~ALL_AXES) != 0) {
    return false;
  }
  uint8_t identity = 0;
  if (!read_reg(WHO_AM_I, &identity, 1) || identity != IDENTITY) {
    return false;
  }
  _identified = true;
  // Clear any configuration retained by the chip across an ESP reset. Configure
  // while powered down; route the interrupt only after every setting succeeds.
  if (!write_reg(CTRL3, 0) || !write_reg(CTRL1, 0) || !write_reg(INT1_CFG, 0) ||
      !write_reg(CTRL2, config.high_pass ? HP_OUTPUT_AND_INT1 : 0) ||
      !write_reg(CTRL4, BDU_HR_4G) || !write_reg(CTRL5, LATCH_INT1) || !write_reg(CTRL6, 0) ||
      !write_reg(FIFO_CTRL, 0) || !write_reg(ACT_THS, 0) ||
      !write_reg(INT1_THS, config.threshold_mg / THRESHOLD_STEP_MG) ||
      !write_reg(INT1_DURATION, config.duration_ms / SAMPLE_INTERVAL_MS)) {
    return false;
  }
  // OR of absolute-acceleration high events. XL/YL/ZL mean BELOW magnitude
  // threshold in this mode; they are not negative-direction peak detectors.
  uint8_t events = 0;
  if (config.axes & AXIS_X)
    events |= X_HIGH;
  if (config.axes & AXIS_Y)
    events |= Y_HIGH;
  if (config.axes & AXIS_Z)
    events |= Z_HIGH;
  return write_reg(INT1_CFG, events) && write_reg(CTRL1, ODR_100HZ_XYZ) &&
         write_reg(CTRL3, ROUTE_IA1);
}

bool Lis2dh12Probe::reset_filter() {
  uint8_t unused = 0;
  return read_reg(REFERENCE, &unused, 1);
}

bool Lis2dh12Probe::log_registers() {
  // Do not read REFERENCE or INT1_SRC here: those reads reset the filter/latch.
  const struct {
    const char *name;
    uint8_t address;
  } registers[] = {
      {"WHO_AM_I", WHO_AM_I},   {"CTRL1", CTRL1},
      {"CTRL2", CTRL2},         {"CTRL3", CTRL3},
      {"CTRL4", CTRL4},         {"CTRL5", CTRL5},
      {"CTRL6", CTRL6},         {"INT1_CFG", INT1_CFG},
      {"INT1_THS", INT1_THS},   {"INT1_DURATION", INT1_DURATION},
      {"FIFO_CTRL", FIFO_CTRL}, {"ACT_THS", ACT_THS},
  };
  for (const auto &reg : registers) {
    uint8_t value = 0;
    if (!read_reg(reg.address, &value, 1)) {
      ESP_LOGE(TAG, "REG %s (0x%02x): read failed", reg.name, reg.address);
      return false;
    }
    ESP_LOGI(TAG, "REG %s (0x%02x)=0x%02x", reg.name, reg.address, value);
  }
  return true;
}

bool Lis2dh12Probe::interrupt_source(uint8_t &source) {
  source = 0;
  return read_reg(INT1_SRC, &source, 1);
}

Lis2dh12Probe::ReadResult Lis2dh12Probe::read(Sample &sample) {
  sample = {};
  uint8_t status = 0;
  if (!read_reg(STATUS, &status, 1))
    return ReadResult::Error;
  if (!(status & XYZ_READY))
    return ReadResult::NotReady;
  uint8_t bytes[6] = {};
  if (!read_reg(OUT_X_L | AUTO_INCREMENT, bytes, sizeof(bytes)))
    return ReadResult::Error;
  const int16_t x = decode(bytes);
  const int16_t y = decode(bytes + 2);
  const int16_t z = decode(bytes + 4);
  sample.x_mg = x * MG_PER_LSB;
  sample.y_mg = y * MG_PER_LSB;
  sample.z_mg = z * MG_PER_LSB;
  sample.overrun = (status & XYZ_OVERRUN) != 0;
  sample.clipped =
      x == RAW_MIN || x == RAW_MAX || y == RAW_MIN || y == RAW_MAX || z == RAW_MIN || z == RAW_MAX;
  return ReadResult::Ready;
}

bool Lis2dh12Probe::stop() {
  // A wrong WHO_AM_I must never lead to writes to an unrelated device.
  if (!_identified)
    return true;
  const bool disconnected = write_reg(CTRL3, 0);
  const bool powered_down = write_reg(CTRL1, 0);
  return disconnected && powered_down;
}

bool ShakeDetector::valid_config(const Config &c) {
  return static_cast<unsigned>(c.axis) <= static_cast<unsigned>(Axis::Z) && c.release_mg > 0 &&
         c.peak_mg > c.release_mg && c.peak_mg < Sample::MAX_MG && c.dominance_percent >= 100 &&
         c.dominance_percent <= Config::MAX_DOMINANCE_PERCENT && c.required_peaks >= 4 &&
         c.min_peak_ms > 0 && c.min_peak_ms <= Config::MAX_WINDOW_MS &&
         c.max_peak_ms >= c.min_peak_ms && c.window_ms >= c.min_peak_ms * (c.required_peaks - 1) &&
         c.window_ms <= Config::MAX_WINDOW_MS && c.max_peak_ms <= c.window_ms &&
         c.max_sample_gap_ms > 0 && c.max_sample_gap_ms <= c.min_peak_ms &&
         c.quiet_lead_ms <= Config::MAX_WINDOW_MS && c.cooldown_ms <= Config::MAX_COOLDOWN_MS &&
         c.rearm_quiet_ms > 0 && c.rearm_quiet_ms <= Config::MAX_WINDOW_MS;
}

void ShakeDetector::reset_sequence() {
  _peaks = 0;
  _last_sign = 0;
  _zone = 0;
  _lead_ready = false;
}

void ShakeDetector::reset_capture() {
  reset_sequence();
  _have_sample = false;
  _quiet = false;
}

ShakeDetector::Result ShakeDetector::update(const Sample &sample, uint32_t now) {
  if (!valid_config(_config) || !sample.is_valid() || sample.clipped) {
    reset_capture();
    return Result::Invalid;
  }
  if (_have_sample && (now - _last_sample) > _config.max_sample_gap_ms) {
    reset_capture();
  }
  if (sample.overrun)
    reset_capture(); // Missing samples cannot complete a sequence.
  _have_sample = true;
  _last_sample = now;

  const int values[] = {sample.x_mg, sample.y_mg, sample.z_mg};
  const unsigned axis = static_cast<unsigned>(_config.axis);
  const int along = values[axis];
  const int amplitude = std::abs(along);
  const int across = std::max(std::abs(values[(axis + 1) % 3]), std::abs(values[(axis + 2) % 3]));
  const bool quiet = std::max(amplitude, across) <= _config.release_mg;
  if (quiet) {
    if (!_quiet)
      _quiet_start = now;
    _quiet = true;
    _last_quiet = now;
    if ((now - _quiet_start) >= _config.quiet_lead_ms)
      _lead_ready = true;
    if (_cooling && (now - _quiet_start) >= _config.rearm_quiet_ms)
      _rearm_ready = true;
  } else {
    _quiet = false;
  }

  if (_cooling) {
    if ((now - _last_shake) < _config.cooldown_ms || !_rearm_ready) {
      return Result::None;
    }
    _cooling = false;
  }

  bool expired = false;
  if (_peaks != 0 &&
      ((now - _last_peak) > _config.max_peak_ms || (now - _first_peak) > _config.window_ms)) {
    reset_sequence();
    expired = true;
  }
  if (amplitude <= _config.release_mg)
    _zone = 0;
  // Qualify each accepted peak, not every sample between peaks. Brief X/Z
  // spikes in a hand-held shake must not erase earlier qualified Y peaks.
  // A skipped peak cannot advance the sequence or extend its timeout.
  if (amplitude < _config.peak_mg || amplitude * 100 < across * _config.dominance_percent) {
    // Allow the gesture to ramp up after stillness, but expire old qualification.
    if (!quiet && _peaks == 0 && now - _last_quiet > _config.max_peak_ms)
      _lead_ready = false;
    if (expired)
      return Result::Rejected;
    return amplitude >= _config.peak_mg ? Result::OffAxis : Result::None;
  }

  const int sign = along > 0 ? 1 : -1;
  if (sign == _zone)
    return Result::None; // A held peak counts only once.
  _zone = sign;
  if (_peaks == 0) {
    if (_config.quiet_lead_ms != 0 && (!_lead_ready || now - _last_quiet > _config.max_peak_ms))
      return Result::None;
    _first_peak = now;
  } else if (sign == _last_sign || (now - _last_peak) < _config.min_peak_ms) {
    reset_sequence();
    return Result::Rejected;
  }
  _last_sign = sign;
  _last_peak = now;
  ++_peaks;
  if (_peaks < _config.required_peaks)
    return Result::Peak;

  _last_shake = now;
  _cooling = true;
  _rearm_ready = false;
  reset_sequence();
  _quiet = false;
  return Result::Shake;
}

constexpr uint32_t IRQ_QUEUE_DEPTH = 1; // INT1 is latched; one pending candidate suffices.

void on_interrupt(void *arg) {
  const uint8_t token = 1;
  RTOS::queue_send_from_isr(static_cast<RtosQueueHandle>(arg), &token);
}

const char *result_name(ShakeDetector::Result result) {
  switch (result) {
  case ShakeDetector::Result::None:
    return "sample";
  case ShakeDetector::Result::Peak:
    return "peak";
  case ShakeDetector::Result::Shake:
    return "SHAKE";
  case ShakeDetector::Result::Rejected:
    return "reject";
  case ShakeDetector::Result::Invalid:
    return "invalid";
  case ShakeDetector::Result::OffAxis:
    return "skip-axis";
  }
  return "invalid";
}

// Ensure no callback survives its queue or the I2C adapter on setup/read errors.
struct InterruptGuard {
  const gpio::Hal &hal;
  RtosQueueHandle queue = nullptr;
  bool attached = false;
  ~InterruptGuard() {
    if (attached) {
      hal.disable_interrupt(PIN_ACCEL_INT);
      hal.remove_interrupt_handler(PIN_ACCEL_INT);
    }
    RTOS::queue_delete(queue);
  }
};

// Reference-local buzzer hardware adapter, matching Go's LEDC configuration.
// PWM plays independently; tick() ends the beep without delaying sensor reads.
class BuzzerAck {
public:
  ~BuzzerAck() {
    if (_ready)
      ledc_stop(MODE, CHANNEL, 0);
  }

  bool init() {
    if (!cfg::BUZZER_ACK_ENABLED)
      return true;
    ledc_timer_config_t timer = {};
    timer.speed_mode = MODE;
    timer.timer_num = TIMER;
    timer.duty_resolution = RESOLUTION;
    timer.freq_hz = cfg::BUZZER_ACK_FREQ_HZ;
    timer.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&timer) != ESP_OK)
      return false;

    ledc_channel_config_t channel = {};
    channel.speed_mode = MODE;
    channel.channel = CHANNEL;
    channel.timer_sel = TIMER;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.gpio_num = PIN_ACCEL_TEST_BUZZER;
    channel.duty = 0; // Start muted; only an accepted SHAKE gets an ACK.
    if (ledc_channel_config(&channel) != ESP_OK)
      return false;
    _ready = true;
    return true;
  }

  bool beep() {
    if (!cfg::BUZZER_ACK_ENABLED)
      return true;
    if (!_ready || ledc_set_duty(MODE, CHANNEL, DUTY) != ESP_OK ||
        ledc_update_duty(MODE, CHANNEL) != ESP_OK)
      return false;
    _started_ms = static_cast<uint32_t>(RTOS::get_time_ms());
    _playing = true;
    return true;
  }

  bool tick(uint32_t now) {
    if (!_playing || now - _started_ms < cfg::BUZZER_ACK_MS)
      return true;
    if (ledc_stop(MODE, CHANNEL, 0) != ESP_OK)
      return false;
    _playing = false;
    return true;
  }

  bool playing() const { return _playing; }

private:
  static constexpr ledc_mode_t MODE = LEDC_LOW_SPEED_MODE;
  static constexpr ledc_timer_t TIMER = LEDC_TIMER_0;
  static constexpr ledc_channel_t CHANNEL = LEDC_CHANNEL_0;
  static constexpr ledc_timer_bit_t RESOLUTION = LEDC_TIMER_10_BIT;
  static constexpr uint32_t DUTY = (1u << RESOLUTION) / 2; // 50% duty.
  uint32_t _started_ms = 0;
  bool _ready = false;
  bool _playing = false;
};

// Pulse without sleeping in the 100 Hz capture loop. Idle queue waits remain
// bounded so the Go external watchdog is serviced even with no motion.
class Watchdog {
public:
  explicit Watchdog(const gpio::Hal &hal) : _hal(hal) {}
  bool init() {
    return _hal.configure(PIN_ACCEL_TEST_WDT, gpio::Mode::Output, gpio::PullMode::Floating,
                          gpio::InterruptType::Disabled) &&
           _hal.set_level(PIN_ACCEL_TEST_WDT, 0);
  }
  void tick(uint32_t now) {
    if (_high && now - _edge >= cfg::WATCHDOG_PULSE_MS) {
      _hal.set_level(PIN_ACCEL_TEST_WDT, 0);
      _high = false;
    } else if (!_high && now - _edge >= cfg::WATCHDOG_INTERVAL_MS) {
      _hal.set_level(PIN_ACCEL_TEST_WDT, 1);
      _edge = now;
      _high = true;
    }
  }

private:
  const gpio::Hal &_hal;
  uint32_t _edge = 0;
  bool _high = false;
};
#endif
} // namespace

void run_test_accel(i2c_master_bus_handle_t bus, const gpio::Hal &hal) {
#if !defined(BOARD_GO)
  (void)bus;
  (void)hal;
  ESP_LOGE(TAG, "Accelerometer diagnostic requires BOARD_GO (INT1 on GPIO3)");
#else
  if (!ShakeDetector::valid_config(cfg::GESTURE)) {
    ESP_LOGE(TAG, "Invalid gesture settings");
    return;
  }
  Watchdog watchdog(hal);
  if (!watchdog.init()) {
    ESP_LOGE(TAG, "External watchdog GPIO setup failed");
    return;
  }
  BuzzerAck buzzer;
  if (!buzzer.init()) {
    ESP_LOGE(TAG, "Buzzer ACK setup failed");
    return;
  }
  Lis2dh12Probe probe(bus);
  InterruptGuard irq{hal};
  if (!probe.ready() || !probe.configure(cfg::SENSOR)) {
    ESP_LOGE(TAG, "LIS2DH12 probe/config failed: check 0x18, wiring and tuning values");
    return;
  }
  if (!probe.log_registers())
    return;
  RTOS::delay_ms(Lis2dh12Probe::FILTER_SETTLE_MS);
  uint8_t source = 0;
  if (!probe.reset_filter() || !probe.interrupt_source(source)) {
    ESP_LOGE(TAG, "Filter/interrupt reset failed");
    return;
  }
  RTOS::delay_ms(cfg::REARM_DELAY_MS);
  irq.queue = RTOS::queue_create(IRQ_QUEUE_DEPTH, sizeof(uint8_t));
  if (irq.queue == nullptr ||
      !hal.configure(PIN_ACCEL_INT, gpio::Mode::Input, gpio::PullMode::Floating,
                     gpio::InterruptType::RisingEdge) ||
      !hal.add_interrupt_handler(PIN_ACCEL_INT, on_interrupt, irq.queue)) {
    ESP_LOGE(TAG, "GPIO3 interrupt setup failed");
    return;
  }
  irq.attached = true;
  if (!hal.enable_interrupt(PIN_ACCEL_INT)) {
    ESP_LOGE(TAG, "GPIO3 interrupt enable failed");
    return;
  }

  constexpr bool continuous = cfg::CAPTURE_MODE == cfg::CaptureMode::Continuous;
  ESP_LOGI(TAG, "LIS2DH12 100 Hz +/-4g HR; GPIO3 active-high latched; mode=%s",
           continuous ? "continuous" : "interrupt-burst");
  ESP_LOGI(TAG, "IRQ threshold=%u mg duration=%" PRIu32 " ms axes=0x%x HP=%d; gesture axis=%c",
           cfg::SENSOR.threshold_mg, cfg::SENSOR.duration_ms, cfg::SENSOR.axes,
           cfg::SENSOR.high_pass, "XYZ"[static_cast<unsigned>(cfg::GESTURE.axis)]);
  ESP_LOGI(TAG, "Sample XYZ is %s; detector %s; IRQ timestamps are task-service times",
           cfg::SENSOR.high_pass ? "high-pass filtered" : "raw (includes gravity)",
           cfg::SENSOR.high_pass ? "enabled" : "disabled in raw mode");
  ESP_LOGI(TAG, "POLL logs: read number, readiness, XYZ (mg), overrun, clipping, peaks, event");
  ESP_LOGI(TAG, "skip-axis: peak lacks axis dominance; sequence waits for next qualifying peak");
  ESP_LOGI(TAG,
           "Gesture peak=%d release=%d mg dominance=%d%% count=%u spacing=%" PRIu32 "..%" PRIu32
           " ms window=%" PRIu32 " ms",
           cfg::GESTURE.peak_mg, cfg::GESTURE.release_mg, cfg::GESTURE.dominance_percent,
           cfg::GESTURE.required_peaks, cfg::GESTURE.min_peak_ms, cfg::GESTURE.max_peak_ms,
           cfg::GESTURE.window_ms);
  ESP_LOGI(TAG,
           "Quiet lead=%" PRIu32 " cooldown=%" PRIu32 " rearm quiet=%" PRIu32 " gap limit=%" PRIu32
           " burst=%" PRIu32 " ms",
           cfg::GESTURE.quiet_lead_ms, cfg::GESTURE.cooldown_ms, cfg::GESTURE.rearm_quiet_ms,
           cfg::GESTURE.max_sample_gap_ms, cfg::BURST_MS);
  ESP_LOGI(TAG, "Awake diagnostic: SHAKE is a candidate classification, not a measurement request");
  ESP_LOGI(TAG, "Buzzer ACK %s: GPIO%d, %" PRIu32 " Hz, %" PRIu32 " ms on accepted SHAKE",
           cfg::BUZZER_ACK_ENABLED ? "enabled" : "disabled", PIN_ACCEL_TEST_BUZZER,
           cfg::BUZZER_ACK_FREQ_HZ, cfg::BUZZER_ACK_MS);
  if (!continuous) {
    ESP_LOGI(TAG, "Waiting for GPIO3 interrupt; XYZ polling is idle");
  } else {
    ESP_LOGI(TAG, "Continuous diagnostic: polling XYZ regardless of GPIO3");
    ESP_LOGI(TAG,
             "IRQ_DIAG checks the chip latch every %" PRIu32
             " ms outside bursts; reading INT1_SRC acknowledges the latch",
             cfg::IRQ_DIAGNOSTIC_INTERVAL_MS);
  }

  ShakeDetector detector(cfg::GESTURE);
  bool burst = false;
  bool burst_shake = false;
  uint32_t burst_start = 0;
  uint32_t last_summary = 0;
  uint32_t last_irq_diagnostic = 0;
  uint32_t last_sample = static_cast<uint32_t>(RTOS::get_time_ms());
  uint32_t interrupts = 0, shakes = 0, rejected_bursts = 0, overruns = 0;
  uint32_t polls = 0;

  while (true) {
    uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
    watchdog.tick(now);
    if (!buzzer.tick(now)) {
      ESP_LOGE(TAG, "Buzzer ACK stop failed; stopping");
      return;
    }
    uint8_t token = 0;
    if (!burst) {
      // A late-burst ACK can still be playing when capture ends. Service its
      // stop at the capture cadence without reading XYZ while waiting for IRQ.
      const uint32_t idle_wait_ms =
          buzzer.playing() ? Lis2dh12Probe::SAMPLE_INTERVAL_MS : cfg::IDLE_WAIT_MS;
      const bool notified = RTOS::queue_receive(irq.queue, &token, continuous ? 0 : idle_wait_ms);
      now = static_cast<uint32_t>(RTOS::get_time_ms());
      // Check level too: a latched event may precede ISR registration/rearming.
      const int irq_level = hal.get_level(PIN_ACCEL_INT);
      if (notified || irq_level == 1) {
        // ISR only queues a token. Log receipt in task context before I2C access.
        ESP_LOGI(TAG, "IRQ received: via=%s GPIO3=%d service_ms=%" PRIu32,
                 notified ? "ISR" : "latched-level", irq_level, now);
        if (!hal.disable_interrupt(PIN_ACCEL_INT) || !probe.interrupt_source(source)) {
          ESP_LOGE(TAG, "Interrupt read/disable failed; stopping");
          return;
        }
        if (source & Lis2dh12Probe::INTERRUPT_ACTIVE) {
          ++interrupts;
          burst = true;
          burst_shake = false;
          burst_start = now;
          if (!continuous)
            detector.reset_capture();
          last_sample = now;
          ESP_LOGI(TAG, "IRQ,%" PRIu32 ",source=0x%02x,count=%" PRIu32, now, source, interrupts);
          ESP_LOGI(TAG,
                   "Burst started: duration=%" PRIu32 " ms poll_delay=%" PRIu32
                   " ms; GPIO3 interrupts masked during capture",
                   cfg::BURST_MS, Lis2dh12Probe::SAMPLE_INTERVAL_MS);
        } else {
          ESP_LOGW(TAG, "IRQ source=0x%02x inactive; no burst started", source);
          if (!hal.enable_interrupt(PIN_ACCEL_INT)) {
            ESP_LOGE(TAG, "GPIO3 interrupt reenable failed");
            return;
          }
        }
      }
    }

    if (continuous && !burst && now - last_irq_diagnostic >= cfg::IRQ_DIAGNOSTIC_INTERVAL_MS) {
      last_irq_diagnostic = now;
      // Capture the pin BEFORE reading INT1_SRC, which clears a latched event.
      // A source event without an observed pin/ISR is diagnostic evidence, not
      // an interrupt-driven burst. Never count it as a GPIO interrupt.
      const int level_before = hal.get_level(PIN_ACCEL_INT);
      uint8_t diagnostic_source = 0;
      if (!probe.interrupt_source(diagnostic_source)) {
        ESP_LOGE(TAG, "IRQ_DIAG: INT1_SRC read failed; stopping");
        return;
      }
      const bool active = (diagnostic_source & Lis2dh12Probe::INTERRUPT_ACTIVE) != 0;
      ESP_LOGI(TAG, "IRQ_DIAG GPIO3_before=%d INT1_SRC=0x%02x IA=%d (latch acknowledged)",
               level_before, diagnostic_source, active);
      if (active && level_before == 0) {
        ESP_LOGW(TAG,
                 "Chip recorded motion while GPIO3 sampled low; compare repeated IRQ_DIAG events");
      }
    }

    if (continuous || burst) {
      Sample sample;
      ++polls;
      const auto read = probe.read(sample);
      if (read == Lis2dh12Probe::ReadResult::Error) {
        ESP_LOGE(TAG, "POLL #%" PRIu32 ": I2C error; stopping (no stale-sample classification)",
                 polls);
        return;
      }
      if (read == Lis2dh12Probe::ReadResult::Ready) {
        last_sample = now;
        if (sample.overrun)
          ++overruns;
        const auto result =
            cfg::SENSOR.high_pass ? detector.update(sample, now) : ShakeDetector::Result::None;
        ESP_LOGI(TAG, "POLL #%" PRIu32 " ready xyz_mg=%d,%d,%d ov=%d clip=%d peaks=%u %s", polls,
                 sample.x_mg, sample.y_mg, sample.z_mg, sample.overrun, sample.clipped,
                 detector.peaks(), result_name(result));
        if (result == ShakeDetector::Result::Shake) {
          ++shakes;
          burst_shake = true;
          if (!buzzer.beep()) {
            ESP_LOGE(TAG, "SHAKE accepted but buzzer ACK failed; stopping");
            return;
          }
          ESP_LOGI(TAG, "SHAKE,%" PRIu32 ",count=%" PRIu32, now, shakes);
        }
      } else {
        ESP_LOGI(TAG, "POLL #%" PRIu32 " not-ready; last sample %" PRIu32 " ms ago", polls,
                 now - last_sample);
        if (now - last_sample >= cfg::SENSOR_STALL_MS) {
          ESP_LOGE(TAG, "No fresh accelerometer samples for %" PRIu32 " ms; stopping",
                   cfg::SENSOR_STALL_MS);
          return;
        }
      }
      RTOS::delay_ms(Lis2dh12Probe::SAMPLE_INTERVAL_MS);
    }

    if (burst && now - burst_start >= cfg::BURST_MS) {
      if (!burst_shake)
        ++rejected_bursts;
      ESP_LOGI(TAG, "BURST_END,%" PRIu32 ",shake=%d,remaining_peaks=%u", now, burst_shake,
               detector.peaks());
      if (!probe.interrupt_source(source)) {
        ESP_LOGE(TAG, "Interrupt clear failed at end of burst");
        return;
      }
      RTOS::delay_ms(cfg::REARM_DELAY_MS);
      while (RTOS::queue_receive(irq.queue, &token, 0)) {
      }
      burst = false;
      if (!hal.enable_interrupt(PIN_ACCEL_INT)) {
        ESP_LOGE(TAG, "GPIO3 interrupt reenable failed");
        return;
      }
      ESP_LOGI(TAG, "GPIO3 rearmed (level=%d); %s", hal.get_level(PIN_ACCEL_INT),
               continuous ? "continuous polling continues" : "XYZ polling idle, waiting for IRQ");
    }
    if (now - last_summary >= cfg::SUMMARY_INTERVAL_MS) {
      last_summary = now;
      ESP_LOGI(TAG,
               "SUMMARY,ms=%" PRIu32 ",irq_windows=%" PRIu32 ",shakes=%" PRIu32
               ",windows_without_shake=%" PRIu32 ",overruns=%" PRIu32 ",polls=%" PRIu32,
               now, interrupts, shakes, rejected_bursts, overruns, polls);
    }
  }
#endif
}
