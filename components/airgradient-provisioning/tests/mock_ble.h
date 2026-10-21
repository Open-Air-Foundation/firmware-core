/**
 * AirGradient — Mock BLE types for provisioning host tests
 *
 * Hand-written test doubles that implement the AgBleServer abstraction.
 * Captures calls for assertions and stores callbacks so tests can
 * simulate BLE events (connect, disconnect, characteristic writes).
 */

#ifndef AG_PROVISIONING_TEST_MOCK_BLE_H
#define AG_PROVISIONING_TEST_MOCK_BLE_H

#include "hal/ble_server.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// MockBleCharacteristic
// ---------------------------------------------------------------------------

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

  void set_write_callback(AgBleWriteCallback callback) override { write_cb = std::move(callback); }

  // -- Test helpers --

  // Simulate a client write to this characteristic.
  void simulate_write(const uint8_t *data, size_t len) {
    if (write_cb) {
      write_cb(data, len);
    }
  }

  void simulate_write(const char *json) {
    simulate_write(reinterpret_cast<const uint8_t *>(json), std::strlen(json));
  }

  // Last value as a string (for JSON comparison).
  std::string last_value_str() const { return std::string(last_value.begin(), last_value.end()); }

  // Value at index as string.
  std::string value_str_at(size_t idx) const {
    if (idx >= all_values.size()) {
      return {};
    }
    return std::string(all_values[idx].begin(), all_values[idx].end());
  }

  // Last notified payload as a string (for JSON comparison).
  std::string last_notified_value_str() const {
    return std::string(last_notified_value.begin(), last_notified_value.end());
  }

  void reset() {
    last_value.clear();
    all_values.clear();
    last_notified_value.clear();
    set_value_count = 0;
    notify_count = 0;
    notify_returns = true;
  }

  // -- Captured state --
  std::vector<uint8_t> last_value;
  std::vector<std::vector<uint8_t>> all_values;
  std::vector<uint8_t> last_notified_value;
  int set_value_count = 0;
  int notify_count = 0;
  bool notify_returns = true;
  AgBleWriteCallback write_cb;
};

// ---------------------------------------------------------------------------
// MockBleGattService
// ---------------------------------------------------------------------------

class MockBleGattService : public AgBleGattService {
public:
  AgBleCharacteristic *add_characteristic(const char *uuid, uint16_t properties) override {
    auto ch = std::make_unique<MockBleCharacteristic>();
    MockBleCharacteristic *raw = ch.get();
    characteristics.push_back({std::string(uuid), properties, std::move(ch)});
    return raw;
  }

  bool start() override {
    started = true;
    return true;
  }

  // Find a characteristic by UUID (test helper).
  MockBleCharacteristic *find_char(const char *uuid) {
    for (auto &entry : characteristics) {
      if (entry.uuid == uuid) {
        return entry.ch.get();
      }
    }
    return nullptr;
  }

  struct CharEntry {
    std::string uuid;
    uint16_t properties;
    std::unique_ptr<MockBleCharacteristic> ch;
  };

  std::vector<CharEntry> characteristics;
  bool started = false;
};

// ---------------------------------------------------------------------------
// MockBleServer
// ---------------------------------------------------------------------------

class MockBleServer : public AgBleServer {
public:
  bool init(const char *device_name) override {
    init_count++;
    last_device_name = device_name != nullptr ? device_name : "";
    initialized = true;
    return init_returns;
  }

  void deinit() override {
    deinit_count++;
    initialized = false;
  }

  bool set_security(AgBleIoCapability io_cap, uint8_t auth_flags) override {
    last_io_cap = io_cap;
    last_auth_flags = auth_flags;
    set_security_count++;
    return true;
  }

  bool delete_all_bonds() override {
    delete_all_bonds_count++;
    return true;
  }

  AgBleGattService *add_service(const char *uuid) override {
    auto svc = std::make_unique<MockBleGattService>();
    MockBleGattService *raw = svc.get();
    services.push_back({std::string(uuid), std::move(svc)});
    return raw;
  }

  bool set_advertising_interval(uint32_t) override { return true; }
  bool set_advertising_name(const char *name) override {
    last_adv_name = name != nullptr ? name : "";
    return true;
  }

  bool add_advertised_service_uuid(const char *uuid) override {
    advertised_uuids.push_back(uuid != nullptr ? uuid : "");
    return true;
  }

  bool set_manufacturer_data(const uint8_t *data, size_t len) override {
    last_mfg_data.assign(data, data + len);
    return true;
  }

  bool start_advertising() override {
    start_advertising_count++;
    advertising = true;
    return true;
  }

  bool stop_advertising() override {
    stop_advertising_count++;
    advertising = false;
    return true;
  }

  void set_connect_callback(AgBleConnectCallback cb) override { connect_cb = std::move(cb); }
  void set_disconnect_callback(AgBleDisconnectCallback cb) override {
    disconnect_cb = std::move(cb);
  }
  void set_passkey_display_callback(AgBlePasskeyDisplayCallback cb) override {
    passkey_cb = std::move(cb);
  }
  void set_auth_complete_callback(AgBleAuthCompleteCallback cb) override {
    auth_cb = std::move(cb);
  }

  // -- Test helpers: fire events --

  void simulate_connect(uint16_t conn_handle = 1) {
    if (connect_cb) {
      connect_cb(conn_handle);
    }
  }

  void simulate_disconnect(uint16_t conn_handle = 1, int reason = 0) {
    if (disconnect_cb) {
      disconnect_cb(conn_handle, reason);
    }
  }

  // Find a service by UUID.
  MockBleGattService *find_service(const char *uuid) {
    for (auto &entry : services) {
      if (entry.uuid == uuid) {
        return entry.svc.get();
      }
    }
    return nullptr;
  }

  // Find a characteristic by service UUID + characteristic UUID.
  MockBleCharacteristic *find_char(const char *service_uuid, const char *char_uuid) {
    MockBleGattService *svc = find_service(service_uuid);
    if (svc == nullptr) {
      return nullptr;
    }
    return svc->find_char(char_uuid);
  }

  struct ServiceEntry {
    std::string uuid;
    std::unique_ptr<MockBleGattService> svc;
  };

  // -- Captured state --
  int init_count = 0;
  int deinit_count = 0;
  int set_security_count = 0;
  int delete_all_bonds_count = 0;
  int start_advertising_count = 0;
  int stop_advertising_count = 0;
  bool init_returns = true;
  bool initialized = false;
  bool advertising = false;
  std::string last_device_name;
  std::string last_adv_name;
  AgBleIoCapability last_io_cap = AgBleIoCapability::NO_INPUT_NO_OUTPUT;
  uint8_t last_auth_flags = 0;
  std::vector<std::string> advertised_uuids;
  std::vector<uint8_t> last_mfg_data;

  std::vector<ServiceEntry> services;

  AgBleConnectCallback connect_cb;
  AgBleDisconnectCallback disconnect_cb;
  AgBlePasskeyDisplayCallback passkey_cb;
  AgBleAuthCompleteCallback auth_cb;
};

#endif // AG_PROVISIONING_TEST_MOCK_BLE_H
