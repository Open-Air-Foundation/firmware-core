/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include <cJSON.h>

#include "hal/ble_server.h"
#include "internal/ble_transport.h"
#include "mock_ble.h"

namespace {

// GATT UUIDs from the spec.
constexpr const char *PROV_SERVICE_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
constexpr const char *CRED_STATUS_CHAR_UUID = "703fa252-3d2a-4da9-a05c-83b0d9cacb8e";
constexpr const char *SCAN_CHAR_UUID = "467a080f-e50f-42c9-b9b2-a2ab14d82725";
constexpr const char *DIS_UUID = "180A";

ProvisioningBleConfig basic_ble_config() {
  ProvisioningBleConfig cfg;
  cfg.device_name = "AirGradient";
  cfg.manufacturer_data = "I-9PSL#AABBCCDD";
  cfg.model_name = "I-9PSL";
  cfg.serial_number = "AABBCCDD";
  cfg.firmware_version = "1.0.0";
  return cfg;
}

// JSON parse helper.
cJSON *parse_value(const MockBleCharacteristic *ch) {
  if (ch == nullptr || ch->last_value.empty()) {
    return nullptr;
  }
  return cJSON_ParseWithLength(reinterpret_cast<const char *>(ch->last_value.data()),
                               ch->last_value.size());
}

int get_int(cJSON *root, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  return cJSON_IsNumber(item) ? static_cast<int>(item->valuedouble) : -9999;
}

std::string get_string(cJSON *root, const char *key) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
  if (cJSON_IsString(item) && item->valuestring != nullptr) {
    return item->valuestring;
  }
  return {};
}

// Parse notification at index from all_values.
cJSON *parse_value_at(const MockBleCharacteristic *ch, size_t idx) {
  if (ch == nullptr || idx >= ch->all_values.size()) {
    return nullptr;
  }
  const auto &v = ch->all_values[idx];
  return cJSON_ParseWithLength(reinterpret_cast<const char *>(v.data()), v.size());
}

} // namespace

// ============================================================================
// Setup / teardown
// ============================================================================

TEST_CASE("BleTransport: setup creates provisioning service and DIS", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;

  REQUIRE(transport.setup(ble, basic_ble_config()));

  // BLE server was initialised and is advertising.
  REQUIRE(ble.init_count == 1);
  REQUIRE(ble.last_device_name == "AirGradient");
  REQUIRE(ble.set_security_count == 1);
  REQUIRE(ble.last_io_cap == AgBleIoCapability::NO_INPUT_NO_OUTPUT);
  REQUIRE((ble.last_auth_flags & AgBleAuth::BOND) != 0);
  REQUIRE((ble.last_auth_flags & AgBleAuth::SC) != 0);
  REQUIRE(ble.start_advertising_count == 1);
  REQUIRE(ble.advertising);

  // Provisioning service exists with two characteristics.
  MockBleGattService *prov_svc = ble.find_service(PROV_SERVICE_UUID);
  REQUIRE(prov_svc != nullptr);
  REQUIRE(prov_svc->started);
  REQUIRE(prov_svc->find_char(CRED_STATUS_CHAR_UUID) != nullptr);
  REQUIRE(prov_svc->find_char(SCAN_CHAR_UUID) != nullptr);

  // DIS exists.
  MockBleGattService *dis = ble.find_service(DIS_UUID);
  REQUIRE(dis != nullptr);
  REQUIRE(dis->started);
  REQUIRE(dis->characteristics.size() == 4);
  const struct {
    const char *uuid;
    const char *value;
  } identity[] = {
      {"2A24", "I-9PSL"}, {"2A25", "AABBCCDD"}, {"2A26", "1.0.0"}, {"2A29", "AirGradient"}};
  for (const auto &field : identity) {
    auto *characteristic = dis->find_char(field.uuid);
    REQUIRE(characteristic != nullptr);
    REQUIRE(characteristic->last_value_str() == field.value);
  }
  for (const auto &characteristic : dis->characteristics) {
    REQUIRE(characteristic.properties == (AgBleProperty::READ | AgBleProperty::READ_ENC));
  }

  // Manufacturer data was set (2-byte company ID + payload).
  REQUIRE_FALSE(ble.last_mfg_data.empty());
  REQUIRE(ble.last_mfg_data[0] == 0xFF);
  REQUIRE(ble.last_mfg_data[1] == 0xFF);

  transport.teardown();
  REQUIRE(ble.deinit_count == 1);
}

