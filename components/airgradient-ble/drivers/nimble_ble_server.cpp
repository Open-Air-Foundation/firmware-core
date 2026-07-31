/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "nimble_ble_server.h"

#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

namespace {

// Maps AgBleProperty flags to the corresponding NIMBLE_PROPERTY bitmask.
// NIMBLE_PROPERTY is an unscoped enum; its enumerators are in global scope.
uint32_t to_nimble_properties(uint16_t props) {
  uint32_t result = 0;
  if (props & AgBleProperty::READ)
    result |= READ;
  if (props & AgBleProperty::WRITE)
    result |= WRITE;
  if (props & AgBleProperty::WRITE_NR)
    result |= WRITE_NR;
  if (props & AgBleProperty::NOTIFY)
    result |= NOTIFY;
  if (props & AgBleProperty::INDICATE)
    result |= INDICATE;
  if (props & AgBleProperty::READ_ENC)
    result |= READ_ENC;
  if (props & AgBleProperty::READ_AUTHEN)
    result |= READ_AUTHEN;
  if (props & AgBleProperty::WRITE_ENC)
    result |= WRITE_ENC;
  if (props & AgBleProperty::WRITE_AUTHEN)
    result |= WRITE_AUTHEN;
  return result;
}

// Maps AgBleIoCapability to the NimBLE BLE_HS_IO_* constant.
uint8_t to_nimble_io_cap(AgBleIoCapability io_cap) {
  switch (io_cap) {
  case AgBleIoCapability::DISPLAY_ONLY:
    return BLE_HS_IO_DISPLAY_ONLY;
  case AgBleIoCapability::DISPLAY_YES_NO:
    return BLE_HS_IO_DISPLAY_YESNO;
  case AgBleIoCapability::KEYBOARD_ONLY:
    return BLE_HS_IO_KEYBOARD_ONLY;
  case AgBleIoCapability::NO_INPUT_NO_OUTPUT:
    return BLE_HS_IO_NO_INPUT_OUTPUT;
  case AgBleIoCapability::KEYBOARD_DISPLAY:
    return BLE_HS_IO_KEYBOARD_DISPLAY;
  }
  return BLE_HS_IO_NO_INPUT_OUTPUT;
}

} // namespace

// NimbleWriteBridge

NimbleWriteBridge::NimbleWriteBridge(AgBleWriteCallback callback)
    : _callback(std::move(callback)) {}

void NimbleWriteBridge::onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) {
  (void)connInfo;
  if (_callback && pCharacteristic != nullptr) {
    const NimBLEAttValue val = pCharacteristic->getValue();
    _callback(val.data(), val.size());
  }
}

// NimbleBleCharacteristic

NimbleBleCharacteristic::NimbleBleCharacteristic(NimBLECharacteristic *characteristic)
    : _characteristic(characteristic) {}

bool NimbleBleCharacteristic::set_value(const uint8_t *data, size_t len) {
  if (_characteristic == nullptr) {
    return false;
  }
  _characteristic->setValue(data, len);
  return true;
}

bool NimbleBleCharacteristic::notify() {
  if (_characteristic == nullptr) {
    return false;
  }
  return _characteristic->notify();
}

bool NimbleBleCharacteristic::notify(const uint8_t *data, size_t len) {
  if (_characteristic == nullptr || data == nullptr) {
    return false;
  }
  // Sends the given buffer without touching the stored value (READ snapshot).
  return _characteristic->notify(data, len);
}

void NimbleBleCharacteristic::set_write_callback(AgBleWriteCallback callback) {
  if (_characteristic == nullptr) {
    return;
  }
  _write_bridge =
      std::unique_ptr<NimbleWriteBridge>(new (std::nothrow) NimbleWriteBridge(std::move(callback)));
  if (_write_bridge != nullptr) {
    _characteristic->setCallbacks(_write_bridge.get());
  }
}

// NimbleBleGattService

NimbleBleGattService::NimbleBleGattService(NimBLEService *service) : _service(service) {}

AgBleCharacteristic *NimbleBleGattService::add_characteristic(const char *uuid,
                                                              uint16_t properties) {
  if (_service == nullptr) {
    return nullptr;
  }

  const uint32_t nimble_props = to_nimble_properties(properties);
  NimBLECharacteristic *ch = _service->createCharacteristic(uuid, nimble_props);
  if (ch == nullptr) {
    return nullptr;
  }

  auto wrapper =
      std::unique_ptr<NimbleBleCharacteristic>(new (std::nothrow) NimbleBleCharacteristic(ch));
  if (wrapper == nullptr) {
    return nullptr;
  }

  NimbleBleCharacteristic *raw = wrapper.get();
  _characteristics.push_back(std::move(wrapper));
  return raw;
}

bool NimbleBleGattService::start() {
  if (_service == nullptr) {
    return false;
  }

  return _service->start();
}

