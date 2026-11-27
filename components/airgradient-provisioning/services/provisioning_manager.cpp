/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "provisioning_manager.h"

#include <cstdio>
#include <cstring>

#include "../internal/ble_transport.h"
#include "../internal/captive_dns_responder.h"
#include "../internal/provisioning_timer.h"
#include "../internal/wifi_portal_transport.h"
#include "ag_log.h"
#include "common.h"
#include "hal/ble_server.h"
#include "hal/http_server.h"
#include "services/wifi_manager.h"

#ifndef TEST_HOST
#include "esp_heap_caps.h"
#include "esp_system.h"
#endif

namespace {

constexpr const char *TAG = "Provisioning";

// Default AP IP on the soft-AP gateway (192.168.4.1) in network byte
// order (octet 0 in the low byte). lwIP's softAP defaults to this.
constexpr uint32_t DEFAULT_AP_IP_BE = 0x0104a8c0; // 192.168.4.1

// Hold before tearing down when stop() is called from Connected, so the
// captive-portal browser can poll /api/status once and see success.
constexpr uint32_t POST_CONNECT_HOLD_MS = 1500;

void format_mac(const uint8_t mac[6], char out[18]) {
  std::snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                mac[5]);
}

// format_ipv4_be is provided by airgradient-common (see common.h).

// Stable token appended to lifecycle log lines.
const char *transport_name(ProvisioningTransport t) {
  switch (t) {
  case ProvisioningTransport::BleOnly:
    return "ble-only";
  case ProvisioningTransport::WifiOnly:
    return "wifi-only";
  case ProvisioningTransport::Both:
    return "both";
  case ProvisioningTransport::BleAttached:
    return "ble-attached";
  }
  return "unknown";
}

// Heap probe around Both-mode teardowns. No-op under TEST_HOST.
void log_teardown_heap(const char *side, const char *phase) {
#ifndef TEST_HOST
  AG_LOGD(TAG, "%s teardown %s free=%u largest=%u dma=%u", side, phase,
          static_cast<unsigned>(esp_get_free_heap_size()),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)),
          static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA)));
#else
  (void)side;
  (void)phase;
#endif
}

} // namespace

// Portal HTML linker symbols (provided by EMBED_FILES in the firmware
// build). Under TEST_HOST these aren't available; the portal transport
// registers only API routes.
#ifndef TEST_HOST
extern "C" const uint8_t _binary_portal_html_start[] asm("_binary_portal_html_start");
extern "C" const uint8_t _binary_portal_html_end[] asm("_binary_portal_html_end");
#endif

ProvisioningManager::ProvisioningManager()
    : _portal(std::make_unique<WifiPortalTransport>()),
      _ble_transport(std::make_unique<BleTransport>()),
      _dns(std::make_unique<CaptiveDnsResponder>()), _timer(std::make_unique<ProvisioningTimer>()) {
  _timer->set_callback([this]() { _on_timeout(); });
}

ProvisioningManager::~ProvisioningManager() {
  // Clear the callback before the destructor's stop() to prevent firing
  // Stopped into potentially-destroyed captures. If the product wanted
  // the Stopped event, they should have called stop() explicitly before
  // the manager goes out of scope.
  _on_event = nullptr;
  stop();
}

void ProvisioningManager::set_on_event(ProvisioningEventCallback cb) {
  _mutex.lock();
  _on_event = std::move(cb);
  _mutex.unlock();
}

