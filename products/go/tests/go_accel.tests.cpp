#include <catch2/catch_test_macros.hpp>

#include "accel/shake_detector.h"
#include "go_accel_test_access.h"

#include <vector>
#include <string>

namespace {
AccelReading sample(int16_t y, int16_t x = 0, int16_t z = 0) {
  AccelReading out;
  out.x_mg = x;
  out.y_mg = y;
  out.z_mg = z;
  return out;
}

int shake(ShakeDetector &detector, uint32_t &now, int direction = 1, uint32_t spacing = 100) {
  int accepted = 0;
  for (int peak = 0; peak < 6; ++peak) {
    for (uint32_t t = 0; t < spacing; t += 10) {
      const auto value = sample(t < spacing / 2 ? direction * 600 : 0);
      accepted += detector.update(value, now) == ShakeDetector::Result::Shake;
      now += 10;
    }
    direction = -direction;
  }
  return accepted;
}
} // namespace

TEST_CASE("Shake: six alternating Y excursions accept either starting sign", "[Shake]") {
  for (int direction : {-1, 1}) {
    ShakeDetector detector({});
    uint32_t now = 1000;
    CHECK(shake(detector, now, direction) == 1);
  }
}

TEST_CASE("Shake: timing limits and timestamp rollover", "[Shake]") {
  SECTION("50 ms reversals qualify") {
    ShakeDetector detector({});
    uint32_t now = 1000;
    CHECK(shake(detector, now, 1, 50) == 1);
  }
  SECTION("40 ms reversals do not qualify") {
    ShakeDetector detector({});
    uint32_t now = 1000;
    CHECK(shake(detector, now, 1, 40) == 0);
  }
  SECTION("six peaks must fit the overall window") {
    ShakeDetector detector({});
    uint32_t now = 1000;
    CHECK(shake(detector, now, 1, 350) == 0);
  }
  SECTION("wrapping the millisecond counter preserves the gesture") {
    ShakeDetector detector({});
    uint32_t now = UINT32_MAX - 200;
    CHECK(shake(detector, now) == 1);
  }
}

TEST_CASE("Shake: held and renewed same-direction peaks cannot complete a gesture", "[Shake]") {
  ShakeDetector detector({});
  for (uint32_t t = 0; t < 2000; t += 10) {
    CHECK(detector.update(sample(600), t) != ShakeDetector::Result::Shake);
  }
  detector.reset_capture();
  CHECK(detector.update(sample(600), 2100) == ShakeDetector::Result::Peak);
  CHECK(detector.update(sample(0), 2110) == ShakeDetector::Result::None);
  CHECK(detector.update(sample(600), 2120) == ShakeDetector::Result::Rejected);
  CHECK(detector.peaks() == 0);
}

TEST_CASE("Shake: intermediate off-axis motion preserves qualified Y peaks", "[Shake]") {
  ShakeDetector detector({});
  uint32_t now = 1000;
  int accepted = 0;
  for (int peak = 0; peak < 6; ++peak) {
    const int sign = peak % 2 ? -1 : 1;
    accepted += detector.update(sample(sign * 600), now) == ShakeDetector::Result::Shake;
    for (int tick = 1; tick < 10; ++tick) {
      // Forearm arc contains strong cross-axis samples between Y excursions.
      detector.update(sample(sign * 600, 900), now + tick * 10);
    }
    now += 100;
  }
  CHECK(accepted == 1);
}

TEST_CASE("Shake: weak and off-axis excursions do not qualify", "[Shake]") {
  ShakeDetector detector({});
  bool off_axis = false;
  SECTION("below threshold") {}
  SECTION("Y does not dominate") { off_axis = true; }
  for (uint32_t now = 0; now < 1000; now += 10) {
    const int sign = (now / 100) % 2 ? -1 : 1;
    auto value = off_axis ? sample(sign * 600, 500) : sample(sign * 396);
    CHECK(detector.update(value, now) != ShakeDetector::Result::Shake);
  }
  CHECK(detector.peaks() == 0);
}