TEST_CASE("BleTransport: missing identity fields remain empty", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;
  ProvisioningBleConfig config;

  REQUIRE(transport.setup(ble, config));
  auto *dis = ble.find_service(DIS_UUID);
  REQUIRE(dis != nullptr);
  REQUIRE(dis->started);
  for (const char *uuid : {"2A24", "2A25", "2A26"}) {
    auto *characteristic = dis->find_char(uuid);
    REQUIRE(characteristic != nullptr);
    REQUIRE(characteristic->last_value.empty());
  }
}

TEST_CASE("BleTransport: incomplete device information prevents advertising", "[ble_transport]") {
  class FailedDisService : public MockBleGattService {
  public:
    AgBleCharacteristic *add_characteristic(const char *, uint16_t) override { return nullptr; }
  };
  class FailedDisServer : public MockBleServer {
  public:
    AgBleGattService *add_service(const char *uuid) override {
      return std::strcmp(uuid, DIS_UUID) == 0 ? &dis : MockBleServer::add_service(uuid);
    }
    FailedDisService dis;
  } ble;
  BleTransport transport;

  REQUIRE_FALSE(transport.setup(ble, basic_ble_config()));
  REQUIRE(ble.start_advertising_count == 0);
  REQUIRE(ble.deinit_count == 1);
}

TEST_CASE("BleTransport: SC-only auth flags propagate (AGo Stationary)", "[ble_transport]") {
  // AGo Stationary provisioning opts out of bonding so a one-shot
  // onboarding session does not leave a phone-side bond behind.
  MockBleServer ble;
  BleTransport transport;

  ProvisioningBleConfig cfg = basic_ble_config();
  cfg.io_capability = AgBleIoCapability::NO_INPUT_NO_OUTPUT;
  cfg.auth_flags = AgBleAuth::SC;

  REQUIRE(transport.setup(ble, cfg));

  REQUIRE(ble.set_security_count == 1);
  REQUIRE(ble.last_io_cap == AgBleIoCapability::NO_INPUT_NO_OUTPUT);
  REQUIRE(ble.last_auth_flags == AgBleAuth::SC);
  REQUIRE((ble.last_auth_flags & AgBleAuth::BOND) == 0);
  REQUIRE((ble.last_auth_flags & AgBleAuth::MITM) == 0);

  transport.teardown();
}

TEST_CASE("BleTransport: teardown is idempotent", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;

  REQUIRE(transport.setup(ble, basic_ble_config()));
  transport.teardown();
  transport.teardown(); // second call must not crash or double-deinit
  REQUIRE(ble.deinit_count == 1);
}

TEST_CASE("BleTransport: setup fails if init fails", "[ble_transport]") {
  MockBleServer ble;
  ble.init_returns = false;
  BleTransport transport;

  REQUIRE_FALSE(transport.setup(ble, basic_ble_config()));
}

// ============================================================================
// Credential write
// ============================================================================

TEST_CASE("BleTransport: credential write triggers callback", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;

  bool cred_received = false;
  ProvisioningData received_data;
  transport.set_on_credentials([&](const ProvisioningData &d) {
    cred_received = true;
    received_data = d;
    return true;
  });

  REQUIRE(transport.setup(ble, basic_ble_config()));

  // Write credentials to the characteristic.
  MockBleCharacteristic *cred_char = ble.find_char(PROV_SERVICE_UUID, CRED_STATUS_CHAR_UUID);
  REQUIRE(cred_char != nullptr);

  cred_char->simulate_write(R"({"ssid":"TestNet","password":"pw123456","disableCloud":true})");

  REQUIRE(cred_received);
  REQUIRE(std::string(received_data.ssid) == "TestNet");
  REQUIRE(std::string(received_data.password) == "pw123456");
  REQUIRE(received_data.disable_cloud == true);

  transport.teardown();
}