// NimbleBleServer

NimbleBleServer::~NimbleBleServer() { deinit(); }

bool NimbleBleServer::init(const char *device_name) {
  if (_server != nullptr) {
    return false;
  }

  if (!NimBLEDevice::init(device_name)) {
    return false;
  }

  // Cache for re-application after server start (ESP-IDF #18489).
  _device_name[0] = '\0';
  if (device_name != nullptr) {
    std::snprintf(_device_name, sizeof(_device_name), "%s", device_name);
  }

  _server = NimBLEDevice::createServer();
  if (_server == nullptr) {
    NimBLEDevice::deinit(true);
    return false;
  }

  // Pass false so NimBLE does not attempt to delete this object's callbacks
  // pointer (this NimbleBleServer IS the callbacks object).
  _server->setCallbacks(this, false);
  return true;
}

bool NimbleBleServer::set_security(AgBleIoCapability io_cap, uint8_t auth_flags) {
  if (_server == nullptr) {
    return false;
  }

  NimBLEDevice::setSecurityIOCap(to_nimble_io_cap(io_cap));
  NimBLEDevice::setSecurityAuth((auth_flags & AgBleAuth::BOND) != 0,
                                (auth_flags & AgBleAuth::MITM) != 0,
                                (auth_flags & AgBleAuth::SC) != 0);
  return true;
}

bool NimbleBleServer::delete_all_bonds() { return NimBLEDevice::deleteAllBonds(); }

void NimbleBleServer::deinit() {
  if (_server == nullptr) {
    return;
  }

  NimBLEDevice::stopAdvertising();

  // Clear application callbacks before disconnecting peers. The disconnects
  // below are an internal teardown step, not external events — callers must
  // not receive callbacks during deinit().
  _connect_callback = nullptr;
  _disconnect_callback = nullptr;
  _passkey_display_callback = nullptr;
  _numeric_comparison_callback = nullptr;
  _auth_complete_callback = nullptr;

  // Disconnect all active clients before teardown. Without this, pending GAP
  // events (e.g. BLE_GAP_EVENT_SUBSCRIBE) may reference freed NimBLE objects
  // after NimBLEDevice::deinit() deletes the server, causing a use-after-free.
  for (auto handle : _server->getPeerDevices()) {
    _server->disconnect(handle);
  }

  // Wait (bounded) for the NimBLE host task to process the disconnections so
  // it drains any queued GAP events that reference our characteristics.
  static constexpr int DISCONNECT_TIMEOUT_MS = 1000;
  static constexpr int DISCONNECT_POLL_MS = 10;
  int waited_ms = 0;
  while (_server->getConnectedCount() > 0 && waited_ms < DISCONNECT_TIMEOUT_MS) {
    vTaskDelay(pdMS_TO_TICKS(DISCONNECT_POLL_MS));
    waited_ms += DISCONNECT_POLL_MS;
  }

  _numeric_comparison_handle.store(BLE_HS_CONN_HANDLE_NONE);
  _services.clear();
  _server = nullptr;
  NimBLEDevice::deinit(true);
}

AgBleGattService *NimbleBleServer::add_service(const char *uuid) {
  if (_server == nullptr) {
    return nullptr;
  }

  NimBLEService *svc = _server->createService(uuid);
  if (svc == nullptr) {
    return nullptr;
  }

  auto wrapper =
      std::unique_ptr<NimbleBleGattService>(new (std::nothrow) NimbleBleGattService(svc));
  if (wrapper == nullptr) {
    return nullptr;
  }

  NimbleBleGattService *raw = wrapper.get();
  _services.push_back(std::move(wrapper));
  return raw;
}

bool NimbleBleServer::set_advertising_name(const char *name) {
  if (_server == nullptr) {
    return false;
  }

  if (name == nullptr) {
    return true;
  }

  if (name[0] == '\0') {
    return false;
  }

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  if (advertising == nullptr) {
    return false;
  }

  // Keep UUID in the advertisement and move the full name to scan response.
  advertising->enableScanResponse(true);
  return advertising->setName(name);
}

bool NimbleBleServer::add_advertised_service_uuid(const char *uuid) {
  if (_server == nullptr || uuid == nullptr || uuid[0] == '\0') {
    return false;
  }

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  if (advertising == nullptr) {
    return false;
  }

  return advertising->addServiceUUID(uuid);
}

bool NimbleBleServer::set_manufacturer_data(const uint8_t *data, size_t len) {
  if (_server == nullptr || data == nullptr || len == 0) {
    return false;
  }

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  if (advertising == nullptr) {
    return false;
  }

  return advertising->setManufacturerData(data, len);
}