TEST_CASE("Shake: broken sample continuity cannot finish a partial gesture", "[Shake]") {
  ShakeDetector detector({});
  uint32_t now = 1000;
  for (int peak = 0; peak < 5; ++peak) {
    for (int tick = 0; tick < 10; ++tick) {
      detector.update(sample((peak % 2 ? -1 : 1) * 600), now);
      now += 10;
    }
  }
  REQUIRE(detector.peaks() == 5);
  auto last = sample(-600);
  SECTION("overrun") { last.overrun = true; }
  SECTION("clipping") { last.clipped = true; }
  SECTION("invalid field") { last.x_mg = AccelReading::INVALID; }
  SECTION("sample gap") { now += 60; }
  SECTION("new capture") { detector.reset_capture(); }
  CHECK(detector.update(last, now) != ShakeDetector::Result::Shake);
  CHECK(detector.peaks() <= 1);
}

TEST_CASE("Shake: cooldown lasts one second without requiring quiet samples", "[Shake]") {
  ShakeDetector detector({});
  uint32_t now = 1000;
  SECTION("normal uptime") {}
  SECTION("millisecond counter wraps") { now = UINT32_MAX - 200; }
  const uint32_t started = now;
  REQUIRE(shake(detector, now) == 1);
  detector.reset_capture();
  // The first shake was detected on its sixth peak, 500 ms after starting.
  now = started + 500 + 999;
  CHECK(detector.update(sample(600), now) == ShakeDetector::Result::None);
  CHECK(detector.peaks() == 0);
  ++now;
  CHECK(shake(detector, now, -1) == 1);
}

namespace {
struct GpioState {
  int level = 0;
  int enables = 0;
  int disables = 0;
  int removes = 0;
  bool enable_ok = true;
  bool disable_ok = true;
  bool remove_ok = true;
  gpio::InterruptHandler handler = nullptr;
  void *arg = nullptr;
} gpio_state;

bool configure(int, gpio::Mode, gpio::PullMode, gpio::InterruptType) { return true; }
int get_level(int) { return gpio_state.level; }
bool set_level(int, int) { return true; }
bool attach(int, gpio::InterruptHandler handler, void *arg) {
  gpio_state.handler = handler;
  gpio_state.arg = arg;
  return true;
}
bool detach(int) {
  ++gpio_state.removes;
  if (!gpio_state.remove_ok) {
    return false;
  }
  gpio_state.handler = nullptr;
  return true;
}
bool enable(int) {
  ++gpio_state.enables;
  return gpio_state.enable_ok;
}
bool disable(int) {
  ++gpio_state.disables;
  return gpio_state.disable_ok;
}
const gpio::Hal gpio_hal{configure, get_level, set_level, attach, detach, enable, disable};

class Clock : public RTOS {
public:
  AccelService *service = nullptr;
  uint32_t now = 1000;
  bool fail_reply_delivery = false;
  bool queue_send_impl(RtosQueueHandle queue, const void *item, uint32_t timeout_ms) override {
    if (fail_reply_delivery && service != nullptr &&
        AccelServiceTestAccess::is_reply_queue(*service, queue)) {
      return false;
    }
    return RTOS::queue_send_impl(queue, item, timeout_ms);
  }
  bool queue_receive_impl(RtosQueueHandle queue, void *item, uint32_t timeout_ms) override {
    if (service != nullptr && AccelServiceTestAccess::is_reply_queue(*service, queue)) {
      AccelServiceTestAccess::process_pending(*service);
    }
    return RTOS::queue_receive_impl(queue, item, timeout_ms);
  }
  void delay_ms_impl(uint32_t ms) override { now += ms; }
  uint64_t get_time_ms_impl() override { return now; }
};

class MotionSensor : public AccelSensor {
public:
  bool active = true;
  bool configure_ok = true;
  bool source_ok = true;
  bool init_ok = true;
  AccelConfig last_config{};
  int reads = 0;
  int source_reads = 0;
  AccelReadResult result = AccelReadResult::Ready;
  AccelReading next = sample(0);
  std::vector<const char *> operations;

