/**
 * AirGradient Go — BLE Service tests
 *
 * Tests CBOR encoding, wire format conversion, pending write buffers,
 * notification flow, connection lifecycle, history download, and string
 * mapping through the BleServiceTestAccess friend class.
 */

#include "go_ble.h"
#include "go_ble_protocol.h"
#include "go_storage.h"
#include "hal/ble_server.h"

#include <cbor.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

// ===========================================================================
// Mock BLE types
// ===========================================================================

/// Captures set_value() data and notify() calls for test assertions.
class MockBleCharacteristic : public AgBleCharacteristic {
public:
  bool set_value(const uint8_t *data, size_t len) override {
    last_value.assign(data, data + len);
    all_values.push_back(last_value);
    set_value_count++;
    return true;
  }

  bool notify() override {
    // No-arg notify records the current stored value as the notified payload.
    last_notified_value = last_value;
    notify_count++;
    return notify_returns;
  }

  bool notify(const uint8_t *data, size_t len) override {
    // Records the supplied buffer; the stored (READ) value is left unchanged.
    last_notified_value.assign(data, data + len);
    notify_count++;
    return notify_returns;
  }

  void set_write_callback(AgBleWriteCallback callback) override { write_cb = callback; }

  // --- Test inspection ---
  std::vector<uint8_t> last_value;
  std::vector<std::vector<uint8_t>> all_values;
  std::vector<uint8_t> last_notified_value;
  int set_value_count = 0;
  int notify_count = 0;
  bool notify_returns = true;
  AgBleWriteCallback write_cb;

  void reset() {
    last_value.clear();
    all_values.clear();
    last_notified_value.clear();
    set_value_count = 0;
    notify_count = 0;
    notify_returns = true;
  }
};

/// Captures advertising calls for connection lifecycle tests.
class MockBleServer : public AgBleServer {
public:
  bool init(const char * /*device_name*/) override { return true; }
  void deinit() override {}
  bool set_security(AgBleIoCapability io_cap, uint8_t auth_flags) override {
    last_io_cap = io_cap;
    last_auth_flags = auth_flags;
    set_security_count++;
    return true;
  }
  bool delete_all_bonds() override {
    delete_all_bonds_count++;
    return delete_all_bonds_result;
  }
  AgBleGattService *add_service(const char * /*uuid*/) override { return nullptr; }
  bool set_advertising_name(const char * /*name*/) override { return true; }
  bool add_advertised_service_uuid(const char * /*uuid*/) override { return true; }
  bool set_manufacturer_data(const uint8_t * /*data*/, size_t /*len*/) override { return true; }

  bool start_advertising() override {
    start_advertising_count++;
    return true;
  }

  bool stop_advertising() override {
    stop_advertising_count++;
    return true;
  }

  void set_connect_callback(AgBleConnectCallback /*cb*/) override {}
  void set_disconnect_callback(AgBleDisconnectCallback /*cb*/) override {}
  void set_passkey_display_callback(AgBlePasskeyDisplayCallback /*cb*/) override {}
  void set_numeric_comparison_callback(AgBleNumericComparisonCallback /*cb*/) override {}
  bool confirm_numeric_comparison(uint16_t conn_handle, bool accept) override {
    last_numeric_comparison_handle = conn_handle;
    last_numeric_comparison_accept = accept;
    confirm_numeric_comparison_count++;
    return true;
  }
  void set_auth_complete_callback(AgBleAuthCompleteCallback /*cb*/) override {}
  uint8_t connected_client_count() const override { return connected_count; }
  bool is_peer_authenticated() const override { return peer_authenticated; }

  // --- Test inspection ---
  int start_advertising_count = 0;
  int stop_advertising_count = 0;
  int delete_all_bonds_count = 0;
  bool delete_all_bonds_result = true;
  bool peer_authenticated = false;
  uint8_t connected_count = 0;
  int set_security_count = 0;
  AgBleIoCapability last_io_cap = AgBleIoCapability::NO_INPUT_NO_OUTPUT;
  uint8_t last_auth_flags = 0;
  int confirm_numeric_comparison_count = 0;
  uint16_t last_numeric_comparison_handle = 0;
  bool last_numeric_comparison_accept = false;

  void reset() {
    start_advertising_count = 0;
    stop_advertising_count = 0;
    delete_all_bonds_count = 0;
    delete_all_bonds_result = true;
    peer_authenticated = false;
    connected_count = 0;
    set_security_count = 0;
    last_auth_flags = 0;
    confirm_numeric_comparison_count = 0;
  }
};

// ===========================================================================
// Storage spy — controllable stub data for history tests
// ===========================================================================

namespace storage_spy {

struct SessionEntry {
  uint32_t id;
  uint32_t point_count;
  time_t start_time;
};

static std::vector<SessionEntry> sessions;
static std::vector<RoutePoint> points;
static uint32_t total_capacity_kb = 0;
static uint32_t used_kb = 0;
static bool delete_route_returns = true;
static uint32_t last_deleted_session_id = 0;

static void reset() {
  sessions.clear();
  points.clear();
  total_capacity_kb = 0;
  used_kb = 0;
  delete_route_returns = true;
  last_deleted_session_id = 0;
}

} // namespace storage_spy

// ===========================================================================
// StorageService stubs (link-time replacements)
// ===========================================================================

// Stubs for methods that go_ble.cpp doesn't call but must link
StorageService::StorageService(PayloadCache &cache, NandStorage &nand)
    : _cache(cache), _nand(nand) {}
bool StorageService::init() { return true; }
void StorageService::cache_measurement(const MeasuresAGo & /*m*/) {}
void StorageService::clear_cache() {}
uint16_t StorageService::read_cached_field(CacheField /*f*/, float * /*out*/,
                                           uint16_t /*max*/) const {
  return 0;
}
uint16_t StorageService::read_cache(MeasuresAGo * /*out*/, uint16_t /*max*/) const { return 0; }
uint16_t StorageService::cached_count() const { return 0; }
void StorageService::backup_cache() const {}
void StorageService::restore_cache() {}
bool StorageService::create_route(uint32_t /*session_id*/) { return true; }
bool StorageService::resume_route(uint32_t /*session_id*/) { return true; }
bool StorageService::route_file_exists(uint32_t /*session_id*/) const { return false; }
bool StorageService::append_route_point(const RoutePoint & /*point*/) { return true; }
void StorageService::end_route() {}
bool StorageService::is_route_active() const { return false; }
uint32_t StorageService::current_route_point_count() const { return 0; }
bool StorageService::delete_route(uint32_t session_id) {
  storage_spy::last_deleted_session_id = session_id;
  return storage_spy::delete_route_returns;
}
uint32_t StorageService::current_route_session_id() const { return 0; }
bool StorageService::clear_routes() { return true; }
bool StorageService::ensure_route_dir() const { return true; }

// History read stubs — controlled by storage_spy
uint16_t StorageService::session_count() const {
  return static_cast<uint16_t>(storage_spy::sessions.size());
}

uint16_t StorageService::list_sessions(uint32_t *out, uint16_t max_count) const {
  uint16_t count = 0;
  for (size_t i = 0; i < storage_spy::sessions.size() && count < max_count; i++) {
    out[count++] = storage_spy::sessions[i].id;
  }
  return count;
}

uint32_t StorageService::get_session_point_count(uint32_t session_id) const {
  for (const auto &s : storage_spy::sessions) {
    if (s.id == session_id) {
      return s.point_count;
    }
  }
  return 0;
}

uint16_t StorageService::read_route_points(uint32_t /*session_id*/, uint32_t offset,
                                           RoutePoint *out, uint16_t count) const {
  uint16_t read = 0;
  for (uint16_t i = 0; i < count && (offset + i) < storage_spy::points.size(); i++) {
    out[i] = storage_spy::points[offset + i];
    read++;
  }
  return read;
}

time_t StorageService::get_session_start_time(uint32_t session_id) const {
  for (const auto &s : storage_spy::sessions) {
    if (s.id == session_id) {
      return s.start_time;
    }
  }
  return 0;
}

uint32_t StorageService::total_capacity_kb() const { return storage_spy::total_capacity_kb; }

uint32_t StorageService::used_kb() const { return storage_spy::used_kb; }

// ===========================================================================
// BleServiceTestAccess — friend class for private member access
// ===========================================================================

class BleServiceTestAccess {
public:
  // --- State setters ---
  static void set_server(BleService &svc, AgBleServer *server) { svc._server = server; }
  static void set_connected(BleService &svc, bool connected) {
    svc._connected_client_count.store(connected ? 1 : 0);
  }
  static void set_initialized(BleService &svc, bool initialized) { svc._initialized = initialized; }
  static void set_measures_char(BleService &svc, AgBleCharacteristic *c) { svc._measures_char = c; }
  static void set_status_char(BleService &svc, AgBleCharacteristic *c) { svc._status_char = c; }
  static void set_config_char(BleService &svc, AgBleCharacteristic *c) { svc._config_char = c; }
  static void set_history_char(BleService &svc, AgBleCharacteristic *c) { svc._history_char = c; }
  static void set_export_active(BleService &svc, bool active) { svc._export_active = active; }
  static void set_export_session_id(BleService &svc, uint32_t id) { svc._export_session_id = id; }

  // --- Private method access ---
  static void on_connect(BleService &svc, uint16_t h) { svc.on_connect(h); }
  static void on_disconnect(BleService &svc, uint16_t h, int r) { svc.on_disconnect(h, r); }
  static void on_config_write(BleService &svc, const uint8_t *d, size_t l) {
    svc.on_config_write(d, l);
  }
  static void on_history_write(BleService &svc, const uint8_t *d, size_t l) {
    svc.on_history_write(d, l);
  }

  static size_t encode_measures(BleService &svc, uint8_t *buf, size_t sz, const MeasuresAGo &m,
                                const GpsData &gps, time_t ts) {
    return svc.encode_measures(buf, sz, m, gps, ts);
  }
  static size_t encode_status(BleService &svc, uint8_t *buf, size_t sz, const PowerSnapshot &p,
                              const GpsData &gps, bool tracking, uint32_t session_id) {
    return svc.encode_status(buf, sz, p, gps, tracking, session_id);
  }
  static size_t encode_config(BleService &svc, uint8_t *buf, size_t sz, const GoSettings &s) {
    return svc.encode_config(buf, sz, s);
  }
  static size_t encode_config_delta(BleService &svc, uint8_t *buf, size_t sz,
                                    const GoSettings &prev, const GoSettings &cur) {
    return svc.encode_config_delta(buf, sz, prev, cur);
  }
  static size_t encode_status_transition(BleService &svc, uint8_t *buf, size_t sz, bool tracking,
                                         uint32_t session_id) {
    return svc.encode_status_transition(buf, sz, tracking, session_id);
  }
  static size_t encode_status_charging(BleService &svc, uint8_t *buf, size_t sz,
                                       const PowerSnapshot &p) {
    return svc.encode_status_charging(buf, sz, p);
  }
  static size_t encode_status_disc(BleService &svc, uint8_t *buf, size_t sz, BleDiscReason r) {
    return svc.encode_status_disc(buf, sz, r);
  }
  static const char *disc_reason_to_str(BleDiscReason r) {
    return BleService::disc_reason_to_str(r);
  }

  static void route_point_to_wire(const RoutePoint &point, uint8_t *out) {
    BleService::route_point_to_wire(point, out);
  }

  static const char *charging_state_to_str(BmsChargingState state) {
    return BleService::charging_state_to_str(state);
  }
  static const char *gps_mode_to_str(GpsMode mode) { return BleService::gps_mode_to_str(mode); }
  static const char *operating_mode_to_str(OperatingMode mode) {
    return BleService::operating_mode_to_str(mode);
  }
  static uint16_t measures_properties() { return BleService::measures_properties(); }
  static uint16_t status_properties() { return BleService::status_properties(); }
  static uint16_t config_properties() { return BleService::config_properties(); }
  static uint16_t history_properties() { return BleService::history_properties(); }