TEST_CASE("BleTransport: malformed credential write is silently rejected", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;

  bool cred_received = false;
  transport.set_on_credentials([&](const ProvisioningData &) {
    cred_received = true;
    return true;
  });

  REQUIRE(transport.setup(ble, basic_ble_config()));

  MockBleCharacteristic *cred_char = ble.find_char(PROV_SERVICE_UUID, CRED_STATUS_CHAR_UUID);
  cred_char->simulate_write("garbage");

  REQUIRE_FALSE(cred_received);
  transport.teardown();
}

// ============================================================================
// Scan write / pagination
// ============================================================================

TEST_CASE("BleTransport: scan write triggers scan callback", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;

  bool scan_requested = false;
  transport.set_on_scan_request([&]() {
    scan_requested = true;
    return true;
  });

  REQUIRE(transport.setup(ble, basic_ble_config()));

  MockBleCharacteristic *scan_char = ble.find_char(PROV_SERVICE_UUID, SCAN_CHAR_UUID);
  scan_char->simulate_write("1"); // any value triggers scan

  REQUIRE(scan_requested);
  transport.teardown();
}

TEST_CASE("BleTransport: empty scan results send found=0", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;
  REQUIRE(transport.setup(ble, basic_ble_config()));

  MockBleCharacteristic *scan_char = ble.find_char(PROV_SERVICE_UUID, SCAN_CHAR_UUID);
  REQUIRE(scan_char != nullptr);

  // Simulate scan complete with no results.
  transport.update_scan_results(nullptr, 0);

  REQUIRE(scan_char->notify_count == 1);
  cJSON *root = parse_value(scan_char);
  REQUIRE(root != nullptr);
  REQUIRE(get_int(root, "found") == 0);
  cJSON_Delete(root);

  transport.teardown();
}

TEST_CASE("BleTransport: scan results paginate via timer", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;
  REQUIRE(transport.setup(ble, basic_ble_config()));

  MockBleCharacteristic *scan_char = ble.find_char(PROV_SERVICE_UUID, SCAN_CHAR_UUID);

  // Create 7 scan entries → 3 pages (3, 3, 1).
  WifiScanEntry entries[7] = {};
  for (int i = 0; i < 7; ++i) {
    snprintf(entries[i].ssid, sizeof(entries[i].ssid), "Net%d", i);
    entries[i].rssi = static_cast<int8_t>(-30 - i);
    entries[i].auth_mode = WifiAuthMode::wpa2_psk;
    entries[i].channel = 6;
  }

  transport.update_scan_results(entries, 7);

  // Timer fired immediately (0ms) — first page should be sent.
  transport.pagination_timer().fire_for_test();
  REQUIRE(scan_char->notify_count == 1);
  REQUIRE(transport.has_more_scan_pages());

  // Fire second page.
  transport.pagination_timer().fire_for_test();
  REQUIRE(scan_char->notify_count == 2);
  REQUIRE(transport.has_more_scan_pages());

  // Fire third (last) page.
  transport.pagination_timer().fire_for_test();
  REQUIRE(scan_char->notify_count == 3);
  REQUIRE_FALSE(transport.has_more_scan_pages());

  // Verify the last page's JSON structure.
  cJSON *root = parse_value(scan_char);
  REQUIRE(root != nullptr);
  REQUIRE(get_int(root, "page") == 3);
  REQUIRE(get_int(root, "tpage") == 3);
  REQUIRE(get_int(root, "found") == 7);
  cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
  REQUIRE(cJSON_GetArraySize(wifi) == 1); // last page has 1 entry
  cJSON_Delete(root);

  transport.teardown();
}

// ============================================================================
// Status notification
// ============================================================================

