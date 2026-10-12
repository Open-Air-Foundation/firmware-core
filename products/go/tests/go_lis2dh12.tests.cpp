#include <catch2/catch_test_macros.hpp>

#include "accel/lis2dh12.h"
#include "rtos.h"

#include <array>
#include <vector>
#include <utility>

namespace {
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
constexpr uint8_t INT1_CFG = 0x30;
constexpr uint8_t INT1_SRC = 0x31;
constexpr uint8_t INT1_THS = 0x32;
constexpr uint8_t INT1_DURATION = 0x33;
} // namespace

struct TestI2cBus {
  std::array<uint8_t, 256> registers{};
  std::vector<std::pair<uint8_t, uint8_t>> writes;
  int fail_write_reg = -1;
  int fail_read_reg = -1;
  bool add_ok = true;
  unsigned xyz_reads = 0;

  TestI2cBus() { registers[WHO_AM_I] = 0x33; }
  void sample(int16_t x, int16_t y, int16_t z, uint8_t status = 0x08) {
    const int16_t values[] = {x, y, z};
    for (unsigned axis = 0; axis < 3; ++axis) {
      const auto encoded = static_cast<uint16_t>(values[axis] * 16);
      registers[OUT_X_L + axis * 2] = encoded & 0xff;
      registers[OUT_X_L + axis * 2 + 1] = encoded >> 8;
    }
    registers[STATUS] = status;
  }
};

esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *,
                                    i2c_master_dev_handle_t *device) {
  if (!bus->add_ok) {
    return ESP_FAIL;
  }
  *device = bus;
  return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t) { return ESP_OK; }
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *bytes, size_t size,
                              int) {
  REQUIRE(size == 2);
  device->writes.emplace_back(bytes[0], bytes[1]);
  if (bytes[0] == device->fail_write_reg) {
    return ESP_FAIL;
  }
  device->registers[bytes[0]] = bytes[1];
  return ESP_OK;
}
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *write,
                                      size_t write_size, uint8_t *read, size_t read_size, int) {
  REQUIRE(write_size == 1);
  const uint8_t reg = write[0] & 0x7f;
  if (reg == device->fail_read_reg) {
    return ESP_FAIL;
  }
  if (read_size > 1) {
    REQUIRE((write[0] & 0x80) != 0);
  }
  for (size_t i = 0; i < read_size; ++i) {
    read[i] = device->registers[reg + i];
  }
  if (reg == INT1_SRC) {
    device->registers[INT1_SRC] = 0;
  }
  if (reg == OUT_X_L) {
    ++device->xyz_reads;
    device->registers[STATUS] = 0;
  }
  return ESP_OK;
}

namespace {
class Clock : public RTOS {
public:
  uint32_t elapsed = 0;
  void delay_ms_impl(uint32_t ms) override { elapsed += ms; }
  uint64_t get_time_ms_impl() override { return elapsed; }
};
struct Fixture {
  TestI2cBus bus;
  Clock clock;
  LIS2DH12 driver{&bus, {}};
  Fixture() { RTOS::set_instance(&clock); }
  ~Fixture() { RTOS::set_instance(nullptr); }
};
AccelConfig filtered_config() {
  return {
      .sample_rate_hz = 100,
      .range = AccelRange::G4,
      .high_pass = true,
      .interrupt = {
          .high_axes = AccelAxis::ALL, .threshold_mg = 640, .duration_ms = 20, .latched = true}};
}
} // namespace

TEST_CASE("LIS2DH12: init probes and leaves sampling disabled", "[LIS2DH12]") {
  Fixture f;
  f.bus.registers[CTRL1] = 0x57;
  f.bus.registers[CTRL3] = 0x40;
  REQUIRE(f.driver.init());
  CHECK(f.bus.registers[CTRL1] == 0);
  CHECK(f.bus.registers[CTRL3] == 0);
  CHECK(f.clock.elapsed == 0);
  AccelReading reading{1, 2, 3};
  CHECK(f.driver.read(reading) == AccelReadResult::Error);
  CHECK_FALSE(reading.is_valid());
  CHECK(f.bus.xyz_reads == 0);
}

TEST_CASE("LIS2DH12: absent or unidentified devices receive no configuration writes",
          "[LIS2DH12]") {
  Fixture f;
  SECTION("wrong identity") { f.bus.registers[WHO_AM_I] = 0; }
  SECTION("identity read failure") { f.bus.fail_read_reg = WHO_AM_I; }
  SECTION("device registration failure") { f.bus.add_ok = false; }
  SECTION("null bus") {
    LIS2DH12 driver(nullptr, {});
    CHECK_FALSE(driver.init());
    CHECK_FALSE(driver.configure(filtered_config()));
    return;
  }
  CHECK_FALSE(f.driver.init());
  CHECK_FALSE(f.driver.configure(filtered_config()));
  CHECK(f.driver.power_down());
  CHECK(f.bus.writes.empty());
}