  // --- State readers ---
  static bool export_active(const BleService &svc) { return svc._export_active; }
  static uint32_t export_session_id(const BleService &svc) { return svc._export_session_id; }
};

// ===========================================================================
// CBOR decode helpers for test assertions
// ===========================================================================

/// Decode a CBOR map from a buffer. Returns key-value pairs as string->CborValue.
/// Only supports top-level flat maps. Asserts on decode errors.
struct CborMapEntry {
  std::string key;
  CborType type;
  // Decoded value — use whichever field matches .type
  int64_t int_val = 0;
  uint64_t uint_val = 0;
  double float_val = 0.0;
  bool bool_val = false;
  std::string text_val;
};

static std::vector<CborMapEntry> decode_cbor_map(const uint8_t *data, size_t len) {
  std::vector<CborMapEntry> entries;
  CborParser parser;
  CborValue it;
  REQUIRE(cbor_parser_init(data, len, 0, &parser, &it) == CborNoError);
  REQUIRE(cbor_value_is_map(&it));

  CborValue map;
  REQUIRE(cbor_value_enter_container(&it, &map) == CborNoError);

  while (!cbor_value_at_end(&map)) {
    CborMapEntry entry;

    // Key must be text
    REQUIRE(cbor_value_is_text_string(&map));
    size_t key_len = 0;
    cbor_value_get_string_length(&map, &key_len);
    entry.key.resize(key_len);
    cbor_value_copy_text_string(&map, entry.key.data(), &key_len, &map);

    // Value
    entry.type = cbor_value_get_type(&map);
    if (cbor_value_is_unsigned_integer(&map)) {
      cbor_value_get_uint64(&map, &entry.uint_val);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_integer(&map)) {
      cbor_value_get_int64(&map, &entry.int_val);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_float(&map)) {
      float f;
      cbor_value_get_float(&map, &f);
      entry.float_val = static_cast<double>(f);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_double(&map)) {
      cbor_value_get_double(&map, &entry.float_val);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_boolean(&map)) {
      cbor_value_get_boolean(&map, &entry.bool_val);
      cbor_value_advance_fixed(&map);
    } else if (cbor_value_is_text_string(&map)) {
      size_t val_len = 0;
      cbor_value_get_string_length(&map, &val_len);
      entry.text_val.resize(val_len);
      cbor_value_copy_text_string(&map, entry.text_val.data(), &val_len, &map);
    } else {
      // Skip unknown types
      cbor_value_advance(&map);
    }

    entries.push_back(entry);
  }

  return entries;
}

static const CborMapEntry *find_entry(const std::vector<CborMapEntry> &entries,
                                      const std::string &key) {
  for (const auto &e : entries) {
    if (e.key == key) {
      return &e;
    }
  }
  return nullptr;
}

static bool top_level_value_is_map(const uint8_t *data, size_t len, const char *target_key) {
  CborParser parser;
  CborValue root;
  REQUIRE(cbor_parser_init(data, len, 0, &parser, &root) == CborNoError);
  REQUIRE(cbor_value_is_map(&root));

  CborValue map;
  REQUIRE(cbor_value_enter_container(&root, &map) == CborNoError);
  while (!cbor_value_at_end(&map)) {
    REQUIRE(cbor_value_is_text_string(&map));
    size_t key_len = 0;
    cbor_value_get_string_length(&map, &key_len);
    std::string key(key_len, '\0');
    cbor_value_copy_text_string(&map, key.data(), &key_len, &map);

    if (key == target_key) {
      return cbor_value_is_map(&map);
    }
    cbor_value_advance(&map);
  }
  return false;
}

// Conservative single-PDU budget (mirrors BLE_NOTIFY_MAX_BYTES in go_ble.cpp);
// the 185-byte minimum MTU yields a 182-byte PDU, so 180 is the test bound.
static constexpr size_t TEST_NOTIFY_BUDGET = 180;
static constexpr int TEST_MAX_INACTIVITY_TIMEOUT_SECONDS = 600;
static constexpr int TEST_MAX_AUTO_LOCK_SECONDS = 60;

// ===========================================================================
// Test fixture helpers
// ===========================================================================

/// Null-backed PayloadCache and NandStorage for StorageService construction.
/// StorageService methods are all stubbed, so these are never used.
static PayloadCache *null_cache_ptr = nullptr;
static NandStorage *null_nand_ptr = nullptr;

/// Default BLE server for BleService construction.  Tests that need a live
/// mock for advertising assertions construct their own MockBleServer and
/// inject it via BleServiceTestAccess::set_server().
static MockBleServer default_ble_server;

/// Helper: constructs a valid MeasuresAGo with all sensors reading valid data.
static MeasuresAGo make_valid_measures() {
  MeasuresAGo m{};
  m.temp_hum_a.temperature = 23.5f;
  m.temp_hum_a.humidity = 45.2f;
  m.pm_a.pm_01 = 5.0f;
  m.pm_a.pm_25 = 8.3f;
  m.pm_a.pm_10 = 12.1f;
  m.co2.co2 = 450;
  m.tvoc_nox.tvoc_index = 120;
  m.tvoc_nox.nox_index = 5;
  m.pressure.pressure = 1013.2f;
  return m;
}

/// Helper: constructs a valid GpsData with a 3D fix.
static GpsData make_valid_gps() {
  GpsData gps{};
  gps.position.latitude = 47.376887;
  gps.position.longitude = 8.541694;
  gps.altitude_m = 408.0f;
  gps.fix.fix_type = GpsFixType::Fix3D;
  gps.fix.satellite_count = 12;
  return gps;
}

/// Helper: constructs default GoSettings.
static GoSettings make_default_settings() { return GoSettings{}; }

/// Helper: constructs a PowerSnapshot with valid data.
static PowerSnapshot make_valid_power() {
  PowerSnapshot p{};
  p.battery_voltage = 3.85f;
  p.battery_percentage = 72.0f;
  p.charging_status = BmsChargingState::NotCharging;
  return p;
}

// ===========================================================================
// Tests
// ===========================================================================

TEST_CASE("BLE: state queries default values") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  CHECK(svc.is_initialized() == false);
  CHECK(svc.is_connected() == false);
}

TEST_CASE("BLE: characteristics always require authenticated access") {
  const uint16_t measures_props = BleServiceTestAccess::measures_properties();
  const uint16_t status_props = BleServiceTestAccess::status_properties();
  const uint16_t config_props = BleServiceTestAccess::config_properties();
  const uint16_t history_props = BleServiceTestAccess::history_properties();

  CHECK((measures_props & (AgBleProperty::READ | AgBleProperty::NOTIFY)) ==
        (AgBleProperty::READ | AgBleProperty::NOTIFY));
  CHECK((status_props & (AgBleProperty::READ | AgBleProperty::NOTIFY)) ==
        (AgBleProperty::READ | AgBleProperty::NOTIFY));
  CHECK((config_props & (AgBleProperty::READ | AgBleProperty::WRITE | AgBleProperty::NOTIFY)) ==
        (AgBleProperty::READ | AgBleProperty::WRITE | AgBleProperty::NOTIFY));
  CHECK((history_props & (AgBleProperty::WRITE | AgBleProperty::NOTIFY)) ==
        (AgBleProperty::WRITE | AgBleProperty::NOTIFY));

  // Security is mandatory: every characteristic gates access behind pairing.
  CHECK((measures_props & AgBleProperty::READ_AUTHEN) != 0);
  CHECK((status_props & AgBleProperty::READ_AUTHEN) != 0);
  CHECK((config_props & AgBleProperty::READ_AUTHEN) != 0);
  CHECK((config_props & AgBleProperty::WRITE_AUTHEN) != 0);
  CHECK((history_props & AgBleProperty::WRITE_AUTHEN) != 0);
}

// ---------------------------------------------------------------------------
// Pending write buffers
// ---------------------------------------------------------------------------

TEST_CASE("BLE: take_pending_config_write returns 0 when no data pending") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  uint8_t buf[256];
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_config_write stores data and take retrieves it") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  uint8_t write_data[] = {0xA1, 0x62, 0x6F, 0x70};
  BleServiceTestAccess::on_config_write(svc, write_data, sizeof(write_data));

  uint8_t buf[256];
  size_t len = svc.take_pending_config_write(buf, sizeof(buf));
  REQUIRE(len == sizeof(write_data));
  CHECK(memcmp(buf, write_data, len) == 0);

  // Second call returns 0 (cleared)
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_config_write rejects null data") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  BleServiceTestAccess::on_config_write(svc, nullptr, 4);

  uint8_t buf[256];
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_config_write rejects zero length") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  uint8_t data[] = {0x01};
  BleServiceTestAccess::on_config_write(svc, data, 0);

  uint8_t buf[256];
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_config_write rejects oversized data") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  uint8_t data[257];
  memset(data, 0xAA, sizeof(data));
  BleServiceTestAccess::on_config_write(svc, data, sizeof(data));

  uint8_t buf[257];
  CHECK(svc.take_pending_config_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: take_pending_config_write truncates to buffer size") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  uint8_t write_data[16];
  for (size_t i = 0; i < sizeof(write_data); i++) {
    write_data[i] = static_cast<uint8_t>(i);
  }
  BleServiceTestAccess::on_config_write(svc, write_data, sizeof(write_data));

  uint8_t buf[4];
  size_t len = svc.take_pending_config_write(buf, sizeof(buf));
  REQUIRE(len == 4);
  CHECK(buf[0] == 0x00);
  CHECK(buf[3] == 0x03);
}

TEST_CASE("BLE: on_history_write stores data and take retrieves it") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  uint8_t write_data[] = {0xB1, 0x22, 0x33};
  BleServiceTestAccess::on_history_write(svc, write_data, sizeof(write_data));

  uint8_t buf[256];
  size_t len = svc.take_pending_history_write(buf, sizeof(buf));
  REQUIRE(len == sizeof(write_data));
  CHECK(memcmp(buf, write_data, len) == 0);

  // Cleared after retrieval
  CHECK(svc.take_pending_history_write(buf, sizeof(buf)) == 0);
}

TEST_CASE("BLE: on_history_write rejects invalid input") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  // null data
  BleServiceTestAccess::on_history_write(svc, nullptr, 4);
  uint8_t buf[256];
  CHECK(svc.take_pending_history_write(buf, sizeof(buf)) == 0);

  // zero length
  uint8_t data[] = {0x01};
  BleServiceTestAccess::on_history_write(svc, data, 0);
  CHECK(svc.take_pending_history_write(buf, sizeof(buf)) == 0);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Measures
// ---------------------------------------------------------------------------

TEST_CASE("BLE: encode_measures with all sensors valid and GPS fix") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  auto m = make_valid_measures();
  auto gps = make_valid_gps();
  time_t ts = 1711234567;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_measures(svc, buf, sizeof(buf), m, gps, ts);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);

  // All sensor fields present
  CHECK(find_entry(entries, "t") != nullptr);
  CHECK(find_entry(entries, "h") != nullptr);
  CHECK(find_entry(entries, "pm1") != nullptr);
  CHECK(find_entry(entries, "pm25") != nullptr);
  CHECK(find_entry(entries, "pm10") != nullptr);
  CHECK(find_entry(entries, "co2") != nullptr);
  CHECK(find_entry(entries, "tvoc") != nullptr);
  CHECK(find_entry(entries, "nox") != nullptr);
  CHECK(find_entry(entries, "pres") != nullptr);

  // GPS fields present
  CHECK(find_entry(entries, "lat") != nullptr);
  CHECK(find_entry(entries, "lon") != nullptr);
  CHECK(find_entry(entries, "alt") != nullptr);
  CHECK(find_entry(entries, "fix") != nullptr);
  CHECK(find_entry(entries, "sat") != nullptr);

  // Timestamp always present
  auto *ts_entry = find_entry(entries, "ts");
  REQUIRE(ts_entry != nullptr);
  CHECK(ts_entry->uint_val == 1711234567);

  // Spot-check values
  CHECK_THAT(find_entry(entries, "t")->float_val, Catch::Matchers::WithinAbs(23.5, 0.1));
  CHECK(find_entry(entries, "co2")->uint_val == 450);
  CHECK_THAT(find_entry(entries, "lat")->float_val, Catch::Matchers::WithinAbs(47.376887, 0.0001));
  CHECK(find_entry(entries, "fix")->uint_val == 3);
  CHECK(find_entry(entries, "sat")->uint_val == 12);
}