TEST_CASE("BleTransport: send_status notifies credentials characteristic", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;
  REQUIRE(transport.setup(ble, basic_ble_config()));

  MockBleCharacteristic *cred_char = ble.find_char(PROV_SERVICE_UUID, CRED_STATUS_CHAR_UUID);

  transport.send_status(0); // WIFI_CONNECTED
  REQUIRE(cred_char->notify_count == 1);

  cJSON *root = parse_value(cred_char);
  REQUIRE(root != nullptr);
  REQUIRE(get_int(root, "status") == 0);
  cJSON_Delete(root);

  transport.send_status(10); // WIFI_CONNECT_FAILED
  REQUIRE(cred_char->notify_count == 2);

  root = parse_value(cred_char);
  REQUIRE(root != nullptr);
  REQUIRE(get_int(root, "status") == 10);
  cJSON_Delete(root);

  transport.teardown();
}

// ============================================================================
// Client connect / disconnect callbacks
// ============================================================================

TEST_CASE("BleTransport: connect stops advertising, disconnect restarts it", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;

  int connects = 0;
  int disconnects = 0;
  transport.set_on_client_connected([&]() { ++connects; });
  transport.set_on_client_disconnected([&]() { ++disconnects; });

  REQUIRE(transport.setup(ble, basic_ble_config()));

  // setup() called start_advertising once.
  REQUIRE(ble.start_advertising_count == 1);
  REQUIRE(ble.stop_advertising_count == 0);

  // Connect: advertising stops.
  ble.simulate_connect(1);
  REQUIRE(connects == 1);
  REQUIRE(ble.stop_advertising_count == 1);
  REQUIRE_FALSE(ble.advertising);

  // Disconnect: advertising restarts.
  ble.simulate_disconnect(1, 0);
  REQUIRE(disconnects == 1);
  REQUIRE(ble.start_advertising_count == 2);
  REQUIRE(ble.advertising);

  transport.teardown();
}

TEST_CASE("BleTransport: teardown during active connection does not restart advertising",
          "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;
  REQUIRE(transport.setup(ble, basic_ble_config()));

  // Connect a client.
  ble.simulate_connect(1);
  REQUIRE(ble.stop_advertising_count == 1);
  REQUIRE(ble.start_advertising_count == 1); // only the initial setup

  // Teardown while connected. AgBleServer::deinit() clears its own
  // callbacks before disconnecting peers, so _on_disconnect() never
  // fires and start_advertising() is not called.
  transport.teardown();
  REQUIRE(ble.deinit_count == 1);
  REQUIRE(ble.start_advertising_count == 1); // unchanged — no wasteful restart
}

// ============================================================================
// Scan page JSON structure (verifies encode helpers via transport)
// ============================================================================

TEST_CASE("BleTransport: scan page JSON contains ssid, rssi, open flag", "[ble_transport]") {
  MockBleServer ble;
  BleTransport transport;
  REQUIRE(transport.setup(ble, basic_ble_config()));

  MockBleCharacteristic *scan_char = ble.find_char(PROV_SERVICE_UUID, SCAN_CHAR_UUID);

  WifiScanEntry entries[2] = {};
  std::strncpy(entries[0].ssid, "HomeWiFi", sizeof(entries[0].ssid) - 1);
  entries[0].rssi = -45;
  entries[0].auth_mode = WifiAuthMode::wpa2_psk;
  std::strncpy(entries[1].ssid, "Guest", sizeof(entries[1].ssid) - 1);
  entries[1].rssi = -62;
  entries[1].auth_mode = WifiAuthMode::open;

  transport.update_scan_results(entries, 2);
  transport.pagination_timer().fire_for_test();
  REQUIRE(scan_char->notify_count == 1);

  // Single page (2 entries < 3 per page).
  cJSON *root = parse_value(scan_char);
  REQUIRE(root != nullptr);
  REQUIRE(get_int(root, "page") == 1);
  REQUIRE(get_int(root, "tpage") == 1);
  REQUIRE(get_int(root, "found") == 2);

  cJSON *wifi = cJSON_GetObjectItemCaseSensitive(root, "wifi");
  REQUIRE(cJSON_IsArray(wifi));
  REQUIRE(cJSON_GetArraySize(wifi) == 2);

  cJSON *first = cJSON_GetArrayItem(wifi, 0);
  REQUIRE(get_string(first, "s") == "HomeWiFi");
  REQUIRE(get_int(first, "r") == -45);
  REQUIRE(get_int(first, "o") == 0); // secured

  cJSON *second = cJSON_GetArrayItem(wifi, 1);
  REQUIRE(get_string(second, "s") == "Guest");
  REQUIRE(get_int(second, "r") == -62);
  REQUIRE(get_int(second, "o") == 1); // open

  cJSON_Delete(root);
  transport.teardown();
}