bool ProvisioningManager::start(WifiManager &wifi, AgBleServer &ble, HttpServer &http,
                                const ProvisioningConfig &config) {
  _mutex.lock();
  if (_state != ProvisioningState::Idle) {
    AG_LOGW(TAG, "start() called while not Idle (state=%u)", static_cast<unsigned>(_state));
    _mutex.unlock();
    return false;
  }

  // Only enforce fields the selected transport will use.
  const bool want_wifi = (config.transport != ProvisioningTransport::BleOnly);
  const bool want_ble = (config.transport != ProvisioningTransport::WifiOnly);
  if (want_wifi && config.ap.ssid[0] == '\0') {
    AG_LOGE(TAG, "AP SSID is required");
    _mutex.unlock();
    return false;
  }
  if (want_ble && config.ble.device_name == nullptr) {
    AG_LOGE(TAG, "BLE device_name is required");
    _mutex.unlock();
    return false;
  }

  AG_LOGI(TAG,
          "start(): transport=%s AP ssid='%s' ch=%u port=%u overall_timeout=%u ms "
          "connect_timeout=%u ms ble='%s' model='%s'",
          transport_name(config.transport), config.ap.ssid,
          static_cast<unsigned>(config.ap.channel), static_cast<unsigned>(config.http_port),
          static_cast<unsigned>(config.overall_timeout_ms),
          static_cast<unsigned>(config.connect_timeout_ms),
          config.ble.device_name != nullptr ? config.ble.device_name : "",
          config.ble.model_name != nullptr ? config.ble.model_name : "");

  _owns_wifi_callbacks = true;
  _wifi = &wifi;
  _http = want_wifi ? &http : nullptr;
  _config = config;
  _ap_client_count = 0;
  _ble_client_count = 0;
  _timeout_armed = false;
  _ble_active = false;
  _wifi_active = false;
  _mdns_profile_installed = false;

  // Portal transport (Wi-Fi side).
  if (want_wifi) {
    _portal->set_on_credentials(
        [this](const ProvisioningData &d) { return _accept_credentials(d); });
    _portal->set_on_scan_request([this]() { return _trigger_scan(); });
    _portal->set_state(WifiPortalTransport::PortalState::Waiting);

    const uint8_t *html_start = nullptr;
    const uint8_t *html_end = nullptr;
#ifndef TEST_HOST
    html_start = _binary_portal_html_start;
    html_end = _binary_portal_html_end;
#endif
    if (!_portal->register_routes(http, html_start, html_end)) {
      AG_LOGE(TAG, "portal route registration failed");
      http.unregister_all();
      _wifi = nullptr;
      _http = nullptr;
      _mutex.unlock();
      return false;
    }
  }

  // BLE transport.
  if (want_ble) {
    _ble_transport->set_on_credentials(
        [this](const ProvisioningData &d) { return _accept_credentials(d); });
    _ble_transport->set_on_scan_request([this]() { return _trigger_scan(); });
    _ble_transport->set_on_client_connected([this]() { _on_ble_client_connected(); });
    _ble_transport->set_on_client_disconnected([this]() { _on_ble_client_disconnected(); });

    if (!_ble_transport->setup(ble, config.ble)) {
      AG_LOGE(TAG, "BLE transport setup failed");
      if (want_wifi) {
        http.unregister_all();
      }
      _wifi = nullptr;
      _http = nullptr;
      _mutex.unlock();
      return false;
    }
  }

  // Shared between both transport
  wifi.set_on_got_ip([this](uint32_t ip) { _on_sta_connected(ip); });
  wifi.set_on_disconnected([this](WifiDisconnectReason reason) {
    (void)reason;
    _on_sta_disconnected();
  });
  wifi.set_on_scan_complete([this](const WifiScanEntry *r, uint16_t c) { _on_scan_results(r, c); });

  // Only wifi transport need this
  if (want_wifi) {
    wifi.set_on_ap_client_joined([this](const uint8_t *mac) { _on_ap_client_joined(mac); });
    wifi.set_on_ap_client_left([this](const uint8_t *mac) { _on_ap_client_left(mac); });
  }

  // Bound how long WifiManager will wait for DHCP after L2 association.
  if (_config.connect_timeout_ms > 0) {
    wifi.set_dhcp_timeout_ms(_config.connect_timeout_ms);
  }

  // BleOnly stays Sta; the other two need ApSta + soft-AP.
  const WifiMode target_mode = want_wifi ? WifiMode::ApSta : WifiMode::Sta;
  if (wifi.set_mode(target_mode) != WifiStatus::Ok) {
    AG_LOGE(TAG, "set_mode(%u) failed", static_cast<unsigned>(target_mode));
    _rollback_start_locked(wifi, http);
    _mutex.unlock();
    return false;
  }

  if (want_wifi) {
    WifiApConfig ap_cfg = {};
    std::strncpy(ap_cfg.ssid, _config.ap.ssid, sizeof(ap_cfg.ssid) - 1);
    std::strncpy(ap_cfg.password, _config.ap.password, sizeof(ap_cfg.password) - 1);
    ap_cfg.channel = _config.ap.channel;
    ap_cfg.max_connections = _config.ap.max_clients;
    if (wifi.start_ap(ap_cfg) != WifiStatus::Ok) {
      AG_LOGE(TAG, "start_ap failed");
      _rollback_start_locked(wifi, http);
      _mutex.unlock();
      return false;
    }

    // DNS responder powers captive-portal auto-popup; if it fails
    // the portal is still reachable manually at the AP IP, so this
    // is a degraded-but-usable case — log and continue. _dns->stop()
    // is idempotent so later teardown paths don't need to know
    // whether start succeeded.
    if (!_dns->start(DEFAULT_AP_IP_BE)) {
      AG_LOGW(TAG, "captive DNS responder failed to start; portal still reachable manually");
    }

    // Start the HTTP server now that all portal routes are registered.
    if (!http.start(_config.http_port)) {
      AG_LOGE(TAG, "http.start(:%u) failed", static_cast<unsigned>(_config.http_port));
      _rollback_start_locked(wifi, http);
      _mutex.unlock();
      return false;
    }

    WifiMdnsServiceRecord mdns_service = {};
    mdns_service.service_type = "_http._tcp";
    mdns_service.port = _config.http_port;

    WifiMdnsProfile mdns_profile = {};
    mdns_profile.config.hostname = _config.hostname;
    mdns_profile.config.services = &mdns_service;
    mdns_profile.config.service_count = 1;
    mdns_profile.lifecycle = WifiMdnsLifecycle::Manual;

    const WifiStatus mdns_profile_status = wifi.set_mdns_profile(mdns_profile);
    if (mdns_profile_status != WifiStatus::Ok) {
      AG_LOGW(TAG, "provisioning mDNS profile setup failed (%u); portal remains reachable",
              static_cast<unsigned>(mdns_profile_status));
    } else {
      _mdns_profile_installed = true;
      const WifiStatus mdns_start_status = wifi.start_mdns();
      if (mdns_start_status != WifiStatus::Ok) {
        AG_LOGW(TAG, "provisioning mDNS start failed (%u); portal remains reachable",
                static_cast<unsigned>(mdns_start_status));
      }
    }
  }

  _ble_active = want_ble;
  _wifi_active = want_wifi;

  _set_state_locked(ProvisioningState::WaitingForCredentials);
  _maybe_arm_timeout_locked();

  AG_LOGI(TAG, "provisioning started transport=%s AP='%s' ch=%u port=%u",
          transport_name(_config.transport), _config.ap.ssid,
          static_cast<unsigned>(_config.ap.channel), static_cast<unsigned>(_config.http_port));

  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Started;
  _mutex.unlock();
  _emit(info);
  return true;
}