TEST_CASE("BLE: encode_measures without GPS fix omits GPS keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  auto m = make_valid_measures();
  GpsData gps{}; // default = NoFix
  time_t ts = 1711234567;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_measures(svc, buf, sizeof(buf), m, gps, ts);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);

  CHECK(find_entry(entries, "lat") == nullptr);
  CHECK(find_entry(entries, "lon") == nullptr);
  CHECK(find_entry(entries, "alt") == nullptr);
  CHECK(find_entry(entries, "fix") == nullptr);
  CHECK(find_entry(entries, "sat") == nullptr);

  // Sensor fields still present
  CHECK(find_entry(entries, "t") != nullptr);
  CHECK(find_entry(entries, "ts") != nullptr);
}

TEST_CASE("BLE: encode_measures omits invalid sensor fields") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  MeasuresAGo m{};
  // Only temperature is valid
  m.temp_hum_a.temperature = 22.0f;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.co2.co2 = MeasuresInvalid::CO2;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.pressure.pressure = MeasuresInvalid::PRESSURE;

  GpsData gps{};
  time_t ts = 100;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_measures(svc, buf, sizeof(buf), m, gps, ts);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);

  CHECK(find_entry(entries, "t") != nullptr);
  CHECK(find_entry(entries, "h") == nullptr);
  CHECK(find_entry(entries, "co2") == nullptr);
  CHECK(find_entry(entries, "tvoc") == nullptr);
  CHECK(find_entry(entries, "nox") == nullptr);
  CHECK(find_entry(entries, "pres") == nullptr);
  CHECK(find_entry(entries, "ts") != nullptr);
}

TEST_CASE("BLE: encode_measures with no valid sensors has only timestamp") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  MeasuresAGo m{};
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.pm_a.pm_01 = MeasuresInvalid::PM;
  m.pm_a.pm_25 = MeasuresInvalid::PM;
  m.pm_a.pm_10 = MeasuresInvalid::PM;
  m.co2.co2 = MeasuresInvalid::CO2;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.pressure.pressure = MeasuresInvalid::PRESSURE;

  GpsData gps{};
  time_t ts = 999;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_measures(svc, buf, sizeof(buf), m, gps, ts);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 1);
  CHECK(entries[0].key == "ts");
  CHECK(entries[0].uint_val == 999);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Status
// ---------------------------------------------------------------------------

TEST_CASE("BLE: encode_status has all 9 keys") {
  storage_spy::reset();
  storage_spy::total_capacity_kb = 262144;
  storage_spy::used_kb = 8192;

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  auto power = make_valid_power();
  auto gps = make_valid_gps();

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_status(svc, buf, sizeof(buf), power, gps, true, 10042);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 9);

  CHECK(find_entry(entries, "gps_fix")->uint_val == 3);
  CHECK(find_entry(entries, "gps_sat")->uint_val == 12);
  CHECK(find_entry(entries, "bat_pct")->uint_val == 72);
  CHECK_THAT(find_entry(entries, "bat_v")->float_val, Catch::Matchers::WithinAbs(3.85, 0.01));
  CHECK(find_entry(entries, "charging")->text_val == "none");
  CHECK(find_entry(entries, "tracking")->bool_val == true);
  CHECK(find_entry(entries, "session")->uint_val == 10042);
  CHECK(find_entry(entries, "flash_kb")->uint_val == 262144);
  CHECK(find_entry(entries, "used_kb")->uint_val == 8192);
}

TEST_CASE("BLE: encode_status clamps negative battery values to 0") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  PowerSnapshot power{};
  power.battery_voltage = -1.0f;
  power.battery_percentage = -1.0f;
  GpsData gps{};

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_status(svc, buf, sizeof(buf), power, gps, false, 0);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(find_entry(entries, "bat_pct")->uint_val == 0);
  CHECK_THAT(find_entry(entries, "bat_v")->float_val, Catch::Matchers::WithinAbs(0.0, 0.01));
}

// ---------------------------------------------------------------------------
// CBOR encoding: Config
// ---------------------------------------------------------------------------

TEST_CASE("BLE: encode_config produces 18 keys with compact device config") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  auto settings = make_default_settings();

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_config(svc, buf, sizeof(buf), settings);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 18);

  CHECK(find_entry(entries, "meas_int") != nullptr);
  CHECK(find_entry(entries, "pm_int") == nullptr);
  CHECK(find_entry(entries, "other_int") == nullptr);
  CHECK(find_entry(entries, "disp_int") == nullptr);
  CHECK(find_entry(entries, "temp_f") != nullptr);
  CHECK(find_entry(entries, "pm_aqi") != nullptr);
  CHECK(find_entry(entries, "gps_mode") != nullptr);
  CHECK(find_entry(entries, "inact_to") != nullptr);
  CHECK(find_entry(entries, "auto_lock") != nullptr);
  CHECK(find_entry(entries, "dev_name") != nullptr);
  CHECK(find_entry(entries, "op_mode") != nullptr);
  CHECK(find_entry(entries, "fled") != nullptr);
  CHECK(find_entry(entries, "bled") != nullptr);
  CHECK(find_entry(entries, "tled") != nullptr);
  CHECK(find_entry(entries, "buz") != nullptr);
  CHECK(find_entry(entries, "abc") != nullptr);
  CHECK(find_entry(entries, "tlo") != nullptr);
  CHECK(find_entry(entries, "nlo") != nullptr);
  CHECK(top_level_value_is_map(buf, len, "pm25_corr"));
  CHECK(top_level_value_is_map(buf, len, "temp_corr"));
  CHECK(top_level_value_is_map(buf, len, "hum_corr"));
  CHECK(len < 256);
}

TEST_CASE("BLE: encode_config values match settings") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  GoSettings s{};
  s.measure_interval_seconds = 30;
  s.use_fahrenheit = true;
  s.gps_mode = GpsMode::AlwaysOn;
  s.device_name = "test-device";
  s.operating_mode = OperatingMode::Stationary;
  s.buzzer_enabled = true;
  s.co2_abc_days = CO2_ABC_DAYS_DISABLED;
  s.tvoc_learning_offset = LEARNING_OFFSET_HOURS_MIN;
  s.nox_learning_offset = LEARNING_OFFSET_HOURS_MAX;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_config(svc, buf, sizeof(buf), s);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(find_entry(entries, "meas_int")->uint_val == 30);
  CHECK(find_entry(entries, "pm_int") == nullptr);
  CHECK(find_entry(entries, "other_int") == nullptr);
  CHECK(find_entry(entries, "disp_int") == nullptr);
  CHECK(find_entry(entries, "temp_f")->bool_val == true);
  CHECK(find_entry(entries, "gps_mode")->text_val == "always");
  CHECK(find_entry(entries, "dev_name")->text_val == "test-device");
  CHECK(find_entry(entries, "op_mode")->text_val == "stationary");
  CHECK(find_entry(entries, "buz")->bool_val == true);
  CHECK(find_entry(entries, "abc")->int_val == CO2_ABC_DAYS_DISABLED);
  CHECK(find_entry(entries, "tlo")->uint_val == LEARNING_OFFSET_HOURS_MIN);
  CHECK(find_entry(entries, "nlo")->uint_val == LEARNING_OFFSET_HOURS_MAX);
}

// ---------------------------------------------------------------------------
// encode_config_delta (NOTIFY form: "type":"config" + changed fields only)
// ---------------------------------------------------------------------------

TEST_CASE("BLE: encode_config_delta single-field change yields 2-key map") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  GoSettings prev = make_default_settings();
  GoSettings cur = prev;
  cur.measure_interval_seconds = prev.measure_interval_seconds + 5;

  uint8_t buf[256];
  size_t len = BleServiceTestAccess::encode_config_delta(svc, buf, sizeof(buf), prev, cur);
  REQUIRE(len > 0);
  CHECK(len <= TEST_NOTIFY_BUDGET);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 2); // "type" + "meas_int"
  CHECK(find_entry(entries, "type")->text_val == "config");
  REQUIRE(find_entry(entries, "meas_int") != nullptr);
  CHECK(find_entry(entries, "meas_int")->uint_val == cur.measure_interval_seconds);
}

TEST_CASE("BLE: encode_config_delta correction change yields one nested field") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  GoSettings prev = make_default_settings();
  GoSettings cur = prev;
  cur.corrections.temperature.algorithm = LinearCorrectionAlgorithm::Custom;
  cur.corrections.temperature.scaling_factor = 1.01f;
  cur.corrections.temperature.intercept = -0.4f;

  uint8_t buf[256];
  size_t len = BleServiceTestAccess::encode_config_delta(svc, buf, sizeof(buf), prev, cur);
  REQUIRE(len > 0);
  CHECK(len <= TEST_NOTIFY_BUDGET);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 2); // "type" + "temp_corr"
  CHECK(find_entry(entries, "type")->text_val == "config");
  CHECK(top_level_value_is_map(buf, len, "temp_corr"));
  CHECK(find_entry(entries, "pm25_corr") == nullptr);
  CHECK(find_entry(entries, "hum_corr") == nullptr);
}

TEST_CASE("BLE: encode_config_delta includes compact device config changes") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  GoSettings prev = make_default_settings();
  GoSettings cur = prev;
  cur.buzzer_enabled = true;
  cur.co2_abc_days = CO2_ABC_DAYS_DISABLED;
  cur.tvoc_learning_offset = LEARNING_OFFSET_HOURS_MIN;
  cur.nox_learning_offset = LEARNING_OFFSET_HOURS_MAX;

  uint8_t buf[256];
  size_t len = BleServiceTestAccess::encode_config_delta(svc, buf, sizeof(buf), prev, cur);
  REQUIRE(len > 0);
  CHECK(len <= TEST_NOTIFY_BUDGET);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 5);
  CHECK(find_entry(entries, "buz")->bool_val == true);
  CHECK(find_entry(entries, "abc")->int_val == CO2_ABC_DAYS_DISABLED);
  CHECK(find_entry(entries, "tlo")->uint_val == LEARNING_OFFSET_HOURS_MIN);
  CHECK(find_entry(entries, "nlo")->uint_val == LEARNING_OFFSET_HOURS_MAX);
}

TEST_CASE("BLE: encode_config_delta no change yields only type") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  GoSettings s = make_default_settings();

  uint8_t buf[256];
  size_t len = BleServiceTestAccess::encode_config_delta(svc, buf, sizeof(buf), s, s);
  REQUIRE(len > 0);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 1);
  CHECK(find_entry(entries, "type")->text_val == "config");
}

// ---------------------------------------------------------------------------
// notify_config (delta via notify(data,len); READ stays full snapshot)
// ---------------------------------------------------------------------------

