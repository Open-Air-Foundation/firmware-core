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
#include "go_events.h"

// The host RTOS intentionally does not launch tasks. Exercise the worker's
// acquisition/wait path synchronously, with cancellation at controlled waits.
class GpsServiceTestAccess {
public:
  static bool activate(GpsService &service) {
    attach_worker(service);
    service._sleep_interval_ms = 0;
    return service.activate_receiver();
  }
  static void cancel(GpsService &service) { service._running = false; }
  static bool running(const GpsService &service) { return service._running; }
  static bool wait_for_wake(GpsService &service) {
    attach_worker(service);
    return service.wait_for_wake();
  }
  static void run(GpsService &service) {
    attach_worker(service);
    service.run();
  }
  static void run_sleeping(GpsService &service, uint32_t interval_ms) {
    service._sleep_interval_ms = interval_ms;
    run(service);
  }
  // The host task API cannot launch tasks. Seed an existing handle to verify
  // public job requests reuse it, then execute the actual worker synchronously.
  static RtosTaskHandle attach_worker(GpsService &service) {
    if (service._task_handle == nullptr) {
      // A newly created task has no notification from its predecessor.
      uint32_t ignored;
      (void)RTOS::task_notify_wait(&ignored, 0);
    }
    service._running = true;
    service._task_handle = reinterpret_cast<RtosTaskHandle>(&service);
    service._done_sem.create();
    return service._task_handle;
  }
  static RtosTaskHandle task_handle(const GpsService &service) { return service._task_handle; }
  static uint32_t active_interval(const GpsService &service) { return service._sleep_interval_ms; }
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
  std::vector<Event> events;
  bool notification_pending = false;
  uint32_t notification_value = 0;

  void task_notify_send_impl(RtosTaskHandle, uint32_t value) override {
    notification_value = value;
    notification_pending = true;
  }

  bool queue_send_impl(RtosQueueHandle, const void *item, uint32_t) override {
    events.push_back(*static_cast<const Event *>(item));
    return true;
  }