bool ProvisioningManager::start_attached(WifiManager &wifi, AgBleServer &ble,
                                         const ProvisioningConfig &config,
                                         bool own_wifi_callbacks) {
  _mutex.lock();
  if (_state != ProvisioningState::Idle) {
    AG_LOGW(TAG, "start_attached() called while not Idle (state=%u)",
            static_cast<unsigned>(_state));
    _mutex.unlock();
    return false;
  }
  if (config.transport != ProvisioningTransport::BleAttached) {
    AG_LOGE(TAG, "start_attached() requires transport=ble-attached");
    _mutex.unlock();
    return false;
  }
  if (config.ble.device_name == nullptr) {
    AG_LOGE(TAG, "BLE device_name is required");
    _mutex.unlock();
    return false;
  }

  AG_LOGI(TAG, "start_attached(): transport=%s ble='%s' model='%s'",
          transport_name(config.transport), config.ble.device_name,
          config.ble.model_name != nullptr ? config.ble.model_name : "");

  _owns_wifi_callbacks = own_wifi_callbacks;
  _wifi = &wifi;
  _http = nullptr; // no portal/HTTP in attached mode
  _config = config;
  _ap_client_count = 0;
  _ble_client_count = 0;
  _timeout_armed = false;
  _ble_active = true;
  _wifi_active = false;
  _mdns_profile_installed = false;

  // Forward writes to the attached hook instead of touching Wi-Fi here.
  _ble_transport->set_on_credentials([this](const ProvisioningData &d) {
    AttachedRequest req;
    req.kind = AttachedRequestKind::Credentials;
    req.data = d;
    _dispatch_attached_request(req);
    return true;
  });
  _ble_transport->set_on_scan_request([this]() {
    AttachedRequest req;
    req.kind = AttachedRequestKind::Scan;
    _dispatch_attached_request(req);
    return true;
  });
  // No connect/disconnect callbacks — those belong to the owning service.

  if (!_ble_transport->setup_on_server(ble, config.ble)) {
    AG_LOGE(TAG, "BLE attached transport setup failed");
    _ble_transport->set_on_credentials(nullptr);
    _ble_transport->set_on_scan_request(nullptr);
    _wifi = nullptr;
    _ble_active = false;
    _mutex.unlock();
    return false;
  }

  // The product may retain callbacks for an existing station connection.
  if (_owns_wifi_callbacks) {
    wifi.set_on_got_ip([this](uint32_t ip) { _on_sta_connected(ip); });
    wifi.set_on_disconnected([this](WifiDisconnectReason reason) {
      (void)reason;
      _on_sta_disconnected();
    });
    wifi.set_on_scan_complete(
        [this](const WifiScanEntry *r, uint16_t c) { _on_scan_results(r, c); });
  }

  if (_config.connect_timeout_ms > 0) {
    wifi.set_dhcp_timeout_ms(_config.connect_timeout_ms);
  }

  // Wait for a product-admitted request; leave the station untouched.
  _set_state_locked(ProvisioningState::WaitingForCredentials);

  AG_LOGI(TAG, "attached provisioning registered transport=%s", transport_name(_config.transport));

  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Started;
  _mutex.unlock();
  _emit(info);
  return true;
}