TEST_CASE("BLE: notify_config sends delta and keeps READ as full snapshot") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  GoSettings prev = make_default_settings();
  GoSettings cur = prev;
  cur.use_fahrenheit = !prev.use_fahrenheit;

  svc.notify_config(prev, cur);

  // Stored value (READ) refreshed to the full snapshot; notify carried the delta.
  REQUIRE(config_char.set_value_count == 1);
  REQUIRE(config_char.notify_count == 1);

  auto read_entries = decode_cbor_map(config_char.last_value.data(), config_char.last_value.size());
  CHECK(read_entries.size() == 18); // full snapshot, no "type"
  CHECK(find_entry(read_entries, "type") == nullptr);

  auto notify_entries = decode_cbor_map(config_char.last_notified_value.data(),
                                        config_char.last_notified_value.size());
  CHECK(notify_entries.size() == 2); // "type" + "temp_f"
  CHECK(find_entry(notify_entries, "type")->text_val == "config");
  CHECK(find_entry(notify_entries, "temp_f") != nullptr);

  // READ bytes != NOTIFY bytes.
  CHECK(config_char.last_value != config_char.last_notified_value);
}

// ---------------------------------------------------------------------------
// Encoder budget + overflow guards
// ---------------------------------------------------------------------------

TEST_CASE("BLE: largest single-field config delta (dev_name 64) within budget") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  GoSettings prev = make_default_settings();
  GoSettings cur = prev;
  cur.device_name = std::string(64, 'x'); // max-length device name

  uint8_t buf[256];
  size_t len = BleServiceTestAccess::encode_config_delta(svc, buf, sizeof(buf), prev, cur);
  REQUIRE(len > 0);
  CHECK(len <= TEST_NOTIFY_BUDGET);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 2); // "type" + "dev_name"
  CHECK(find_entry(entries, "dev_name")->text_val == std::string(64, 'x'));
}

TEST_CASE("BLE: max-size config snapshot encodes within the 512-byte ceiling") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  GoSettings s = make_default_settings();
  s.measure_interval_seconds = 3600;
  s.inactivity_timeout_seconds = TEST_MAX_INACTIVITY_TIMEOUT_SECONDS;
  s.auto_lock_seconds = TEST_MAX_AUTO_LOCK_SECONDS;
  s.device_name = std::string(64, 'x');
  s.gps_mode = GpsMode::OnWhenTracking;
  s.operating_mode = OperatingMode::Stationary;
  s.buzzer_enabled = true;
  s.co2_abc_days = CO2_ABC_DAYS_DISABLED;
  s.tvoc_learning_offset = LEARNING_OFFSET_HOURS_MAX;
  s.nox_learning_offset = LEARNING_OFFSET_HOURS_MAX;
  s.corrections.pm25.algorithm = Pm25CorrectionAlgorithm::CustomViaPm25Raw;
  s.corrections.pm25.scaling_factor = 1.08f;
  s.corrections.pm25.intercept = -0.2f;
  s.corrections.pm25.use_epa2021 = true;
  s.corrections.temperature.algorithm = LinearCorrectionAlgorithm::Custom;
  s.corrections.temperature.scaling_factor = 1.01f;
  s.corrections.temperature.intercept = -0.4f;
  s.corrections.humidity.algorithm = LinearCorrectionAlgorithm::Custom;
  s.corrections.humidity.scaling_factor = 0.98f;
  s.corrections.humidity.intercept = 1.5f;

  uint8_t buf[512];
  size_t len = BleServiceTestAccess::encode_config(svc, buf, sizeof(buf), s);
  REQUIRE(len > 0);
  CHECK(len <= 512);
}

TEST_CASE("BLE: encode_config returns 0 on encoder overflow") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  GoSettings s = make_default_settings();
  s.device_name = std::string(64, 'x');

  uint8_t tiny[16]; // deliberately too small for the full snapshot
  size_t len = BleServiceTestAccess::encode_config(svc, tiny, sizeof(tiny), s);
  CHECK(len == 0);
}

TEST_CASE("BLE: status transition delta and cmd_result are within budget") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  uint8_t buf[256];
  size_t len =
      BleServiceTestAccess::encode_status_transition(svc, buf, sizeof(buf), true, 0xFFFFFFFFu);
  REQUIRE(len > 0);
  CHECK(len <= TEST_NOTIFY_BUDGET);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 2);
  CHECK(find_entry(entries, "tracking") != nullptr);
  CHECK(find_entry(entries, "session") != nullptr);
}

TEST_CASE("BLE: status charging delta is within budget") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  // Worst case: longest charging string ("trickle"), full battery.
  PowerSnapshot power{};
  power.charging_status = BmsChargingState::TrickleCharge;
  power.battery_percentage = 100.0f;
  power.battery_voltage = 4.20f;

  uint8_t buf[256];
  size_t len = BleServiceTestAccess::encode_status_charging(svc, buf, sizeof(buf), power);
  REQUIRE(len > 0);
  CHECK(len <= TEST_NOTIFY_BUDGET);

  auto entries = decode_cbor_map(buf, len);
  CHECK(entries.size() == 3);
  CHECK(find_entry(entries, "charging")->text_val == "trickle");
  CHECK(find_entry(entries, "bat_pct")->uint_val == 100);
  CHECK(find_entry(entries, "bat_v") != nullptr);
}

// ---------------------------------------------------------------------------
// notify_command_result
// ---------------------------------------------------------------------------

TEST_CASE("BLE: notify_command_result success has 3 keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_result(BleCommand::Co2Calibration, true);

  // Command notifications go out via notify(data,len); the Config stored
  // value (READ snapshot) is never overwritten.
  REQUIRE(config_char.notify_count == 1);
  CHECK(config_char.set_value_count == 0);
  auto entries = decode_cbor_map(config_char.last_notified_value.data(),
                                 config_char.last_notified_value.size());
  CHECK(entries.size() == 3);
  CHECK(find_entry(entries, "type")->text_val == "cmd_result");
  CHECK(find_entry(entries, "cmd")->text_val == "co2_cal");
  CHECK(find_entry(entries, "ok")->bool_val == true);
  CHECK(find_entry(entries, "err") == nullptr);
}

TEST_CASE("BLE: notify_command_result failure with error has 4 keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_result(BleCommand::Co2Calibration, false, "sensor_not_ready");

  auto entries = decode_cbor_map(config_char.last_notified_value.data(),
                                 config_char.last_notified_value.size());
  CHECK(entries.size() == 4);
  CHECK(find_entry(entries, "ok")->bool_val == false);
  CHECK(find_entry(entries, "err")->text_val == "sensor_not_ready");
}

TEST_CASE("BLE: notify_command_result failure without error string has 3 keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_result(BleCommand::ClearData, false);

  auto entries = decode_cbor_map(config_char.last_notified_value.data(),
                                 config_char.last_notified_value.size());
  CHECK(entries.size() == 3);
  CHECK(find_entry(entries, "ok")->bool_val == false);
  CHECK(find_entry(entries, "err") == nullptr);
}

TEST_CASE("BLE: notify_command_result encodes start_tracking cmd string") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_result(BleCommand::StartTracking, true);

  auto entries = decode_cbor_map(config_char.last_notified_value.data(),
                                 config_char.last_notified_value.size());
  CHECK(find_entry(entries, "type")->text_val == "cmd_result");
  CHECK(find_entry(entries, "cmd")->text_val == "start_tracking");
  CHECK(find_entry(entries, "ok")->bool_val == true);
}

TEST_CASE("BLE: notify_command_result encodes stop_tracking cmd string") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_result(BleCommand::StopTracking, false, "not_tracking");

  auto entries = decode_cbor_map(config_char.last_notified_value.data(),
                                 config_char.last_notified_value.size());
  CHECK(find_entry(entries, "type")->text_val == "cmd_result");
  CHECK(find_entry(entries, "cmd")->text_val == "stop_tracking");
  CHECK(find_entry(entries, "ok")->bool_val == false);
  CHECK(find_entry(entries, "err")->text_val == "not_tracking");
}

// ---------------------------------------------------------------------------
// notify_command_progress
// ---------------------------------------------------------------------------

TEST_CASE("BLE: notify_command_progress sends 2-key CBOR map") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_progress(BleCommand::Co2Calibration);

  REQUIRE(config_char.notify_count == 1);
  CHECK(config_char.set_value_count == 0);
  auto entries = decode_cbor_map(config_char.last_notified_value.data(),
                                 config_char.last_notified_value.size());
  CHECK(entries.size() == 2);
  CHECK(find_entry(entries, "type")->text_val == "cmd_progress");
  CHECK(find_entry(entries, "cmd")->text_val == "co2_cal");
  CHECK(find_entry(entries, "ok") == nullptr);
  CHECK(find_entry(entries, "err") == nullptr);
}

TEST_CASE("BLE: notify_command_progress encodes clear_data cmd string") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_progress(BleCommand::ClearData);

  auto entries = decode_cbor_map(config_char.last_notified_value.data(),
                                 config_char.last_notified_value.size());
  CHECK(find_entry(entries, "type")->text_val == "cmd_progress");
  CHECK(find_entry(entries, "cmd")->text_val == "clear_data");
}

TEST_CASE("BLE: notify_command_progress encodes factory_rst cmd string") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_command_progress(BleCommand::FactoryReset);

  auto entries = decode_cbor_map(config_char.last_notified_value.data(),
                                 config_char.last_notified_value.size());
  CHECK(find_entry(entries, "type")->text_val == "cmd_progress");
  CHECK(find_entry(entries, "cmd")->text_val == "factory_rst");
}

TEST_CASE("BLE: notify_command_progress is no-op when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);

  svc.notify_command_progress(BleCommand::Co2Calibration);
  CHECK(config_char.set_value_count == 0);
  CHECK(config_char.notify_count == 0);
}

// ---------------------------------------------------------------------------
// decode_config_write: command round-trip
// ---------------------------------------------------------------------------

static size_t encode_cmd_cbor(uint8_t *buf, size_t sz, const char *cmd_str) {
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sz, 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 2);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "cmd");
  cbor_encode_text_stringz(&map, "cmd");
  cbor_encode_text_stringz(&map, cmd_str);
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

TEST_CASE("BLE: decode_config_write decodes start_tracking command") {
  uint8_t buf[64];
  size_t len = encode_cmd_cbor(buf, sizeof(buf), "start_tracking");

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Command);
  CHECK(result.cmd == BleCommand::StartTracking);
}

TEST_CASE("BLE: decode_config_write decodes stop_tracking command") {
  uint8_t buf[64];
  size_t len = encode_cmd_cbor(buf, sizeof(buf), "stop_tracking");

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Command);
  CHECK(result.cmd == BleCommand::StopTracking);
}

// ---------------------------------------------------------------------------
// decode_config_write: unknown key detection
// ---------------------------------------------------------------------------

/// Encode a set-config CBOR map with a single key-value pair.
static size_t encode_set_uint(uint8_t *buf, size_t sz, const char *key, uint64_t value) {
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sz, 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 2);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "set");
  cbor_encode_text_stringz(&map, key);
  cbor_encode_uint(&map, value);
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

static size_t encode_set_int(uint8_t *buf, size_t sz, const char *key, int64_t value) {
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sz, 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 2);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "set");
  cbor_encode_text_stringz(&map, key);
  cbor_encode_int(&map, value);
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

static size_t encode_set_bool(uint8_t *buf, size_t sz, const char *key, bool value) {
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sz, 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 2);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "set");
  cbor_encode_text_stringz(&map, key);
  cbor_encode_boolean(&map, value);
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

static size_t encode_set_text(uint8_t *buf, size_t sz, const char *key, const char *value) {
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sz, 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 2);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "set");
  cbor_encode_text_stringz(&map, key);
  cbor_encode_text_stringz(&map, value);
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

static size_t encode_set_pm25_correction(uint8_t *buf, size_t sz, uint64_t algorithm, float scale,
                                         float intercept, bool use_epa, uint64_t schema = 1) {
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sz, 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 2);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "set");
  cbor_encode_text_stringz(&map, "pm25_corr");

  CborEncoder correction;
  cbor_encoder_create_map(&map, &correction, 2);
  cbor_encode_text_stringz(&correction, "s");
  cbor_encode_uint(&correction, schema);
  cbor_encode_text_stringz(&correction, "v");
  CborEncoder values;
  cbor_encoder_create_array(&correction, &values, 4);
  cbor_encode_uint(&values, algorithm);
  cbor_encode_float(&values, scale);
  cbor_encode_float(&values, intercept);
  cbor_encode_uint(&values, use_epa ? 1 : 0);
  cbor_encoder_close_container(&correction, &values);
  cbor_encoder_close_container(&map, &correction);
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