  bool init() override {
    operations.push_back("init");
    return init_ok;
  }
  uint8_t who_am_i() override { return 0x33; }
  uint8_t expected_who_am_i() const override { return 0x33; }
  bool configure(const AccelConfig &config) override {
    last_config = config;
    operations.push_back("configure");
    return configure_ok;
  }
  bool power_down() override {
    operations.push_back("stop");
    return true;
  }
  bool read_interrupt(bool &out) override {
    ++source_reads;
    out = active;
    gpio_state.level = 0;
    return source_ok;
  }
  AccelReadResult read(AccelReading &out) override {
    ++reads;
    out = result == AccelReadResult::Ready ? next : AccelReading{};
    return result;
  }
};

struct Fixture {
  Clock clock;
  MotionSensor sensor;
  RtosQueueHandle events;
  AccelService service;

  Fixture()
      : events(clock.queue_create_impl(4, sizeof(Event))),
        service(&sensor, gpio_hal, events, {.pin_int = 3, .log_polls = false}) {
    gpio_state = {};
    RTOS::set_instance(&clock);
    clock.service = &service;
  }
  ~Fixture() {
    service.stop();
    RTOS::queue_delete(events);
    RTOS::set_instance(nullptr);
  }
  void init() { REQUIRE(AccelServiceTestAccess::initialize(service)); }
  bool poll(bool notified = false) { return AccelServiceTestAccess::poll(service, notified); }
};
} // namespace

TEST_CASE("Accel: service supplies the tested capture settings", "[Accel]") {
  Fixture f;
  f.init();
  const auto &config = f.sensor.last_config;
  CHECK(config.sample_rate_hz == 100);
  CHECK(config.range == AccelRange::G4);
  CHECK(config.high_pass);
  CHECK(config.interrupt.high_axes == AccelAxis::ALL);
  CHECK(config.interrupt.low_axes == 0);
  CHECK(config.interrupt.threshold_mg == 640);
  CHECK(config.interrupt.duration_ms == 20);
  CHECK(config.interrupt.latched);
  CHECK_FALSE(config.interrupt.active_low);
}

TEST_CASE("Accel: idle performs no sensor reads; ISR or latched level starts a burst", "[Accel]") {
  Fixture f;
  f.init();
  for (int i = 0; i < 20; ++i) {
    f.clock.now += 100;
    REQUIRE(f.poll());
  }
  CHECK(f.sensor.reads == 0);
  CHECK(f.sensor.source_reads == 0);
  SECTION("queued ISR notification") {
    REQUIRE(AccelServiceTestAccess::queue_interrupt(f.service));
    AccelServiceTestAccess::process_pending(f.service);
    CHECK(f.sensor.reads == 0); // Dispatch records the IRQ; capture owns polling.
    REQUIRE(f.poll());
  }
  SECTION("already asserted latch") {
    gpio_state.level = 1;
    REQUIRE(f.poll());
  }
  CHECK(f.sensor.reads == 1);
  CHECK(gpio_state.disables == 1);
  CHECK(AccelServiceTestAccess::burst(f.service));
  Event event{};
  CHECK_FALSE(RTOS::queue_receive(f.events, &event, 0));
}

TEST_CASE("Accel: inactive interrupt does not start XYZ polling", "[Accel]") {
  Fixture f;
  f.init();
  f.sensor.active = false;
  REQUIRE(f.poll(true));
  CHECK_FALSE(AccelServiceTestAccess::burst(f.service));
  CHECK(f.sensor.reads == 0);
  CHECK(gpio_state.enables == 2);
}