void ProvisioningManager::handle_wifi_connected(uint32_t ip) { _on_sta_connected(ip); }

void ProvisioningManager::handle_wifi_disconnected() { _on_sta_disconnected(); }

void ProvisioningManager::handle_scan_results(const WifiScanEntry *entries, uint16_t count) {
  _on_scan_results(entries, count);
}

void ProvisioningManager::set_attached_request_hook(AttachedRequestCallback hook) {
  _mutex.lock();
  _attached_hook = std::move(hook);
  _mutex.unlock();
}

bool ProvisioningManager::request_scan() { return _trigger_scan(); }

bool ProvisioningManager::submit_credentials(const ProvisioningData &data) {
  return _accept_credentials(data);
}

void ProvisioningManager::reset_to_listening() {
  _mutex.lock();
  if (_state == ProvisioningState::Connected) {
    AG_LOGI(TAG, "reset_to_listening: Connected -> WaitingForCredentials");
    _set_state_locked(ProvisioningState::WaitingForCredentials);
  } else {
    AG_LOGD(TAG, "reset_to_listening ignored in state=%u", static_cast<unsigned>(_state));
  }
  _mutex.unlock();
}

void ProvisioningManager::stop(bool stop_http_server) {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Stopped;
  info.stop_reason = ProvisioningStopReason::ProductRequested;

  _mutex.lock();
  const ProvisioningState entry_state = _state;
  if (_state == ProvisioningState::Idle) {
    _mutex.unlock();
    return;
  }
  const bool attached = (_config.transport == ProvisioningTransport::BleAttached);
  AG_LOGI(TAG, "stop() requested (state=%u stop_http_server=%d transport=%s)",
          static_cast<unsigned>(_state), stop_http_server ? 1 : 0,
          transport_name(_config.transport));
  _timer->cancel();
  _timeout_armed = false;

  // Attached mode has no portal — skip the post-connect hold.
  if (entry_state == ProvisioningState::Connected && !attached) {
    AG_LOGI(TAG, "stop(): holding portal for %u ms before teardown",
            static_cast<unsigned>(POST_CONNECT_HOLD_MS));
    RTOS::delay_ms(POST_CONNECT_HOLD_MS);
  }

  _teardown_mdns_locked();
  _dns->stop();

  // Clear BLE transport callbacks before teardown so the disconnect
  // triggered by deinit() doesn't call back into the manager while
  // the mutex is held (which would deadlock).
  _ble_transport->set_on_credentials(nullptr);
  _ble_transport->set_on_scan_request(nullptr);
  _ble_transport->set_on_client_connected(nullptr);
  _ble_transport->set_on_client_disconnected(nullptr);
  // Attached: detach (server belongs to the owner); otherwise full teardown.
  if (attached) {
    _ble_transport->detach();
  } else {
    _ble_transport->teardown();
  }

  if (_http != nullptr) {
    _http->unregister_all();
    if (stop_http_server) {
      _http->stop();
    }
  }

  // Detach WifiManager callbacks. Attached mode leaves the radio to the
  // product, so the manager does not change Wi-Fi mode here.
  if (_wifi != nullptr && _owns_wifi_callbacks) {
    _wifi->set_on_scan_complete(nullptr);
    _wifi->set_on_ap_client_joined(nullptr);
    _wifi->set_on_ap_client_left(nullptr);
    _wifi->set_on_got_ip(nullptr);
    _wifi->set_on_disconnected(nullptr);
    if (!attached) {
      _wifi->set_mode(WifiMode::Sta);
    }
  }
  const ProvisioningTransport stopped_transport = _config.transport;
  _wifi = nullptr;
  _http = nullptr;
  _ble_active = false;
  _wifi_active = false;
  _set_state_locked(ProvisioningState::Idle);
  _mutex.unlock();

  AG_LOGI(TAG, "provisioning stopped (reason=%u transport=%s)",
          static_cast<unsigned>(info.stop_reason), transport_name(stopped_transport));
  _emit(info);
}