static size_t encode_set_linear_correction(uint8_t *buf, size_t sz, const char *key,
                                           uint64_t algorithm, float scale, float intercept,
                                           uint64_t schema = 1) {
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sz, 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 2);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "set");
  cbor_encode_text_stringz(&map, key);

  CborEncoder correction;
  cbor_encoder_create_map(&map, &correction, 2);
  cbor_encode_text_stringz(&correction, "s");
  cbor_encode_uint(&correction, schema);
  cbor_encode_text_stringz(&correction, "v");
  CborEncoder values;
  cbor_encoder_create_array(&correction, &values, 3);
  cbor_encode_uint(&values, algorithm);
  cbor_encode_float(&values, scale);
  cbor_encode_float(&values, intercept);
  cbor_encoder_close_container(&correction, &values);
  cbor_encoder_close_container(&map, &correction);
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

/// Encode a set-config CBOR map with two key-value pairs (both uint).
static size_t encode_set_two_uints(uint8_t *buf, size_t sz, const char *key1, uint64_t val1,
                                   const char *key2, uint64_t val2) {
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sz, 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 3);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "set");
  cbor_encode_text_stringz(&map, key1);
  cbor_encode_uint(&map, val1);
  cbor_encode_text_stringz(&map, key2);
  cbor_encode_uint(&map, val2);
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

TEST_CASE("BLE: decode_config_write with known key has no unknown keys") {
  uint8_t buf[64];
  size_t len = encode_set_uint(buf, sizeof(buf), "meas_int", 30);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK_FALSE(result.has_unknown_keys);
  CHECK(settings.measure_interval_seconds == 30);
}

TEST_CASE("BLE: decode_config_write decodes compact device config fields") {
  uint8_t buf[64];
  GoSettings settings;

  SECTION("buzzer") {
    const size_t len = encode_set_bool(buf, sizeof(buf), "buz", true);
    const auto result = BleService::decode_config_write(buf, len, settings);
    CHECK(settings.buzzer_enabled);
    CHECK(result.recognized_config_key_count == 1);
    CHECK_FALSE(result.has_invalid_config_values);
  }

  SECTION("ABC disabled") {
    const size_t len = encode_set_int(buf, sizeof(buf), "abc", CO2_ABC_DAYS_DISABLED);
    const auto result = BleService::decode_config_write(buf, len, settings);
    CHECK(settings.co2_abc_days == CO2_ABC_DAYS_DISABLED);
    CHECK(result.recognized_config_key_count == 1);
    CHECK_FALSE(result.has_invalid_config_values);
  }

  SECTION("ABC enabled") {
    const size_t len = encode_set_int(buf, sizeof(buf), "abc", CO2_ABC_DAYS_MAX);
    const auto result = BleService::decode_config_write(buf, len, settings);
    CHECK(settings.co2_abc_days == CO2_ABC_DAYS_MAX);
    CHECK(result.recognized_config_key_count == 1);
    CHECK_FALSE(result.has_invalid_config_values);
  }

  SECTION("TVOC learning") {
    const size_t len = encode_set_uint(buf, sizeof(buf), "tlo", LEARNING_OFFSET_HOURS_MIN);
    const auto result = BleService::decode_config_write(buf, len, settings);
    CHECK(settings.tvoc_learning_offset == LEARNING_OFFSET_HOURS_MIN);
    CHECK(result.recognized_config_key_count == 1);
    CHECK_FALSE(result.has_invalid_config_values);
  }

  SECTION("NOx learning") {
    const size_t len = encode_set_uint(buf, sizeof(buf), "nlo", LEARNING_OFFSET_HOURS_MAX);
    const auto result = BleService::decode_config_write(buf, len, settings);
    CHECK(settings.nox_learning_offset == LEARNING_OFFSET_HOURS_MAX);
    CHECK(result.recognized_config_key_count == 1);
    CHECK_FALSE(result.has_invalid_config_values);
  }

  SECTION("GPS mode") {
    const size_t len = encode_set_text(buf, sizeof(buf), "gps_mode", "off");
    const auto result = BleService::decode_config_write(buf, len, settings);
    CHECK(settings.gps_mode == GpsMode::AlwaysOff);
    CHECK(result.recognized_config_key_count == 1);
    CHECK_FALSE(result.has_invalid_config_values);
  }
}

TEST_CASE("BLE: decode_config_write rejects invalid requested config values") {
  uint8_t buf[64];
  GoSettings settings;
  const GoSettings original = settings;
  size_t len = 0;

  SECTION("measurement interval") {
    len = encode_set_uint(buf, sizeof(buf), "meas_int", MEASURE_INTERVAL_SECONDS_MIN - 1);
  }
  SECTION("GPS mode") { len = encode_set_text(buf, sizeof(buf), "gps_mode", "sometimes"); }
  SECTION("front LED") {
    len =
        encode_set_uint(buf, sizeof(buf), "fled", static_cast<uint64_t>(LedBrightness::Bright) + 1);
  }
  SECTION("back LED") {
    len =
        encode_set_uint(buf, sizeof(buf), "bled", static_cast<uint64_t>(LedBrightness::Bright) + 1);
  }
  SECTION("touch LED") {
    len = encode_set_uint(buf, sizeof(buf), "tled",
                          static_cast<uint64_t>(TouchLedIntensity::Bright) + 1);
  }
  SECTION("buzzer type") { len = encode_set_uint(buf, sizeof(buf), "buz", 1); }
  SECTION("ABC days") { len = encode_set_int(buf, sizeof(buf), "abc", CO2_ABC_DAYS_MIN - 1); }
  SECTION("TVOC learning") {
    len = encode_set_uint(buf, sizeof(buf), "tlo", LEARNING_OFFSET_HOURS_MIN - 1);
  }
  SECTION("NOx learning") {
    len = encode_set_uint(buf, sizeof(buf), "nlo", LEARNING_OFFSET_HOURS_MAX + 1);
  }

  const auto result = BleService::decode_config_write(buf, len, settings);
  CHECK(result.op == BleConfigOp::Set);
  CHECK(result.recognized_config_key_count == 1);
  CHECK_FALSE(result.has_unknown_keys);
  CHECK(result.has_invalid_config_values);
  CHECK(settings.measure_interval_seconds == original.measure_interval_seconds);
  CHECK(settings.gps_mode == original.gps_mode);
  CHECK(settings.front_led_brightness == original.front_led_brightness);
  CHECK(settings.back_led_brightness == original.back_led_brightness);
  CHECK(settings.touch_led_intensity == original.touch_led_intensity);
  CHECK(settings.buzzer_enabled == original.buzzer_enabled);
  CHECK(settings.co2_abc_days == original.co2_abc_days);
  CHECK(settings.tvoc_learning_offset == original.tvoc_learning_offset);
  CHECK(settings.nox_learning_offset == original.nox_learning_offset);
}

TEST_CASE("BLE: decode_config_write decodes PM25 correction group") {
  uint8_t buf[192];
  size_t len = encode_set_pm25_correction(buf, sizeof(buf), 2, 1.08f, -0.2f, true);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK(result.recognized_config_key_count == 1);
  CHECK_FALSE(result.has_unknown_keys);
  CHECK_FALSE(result.has_invalid_config_values);
  CHECK(settings.corrections.pm25.algorithm == Pm25CorrectionAlgorithm::CustomViaPm25Raw);
  CHECK(settings.corrections.pm25.scaling_factor == Catch::Approx(1.08f));
  CHECK(settings.corrections.pm25.intercept == Catch::Approx(-0.2f));
  CHECK(settings.corrections.pm25.use_epa2021);
}

TEST_CASE("BLE: decode_config_write decodes linear correction group") {
  uint8_t buf[160];
  size_t len = encode_set_linear_correction(buf, sizeof(buf), "temp_corr", 1, 1.01f, -0.4f);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK(result.recognized_config_key_count == 1);
  CHECK_FALSE(result.has_unknown_keys);
  CHECK_FALSE(result.has_invalid_config_values);
  CHECK(settings.corrections.temperature.algorithm == LinearCorrectionAlgorithm::Custom);
  CHECK(settings.corrections.temperature.scaling_factor == Catch::Approx(1.01f));
  CHECK(settings.corrections.temperature.intercept == Catch::Approx(-0.4f));
}

TEST_CASE("BLE: decode_config_write rejects invalid correction values") {
  uint8_t buf[160];
  size_t len = encode_set_linear_correction(buf, sizeof(buf), "temp_corr", 99, 1.0f, 0.0f);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK_FALSE(result.has_unknown_keys);
  CHECK(result.has_invalid_config_values);
  CHECK(settings.corrections.temperature.algorithm == LinearCorrectionAlgorithm::None);
}

TEST_CASE("BLE: decode_config_write rejects unsupported correction schema") {
  uint8_t buf[160];
  size_t len = encode_set_linear_correction(buf, sizeof(buf), "temp_corr", 1, 1.0f, 0.0f, 2);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK_FALSE(result.has_unknown_keys);
  CHECK(result.has_invalid_config_values);
  CHECK(settings.corrections.temperature.algorithm == LinearCorrectionAlgorithm::None);
}

TEST_CASE("BLE: decode_config_write with deprecated key has no unknown keys") {
  uint8_t buf[64];
  size_t len = encode_set_uint(buf, sizeof(buf), "pm_int", 30);

  GoSettings settings;
  settings.measure_interval_seconds = 10;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK_FALSE(result.has_unknown_keys);
  CHECK(settings.measure_interval_seconds == 10); // unchanged
}

TEST_CASE("BLE: decode_config_write with unknown key sets has_unknown_keys") {
  uint8_t buf[64];
  size_t len = encode_set_uint(buf, sizeof(buf), "bad_key", 42);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK(result.has_unknown_keys);
}

TEST_CASE("BLE: decode_config_write with mixed known and unknown keys sets has_unknown_keys") {
  uint8_t buf[128];
  size_t len = encode_set_two_uints(buf, sizeof(buf), "meas_int", 30, "bad_key", 42);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK(result.has_unknown_keys);
}

// ---------------------------------------------------------------------------
// decode_config_write: single-field "set" enforcement
// ---------------------------------------------------------------------------

TEST_CASE("BLE: decode_config_write counts a single recognized config key") {
  uint8_t buf[64];
  size_t len = encode_set_uint(buf, sizeof(buf), "meas_int", 30);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK(result.recognized_config_key_count == 1);
  CHECK_FALSE(result.has_unknown_keys);
}

TEST_CASE("BLE: decode_config_write counts duplicate config-key occurrences") {
  uint8_t buf[128];
  size_t len = encode_set_two_uints(buf, sizeof(buf), "meas_int", 30, "meas_int", 40);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK(result.recognized_config_key_count == 2);
}

TEST_CASE("BLE: deprecated keys do not count toward recognized config keys") {
  uint8_t buf[64];
  size_t len = encode_set_uint(buf, sizeof(buf), "pm_int", 30);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK(result.recognized_config_key_count == 0);
  CHECK_FALSE(result.has_unknown_keys);
}

TEST_CASE("BLE: aiding key under op:set is flagged as unknown config key") {
  uint8_t buf[64];
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 2);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "set");
  cbor_encode_text_stringz(&map, "lat");
  cbor_encode_double(&map, 47.376887);
  cbor_encoder_close_container(&enc, &map);
  size_t len = cbor_encoder_get_buffer_size(&enc, buf);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Set);
  CHECK(result.has_unknown_keys); // aiding key meaningless under "set"
  CHECK(result.recognized_config_key_count == 0);
}

