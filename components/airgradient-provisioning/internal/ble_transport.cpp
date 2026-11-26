/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "ble_transport.h"

#include <cstring>
#include <vector>

#include <cJSON.h>

#include "ag_log.h"
#include "hal/ble_server.h"
#include "provisioning_json.h"
#include "scan_filter.h"
#include "services/ble_device_information.h"

namespace {

constexpr const char *TAG = "BleProv";

// GATT UUIDs — AirGradient Provisioning Service
constexpr const char *PROV_SERVICE_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
constexpr const char *CRED_STATUS_CHAR_UUID = "703fa252-3d2a-4da9-a05c-83b0d9cacb8e";
constexpr const char *SCAN_CHAR_UUID = "467a080f-e50f-42c9-b9b2-a2ab14d82725";

constexpr const char *DEFAULT_MANUFACTURER = "AirGradient";

// BLE company ID for manufacturer data (0xFFFF = unregistered).
constexpr uint8_t MFG_COMPANY_ID_LO = 0xFF;
constexpr uint8_t MFG_COMPANY_ID_HI = 0xFF;

// Maximum size for a single BLE notification payload (JSON).
constexpr size_t BLE_NOTIFY_BUF_SIZE = 512;

// Number of networks per BLE scan notification page (spec constant).
constexpr size_t NETWORKS_PER_PAGE = 3;

// ---------------------------------------------------------------------------
// BLE JSON encode helpers (private to this translation unit)
// ---------------------------------------------------------------------------

// Render a cJSON tree into a byte buffer. Returns bytes written, 0 on error.
size_t render_json(cJSON *root, uint8_t *buf, size_t buf_size) {
  char *encoded = cJSON_PrintUnformatted(root);
  if (encoded == nullptr) {
    return 0;
  }
  size_t len = std::strlen(encoded);
  if (len >= buf_size) {
    AG_LOGW(TAG, "JSON output (%u) exceeds buffer (%u)", static_cast<unsigned>(len),
            static_cast<unsigned>(buf_size));
    cJSON_free(encoded);
    return 0;
  }
  std::memcpy(buf, encoded, len);
  cJSON_free(encoded);
  return len;
}

// Encode one page of BLE scan results as JSON.
size_t encode_scan_page(const WifiScanEntry *entries, size_t entries_count, size_t page,
                        size_t total_pages, size_t total_found, uint8_t *buf, size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return 0;
  }

  cJSON *wifi = cJSON_CreateArray();
  if (wifi == nullptr) {
    cJSON_Delete(root);
    return 0;
  }

  for (size_t i = 0; i < entries_count; ++i) {
    cJSON *entry = cJSON_CreateObject();
    if (entry == nullptr) {
      break;
    }
    cJSON_AddStringToObject(entry, "s", entries[i].ssid);
    cJSON_AddNumberToObject(entry, "r", static_cast<double>(entries[i].rssi));
    int open_flag = (entries[i].auth_mode == WifiAuthMode::open) ? 1 : 0;
    cJSON_AddNumberToObject(entry, "o", static_cast<double>(open_flag));
    cJSON_AddItemToArray(wifi, entry);
  }

  cJSON_AddItemToObject(root, "wifi", wifi);
  cJSON_AddNumberToObject(root, "page", static_cast<double>(page));
  cJSON_AddNumberToObject(root, "tpage", static_cast<double>(total_pages));
  cJSON_AddNumberToObject(root, "found", static_cast<double>(total_found));

  size_t written = render_json(root, buf, buf_size);
  cJSON_Delete(root);
  return written;
}

// Encode an empty scan result: {"found":0}
size_t encode_scan_empty(uint8_t *buf, size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return 0;
  }
  cJSON_AddNumberToObject(root, "found", 0);

  size_t written = render_json(root, buf, buf_size);
  cJSON_Delete(root);
  return written;
}