ProvisioningState ProvisioningManager::state() const {
  _mutex.lock();
  ProvisioningState s = _state;
  _mutex.unlock();
  return s;
}

void ProvisioningManager::send_ble_status(uint8_t status_code) {
  _mutex.lock();
  if (_state == ProvisioningState::Idle) {
    _mutex.unlock();
    return;
  }
  AG_LOGD(TAG, "send_ble_status code=%u", static_cast<unsigned>(status_code));
  _ble_transport->send_status(status_code);
  _mutex.unlock();
}

// ---------------------------------------------------------------------------
// Private event handlers
// ---------------------------------------------------------------------------

void ProvisioningManager::_on_scan_results(const WifiScanEntry *entries, uint16_t count) {
  AG_LOGI(TAG, "scan complete: %u raw entries", static_cast<unsigned>(count));
  _mutex.lock();
  _portal->update_scan_results(entries, count);
  _ble_transport->update_scan_results(entries, count);
  _mutex.unlock();
}

void ProvisioningManager::_on_sta_connected(uint32_t ip) {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Connected;

  _mutex.lock();
  if (_state != ProvisioningState::Connecting) {
    AG_LOGD(TAG, "ignored stale STA connected in state=%u", static_cast<unsigned>(_state));
    _mutex.unlock();
    return;
  }
  info.data = _pending_data;
  info.ip = ip;

  // Persist the verified credential before emitting Connected. On failure
  // still report Connected, but warn over BLE that it won't survive reboot.
  if (_wifi != nullptr) {
    const WifiStatus persist = _wifi->add_network(_pending_data.ssid, _pending_data.password);
    if (persist != WifiStatus::Ok) {
      AG_LOGE(TAG, "add_network('%s') failed (%d) — credential will not survive reboot",
              _pending_data.ssid, static_cast<int>(persist));
      _ble_transport->send_status(ProvisioningBleStatus::CREDENTIALS_NOT_SAVED);
    }
  }

  _portal->set_state(WifiPortalTransport::PortalState::Connected);
  _ble_transport->send_status(ProvisioningBleStatus::WIFI_CONNECTED);
  _set_state_locked(ProvisioningState::Connected);
  _pause_timeout_locked(); // no further timeout in Connected
  _mutex.unlock();
  _emit(info);
}

void ProvisioningManager::_on_sta_disconnected() {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::ConnectFailed;

  _mutex.lock();
  if (_state != ProvisioningState::Connecting) {
    AG_LOGD(TAG, "ignored stale STA disconnect in state=%u", static_cast<unsigned>(_state));
    _mutex.unlock();
    return;
  }
  AG_LOGI(TAG, "STA connect failed for ssid='%s'", _pending_data.ssid);
  _portal->set_state(WifiPortalTransport::PortalState::Failed);
  _ble_transport->send_status(ProvisioningBleStatus::WIFI_CONNECT_FAILED);
  _set_state_locked(ProvisioningState::WaitingForCredentials);
  _maybe_arm_timeout_locked();
  _mutex.unlock();
  _emit(info);
}

void ProvisioningManager::_on_ap_client_joined(const uint8_t *mac) {
  _mutex.lock();
  const bool first_join = (_ap_client_count == 0);
  ++_ap_client_count;
  if (mac != nullptr) {
    char mac_str[18];
    format_mac(mac, mac_str);
    AG_LOGI(TAG, "AP client joined: %s (ap=%u ble=%u)", mac_str,
            static_cast<unsigned>(_ap_client_count), static_cast<unsigned>(_ble_client_count));
  } else {
    AG_LOGI(TAG, "AP client joined (ap=%u ble=%u)", static_cast<unsigned>(_ap_client_count),
            static_cast<unsigned>(_ble_client_count));
  }
  // Both mode: first AP client wins, drop BLE.
  if (first_join && _config.transport == ProvisioningTransport::Both) {
    _teardown_ble_transport_locked();
  }
  if (_total_client_count_locked() == 1) {
    _pause_timeout_locked();
  }
  _mutex.unlock();
}

