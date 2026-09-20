/**
 * AirGradient Go — StorageService unit tests
 *
 * Covers two tiers independently:
 *
 * Cache tier  — read_cached_field() field extraction: valid values, invalid
 *               sentinels, oldest-first ordering, max_count cap.
 *
 * Route tier  — start/end lifecycle, file naming, append, binary round-trip,
 *               and sleep-resume (point count derived from file size).
 *
 * Route tests use a real POSIX temp directory so fopen/fwrite/stat/mkdir run
 * natively on the host without any ESP-IDF dependency.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "go_storage.h"
#include "hal/payload_cache_storage.h"
#include "nand_storage.h"
#include "rtos.h"
#include "services/payload_cache.h"

// ============================================================================
// FakeRTOS — virtual clock for tests that exercise the durability budget.
//
// `append_route_point()` calls `RTOS::get_time_ms()`, so every test in this
// translation unit needs a valid RTOS singleton installed. The fake reads
// 0 by default; tests that exercise the fsync cadence call advance() to
// move the virtual clock forward.
// ============================================================================

class FakeRTOS : public RTOS {
public:
  void delay_ms_impl(uint32_t /*ms*/) override {}
  uint64_t get_time_ms_impl() override { return _now_ms; }

  void set(uint64_t now_ms) { _now_ms = now_ms; }
  void advance(uint64_t delta_ms) { _now_ms += delta_ms; }

private:
  uint64_t _now_ms = 0;
};

static FakeRTOS s_fake_rtos;

struct GlobalSetup {
  GlobalSetup() { RTOS::set_instance(&s_fake_rtos); }
  ~GlobalSetup() { RTOS::set_instance(nullptr); }
};

static GlobalSetup s_global_setup;

// ============================================================================
// MockNandStorage — for testing init() failure paths
// ============================================================================

class MockNandStorage : public trompeloeil::mock_interface<NandStorage> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK0(deinit);
  IMPLEMENT_MOCK0(format);
  IMPLEMENT_CONST_MOCK0(is_mounted);
  IMPLEMENT_CONST_MOCK0(mount_path);
};

// ============================================================================
// FakeNandStorage — simple concrete stub for route-tier tests.
//
// Always reports mounted and returns the path supplied at construction.
// Avoids trompeloeil complexity for tests that only care about file I/O
// behaviour, not about how many times the NandStorage methods are called.
// ============================================================================

class FakeNandStorage : public NandStorage {
public:
  explicit FakeNandStorage(const char *path) : _path(path) {}

  bool init() override { return true; }
  void deinit() override {}
  bool format() override { return true; }
  bool is_mounted() const override { return true; }
  const char *mount_path() const override { return _path; }

private:
  const char *_path;
};

// ============================================================================
// StubPayloadCacheStorage
//
// Silent in-memory stub — load() always signals "no saved state" so the cache
// starts empty; save() silently succeeds.  Avoids per-test mock wiring noise
// for tests that only care about read_cached_field() behaviour.
// ============================================================================

class StubPayloadCacheStorage : public PayloadCacheStorage {
public:
  bool load(PayloadCacheStorageData &) override { return false; }
  bool save(const PayloadCacheStorageData &) override { return true; }
  bool clear() override { return true; }
};

// ============================================================================
// TempDir
//
// RAII helper that creates a unique temp directory via mkdtemp() and removes
// it (recursively) in the destructor.
// ============================================================================

struct TempDir {
  char path[256];

  TempDir() {
    std::snprintf(path, sizeof(path), "/tmp/go_storage_test_XXXXXX");
    REQUIRE(mkdtemp(path) != nullptr);
  }

  ~TempDir() { std::filesystem::remove_all(path); }

  // Returns the full path to a file under this directory.
  std::string operator/(const char *rel) const { return std::string(path) + "/" + rel; }
};

// ============================================================================
// Helpers
// ============================================================================

// Build a MeasuresAGo with distinct, in-range values seeded from an integer.
static MeasuresAGo make_valid_entry(int seed) {
  MeasuresAGo m{};
  m.temp_hum_a.temperature = static_cast<float>(seed) + 0.1f;   // valid: -40..125
  m.temp_hum_a.humidity = static_cast<float>(seed % 90) + 5.0f; // valid: 0..100
  m.pm_a.pm_01 = static_cast<float>(seed) + 1.0f;               // valid: >= 0
  m.pm_a.pm_25 = static_cast<float>(seed) + 2.0f;
  m.pm_a.pm_10 = static_cast<float>(seed) + 3.0f;
  m.co2.co2 = seed + 400;            // valid: 0..10000
  m.tvoc_nox.tvoc_index = seed + 10; // valid: >= 0
  m.tvoc_nox.nox_index = seed + 20;
  // fill remaining PMData sub-fields to valid (>= 0)
  m.pm_a.pm_01_sp = m.pm_a.pm_25_sp = m.pm_a.pm_10_sp = 0.0f;
  m.pm_a.pm_03_pc = m.pm_a.pm_05_pc = m.pm_a.pm_01_pc = 0.0f;
  m.pm_a.pm_25_pc = m.pm_a.pm_5_pc = m.pm_a.pm_10_pc = 0.0f;
  m.tvoc_nox.tvoc_raw = seed + 50;
  m.tvoc_nox.nox_raw = seed + 60;
  return m;
}