// ============================================================================
// Attached mode: setup_on_server() / detach()
// ============================================================================

TEST_CASE("BleTransport: setup_on_server registers provisioning without duplicating owner services",
          "[ble_transport][attached]") {
  MockBleServer ble;
  // The owner already initialised the server (BleService path).
  REQUIRE(ble.init("AirGradient Go ef0e"));
  const int init_count_before = ble.init_count;

  BleTransport transport;
  REQUIRE(transport.setup_on_server(ble, basic_ble_config()));

  // No second init, no set_security, no advertising — the owner did/does those.
  REQUIRE(ble.init_count == init_count_before);
  REQUIRE(ble.set_security_count == 0);
  REQUIRE(ble.start_advertising_count == 0);
  REQUIRE(ble.last_mfg_data.empty());

  // Single-owner: connect/disconnect callbacks are NOT installed by the
  // attached transport (they belong to the owning service).
  REQUIRE_FALSE(static_cast<bool>(ble.connect_cb));
  REQUIRE_FALSE(static_cast<bool>(ble.disconnect_cb));

  // The provisioning service is registered and started.
  MockBleGattService *prov_svc = ble.find_service(PROV_SERVICE_UUID);
  REQUIRE(prov_svc != nullptr);
  REQUIRE(prov_svc->started);
  REQUIRE(prov_svc->find_char(CRED_STATUS_CHAR_UUID) != nullptr);
  REQUIRE(prov_svc->find_char(SCAN_CHAR_UUID) != nullptr);

  // The owner already supplies Device Information on the shared server.
  CHECK(ble.find_service(DIS_UUID) == nullptr);

  transport.detach();
}

TEST_CASE("BleTransport: detach clears char callbacks and does NOT deinit the server",
          "[ble_transport][attached]") {
  MockBleServer ble;
  REQUIRE(ble.init("AirGradient Go ef0e"));

  BleTransport transport;
  bool cred_received = false;
  transport.set_on_credentials([&](const ProvisioningData &) {
    cred_received = true;
    return true;
  });
  REQUIRE(transport.setup_on_server(ble, basic_ble_config()));

  MockBleCharacteristic *cred_char = ble.find_char(PROV_SERVICE_UUID, CRED_STATUS_CHAR_UUID);
  REQUIRE(cred_char != nullptr);

  transport.detach();

  // Borrowed server is left intact — detach must never deinit it.
  REQUIRE(ble.deinit_count == 0);

  // Char write callback was cleared: a late write does not re-enter us.
  cred_received = false;
  cred_char->simulate_write(R"({"ssid":"X","password":"pw123456"})");
  REQUIRE_FALSE(cred_received);

  // Idempotent.
  transport.detach();
  REQUIRE(ble.deinit_count == 0);
}

TEST_CASE("BleTransport: attached scan pagination still works", "[ble_transport][attached]") {
  MockBleServer ble;
  REQUIRE(ble.init("AirGradient Go ef0e"));

  BleTransport transport;
  REQUIRE(transport.setup_on_server(ble, basic_ble_config()));

  MockBleCharacteristic *scan_char = ble.find_char(PROV_SERVICE_UUID, SCAN_CHAR_UUID);
  REQUIRE(scan_char != nullptr);

  transport.update_scan_results(nullptr, 0);
  REQUIRE(scan_char->notify_count == 1);
  cJSON *root = parse_value(scan_char);
  REQUIRE(root != nullptr);
  REQUIRE(get_int(root, "found") == 0);
  cJSON_Delete(root);

  transport.detach();
}