bool NimbleBleServer::start_advertising() {
  if (_server == nullptr) {
    return false;
  }

  // start() registers all services with the GATT layer (returns void). Its
  // resetGATT() clobbers the GAP device name to the compile-time default under
  // CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC (ESP-IDF #18489).
  _server->start();

  // Restore the cached name before advertising makes the device connectable,
  // so clients read the correct 0x2A00 on first read and after reconnect.
  if (_device_name[0] != '\0') {
    NimBLEDevice::setDeviceName(_device_name);
  }

  return NimBLEDevice::startAdvertising();
}

bool NimbleBleServer::stop_advertising() {
  if (_server == nullptr) {
    return false;
  }

  return NimBLEDevice::stopAdvertising();
}

bool NimbleBleServer::request_conn_params(uint16_t min_interval_ms, uint16_t max_interval_ms,
                                          uint16_t latency, uint16_t supervision_timeout_ms) {
  if (_server == nullptr) {
    return false;
  }

  const std::vector<uint16_t> peers = _server->getPeerDevices();
  if (peers.empty()) {
    return false;
  }

  // BLE units: connection interval is 1.25 ms/step, supervision timeout is
  // 10 ms/step. Convert from milliseconds (round to nearest step).
  const uint16_t itvl_min = static_cast<uint16_t>((min_interval_ms * 4 + 2) / 5);
  const uint16_t itvl_max = static_cast<uint16_t>((max_interval_ms * 4 + 2) / 5);
  const uint16_t timeout = static_cast<uint16_t>((supervision_timeout_ms + 5) / 10);

  // A hint to each central; NimBLE logs (but does not surface) a per-peer error.
  for (const uint16_t handle : peers) {
    _server->updateConnParams(handle, itvl_min, itvl_max, latency, timeout);
  }
  return true;
}

void NimbleBleServer::set_connect_callback(AgBleConnectCallback callback) {
  _connect_callback = std::move(callback);
}

void NimbleBleServer::set_disconnect_callback(AgBleDisconnectCallback callback) {
  _disconnect_callback = std::move(callback);
}

void NimbleBleServer::set_passkey_display_callback(AgBlePasskeyDisplayCallback callback) {
  _passkey_display_callback = std::move(callback);
}

void NimbleBleServer::set_numeric_comparison_callback(AgBleNumericComparisonCallback callback) {
  _numeric_comparison_callback = std::move(callback);
}

bool NimbleBleServer::confirm_numeric_comparison(uint16_t conn_handle, bool accept) {
  if (_server == nullptr || _numeric_comparison_handle.load() != conn_handle) {
    return false;
  }

  const NimBLEConnInfo peer_info = _server->getPeerInfoByHandle(conn_handle);
  if (peer_info.getConnHandle() != conn_handle ||
      !NimBLEDevice::injectConfirmPasskey(peer_info, accept)) {
    return false;
  }

  _numeric_comparison_handle.store(BLE_HS_CONN_HANDLE_NONE);
  return true;
}

void NimbleBleServer::set_auth_complete_callback(AgBleAuthCompleteCallback callback) {
  _auth_complete_callback = std::move(callback);
}

void NimbleBleServer::onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) {
  (void)pServer;
  if (_connect_callback) {
    _connect_callback(connInfo.getConnHandle());
  }
}

void NimbleBleServer::onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) {
  (void)pServer;
  const uint16_t conn_handle = connInfo.getConnHandle();
  uint16_t expected = conn_handle;
  (void)_numeric_comparison_handle.compare_exchange_strong(expected, BLE_HS_CONN_HANDLE_NONE);
  if (_disconnect_callback) {
    _disconnect_callback(conn_handle, reason);
  }
}

uint8_t NimbleBleServer::connected_client_count() const {
  return _server == nullptr ? 0 : static_cast<uint8_t>(_server->getConnectedCount());
}

bool NimbleBleServer::is_peer_authenticated() const {
  if (_server == nullptr) {
    return false;
  }

  for (const uint16_t handle : _server->getPeerDevices()) {
    if (_server->getPeerInfoByHandle(handle).isAuthenticated()) {
      return true;
    }
  }
  return false;
}

uint32_t NimbleBleServer::onPassKeyDisplay() {
  // Generate a random 6-digit passkey (000000–999999).
  const uint32_t passkey = esp_random() % 1000000;

  if (_passkey_display_callback) {
    _passkey_display_callback(passkey);
  }

  return passkey;
}

void NimbleBleServer::onConfirmPassKey(NimBLEConnInfo &connInfo, uint32_t pin) {
  const uint16_t conn_handle = connInfo.getConnHandle();
  _numeric_comparison_handle.store(conn_handle);
  if (_numeric_comparison_callback) {
    _numeric_comparison_callback(conn_handle, pin);
  }
}

void NimbleBleServer::onAuthenticationComplete(NimBLEConnInfo &connInfo) {
  if (_auth_complete_callback) {
    _auth_complete_callback(connInfo.getConnHandle(), connInfo.isEncrypted());
  }
}