// Build an entry where every field carries an invalid sentinel value.
static MeasuresAGo make_invalid_entry() {
  MeasuresAGo m{};
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;          // -1000
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;                // -1
  m.pm_a.pm_01 = m.pm_a.pm_25 = m.pm_a.pm_10 = MeasuresInvalid::PM; // -1
  m.pm_a.pm_01_sp = m.pm_a.pm_25_sp = m.pm_a.pm_10_sp = MeasuresInvalid::PM;
  m.pm_a.pm_03_pc = m.pm_a.pm_05_pc = m.pm_a.pm_01_pc = MeasuresInvalid::PM;
  m.pm_a.pm_25_pc = m.pm_a.pm_5_pc = m.pm_a.pm_10_pc = MeasuresInvalid::PM;
  m.co2.co2 = MeasuresInvalid::CO2;              // -1
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC; // -1
  m.tvoc_nox.tvoc_raw = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX; // -1
  m.tvoc_nox.nox_raw = MeasuresInvalid::NOX;
  return m;
}

// Build a minimal RoutePoint with identifiable values for binary verification.
static RoutePoint make_route_point(int seed) {
  RoutePoint p{};
  p.timestamp = static_cast<time_t>(1000 + seed);
  p.gps.position.latitude = static_cast<double>(seed);
  p.gps.position.longitude = static_cast<double>(seed) + 0.5;
  p.gps.altitude_m = static_cast<float>(seed) + 100.0f;
  p.gps.fix.fix_type = GpsFixType::Fix3D;
  p.sensors = make_valid_entry(seed);
  return p;
}

// ============================================================================
// TEST CASE 1 — Cache: read_cached_field field extraction
// ============================================================================

TEST_CASE("Cache: read_cached_field field extraction", "[StorageService][cache]") {
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  MockNandStorage mock_nand;
  StorageService svc(cache, mock_nand);

  static constexpr uint16_t BUF = 16;
  float out[BUF] = {};

  SECTION("empty cache returns 0") { CHECK(svc.read_cached_field(CacheField::CO2, out, BUF) == 0); }

  SECTION("Temperature: valid readings oldest-first") {
    svc.cache_measurement(make_valid_entry(10)); // temp = 10.1
    svc.cache_measurement(make_valid_entry(20)); // temp = 20.1
    svc.cache_measurement(make_valid_entry(30)); // temp = 30.1

    const uint16_t n = svc.read_cached_field(CacheField::Temperature, out, BUF);

    REQUIRE(n == 3);
    CHECK(out[0] == Catch::Approx(10.1f));
    CHECK(out[1] == Catch::Approx(20.1f));
    CHECK(out[2] == Catch::Approx(30.1f));
  }

  SECTION("Temperature: invalid entry writes MeasuresInvalid::TEMPERATURE") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::Temperature, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(MeasuresInvalid::TEMPERATURE));
  }

  SECTION("Humidity: valid entry") {
    svc.cache_measurement(make_valid_entry(5)); // humidity = (5 % 90) + 5 = 10.0

    const uint16_t n = svc.read_cached_field(CacheField::Humidity, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(10.0f));
  }

  SECTION("Humidity: invalid entry writes MeasuresInvalid::HUMIDITY") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::Humidity, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(MeasuresInvalid::HUMIDITY));
  }

  SECTION("PM01: valid entry") {
    svc.cache_measurement(make_valid_entry(7)); // pm_01 = 8.0

    const uint16_t n = svc.read_cached_field(CacheField::PM01, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(8.0f));
  }

  SECTION("PM25: valid entry") {
    svc.cache_measurement(make_valid_entry(7)); // pm_25 = 9.0

    const uint16_t n = svc.read_cached_field(CacheField::PM25, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(9.0f));
  }

  SECTION("PM10: valid entry") {
    svc.cache_measurement(make_valid_entry(7)); // pm_10 = 10.0

    const uint16_t n = svc.read_cached_field(CacheField::PM10, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(10.0f));
  }

  SECTION("PM01: invalid entry writes MeasuresInvalid::PM") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::PM01, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(MeasuresInvalid::PM));
  }

  SECTION("CO2: valid entry cast to float") {
    svc.cache_measurement(make_valid_entry(10)); // co2 = 410

    const uint16_t n = svc.read_cached_field(CacheField::CO2, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(410.0f));
  }

  SECTION("CO2: invalid entry writes float(MeasuresInvalid::CO2)") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::CO2, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(static_cast<float>(MeasuresInvalid::CO2)));
  }

  SECTION("TvocIndex: valid entry cast to float") {
    svc.cache_measurement(make_valid_entry(10)); // tvoc_index = 20

    const uint16_t n = svc.read_cached_field(CacheField::TvocIndex, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(20.0f));
  }

  SECTION("TvocIndex: invalid entry writes float(MeasuresInvalid::TVOC)") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::TvocIndex, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(static_cast<float>(MeasuresInvalid::TVOC)));
  }

  SECTION("NoxIndex: valid entry cast to float") {
    svc.cache_measurement(make_valid_entry(10)); // nox_index = 30

    const uint16_t n = svc.read_cached_field(CacheField::NoxIndex, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(30.0f));
  }

  SECTION("NoxIndex: invalid entry writes float(MeasuresInvalid::NOX)") {
    svc.cache_measurement(make_invalid_entry());

    const uint16_t n = svc.read_cached_field(CacheField::NoxIndex, out, BUF);

    REQUIRE(n == 1);
    CHECK(out[0] == Catch::Approx(static_cast<float>(MeasuresInvalid::NOX)));
  }

  SECTION("mixed valid and invalid entries: each slot independently correct") {
    svc.cache_measurement(make_valid_entry(5));  // temp = 5.1
    svc.cache_measurement(make_invalid_entry()); // temp = MeasuresInvalid::TEMPERATURE
    svc.cache_measurement(make_valid_entry(15)); // temp = 15.1

    const uint16_t n = svc.read_cached_field(CacheField::Temperature, out, BUF);

    REQUIRE(n == 3);
    CHECK(out[0] == Catch::Approx(5.1f));
    CHECK(out[1] == Catch::Approx(MeasuresInvalid::TEMPERATURE));
    CHECK(out[2] == Catch::Approx(15.1f));
  }

  SECTION("max_count caps output to requested size") {
    for (int i = 0; i < 5; ++i) {
      svc.cache_measurement(make_valid_entry(i * 10));
    }

    const uint16_t n = svc.read_cached_field(CacheField::CO2, out, 3);

    REQUIRE(n == 3);
    // Only first 3 entries written — verify seed 0, 10, 20
    CHECK(out[0] == Catch::Approx(400.0f));
    CHECK(out[1] == Catch::Approx(410.0f));
    CHECK(out[2] == Catch::Approx(420.0f));
  }
}

