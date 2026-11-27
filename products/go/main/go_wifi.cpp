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

static constexpr uint32_t WIFI_CLEANUP_STACK_BYTES = 4096;
static constexpr uint32_t WIFI_CLEANUP_PRIORITY = 3;

static void apply_stationary_power_save(WifiManager &wifi, bool enabled) {
  const WifiPowerSave mode = enabled ? WifiPowerSave::MaxModem : WifiPowerSave::None;
  const WifiStatus status = wifi.set_power_save(mode);
  if (status != WifiStatus::Ok) {
    AG_LOGW(TAG, "failed to set Stationary Wi-Fi power save %s: %u",
            enabled ? "enabled" : "disabled", static_cast<unsigned>(status));
  } else {
    AG_LOGI(TAG, "Stationary Wi-Fi power save: %s", enabled ? "MAX_MODEM" : "NONE");
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
  if (_suspend_task != nullptr) {
    _wait_for_suspend();
    RTOS::task_delete(_suspend_task);
    _suspend_task = nullptr;
  }
  if (_prov != nullptr) {
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

void WifiService::set_power_save_enabled(bool enabled) {
  _power_save_enabled = enabled;
  if (!is_suspended() && !_provisioning_active && _wifi.get_mode() == WifiMode::Sta) {
    apply_stationary_power_save(_wifi, _power_save_enabled);
  }
}

// ---------------------------------------------------------------------------
// Connection attempts
// ---------------------------------------------------------------------------

void WifiService::connect_with_saved_credentials(const WifiStaticIpConfig *static_ip,
                                                 bool retain_for_ble) {
  _use_default_credentials = false;
  _connect_saved_internal(static_ip, /*reset_online_latches=*/true, /*arm_window=*/true);
  if (retain_for_ble) {
    _state.store(WifiState::ServicesSuspended);
  }
}

void WifiService::_connect_saved_internal(const WifiStaticIpConfig *static_ip,
                                          bool reset_online_latches, bool arm_window,
                                          const char *saved_ssid) {
  if (reset_online_latches) {
    _state.store(WifiState::Active);
  }
  _install_wifi_callbacks();
  if (arm_window) {
    _reset_deadline();
    _arm_deadline(_cfg.initial_connect_window_ms);
  }
  if (reset_online_latches) {
    _reset_online_latches();
  }

  if (static_ip != nullptr && static_ip->ip != 0) {
    _wifi.set_static_ip(*static_ip);
  } else {
    _wifi.clear_static_ip();
  }

  _wifi.set_mode(WifiMode::Sta);
  apply_stationary_power_save(_wifi, _power_save_enabled);

  WifiStaConfig sta{}; // empty ssid = auto-connect from saved networks
  // Report link loss immediately. Go owns retries and their total deadline.
  sta.max_retry_count = 0;

  const WifiStatus status =
      saved_ssid != nullptr ? _wifi.connect_saved_network(saved_ssid, sta) : _wifi.connect(sta);
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
}

void WifiService::schedule_reconnect(const WifiStaticIpConfig *static_ip) {
  if (is_suspended()) {
    return;
  }
  if (_clear_deadline_pending.exchange(false)) {
    _initial_connect_deadline_ms = 0;
  }
  if (_initial_connect_deadline_ms == 0) {
    _arm_deadline(_cfg.initial_connect_window_ms);
  }
  if (static_ip != nullptr && static_ip->ip != 0) {
    _reconnect_static_ip = *static_ip;
    _reconnect_has_static_ip = true;
  } else {
    _reconnect_has_static_ip = false;
  }
  _reconnect_at_ms = static_cast<uint32_t>(RTOS::get_time_ms()) + _cfg.reconnect_delay_ms;
  if (_reconnect_at_ms == 0) {
    _reconnect_at_ms = UINT32_MAX;
  }
  AG_LOGI(TAG, "reconnect scheduled in %u ms", static_cast<unsigned>(_cfg.reconnect_delay_ms));
}

void WifiService::try_default_fallback_credentials() {
  _use_default_credentials = true;
  _connect_fallback_internal(/*reset_online_latches=*/true, /*arm_window=*/true);
}

void WifiService::_connect_fallback_internal(bool reset_online_latches, bool arm_window) {
  _state.store(WifiState::Active);
  _install_wifi_callbacks();
  _reset_deadline();
  if (reset_online_latches) {
    _reset_online_latches();
  }
  _wifi.clear_static_ip();
  _wifi.set_mode(WifiMode::Sta);
  apply_stationary_power_save(_wifi, _power_save_enabled);

  WifiStaConfig sta{};
  std::strncpy(sta.ssid, _cfg.fallback_ssid, sizeof(sta.ssid) - 1);
  std::strncpy(sta.password, _cfg.fallback_password, sizeof(sta.password) - 1);
  // Explicit SSID = transient connect (never saved); single-shot, no retry.
  sta.max_retry_count = 0;

  // Manufacturing must still time out if the driver rejects the attempt.
  if (arm_window) {
    _arm_deadline(_cfg.fallback_connect_window_ms);
  }

  const WifiStatus status = _wifi.connect(sta);
  if (status != WifiStatus::Ok) {
    AG_LOGE(TAG, "fallback connect failed: %d", static_cast<int>(status));
    _post_wifi_disconnected(WifiDisconnectReason::unknown);
    return;
  }
}

// ---------------------------------------------------------------------------
// Provisioning
// ---------------------------------------------------------------------------

bool WifiService::attach_ble_provisioning() {
  if (_provisioning_attached) {
    return true;
  }
  if (_provisioning_active) {
    return false;
  }
  _ensure_provisioning_manager();
  _prov->set_attached_request_hook(
      [this](const AttachedRequest &request) { _on_ble_provisioning_request(request); });
  ProvisioningConfig config{};
  config.transport = ProvisioningTransport::BleAttached;
  config.ble.device_name = _cfg.ble_device_name;
  config.ble.model_name = _cfg.ble_model_name;
  config.ble.serial_number = _cfg.ble_serial_number;
  config.ble.firmware_version = _cfg.ble_firmware_version;
  config.overall_timeout_ms = 0;
  _transport = ProvisioningTransport::BleAttached;
  _provisioning_attached = _prov->start_attached(_wifi, _ble, config, /*own_wifi_callbacks=*/false);
  return _provisioning_attached;
}

void WifiService::detach_ble_provisioning() {
  if (!_provisioning_attached) {
    return;
  }
  if (_provisioning_active) {
    _provisioning_active = false;
    _reset_deadline();
    _wifi.disconnect();
  }
  _provisioning_connected_event_pending.store(false);
  _prov->stop();
  delete _prov;
  _prov = nullptr;
  _provisioning_attached = false;
  _provisioning_request_mutex.lock();
  _provisioning_request_pending = false;
  _provisioning_request_mutex.unlock();
}

bool WifiService::ble_provisioning_attached() const { return _provisioning_attached; }

void WifiService::_on_ble_provisioning_request(const AttachedRequest &request) {
  _provisioning_request_mutex.lock();
  _provisioning_request = request;
  _provisioning_request_pending = true;
  _provisioning_request_mutex.unlock();
  Event event{};
  event.type = EventType::BleProvisioningRequest;
  RTOS::queue_send(_event_queue, &event);
}

void WifiService::handle_ble_provisioning_request(bool allowed) {
  _provisioning_request_mutex.lock();
  if (!_provisioning_request_pending) {
    _provisioning_request_mutex.unlock();
    return;
  }
  const AttachedRequest request = _provisioning_request;
  _provisioning_request_pending = false;
  _provisioning_request_mutex.unlock();
  if (!_provisioning_attached) {
    return;
  }
  if (!allowed || _provisioning_active || suspension_pending() || is_connecting()) {
    _prov->send_ble_status(ProvisioningBleStatus::WIFI_CONNECT_FAILED);
    return;
  }
  if (_wifi.initialize() != WifiStatus::Ok || _wifi.set_mode(WifiMode::Sta) != WifiStatus::Ok) {
    _prov->send_ble_status(ProvisioningBleStatus::WIFI_CONNECT_FAILED);
    return;
  }
  _state.store(WifiState::ServicesSuspended);
  _install_wifi_callbacks();
  apply_stationary_power_save(_wifi, false);
  _provisioning_active = true;
  if (request.kind == AttachedRequestKind::Scan) {
    if (!_prov->request_scan()) {
      _prov->send_ble_status(ProvisioningBleStatus::WIFI_CONNECT_FAILED);
      finish_ble_provisioning();
    }
  } else {
    _reset_deadline();
    _reconnect_at_ms = 0;
    _online.store(false);
    _arm_deadline(_cfg.initial_connect_window_ms);
    if (!_prov->submit_credentials(request.data)) {
      finish_ble_provisioning();
    }
  }
}

void WifiService::finish_ble_provisioning() {
  if (!_provisioning_attached) {
    return;
  }
  _provisioning_active = false;
  _reset_deadline();
  _prov->reset_to_listening();
  apply_stationary_power_save(_wifi, _power_save_enabled);
}

void WifiService::_on_scan_results(const WifiScanEntry *entries, uint16_t count) {
  if (!_provisioning_attached || !_provisioning_active ||
      _prov->state() != ProvisioningState::WaitingForCredentials) {
    return;
  }
  _prov->handle_scan_results(entries, count);
  Event event{};
  event.type = EventType::BleProvisioningScanComplete;
  RTOS::queue_send(_event_queue, &event);
}

void WifiService::start_provisioning(ProvisioningTransport transport) {
  if (transport == ProvisioningTransport::BleAttached) {
    _reset_deadline();
    _reconnect_at_ms = 0;
    _wifi.disconnect();
    _online.store(false);
    _state.store(WifiState::ServicesSuspended);
    _install_wifi_callbacks();
    ProvisioningEventInfo info{};
    info.event = _provisioning_attached ? ProvisioningEvent::Started : ProvisioningEvent::Stopped;
    _post_provisioning_event(info);
    return;
  }
  if (_wifi.initialize() != WifiStatus::Ok) {
    AG_LOGE(TAG, "Wi-Fi initialization failed for provisioning");
    ProvisioningEventInfo failure{};
    failure.event = ProvisioningEvent::Stopped;
    failure.stop_reason = ProvisioningStopReason::ProductRequested;
    _post_provisioning_event(failure);
    return;
  }
  _state.store(WifiState::Active);
  _use_default_credentials = false;
  _portal_used = false;
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
  if (_provisioning_attached) {
    if (_provisioning_active && _prov->state() == ProvisioningState::Connecting) {
      _wifi.disconnect();
      _prov->handle_wifi_disconnected();
    }
    finish_ble_provisioning();
    return;
  }
  if (!_provisioning_active || _prov == nullptr) {
    return;
  }
  AG_LOGI(TAG, "stop_provisioning");
  log_heap(TAG, "wifi.stop_provisioning:enter");
  // Blocks for ~1.5 s when called after Connected (component-side hold).
  _prov->stop(stop_http_server);
  _provisioning_active = false;
  // Release transport caches and timers before cloud TLS needs the heap.
  delete _prov;
  _prov = nullptr;
  // ProvisioningManager::stop() cleared the WifiManager callback slots;
  // restore ours so post-online disconnects and shutdown work.
  _install_wifi_callbacks();
  if (_online.load()) {
    apply_stationary_power_save(_wifi, _power_save_enabled);
  }
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
  if (transport == ProvisioningTransport::WifiOnly || transport == ProvisioningTransport::Both) {
    _portal_used = true;
  }
  // Keep onboarding responsive without changing the saved preference.
  apply_stationary_power_save(_wifi, false);
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
  _wait_for_suspend();
  if (_state.load() == WifiState::SuspendingServices) {
    // Shutdown stops STA even if service-only completion has not been consumed.
    _state.store(WifiState::Suspending);
  }
  // Leave a completed worker request pending until its event is acknowledged.
  if (!suspension_pending() && !driver_released()) {
    _state.store(WifiState::Suspended);
  }
  _use_default_credentials = false;
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

void WifiService::_wait_for_suspend() {
  if (suspension_pending()) {
    _suspend_done_sem.take();
    // Keep completion available for repeated shutdown or destruction before
    // the orchestrator consumes the event.
    _suspend_done_sem.give();
  }
}

bool WifiService::request_suspend(SuspendMode mode) {
  if (suspension_pending()) {
    return true;
  }
  // A driver already released stays released after another cleanup request.
  if (driver_released()) {
    mode = SuspendMode::ReleaseDriver;
  }
  switch (mode) {
  case SuspendMode::Services:
    _state.store(WifiState::SuspendingServices);
    break;
  case SuspendMode::StopWifi:
    _state.store(WifiState::Suspending);
    break;
  case SuspendMode::ReleaseDriver:
    _state.store(WifiState::Releasing);
    break;
  }
  if (mode != SuspendMode::Services) {
    _provisioning_connected_event_pending.store(false);
    _reset_deadline();
    _reconnect_at_ms = 0;
    _online.store(false);
    _ip.store(0);
    _rssi.store(WIFI_RSSI_INVALID);
  }
  if (_suspend_task == nullptr) {
    if (!_suspend_done_sem.create()) {
      complete_suspend(false);
      return false;
    }
    if (!RTOS::task_create(_suspend_entry, "wifi_cleanup", WIFI_CLEANUP_STACK_BYTES, this,
                           WIFI_CLEANUP_PRIORITY, &_suspend_task)) {
      _suspend_done_sem.destroy();
      complete_suspend(false);
      return false;
    }
  }
  _suspend_done_sem.take(0);
  RTOS::task_notify_send(_suspend_task, static_cast<uint32_t>(mode));
  return true;
}

void WifiService::_suspend_entry(void *arg) {
  auto *self = static_cast<WifiService *>(arg);
  for (;;) {
    uint32_t request = 0;
    if (RTOS::task_notify_wait(&request, UINT32_MAX)) {
      self->_run_suspend(static_cast<SuspendMode>(request));
    }
  }
}

void WifiService::_run_suspend(SuspendMode mode) {
  Event event{};
  event.type = EventType::WifiSuspendComplete;
  event.wifi_suspend_ok = _suspend(mode);
  // Shutdown may wait without draining the event queue. Signal first so it
  // can finish even if the worker is waiting for queue space.
  _suspend_done_sem.give();
  RTOS::queue_send(_event_queue, &event, UINT32_MAX);
}

bool WifiService::_suspend(SuspendMode mode) {
  log_heap(TAG, "wifi.suspend:enter");
  // A failed mDNS stop must not be treated as successful ownership release.
  if (_wifi.clear_mdns_profile() != WifiStatus::Ok) {
    AG_LOGE(TAG, "handoff: mDNS cleanup failed; retry required");
    return false;
  }
  _local_mdns_profile_installed = false;
  _http.stop();
  _local_server.end();
  _local_http_active = false;
  if (mode == SuspendMode::Services) {
    return true;
  }
  if (_wifi.set_mode(WifiMode::Off) != WifiStatus::Ok) {
    AG_LOGE(TAG, "handoff: Wi-Fi stop failed; retry required");
    return false;
  }
  log_heap(TAG, "wifi.suspend:radio-stopped");
  if (mode == SuspendMode::ReleaseDriver) {
    if (_wifi.deinitialize() != WifiStatus::Ok) {
      return false;
    }
    log_heap(TAG, "wifi.suspend:driver-released");
  }
  return true;
}

void WifiService::complete_suspend(bool success) {
  const auto state = _state.load();
  if (!suspension_pending()) {
    return;
  }
  if (state == WifiState::SuspendingServices) {
    _state.store(success ? WifiState::ServicesSuspended : WifiState::ServicesCleanupFailed);
    return;
  }
  if (!success) {
    _state.store(WifiState::CleanupFailed);
    return;
  }
  _state.store(state == WifiState::Releasing ? WifiState::Released : WifiState::Suspended);
}

bool WifiService::resume_station(const WifiStaticIpConfig *static_ip, const char *saved_ssid) {
  if (suspension_pending()) {
    return false;
  }
  if (station_retained()) {
    _state.store(WifiState::Active);
    return true;
  }
  if (_wifi.initialize() != WifiStatus::Ok) {
    return false;
  }
  _state.store(WifiState::Active);
  _connect_saved_internal(static_ip, false, true, saved_ssid);
  return true;
}

void WifiService::clear_credentials() {
  _use_default_credentials = false;
  _wifi.clear_networks();
  _reset_online_latches();
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool WifiService::suspension_pending() const {
  const auto state = _state.load();
  return state == WifiState::SuspendingServices || state == WifiState::Suspending ||
         state == WifiState::Releasing;
}
bool WifiService::station_retained() const {
  const auto state = _state.load();
  return state == WifiState::SuspendingServices || state == WifiState::ServicesSuspended ||
         state == WifiState::ServicesCleanupFailed;
}
bool WifiService::is_suspended() const {
  return _state.load() != WifiState::Active && !station_retained();
}
bool WifiService::driver_released() const { return _state.load() == WifiState::Released; }

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
  return static_cast<int32_t>(reconnect - connect) < 0 ? reconnect : connect;
}

void WifiService::tick(uint32_t now_ms) {
  if (is_suspended()) {
    return;
  }
  if (_provisioning_connected_event_pending.load()) {
    if (RTOS::queue_send(_event_queue, &_pending_provisioning_connected_event, 0)) {
      _provisioning_connected_event_pending.store(false);
    }
  }

  // Latched clear from on_got_ip (kept off the deadline writer thread).
  if (_clear_deadline_pending.exchange(false)) {
    _initial_connect_deadline_ms = 0;
  }

  if (_provisioning_attached && _provisioning_active) {
    if (_initial_connect_deadline_ms != 0 &&
        static_cast<int32_t>(now_ms - _initial_connect_deadline_ms) >= 0 &&
        _prov->state() == ProvisioningState::Connecting) {
      _initial_connect_deadline_ms = 0;
      _wifi.disconnect();
      _prov->handle_wifi_disconnected();
    }
    return;
  }
  if (_initial_connect_deadline_ms != 0 &&
      static_cast<int32_t>(now_ms - _initial_connect_deadline_ms) >= 0) {
    // Keep the deadline due until admitted so queue pressure cannot strand setup.
    Event event{};
    event.type = EventType::WifiConnectionTimeout;
    _reconnect_at_ms = 0;
    if (RTOS::queue_send(_event_queue, &event)) {
      _initial_connect_deadline_ms = 0;
      AG_LOGW(TAG, "Wi-Fi connection window expired");
    }
    return;
  }

  if (_reconnect_at_ms != 0 && static_cast<int32_t>(now_ms - _reconnect_at_ms) >= 0) {
    _reconnect_at_ms = 0;
    // Preserve the original reconnect deadline across internal retry attempts.
    if (!_use_default_credentials) {
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
  _wifi.set_on_scan_complete(
      [this](const WifiScanEntry *entries, uint16_t count) { _on_scan_results(entries, count); });
}

void WifiService::_detach_wifi_callbacks() {
  _wifi.set_on_got_ip(nullptr);
  _wifi.set_on_disconnected(nullptr);
  _wifi.set_on_scan_complete(nullptr);
}

void WifiService::_on_got_ip(uint32_t ip) {
  if (is_suspended()) {
    return;
  }
  AG_LOGI(TAG, "wifi online: ip=0x%08x", static_cast<unsigned>(ip));
  _ip.store(ip);
  _online.store(true);
  _has_been_online.store(true);
  // Defer deadline clear to tick() so deadline stays single-writer.
  _clear_deadline_pending.store(true);
  _rssi.store(_wifi.status_snapshot().rssi);

  if (_provisioning_attached && _provisioning_active &&
      _prov->state() == ProvisioningState::Connecting) {
    _prov->handle_wifi_connected(ip);
    return;
  }
  Event evt{};
  evt.type = EventType::WifiConnected;
  evt.wifi_ip = ip;
  RTOS::queue_send(_event_queue, &evt);
}

void WifiService::_on_disconnected(WifiDisconnectReason reason) {
  if (is_suspended()) {
    return;
  }
  AG_LOGI(TAG, "wifi disconnected: reason=%u", static_cast<unsigned>(reason));
  _online.store(false);
  _last_disconnect_reason.store(static_cast<uint8_t>(reason));
  if (reason == WifiDisconnectReason::requested_by_user) {
    return; // synthetic teardown reason; not an external failure
  }
  if (_provisioning_attached && _provisioning_active &&
      _prov->state() == ProvisioningState::Connecting) {
    _prov->handle_wifi_disconnected();
    return;
  }
  _post_wifi_disconnected(reason);
}

void WifiService::_reset_deadline() {
  _initial_connect_deadline_ms = 0;
  _clear_deadline_pending.store(false);
}

void WifiService::_arm_deadline(uint32_t window_ms) {
  _initial_connect_deadline_ms = static_cast<uint32_t>(RTOS::get_time_ms()) + window_ms;
  if (_initial_connect_deadline_ms == 0) {
    _initial_connect_deadline_ms = UINT32_MAX; // Zero is unarmed; expire one millisecond earlier.
  }
}

void WifiService::_reset_online_latches() {
  _online.store(false);
  _has_been_online.store(false);
  _ip.store(0);
  _rssi.store(WIFI_RSSI_INVALID);
}

bool WifiService::_post_wifi_disconnected(WifiDisconnectReason reason) {
  Event evt{};
  evt.type = EventType::WifiDisconnected;
  evt.wifi_disconnect_reason = static_cast<uint8_t>(reason);
  return RTOS::queue_send(_event_queue, &evt);
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
  evt.prov.portal_used = _portal_used;
  evt.prov.static_ip = info.data.static_ip;
  if (!RTOS::queue_send(_event_queue, &evt, 0) && info.event == ProvisioningEvent::Connected &&
      !_provisioning_connected_event_pending.load()) {
    _pending_provisioning_connected_event = evt;
    _provisioning_connected_event_pending.store(true);
  }
}