TEST_CASE("Accel: shake stops polling immediately and rearms after one second", "[Accel]") {
  Fixture f;
  uint32_t gesture_delay_ms = 0;
  SECTION("shake early in the burst") {}
  SECTION("shake at the burst deadline") { gesture_delay_ms = 4500; }
  SECTION("millisecond counter wraps") { f.clock.now = UINT32_MAX - 200; }
  f.init();
  const uint32_t started = f.clock.now;
  for (uint32_t elapsed = 0; elapsed <= gesture_delay_ms + 500; elapsed += 10) {
    f.clock.now = started + elapsed;
    f.sensor.next = sample(0);
    if (elapsed >= gesture_delay_ms) {
      const uint32_t peak = (elapsed - gesture_delay_ms) / 100;
      f.sensor.next = sample(peak % 2 ? -600 : 600);
    }
    REQUIRE(f.poll(elapsed == 0));
  }
  CHECK_FALSE(AccelServiceTestAccess::burst(f.service));
  CHECK(gpio_state.enables == 1); // GPIO stays masked throughout cooldown.
  Event event{};
  REQUIRE(RTOS::queue_receive(f.events, &event, 0));
  CHECK(event.type == EventType::ShakeDetected);
  CHECK(event.shake_detected_ms == started + gesture_delay_ms + 500);
  CHECK(f.clock.now == event.shake_detected_ms); // No blocking cooldown delay.
  const uint32_t detected = event.shake_detected_ms;
  CHECK_FALSE(RTOS::queue_receive(f.events, &event, 0));
  const auto reads = f.sensor.reads;
  const auto source_reads = f.sensor.source_reads;
  for (uint32_t elapsed : {0u, 100u, 500u, 999u}) {
    f.clock.now = detected + elapsed;
    gpio_state.level = 1; // Motion during cooldown must not start another burst.
    REQUIRE(f.poll(true));
    CHECK(f.sensor.reads == reads);
    CHECK(f.sensor.source_reads == source_reads);
    CHECK(gpio_state.enables == 1);
    CHECK_FALSE(AccelServiceTestAccess::burst(f.service));
  }

  f.clock.now = detected + 1000;
  REQUIRE(f.poll());
  CHECK(gpio_state.enables == 2);
  CHECK(f.sensor.source_reads == source_reads + 1); // Discard the cooldown's latched motion.
  CHECK(gpio_state.level == 0);
  CHECK(f.sensor.reads == reads);
  REQUIRE(f.poll());
  CHECK(f.sensor.reads == reads); // Wait for a new interrupt after rearming.

  const uint32_t next_started = f.clock.now;
  for (uint32_t elapsed = 0; elapsed <= 500; elapsed += 10) {
    f.clock.now = next_started + elapsed;
    f.sensor.next = sample((elapsed / 100) % 2 ? 600 : -600);
    REQUIRE(f.poll(elapsed == 0));
  }
  REQUIRE(RTOS::queue_receive(f.events, &event, 0));
  CHECK(event.type == EventType::ShakeDetected);
  CHECK(event.shake_detected_ms == next_started + 500);
  CHECK_FALSE(AccelServiceTestAccess::burst(f.service));
  CHECK_FALSE(RTOS::queue_receive(f.events, &event, 0));
}

TEST_CASE("Accel: burst without a shake still ends after five seconds", "[Accel]") {
  Fixture f;
  f.init();
  const uint32_t started = f.clock.now;
  REQUIRE(f.poll(true));
  f.clock.now = started + 4999;
  REQUIRE(f.poll());
  CHECK(AccelServiceTestAccess::burst(f.service));
  CHECK(gpio_state.enables == 1);
  f.clock.now = started + 5000;
  REQUIRE(f.poll());
  CHECK_FALSE(AccelServiceTestAccess::burst(f.service));
  CHECK(gpio_state.enables == 2);
  Event event{};
  CHECK_FALSE(RTOS::queue_receive(f.events, &event, 0));
}