// Encode a BLE status notification: {"status":<code>}
size_t encode_status(uint8_t status_code, uint8_t *buf, size_t buf_size) {
  if (buf == nullptr || buf_size == 0) {
    return 0;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == nullptr) {
    return 0;
  }
  cJSON_AddNumberToObject(root, "status", static_cast<double>(status_code));

  size_t written = render_json(root, buf, buf_size);
  cJSON_Delete(root);
  return written;
}

} // namespace

BleTransport::BleTransport() {
  _page_timer.set_callback([this]() { send_next_scan_page(); });
}

BleTransport::~BleTransport() { teardown(); }

bool BleTransport::setup(AgBleServer &ble, const ProvisioningBleConfig &config) {
  if (_ble != nullptr) {
    AG_LOGW(TAG, "setup() called while already active");
    return false;
  }

  const char *device_name = config.device_name != nullptr ? config.device_name : "AirGradient";

  AG_LOGI(TAG, "BLE setup: device='%s' model='%s' serial='%s' fw='%s'", device_name,
          config.model_name != nullptr ? config.model_name : "",
          config.serial_number != nullptr ? config.serial_number : "",
          config.firmware_version != nullptr ? config.firmware_version : "");

  if (!ble.init(device_name)) {
    AG_LOGE(TAG, "ble.init() failed");
    return false;
  }
  _ble = &ble;
  _owns_server = true;

  // Security: io_cap + auth_flags are product-controlled. Defaults
  // preserve Just Works + BOND|SC; AGo opts into SC-only (no bond).
  if (!ble.set_security(config.io_capability, config.auth_flags)) {
    AG_LOGE(TAG, "set_security failed");
    teardown();
    return false;
  }
  AG_LOGD(TAG, "BLE security: io_cap=%u auth_flags=0x%02x",
          static_cast<unsigned>(config.io_capability), static_cast<unsigned>(config.auth_flags));

  if (!_register_gatt(config)) {
    teardown();
    return false;
  }

  // --- Connection callbacks ---
  ble.set_connect_callback([this](uint16_t h) { _on_connect(h); });
  ble.set_disconnect_callback([this](uint16_t h, int r) { _on_disconnect(h, r); });

  // --- Advertising configuration ---
  ble.set_advertising_name(device_name);
  ble.add_advertised_service_uuid(PROV_SERVICE_UUID);

  // Manufacturer data: company ID (0xFFFF) + "<model>#<serial>"
  if (config.manufacturer_data != nullptr) {
    const size_t payload_len = std::strlen(config.manufacturer_data);
    std::vector<uint8_t> mfg_data;
    mfg_data.reserve(2 + payload_len);
    mfg_data.push_back(MFG_COMPANY_ID_LO);
    mfg_data.push_back(MFG_COMPANY_ID_HI);
    mfg_data.insert(mfg_data.end(), reinterpret_cast<const uint8_t *>(config.manufacturer_data),
                    reinterpret_cast<const uint8_t *>(config.manufacturer_data) + payload_len);
    ble.set_manufacturer_data(mfg_data.data(), mfg_data.size());
  }

  if (!ble.start_advertising()) {
    AG_LOGE(TAG, "start_advertising failed");
    teardown();
    return false;
  }

  AG_LOGI(TAG, "BLE provisioning active, advertising as '%s'", device_name);
  return true;
}

bool BleTransport::setup_on_server(AgBleServer &ble, const ProvisioningBleConfig &config) {
  if (_ble != nullptr) {
    AG_LOGW(TAG, "setup_on_server() called while already active");
    return false;
  }

  AG_LOGI(TAG, "BLE setup_on_server (attached): model='%s' serial='%s' fw='%s'",
          config.model_name != nullptr ? config.model_name : "",
          config.serial_number != nullptr ? config.serial_number : "",
          config.firmware_version != nullptr ? config.firmware_version : "");

  // Borrowed server — owner already init'd + secured it. No init/security,
  // no connect/disconnect callbacks, no advertising (all owner-driven).
  _ble = &ble;
  _owns_server = false;

  if (!_register_gatt(config)) {
    detach();
    return false;
  }

  AG_LOGI(TAG, "BLE provisioning attached to existing server (no init/advertising)");
  return true;
}