// ---------------------------------------------------------------------------
// decode_config_write: set_aiding command
// ---------------------------------------------------------------------------

/// Encode a set_aiding command with all aiding fields as CBOR.
static size_t encode_set_aiding_full(uint8_t *buf, size_t sz, double lat, double lon, float alt,
                                     float pos_acc, uint64_t epoch, uint32_t time_acc) {
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sz, 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 8);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "cmd");
  cbor_encode_text_stringz(&map, "cmd");
  cbor_encode_text_stringz(&map, "set_aiding");
  cbor_encode_text_stringz(&map, "lat");
  cbor_encode_double(&map, lat);
  cbor_encode_text_stringz(&map, "lon");
  cbor_encode_double(&map, lon);
  cbor_encode_text_stringz(&map, "alt");
  cbor_encode_float(&map, alt);
  cbor_encode_text_stringz(&map, "pos_acc");
  cbor_encode_float(&map, pos_acc);
  cbor_encode_text_stringz(&map, "epoch");
  cbor_encode_uint(&map, epoch);
  cbor_encode_text_stringz(&map, "time_acc");
  cbor_encode_uint(&map, time_acc);
  cbor_encoder_close_container(&enc, &map);
  return cbor_encoder_get_buffer_size(&enc, buf);
}

TEST_CASE("BLE: decode_config_write decodes set_aiding command with all fields") {
  uint8_t buf[128];
  size_t len = encode_set_aiding_full(buf, sizeof(buf), 47.376887, 8.541694, 408.0f, 50.0f,
                                      1711234567, 2000);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Command);
  CHECK(result.cmd == BleCommand::SetAiding);
  CHECK_FALSE(result.has_unknown_keys);
  CHECK(result.aiding.latitude == 47.376887);
  CHECK(result.aiding.longitude == 8.541694);
  CHECK(result.aiding.altitude_m == 408.0f);
  CHECK(result.aiding.pos_acc_m == 50.0f);
  CHECK(result.aiding.epoch_s == 1711234567);
  CHECK(result.aiding.time_acc_ms == 2000);
}

TEST_CASE("BLE: decode_config_write decodes set_aiding with position only") {
  uint8_t buf[128];
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 4);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "cmd");
  cbor_encode_text_stringz(&map, "cmd");
  cbor_encode_text_stringz(&map, "set_aiding");
  cbor_encode_text_stringz(&map, "lat");
  cbor_encode_double(&map, 47.376887);
  cbor_encode_text_stringz(&map, "lon");
  cbor_encode_double(&map, 8.541694);
  cbor_encoder_close_container(&enc, &map);
  size_t len = cbor_encoder_get_buffer_size(&enc, buf);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Command);
  CHECK(result.cmd == BleCommand::SetAiding);
  CHECK(result.aiding.latitude == 47.376887);
  CHECK(result.aiding.longitude == 8.541694);
  // Unset fields remain at default sentinels
  CHECK(result.aiding.epoch_s == 0);
  CHECK(result.aiding.altitude_m == GPS_ALTITUDE_INVALID);
}

TEST_CASE("BLE: decode_config_write decodes set_aiding with time only") {
  uint8_t buf[128];
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 4);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "cmd");
  cbor_encode_text_stringz(&map, "cmd");
  cbor_encode_text_stringz(&map, "set_aiding");
  cbor_encode_text_stringz(&map, "epoch");
  cbor_encode_uint(&map, 1711234567);
  cbor_encode_text_stringz(&map, "time_acc");
  cbor_encode_uint(&map, 2000);
  cbor_encoder_close_container(&enc, &map);
  size_t len = cbor_encoder_get_buffer_size(&enc, buf);

  GoSettings settings;
  auto result = BleService::decode_config_write(buf, len, settings);

  CHECK(result.op == BleConfigOp::Command);
  CHECK(result.cmd == BleCommand::SetAiding);
  CHECK(result.aiding.epoch_s == 1711234567);
  CHECK(result.aiding.time_acc_ms == 2000);
  // Position fields remain at invalid sentinels
  CHECK(result.aiding.latitude == GPS_LATITUDE_INVALID);
  CHECK(result.aiding.longitude == GPS_LONGITUDE_INVALID);
}

// ---------------------------------------------------------------------------
// Wire format: RoutePointWire
// ---------------------------------------------------------------------------

TEST_CASE("BLE: route_point_to_wire produces 56 bytes with all valid fields") {
  RoutePoint point{};
  point.timestamp = 1711234567;
  point.gps.position.latitude = 47.376887;
  point.gps.position.longitude = 8.541694;
  point.gps.altitude_m = 408.0f;
  point.gps.fix.fix_type = GpsFixType::Fix3D;
  point.sensors.temp_hum_a.temperature = 23.5f;
  point.sensors.temp_hum_a.humidity = 45.2f;
  point.sensors.pm_a.pm_01 = 5.0f;
  point.sensors.pm_a.pm_25 = 8.3f;
  point.sensors.pm_a.pm_10 = 12.1f;
  point.sensors.co2.co2 = 450;
  point.sensors.tvoc_nox.tvoc_index = 120;
  point.sensors.tvoc_nox.nox_index = 5;
  point.sensors.pressure.pressure = 1013.2f;
  point.battery_percentage = 72.0f;

  uint8_t wire[56];
  BleServiceTestAccess::route_point_to_wire(point, wire);

  // Verify timestamp at offset 0 (uint32_le)
  uint32_t ts;
  memcpy(&ts, wire, sizeof(ts));
  CHECK(ts == 1711234567);

  // Verify latitude at offset 4 (float64_le)
  double lat;
  memcpy(&lat, wire + 4, sizeof(lat));
  CHECK_THAT(lat, Catch::Matchers::WithinAbs(47.376887, 0.0001));

  // Verify longitude at offset 12 (float64_le)
  double lon;
  memcpy(&lon, wire + 12, sizeof(lon));
  CHECK_THAT(lon, Catch::Matchers::WithinAbs(8.541694, 0.0001));

  // Verify altitude at offset 20 (float32_le)
  float alt;
  memcpy(&alt, wire + 20, sizeof(alt));
  CHECK_THAT(static_cast<double>(alt), Catch::Matchers::WithinAbs(408.0, 0.1));

  // Verify gps_fix at offset 24 (uint8)
  CHECK(wire[24] == 3);

  // Verify temperature at offset 25 (float32_le)
  float temp;
  memcpy(&temp, wire + 25, sizeof(temp));
  CHECK_THAT(static_cast<double>(temp), Catch::Matchers::WithinAbs(23.5, 0.1));

  // Verify co2 at offset 45 (int16_le)
  int16_t co2;
  memcpy(&co2, wire + 45, sizeof(co2));
  CHECK(co2 == 450);

  // Verify pressure at offset 51 (float32_le)
  float pres;
  memcpy(&pres, wire + 51, sizeof(pres));
  CHECK_THAT(static_cast<double>(pres), Catch::Matchers::WithinAbs(1013.2, 0.1));

  // Verify battery_percentage at offset 55 (uint8)
  CHECK(wire[55] == 72);
}

TEST_CASE("BLE: route_point_to_wire uses sentinels for invalid fields") {
  RoutePoint point{};
  point.timestamp = 100;
  // GPS invalid (defaults)
  point.sensors.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  point.sensors.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  point.sensors.pm_a.pm_01 = MeasuresInvalid::PM;
  point.sensors.pm_a.pm_25 = MeasuresInvalid::PM;
  point.sensors.pm_a.pm_10 = MeasuresInvalid::PM;
  point.sensors.co2.co2 = MeasuresInvalid::CO2;
  point.sensors.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  point.sensors.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  point.sensors.pressure.pressure = MeasuresInvalid::PRESSURE;
  // battery_percentage left at default -1.0f (invalid)

  uint8_t wire[56];
  BleServiceTestAccess::route_point_to_wire(point, wire);

  // Latitude should be GPS_LATITUDE_INVALID (91.0)
  double lat;
  memcpy(&lat, wire + 4, sizeof(lat));
  CHECK_THAT(lat, Catch::Matchers::WithinAbs(91.0, 0.001));

  // Longitude should be GPS_LONGITUDE_INVALID (181.0)
  double lon;
  memcpy(&lon, wire + 12, sizeof(lon));
  CHECK_THAT(lon, Catch::Matchers::WithinAbs(181.0, 0.001));

  // Altitude should be GPS_ALTITUDE_INVALID (-10000.0)
  float alt;
  memcpy(&alt, wire + 20, sizeof(alt));
  CHECK_THAT(static_cast<double>(alt), Catch::Matchers::WithinAbs(-10000.0, 0.1));

  // CO2 should be -1
  int16_t co2;
  memcpy(&co2, wire + 45, sizeof(co2));
  CHECK(co2 == -1);

  // TVOC should be -1
  int16_t tvoc;
  memcpy(&tvoc, wire + 47, sizeof(tvoc));
  CHECK(tvoc == -1);

  // NOx should be -1
  int16_t nox;
  memcpy(&nox, wire + 49, sizeof(nox));
  CHECK(nox == -1);

  // Battery percentage should be 255 (invalid sentinel)
  CHECK(wire[55] == 255);
}

// ---------------------------------------------------------------------------
// String mapping
// ---------------------------------------------------------------------------

TEST_CASE("BLE: charging_state_to_str maps all values") {
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::Unknown)) ==
        "unknown");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::NotCharging)) ==
        "none");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::TrickleCharge)) ==
        "trickle");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::PreCharge)) ==
        "pre");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::FastCharge)) ==
        "fast");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(BmsChargingState::TaperCharge)) ==
        "taper");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(
            BmsChargingState::TopOffTimerActiveCharging)) == "topoff");
  CHECK(std::string(BleServiceTestAccess::charging_state_to_str(
            BmsChargingState::ChargeTerminationDone)) == "done");
}

TEST_CASE("BLE: gps_mode_to_str maps all values") {
  CHECK(std::string(BleServiceTestAccess::gps_mode_to_str(GpsMode::AlwaysOff)) == "off");
  CHECK(std::string(BleServiceTestAccess::gps_mode_to_str(GpsMode::OnWhenTracking)) == "tracking");
  CHECK(std::string(BleServiceTestAccess::gps_mode_to_str(GpsMode::AlwaysOn)) == "always");
}

TEST_CASE("BLE: operating_mode_to_str maps all values") {
  CHECK(std::string(BleServiceTestAccess::operating_mode_to_str(OperatingMode::Portable)) ==
        "portable");
  CHECK(std::string(BleServiceTestAccess::operating_mode_to_str(OperatingMode::Stationary)) ==
        "stationary");
  CHECK(std::string(BleServiceTestAccess::operating_mode_to_str(OperatingMode::Offline)) ==
        "offline");
}

// ---------------------------------------------------------------------------
// Notification flow
// ---------------------------------------------------------------------------

TEST_CASE("BLE: notify_measures sets value but skips notify when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic measures_char;
  BleServiceTestAccess::set_measures_char(svc, &measures_char);
  // Not connected (default)

  svc.notify_measures(make_valid_measures(), make_valid_gps(), 100);
  CHECK(measures_char.set_value_count == 1);
  CHECK(measures_char.notify_count == 0);
  CHECK(!measures_char.last_value.empty());
}

TEST_CASE("BLE: notify_measures encodes and notifies when connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic measures_char;
  BleServiceTestAccess::set_measures_char(svc, &measures_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_measures(make_valid_measures(), make_valid_gps(), 100);
  CHECK(measures_char.set_value_count == 1);
  CHECK(measures_char.notify_count == 1);
  CHECK(!measures_char.last_value.empty());
}

TEST_CASE("BLE: update_status is no-op when char is null") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  // _status_char is nullptr by default

  // Should not crash
  svc.update_status(make_valid_power(), make_valid_gps(), false, 0);
}