TEST_CASE("Accel: Hardware Test remains available during shake cooldown", "[Accel]") {
  Fixture f;
  f.init();
  const uint32_t started = f.clock.now;
  for (uint32_t elapsed = 0; elapsed <= 500; elapsed += 10) {
    f.clock.now = started + elapsed;
    f.sensor.next = sample((elapsed / 100) % 2 ? -600 : 600);
    REQUIRE(f.poll(elapsed == 0));
  }
  REQUIRE_FALSE(AccelServiceTestAccess::burst(f.service));
  CHECK(gpio_state.enables == 1);
  REQUIRE(f.service.end_hardware_test()); // Already capturing: keep the cooldown intact.
  CHECK(gpio_state.enables == 1);

  AccelService::TestReading reading;
  REQUIRE(f.service.begin_hardware_test(reading));
  CHECK(reading.result == AccelReadResult::Ready);
  REQUIRE(f.service.read_hardware_test(reading));
  REQUIRE(f.service.end_hardware_test());
  CHECK(AccelServiceTestAccess::running(f.service));
  CHECK(gpio_state.enables == 1);

  const auto reads = f.sensor.reads;
  f.clock.now = started + 500 + 999;
  REQUIRE(f.poll(true));
  CHECK(f.sensor.reads == reads);
  CHECK(gpio_state.enables == 1);
  ++f.clock.now;
  REQUIRE(f.poll());
  CHECK(f.sensor.reads == reads);
  CHECK(gpio_state.enables == 2);
}

TEST_CASE("Accel: Hardware Test keeps the worker and switches filtering at +/-4 g", "[Accel]") {
  Fixture f;
  f.init();
  const auto queue = AccelServiceTestAccess::work_queue(f.service);
  const auto handler = gpio_state.handler;
  REQUIRE(f.poll(true));
  AccelService::TestReading reading;
  REQUIRE(f.service.begin_hardware_test(reading));
  CHECK(reading.result == AccelReadResult::Ready);
  CHECK(reading.who_am_i == 0x33);
  CHECK(f.sensor.last_config.range == AccelRange::G4);
  CHECK_FALSE(f.sensor.last_config.high_pass);
  CHECK(f.sensor.last_config.interrupt.high_axes == 0);
  CHECK(AccelServiceTestAccess::running(f.service));
  CHECK(gpio_state.handler == handler);
  CHECK(gpio_state.removes == 0);
  const auto reads = f.sensor.reads;
  for (int tick = 0; tick < 60; ++tick) {
    f.sensor.next = sample(tick % 2 ? -600 : 600);
    REQUIRE(f.poll(true));
    f.clock.now += 100;
  }
  CHECK(f.sensor.reads == reads); // Raw samples only on a Hardware Test request.
  f.sensor.next = sample(0, 0, 1000);
  REQUIRE(f.service.read_hardware_test(reading));
  CHECK(reading.sample.z_mg == 1000);
  REQUIRE(f.service.end_hardware_test());
  CHECK(f.sensor.last_config.range == AccelRange::G4);
  CHECK(f.sensor.last_config.high_pass);
  CHECK(f.sensor.last_config.interrupt.high_axes == AccelAxis::ALL);
  CHECK(AccelServiceTestAccess::running(f.service));
  CHECK(AccelServiceTestAccess::work_queue(f.service) == queue);
  CHECK(gpio_state.removes == 0);
  for (const auto *operation : f.sensor.operations) {
    CHECK(std::string(operation) == "configure");
  }
  Event event{};
  CHECK_FALSE(RTOS::queue_receive(f.events, &event, 0));
  REQUIRE(f.poll(true)); // Interrupt capture works again on the same worker.
  CHECK(AccelServiceTestAccess::burst(f.service));
}