bool BleTransport::_register_gatt(const ProvisioningBleConfig &config) {
  // --- AirGradient Provisioning Service ---
  AgBleGattService *prov_svc = _ble->add_service(PROV_SERVICE_UUID);
  if (prov_svc == nullptr) {
    AG_LOGE(TAG, "add provisioning service failed");
    return false;
  }

  constexpr uint16_t cred_props = AgBleProperty::READ | AgBleProperty::READ_ENC |
                                  AgBleProperty::WRITE | AgBleProperty::WRITE_ENC |
                                  AgBleProperty::NOTIFY;
  _cred_char = prov_svc->add_characteristic(CRED_STATUS_CHAR_UUID, cred_props);
  if (_cred_char == nullptr) {
    AG_LOGE(TAG, "add credentials characteristic failed");
    return false;
  }

  constexpr uint16_t scan_props =
      AgBleProperty::WRITE | AgBleProperty::WRITE_ENC | AgBleProperty::NOTIFY;
  _scan_char = prov_svc->add_characteristic(SCAN_CHAR_UUID, scan_props);
  if (_scan_char == nullptr) {
    AG_LOGE(TAG, "add scan characteristic failed");
    return false;
  }

  _cred_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { _on_credentials_write(data, len); });
  _scan_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { _on_scan_write(data, len); });

  if (!prov_svc->start()) {
    AG_LOGE(TAG, "provisioning service start failed");
    return false;
  }

  // --- Device Information Service ---
  const DeviceInformation info{config.model_name, config.serial_number, config.firmware_version,
                               DEFAULT_MANUFACTURER};
  constexpr uint16_t dis_props = AgBleProperty::READ | AgBleProperty::READ_ENC;
  if (!register_device_information(*_ble, info, dis_props)) {
    AG_LOGE(TAG, "Device Information registration failed");
    return false;
  }
  AG_LOGD(TAG, "GATT services registered (Prov + DIS)");
  return true;
}

void BleTransport::teardown() {
  if (_ble != nullptr) {
    AG_LOGI(TAG, "BLE teardown");
  }
  _page_timer.cancel();
  _scan_cache_size = 0;
  _current_page = 0;
  _total_pages = 0;

  _cred_char = nullptr;
  _scan_char = nullptr;

  if (_ble != nullptr) {
    // Only deinit a server we own; a borrowed one is torn down by its owner.
    if (_owns_server) {
      _ble->deinit();
    }
    _ble = nullptr;
    _owns_server = false;
  }
}

void BleTransport::detach() {
  if (_ble == nullptr) {
    return;
  }
  AG_LOGI(TAG, "BLE detach (attached teardown, no deinit)");
  _page_timer.cancel();
  _scan_cache_size = 0;
  _current_page = 0;
  _total_pages = 0;

  // Clear char write callbacks: the chars outlive us (owned by the borrowed
  // server) until the owner deinits, so a late write must not re-enter us.
  if (_cred_char != nullptr) {
    _cred_char->set_write_callback(nullptr);
  }
  if (_scan_char != nullptr) {
    _scan_char->set_write_callback(nullptr);
  }
  _cred_char = nullptr;
  _scan_char = nullptr;

  // Borrowed server — never deinit.
  _ble = nullptr;
  _owns_server = false;
}

void BleTransport::update_scan_results(const WifiScanEntry *entries, uint16_t count) {
  _page_timer.cancel();

  _scan_cache_size = ScanFilter::apply(entries, count, _scan_cache, MAX_CACHED_SCAN);
  _current_page = 0;

  if (_scan_cache_size == 0) {
    AG_LOGI(TAG, "BLE scan notify: empty");
    uint8_t buf[BLE_NOTIFY_BUF_SIZE];
    size_t len = encode_scan_empty(buf, sizeof(buf));
    if (len > 0 && _scan_char != nullptr) {
      _scan_char->set_value(buf, len);
      _scan_char->notify();
    }
    _total_pages = 0;
    return;
  }

  _total_pages = (_scan_cache_size + NETWORKS_PER_PAGE - 1) / NETWORKS_PER_PAGE;
  AG_LOGI(TAG, "BLE scan notify: %u networks, %u pages", static_cast<unsigned>(_scan_cache_size),
          static_cast<unsigned>(_total_pages));
  _page_timer.arm(0);
}