// ============================================================================
// TEST CASE 2 — Route: init
// ============================================================================

TEST_CASE("Route: init", "[StorageService][route]") {
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  MockNandStorage mock_nand;
  StorageService svc(cache, mock_nand);

  SECTION("init succeeds when NAND mounts successfully") {
    REQUIRE_CALL(mock_nand, init()).RETURN(true);
    ALLOW_CALL(mock_nand, mount_path()).RETURN("/tmp");

    CHECK(svc.init() == true);
  }

  SECTION("init fails when NAND mount fails") {
    REQUIRE_CALL(mock_nand, init()).RETURN(false);

    CHECK(svc.init() == false);
  }
}

// ============================================================================
// TEST CASE 3 — Route: end_route edge cases
//
// The start / failure / already-active semantics are covered by the
// "Route: create_route / resume_route / route_file_exists" TEST_CASE
// further down.
// ============================================================================

TEST_CASE("Route: end_route edge cases", "[StorageService][route]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  SECTION("end_route closes file: is_route_active false, counts reset") {
    REQUIRE(svc.create_route(12345));
    svc.end_route();

    CHECK_FALSE(svc.is_route_active());
    CHECK(svc.current_route_point_count() == 0);
  }

  SECTION("end_route with no active route is a no-op") {
    CHECK_FALSE(svc.is_route_active());
    svc.end_route(); // must not crash
    CHECK_FALSE(svc.is_route_active());
  }
}

// ============================================================================
// TEST CASE 4 — Route: append_route_point
// ============================================================================

