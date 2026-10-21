/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "esp_wifi_hal.h"

#include <cstring>
#include <memory>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi_default.h"
#include "lwip/inet.h"
#include "mdns.h"
#include "nvs.h"
#include "nvs_flash.h"

static constexpr const char *TAG = "EspWifiHal";

namespace {

WifiAuthMode _map_auth_mode(wifi_auth_mode_t raw) {
  switch (raw) {
  case WIFI_AUTH_OPEN:
    return WifiAuthMode::open;
  case WIFI_AUTH_WEP:
    return WifiAuthMode::wep;
  case WIFI_AUTH_WPA_PSK:
    return WifiAuthMode::wpa_psk;
  case WIFI_AUTH_WPA2_PSK:
    return WifiAuthMode::wpa2_psk;
  case WIFI_AUTH_WPA_WPA2_PSK:
    return WifiAuthMode::wpa_wpa2_psk;
  case WIFI_AUTH_WPA3_PSK:
    return WifiAuthMode::wpa3_psk;
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return WifiAuthMode::wpa2_wpa3_psk;
  case WIFI_AUTH_WAPI_PSK:
    return WifiAuthMode::wapi_psk;
  case WIFI_AUTH_OWE:
    return WifiAuthMode::owe;
  default:
    return WifiAuthMode::unknown;
  }
}

wifi_mode_t _to_esp_mode(WifiMode mode) {
  switch (mode) {
  case WifiMode::Off:
    return WIFI_MODE_NULL;
  case WifiMode::Sta:
    return WIFI_MODE_STA;
  case WifiMode::Ap:
    return WIFI_MODE_AP;
  case WifiMode::ApSta:
    return WIFI_MODE_APSTA;
  }
  return WIFI_MODE_NULL;
}

wifi_ps_type_t _to_esp_ps(WifiPowerSave mode) {
  switch (mode) {
  case WifiPowerSave::None:
    return WIFI_PS_NONE;
  case WifiPowerSave::MinModem:
    return WIFI_PS_MIN_MODEM;
  case WifiPowerSave::MaxModem:
    return WIFI_PS_MAX_MODEM;
  }
  return WIFI_PS_NONE;
}

} // namespace

EspWifiHal::EspWifiHal() = default;

EspWifiHal::~EspWifiHal() { deinit(); }

WifiStatus EspWifiHal::init() {
  if (_initialized) {
    return WifiStatus::Ok;
  }

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "nvs_flash_init failed: %d", err);
    return WifiStatus::Failed;
  }

  if (esp_netif_init() != ESP_OK) {
    return WifiStatus::Failed;
  }
  // esp_event_loop_create_default may already exist; treat ALREADY as ok.
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    return WifiStatus::Failed;
  }

  _sta_netif = esp_netif_create_default_wifi_sta();
  _ap_netif = esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (esp_wifi_init(&cfg) != ESP_OK) {
    return WifiStatus::Failed;
  }
  // RAM storage: WifiManager owns persistence (ConfigStore); ESP-IDF must
  // never write STA credentials to its own NVS.
  if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK) {
    return WifiStatus::Failed;
  }

  if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                          &EspWifiHal::_wifi_event_handler, this,
                                          &_wifi_handler_instance) != ESP_OK) {
    return WifiStatus::Failed;
  }
  if (esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                          &EspWifiHal::_ip_event_handler, this,
                                          &_ip_handler_instance) != ESP_OK) {
    return WifiStatus::Failed;
  }

  // Single-shot timers shared across the manager's lifetime.
  esp_timer_create_args_t dhcp_args = {};
  dhcp_args.callback = &EspWifiHal::_dhcp_timer_cb;
  dhcp_args.arg = this;
  dhcp_args.name = "ag_wifi_dhcp";
  esp_timer_create(&dhcp_args, &_dhcp_timer);

  esp_timer_create_args_t retry_args = {};
  retry_args.callback = &EspWifiHal::_retry_timer_cb;
  retry_args.arg = this;
  retry_args.name = "ag_wifi_retry";
  esp_timer_create(&retry_args, &_retry_timer);

  _initialized = true;
  _mode = WifiMode::Off;
  return WifiStatus::Ok;
}