void ProvisioningManager::_on_ap_client_left(const uint8_t *mac) {
  _mutex.lock();
  if (_ap_client_count > 0) {
    --_ap_client_count;
  }
  if (mac != nullptr) {
    char mac_str[18];
    format_mac(mac, mac_str);
    AG_LOGI(TAG, "AP client left: %s (ap=%u ble=%u)", mac_str,
            static_cast<unsigned>(_ap_client_count), static_cast<unsigned>(_ble_client_count));
  } else {
    AG_LOGI(TAG, "AP client left (ap=%u ble=%u)", static_cast<unsigned>(_ap_client_count),
            static_cast<unsigned>(_ble_client_count));
  }
  if (_total_client_count_locked() == 0) {
    _resume_timeout_locked();
  }
  _mutex.unlock();
}

void ProvisioningManager::_on_ble_client_connected() {
  _mutex.lock();
  const bool first_connect = (_ble_client_count == 0);
  ++_ble_client_count;
  AG_LOGI(TAG, "BLE client connected (ap=%u ble=%u)", static_cast<unsigned>(_ap_client_count),
          static_cast<unsigned>(_ble_client_count));
  // Both mode: first BLE central wins, drop Wi-Fi.
  if (first_connect && _config.transport == ProvisioningTransport::Both) {
    _teardown_wifi_transport_locked();
  }
  if (_total_client_count_locked() == 1) {
    _pause_timeout_locked();
  }
  _mutex.unlock();
}

void ProvisioningManager::_on_ble_client_disconnected() {
  _mutex.lock();
  if (_ble_client_count > 0) {
    --_ble_client_count;
  }
  AG_LOGI(TAG, "BLE client disconnected (ap=%u ble=%u)", static_cast<unsigned>(_ap_client_count),
          static_cast<unsigned>(_ble_client_count));
  if (_total_client_count_locked() == 0) {
    _resume_timeout_locked();
  }
  _mutex.unlock();
}

// ---------------------------------------------------------------------------
// Internals
// ---------------------------------------------------------------------------

void ProvisioningManager::_set_state_locked(ProvisioningState s) {
  if (s != _state) {
    AG_LOGI(TAG, "state: %u -> %u", static_cast<unsigned>(_state), static_cast<unsigned>(s));
  }
  _state = s;
}

void ProvisioningManager::_emit(const ProvisioningEventInfo &info) {
  ProvisioningEventCallback cb;
  _mutex.lock();
  cb = _on_event;
  _mutex.unlock();
  if (cb) {
    cb(info);
  }
}

void ProvisioningManager::_dispatch_attached_request(const AttachedRequest &req) {
  // Copy under the lock, invoke without it: the hook may block (queue_send)
  // and re-enters the manager via request_scan() / submit_credentials().
  AttachedRequestCallback hook;
  _mutex.lock();
  hook = _attached_hook;
  _mutex.unlock();
  if (hook) {
    hook(req);
  } else {
    AG_LOGW(TAG, "attached request dropped — no hook installed");
  }
}

bool ProvisioningManager::_accept_credentials(const ProvisioningData &data) {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Connecting;

  _mutex.lock();
  if (_state != ProvisioningState::WaitingForCredentials) {
    AG_LOGW(TAG, "credentials rejected — state=%u (expected WaitingForCredentials)",
            static_cast<unsigned>(_state));
    _mutex.unlock();
    return false;
  }
  AG_LOGI(TAG, "credentials accepted: ssid='%s' pw_len=%u disable_cloud=%d static_ip=%d", data.ssid,
          static_cast<unsigned>(std::strlen(data.password)), data.disable_cloud ? 1 : 0,
          data.has_static_ip() ? 1 : 0);
  _pending_data = data;
  info.data = data;
  _portal->set_state(WifiPortalTransport::PortalState::Connecting);
  _set_state_locked(ProvisioningState::Connecting);
  _pause_timeout_locked();

  if (_wifi != nullptr) {
    if (data.has_static_ip()) {
      char ip_str[16];
      char nm_str[16];
      char gw_str[16];
      char dns_str[16];
      format_ipv4_be(data.static_ip.ip, ip_str);
      format_ipv4_be(data.static_ip.netmask, nm_str);
      format_ipv4_be(data.static_ip.gateway, gw_str);
      format_ipv4_be(data.static_ip.dns_primary, dns_str);
      AG_LOGI(TAG, "applying static IP %s/%s gw=%s dns=%s", ip_str, nm_str, gw_str, dns_str);
      _wifi->set_static_ip(data.static_ip);
    } else {
      AG_LOGI(TAG, "no static IP — using DHCP");
      _wifi->clear_static_ip();
    }
    WifiStaConfig sta = {};
    std::strncpy(sta.ssid, data.ssid, sizeof(sta.ssid) - 1);
    std::strncpy(sta.password, data.password, sizeof(sta.password) - 1);
    sta.max_retry_count = 0;
    _wifi->connect(sta);
  }
  _mutex.unlock();
  _emit(info);
  return true;
}