TEST_CASE("Route: append_route_point", "[StorageService][route]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  SECTION("append returns false when no route is active") {
    CHECK_FALSE(svc.append_route_point(make_route_point(0)));
  }

  SECTION("single append increments point count to 1") {
    REQUIRE(svc.create_route(11111));

    CHECK(svc.append_route_point(make_route_point(1)));
    CHECK(svc.current_route_point_count() == 1);

    svc.end_route();
  }

  SECTION("multiple appends accumulate the count correctly") {
    REQUIRE(svc.create_route(22222));

    for (int i = 0; i < 5; ++i) {
      REQUIRE(svc.append_route_point(make_route_point(i)));
      CHECK(svc.current_route_point_count() == static_cast<uint32_t>(i + 1));
    }

    svc.end_route();
  }

  SECTION("fix loss clears stored coordinates until a valid fix returns") {
    constexpr uint32_t SESSION_ID = 33333;
    RoutePoint first = make_route_point(7);
    first.gps.fix.fix_type = GpsFixType::Fix2D;
    RoutePoint lost = make_route_point(8);
    lost.gps.position = first.gps.position;
    lost.gps.altitude_m = first.gps.altitude_m;
    lost.gps.fix.fix_type = GpsFixType::NoFix;
    lost.gps.fix.satellite_count = 3;
    lost.battery_percentage = 75.0f;
    const RoutePoint regained = make_route_point(9);

    REQUIRE(svc.create_route(SESSION_ID));
    REQUIRE(svc.append_route_point(first));
    REQUIRE(svc.append_route_point(lost));
    REQUIRE(svc.append_route_point(regained));
    CHECK(svc.current_route_point_count() == 3);
    REQUIRE(svc.end_route());

    const std::string path = tmp / "routes/route_33333.bin";
    FILE *f = fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    RoutePoint points[3]{};
    const size_t count = fread(points, sizeof(RoutePoint), 3, f);
    fclose(f);
    REQUIRE(count == 3);

    CHECK(points[0].gps.fix.fix_type == GpsFixType::Fix2D);
    CHECK(points[0].gps.position.latitude == first.gps.position.latitude);
    CHECK(points[0].gps.position.longitude == first.gps.position.longitude);
    CHECK(points[0].gps.altitude_m == first.gps.altitude_m);

    CHECK(points[1].gps.fix.fix_type == GpsFixType::NoFix);
    CHECK(points[1].gps.position.latitude == GPS_LATITUDE_INVALID);
    CHECK(points[1].gps.position.longitude == GPS_LONGITUDE_INVALID);
    CHECK(points[1].gps.altitude_m == GPS_ALTITUDE_INVALID);
    CHECK(points[1].gps.fix.satellite_count == lost.gps.fix.satellite_count);
    CHECK(points[1].timestamp == lost.timestamp);
    CHECK(points[1].sensors.temp_hum_a.temperature == lost.sensors.temp_hum_a.temperature);
    CHECK(points[1].sensors.temp_hum_a.humidity == lost.sensors.temp_hum_a.humidity);
    CHECK(points[1].sensors.pm_a.pm_25 == lost.sensors.pm_a.pm_25);
    CHECK(points[1].sensors.co2.co2 == lost.sensors.co2.co2);
    CHECK(points[1].battery_percentage == lost.battery_percentage);

    CHECK(points[2].gps.fix.fix_type == GpsFixType::Fix3D);
    CHECK(points[2].gps.position.latitude == regained.gps.position.latitude);
    CHECK(points[2].gps.position.longitude == regained.gps.position.longitude);
    CHECK(points[2].gps.altitude_m == regained.gps.altitude_m);
    CHECK(lost.gps.position.latitude == first.gps.position.latitude);
    CHECK(lost.gps.position.longitude == first.gps.position.longitude);
    CHECK(lost.gps.altitude_m == first.gps.altitude_m);
  }

  SECTION("binary round-trip: written RoutePoints match field-by-field when read back") {
    constexpr uint32_t SESSION_ID = 33333;
    const RoutePoint p0 = make_route_point(7);
    const RoutePoint p1 = make_route_point(42);

    REQUIRE(svc.create_route(SESSION_ID));
    REQUIRE(svc.append_route_point(p0));
    REQUIRE(svc.append_route_point(p1));
    svc.end_route();

    // Reopen the file and read back the raw structs.
    const std::string path = tmp / "routes/route_33333.bin";
    FILE *f = fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);

    RoutePoint read0{}, read1{};
    REQUIRE(fread(&read0, sizeof(RoutePoint), 1, f) == 1);
    REQUIRE(fread(&read1, sizeof(RoutePoint), 1, f) == 1);
    fclose(f);

    CHECK(read0.timestamp == p0.timestamp);
    CHECK(read0.gps.position.latitude == Catch::Approx(p0.gps.position.latitude));
    CHECK(read0.gps.position.longitude == Catch::Approx(p0.gps.position.longitude));
    CHECK(read0.sensors.co2.co2 == p0.sensors.co2.co2);

    CHECK(read1.timestamp == p1.timestamp);
    CHECK(read1.gps.position.latitude == Catch::Approx(p1.gps.position.latitude));
    CHECK(read1.gps.position.longitude == Catch::Approx(p1.gps.position.longitude));
    CHECK(read1.sensors.co2.co2 == p1.sensors.co2.co2);
  }
}

// ============================================================================
// TEST CASE 5 — Route: sleep resume (resume_route restores file state)
// ============================================================================