TEST_CASE("Accel: failed Hardware Test configuration can recover on the same worker", "[Accel]") {
  Fixture f;
  f.init();
  AccelService::TestReading reading;
  SECTION("configuration failure") {
    SECTION("sensor configuration") { f.sensor.configure_ok = false; }
    SECTION("GPIO masking") { gpio_state.disable_ok = false; }
    CHECK_FALSE(f.service.begin_hardware_test(reading));
    CHECK(reading.result == AccelReadResult::Error);
    CHECK_FALSE(f.service.read_hardware_test(reading));
    CHECK(f.sensor.reads == 0);
    CHECK(AccelServiceTestAccess::running(f.service));
    REQUIRE(f.poll(true));
    CHECK(f.sensor.reads == 0);
    CHECK(f.sensor.source_reads == 0);
    f.sensor.configure_ok = true;
    gpio_state.disable_ok = true;
    REQUIRE(f.service.end_hardware_test());
    CHECK(AccelServiceTestAccess::running(f.service));
  }
  SECTION("sample error is reported") {
    REQUIRE(f.service.begin_hardware_test(reading));
    f.sensor.result = AccelReadResult::Error;
    REQUIRE(f.service.read_hardware_test(reading));
    CHECK(reading.result == AccelReadResult::Error);
    CHECK_FALSE(reading.sample.is_valid());
    CHECK(AccelServiceTestAccess::running(f.service));
    f.sensor.result = AccelReadResult::Ready;
    REQUIRE(f.service.read_hardware_test(reading));
    CHECK(reading.result == AccelReadResult::Ready);
  }
}

TEST_CASE("Accel: failed capture restoration leaves the worker available for retry", "[Accel]") {
  Fixture f;
  f.init();
  const auto queue = AccelServiceTestAccess::work_queue(f.service);
  const auto handler = gpio_state.handler;
  AccelService::TestReading reading;
  REQUIRE(f.service.begin_hardware_test(reading));
  SECTION("sensor configuration fails") { f.sensor.configure_ok = false; }
  SECTION("GPIO masking fails") { gpio_state.disable_ok = false; }
  SECTION("GPIO rearming fails") { gpio_state.enable_ok = false; }
  CHECK_FALSE(f.service.end_hardware_test());
  CHECK(AccelServiceTestAccess::running(f.service));
  CHECK_FALSE(AccelServiceTestAccess::burst(f.service));
  const auto reads = f.sensor.reads;
  const auto source_reads = f.sensor.source_reads;
  gpio_state.level = 1;
  for (int tick = 0; tick < 20; ++tick) {
    REQUIRE(f.poll(tick == 0));
    f.clock.now += 100;
  }
  CHECK_FALSE(f.service.read_hardware_test(reading));
  CHECK(reading.result == AccelReadResult::Error);
  CHECK_FALSE(reading.sample.is_valid());
  CHECK(f.sensor.reads == reads);
  CHECK(f.sensor.source_reads == source_reads);
  CHECK_FALSE(f.service.end_hardware_test()); // Repeated failure also keeps the worker alive.
  CHECK(AccelServiceTestAccess::running(f.service));

  f.sensor.configure_ok = true;
  gpio_state.disable_ok = true;
  gpio_state.enable_ok = true;
  REQUIRE(f.service.end_hardware_test());
  CHECK(f.sensor.last_config.high_pass);
  CHECK(AccelServiceTestAccess::work_queue(f.service) == queue);
  CHECK(gpio_state.handler == handler);
  CHECK(gpio_state.removes == 0);
  gpio_state.level = 0;
  REQUIRE(f.poll());
  CHECK(f.sensor.reads == reads); // No IRQ from the failed restore is carried forward.
  REQUIRE(f.poll(true));
  CHECK(AccelServiceTestAccess::burst(f.service));
  CHECK(f.sensor.reads == reads + 1);
}

TEST_CASE("Accel: Hardware Test can be reopened after capture restoration fails", "[Accel]") {
  Fixture f;
  f.init();
  AccelService::TestReading reading;
  REQUIRE(f.service.begin_hardware_test(reading));
  f.sensor.configure_ok = false;
  CHECK_FALSE(f.service.end_hardware_test());
  f.sensor.configure_ok = true;
  REQUIRE(f.service.begin_hardware_test(reading));
  CHECK(reading.result == AccelReadResult::Ready);
  CHECK_FALSE(f.sensor.last_config.high_pass);
  REQUIRE(f.service.end_hardware_test());
  CHECK(AccelServiceTestAccess::running(f.service));
}

