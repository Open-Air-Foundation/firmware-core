/**
 * AirGradient Go — WifiService implementation
 *
 * Stationary Wi-Fi mechanics. Mode policy lives in the orchestrator.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_wifi.h"

#include "ag_log.h"
#include "common.h"
#include "go_events.h"
#include "services/local_server.h"
#include "services/provisioning_manager.h"
#include "services/wifi_manager.h"

#include <cstring>

static constexpr const char *TAG = "WifiService";

// Saved-credentials policy. Bounded retry; backoffs use WifiManager defaults.
// Kept low so a runtime dead-AP drop reaches the terminal disconnect (and the
// disconnected icon + reconnect loop) promptly, while still riding out a
// transient missed sweep at bring-up.
static constexpr uint8_t STATIONARY_MAX_RETRY_COUNT = 3;

// Enable Minimal Wifi Power Saving --> MinModem (Wake at AP's DTIM interval, beacons, etc...)
static void enable_stationary_minimum_power_save(WifiManager &wifi) {
  const WifiStatus status = wifi.set_power_save(WifiPowerSave::MinModem);
  if (status != WifiStatus::Ok) {
    AG_LOGW(TAG, "failed to enable minimum Stationary Wi-Fi power save: %u",
            static_cast<unsigned>(status));
  }
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

WifiService::WifiService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg)
    : _event_queue(event_queue), _wifi(deps.wifi), _ble(deps.ble), _http(deps.http),
      _local_server(deps.local_server), _cfg(cfg) {
  _mdns_txt_values[0] = "AirGradient";
  _mdns_txt_values[1] = _cfg.model;
  _mdns_txt_values[2] = _cfg.serial_number;
  _mdns_txt_values[3] = _cfg.firmware_version;
  _mdns_txt_values[4] = "1";
  _install_wifi_callbacks();
}

WifiService::~WifiService() {
  if (_provisioning_active && _prov != nullptr) {
    _prov->stop();
    _provisioning_active = false;
  }
  stop_local_endpoint();
  _detach_wifi_callbacks();
  delete _prov;
}

// ---------------------------------------------------------------------------
// Credential queries
// ---------------------------------------------------------------------------

bool WifiService::has_saved_networks() const { return _wifi.has_saved_networks(); }

// ---------------------------------------------------------------------------
// Connection attempts
// ---------------------------------------------------------------------------

void WifiService::connect_with_saved_credentials(const WifiStaticIpConfig *static_ip) {
  _connect_saved_internal(static_ip, /*reset_online_latches=*/true, /*arm_window=*/true);
}

void WifiService::_connect_saved_internal(const WifiStaticIpConfig *static_ip,
                                          bool reset_online_latches, bool arm_window) {
  _install_wifi_callbacks();
  _reset_deadline();
  if (reset_online_latches) {
    _reset_online_latches();
  }

  if (static_ip != nullptr && static_ip->ip != 0) {
    _wifi.set_static_ip(*static_ip);
  } else {
    _wifi.clear_static_ip();
  }

  _wifi.set_mode(WifiMode::Sta);
  enable_stationary_minimum_power_save(_wifi);

  WifiStaConfig sta{}; // empty ssid = auto-connect from saved networks
  sta.max_retry_count = STATIONARY_MAX_RETRY_COUNT;

  const WifiStatus status = _wifi.connect(sta);
  if (status == WifiStatus::NotFound) {
    // No saved networks. Disconnect-policy router takes it from here.
    AG_LOGW(TAG, "saved-creds connect: no saved networks");
    _post_wifi_disconnected(WifiDisconnectReason::no_ap_found);
    return;
  }
  if (status != WifiStatus::Ok) {
    AG_LOGE(TAG, "saved-creds connect failed: %d", static_cast<int>(status));
    _post_wifi_disconnected(WifiDisconnectReason::unknown);
    return;
  }

  if (arm_window) {
    _arm_deadline(_cfg.initial_connect_window_ms);
  }
}