void EspWifiHal::deinit() {
  if (!_initialized) {
    return;
  }
  if (_dhcp_timer != nullptr) {
    esp_timer_stop(_dhcp_timer);
    esp_timer_delete(_dhcp_timer);
    _dhcp_timer = nullptr;
  }
  if (_retry_timer != nullptr) {
    esp_timer_stop(_retry_timer);
    esp_timer_delete(_retry_timer);
    _retry_timer = nullptr;
  }
  if (_mdns_started) {
    mdns_free();
    _mdns_started = false;
  }
  if (_wifi_handler_instance != nullptr) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _wifi_handler_instance);
    _wifi_handler_instance = nullptr;
  }
  if (_ip_handler_instance != nullptr) {
    esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, _ip_handler_instance);
    _ip_handler_instance = nullptr;
  }
  esp_wifi_stop();
  esp_wifi_deinit();
  _initialized = false;
  _mode = WifiMode::Off;
}

WifiStatus EspWifiHal::set_mode(WifiMode mode) {
  const wifi_mode_t target = _to_esp_mode(mode);
  if (mode == WifiMode::Off) {
    esp_wifi_stop();
    _mode = mode;
    _snapshot.mode = mode;
    return WifiStatus::Ok;
  }
  if (esp_wifi_set_mode(target) != ESP_OK) {
    return WifiStatus::Failed;
  }
  if (esp_wifi_start() != ESP_OK) {
    return WifiStatus::Failed;
  }
  _mode = mode;
  _snapshot.mode = mode;
  return WifiStatus::Ok;
}

WifiMode EspWifiHal::get_mode() const { return _mode; }