TEST_CASE("LIS2DH12: caller selects filtered capture and restores unfiltered sampling",
          "[LIS2DH12]") {
  Fixture f;
  REQUIRE(f.driver.init());
  REQUIRE(f.driver.configure(filtered_config()));
  CHECK(f.bus.registers[CTRL1] == 0x57);
  CHECK(f.bus.registers[CTRL2] == 0x09);
  CHECK(f.bus.registers[CTRL3] == 0x40);
  CHECK(f.bus.registers[CTRL4] == 0x98);
  CHECK(f.bus.registers[CTRL5] == 0x08);
  CHECK(f.bus.registers[CTRL6] == 0);
  CHECK(f.bus.registers[INT1_CFG] == 0x2a);
  CHECK(f.bus.registers[INT1_THS] == 20);
  CHECK(f.bus.registers[INT1_DURATION] == 2);
  CHECK(f.clock.elapsed == 530);
  f.bus.sample(100, -200, 1000);
  AccelReading reading;
  REQUIRE(f.driver.read(reading) == AccelReadResult::Ready);
  CHECK(reading.x_mg == 200);
  CHECK(reading.y_mg == -400);
  CHECK(reading.z_mg == 2000);

  REQUIRE(f.driver.configure({.sample_rate_hz = 100, .range = AccelRange::G4}));
  CHECK(f.bus.registers[CTRL1] == 0x57);
  CHECK(f.bus.registers[CTRL2] == 0);
  CHECK(f.bus.registers[CTRL3] == 0);
  CHECK(f.bus.registers[CTRL4] == 0x98);
  CHECK(f.bus.registers[CTRL5] == 0);
  CHECK(f.bus.registers[INT1_CFG] == 0);
  CHECK(f.clock.elapsed == 600);
  f.bus.sample(100, -200, 1000);
  REQUIRE(f.driver.read(reading) == AccelReadResult::Ready);
  CHECK(reading.x_mg == 200);
  CHECK(reading.y_mg == -400);
  CHECK(reading.z_mg == 2000);
}

TEST_CASE("LIS2DH12: rate, range, axes and polarity are hardware configuration", "[LIS2DH12]") {
  Fixture f;
  REQUIRE(f.driver.init());
  const AccelConfig config{.sample_rate_hz = 50,
                           .range = AccelRange::G8,
                           .interrupt = {.high_axes = AccelAxis::Y,
                                         .low_axes = AccelAxis::Z,
                                         .threshold_mg = 620,
                                         .duration_ms = 40,
                                         .active_low = true}};
  REQUIRE(f.driver.configure(config));
  CHECK(f.bus.registers[CTRL1] == 0x47);
  CHECK(f.bus.registers[CTRL4] == 0xa8);
  CHECK(f.bus.registers[INT1_CFG] == 0x18);
  CHECK(f.bus.registers[INT1_THS] == 10);
  CHECK(f.bus.registers[INT1_DURATION] == 2);
  CHECK(f.bus.registers[CTRL5] == 0);
  CHECK(f.bus.registers[CTRL6] == 0x02);
  CHECK(f.clock.elapsed == 140);
  AccelReading reading;
  f.bus.sample(-1000, 1, 500);
  REQUIRE(f.driver.read(reading) == AccelReadResult::Ready);
  CHECK(reading.x_mg == -4000);
  CHECK(reading.y_mg == 4);
  CHECK(reading.z_mg == 2000);
  REQUIRE(f.driver.configure({.sample_rate_hz = 400, .range = AccelRange::G16}));
  f.bus.sample(-2048, 2047, 1);
  REQUIRE(f.driver.read(reading) == AccelReadResult::Ready);
  CHECK(reading.x_mg == -24576);
  CHECK(reading.y_mg == 24564);
  CHECK(reading.z_mg == 12);
  CHECK(reading.clipped);
}

TEST_CASE("LIS2DH12: invalid settings do not alter an existing configuration", "[LIS2DH12]") {
  Fixture f;
  REQUIRE(f.driver.init());
  auto config = filtered_config();
  REQUIRE(f.driver.configure(config));
  SECTION("unspecified rate") { config.sample_rate_hz = 0; }
  SECTION("unsupported rate") { config.sample_rate_hz = 123; }
  SECTION("invalid range") { config.range = static_cast<AccelRange>(3); }
  SECTION("invalid high axis") { config.interrupt.high_axes = 8; }
  SECTION("invalid low axis") { config.interrupt.low_axes = 8; }
  SECTION("zero threshold") { config.interrupt.threshold_mg = 0; }
  SECTION("threshold needs rounding") { config.interrupt.threshold_mg = 641; }
  SECTION("threshold exceeds register") { config.interrupt.threshold_mg = 4096; }
  SECTION("duration needs rounding") { config.interrupt.duration_ms = 15; }
  SECTION("duration exceeds register") { config.interrupt.duration_ms = 1280; }
  const auto writes = f.bus.writes.size();
  CHECK_FALSE(f.driver.configure(config));
  CHECK(f.bus.writes.size() == writes);
  f.bus.sample(1, 1, 1);
  AccelReading reading;
  REQUIRE(f.driver.read(reading) == AccelReadResult::Ready);
  CHECK(reading.x_mg == 2);
}

