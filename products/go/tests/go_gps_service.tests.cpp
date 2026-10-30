/**
 * AirGradient Go — GPS service tests
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <deque>
#include <vector>

#include "gps/gps_service.h"

// The host RTOS intentionally does not launch tasks. Exercise the worker's
// acquisition/wait path synchronously, with cancellation at controlled waits.
class GpsServiceTestAccess {
public:
  static bool activate(GpsService &service) {
    service._running = true;
    return service.activate_receiver();
  }
  static void cancel(GpsService &service) { service._running = false; }
  static bool running(const GpsService &service) { return service._running; }
  static bool wait_for_wake(GpsService &service) {
    service._running = true;
    return service.wait_for_wake();
  }
  static void run(GpsService &service) {
    service._running = true;
    service.run();
  }
  static void run_sleeping(GpsService &service, uint32_t interval_ms) {
    service._sleep_interval_ms = interval_ms;
    run(service);
  }
  static void seed_fix(GpsService &service) {
    GpsData data;
    data.position.latitude = 13.0;
    data.position.longitude = 100.0;
    service.update_latest_fix(data);
  }
};

namespace {

class ServiceClock : public RTOS {
public:
  uint64_t retained_ms = 1000;
  uint64_t boot_ms = 0;
  std::function<void()> on_delay;
  std::function<void()> on_wait;
  std::vector<uint32_t> waits;

  bool task_notify_wait_impl(uint32_t *, uint32_t timeout_ms) override {
    waits.push_back(timeout_ms);
    if (on_wait) {
      on_wait();
      return true;
    }
    delay_ms_impl(timeout_ms);
    return false;
  }

  void delay_ms_impl(uint32_t ms) override {
    retained_ms += ms;
    boot_ms += ms;
    if (on_delay) {
      on_delay();
    }
  }
  uint64_t get_time_ms_impl() override { return boot_ms; }
  uint64_t get_retained_time_ms_impl() override { return retained_ms; }
};

class ServiceSerial : public AirgradientSerial {
public:
  struct Write {
    int baud;
    uint64_t time_ms;
    std::vector<uint8_t> bytes;
  };
  std::vector<int> opened_bauds;
  std::vector<Write> writes;
  bool open_ok = true;
  bool tx_ok = true;
  bool short_write = false;
  bool open = false;
  int baud = 0;
  uint32_t tx_delay_ms = 0;
  int receiver_baud = GpsDriver::MODULE_DEFAULT_BAUD;
  bool respond = true;
  uint64_t receiver_wake_ms = 0;
  std::deque<uint8_t> rx;

  void reply(uint8_t group, uint8_t sub, std::vector<uint8_t> payload) {
    std::vector<uint8_t> packet{0xF1, 0xD9, group, sub, static_cast<uint8_t>(payload.size()), 0};
    packet.insert(packet.end(), payload.begin(), payload.end());
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < packet.size(); ++i) {
      a += packet[i];
      b += a;
    }
    packet.push_back(a);
    packet.push_back(b);
    rx.insert(rx.end(), packet.begin(), packet.end());
  }

  bool begin(int rate) override {
    opened_bauds.push_back(rate);
    baud = rate;
    open = open_ok;
    return open_ok;
  }
  void end() override { open = false; }
  int available() override { return static_cast<int>(rx.size()); }
  int read() override {
    if (rx.empty())
      return -1;
    const int byte = rx.front();
    rx.pop_front();
    return byte;
  }
  void print(const char *) override {}
  int write(const uint8_t *data, int len) override {
    writes.push_back({baud, RTOS::get_retained_time_ms(), {data, data + len}});
    if (receiver_wake_ms != 0 && RTOS::get_retained_time_ms() >= receiver_wake_ms) {
      receiver_wake_ms = 0;
      receiver_baud = GpsDriver::MODULE_DEFAULT_BAUD;
    }
    if (!short_write && respond && baud == receiver_baud && receiver_wake_ms == 0 && len >= 8) {
      if (data[2] == 0x0A && data[3] == 0x04) {
        reply(0x0A, 0x04, std::vector<uint8_t>(32, 0));
      } else if (data[2] == 0x06 && data[3] == 0x00 && tx_ok) {
        receiver_baud = GpsDriver::MODULE_ACTIVE_BAUD;
      } else if (data[2] == 0x06 && data[3] == 0x41 && tx_ok) {
        const uint32_t duration = uint32_t{data[6]} | (uint32_t{data[7]} << 8) |
                                  (uint32_t{data[8]} << 16) | (uint32_t{data[9]} << 24);
        receiver_wake_ms = RTOS::get_retained_time_ms() + duration;
      } else if (data[2] == 0x06) {
        reply(0x05, 0x01, {data[2], data[3]});
      }
    }
    return short_write ? len - 1 : len;
  }
  bool wait_tx_done(uint32_t) override {
    RTOS::delay_ms(tx_delay_ms);
    return tx_ok;
  }
};

struct Fixture {
  ServiceClock clock;
  ServiceSerial serial;
  GpsDriver driver{serial};
  GpsService service{driver, nullptr, GpsService::Config{}};

  Fixture() { RTOS::set_instance(&clock); }
  ~Fixture() {
    service.stop();
    RTOS::set_instance(nullptr);
  }
};

bool is_command(const ServiceSerial::Write &write, uint8_t sub) {
  return write.bytes.size() >= 8 && write.bytes[2] == 0x06 && write.bytes[3] == sub;
}

} // namespace

TEST_CASE_METHOD(Fixture, "GPS service sleep uses default baud without acquisition setup",
                 "[gps_service]") {
  GpsServiceTestAccess::seed_fix(service);
  serial.tx_delay_ms = 20;
  REQUIRE(service.request_sleep(60000));
  CHECK(serial.opened_bauds == std::vector<int>{GpsDriver::MODULE_DEFAULT_BAUD});
  REQUIRE(serial.writes.size() == 2); // MON-VER response check, then sleep
  CHECK(is_command(serial.writes.back(), 0x41));
  CHECK(service.sleep_remaining_ms() == 60000);
  CHECK_FALSE(is_position_valid(service.get_latest_fix().position));
  CHECK_FALSE(serial.open);
}

TEST_CASE_METHOD(Fixture, "GPS service tries the other baud only when expected baud is silent",
                 "[gps_service]") {
  int expected = GpsDriver::MODULE_DEFAULT_BAUD;
  int actual = GpsDriver::MODULE_ACTIVE_BAUD;
  SECTION("expected default, actual active") {}
  SECTION("expected active, actual default") {
    expected = GpsDriver::MODULE_ACTIVE_BAUD;
    actual = GpsDriver::MODULE_DEFAULT_BAUD;
  }
  serial.receiver_baud = actual;
  GpsService gps(driver, nullptr, {.expected_baud = expected});
  REQUIRE(gps.request_sleep(60000));
  CHECK(serial.opened_bauds == std::vector<int>{expected, actual});
  CHECK(serial.writes.back().baud == actual);
  CHECK(is_command(serial.writes.back(), 0x41));
  for (const auto &write : serial.writes)
    CHECK_FALSE(is_command(write, 0x00));
}

TEST_CASE_METHOD(Fixture, "GPS service sleeps at active baud then re-sleeps at default baud",
                 "[gps_service]") {
  REQUIRE(GpsServiceTestAccess::activate(service));
  service.stop();
  serial.opened_bauds.clear();
  serial.writes.clear();
  REQUIRE(service.request_sleep(60000));
  CHECK(serial.writes.back().baud == GpsDriver::MODULE_ACTIVE_BAUD);
  clock.retained_ms += service.sleep_remaining_ms();
  REQUIRE(service.request_sleep(10000));
  CHECK(serial.writes.back().baud == GpsDriver::MODULE_DEFAULT_BAUD);
  CHECK(service.sleep_remaining_ms() == 10000);
}

TEST_CASE_METHOD(Fixture, "GPS service zero or replacement sleep preserves the original deadline",
                 "[gps_service]") {
  CHECK_FALSE(service.request_sleep(0));
  CHECK(serial.writes.empty());
  REQUIRE(service.request_sleep(60000));
  const auto count = serial.writes.size();
  clock.retained_ms += 1000;
  CHECK_FALSE(service.request_sleep(120000));
  CHECK(serial.writes.size() == count);
  CHECK(service.sleep_remaining_ms() == 59000);
}

TEST_CASE_METHOD(Fixture, "GPS service activation waits then switches to 115200", "[gps_service]") {
  REQUIRE(service.request_sleep(250));
  const auto deadline = service.sleep_deadline_ms();
  serial.opened_bauds.clear();
  serial.writes.clear();
  REQUIRE(GpsServiceTestAccess::activate(service));
  CHECK(clock.waits == std::vector<uint32_t>{250});
  REQUIRE(serial.opened_bauds ==
          std::vector<int>{GpsDriver::MODULE_DEFAULT_BAUD, GpsDriver::MODULE_ACTIVE_BAUD});
  REQUIRE_FALSE(serial.writes.empty());
  CHECK(serial.writes.front().time_ms >= deadline);
  CHECK(is_command(serial.writes[1], 0x00));
  CHECK(is_command(serial.writes.back(), 0x40));
  CHECK(service.sleep_deadline_ms() == 0);
}

TEST_CASE_METHOD(Fixture, "GPS service activation wait can be cancelled without UART traffic",
                 "[gps_service]") {
  REQUIRE(service.request_sleep(60000));
  serial.opened_bauds.clear();
  serial.writes.clear();
  clock.on_wait = [&] { service.stop(); };
  CHECK_FALSE(GpsServiceTestAccess::activate(service));
  CHECK(clock.waits == std::vector<uint32_t>{60000});
  CHECK(serial.opened_bauds.empty());
  CHECK(serial.writes.empty());
  CHECK(service.sleep_remaining_ms() == 60000);
}

TEST_CASE_METHOD(Fixture, "GPS service failed sleep does not create a deadline", "[gps_service]") {
  SECTION("TX failure") { serial.tx_ok = false; }
  SECTION("Partial write") { serial.short_write = true; }
  SECTION("UART unavailable") { serial.open_ok = false; }
  SECTION("Neither baud responds") { serial.respond = false; }
  CHECK_FALSE(service.request_sleep(60000));
  CHECK(service.sleep_deadline_ms() == 0);
  CHECK_FALSE(serial.open);
  serial.tx_ok = true;
  serial.short_write = false;
  serial.open_ok = true;
  serial.respond = true;
  REQUIRE(service.request_sleep(5000));
  CHECK(service.sleep_remaining_ms() == 5000);
}

TEST_CASE_METHOD(Fixture, "GPS service failed activation exits without GNSS start",
                 "[gps_service]") {
  SECTION("UART unavailable") { serial.open_ok = false; }
  SECTION("Baud switch failed") { serial.tx_ok = false; }
  SECTION("Neither baud responds") { serial.respond = false; }
  GpsServiceTestAccess::run(service);
  CHECK_FALSE(GpsServiceTestAccess::running(service));
  CHECK_FALSE(serial.open);
  for (const auto &write : serial.writes)
    CHECK_FALSE(is_command(write, 0x40));
}

TEST_CASE_METHOD(Fixture, "GPS service restores only the sleep deadline after MCU deep sleep",
                 "[gps_service]") {
  REQUIRE(service.request_sleep(60000));
  const auto deadline = service.sleep_deadline_ms();
  clock.retained_ms += 20000;
  clock.boot_ms = 0;
  GpsService resumed(driver, nullptr, {.wake_deadline_ms = deadline});
  CHECK(resumed.sleep_remaining_ms() == 40000);
  CHECK_FALSE(resumed.request_sleep(60000));
  clock.retained_ms += 40000;
  REQUIRE(resumed.request_sleep(10000));
  CHECK(serial.writes.back().baud == GpsDriver::MODULE_DEFAULT_BAUD);
}

TEST_CASE_METHOD(Fixture, "GPS wake probe re-sleeps a responding receiver without acquisition",
                 "[gps_service]") {
  REQUIRE(service.request_sleep(UINT32_MAX));
  clock.retained_ms += 300000;
  serial.opened_bauds.clear();
  serial.writes.clear();
  bool awake = true;
  SECTION("receiver woke at default baud") { serial.receiver_wake_ms = 0; }
  SECTION("receiver woke at active baud") {
    serial.receiver_wake_ms = 0;
    serial.receiver_baud = GpsDriver::MODULE_ACTIVE_BAUD;
  }
  SECTION("receiver is still asleep") { awake = false; }

  // Timer-wake probing intentionally does not restore the retained deadline.
  GpsService probe(driver, nullptr, {});
  CHECK(probe.request_sleep(UINT32_MAX) == awake);
  const bool both_bauds = !awake || serial.writes.back().baud == GpsDriver::MODULE_ACTIVE_BAUD;
  CHECK(serial.opened_bauds.size() == (both_bauds ? 2 : 1));
  CHECK(serial.opened_bauds.front() == GpsDriver::MODULE_DEFAULT_BAUD);
  if (both_bauds) {
    CHECK(serial.opened_bauds.back() == GpsDriver::MODULE_ACTIVE_BAUD);
  }
  int sleep_commands = 0;
  for (const auto &write : serial.writes) {
    if (is_command(write, 0x41)) {
      ++sleep_commands;
      CHECK(write.baud == serial.receiver_baud);
    }
    CHECK_FALSE(is_command(write, 0x40)); // No GNSS start.
    CHECK_FALSE(is_command(write, 0x00)); // No baud switch.
  }
  CHECK(sleep_commands == (awake ? 1 : 0));
  CHECK(probe.sleep_remaining_ms() == (awake ? UINT32_MAX : 0));
  CHECK_FALSE(serial.open);
}

TEST_CASE_METHOD(Fixture, "GPS service handles maximum sleep without truncating retained time",
                 "[gps_service]") {
  clock.retained_ms = uint64_t{UINT32_MAX} + 1234;
  REQUIRE(service.request_sleep(UINT32_MAX));
  CHECK(service.sleep_remaining_ms() == UINT32_MAX);
  clock.retained_ms += 1;
  CHECK(service.sleep_remaining_ms() == UINT32_MAX - 1);
}

TEST_CASE_METHOD(Fixture, "GPS service maximum sleep uses notification waits of at most one hour",
                 "[gps_service]") {
  clock.retained_ms = uint64_t{UINT32_MAX} + 1234;
  REQUIRE(service.request_sleep(UINT32_MAX));
  const auto deadline = service.sleep_deadline_ms();
  REQUIRE(GpsServiceTestAccess::wait_for_wake(service));
  REQUIRE_FALSE(clock.waits.empty());
  CHECK(clock.waits.front() == 3600000);
  for (const auto wait_ms : clock.waits) {
    CHECK(wait_ms <= 3600000);
  }
  CHECK(clock.retained_ms == deadline);
}

TEST_CASE_METHOD(Fixture, "GPS service short remaining sleep waits at least one tick",
                 "[gps_service]") {
  REQUIRE(service.request_sleep(1));
  REQUIRE(GpsServiceTestAccess::wait_for_wake(service));
  CHECK(clock.waits == std::vector<uint32_t>{10});
  CHECK(service.sleep_remaining_ms() == 0);
}

TEST_CASE_METHOD(Fixture, "GPS service rechecks the deadline after an early notification",
                 "[gps_service]") {
  REQUIRE(service.request_sleep(60000));
  clock.on_wait = [&] { clock.retained_ms += clock.waits.size() == 1 ? 1000 : clock.waits.back(); };
  REQUIRE(GpsServiceTestAccess::wait_for_wake(service));
  CHECK(clock.waits == std::vector<uint32_t>{60000, 59000});
  CHECK(service.sleep_remaining_ms() == 0);
}

TEST_CASE_METHOD(Fixture, "GPS service independently repeats sleep after each wake",
                 "[gps_service]") {
  constexpr uint32_t interval_ms = 60000;
  clock.on_delay = [&] {
    size_t sleeps = 0;
    for (const auto &write : serial.writes) {
      if (is_command(write, 0x41))
        ++sleeps;
    }
    if (sleeps == 2 || clock.boot_ms > interval_ms * 3) {
      GpsServiceTestAccess::cancel(service);
    }
  };
  GpsServiceTestAccess::run_sleeping(service, interval_ms);
  std::vector<ServiceSerial::Write> sleeps;
  for (const auto &write : serial.writes) {
    if (is_command(write, 0x41))
      sleeps.push_back(write);
    CHECK_FALSE(is_command(write, 0x00));
    CHECK_FALSE(is_command(write, 0x40));
  }
  REQUIRE(sleeps.size() == 2);
  CHECK(clock.waits == std::vector<uint32_t>{interval_ms});
  CHECK(sleeps[1].time_ms >= sleeps[0].time_ms + interval_ms);
  CHECK(service.sleep_remaining_ms() == interval_ms);
  CHECK_FALSE(serial.open);
}

TEST_CASE_METHOD(Fixture,
                 "GPS service sleep maintenance preserves an existing timer and stops promptly",
                 "[gps_service]") {
  REQUIRE(service.request_sleep(60000));
  serial.writes.clear();
  serial.opened_bauds.clear();
  clock.on_wait = [&] { service.stop(); };
  GpsServiceTestAccess::run_sleeping(service, 120000);
  CHECK(clock.waits == std::vector<uint32_t>{60000});
  CHECK(serial.writes.empty());
  CHECK(serial.opened_bauds.empty());
  CHECK(service.sleep_remaining_ms() == 60000);
  clock.on_wait = {};
  REQUIRE(GpsServiceTestAccess::activate(service));
  CHECK(serial.receiver_baud == GpsDriver::MODULE_ACTIVE_BAUD);
  CHECK(service.sleep_deadline_ms() == 0);
}

TEST_CASE_METHOD(Fixture, "GPS service retries failed sleep without an orchestrator timer",
                 "[gps_service]") {
  serial.tx_ok = false;
  clock.on_delay = [&] {
    size_t sleeps = 0;
    uint64_t first_ms = 0;
    for (const auto &write : serial.writes) {
      if (is_command(write, 0x41)) {
        if (sleeps == 0)
          first_ms = write.time_ms;
        ++sleeps;
      }
    }
    if (sleeps == 1 && clock.retained_ms >= first_ms + 1000)
      serial.tx_ok = true;
    if (sleeps == 2 || clock.boot_ms > 5000)
      GpsServiceTestAccess::cancel(service);
  };
  GpsServiceTestAccess::run_sleeping(service, 60000);
  std::vector<ServiceSerial::Write> sleeps;
  for (const auto &write : serial.writes) {
    if (is_command(write, 0x41))
      sleeps.push_back(write);
  }
  REQUIRE(sleeps.size() == 2);
  CHECK(clock.waits == std::vector<uint32_t>{1000});
  CHECK(sleeps[1].time_ms >= sleeps[0].time_ms + 1000);
  CHECK(service.sleep_remaining_ms() == 60000);
}

TEST_CASE_METHOD(Fixture, "GPS one-shot sleep stops maintenance without scheduling another worker",
                 "[gps_service]") {
  clock.on_delay = [&] { GpsServiceTestAccess::cancel(service); };
  GpsServiceTestAccess::run_sleeping(service, 60000);
  clock.on_delay = {};
  clock.retained_ms += service.sleep_remaining_ms();
  serial.writes.clear();
  REQUIRE(service.request_sleep(120000));
  CHECK_FALSE(GpsServiceTestAccess::running(service));
  CHECK(service.sleep_remaining_ms() == 120000);
  size_t sleeps = 0;
  for (const auto &write : serial.writes) {
    if (is_command(write, 0x41))
      ++sleeps;
  }
  CHECK(sleeps == 1);
}

TEST_CASE_METHOD(Fixture, "GPS service stop interrupts a failed-sleep retry", "[gps_service]") {
  serial.tx_ok = false;
  uint64_t stopped_at_ms = 0;
  clock.on_wait = [&] {
    stopped_at_ms = clock.retained_ms;
    service.stop();
  };
  GpsServiceTestAccess::run_sleeping(service, 60000);
  CHECK(clock.waits == std::vector<uint32_t>{1000});
  CHECK(clock.retained_ms == stopped_at_ms);
  CHECK(service.sleep_deadline_ms() == 0);
  REQUIRE(serial.writes.size() == 2); // Response check and one failed sleep.
  CHECK(is_command(serial.writes.back(), 0x41));
}