WifiStatus EspWifiHal::connect_sta(const char *ssid, const char *password) {
  // Manager always passes a concrete SSID; storage is RAM-only (init), so
  // set_config never touches NVS.
  if (ssid == nullptr || ssid[0] == '\0') {
    return WifiStatus::InvalidArgument;
  }

  wifi_config_t cfg = {};
  std::strncpy(reinterpret_cast<char *>(cfg.sta.ssid), ssid, sizeof(cfg.sta.ssid) - 1);
  if (password != nullptr) {
    std::strncpy(reinterpret_cast<char *>(cfg.sta.password), password,
                 sizeof(cfg.sta.password) - 1);
  }
  cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // accept anything; AP decides
  if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) {
    return WifiStatus::Failed;
  }

  if (_has_static_ip) {
    _apply_static_ip_to_netif();
  } else {
    _apply_dhcp_to_netif();
  }
  if (esp_wifi_connect() != ESP_OK) {
    return WifiStatus::Failed;
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::disconnect_sta() {
  esp_wifi_disconnect();
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::set_static_ip(const WifiStaticIpConfig &config) {
  _has_static_ip = true;
  _static_ip = config;
  if (_sta_netif != nullptr) {
    _apply_static_ip_to_netif();
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::clear_static_ip() {
  _has_static_ip = false;
  _static_ip = {};
  if (_sta_netif != nullptr) {
    _apply_dhcp_to_netif();
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::start_scan(const WifiScanConfig &config) {
  wifi_scan_config_t scan_cfg = {};
  scan_cfg.show_hidden = config.show_hidden;
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  // 60 ms/channel keeps a full 42-channel scan inside the ~10 s PMF
  // SA-Query window of any client on a co-resident SoftAP. Silently
  // overridden to BT-coex defaults (~240 ms) when BLE is enabled.
  scan_cfg.scan_time.active.min = 0;
  scan_cfg.scan_time.active.max = 60;
  // Non-blocking: completion arrives as WIFI_EVENT_SCAN_DONE.
  if (esp_wifi_scan_start(&scan_cfg, false) != ESP_OK) {
    return WifiStatus::Failed;
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::start_ap(const WifiApConfig &config) {
  if (config.ssid[0] == '\0') {
    return WifiStatus::InvalidArgument;
  }
  // PMF on the SoftAP is not configurable: ESP-IDF forces PMF on when
  // the peer advertises it (pmf_cfg.capable=false is silently dropped).
  // Any long radio stall will then trigger an SA-Query disassoc, so
  // callers running this SoftAP alongside other radio activity (e.g.
  // BLE) must mitigate at the application level.
  wifi_config_t cfg = {};
  std::strncpy(reinterpret_cast<char *>(cfg.ap.ssid), config.ssid, sizeof(cfg.ap.ssid) - 1);
  cfg.ap.ssid_len = static_cast<uint8_t>(std::strlen(config.ssid));
  cfg.ap.channel = (config.channel == 0) ? 1 : config.channel;
  cfg.ap.max_connection = (config.max_connections == 0) ? 4 : config.max_connections;
  if (config.password[0] == '\0') {
    cfg.ap.authmode = WIFI_AUTH_OPEN;
  } else {
    std::strncpy(reinterpret_cast<char *>(cfg.ap.password), config.password,
                 sizeof(cfg.ap.password) - 1);
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
  }
  if (esp_wifi_set_config(WIFI_IF_AP, &cfg) != ESP_OK) {
    return WifiStatus::Failed;
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::stop_ap() {
  // The AP follows the mode; switching mode away from AP/APSTA stops it.
  // Nothing extra to do here.
  return WifiStatus::Ok;
}

WifiStatusSnapshot EspWifiHal::get_status() const {
  WifiStatusSnapshot snapshot = _snapshot;
  snapshot.mode = _mode;
  if (_mode == WifiMode::Sta || _mode == WifiMode::ApSta) {
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      snapshot.rssi = ap.rssi;
      std::memcpy(snapshot.bssid, ap.bssid, sizeof(snapshot.bssid));
      std::memcpy(snapshot.ssid, ap.ssid, sizeof(snapshot.ssid) - 1);
      snapshot.ssid[sizeof(snapshot.ssid) - 1] = '\0';
      snapshot.channel = ap.primary;
    }
  }
  return snapshot;
}

WifiStatus EspWifiHal::set_power_save(WifiPowerSave mode) {
  if (esp_wifi_set_ps(_to_esp_ps(mode)) != ESP_OK) {
    return WifiStatus::Failed;
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::start_mdns(const WifiMdnsConfig &config) {
  if (config.hostname == nullptr || (config.service_count > 0 && config.services == nullptr)) {
    return WifiStatus::InvalidArgument;
  }
  if (!_mdns_started) {
    if (mdns_init() != ESP_OK) {
      return WifiStatus::Failed;
    }
    _mdns_started = true;
  }
  if (mdns_hostname_set(config.hostname) != ESP_OK) {
    mdns_free();
    _mdns_started = false;
    return WifiStatus::Failed;
  }
  for (uint8_t i = 0; i < config.service_count; ++i) {
    const WifiMdnsServiceRecord &svc = config.services[i];
    if (svc.service_type == nullptr) {
      mdns_free();
      _mdns_started = false;
      return WifiStatus::InvalidArgument;
    }
    // Service-type strings are split into instance / proto by mDNS:
    // "_http._tcp" -> service "_http", proto "_tcp". Cheap split here.
    char service_buf[WIFI_MDNS_MAX_SERVICE_LENGTH + 1] = {};
    char proto_buf[WIFI_MDNS_MAX_PROTOCOL_LENGTH + 1] = {};
    const char *dot = std::strchr(svc.service_type, '.');
    if (dot == nullptr || dot == svc.service_type || dot[1] == '\0' ||
        std::strlen(dot + 1) > WIFI_MDNS_MAX_PROTOCOL_LENGTH) {
      mdns_free();
      _mdns_started = false;
      return WifiStatus::InvalidArgument;
    }
    const size_t svc_len = static_cast<size_t>(dot - svc.service_type);
    if (svc_len > WIFI_MDNS_MAX_SERVICE_LENGTH) {
      mdns_free();
      _mdns_started = false;
      return WifiStatus::InvalidArgument;
    }
    std::memcpy(service_buf, svc.service_type, svc_len);
    service_buf[svc_len] = '\0';
    std::strncpy(proto_buf, dot + 1, sizeof(proto_buf) - 1);

    if (mdns_service_add(nullptr, service_buf, proto_buf, svc.port, nullptr, 0) != ESP_OK) {
      mdns_free();
      _mdns_started = false;
      return WifiStatus::Failed;
    }
    for (uint8_t t = 0; t < svc.txt_count; ++t) {
      if (svc.txt_keys == nullptr || svc.txt_values == nullptr || svc.txt_keys[t] == nullptr ||
          svc.txt_values[t] == nullptr) {
        mdns_free();
        _mdns_started = false;
        return WifiStatus::InvalidArgument;
      }
      if (mdns_service_txt_item_set(service_buf, proto_buf, svc.txt_keys[t], svc.txt_values[t]) !=
          ESP_OK) {
        mdns_free();
        _mdns_started = false;
        return WifiStatus::Failed;
      }
    }
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::stop_mdns() {
  if (_mdns_started) {
    mdns_free();
    _mdns_started = false;
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::arm_dhcp_timeout(uint32_t timeout_ms) {
  if (_dhcp_timer == nullptr) {
    return WifiStatus::Failed;
  }
  esp_timer_stop(_dhcp_timer);
  if (timeout_ms == 0) {
    return WifiStatus::Ok;
  }
  if (esp_timer_start_once(_dhcp_timer, static_cast<uint64_t>(timeout_ms) * 1000ULL) != ESP_OK) {
    return WifiStatus::Failed;
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::cancel_dhcp_timeout() {
  if (_dhcp_timer != nullptr) {
    esp_timer_stop(_dhcp_timer);
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::arm_retry_timer(uint32_t delay_ms) {
  if (_retry_timer == nullptr) {
    return WifiStatus::Failed;
  }
  esp_timer_stop(_retry_timer);
  if (delay_ms == 0) {
    if (_on_retry_due) {
      _on_retry_due();
    }
    return WifiStatus::Ok;
  }
  if (esp_timer_start_once(_retry_timer, static_cast<uint64_t>(delay_ms) * 1000ULL) != ESP_OK) {
    return WifiStatus::Failed;
  }
  return WifiStatus::Ok;
}

WifiStatus EspWifiHal::cancel_retry_timer() {
  if (_retry_timer != nullptr) {
    esp_timer_stop(_retry_timer);
  }
  return WifiStatus::Ok;
}

void EspWifiHal::set_on_sta_connected(WifiConnectedCallback cb) {
  _on_sta_connected = std::move(cb);
}
void EspWifiHal::set_on_sta_disconnected(std::function<void(int)> cb) {
  _on_sta_disconnected = std::move(cb);
}
void EspWifiHal::set_on_got_ip(WifiGotIpCallback cb) { _on_got_ip = std::move(cb); }
void EspWifiHal::set_on_scan_complete(WifiScanCompleteCallback cb) {
  _on_scan_complete = std::move(cb);
}
void EspWifiHal::set_on_ap_client_joined(WifiApClientJoinedCallback cb) {
  _on_ap_client_joined = std::move(cb);
}
void EspWifiHal::set_on_ap_client_left(WifiApClientLeftCallback cb) {
  _on_ap_client_left = std::move(cb);
}
void EspWifiHal::set_on_dhcp_timeout(std::function<void()> cb) { _on_dhcp_timeout = std::move(cb); }
void EspWifiHal::set_on_retry_due(std::function<void()> cb) { _on_retry_due = std::move(cb); }

void EspWifiHal::_wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)base;
  static_cast<EspWifiHal *>(arg)->_handle_wifi_event(id, data);
}

void EspWifiHal::_ip_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)base;
  static_cast<EspWifiHal *>(arg)->_handle_ip_event(id, data);
}

void EspWifiHal::_dhcp_timer_cb(void *arg) {
  EspWifiHal *self = static_cast<EspWifiHal *>(arg);
  if (self->_on_dhcp_timeout) {
    self->_on_dhcp_timeout();
  }
}

void EspWifiHal::_retry_timer_cb(void *arg) {
  EspWifiHal *self = static_cast<EspWifiHal *>(arg);
  if (self->_on_retry_due) {
    self->_on_retry_due();
  }
}

void EspWifiHal::_handle_wifi_event(int32_t id, void *data) {
  switch (id) {
  case WIFI_EVENT_STA_CONNECTED:
    if (_on_sta_connected) {
      _on_sta_connected();
    }
    break;
  case WIFI_EVENT_STA_DISCONNECTED: {
    auto *evt = static_cast<wifi_event_sta_disconnected_t *>(data);
    const int reason = (evt != nullptr) ? evt->reason : 0;
    if (_on_sta_disconnected) {
      _on_sta_disconnected(reason);
    }
    break;
  }
  case WIFI_EVENT_SCAN_DONE: {
    // Heap-allocate the scan result buffers. Putting them on the stack
    // here pushes the sys_evt task stack past its limit (the compiler
    // reserves space for every switch-case local at function entry).
    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > WIFI_SCAN_MAX_RESULTS) {
      ap_count = WIFI_SCAN_MAX_RESULTS;
    }
    std::unique_ptr<wifi_ap_record_t[]> records(new (std::nothrow)
                                                    wifi_ap_record_t[WIFI_SCAN_MAX_RESULTS]);
    std::unique_ptr<WifiScanEntry[]> entries(new (std::nothrow)
                                                 WifiScanEntry[WIFI_SCAN_MAX_RESULTS]);
    if (records == nullptr || entries == nullptr) {
      ESP_LOGE(TAG, "scan-done: allocation failed");
      break;
    }
    if (ap_count > 0) {
      esp_wifi_scan_get_ap_records(&ap_count, records.get());
    }
    for (uint16_t i = 0; i < ap_count; ++i) {
      std::memcpy(entries[i].ssid, records[i].ssid, sizeof(entries[i].ssid) - 1);
      entries[i].ssid[sizeof(entries[i].ssid) - 1] = '\0';
      std::memcpy(entries[i].bssid, records[i].bssid, sizeof(entries[i].bssid));
      entries[i].rssi = records[i].rssi;
      entries[i].channel = records[i].primary;
      entries[i].auth_mode = _map_auth_mode(records[i].authmode);
    }
    if (_on_scan_complete) {
      _on_scan_complete(entries.get(), ap_count);
    }
    break;
  }
  case WIFI_EVENT_AP_STACONNECTED: {
    auto *evt = static_cast<wifi_event_ap_staconnected_t *>(data);
    _snapshot.ap_client_count += 1;
    if (evt != nullptr && _on_ap_client_joined) {
      _on_ap_client_joined(evt->mac);
    }
    break;
  }
  case WIFI_EVENT_AP_STADISCONNECTED: {
    auto *evt = static_cast<wifi_event_ap_stadisconnected_t *>(data);
    if (_snapshot.ap_client_count > 0) {
      _snapshot.ap_client_count -= 1;
    }
    if (evt != nullptr && _on_ap_client_left) {
      _on_ap_client_left(evt->mac);
    }
    break;
  }
  default:
    break;
  }
}

void EspWifiHal::_handle_ip_event(int32_t id, void *data) {
  switch (id) {
  case IP_EVENT_STA_GOT_IP: {
    auto *evt = static_cast<ip_event_got_ip_t *>(data);
    const uint32_t ip = (evt != nullptr) ? evt->ip_info.ip.addr : 0;
    _snapshot.ip = ip;
    wifi_ps_type_t power_save = WIFI_PS_NONE;
    const esp_err_t err = esp_wifi_get_ps(&power_save);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "Wi-Fi power save after got IP: %d", static_cast<int>(power_save));
    } else {
      ESP_LOGW(TAG, "failed to read Wi-Fi power save: %d", err);
    }
    if (_on_got_ip) {
      _on_got_ip(ip);
    }
    break;
  }
  case IP_EVENT_STA_LOST_IP:
    _snapshot.ip = WIFI_IP_INVALID;
    break;
  default:
    break;
  }
}

void EspWifiHal::_apply_static_ip_to_netif() {
  if (_sta_netif == nullptr) {
    return;
  }
  esp_netif_dhcpc_stop(_sta_netif);
  esp_netif_ip_info_t info = {};
  info.ip.addr = _static_ip.ip;
  info.netmask.addr = _static_ip.netmask;
  info.gw.addr = _static_ip.gateway;
  esp_netif_set_ip_info(_sta_netif, &info);
  if (_static_ip.dns_primary != 0) {
    esp_netif_dns_info_t dns = {};
    dns.ip.u_addr.ip4.addr = _static_ip.dns_primary;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
  }
  if (_static_ip.dns_secondary != 0) {
    esp_netif_dns_info_t dns = {};
    dns.ip.u_addr.ip4.addr = _static_ip.dns_secondary;
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_set_dns_info(_sta_netif, ESP_NETIF_DNS_BACKUP, &dns);
  }
}

void EspWifiHal::_apply_dhcp_to_netif() {
  if (_sta_netif == nullptr) {
    return;
  }
  // Idempotent: returns ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED if running.
  esp_netif_dhcpc_start(_sta_netif);
}