TEST_CASE("LIS2DH12: read reports freshness and quality without reusing old output", "[LIS2DH12]") {
  Fixture f;
  REQUIRE(f.driver.init());
  REQUIRE(f.driver.configure(filtered_config()));
  f.bus.sample(2047, -2048, -1, 0x88);
  AccelReading reading;
  REQUIRE(f.driver.read(reading) == AccelReadResult::Ready);
  CHECK(reading.is_valid());
  CHECK(reading.x_mg == 4094);
  CHECK(reading.y_mg == -4096);
  CHECK(reading.z_mg == -2);
  CHECK(reading.overrun);
  CHECK(reading.clipped);
  SECTION("no fresh sample") {
    CHECK(f.driver.read(reading) == AccelReadResult::NotReady);
    CHECK(f.bus.xyz_reads == 1);
  }
  SECTION("status failure") {
    f.bus.fail_read_reg = STATUS;
    CHECK(f.driver.read(reading) == AccelReadResult::Error);
  }
  SECTION("XYZ failure") {
    f.bus.sample(1, 2, 3);
    f.bus.fail_read_reg = OUT_X_L;
    CHECK(f.driver.read(reading) == AccelReadResult::Error);
  }
  CHECK_FALSE(reading.is_x_valid());
  CHECK_FALSE(reading.is_y_valid());
  CHECK_FALSE(reading.is_z_valid());
  CHECK_FALSE(reading.overrun);
  CHECK_FALSE(reading.clipped);
}

TEST_CASE("LIS2DH12: interrupt acknowledgement reports activity and clears the latch",
          "[LIS2DH12]") {
  Fixture f;
  REQUIRE(f.driver.init());
  REQUIRE(f.driver.configure(filtered_config()));
  f.bus.registers[INT1_SRC] = 0x48;
  bool active = false;
  REQUIRE(f.driver.read_interrupt(active));
  CHECK(active);
  REQUIRE(f.driver.read_interrupt(active));
  CHECK_FALSE(active);
  active = true;
  f.bus.fail_read_reg = INT1_SRC;
  CHECK_FALSE(f.driver.read_interrupt(active));
  CHECK_FALSE(active);
}

TEST_CASE("LIS2DH12: failed setup and power-down prevent reads until reconfigured", "[LIS2DH12]") {
  Fixture f;
  REQUIRE(f.driver.init());
  REQUIRE(f.driver.configure(filtered_config()));
  SECTION("range write failure") {
    f.bus.fail_write_reg = CTRL4;
    CHECK_FALSE(f.driver.configure({.sample_rate_hz = 100}));
    CHECK(f.bus.registers[CTRL1] == 0);
  }
  SECTION("filter reset read failure") {
    f.bus.fail_read_reg = REFERENCE;
    CHECK_FALSE(f.driver.configure(filtered_config()));
    CHECK(f.bus.registers[CTRL1] == 0);
  }
  SECTION("latch clear failure") {
    f.bus.fail_read_reg = INT1_SRC;
    CHECK_FALSE(f.driver.configure(filtered_config()));
    CHECK(f.bus.registers[CTRL1] == 0);
  }
  SECTION("power-down attempts both writes") {
    f.bus.fail_write_reg = CTRL3;
    CHECK_FALSE(f.driver.power_down());
    CHECK(f.bus.registers[CTRL1] == 0);
  }
  SECTION("successful power-down") { REQUIRE(f.driver.power_down()); }
  SECTION("failed reprobe invalidates previous configuration") {
    f.bus.registers[WHO_AM_I] = 0;
    CHECK_FALSE(f.driver.init());
  }
  AccelReading reading;
  CHECK(f.driver.read(reading) == AccelReadResult::Error);
  CHECK_FALSE(reading.is_valid());
  f.bus.fail_write_reg = f.bus.fail_read_reg = -1;
  f.bus.registers[WHO_AM_I] = 0x33;
  REQUIRE(f.driver.init());
  REQUIRE(f.driver.configure(filtered_config()));
  f.bus.sample(1, 2, 3);
  CHECK(f.driver.read(reading) == AccelReadResult::Ready);
}