TEST_CASE("Route: sleep resume", "[StorageService][route]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  SECTION("resume_route opens existing file in append mode; point count restored") {
    // First session: write 3 points.
    REQUIRE(svc.create_route(55555));
    for (int i = 0; i < 3; ++i) {
      REQUIRE(svc.append_route_point(make_route_point(i)));
    }
    svc.end_route();

    // Simulate wake: resume same session.
    REQUIRE(svc.resume_route(55555));

    CHECK(svc.current_route_point_count() == 3);

    svc.end_route();
  }

  SECTION("append after resume continues from restored count") {
    // First session: 3 points.
    REQUIRE(svc.create_route(66666));
    for (int i = 0; i < 3; ++i) {
      REQUIRE(svc.append_route_point(make_route_point(i)));
    }
    svc.end_route();

    // Resume and append 2 more.
    REQUIRE(svc.resume_route(66666));
    REQUIRE(svc.append_route_point(make_route_point(10)));
    REQUIRE(svc.append_route_point(make_route_point(11)));

    CHECK(svc.current_route_point_count() == 5);

    svc.end_route();
  }

  SECTION("full N+M cycle: file on disk contains N+M RoutePoints") {
    constexpr uint32_t SESSION_ID = 77777;
    const RoutePoint p0 = make_route_point(1);
    const RoutePoint p1 = make_route_point(2);
    const RoutePoint p2 = make_route_point(3); // written in second boot

    // First boot: write 2 points.
    REQUIRE(svc.create_route(SESSION_ID));
    REQUIRE(svc.append_route_point(p0));
    REQUIRE(svc.append_route_point(p1));
    svc.end_route();

    // Second boot (resume): write 1 more point.
    REQUIRE(svc.resume_route(SESSION_ID));
    REQUIRE(svc.append_route_point(p2));
    svc.end_route();

    // Verify file contains exactly 3 RoutePoints.
    const std::string path = tmp / "routes/route_77777.bin";
    FILE *f = fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);

    RoutePoint pts[3]{};
    REQUIRE(fread(pts, sizeof(RoutePoint), 3, f) == 3);
    // No fourth point should exist.
    RoutePoint extra{};
    CHECK(fread(&extra, sizeof(RoutePoint), 1, f) == 0);
    fclose(f);

    CHECK(pts[0].timestamp == p0.timestamp);
    CHECK(pts[1].timestamp == p1.timestamp);
    CHECK(pts[2].timestamp == p2.timestamp);
  }
}

// ============================================================================
// TEST CASE 6 - Clear data helpers
// ============================================================================

TEST_CASE("Storage clear helpers", "[StorageService][clear]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  SECTION("clear_cache removes all cached measurements") {
    svc.cache_measurement(make_valid_entry(1));
    svc.cache_measurement(make_valid_entry(2));
    REQUIRE(svc.cached_count() == 2);

    svc.clear_cache();

    CHECK(svc.cached_count() == 0);
  }

  SECTION("clear_routes deletes all files under the routes directory") {
    REQUIRE(svc.create_route(12345));
    REQUIRE(svc.append_route_point(make_route_point(1)));
    svc.end_route();

    REQUIRE(svc.create_route(23456));
    REQUIRE(svc.append_route_point(make_route_point(2)));
    svc.end_route();

    REQUIRE(std::filesystem::exists(tmp / "routes/route_12345.bin"));
    REQUIRE(std::filesystem::exists(tmp / "routes/route_23456.bin"));

    REQUIRE(svc.clear_routes());

    CHECK_FALSE(std::filesystem::exists(tmp / "routes/route_12345.bin"));
    CHECK_FALSE(std::filesystem::exists(tmp / "routes/route_23456.bin"));
  }

  SECTION("clear_routes succeeds when the routes directory does not exist") {
    CHECK(svc.clear_routes());
  }
}

// ============================================================================
// TEST CASE — Route: create_route / resume_route / route_file_exists
// ============================================================================