void BleTransport::send_next_scan_page() {
  if (_current_page >= _total_pages || _scan_char == nullptr) {
    return;
  }

  size_t offset = _current_page * NETWORKS_PER_PAGE;
  size_t entries_in_page = _scan_cache_size - offset;
  if (entries_in_page > NETWORKS_PER_PAGE) {
    entries_in_page = NETWORKS_PER_PAGE;
  }

  uint8_t buf[BLE_NOTIFY_BUF_SIZE];
  size_t len = encode_scan_page(&_scan_cache[offset], entries_in_page, _current_page + 1,
                                _total_pages, _scan_cache_size, buf, sizeof(buf));
  if (len > 0) {
    AG_LOGD(TAG, "BLE scan page %u/%u, %u entries, %u bytes",
            static_cast<unsigned>(_current_page + 1), static_cast<unsigned>(_total_pages),
            static_cast<unsigned>(entries_in_page), static_cast<unsigned>(len));
    _scan_char->set_value(buf, len);
    _scan_char->notify();
  }

  ++_current_page;

  if (_current_page < _total_pages) {
    _page_timer.arm(PAGE_DELAY_MS);
  }
}

bool BleTransport::has_more_scan_pages() const { return _current_page < _total_pages; }

void BleTransport::send_status(uint8_t status_code) {
  if (_cred_char == nullptr) {
    return;
  }
  uint8_t buf[64];
  size_t len = encode_status(status_code, buf, sizeof(buf));
  if (len > 0) {
    AG_LOGD(TAG, "BLE status notify: code=%u", static_cast<unsigned>(status_code));
    _cred_char->set_value(buf, len);
    _cred_char->notify();
  }
}

// ---------------------------------------------------------------------------
// Private write-callback handlers
// ---------------------------------------------------------------------------

void BleTransport::_on_credentials_write(const uint8_t *data, size_t len) {
  AG_LOGI(TAG, "BLE credentials write: %u bytes", static_cast<unsigned>(len));
  if (data == nullptr || len == 0) {
    AG_LOGW(TAG, "empty credential write");
    return;
  }

  cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(data), len);
  if (root == nullptr) {
    AG_LOGW(TAG, "malformed credential JSON (%u bytes)", static_cast<unsigned>(len));
    return;
  }

  ProvisioningData parsed;
  ProvisioningJsonError err = parse_provisioning_json(root, parsed);
  cJSON_Delete(root);

  if (err != ProvisioningJsonError::Ok) {
    AG_LOGW(TAG, "credential parse error %u", static_cast<unsigned>(err));
    return;
  }

  AG_LOGI(TAG, "BLE credentials parsed: ssid='%s'", parsed.ssid);
  if (_on_credentials) {
    _on_credentials(parsed);
  }
}

void BleTransport::_on_scan_write(const uint8_t *data, size_t len) {
  (void)data;
  (void)len;
  AG_LOGI(TAG, "BLE scan request");
  if (_on_scan_request) {
    _on_scan_request();
  }
}

void BleTransport::_on_connect(uint16_t conn_handle) {
  AG_LOGI(TAG, "BLE client connected (handle=%u)", static_cast<unsigned>(conn_handle));

  if (_ble != nullptr) {
    _ble->stop_advertising();
  }

  if (_on_client_connected) {
    _on_client_connected();
  }
}

void BleTransport::_on_disconnect(uint16_t conn_handle, int reason) {
  AG_LOGI(TAG, "BLE client disconnected (handle=%u reason=%d)", static_cast<unsigned>(conn_handle),
          reason);

  if (_ble != nullptr) {
    _ble->start_advertising();
  }

  if (_on_client_disconnected) {
    _on_client_disconnected();
  }
}