  bool task_notify_wait_impl(uint32_t *value, uint32_t timeout_ms) override {
    if (!notification_pending && timeout_ms != 0) {
      waits.push_back(timeout_ms);
      if (on_wait) {
        on_wait();
      } else {
        delay_ms_impl(timeout_ms);
      }
    }
    if (notification_pending) {
      *value = notification_value;
      notification_pending = false;
      return true;
    }
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
  bool nmea_sent = false;
  std::function<void()> on_read;

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
    nmea_sent = false;
    rx.clear();
    open = open_ok;
    return open_ok;
  }
  void end() override { open = false; }
  int available() override {
    if (receiver_wake_ms != 0 && RTOS::get_retained_time_ms() >= receiver_wake_ms) {
      receiver_wake_ms = 0;
      receiver_baud = GpsDriver::MODULE_DEFAULT_BAUD;
    }
    if (open && respond && !nmea_sent && baud == receiver_baud && receiver_wake_ms == 0) {
      const std::string sentence =
          "$GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*53\r\n";
      rx.insert(rx.end(), sentence.begin(), sentence.end());
      nmea_sent = true;
    }
    return static_cast<int>(rx.size());
  }
  int read() override {
    if (rx.empty())
      return -1;
    const int byte = rx.front();
    rx.pop_front();
    if (on_read) {
      on_read();
    }
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

size_t command_count(const ServiceSerial &serial, uint8_t sub) {
  size_t count = 0;
  for (const auto &write : serial.writes) {
    if (is_command(write, sub)) {
      ++count;
    }
  }
  return count;
}

} // namespace

TEST_CASE_METHOD(Fixture, "GPS connection probe waits three seconds at each baud",
                 "[gps_service][probe]") {
  serial.respond = false;
  const uint64_t start_ms = clock.retained_ms;

  SECTION("silent receiver times out at both bauds") {}
  SECTION("continuous unrelated data does not end the probe early or extend it") {
    clock.on_delay = [&] { serial.rx.insert(serial.rx.end(), 128, '$'); };
  }

  CHECK(driver.connect(GpsDriver::MODULE_DEFAULT_BAUD) == 0);
  CHECK(clock.retained_ms - start_ms == 6000);
  CHECK(serial.opened_bauds == std::vector<int>{9600, 115200});
  CHECK_FALSE(serial.open);
}

TEST_CASE_METHOD(Fixture, "GPS connection probe detects valid NMEA without sending a command",
                 "[gps_service][probe]") {
  serial.respond = false;
  const uint64_t start_ms = clock.retained_ms;
  int responding_baud = GpsDriver::MODULE_DEFAULT_BAUD;
  uint64_t expected_ms = 2500;
  SECTION("first baud") {}
  SECTION("alternate baud") {
    responding_baud = GpsDriver::MODULE_ACTIVE_BAUD;
    expected_ms = 5500;
  }
  clock.on_delay = [&] {
    if (serial.baud == responding_baud && clock.retained_ms - start_ms == expected_ms) {
      const std::string nmea =
          "$GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*53\r\n";
      serial.rx.insert(serial.rx.end(), nmea.begin(), nmea.end());
    }
  };
  CHECK(driver.connect(GpsDriver::MODULE_DEFAULT_BAUD) == responding_baud);
  CHECK(clock.retained_ms - start_ms == expected_ms);
  CHECK(serial.writes.empty());
}

TEST_CASE_METHOD(Fixture, "GPS connection validates fragmented NMEA without parsing fields",
                 "[gps_service][probe]") {
  serial.respond = false;
  const std::string valid =
      "$GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*53\r\n";
  clock.on_delay = [&] {
    if (clock.boot_ms == 20) {
      const std::string invalid = "$GPGGA,invalid*00\r\n$GPGGA,invalid*ZZ\r\n$\n";
      serial.rx.insert(serial.rx.end(), invalid.begin(), invalid.end());
      serial.rx.insert(serial.rx.end(), valid.begin(), valid.begin() + 10);
    } else if (clock.boot_ms == 40) {
      serial.rx.insert(serial.rx.end(), valid.begin() + 10, valid.end());
    }
  };
  CHECK(driver.connect(GpsDriver::MODULE_DEFAULT_BAUD) == GpsDriver::MODULE_DEFAULT_BAUD);
  CHECK(clock.boot_ms == 40);
  CHECK(serial.writes.empty());
}

TEST_CASE_METHOD(Fixture, "GPS service sleep uses default baud without acquisition setup",
                 "[gps_service]") {
  GpsServiceTestAccess::seed_fix(service);
  serial.tx_delay_ms = 20;
  REQUIRE(service.request_sleep(60000));
  CHECK(serial.opened_bauds == std::vector<int>{GpsDriver::MODULE_DEFAULT_BAUD});
  REQUIRE(serial.writes.size() == 1); // Sleep command only
  CHECK(is_command(serial.writes.back(), 0x41));
  CHECK(service.sleep_remaining_ms() == 60000);
  CHECK_FALSE(is_position_valid(service.get_latest_fix().position));
  CHECK_FALSE(serial.open);
}

TEST_CASE_METHOD(Fixture, "Fast-path GPS probe uses NMEA at both bauds",
                 "[gps_service][fast_path]") {
  GpsService gps(driver, nullptr, {.fast_path_sleep_probe = true});
  const uint64_t start_ms = clock.retained_ms;
  bool succeeds = true;
  uint64_t elapsed_ms = 20;
  std::vector<int> expected_bauds{9600};

  SECTION("silent receiver") {
    serial.respond = false;
    succeeds = false;
    elapsed_ms = 6000;
    expected_bauds.push_back(115200);
  }
  SECTION("alternate baud") {
    serial.receiver_baud = GpsDriver::MODULE_ACTIVE_BAUD;
    elapsed_ms = 3020;
    expected_bauds.push_back(115200);
  }
  SECTION("UART open fails") {
    serial.open_ok = false;
    succeeds = false;
    elapsed_ms = 0;
    expected_bauds.push_back(115200);
  }
  SECTION("receiver wakes near the end of the first probe") {
    serial.receiver_wake_ms = start_ms + 2980;
    elapsed_ms = 2980;
  }

  REQUIRE(gps.request_sleep(UINT32_MAX) == succeeds);
  CHECK(serial.opened_bauds == expected_bauds);
  CHECK_FALSE(serial.open);
  CHECK(clock.retained_ms - start_ms == elapsed_ms);
  REQUIRE(serial.writes.size() == (succeeds ? 1 : 0));
  if (succeeds) {
    CHECK(is_command(serial.writes.back(), 0x41));
    CHECK(gps.sleep_remaining_ms() == UINT32_MAX);
  } else {
    CHECK(gps.sleep_deadline_ms() == 0);
  }
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
  CHECK(is_command(serial.writes[0], 0x00));
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

TEST_CASE_METHOD(Fixture, "GPS service repeats failed activation with a delay between attempts",
                 "[gps_service]") {
  SECTION("UART unavailable") { serial.open_ok = false; }
  SECTION("Baud switch failed") { serial.tx_ok = false; }
  SECTION("Neither baud responds") { serial.respond = false; }
  size_t bauds_per_attempt = 0;
  uint64_t previous_failure_ms = 0;
  clock.on_wait = [&] {
    CHECK(GpsServiceTestAccess::running(service));
    CHECK_FALSE(serial.open);
    CHECK(clock.waits.back() == 30000);
    if (clock.waits.size() == 1) {
      bauds_per_attempt = serial.opened_bauds.size();
    } else {
      CHECK(serial.opened_bauds.size() == bauds_per_attempt * clock.waits.size());
      CHECK(clock.retained_ms >= previous_failure_ms + 30000);
    }
    previous_failure_ms = clock.retained_ms;
    if (clock.waits.size() == 3) {
      GpsServiceTestAccess::cancel(service);
    } else {
      clock.delay_ms_impl(clock.waits.back());
    }
  };
  GpsServiceTestAccess::run(service);
  CHECK(clock.waits == std::vector<uint32_t>{30000, 30000, 30000});
  CHECK_FALSE(GpsServiceTestAccess::running(service));
  CHECK_FALSE(serial.open);
  for (const auto &write : serial.writes)
    CHECK_FALSE(is_command(write, 0x40));
}

TEST_CASE_METHOD(Fixture, "GPS activation recovers automatically on the same task",
                 "[gps_service][jobs]") {
  const auto task = GpsServiceTestAccess::attach_worker(service);
  SECTION("UART becomes available") { serial.open_ok = false; }
  SECTION("baud transmission recovers") { serial.tx_ok = false; }
  SECTION("receiver starts responding") { serial.respond = false; }
  uint64_t failed_at_ms = 0;
  clock.on_wait = [&] {
    CHECK(GpsServiceTestAccess::task_handle(service) == task);
    if (clock.waits.size() == 1) {
      CHECK(clock.waits.back() == 30000);
      CHECK_FALSE(serial.open);
      failed_at_ms = clock.retained_ms;
      clock.delay_ms_impl(clock.waits.back());
      serial.open_ok = true;
      serial.tx_ok = true;
      serial.respond = true;
    } else {
      CHECK(command_count(serial, 0x40) == 1);
      CHECK(clock.waits.back() != 30000);
      CHECK(serial.open);
      CHECK(serial.writes.back().time_ms >= failed_at_ms + 30000);
      GpsServiceTestAccess::cancel(service);
    }
  };
  GpsServiceTestAccess::run(service);
  CHECK(clock.waits.size() == 2);
  CHECK(GpsServiceTestAccess::task_handle(service) == task);
}

TEST_CASE_METHOD(Fixture, "GPS stop interrupts an acquisition retry delay", "[gps_service][jobs]") {
  serial.respond = false;
  uint64_t stopped_at_ms = 0;
  clock.on_wait = [&] {
    REQUIRE(clock.waits.back() == 30000);
    stopped_at_ms = clock.retained_ms;
    service.stop();
  };
  GpsServiceTestAccess::run(service);
  CHECK(clock.waits == std::vector<uint32_t>{30000});
  CHECK(clock.retained_ms == stopped_at_ms);
  CHECK(serial.opened_bauds == std::vector<int>{9600, 115200});
  CHECK(serial.writes.empty());
  CHECK_FALSE(serial.open);
  CHECK_FALSE(GpsServiceTestAccess::running(service));
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
  clock.on_wait = [&] {
    if (clock.waits.size() == 1) {
      clock.retained_ms += 1000;
      REQUIRE(service.acquire()); // Same job wakes the task without changing the deadline.
    } else {
      clock.retained_ms += clock.waits.back();
    }
  };
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
  REQUIRE(serial.writes.size() == 1); // One failed sleep command.
  CHECK(is_command(serial.writes.back(), 0x41));
}

TEST_CASE_METHOD(Fixture, "GPS job requests cannot create a stopped task", "[gps_service][jobs]") {
  SECTION("before initialization") {}
  SECTION("after explicit stop") { service.stop(); }
  const auto now = clock.retained_ms;
  CHECK_FALSE(service.acquire());
  CHECK_FALSE(service.maintain_sleep(60000));
  CHECK_FALSE(GpsServiceTestAccess::running(service));
  CHECK(GpsServiceTestAccess::task_handle(service) == nullptr);
  CHECK(serial.opened_bauds.empty());
  CHECK(serial.writes.empty());
  CHECK(clock.retained_ms == now);
}

TEST_CASE_METHOD(Fixture, "GPS start leaves an existing task and its job unchanged",
                 "[gps_service][jobs]") {
  const auto task = GpsServiceTestAccess::attach_worker(service);
  clock.on_wait = [&] {
    if (clock.waits.size() == 1) {
      REQUIRE(service.start(60000)); // Lifecycle call must not change acquisition.
    } else {
      CHECK(command_count(serial, 0x40) == 1);
      CHECK(command_count(serial, 0x41) == 0);
      CHECK(GpsServiceTestAccess::task_handle(service) == task);
      GpsServiceTestAccess::cancel(service);
    }
  };
  GpsServiceTestAccess::run(service);
}

TEST_CASE_METHOD(Fixture, "GPS job switches reuse the worker and preserve a sent sleep deadline",
                 "[gps_service][jobs]") {
  const auto task = GpsServiceTestAccess::attach_worker(service);
  constexpr uint32_t interval_ms = 60000;
  uint64_t sleep_deadline = 0;
  unsigned stage = 0;
  clock.on_wait = [&] {
    CHECK(GpsServiceTestAccess::task_handle(service) == task);
    if (stage == 0) {
      CHECK(command_count(serial, 0x40) == 1);
      CHECK(serial.open);
      const auto writes = serial.writes.size();
      const auto now = clock.retained_ms;
      REQUIRE(service.acquire()); // Same acquisition job: no restart.
      CHECK_FALSE(service.maintain_sleep(0));
      CHECK_FALSE(service.maintain_sleep(UINT32_MAX - 1));
      REQUIRE(service.maintain_sleep(interval_ms));
      CHECK(GpsServiceTestAccess::active_interval(service) == 0); // Applied by the worker.
      CHECK(serial.open); // Caller must leave UART ownership with the worker.
      CHECK(serial.writes.size() == writes);
      CHECK(clock.retained_ms == now); // Request does not join or probe.
    } else if (stage == 1) {
      CHECK(command_count(serial, 0x41) == 1);
      CHECK_FALSE(serial.open);
      sleep_deadline = service.sleep_deadline_ms();
      REQUIRE(service.maintain_sleep(interval_ms));
      REQUIRE(service.acquire());
      CHECK(service.sleep_deadline_ms() == sleep_deadline);
    } else if (stage == 2) {
      CHECK(clock.waits.back() == interval_ms);
      CHECK(command_count(serial, 0x40) == 1); // No premature acquisition.
      clock.delay_ms_impl(clock.waits.back());
    } else {
      CHECK(command_count(serial, 0x40) == 2);
      CHECK(service.sleep_deadline_ms() == 0);
      CHECK(serial.writes.back().time_ms >= sleep_deadline);
      GpsServiceTestAccess::cancel(service);
    }
    ++stage;
  };
  GpsServiceTestAccess::run(service);
  CHECK(stage == 4);
  CHECK(command_count(serial, 0x41) == 1);
  CHECK(GpsServiceTestAccess::task_handle(service) == task);
  service.stop();
  CHECK(GpsServiceTestAccess::task_handle(service) == nullptr);
  CHECK_FALSE(serial.open);
}

TEST_CASE_METHOD(Fixture, "GPS repeated acquisition requests leave the active job untouched",
                 "[gps_service][jobs]") {
  const auto task = GpsServiceTestAccess::attach_worker(service);
  clock.on_wait = [&] {
    if (clock.waits.size() == 1) {
      GpsServiceTestAccess::seed_fix(service);
      REQUIRE(service.acquire());
      CHECK(is_position_valid(service.get_latest_fix().position));
    } else {
      CHECK(command_count(serial, 0x40) == 1);
      CHECK(serial.opened_bauds == std::vector<int>{9600, 115200});
      GpsServiceTestAccess::cancel(service);
    }
  };
  GpsServiceTestAccess::run(service);
  CHECK(GpsServiceTestAccess::task_handle(service) == task);
}

TEST_CASE_METHOD(Fixture, "GPS job changes during connection do not send superseded commands",
                 "[gps_service][jobs]") {
  const auto task = GpsServiceTestAccess::attach_worker(service);
  bool initially_sleeping = false;
  SECTION("acquisition becomes sleep maintenance") {}
  SECTION("sleep maintenance becomes acquisition") { initially_sleeping = true; }
  bool changed = false;
  clock.on_delay = [&] {
    if (!changed) {
      changed = true;
      const auto now = clock.retained_ms;
      REQUIRE(serial.writes.empty());
      REQUIRE((initially_sleeping ? service.acquire() : service.maintain_sleep(60000)));
      CHECK(serial.open);
      CHECK(clock.retained_ms == now);
      CHECK(GpsServiceTestAccess::task_handle(service) == task);
    }
  };
  clock.on_wait = [&] { GpsServiceTestAccess::cancel(service); };
  if (initially_sleeping) {
    GpsServiceTestAccess::run_sleeping(service, 60000);
    CHECK(command_count(serial, 0x41) == 0);
    CHECK(command_count(serial, 0x40) == 1);
  } else {
    GpsServiceTestAccess::run(service);
    REQUIRE(serial.writes.size() == 1);
    CHECK(is_command(serial.writes.front(), 0x41));
  }
  CHECK(changed);
  CHECK_FALSE(serial.open);
}

TEST_CASE_METHOD(Fixture, "GPS latest job wins after rapid changes during a probe",
                 "[gps_service][jobs]") {
  const auto task = GpsServiceTestAccess::attach_worker(service);
  bool changed = false;
  clock.on_delay = [&] {
    if (!changed) {
      changed = true;
      REQUIRE(service.maintain_sleep(60000));
      REQUIRE(service.acquire());
    }
  };
  clock.on_wait = [&] { GpsServiceTestAccess::cancel(service); };
  GpsServiceTestAccess::run(service);
  CHECK(serial.opened_bauds == std::vector<int>{9600, 115200}); // No intermediate restart.
  CHECK(command_count(serial, 0x41) == 0);
  CHECK(command_count(serial, 0x40) == 1);
  CHECK(GpsServiceTestAccess::task_handle(service) == task);
}

TEST_CASE_METHOD(Fixture, "GPS completes receiver setup before applying a job change",
                 "[gps_service][jobs]") {
  GpsServiceTestAccess::attach_worker(service);
  uint8_t change_after = 0x00;
  SECTION("during baud switch") {}
  SECTION("during receiver configuration") { change_after = 0x10; }
  bool changed = false;
  clock.on_delay = [&] {
    if (!changed && command_count(serial, change_after) != 0) {
      changed = true;
      REQUIRE(service.maintain_sleep(60000));
    }
  };
  clock.on_wait = [&] { GpsServiceTestAccess::cancel(service); };
  GpsServiceTestAccess::run(service);
  CHECK(changed);
  CHECK(command_count(serial, 0x40) == 1);
  CHECK(command_count(serial, 0x41) == 1);
  CHECK(serial.writes.back().baud == GpsDriver::MODULE_ACTIVE_BAUD);
  CHECK(command_count(serial, 0x10) != 0);
  CHECK(command_count(serial, 0x0C) != 0);
}

TEST_CASE_METHOD(Fixture, "GPS changing sleep interval preserves the current sleep",
                 "[gps_service][jobs]") {
  GpsServiceTestAccess::attach_worker(service);
  uint32_t next_interval_ms = 120000;
  SECTION("ordinary interval") {}
  SECTION("energy-saving maximum interval") { next_interval_ms = UINT32_MAX; }
  uint64_t deadline = 0;
  clock.on_wait = [&] {
    if (clock.waits.size() == 1) {
      deadline = service.sleep_deadline_ms();
      REQUIRE(service.maintain_sleep(next_interval_ms));
      CHECK(service.sleep_deadline_ms() == deadline);
    } else if (clock.waits.size() == 2) {
      CHECK(clock.waits.back() == 60000);
      CHECK(command_count(serial, 0x41) == 1);
      clock.delay_ms_impl(clock.waits.back());
    } else {
      CHECK(command_count(serial, 0x41) == 2);
      CHECK(service.sleep_remaining_ms() == next_interval_ms);
      CHECK(serial.writes.back().time_ms >= deadline);
      GpsServiceTestAccess::cancel(service);
    }
  };
  GpsServiceTestAccess::run_sleeping(service, 60000);
  const uint32_t next_wait_ms = next_interval_ms == UINT32_MAX ? 3600000 : next_interval_ms;
  CHECK(clock.waits == std::vector<uint32_t>{60000, 60000, next_wait_ms});
}

TEST_CASE_METHOD(Fixture, "GPS stop notification replaces an unconsumed job request",
                 "[gps_service][jobs]") {
  clock.on_wait = [&] {
    REQUIRE(service.acquire());
    service.stop();
  };
  GpsServiceTestAccess::run_sleeping(service, 60000);
  CHECK_FALSE(GpsServiceTestAccess::running(service));
  CHECK(GpsServiceTestAccess::task_handle(service) == nullptr);
  CHECK_FALSE(serial.open);
  CHECK(command_count(serial, 0x41) == 1);
  CHECK(command_count(serial, 0x40) == 0);
  CHECK(service.sleep_remaining_ms() == 60000);
}

TEST_CASE_METHOD(Fixture, "GPS honors sleep sent while acquisition is being requested",
                 "[gps_service][jobs]") {
  GpsServiceTestAccess::attach_worker(service);
  serial.tx_delay_ms = 20;
  bool changed = false;
  clock.on_delay = [&] {
    if (!changed && command_count(serial, 0x41) == 1) {
      changed = true;
      REQUIRE(service.acquire());
    }
  };
  uint64_t deadline = 0;
  clock.on_wait = [&] {
    if (clock.waits.size() == 1) {
      deadline = service.sleep_deadline_ms();
      CHECK(clock.waits.back() == 60000);
      CHECK(command_count(serial, 0x40) == 0);
      clock.delay_ms_impl(clock.waits.back());
    } else {
      CHECK(command_count(serial, 0x40) == 1);
      CHECK(serial.writes.back().time_ms >= deadline);
      GpsServiceTestAccess::cancel(service);
    }
  };
  GpsServiceTestAccess::run_sleeping(service, 60000);
  CHECK(changed);
  CHECK(command_count(serial, 0x41) == 1);
}

TEST_CASE_METHOD(Fixture, "GPS failed acquisition accepts a new request on the same worker",
                 "[gps_service][jobs]") {
  const auto task = GpsServiceTestAccess::attach_worker(service);
  serial.respond = false;
  bool request_sleep = false;
  SECTION("explicit acquisition retry") {}
  SECTION("switch to sleep maintenance") { request_sleep = true; }
  uint64_t requested_at_ms = 0;
  clock.on_wait = [&] {
    CHECK(GpsServiceTestAccess::task_handle(service) == task);
    if (clock.waits.size() == 1) {
      CHECK(clock.waits.back() == 30000);
      CHECK(serial.writes.empty());
      CHECK_FALSE(serial.open);
      requested_at_ms = clock.retained_ms;
      serial.respond = true;
      REQUIRE((request_sleep ? service.maintain_sleep(60000) : service.acquire()));
    } else {
      CHECK(command_count(serial, request_sleep ? 0x41 : 0x40) == 1);
      CHECK(serial.writes.back().time_ms < requested_at_ms + 30000);
      GpsServiceTestAccess::cancel(service);
    }
  };
  GpsServiceTestAccess::run(service);
  CHECK(clock.waits.size() == 2);
  CHECK(GpsServiceTestAccess::task_handle(service) == task);
}

TEST_CASE_METHOD(Fixture, "GPS acquisition request interrupts a failed-sleep retry",
                 "[gps_service][jobs]") {
  GpsServiceTestAccess::attach_worker(service);
  serial.tx_ok = false;
  clock.on_wait = [&] {
    if (clock.waits.size() == 1) {
      CHECK(clock.waits.back() == 1000);
      serial.tx_ok = true;
      REQUIRE(service.acquire());
    } else {
      CHECK(command_count(serial, 0x40) == 1);
      GpsServiceTestAccess::cancel(service);
    }
  };
  GpsServiceTestAccess::run_sleeping(service, 60000);
  CHECK(command_count(serial, 0x41) == 1);
  CHECK(service.sleep_deadline_ms() == 0);
}

TEST_CASE_METHOD(Fixture, "GPS finishes its acquisition iteration before applying a job change",
                 "[gps_service][jobs]") {
  GpsService gps(driver, reinterpret_cast<RtosQueueHandle>(&clock), {.posting_interval_ms = 1});
  GpsServiceTestAccess::attach_worker(gps);
  bool supersede_during_read = false;
  SECTION("active acquisition publishes valid data") {}
  SECTION("sleep request during reading takes effect after publication") {
    supersede_during_read = true;
  }
  bool request_pending = false;
  serial.on_read = [&] {
    if (request_pending) {
      request_pending = false;
      REQUIRE(gps.maintain_sleep(60000));
    }
  };
  clock.on_wait = [&] {
    if (clock.waits.size() == 1) {
      clock.events.clear();
      const std::string fix =
          "$GPGGA,092725.00,4717.11364,N,00833.91565,E,1,08,1.01,499.6,M,48.0,M,,*53\r\n";
      serial.rx.insert(serial.rx.end(), fix.begin(), fix.end());
      request_pending = supersede_during_read;
      clock.delay_ms_impl(1000);
    } else {
      REQUIRE(clock.events.size() == 1);
      CHECK(clock.events.front().type == EventType::GpsFixUpdate);
      CHECK(is_position_valid(clock.events.front().gps_data.position));
      if (supersede_during_read) {
        CHECK_FALSE(is_position_valid(gps.get_latest_fix().position));
        CHECK(command_count(serial, 0x41) == 1);
      } else {
        CHECK(is_position_valid(gps.get_latest_fix().position));
      }
      GpsServiceTestAccess::cancel(gps);
    }
  };
  GpsServiceTestAccess::run(gps);
  CHECK(clock.waits.size() == 2);
}