void WifiService::schedule_reconnect(const WifiStaticIpConfig *static_ip) {
  if (static_ip != nullptr && static_ip->ip != 0) {
    _reconnect_static_ip = *static_ip;
    _reconnect_has_static_ip = true;
  } else {
    _reconnect_has_static_ip = false;
  }
  _reconnect_at_ms = static_cast<uint32_t>(RTOS::get_time_ms()) + _cfg.reconnect_delay_ms;
  AG_LOGI(TAG, "reconnect scheduled in %u ms", static_cast<unsigned>(_cfg.reconnect_delay_ms));
}

void WifiService::try_default_fallback_credentials() {
  _connect_fallback_internal(/*reset_online_latches=*/true, /*arm_window=*/true);
}

void WifiService::_connect_fallback_internal(bool reset_online_latches, bool arm_window) {
  _install_wifi_callbacks();
  _reset_deadline();
  if (reset_online_latches) {
    _reset_online_latches();
  }
  _wifi.clear_static_ip();
  _wifi.set_mode(WifiMode::Sta);
  enable_stationary_minimum_power_save(_wifi);

  WifiStaConfig sta{};
  std::strncpy(sta.ssid, _cfg.fallback_ssid, sizeof(sta.ssid) - 1);
  std::strncpy(sta.password, _cfg.fallback_password, sizeof(sta.password) - 1);
  // Explicit SSID = transient connect (never saved); single-shot, no retry.
  sta.max_retry_count = 0;

  const WifiStatus status = _wifi.connect(sta);
  if (status != WifiStatus::Ok) {
    AG_LOGE(TAG, "fallback connect failed: %d", static_cast<int>(status));
    _post_wifi_disconnected(WifiDisconnectReason::unknown);
    return;
  }

  if (arm_window) {
    _arm_deadline(_cfg.fallback_connect_window_ms);
  }
}

// ---------------------------------------------------------------------------
// Provisioning
// ---------------------------------------------------------------------------

void WifiService::start_provisioning(ProvisioningTransport transport) {
  AG_LOGI(TAG, "start_provisioning(%u)", static_cast<unsigned>(transport));
  log_heap(TAG, "wifi.start_provisioning:enter");
  _provisioning_connected_event_pending.store(false);

  // Provisioning exclusively owns the listener and mDNS profile while active.
  stop_local_endpoint();

  // Cancel any in-flight STA connect so prov.start() can take over the
  // Wi-Fi callbacks cleanly. Idempotent when STA is already disconnected.
  _wifi.disconnect();

  // Zero deadlines before prov takes the callback slots — guards against a
  // stale tick() firing a synthetic disconnect mid-provisioning after a
  // fast credential-class fail.
  _reset_deadline();
  _reconnect_at_ms = 0;

  _ensure_provisioning_manager();
  if (_start_provisioning_internal(transport)) {
    _provisioning_active = true;
    _transport = transport;
  }
  log_heap(TAG, "wifi.start_provisioning:exit");
}

void WifiService::switch_provisioning_transport() {
  if (!_provisioning_active || _prov == nullptr) {
    AG_LOGW(TAG, "switch_provisioning_transport: not active");
    return;
  }

  const ProvisioningTransport prev = _transport;
  const ProvisioningTransport other = (prev == ProvisioningTransport::BleOnly)
                                          ? ProvisioningTransport::WifiOnly
                                          : ProvisioningTransport::BleOnly;
  AG_LOGI(TAG, "switching transport %u -> %u", static_cast<unsigned>(prev),
          static_cast<unsigned>(other));
  log_heap(TAG, "wifi.switch_transport:enter");

  _switching_transport = true;
  // Keep the shared listener bound while route ownership changes.
  _prov->stop(/*stop_http_server=*/false);
  log_heap(TAG, "wifi.switch_transport:after-teardown");

  // Update _transport before _prov->start so the Started event fired
  // during the inner start carries the destination transport, not the
  // (stale) source. Rolled back below if start fails.
  _transport = other;
  const bool ok = _start_provisioning_internal(other);
  _switching_transport = false;
  log_heap(TAG, "wifi.switch_transport:after-restart");

  if (!ok) {
    AG_LOGE(TAG, "switch start failed; synthesizing Stopped");
    _transport = prev; // rollback
    _provisioning_active = false;
    // Synthesize a Stopped event so the orchestrator's "Stopped before
    // online -> change_mode(Portable)" rule rescues the user.
    ProvisioningEventInfo synth{};
    synth.event = ProvisioningEvent::Stopped;
    synth.stop_reason = ProvisioningStopReason::ProductRequested;
    _post_provisioning_event(synth);
  }
}