TEST_CASE("Route: create_route / resume_route / route_file_exists",
          "[StorageService][route][api-split]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  SECTION("create_route opens a fresh file when none exists") {
    CHECK_FALSE(svc.is_route_active());
    REQUIRE(svc.create_route(12345));

    CHECK(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 12345);
    CHECK(svc.current_route_point_count() == 0);
    CHECK(std::filesystem::exists(tmp / "routes/route_12345.bin"));

    svc.end_route();
  }

  SECTION("create_route refuses to truncate an existing file") {
    // Pre-populate a session file.
    REQUIRE(svc.create_route(33333));
    REQUIRE(svc.append_route_point(make_route_point(1)));
    svc.end_route();

    // Same session_id — create_route must refuse so the existing file is
    // not silently truncated. Post-condition: no route active, ids zero.
    CHECK_FALSE(svc.create_route(33333));
    CHECK_FALSE(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 0);
    CHECK(svc.current_route_point_count() == 0);

    // Existing file is intact (one full RoutePoint).
    const std::string path = tmp / "routes/route_33333.bin";
    struct stat st{};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK(static_cast<size_t>(st.st_size) == sizeof(RoutePoint));
  }

  SECTION("create_route returns false when NAND is not mounted") {
    MockNandStorage unmounted;
    REQUIRE_CALL(unmounted, is_mounted()).RETURN(false);
    StubPayloadCacheStorage stub2;
    PayloadCache cache2(stub2, 16);
    StorageService svc2(cache2, unmounted);

    CHECK_FALSE(svc2.create_route(12345));
    CHECK_FALSE(svc2.is_route_active());
    CHECK(svc2.current_route_session_id() == 0);
  }

  SECTION("create_route returns false when a route is already active and leaves it untouched") {
    REQUIRE(svc.create_route(11111));
    CHECK(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 11111);

    CHECK_FALSE(svc.create_route(22222));

    // Active route from the first call must be intact.
    CHECK(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 11111);

    svc.end_route();
  }

  SECTION("resume_route opens an existing file and restores the point count") {
    // Seed a session with three points.
    REQUIRE(svc.create_route(55555));
    for (int i = 0; i < 3; ++i) {
      REQUIRE(svc.append_route_point(make_route_point(i)));
    }
    svc.end_route();

    REQUIRE(svc.resume_route(55555));
    CHECK(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 55555);
    CHECK(svc.current_route_point_count() == 3);

    svc.end_route();
  }

  SECTION("resume_route fails when the file does not exist") {
    CHECK_FALSE(svc.resume_route(42424));
    CHECK_FALSE(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 0);
    CHECK(svc.current_route_point_count() == 0);
  }

  SECTION("resume_route returns false when a route is already active and leaves it untouched") {
    // Seed a file we could resume.
    REQUIRE(svc.create_route(55555));
    svc.end_route();

    // Open a different session, then try to resume — should refuse.
    REQUIRE(svc.create_route(66666));
    CHECK(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 66666);

    CHECK_FALSE(svc.resume_route(55555));

    CHECK(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 66666);

    svc.end_route();
  }

  SECTION("route_file_exists reports true after create_route + end_route") {
    CHECK_FALSE(svc.route_file_exists(77777));

    REQUIRE(svc.create_route(77777));
    svc.end_route();

    CHECK(svc.route_file_exists(77777));
    CHECK_FALSE(svc.route_file_exists(77778));
  }

  SECTION("route_file_exists returns false when NAND is not mounted") {
    MockNandStorage unmounted;
    REQUIRE_CALL(unmounted, is_mounted()).RETURN(false);
    StubPayloadCacheStorage stub2;
    PayloadCache cache2(stub2, 16);
    StorageService svc2(cache2, unmounted);

    CHECK_FALSE(svc2.route_file_exists(12345));
  }

  SECTION("resume_route truncates a torn trailing record and keeps the rest intact") {
    constexpr uint32_t SESSION_ID = 80808;
    const std::string path = tmp / "routes/route_80808.bin";

    // Seed the routes directory and lay down 3 full RoutePoints + a torn tail.
    REQUIRE(svc.create_route(SESSION_ID));
    REQUIRE(svc.append_route_point(make_route_point(1)));
    REQUIRE(svc.append_route_point(make_route_point(2)));
    REQUIRE(svc.append_route_point(make_route_point(3)));
    svc.end_route();

    // Append a half-record manually to simulate a power cut mid-fwrite.
    constexpr size_t TORN_BYTES = sizeof(RoutePoint) / 2;
    FILE *fp = fopen(path.c_str(), "ab");
    REQUIRE(fp != nullptr);
    std::vector<uint8_t> garbage(TORN_BYTES, 0xAA);
    REQUIRE(fwrite(garbage.data(), 1, TORN_BYTES, fp) == TORN_BYTES);
    fclose(fp);

    {
      struct stat st{};
      REQUIRE(::stat(path.c_str(), &st) == 0);
      REQUIRE(static_cast<size_t>(st.st_size) == 3 * sizeof(RoutePoint) + TORN_BYTES);
    }

    // Resume: the torn tail must be dropped to the nearest record boundary.
    REQUIRE(svc.resume_route(SESSION_ID));
    CHECK(svc.is_route_active());
    CHECK(svc.current_route_point_count() == 3);

    // The next append must land on a clean boundary — read the whole file
    // back and verify exactly 4 RoutePoints, no leftover torn bytes.
    REQUIRE(svc.append_route_point(make_route_point(4)));
    svc.end_route();

    struct stat st{};
    REQUIRE(::stat(path.c_str(), &st) == 0);
    CHECK(static_cast<size_t>(st.st_size) == 4 * sizeof(RoutePoint));
  }
}

// ============================================================================
// TEST CASE — Route: durability budget (via StorageTestSeam)
//
// stat() only sees FATFS cache state, not NAND, and libc auto-flushes
// confound size-based cadence assertions.  These tests use the
// `TEST_HOST`-only seam to verify fflush / fsync cadence directly via
// call counts.
// ============================================================================

