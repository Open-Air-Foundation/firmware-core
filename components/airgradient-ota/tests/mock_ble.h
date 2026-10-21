/**
 * AirGradient — Mock BLE types for airgradient-ota host tests
 *
 * Hand-written test doubles that implement the AgBleServer abstraction.
 * Captures registered write callbacks and NOTIFY payloads so tests can
 * simulate Control/Data writes and assert Status notifications. Mirrors the
 * provisioning mock_ble.h pattern.
 */

#ifndef AG_OTA_TEST_MOCK_BLE_H
#define AG_OTA_TEST_MOCK_BLE_H

#include "hal/ble_server.h"

#include <cstring>
#include <memory>
#include <string>
#include <vector>

class MockBleCharacteristic : public AgBleCharacteristic {
public:
  bool set_value(const uint8_t *data, size_t len) override {
    last_value.assign(data, data + len);
    set_value_count++;
    return true;
  }

  bool notify() override {
    last_notified_value = last_value;
    all_notified.push_back(last_value);
    notify_count++;
    return notify_returns;
  }

  bool notify(const uint8_t *data, size_t len) override {
    last_notified_value.assign(data, data + len);
    all_notified.push_back(last_notified_value);
    notify_count++;
    return notify_returns;
  }

  void set_write_callback(AgBleWriteCallback callback) override { write_cb = std::move(callback); }

  // -- Test helpers --
  void simulate_write(const uint8_t *data, size_t len) {
    if (write_cb) {
      write_cb(data, len);
    }
  }

  bool has_write_callback() const { return static_cast<bool>(write_cb); }

  std::vector<uint8_t> last_value;
  std::vector<uint8_t> last_notified_value;
  std::vector<std::vector<uint8_t>> all_notified;
  int set_value_count = 0;
  int notify_count = 0;
  bool notify_returns = true;
  AgBleWriteCallback write_cb;
};

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
    return start_returns;
  }

  MockBleCharacteristic *find_char(const char *uuid) {
    for (auto &entry : characteristics) {
      if (entry.uuid == uuid) {
        return entry.ch.get();
      }
    }
    return nullptr;
  }

  uint16_t props_of(const char *uuid) {
    for (auto &entry : characteristics) {
      if (entry.uuid == uuid) {
        return entry.properties;
      }
    }
    return 0;
  }

  struct CharEntry {
    std::string uuid;
    uint16_t properties;
    std::unique_ptr<MockBleCharacteristic> ch;
  };

  std::vector<CharEntry> characteristics;
  bool started = false;
  bool start_returns = true;
};

class MockBleServer : public AgBleServer {
public:
  bool init(const char *) override { return true; }
  void deinit() override { deinit_count++; }
  bool set_security(AgBleIoCapability, uint8_t) override { return true; }
  bool delete_all_bonds() override { return true; }

  AgBleGattService *add_service(const char *uuid) override {
    if (!add_service_returns) {
      return nullptr;
    }
    auto svc = std::make_unique<MockBleGattService>();
    MockBleGattService *raw = svc.get();
    services.push_back({std::string(uuid), std::move(svc)});
    return raw;
  }

  bool set_advertising_interval(uint32_t) override { return true; }
  bool set_advertising_name(const char *) override { return true; }
  bool add_advertised_service_uuid(const char *) override { return true; }
  bool set_manufacturer_data(const uint8_t *, size_t) override { return true; }
  bool start_advertising() override { return true; }
  bool stop_advertising() override { return true; }

  bool request_conn_params(uint16_t min_interval_ms, uint16_t max_interval_ms, uint16_t latency,
                           uint16_t supervision_timeout_ms) override {
    conn_param_requests.push_back(
        {min_interval_ms, max_interval_ms, latency, supervision_timeout_ms});
    return true;
  }
  void set_connect_callback(AgBleConnectCallback) override {}
  void set_disconnect_callback(AgBleDisconnectCallback) override {}
  void set_passkey_display_callback(AgBlePasskeyDisplayCallback) override {}
  void set_auth_complete_callback(AgBleAuthCompleteCallback) override {}

  MockBleGattService *find_service(const char *uuid) {
    for (auto &entry : services) {
      if (entry.uuid == uuid) {
        return entry.svc.get();
      }
    }
    return nullptr;
  }

  MockBleCharacteristic *find_char(const char *service_uuid, const char *char_uuid) {
    MockBleGattService *svc = find_service(service_uuid);
    return svc != nullptr ? svc->find_char(char_uuid) : nullptr;
  }

  struct ServiceEntry {
    std::string uuid;
    std::unique_ptr<MockBleGattService> svc;
  };

  // Records each request_conn_params() call so tests can assert the OTA service
  // brackets a transfer with the fast/relaxed windows.
  struct ConnParamRequest {
    uint16_t min_interval_ms;
    uint16_t max_interval_ms;
    uint16_t latency;
    uint16_t supervision_timeout_ms;
  };

  std::vector<ServiceEntry> services;
  std::vector<ConnParamRequest> conn_param_requests;
  int deinit_count = 0;
  bool add_service_returns = true;
};

#endif // AG_OTA_TEST_MOCK_BLE_H