TEST_CASE("Accel: ending Hardware Test during capture preserves the active burst", "[Accel]") {
  Fixture f;
  f.init();
  REQUIRE(f.poll(true));
  const auto configurations = f.sensor.operations.size();
  REQUIRE(f.service.end_hardware_test());
  CHECK(AccelServiceTestAccess::burst(f.service));
  CHECK(f.sensor.operations.size() == configurations);
  f.clock.now += 10;
  REQUIRE(f.poll());
  CHECK(f.sensor.reads == 2);
}

TEST_CASE("Accel: reply delivery failure stops the worker and releases the caller", "[Accel]") {
  Fixture f;
  f.init();
  SECTION("successful test configuration") {}
  SECTION("failed test configuration") { f.sensor.configure_ok = false; }
  f.clock.fail_reply_delivery = true;
  AccelService::TestReading reading;
  CHECK_FALSE(f.service.begin_hardware_test(reading));
  CHECK_FALSE(reading.sample.is_valid());
  CHECK_FALSE(AccelServiceTestAccess::running(f.service));
  CHECK_FALSE(f.service.read_hardware_test(reading));
}

TEST_CASE("Accel: Stop dispatch prevents further test commands and releases the caller",
          "[Accel]") {
  Fixture f;
  f.init();
  const auto configurations = f.sensor.operations.size();
  REQUIRE(AccelServiceTestAccess::queue_stop(f.service));
  AccelService::TestReading reading;
  CHECK_FALSE(f.service.begin_hardware_test(reading));
  CHECK_FALSE(AccelServiceTestAccess::running(f.service));
  CHECK(f.sensor.operations.size() == configurations);
  CHECK(f.sensor.reads == 0);
}

TEST_CASE("Accel: capture errors are reported without producing a shake", "[Accel]") {
  Fixture f;
  f.init();
  SECTION("interrupt read failure") {
    f.sensor.source_ok = false;
    CHECK_FALSE(f.poll(true));
  }
  SECTION("I2C sample failure") {
    f.sensor.result = AccelReadResult::Error;
    CHECK_FALSE(f.poll(true));
  }
  SECTION("sensor stalls") {
    f.sensor.result = AccelReadResult::NotReady;
    REQUIRE(f.poll(true));
    f.clock.now += 250;
    CHECK_FALSE(f.poll());
  }
  SECTION("rearm fails") {
    REQUIRE(f.poll(true));
    gpio_state.enable_ok = false;
    f.clock.now += 5000;
    CHECK_FALSE(f.poll());
  }
  f.service.stop();
  CHECK(gpio_state.handler == nullptr);
  Event event{};
  CHECK_FALSE(RTOS::queue_receive(f.events, &event, 0));
}

TEST_CASE("Accel: absent sensor, setup failure, are nonfatal", "[Accel]") {
  Fixture f;
  SECTION("absent sensor") {
    AccelService absent(nullptr, gpio_hal, f.events, {.pin_int = 3});
    CHECK_FALSE(absent.start());
    AccelService::TestReading reading;
    CHECK_FALSE(absent.begin_hardware_test(reading));
  }
  SECTION("configuration failure powers motion down") {
    f.sensor.configure_ok = false;
    CHECK_FALSE(AccelServiceTestAccess::initialize(f.service));
    f.service.stop();
    CHECK(std::string(f.sensor.operations.back()) == "stop");
  }
  SECTION("host task allocation failure releases resources") {
    CHECK_FALSE(f.service.start());
    CHECK(gpio_state.handler == nullptr);
    CHECK(f.sensor.operations.empty());
  }
}