TEST_CASE("Route: durability budget", "[StorageService][route][durability]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  // Each test owns its seam — reset() is implicit via the new instance.
  StorageTestSeam seam{};
  svc.set_test_seam(&seam);
  s_fake_rtos.set(0);

  SECTION("pause close reports sync failure but releases the route") {
    REQUIRE(svc.create_route(40001));
    seam.fsync_return = -1;
    CHECK_FALSE(svc.end_route());
    CHECK_FALSE(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 0);
    CHECK(svc.route_file_exists(40001));
    seam.fsync_return = 0;
    REQUIRE(svc.resume_route(40001));
    CHECK(svc.end_route());
    CHECK(svc.end_route());
  }

  SECTION("pause close reports fflush failure and allows a later resume") {
    REQUIRE(svc.create_route(40001));
    seam.fflush_return = -1;
    CHECK_FALSE(svc.end_route());
    CHECK_FALSE(svc.is_route_active());
    seam.fflush_return = 0;
    CHECK(svc.resume_route(40001));
    CHECK(svc.end_route());
  }

  SECTION("empty-file fsync: create_route flushes + fsyncs before returning") {
    REQUIRE(svc.create_route(40001));
    // Pre-append: counts must already reflect the create-time sync.
    CHECK(seam.fflush_count == 1);
    CHECK(seam.fsync_count == 1);

    svc.end_route();
  }

  SECTION("empty-file fsync failure leaves no half-open route") {
    seam.fsync_return = -1;

    CHECK_FALSE(svc.create_route(40002));

    CHECK_FALSE(svc.is_route_active());
    CHECK(svc.current_route_session_id() == 0);
    CHECK(svc.current_route_point_count() == 0);
  }

  SECTION("first-append fsync: first append after create increments counts again") {
    REQUIRE(svc.create_route(40003));
    REQUIRE(seam.fflush_count == 1);
    REQUIRE(seam.fsync_count == 1);

    // Inside the budget window — would normally not sync — but the
    // first-append guarantee (_last_fsync_ms = 0) forces it.
    REQUIRE(svc.append_route_point(make_route_point(1)));
    CHECK(seam.fflush_count == 2);
    CHECK(seam.fsync_count == 2);

    // A second append inside the same window must NOT cross the budget.
    REQUIRE(svc.append_route_point(make_route_point(2)));
    CHECK(seam.fflush_count == 2);
    CHECK(seam.fsync_count == 2);

    svc.end_route();
  }

  SECTION("first-append fsync for resume_route") {
    REQUIRE(svc.create_route(40004));
    REQUIRE(svc.append_route_point(make_route_point(1)));
    svc.end_route();

    // Reset counts to isolate post-resume behaviour from the seed session.
    seam = {};
    svc.set_test_seam(&seam);

    REQUIRE(svc.resume_route(40004));
    // resume_route() does not sync on open.
    CHECK(seam.fflush_count == 0);
    CHECK(seam.fsync_count == 0);

    // First post-resume append crosses the budget unconditionally.
    REQUIRE(svc.append_route_point(make_route_point(2)));
    CHECK(seam.fflush_count == 1);
    CHECK(seam.fsync_count == 1);

    svc.end_route();
  }

  SECTION("fsync cadence: appends within one budget window do not re-sync; "
          "next sync fires after the window") {
    REQUIRE(svc.create_route(40005));
    // Seed: empty-file sync (counts == 1) + first-append sync (counts == 2).
    REQUIRE(svc.append_route_point(make_route_point(1)));
    REQUIRE(seam.fsync_count == 2);

    // N further appends inside the same window — no new syncs.
    for (int i = 0; i < 5; ++i) {
      s_fake_rtos.advance(CONFIG_TRACKING_FSYNC_INTERVAL_MS / 10); // well under budget
      REQUIRE(svc.append_route_point(make_route_point(i + 10)));
    }
    CHECK(seam.fsync_count == 2);

    // Cross the window — next append must sync.
    s_fake_rtos.advance(CONFIG_TRACKING_FSYNC_INTERVAL_MS);
    REQUIRE(svc.append_route_point(make_route_point(99)));
    CHECK(seam.fflush_count == 3);
    CHECK(seam.fsync_count == 3);

    svc.end_route();
  }

  SECTION("fsync failure: append returns false AND the anchor is preserved") {
    REQUIRE(svc.create_route(40006));
    // Burn the first-append sync.
    REQUIRE(svc.append_route_point(make_route_point(1)));
    REQUIRE(seam.fsync_count == 2);

    // Advance past the budget so the next append crosses the threshold,
    // then make the sync fail.
    s_fake_rtos.advance(CONFIG_TRACKING_FSYNC_INTERVAL_MS + 100);
    seam.fsync_return = -1;
    CHECK_FALSE(svc.append_route_point(make_route_point(2)));

    // The anchor must NOT have advanced — the next budget crossing
    // should still fire as if no prior sync occurred. Re-arm by
    // clearing the failure, advance the clock again, and observe.
    seam.fsync_return = 0;
    s_fake_rtos.advance(CONFIG_TRACKING_FSYNC_INTERVAL_MS + 100);
    REQUIRE(svc.append_route_point(make_route_point(3)));
    // first-append (1) + create-time (1) + failed sync (1) + recovery (1)
    // = 4 fsync calls.
    CHECK(seam.fsync_count == 4);

    svc.end_route();
  }

  SECTION("update-on-success: _last_fsync_ms advances to virtual now on success") {
    REQUIRE(svc.create_route(40007));
    REQUIRE(svc.append_route_point(make_route_point(1)));
    const int after_first_sync = seam.fsync_count;

    // Cross the budget twice in a row and verify the second crossing
    // *does* sync (i.e. the anchor was updated to the moment of the
    // first crossing, not left at zero).
    s_fake_rtos.advance(CONFIG_TRACKING_FSYNC_INTERVAL_MS + 100);
    REQUIRE(svc.append_route_point(make_route_point(2)));
    CHECK(seam.fsync_count == after_first_sync + 1);

    // Inside one window of the previous sync — must NOT sync.
    s_fake_rtos.advance(CONFIG_TRACKING_FSYNC_INTERVAL_MS / 4);
    REQUIRE(svc.append_route_point(make_route_point(3)));
    CHECK(seam.fsync_count == after_first_sync + 1);

    svc.end_route();
  }

  SECTION("end_route always flushes + syncs regardless of budget state") {
    REQUIRE(svc.create_route(40008));
    REQUIRE(svc.append_route_point(make_route_point(1)));
    const int before_end_flush = seam.fflush_count;
    const int before_end_sync = seam.fsync_count;

    svc.end_route();

    CHECK(seam.fflush_count == before_end_flush + 1);
    CHECK(seam.fsync_count == before_end_sync + 1);
  }
}