bool ProvisioningManager::_trigger_scan() {
  _mutex.lock();
  if (_state != ProvisioningState::WaitingForCredentials) {
    AG_LOGD(TAG, "scan ignored in state=%u", static_cast<unsigned>(_state));
    _mutex.unlock();
    return false;
  }
  AG_LOGI(TAG, "wifi scan triggered");
  const bool started = _wifi != nullptr && _wifi->start_scan({}) == WifiStatus::Ok;
  _mutex.unlock();
  return started;
}

void ProvisioningManager::_on_timeout() {
  ProvisioningEventInfo info;
  info.event = ProvisioningEvent::Stopped;
  info.stop_reason = ProvisioningStopReason::TimedOut;

  _mutex.lock();
  if (_state == ProvisioningState::Idle) {
    _mutex.unlock();
    return;
  }
  if (_total_client_count_locked() > 0) {
    // Spurious — clients reattached after the timer fired. Re-arm.
    AG_LOGW(TAG, "timeout fired but clients present (ap=%u ble=%u) — re-arming",
            static_cast<unsigned>(_ap_client_count), static_cast<unsigned>(_ble_client_count));
    _maybe_arm_timeout_locked();
    _mutex.unlock();
    return;
  }
  AG_LOGI(TAG, "inactivity timeout expired — tearing down");
  _timeout_armed = false;
  _teardown_mdns_locked();
  _dns->stop();

  _ble_transport->set_on_credentials(nullptr);
  _ble_transport->set_on_scan_request(nullptr);
  _ble_transport->set_on_client_connected(nullptr);
  _ble_transport->set_on_client_disconnected(nullptr);
  _ble_transport->teardown();

  if (_http != nullptr) {
    _http->unregister_all();
    _http->stop();
  }
  if (_wifi != nullptr) {
    _wifi->set_on_scan_complete(nullptr);
    _wifi->set_on_ap_client_joined(nullptr);
    _wifi->set_on_ap_client_left(nullptr);
    _wifi->set_on_got_ip(nullptr);
    _wifi->set_on_disconnected(nullptr);
    _wifi->set_mode(WifiMode::Sta);
  }
  const ProvisioningTransport stopped_transport = _config.transport;
  _wifi = nullptr;
  _http = nullptr;
  _ble_active = false;
  _wifi_active = false;
  _set_state_locked(ProvisioningState::Idle);
  _mutex.unlock();
  AG_LOGI(TAG, "provisioning stopped (reason=%u transport=%s)",
          static_cast<unsigned>(info.stop_reason), transport_name(stopped_transport));
  _emit(info);
}

void ProvisioningManager::_maybe_arm_timeout_locked() {
  if (_config.overall_timeout_ms == 0) {
    return;
  }
  if (_state != ProvisioningState::WaitingForCredentials) {
    return;
  }
  if (_total_client_count_locked() > 0) {
    return;
  }
  _timer->arm(_config.overall_timeout_ms);
  _timeout_armed = true;
  AG_LOGI(TAG, "inactivity timeout armed (%u ms)",
          static_cast<unsigned>(_config.overall_timeout_ms));
}

void ProvisioningManager::_pause_timeout_locked() {
  if (_timeout_armed) {
    _timer->cancel();
    _timeout_armed = false;
    AG_LOGD(TAG, "inactivity timeout paused");
  }
}