TEST_CASE("BLE: update_status sets value but does not notify") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic status_char;
  BleServiceTestAccess::set_status_char(svc, &status_char);

  // Confirm update_status() does not invoke notify(), even when connected
  // — it is the steady-state set-value-only path.
  BleServiceTestAccess::set_connected(svc, true);

  svc.update_status(make_valid_power(), make_valid_gps(), true, 10042);
  CHECK(status_char.set_value_count == 1);
  CHECK(status_char.notify_count == 0);
}

TEST_CASE("BLE: notify_tracking_status notifies 2-key delta while READ stays 9 keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic status_char;
  BleServiceTestAccess::set_status_char(svc, &status_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_tracking_status(make_valid_power(), make_valid_gps(), true, 10042);
  CHECK(status_char.set_value_count == 1);
  CHECK(status_char.notify_count == 1);

  // Stored value (READ) is the full 9-key snapshot.
  auto read_entries = decode_cbor_map(status_char.last_value.data(), status_char.last_value.size());
  CHECK(read_entries.size() == 9);
  CHECK(find_entry(read_entries, "type") == nullptr);

  // NOTIFY carries only the {tracking, session} transition delta.
  auto notify_entries = decode_cbor_map(status_char.last_notified_value.data(),
                                        status_char.last_notified_value.size());
  CHECK(notify_entries.size() == 2);
  CHECK(find_entry(notify_entries, "tracking")->bool_val == true);
  CHECK(find_entry(notify_entries, "session")->uint_val == 10042);
}

TEST_CASE("BLE: notify_tracking_status sets value but skips notify when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic status_char;
  BleServiceTestAccess::set_status_char(svc, &status_char);

  // Disconnected (default): value still updated for the next Read.
  svc.notify_tracking_status(make_valid_power(), make_valid_gps(), false, 0);
  CHECK(status_char.set_value_count == 1);
  CHECK(status_char.notify_count == 0);
}

TEST_CASE("BLE: notify_tracking_status is no-op when char is null") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  // _status_char is nullptr by default — must not crash.
  svc.notify_tracking_status(make_valid_power(), make_valid_gps(), false, 0);
}

TEST_CASE("BLE: notify_charging_status notifies 3-key power delta while READ stays 9 keys") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic status_char;
  BleServiceTestAccess::set_status_char(svc, &status_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_charging_status(make_valid_power(), make_valid_gps(), true, 10042);
  CHECK(status_char.set_value_count == 1);
  CHECK(status_char.notify_count == 1);

  // Stored value (READ) is the full 9-key snapshot, no discriminator.
  auto read_entries = decode_cbor_map(status_char.last_value.data(), status_char.last_value.size());
  CHECK(read_entries.size() == 9);
  CHECK(find_entry(read_entries, "type") == nullptr);

  // NOTIFY carries only the {charging, bat_pct, bat_v} power delta — keys
  // disjoint from the tracking transition delta, no "type" discriminator.
  auto notify_entries = decode_cbor_map(status_char.last_notified_value.data(),
                                        status_char.last_notified_value.size());
  CHECK(notify_entries.size() == 3);
  CHECK(find_entry(notify_entries, "charging") != nullptr);
  CHECK(find_entry(notify_entries, "bat_pct") != nullptr);
  CHECK(find_entry(notify_entries, "bat_v") != nullptr);
  CHECK(find_entry(notify_entries, "type") == nullptr);
  CHECK(find_entry(notify_entries, "tracking") == nullptr);
  CHECK(find_entry(notify_entries, "session") == nullptr);
}

TEST_CASE("BLE: notify_charging_status sets value but skips notify when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic status_char;
  BleServiceTestAccess::set_status_char(svc, &status_char);

  // Disconnected (default): snapshot still refreshed for the next Read.
  svc.notify_charging_status(make_valid_power(), make_valid_gps(), false, 0);
  CHECK(status_char.set_value_count == 1);
  CHECK(status_char.notify_count == 0);
}

TEST_CASE("BLE: notify_charging_status is no-op when char is null") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  // _status_char is nullptr by default — must not crash.
  svc.notify_charging_status(make_valid_power(), make_valid_gps(), false, 0);
}

TEST_CASE("BLE: encode_status_charging overflows on undersized buffer") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  uint8_t tiny[4];
  size_t len =
      BleServiceTestAccess::encode_status_charging(svc, tiny, sizeof(tiny), make_valid_power());
  CHECK(len == 0);
}

TEST_CASE("BLE: notify_disconnect sends a 1-key {disc} delta without touching the READ value") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic status_char;
  BleServiceTestAccess::set_status_char(svc, &status_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_disconnect(BleDiscReason::OpStationary);

  // NOTIFY-only: the stored READ value is never written.
  CHECK(status_char.set_value_count == 0);
  CHECK(status_char.notify_count == 1);

  auto entries = decode_cbor_map(status_char.last_notified_value.data(),
                                 status_char.last_notified_value.size());
  CHECK(entries.size() == 1);
  CHECK(find_entry(entries, "disc")->text_val == "op_stationary");
}

TEST_CASE("BLE: notify_disconnect skips notify when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic status_char;
  BleServiceTestAccess::set_status_char(svc, &status_char);

  svc.notify_disconnect(BleDiscReason::Overheat);
  CHECK(status_char.set_value_count == 0);
  CHECK(status_char.notify_count == 0);
}

TEST_CASE("BLE: notify_disconnect is no-op when char is null") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  BleServiceTestAccess::set_connected(svc, true);
  // _status_char is nullptr by default — must not crash.
  svc.notify_disconnect(BleDiscReason::User);
}

TEST_CASE("BLE: disc delta is within budget for every reason") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  for (auto reason : {BleDiscReason::Overheat, BleDiscReason::LowBatt, BleDiscReason::User,
                      BleDiscReason::OpStationary, BleDiscReason::OpOffline}) {
    uint8_t buf[256];
    size_t len = BleServiceTestAccess::encode_status_disc(svc, buf, sizeof(buf), reason);
    REQUIRE(len > 0);
    CHECK(len <= TEST_NOTIFY_BUDGET);

    auto entries = decode_cbor_map(buf, len);
    CHECK(entries.size() == 1);
    CHECK(find_entry(entries, "disc")->text_val ==
          std::string(BleServiceTestAccess::disc_reason_to_str(reason)));
  }
}

TEST_CASE("BLE: encode_status_disc overflows on undersized buffer") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);

  uint8_t tiny[4];
  size_t len = BleServiceTestAccess::encode_status_disc(svc, tiny, sizeof(tiny),
                                                        BleDiscReason::OpStationary);
  CHECK(len == 0);
}

TEST_CASE("BLE: update_config sets value but does not notify") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);

  svc.update_config(make_default_settings());
  CHECK(config_char.set_value_count == 1);
  CHECK(config_char.notify_count == 0);
}

TEST_CASE("BLE: notify_config refreshes snapshot but skips notify when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);

  GoSettings prev = make_default_settings();
  GoSettings cur = prev;
  cur.use_fahrenheit = !prev.use_fahrenheit;

  svc.notify_config(prev, cur);
  // Stored snapshot is refreshed via internal update_config() even when
  // disconnected; no notification is sent.
  CHECK(config_char.set_value_count == 1);
  CHECK(config_char.notify_count == 0);
}

TEST_CASE("BLE: notify_command_result is no-op when not connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic config_char;
  BleServiceTestAccess::set_config_char(svc, &config_char);

  svc.notify_command_result(BleCommand::Unknown, true);
  CHECK(config_char.set_value_count == 0);
}

// ---------------------------------------------------------------------------
// Connection lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("BLE: on_connect keeps advertising while a connection slot remains") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleServer server;
  BleServiceTestAccess::set_server(svc, &server);

  CHECK(svc.is_connected() == false);
  BleServiceTestAccess::on_connect(svc, 1);
  CHECK(svc.is_connected() == true);
  // Link starts unauthenticated until the peer pairs.
  CHECK(svc.is_authenticated() == false);
  CHECK(server.stop_advertising_count == 0);
  CHECK(server.start_advertising_count == 1);
}

TEST_CASE("BLE: connection count advertises only below capacity") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleServer server;
  BleServiceTestAccess::set_server(svc, &server);

  BleServiceTestAccess::on_connect(svc, 1);
  CHECK(svc.connected_client_count() == 1);
  CHECK(server.start_advertising_count == 1);

  BleServiceTestAccess::on_connect(svc, 2);
  CHECK(svc.connected_client_count() == BleService::MAX_CONNECTED_CLIENTS);
  CHECK(server.start_advertising_count == 1);
  CHECK(server.stop_advertising_count == 1);

  uint16_t disconnected_handle = UINT16_MAX;
  svc.set_disconnect_observer([&disconnected_handle](uint16_t conn_handle, int /*reason*/) {
    disconnected_handle = conn_handle;
  });
  BleServiceTestAccess::on_disconnect(svc, 1, 0);
  CHECK(svc.connected_client_count() == 1);
  CHECK(server.start_advertising_count == 2);
  CHECK(disconnected_handle == 1);
}

TEST_CASE("BLE: watch profile and Numeric Comparison response are explicit") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleServer server;
  BleServiceTestAccess::set_server(svc, &server);
  BleServiceTestAccess::set_initialized(svc, true);

  REQUIRE(svc.begin_watch_pairing());
  CHECK(server.last_io_cap == AgBleIoCapability::DISPLAY_YES_NO);
  CHECK(server.last_auth_flags == (AgBleAuth::BOND | AgBleAuth::MITM | AgBleAuth::SC));

  REQUIRE(svc.confirm_numeric_comparison(7, true));
  CHECK(server.confirm_numeric_comparison_count == 1);
  CHECK(server.last_numeric_comparison_handle == 7);
  CHECK(server.last_numeric_comparison_accept);

  REQUIRE(svc.restore_normal_pairing());
  CHECK(server.last_io_cap == AgBleIoCapability::DISPLAY_ONLY);
  CHECK(server.last_auth_flags == (AgBleAuth::BOND | AgBleAuth::MITM));
}

TEST_CASE("BLE: is_authenticated tracks live peer state while connected") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleServer server;
  BleServiceTestAccess::set_server(svc, &server);

  // Not connected: never authenticated regardless of peer state.
  server.peer_authenticated = true;
  CHECK(svc.is_authenticated() == false);

  // Connected + authenticated peer → usable link.
  BleServiceTestAccess::on_connect(svc, 1);
  CHECK(svc.is_authenticated() == true);

  // Peer auth dropping reflects immediately — no stale cached flag.
  server.peer_authenticated = false;
  CHECK(svc.is_authenticated() == false);
}

TEST_CASE("BLE: on_disconnect clears state and restarts advertising") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleServer server;
  BleServiceTestAccess::set_server(svc, &server);
  BleServiceTestAccess::set_connected(svc, true);
  server.peer_authenticated = true;
  BleServiceTestAccess::set_export_active(svc, true);
  BleServiceTestAccess::set_export_session_id(svc, 10042);

  BleServiceTestAccess::on_disconnect(svc, 1, 0);
  CHECK(svc.is_connected() == false);
  CHECK(svc.is_authenticated() == false);
  CHECK(BleServiceTestAccess::export_active(svc) == false);
  CHECK(BleServiceTestAccess::export_session_id(svc) == 0);
  CHECK(server.start_advertising_count == 1);
}

TEST_CASE("BLE: delete_all_bonds proxies to server") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleServer server;
  BleServiceTestAccess::set_server(svc, &server);

  CHECK(svc.delete_all_bonds());
  CHECK(server.delete_all_bonds_count == 1);
}

// ---------------------------------------------------------------------------
// History download
// ---------------------------------------------------------------------------

TEST_CASE("BLE: handle_history_list is no-op when char is null") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  // _history_char is nullptr — should not crash
  svc.handle_history_list();
}