// ============================================================================
// TEST CASE 7 — Route: session_count and list_sessions
// ============================================================================

TEST_CASE("Route: session_count and list_sessions", "[StorageService][route]") {
  TempDir tmp;
  StubPayloadCacheStorage stub_storage;
  PayloadCache cache(stub_storage, 16);
  FakeNandStorage fake_nand(tmp.path);
  StorageService svc(cache, fake_nand);

  SECTION("session_count returns 0 when no routes directory exists") {
    CHECK(svc.session_count() == 0);
  }

  SECTION("session_count returns 0 when routes directory is empty") {
    // Create the routes directory but no files
    std::filesystem::create_directories(tmp / "routes");
    CHECK(svc.session_count() == 0);
  }

  SECTION("session_count returns correct count after creating sessions") {
    REQUIRE(svc.create_route(10001));
    REQUIRE(svc.append_route_point(make_route_point(1)));
    svc.end_route();

    REQUIRE(svc.create_route(10002));
    REQUIRE(svc.append_route_point(make_route_point(2)));
    svc.end_route();

    REQUIRE(svc.create_route(10003));
    REQUIRE(svc.append_route_point(make_route_point(3)));
    svc.end_route();

    CHECK(svc.session_count() == 3);
  }

  SECTION("session_count returns 0 when NAND is not mounted") {
    MockNandStorage unmounted_nand;
    REQUIRE_CALL(unmounted_nand, is_mounted()).RETURN(false);
    StubPayloadCacheStorage stub2;
    PayloadCache cache2(stub2, 16);
    StorageService svc2(cache2, unmounted_nand);

    CHECK(svc2.session_count() == 0);
  }

  SECTION("list_sessions returns sorted session IDs") {
    // Create sessions in non-sorted order
    REQUIRE(svc.create_route(10003));
    svc.end_route();
    REQUIRE(svc.create_route(10001));
    svc.end_route();
    REQUIRE(svc.create_route(10002));
    svc.end_route();

    uint32_t ids[4] = {};
    uint16_t count = svc.list_sessions(ids, 4);

    REQUIRE(count == 3);
    CHECK(ids[0] == 10001);
    CHECK(ids[1] == 10002);
    CHECK(ids[2] == 10003);
  }

  SECTION("list_sessions respects max_count") {
    REQUIRE(svc.create_route(10001));
    svc.end_route();
    REQUIRE(svc.create_route(10002));
    svc.end_route();
    REQUIRE(svc.create_route(10003));
    svc.end_route();

    uint32_t ids[2] = {};
    uint16_t count = svc.list_sessions(ids, 2);

    // Should return at most 2 (max_count), though order within the
    // directory scan before sorting is filesystem-dependent
    CHECK(count == 2);
  }

  SECTION("list_sessions returns 0 with nullptr output") {
    REQUIRE(svc.create_route(10001));
    svc.end_route();

    CHECK(svc.list_sessions(nullptr, 10) == 0);
  }

  SECTION("list_sessions returns 0 with zero max_count") {
    REQUIRE(svc.create_route(10001));
    svc.end_route();

    uint32_t ids[1] = {};
    CHECK(svc.list_sessions(ids, 0) == 0);
  }

  SECTION("session_count and list_sessions are consistent") {
    REQUIRE(svc.create_route(10001));
    REQUIRE(svc.append_route_point(make_route_point(1)));
    svc.end_route();

    REQUIRE(svc.create_route(10002));
    REQUIRE(svc.append_route_point(make_route_point(2)));
    svc.end_route();

    uint16_t count = svc.session_count();
    uint32_t ids[10] = {};
    uint16_t listed = svc.list_sessions(ids, 10);

    CHECK(count == listed);
    CHECK(count == 2);
  }

  SECTION("session_count updates after deleting a session") {
    REQUIRE(svc.create_route(10001));
    REQUIRE(svc.append_route_point(make_route_point(1)));
    svc.end_route();

    REQUIRE(svc.create_route(10002));
    REQUIRE(svc.append_route_point(make_route_point(2)));
    svc.end_route();

    CHECK(svc.session_count() == 2);

    REQUIRE(svc.delete_route(10001));

    CHECK(svc.session_count() == 1);

    // Remaining session should be 10002
    uint32_t ids[2] = {};
    uint16_t listed = svc.list_sessions(ids, 2);
    REQUIRE(listed == 1);
    CHECK(ids[0] == 10002);
  }

  SECTION("session_count updates after clear_routes") {
    REQUIRE(svc.create_route(10001));
    svc.end_route();
    REQUIRE(svc.create_route(10002));
    svc.end_route();

    CHECK(svc.session_count() == 2);

    REQUIRE(svc.clear_routes());

    CHECK(svc.session_count() == 0);
  }
}