void ProvisioningManager::_resume_timeout_locked() {
  if (_config.overall_timeout_ms == 0) {
    return;
  }
  if (_state != ProvisioningState::WaitingForCredentials) {
    return;
  }
  if (!_timeout_armed) {
    _timer->arm(_config.overall_timeout_ms);
    _timeout_armed = true;
    AG_LOGD(TAG, "inactivity timeout resumed (%u ms)",
            static_cast<unsigned>(_config.overall_timeout_ms));
  }
}

uint32_t ProvisioningManager::_total_client_count_locked() const {
  return _ap_client_count + _ble_client_count;
}

void ProvisioningManager::_rollback_start_locked(WifiManager &wifi, HttpServer &http) {
  // Clear BLE callbacks before teardown so a disconnect triggered by
  // deinit() doesn't re-enter the manager while the mutex is held.
  _ble_transport->set_on_credentials(nullptr);
  _ble_transport->set_on_scan_request(nullptr);
  _ble_transport->set_on_client_connected(nullptr);
  _ble_transport->set_on_client_disconnected(nullptr);
  _ble_transport->teardown();

  _dns->stop(); // idempotent — safe whether start succeeded or not

  // Detach WifiManager callbacks before mode change (mirrors stop()).
  wifi.set_on_scan_complete(nullptr);
  wifi.set_on_ap_client_joined(nullptr);
  wifi.set_on_ap_client_left(nullptr);
  wifi.set_on_got_ip(nullptr);
  wifi.set_on_disconnected(nullptr);
  wifi.set_mode(WifiMode::Sta);

  // start() only calls http.start() at its final step; if we got here
  // the HTTP server is either not started or about to fail-start, so
  // unregistering routes is enough — no http.stop() needed.
  http.unregister_all();

  _wifi = nullptr;
  _http = nullptr;
}

void ProvisioningManager::_teardown_mdns_locked() {
  if (!_mdns_profile_installed || _wifi == nullptr) {
    return;
  }

  const WifiStatus clear_status = _wifi->clear_mdns_profile();
  if (clear_status != WifiStatus::Ok) {
    AG_LOGW(TAG, "provisioning mDNS stop/clear failed (%u); profile remains retained",
            static_cast<unsigned>(clear_status));
  }
  _mdns_profile_installed = false;
}

// ---------------------------------------------------------------------------
// Both-mode first-client teardown. Called with _mutex held. Leaves
// unique_ptrs alive; BleTransport::teardown() and _dns->stop() are
// already null-safe / idempotent.
// ---------------------------------------------------------------------------

void ProvisioningManager::_teardown_ble_transport_locked() {
  // Already torn down (e.g. repeat AP-client commit) — no-op.
  if (!_ble_active) {
    _ble_client_count = 0;
    return;
  }
  _ble_active = false;

  log_teardown_heap("BLE", "begin");
  AG_LOGI(TAG, "first AP client committed — tearing down BLE transport");

  // Clear callbacks first: deinit()'s disconnect must not re-enter
  // the manager while _mutex is held.
  _ble_transport->set_on_credentials(nullptr);
  _ble_transport->set_on_scan_request(nullptr);
  _ble_transport->set_on_client_connected(nullptr);
  _ble_transport->set_on_client_disconnected(nullptr);
  _ble_transport->teardown();

  _ble_client_count = 0;
  if (_total_client_count_locked() == 0) {
    _resume_timeout_locked();
  }

  log_teardown_heap("BLE", "done");
}

void ProvisioningManager::_teardown_wifi_transport_locked() {
  // Already torn down (e.g. repeat BLE-central commit) — no-op.
  if (!_wifi_active) {
    _ap_client_count = 0;
    return;
  }
  _wifi_active = false;

  log_teardown_heap("Wi-Fi", "begin");
  AG_LOGI(TAG, "first BLE client committed — tearing down Wi-Fi transport");

  _teardown_mdns_locked();
  _dns->stop();

  // Wipe portal routes but leave the listener bound. Provisioning retains
  // ownership until stop() returns.
  if (_http != nullptr) {
    _http->unregister_all();
  }

  // Detach AP-client callbacks so no stale *_left() lands after mode
  // change.
  if (_wifi != nullptr) {
    _wifi->set_on_ap_client_joined(nullptr);
    _wifi->set_on_ap_client_left(nullptr);
    _wifi->set_mode(WifiMode::Sta);
  }

  _ap_client_count = 0;
  if (_total_client_count_locked() == 0) {
    _resume_timeout_locked();
  }

  log_teardown_heap("Wi-Fi", "done");
}