TEST_CASE("BLE: handle_history_list sends session list with pagination fields") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 150, 1737000000}, {10002, 300, 1737100000}};

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_list();

  // 2 sessions fit in a single page
  REQUIRE(history_char.set_value_count == 1);
  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);

  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "sessions");
  CHECK(find_entry(entries, "pg")->uint_val == 1);
  CHECK(find_entry(entries, "tpg")->uint_val == 1);
  CHECK(find_entry(entries, "cnt")->uint_val == 2);
}

TEST_CASE("BLE: handle_history_list empty list sends one page with count zero") {
  storage_spy::reset();
  storage_spy::sessions.clear();

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_list();

  REQUIRE(history_char.set_value_count == 1);
  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);

  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "sessions");
  CHECK(find_entry(entries, "pg")->uint_val == 1);
  CHECK(find_entry(entries, "tpg")->uint_val == 1);
  CHECK(find_entry(entries, "cnt")->uint_val == 0);
}

TEST_CASE("BLE: handle_history_list paginates large session lists") {
  storage_spy::reset();

  // 14 sessions → 3 pages (6 + 6 + 2)
  for (uint32_t i = 0; i < 14; i++) {
    storage_spy::sessions.push_back(
        {10001 + i, static_cast<uint32_t>(100 + i * 10), static_cast<time_t>(1737000000 + i)});
  }

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_list();

  REQUIRE(history_char.set_value_count == 3);
  REQUIRE(history_char.all_values.size() == 3);

  // Page 1: 6 sessions
  {
    const auto &val = history_char.all_values[0];
    REQUIRE(val[0] == 0x00);
    auto entries = decode_cbor_map(val.data() + 1, val.size() - 1);
    CHECK(find_entry(entries, "type")->text_val == "sessions");
    CHECK(find_entry(entries, "pg")->uint_val == 1);
    CHECK(find_entry(entries, "tpg")->uint_val == 3);
    CHECK(find_entry(entries, "cnt")->uint_val == 14);
  }

  // Page 2: 6 sessions
  {
    const auto &val = history_char.all_values[1];
    REQUIRE(val[0] == 0x00);
    auto entries = decode_cbor_map(val.data() + 1, val.size() - 1);
    CHECK(find_entry(entries, "pg")->uint_val == 2);
    CHECK(find_entry(entries, "tpg")->uint_val == 3);
    CHECK(find_entry(entries, "cnt")->uint_val == 14);
  }

  // Page 3: 2 sessions (remainder)
  {
    const auto &val = history_char.all_values[2];
    REQUIRE(val[0] == 0x00);
    auto entries = decode_cbor_map(val.data() + 1, val.size() - 1);
    CHECK(find_entry(entries, "pg")->uint_val == 3);
    CHECK(find_entry(entries, "tpg")->uint_val == 3);
    CHECK(find_entry(entries, "cnt")->uint_val == 14);
  }
}

TEST_CASE("BLE: handle_history_start sends error for non-existent session") {
  storage_spy::reset();

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_start(99999);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);

  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "error");
  CHECK(find_entry(entries, "err")->text_val == "session_not_found");
}

TEST_CASE("BLE: handle_history_start streams points and sends done") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 2, 1737000000}};

  RoutePoint pt1{};
  pt1.timestamp = 1737000000;
  pt1.sensors.temp_hum_a.temperature = 20.0f;
  pt1.sensors.temp_hum_a.humidity = 50.0f;
  RoutePoint pt2{};
  pt2.timestamp = 1737000060;
  pt2.sensors.temp_hum_a.temperature = 21.0f;
  pt2.sensors.temp_hum_a.humidity = 48.0f;
  storage_spy::points = {pt1, pt2};

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_start(10001);

  // Should have sent multiple notifications:
  // 1. started (CBOR tag 0x00)
  // 2. binary data (tag 0x01)
  // 3. done (CBOR tag 0x00)
  // The last set_value is the "done" response
  REQUIRE(history_char.set_value_count >= 3);
  REQUIRE(history_char.notify_count >= 3);

  // Last notification should be "done"
  CHECK(history_char.last_value[0] == 0x00);
  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "done");
  CHECK(find_entry(entries, "sent")->uint_val == 2);
}

TEST_CASE("BLE: history export sends raw route points") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 1, 1737000000}};

  RoutePoint point{};
  point.timestamp = 1737000000;
  point.sensors.temp_hum_a.temperature = 20.0f;
  point.sensors.temp_hum_a.humidity = 50.0f;
  point.sensors.pm_a.pm_25 = 10.0f;
  storage_spy::points = {point};

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_start(10001);

  const auto binary =
      std::find_if(history_char.all_values.begin(), history_char.all_values.end(),
                   [](const auto &value) { return !value.empty() && value[0] == 0x01; });
  REQUIRE(binary != history_char.all_values.end());
  float temperature = 0.0f;
  float pm25 = 0.0f;
  memcpy(&temperature, binary->data() + 3 + 25, sizeof(temperature));
  memcpy(&pm25, binary->data() + 3 + 37, sizeof(pm25));
  CHECK(temperature == 20.0f);
  CHECK(pm25 == 10.0f);
  CHECK(storage_spy::points[0].sensors.temp_hum_a.temperature == 20.0f);
  CHECK(storage_spy::points[0].sensors.pm_a.pm_25 == 10.0f);
}

TEST_CASE("BLE: history fill sends raw route points") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 1, 1737000000}};

  RoutePoint point{};
  point.sensors.pm_a.pm_25 = 10.0f;
  storage_spy::points = {point};

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_start(10001);

  history_char.all_values.clear();
  uint32_t index = 0;
  svc.handle_history_fill(&index, 1);

  const auto binary =
      std::find_if(history_char.all_values.begin(), history_char.all_values.end(),
                   [](const auto &value) { return !value.empty() && value[0] == 0x01; });
  REQUIRE(binary != history_char.all_values.end());
  float pm25 = 0.0f;
  memcpy(&pm25, binary->data() + 3 + 37, sizeof(pm25));
  CHECK(pm25 == 10.0f);
}

TEST_CASE("BLE: handle_history_fill sends error when no active download") {
  storage_spy::reset();

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  uint32_t indices[] = {0, 1};
  svc.handle_history_fill(indices, 2);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);
  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "error");
  CHECK(find_entry(entries, "err")->text_val == "no_active_download");
}

TEST_CASE("BLE: handle_history_fill retransmits requested points") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 3, 1737000000}};

  RoutePoint pt{};
  pt.timestamp = 1737000000;
  storage_spy::points = {pt, pt, pt};

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);
  BleServiceTestAccess::set_export_active(svc, true);
  BleServiceTestAccess::set_export_session_id(svc, 10001);

  uint32_t indices[] = {0, 2};
  svc.handle_history_fill(indices, 2);

  // Should have sent: 2 binary data + 1 done
  REQUIRE(history_char.notify_count >= 3);

  // Last notification is "done" with sent: 2
  CHECK(history_char.last_value[0] == 0x00);
  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "done");
  CHECK(find_entry(entries, "sent")->uint_val == 2);
}

TEST_CASE("BLE: handle_history_end clears export state and sends ended") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);
  BleServiceTestAccess::set_export_active(svc, true);
  BleServiceTestAccess::set_export_session_id(svc, 10042);

  svc.handle_history_end();

  CHECK(BleServiceTestAccess::export_active(svc) == false);
  CHECK(BleServiceTestAccess::export_session_id(svc) == 0);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);
  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "ended");
}

// ---------------------------------------------------------------------------
// History delete
// ---------------------------------------------------------------------------

TEST_CASE("BLE: handle_history_delete is no-op when char is null") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  // _history_char is nullptr — should not crash
  svc.handle_history_delete(10001);
}

TEST_CASE("BLE: handle_history_delete sends error for non-existent session") {
  storage_spy::reset();

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_delete(99999);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);

  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "error");
  CHECK(find_entry(entries, "err")->text_val == "session_not_found");
}

TEST_CASE("BLE: handle_history_delete succeeds and sends deleted response") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 150, 1737000000}};
  storage_spy::delete_route_returns = true;

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_delete(10001);

  CHECK(storage_spy::last_deleted_session_id == 10001);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);

  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "deleted");
  CHECK(find_entry(entries, "session")->uint_val == 10001);
}

TEST_CASE("BLE: handle_history_delete sends error when storage delete fails") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 150, 1737000000}};
  storage_spy::delete_route_returns = false;

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.handle_history_delete(10001);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);

  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "error");
  CHECK(find_entry(entries, "err")->text_val == "delete_failed");
}

TEST_CASE("BLE: handle_history_delete ends active export for deleted session") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 150, 1737000000}};
  storage_spy::delete_route_returns = true;

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);
  BleServiceTestAccess::set_export_active(svc, true);
  BleServiceTestAccess::set_export_session_id(svc, 10001);

  svc.handle_history_delete(10001);

  CHECK(BleServiceTestAccess::export_active(svc) == false);
  CHECK(BleServiceTestAccess::export_session_id(svc) == 0);
  CHECK(storage_spy::last_deleted_session_id == 10001);

  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "deleted");
}

TEST_CASE("BLE: handle_history_delete does not end export for different session") {
  storage_spy::reset();
  storage_spy::sessions = {{10001, 150, 1737000000}};
  storage_spy::delete_route_returns = true;

  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);
  BleServiceTestAccess::set_export_active(svc, true);
  BleServiceTestAccess::set_export_session_id(svc, 20001);

  svc.handle_history_delete(10001);

  // Export for session 20001 should remain active
  CHECK(BleServiceTestAccess::export_active(svc) == true);
  CHECK(BleServiceTestAccess::export_session_id(svc) == 20001);
}

TEST_CASE("BLE: notify_history_error sends error notification") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  MockBleCharacteristic history_char;
  BleServiceTestAccess::set_history_char(svc, &history_char);
  BleServiceTestAccess::set_connected(svc, true);

  svc.notify_history_error(BLE_VAL_ERR_SESSION_ACTIVE);

  REQUIRE(!history_char.last_value.empty());
  CHECK(history_char.last_value[0] == 0x00);

  auto entries =
      decode_cbor_map(history_char.last_value.data() + 1, history_char.last_value.size() - 1);
  CHECK(find_entry(entries, "type")->text_val == "error");
  CHECK(find_entry(entries, "err")->text_val == "session_active");
}

TEST_CASE("BLE: decode_history_write decodes delete operation") {
  // Encode: {"op": "delete", "session": 10042}
  uint8_t buf[64];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);
  cbor_encode_text_stringz(&map, "op");
  cbor_encode_text_stringz(&map, "delete");
  cbor_encode_text_stringz(&map, "session");
  cbor_encode_uint(&map, 10042);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);

  BleHistoryDecodeResult result = BleService::decode_history_write(buf, len);
  CHECK(result.op == BleHistoryOp::Delete);
  CHECK(result.session_id == 10042);
}

TEST_CASE("BLE: deinit is no-op when not initialized") {
  StorageService storage(*null_cache_ptr, *null_nand_ptr);
  BleService svc(nullptr, storage, default_ble_server);
  // Should not crash
  svc.deinit();
  CHECK(svc.is_initialized() == false);
}

TEST_CASE("BLE: compute_ble_adv_name uses the serial tail") {
  char name[BLE_ADV_NAME_BUF_SIZE];

  compute_ble_adv_name("d0cf13e847e8", name, sizeof(name));
  CHECK(std::string(name) == "AirGradient Go 47e8");

  // Shorter-than-suffix and null fall back to "0000".
  compute_ble_adv_name("ab", name, sizeof(name));
  CHECK(std::string(name) == "AirGradient Go 0000");
  compute_ble_adv_name(nullptr, name, sizeof(name));
  CHECK(std::string(name) == "AirGradient Go 0000");
}