void WifiService::stop_provisioning(bool stop_http_server) {
  if (!_provisioning_active || _prov == nullptr) {
    return;
  }
  AG_LOGI(TAG, "stop_provisioning");
  log_heap(TAG, "wifi.stop_provisioning:enter");
  // Blocks for ~1.5 s when called after Connected (component-side hold).
  _prov->stop(stop_http_server);
  _provisioning_active = false;
  // ProvisioningManager::stop() cleared the WifiManager callback slots;
  // restore ours so post-online disconnects and shutdown work.
  _install_wifi_callbacks();
  log_heap(TAG, "wifi.stop_provisioning:exit");
}

// ---------------------------------------------------------------------------
// Local endpoint
// ---------------------------------------------------------------------------

bool WifiService::ensure_local_http() {
  if (_local_http_active) {
    return true;
  }
  log_heap(TAG, "wifi.local-http:pre-activation");
  if (!_local_server.begin()) {
    return false;
  }
  if (!_http.start(_cfg.http_port)) {
    _local_server.end();
    return false;
  }
  log_heap(TAG, "wifi.local-http:post-activation");
  _local_http_active = true;
  return true;
}

bool WifiService::ensure_local_mdns() {
  if (!_local_http_active) {
    return false;
  }

  if (!_local_mdns_profile_installed) {
    WifiMdnsServiceRecord service{};
    service.service_type = "_airgradient._tcp";
    service.port = _cfg.http_port;
    service.txt_keys = _mdns_txt_keys;
    service.txt_values = _mdns_txt_values;
    service.txt_count = LOCAL_MDNS_TXT_COUNT;

    WifiMdnsProfile profile{};
    profile.config.hostname = _cfg.hostname;
    profile.config.services = &service;
    profile.config.service_count = 1;
    profile.lifecycle = WifiMdnsLifecycle::StaIpAuto;

    const WifiStatus status = _wifi.set_mdns_profile(profile);
    if (status != WifiStatus::Ok) {
      return false;
    }
    _local_mdns_profile_installed = true;
  }

  return _wifi.start_mdns() == WifiStatus::Ok;
}

void WifiService::stop_local_endpoint() {
  log_heap(TAG, "wifi.local-http:pre-deactivation");
  const WifiStatus mdns_status = _wifi.clear_mdns_profile();
  if (mdns_status != WifiStatus::Ok) {
    AG_LOGW(TAG, "local mDNS stop/clear failed: %u", static_cast<unsigned>(mdns_status));
  }
  _local_mdns_profile_installed = false;
  _http.stop();
  _local_server.end();
  _local_http_active = false;
  log_heap(TAG, "wifi.local-http:post-deactivation");
}

bool WifiService::_start_provisioning_internal(ProvisioningTransport transport) {
  ProvisioningConfig config{};
  if (_cfg.ap_ssid != nullptr) {
    std::strncpy(config.ap.ssid, _cfg.ap_ssid, sizeof(config.ap.ssid) - 1);
  }
  if (_cfg.ap_password != nullptr) {
    std::strncpy(config.ap.password, _cfg.ap_password, sizeof(config.ap.password) - 1);
  }
  config.ap.channel = _cfg.ap_channel;

  config.ble.device_name = _cfg.ble_device_name;
  config.ble.manufacturer_data = _cfg.ble_manufacturer_data;
  config.ble.model_name = _cfg.ble_model_name;
  config.ble.serial_number = _cfg.ble_serial_number;
  config.ble.firmware_version = _cfg.ble_firmware_version;
  config.ble.io_capability = AgBleIoCapability::NO_INPUT_NO_OUTPUT;
  config.ble.auth_flags = _cfg.ble_auth_flags;

  config.transport = transport;
  config.overall_timeout_ms = 0; // disabled per AGo policy
  config.hostname = _cfg.hostname;
  config.http_port = _cfg.http_port;

  return _prov->start(_wifi, _ble, _http, config);
}

void WifiService::_ensure_provisioning_manager() {
  if (_prov != nullptr) {
    return;
  }

  _prov = new ProvisioningManager();
  _prov->set_on_event([this](const ProvisioningEventInfo &info) { _on_provisioning_event(info); });
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void WifiService::shutdown() {
  _provisioning_connected_event_pending.store(false);
  _reset_deadline();
  _reconnect_at_ms = 0;
  if (_provisioning_active && _prov != nullptr) {
    _prov->stop();
    _provisioning_active = false;
  }
  stop_local_endpoint();
  _wifi.disconnect();
  _wifi.set_mode(WifiMode::Off);
  _detach_wifi_callbacks();
  _reset_online_latches();
}

void WifiService::clear_credentials() {
  _wifi.clear_networks();
  _reset_online_latches();
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool WifiService::is_online() const { return _online.load(); }

bool WifiService::is_connecting() const {
  return _initial_connect_deadline_ms != 0 && !_online.load();
}

bool WifiService::is_provisioning() const { return _provisioning_active; }

ProvisioningTransport WifiService::current_transport() const { return _transport; }

uint32_t WifiService::ip() const { return _ip.load(); }

int WifiService::rssi() const { return _rssi.load(); }

WifiDisconnectReason WifiService::last_disconnect_reason() const {
  return static_cast<WifiDisconnectReason>(_last_disconnect_reason.load());
}

bool WifiService::has_been_online() const { return _has_been_online.load(); }

// ---------------------------------------------------------------------------
// Timer integration
// ---------------------------------------------------------------------------

uint32_t WifiService::next_deadline_ms() const {
  // Nearer of the two armed timers (0 = not armed, so it never "wins").
  const uint32_t connect = _initial_connect_deadline_ms;
  const uint32_t reconnect = _reconnect_at_ms;
  if (connect == 0) {
    return reconnect;
  }
  if (reconnect == 0) {
    return connect;
  }
  return (reconnect < connect) ? reconnect : connect;
}

void WifiService::tick(uint32_t now_ms) {
  if (_provisioning_connected_event_pending.load()) {
    if (RTOS::queue_send(_event_queue, &_pending_provisioning_connected_event, 0)) {
      _provisioning_connected_event_pending.store(false);
    }
  }

  // Latched clear from on_got_ip (kept off the deadline writer thread).
  if (_clear_deadline_pending.exchange(false)) {
    _initial_connect_deadline_ms = 0;
  }

  if (_initial_connect_deadline_ms != 0 && now_ms >= _initial_connect_deadline_ms) {
    _initial_connect_deadline_ms = 0;
    AG_LOGW(TAG, "initial connect window expired");
    _post_wifi_disconnected(WifiDisconnectReason::connection_lost);
  }

  if (_reconnect_at_ms != 0 && now_ms >= _reconnect_at_ms) {
    _reconnect_at_ms = 0;
    // Keep has_been_online() latched (stay "runtime") and skip the connect
    // window; the WifiManager terminal disconnect drives the next cycle.
    if (_wifi.has_saved_networks()) {
      AG_LOGI(TAG, "runtime reconnect: attempting saved networks");
      const WifiStaticIpConfig *ip = _reconnect_has_static_ip ? &_reconnect_static_ip : nullptr;
      _connect_saved_internal(ip, /*reset_online_latches=*/false, /*arm_window=*/false);
    } else {
      AG_LOGI(TAG, "runtime reconnect: attempting default fallback");
      _connect_fallback_internal(/*reset_online_latches=*/false, /*arm_window=*/false);
    }
  }
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void WifiService::_install_wifi_callbacks() {
  _wifi.set_on_got_ip([this](uint32_t ip) { _on_got_ip(ip); });
  _wifi.set_on_disconnected([this](WifiDisconnectReason r) { _on_disconnected(r); });
}

void WifiService::_detach_wifi_callbacks() {
  _wifi.set_on_got_ip(nullptr);
  _wifi.set_on_disconnected(nullptr);
}

void WifiService::_on_got_ip(uint32_t ip) {
  AG_LOGI(TAG, "wifi online: ip=0x%08x", static_cast<unsigned>(ip));
  _ip.store(ip);
  _online.store(true);
  _has_been_online.store(true);
  // Defer deadline clear to tick() so deadline stays single-writer.
  _clear_deadline_pending.store(true);
  _rssi.store(_wifi.status_snapshot().rssi);

  Event evt{};
  evt.type = EventType::WifiConnected;
  evt.wifi_ip = ip;
  RTOS::queue_send(_event_queue, &evt);
}

void WifiService::_on_disconnected(WifiDisconnectReason reason) {
  AG_LOGI(TAG, "wifi disconnected: reason=%u", static_cast<unsigned>(reason));
  _online.store(false);
  _last_disconnect_reason.store(static_cast<uint8_t>(reason));
  if (reason == WifiDisconnectReason::requested_by_user) {
    return; // synthetic teardown reason; not an external failure
  }
  _post_wifi_disconnected(reason);
}

void WifiService::_reset_deadline() {
  _initial_connect_deadline_ms = 0;
  _clear_deadline_pending.store(false);
}

void WifiService::_arm_deadline(uint32_t window_ms) {
  _initial_connect_deadline_ms = static_cast<uint32_t>(RTOS::get_time_ms()) + window_ms;
}

void WifiService::_reset_online_latches() {
  _online.store(false);
  _has_been_online.store(false);
  _ip.store(0);
  _rssi.store(WIFI_RSSI_INVALID);
}

void WifiService::_post_wifi_disconnected(WifiDisconnectReason reason) {
  Event evt{};
  evt.type = EventType::WifiDisconnected;
  evt.wifi_disconnect_reason = static_cast<uint8_t>(reason);
  RTOS::queue_send(_event_queue, &evt);
}

void WifiService::_on_provisioning_event(const ProvisioningEventInfo &info) {
  // During a transport switch, swallow the intermediate Stopped so the
  // orchestrator does not interpret it as a user abort.
  if (_switching_transport && info.event == ProvisioningEvent::Stopped) {
    return;
  }

  // Connected is the online transition while provisioning owns the
  // Wi-Fi callback slot — mirror what on_got_ip would have latched so
  // has_been_online() reads true when the orchestrator processes the
  // subsequent Stopped from stop_provisioning's teardown.
  if (info.event == ProvisioningEvent::Connected) {
    _ip.store(info.ip);
    _online.store(true);
    _has_been_online.store(true);
    _rssi.store(_wifi.status_snapshot().rssi);
  }

  _post_provisioning_event(info);
}

void WifiService::_post_provisioning_event(const ProvisioningEventInfo &info) {
  Event evt{};
  evt.type = EventType::ProvisioningStateChanged;
  evt.prov.event = static_cast<uint8_t>(info.event);
  evt.prov.transport = static_cast<uint8_t>(_transport);
  evt.prov.stop_reason = static_cast<uint8_t>(info.stop_reason);
  evt.prov.ip = info.ip;
  evt.prov.disable_cloud = info.data.disable_cloud;
  evt.prov.static_ip = info.data.static_ip;
  if (!RTOS::queue_send(_event_queue, &evt, 0) && info.event == ProvisioningEvent::Connected &&
      !_provisioning_connected_event_pending.load()) {
    _pending_provisioning_connected_event = evt;
    _provisioning_connected_event_pending.store(true);
  }
}
