/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "wifi_manager.h"

#include <cstring>

#include "../hal/wifi_hal.h"
#include "ag_log.h"

static constexpr const char *TAG = "WifiManager";

#ifdef CONFIG_AG_WIFI_DHCP_TIMEOUT_MS
static constexpr uint32_t k_default_dhcp_timeout_ms = CONFIG_AG_WIFI_DHCP_TIMEOUT_MS;
#else
static constexpr uint32_t k_default_dhcp_timeout_ms = WIFI_DEFAULT_DHCP_TIMEOUT_MS;
#endif

WifiManager::WifiManager(WifiHal &hal, ConfigStore &store) : _hal(hal), _creds(store) {
  _dhcp_timeout_ms = k_default_dhcp_timeout_ms;

  _hal.set_on_sta_connected([this]() { _on_hal_sta_connected(); });
  _hal.set_on_sta_disconnected([this](int reason) { _on_hal_sta_disconnected(reason); });
  _hal.set_on_got_ip([this](uint32_t ip) { _on_hal_got_ip(ip); });
  _hal.set_on_scan_complete(
      [this](const WifiScanEntry *r, uint16_t c) { _on_hal_scan_complete(r, c); });
  _hal.set_on_ap_client_joined([this](const uint8_t mac[6]) { _on_hal_ap_client_joined(mac); });
  _hal.set_on_ap_client_left([this](const uint8_t mac[6]) { _on_hal_ap_client_left(mac); });
  _hal.set_on_dhcp_timeout([this]() { _on_hal_dhcp_timeout(); });
  _hal.set_on_retry_due([this]() { _on_hal_retry_due(); });
}

WifiManager::~WifiManager() {
  // Detach callbacks so the HAL cannot call back into a destroyed manager.
  _hal.set_on_sta_connected(nullptr);
  _hal.set_on_sta_disconnected(nullptr);
  _hal.set_on_got_ip(nullptr);
  _hal.set_on_scan_complete(nullptr);
  _hal.set_on_ap_client_joined(nullptr);
  _hal.set_on_ap_client_left(nullptr);
  _hal.set_on_dhcp_timeout(nullptr);
  _hal.set_on_retry_due(nullptr);
}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

WifiDisconnectReason WifiManager::map_disconnect_reason(int raw_reason) {
  // Numeric values come from esp_wifi_types_generic.h (wifi_err_reason_t).
  // We avoid pulling that header into pure-C++ code by hard-coding the
  // values. Keep this table in sync with the spec mapping.
  switch (raw_reason) {
  // AuthFailed
  case 2:   // WIFI_REASON_AUTH_EXPIRE
  case 6:   // WIFI_REASON_NOT_AUTHED (deprecated)
  case 202: // WIFI_REASON_AUTH_FAIL
    return WifiDisconnectReason::auth_failed;

  // NoApFound
  case 201: // WIFI_REASON_NO_AP_FOUND
  case 210: // WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY
  case 211: // WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD
  case 212: // WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD
    return WifiDisconnectReason::no_ap_found;

  // AssocFailed
  case 4: // WIFI_REASON_ASSOC_EXPIRE (deprecated)
  case 5: // WIFI_REASON_ASSOC_TOOMANY
  case 7: // WIFI_REASON_NOT_ASSOCED (deprecated)
  case 9: // WIFI_REASON_ASSOC_NOT_AUTHED
    return WifiDisconnectReason::assoc_failed;

  // ApDisconnected
  case 8:   // WIFI_REASON_ASSOC_LEAVE
  case 206: // WIFI_REASON_AP_TSF_RESET
    return WifiDisconnectReason::ap_disconnected;

  // ConnectionLost
  case 200: // WIFI_REASON_BEACON_TIMEOUT
  case 208: // WIFI_REASON_ASSOC_COMEBACK_TIME_TOO_LONG
    return WifiDisconnectReason::connection_lost;

  // HandshakeFailed
  case 14:  // WIFI_REASON_MIC_FAILURE
  case 15:  // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
  case 16:  // WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT
  case 17:  // WIFI_REASON_IE_IN_4WAY_DIFFERS
  case 204: // WIFI_REASON_HANDSHAKE_TIMEOUT
    return WifiDisconnectReason::handshake_failed;

  default:
    return WifiDisconnectReason::unknown;
  }
}

bool WifiManager::is_retriable(WifiDisconnectReason reason) {
  switch (reason) {
  case WifiDisconnectReason::ap_disconnected:
  case WifiDisconnectReason::connection_lost:
  case WifiDisconnectReason::handshake_failed:
  case WifiDisconnectReason::unknown:
  // A single sweep can miss a present AP's probe response; retry so a
  // transient miss spends the budget instead of failing on first sweep.
  case WifiDisconnectReason::no_ap_found:
    return true;
  case WifiDisconnectReason::auth_failed:
  case WifiDisconnectReason::assoc_failed:
  case WifiDisconnectReason::dhcp_failed:
  case WifiDisconnectReason::requested_by_user:
    return false;
  }
  return false;
}

uint32_t WifiManager::compute_backoff_ms(uint32_t initial_ms, uint32_t cap_ms, uint32_t attempt) {
  if (initial_ms == 0) {
    return 0;
  }
  // Doubling per attempt; clamp to cap and avoid overflow.
  uint32_t value = initial_ms;
  for (uint32_t i = 0; i < attempt; ++i) {
    if (value > cap_ms / 2) {
      return cap_ms;
    }
    value *= 2;
  }
  if (value > cap_ms) {
    value = cap_ms;
  }
  return value;
}

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

WifiStatus WifiManager::set_mdns_config(const WifiMdnsConfig &config) {
  WifiMdnsProfile profile;
  profile.config = config;
  profile.lifecycle = WifiMdnsLifecycle::StaIpAuto;
  return set_mdns_profile(profile);
}

WifiStatus WifiManager::set_mdns_profile(const WifiMdnsProfile &profile) {
  if (!_is_mdns_profile_valid(profile)) {
    return WifiStatus::InvalidArgument;
  }

  const WifiStatus stop_status = stop_mdns();
  if (stop_status != WifiStatus::Ok) {
    return stop_status;
  }

  for (WifiMdnsServiceRecord &service : _mdns_services) {
    service = {};
  }
  const size_t hostname_length = std::strlen(profile.config.hostname);
  std::memcpy(_mdns_hostname, profile.config.hostname, hostname_length + 1);
  for (uint8_t i = 0; i < profile.config.service_count; ++i) {
    _mdns_services[i] = profile.config.services[i];
  }

  _mdns_profile = profile;
  _mdns_profile.config.hostname = _mdns_hostname;
  _mdns_profile.config.services = (profile.config.service_count > 0) ? _mdns_services : nullptr;
  _has_mdns_profile = true;
  return WifiStatus::Ok;
}

WifiStatus WifiManager::start_mdns() {
  if (_mdns_running) {
    return WifiStatus::Ok;
  }
  if (!_has_mdns_profile) {
    return WifiStatus::InvalidState;
  }
  const WifiStatus status = _hal.start_mdns(_mdns_profile.config);
  if (status == WifiStatus::Ok) {
    _mdns_running = true;
  }
  return status;
}

WifiStatus WifiManager::stop_mdns() {
  if (!_mdns_running) {
    return WifiStatus::Ok;
  }
  const WifiStatus status = _hal.stop_mdns();
  if (status == WifiStatus::Ok) {
    _mdns_running = false;
  }
  return status;
}

WifiStatus WifiManager::clear_mdns_profile() {
  const WifiStatus status = stop_mdns();
  if (status != WifiStatus::Ok) {
    return status;
  }
  _mdns_profile = {};
  for (WifiMdnsServiceRecord &service : _mdns_services) {
    service = {};
  }
  std::memset(_mdns_hostname, 0, sizeof(_mdns_hostname));
  _has_mdns_profile = false;
  return WifiStatus::Ok;
}

WifiStatus WifiManager::set_static_ip(const WifiStaticIpConfig &config) {
  return _hal.set_static_ip(config);
}

WifiStatus WifiManager::clear_static_ip() { return _hal.clear_static_ip(); }

WifiStatus WifiManager::set_power_save(WifiPowerSave mode) { return _hal.set_power_save(mode); }

void WifiManager::set_dhcp_timeout_ms(uint32_t timeout_ms) { _dhcp_timeout_ms = timeout_ms; }

// ---------------------------------------------------------------------------
// Mode control
// ---------------------------------------------------------------------------

WifiStatus WifiManager::set_mode(WifiMode mode) {
  const WifiMode current = _hal.get_mode();
  if (mode == WifiMode::Off) {
    const WifiStatus mdns_status = stop_mdns();
    if (mdns_status != WifiStatus::Ok) {
      return mdns_status;
    }
  }
  if (current == mode) {
    return WifiStatus::Ok;
  }

  // Leaving STA: emit requested_by_user and swallow the driver echo,
  // mirroring disconnect(). Otherwise the echo would be classified as
  // Unknown (retriable) and arm a retry in a mode without STA.
  const bool leaving_sta = (current == WifiMode::Sta || current == WifiMode::ApSta) &&
                           (mode == WifiMode::Off || mode == WifiMode::Ap);
  if (leaving_sta) {
    _hal.cancel_retry_timer();
    _hal.cancel_dhcp_timeout();
    _stop_mdns_auto_if_running();
    _clear_auto_state();
    const WifiStaState prev = _sta_state;
    _sta_state = WifiStaState::Disconnected;
    _retry_attempt = 0;
    _disconnect_requested = true;
    if (prev != WifiStaState::Disconnected) {
      _emit_disconnected(WifiDisconnectReason::requested_by_user);
    }
  }

  return _hal.set_mode(mode);
}

WifiMode WifiManager::get_mode() const { return _hal.get_mode(); }

WifiStatus WifiManager::initialize() { return _hal.init(); }

WifiStatus WifiManager::deinitialize() {
  const WifiStatus status = set_mode(WifiMode::Off);
  if (status != WifiStatus::Ok) {
    return status;
  }
  _hal.deinit();
  return WifiStatus::Ok;
}

// ---------------------------------------------------------------------------
// STA operations
// ---------------------------------------------------------------------------

WifiStatus WifiManager::connect(const WifiStaConfig &config) {
  const WifiMode mode = _hal.get_mode();
  if (mode != WifiMode::Sta && mode != WifiMode::ApSta) {
    return WifiStatus::InvalidState;
  }
  // Re-entrancy: the auto flags also cover the internal-scan window, which
  // leaves the STA Disconnected.
  if (_sta_state == WifiStaState::Connecting || _auto_scan_pending || _auto_sweeping) {
    return WifiStatus::AlreadyInProgress;
  }

  // Empty SSID = auto-connect from saved networks.
  if (config.ssid[0] == '\0') {
    return _start_auto_connect(config);
  }

  // Explicit SSID = one-shot transient connect; never writes the store.
  _sta_config = config;
  _has_sta_config = true;
  _retry_attempt = 0;
  _disconnect_requested = false;
  _start_connect_attempt();
  return WifiStatus::Ok;
}

WifiStatus WifiManager::connect_saved_network(const char *ssid, const WifiStaConfig &config) {
  if (ssid == nullptr || ssid[0] == '\0') {
    return WifiStatus::InvalidArgument;
  }
  WifiCredential saved[WIFI_MAX_SAVED_NETWORKS];
  const auto count = _creds.load_all(saved, WIFI_MAX_SAVED_NETWORKS);
  for (uint8_t i = 0; i < count; ++i) {
    if (std::strcmp(saved[i].ssid, ssid) == 0) {
      WifiStaConfig selected = config;
      std::memcpy(selected.ssid, saved[i].ssid, sizeof(selected.ssid));
      std::memcpy(selected.password, saved[i].password, sizeof(selected.password));
      return connect(selected);
    }
  }
  return WifiStatus::NotFound;
}

bool WifiManager::select_saved_network(const WifiScanEntry *results, uint16_t count,
                                       char (&ssid)[33]) const {
  ssid[0] = '\0';
  char saved[WIFI_MAX_SAVED_NETWORKS][33];
  const auto saved_count = list_networks(saved, WIFI_MAX_SAVED_NETWORKS);
  int8_t best_rssi = 0;
  for (uint8_t i = 0; i < saved_count; ++i) {
    for (uint16_t j = 0; j < count; ++j) {
      if (std::strcmp(saved[i], results[j].ssid) == 0 &&
          (ssid[0] == '\0' || results[j].rssi > best_rssi)) {
        best_rssi = results[j].rssi;
        std::memcpy(ssid, saved[i], sizeof(ssid));
      }
    }
  }
  return ssid[0] != '\0';
}

WifiStatus WifiManager::disconnect() {
  _disconnect_requested = true;
  _hal.cancel_retry_timer();
  _hal.cancel_dhcp_timeout();
  _clear_auto_state();
  const WifiStaState prev = _sta_state;
  _sta_state = WifiStaState::Disconnected;
  _retry_attempt = 0;
  _stop_mdns_auto_if_running();

  WifiStatus status = WifiStatus::Ok;
  if (prev == WifiStaState::Connecting || prev == WifiStaState::Connected ||
      prev == WifiStaState::GotIp) {
    status = _hal.disconnect_sta();
  }
  // Synthesise a RequestedByUser reason for the product-facing callback.
  // Only fire if we actually had an active connection or pending retry to
  // tear down — silent for an already-disconnected state.
  if (prev != WifiStaState::Disconnected) {
    _emit_disconnected(WifiDisconnectReason::requested_by_user);
  }
  return status;
}

// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

WifiStatus WifiManager::start_scan(const WifiScanConfig &config) {
  const WifiMode mode = _hal.get_mode();
  if (mode != WifiMode::Sta && mode != WifiMode::ApSta) {
    return WifiStatus::InvalidState;
  }
  // An auto cycle owns the radio for its internal scan; product scans must
  // not share it.
  if (_auto_scan_pending || _auto_sweeping) {
    return WifiStatus::InvalidState;
  }
  // Spec answer 3: only allow scan when STA is fully disconnected.
  if (_sta_state != WifiStaState::Disconnected) {
    return WifiStatus::InvalidState;
  }
  return _hal.start_scan(config);
}

// ---------------------------------------------------------------------------
// AP
// ---------------------------------------------------------------------------

WifiStatus WifiManager::start_ap(const WifiApConfig &config) {
  const WifiMode mode = _hal.get_mode();
  if (mode != WifiMode::Ap && mode != WifiMode::ApSta) {
    return WifiStatus::InvalidState;
  }
  if (config.ssid[0] == '\0') {
    return WifiStatus::InvalidArgument;
  }
  return _hal.start_ap(config);
}

// ---------------------------------------------------------------------------
// Credential storage
// ---------------------------------------------------------------------------

WifiStatus WifiManager::add_network(const char *ssid, const char *password) {
  return _creds.add(ssid, password);
}

WifiStatus WifiManager::remove_network(const char *ssid) { return _creds.remove(ssid); }

uint8_t WifiManager::list_networks(char (*out)[33], uint8_t max) const {
  return _creds.list(out, max);
}

bool WifiManager::has_saved_networks() const { return _creds.has_networks(); }

WifiStatus WifiManager::clear_networks() { return _creds.clear(); }

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

WifiStatusSnapshot WifiManager::status_snapshot() const {
  WifiStatusSnapshot snapshot = _hal.get_status();
  // Manager owns sta_state; HAL only sees raw events.
  snapshot.sta_state = _sta_state;
  // Drop stale STA-only fields when disconnected (esp-idf does not fire
  // IP_EVENT_STA_LOST_IP on esp_wifi_stop, so the HAL cache lingers).
  if (_sta_state == WifiStaState::Disconnected) {
    snapshot.ip = WIFI_IP_INVALID;
    snapshot.rssi = WIFI_RSSI_INVALID;
    snapshot.channel = 0;
    for (uint8_t i = 0; i < sizeof(snapshot.bssid); ++i) {
      snapshot.bssid[i] = 0;
    }
    snapshot.ssid[0] = '\0';
  }
  return snapshot;
}

// ---------------------------------------------------------------------------
// Callback setters
// ---------------------------------------------------------------------------

void WifiManager::set_on_connected(WifiConnectedCallback cb) { _on_connected = std::move(cb); }
void WifiManager::set_on_disconnected(WifiDisconnectedCallback cb) {
  _on_disconnected = std::move(cb);
}
void WifiManager::set_on_got_ip(WifiGotIpCallback cb) { _on_got_ip = std::move(cb); }
void WifiManager::set_on_scan_complete(WifiScanCompleteCallback cb) {
  _on_scan_complete = std::move(cb);
}
void WifiManager::set_on_ap_client_joined(WifiApClientJoinedCallback cb) {
  _on_ap_client_joined = std::move(cb);
}
void WifiManager::set_on_ap_client_left(WifiApClientLeftCallback cb) {
  _on_ap_client_left = std::move(cb);
}

// ---------------------------------------------------------------------------
// HAL event handlers
// ---------------------------------------------------------------------------

void WifiManager::_on_hal_sta_connected() {
  const WifiMode mode = _hal.get_mode();
  if (mode != WifiMode::Sta && mode != WifiMode::ApSta) {
    return; // A queued station event cannot revive work after radio suspension.
  }

  _sta_state = WifiStaState::Connected;
  // Note: do NOT reset _retry_attempt here. L2 association succeeding
  // and then dropping again is itself part of the retry sequence — the
  // backoff curve only resets on a full got-IP (see _on_hal_got_ip).
  // Arm DHCP watchdog: ESP-IDF doesn't tell us when DHCP fails, so we
  // give it a bounded window to deliver IP_EVENT_STA_GOT_IP.
  _hal.arm_dhcp_timeout(_dhcp_timeout_ms);
  if (_on_connected) {
    _on_connected();
  }
}

void WifiManager::_on_hal_sta_disconnected(int raw_reason) {
  const WifiMode mode = _hal.get_mode();
  if (mode != WifiMode::Sta && mode != WifiMode::ApSta) {
    return; // A queued station event cannot revive work after radio suspension.
  }

  // Ignore our own disconnect_sta() echo; no state change so an in-flight
  // next-candidate attempt survives.
  if (_swallow_next_disconnect) {
    _swallow_next_disconnect = false;
    return;
  }
  _hal.cancel_dhcp_timeout();
  _stop_mdns_auto_if_running();

  if (_disconnect_requested) {
    // The user-initiated path already emitted RequestedByUser via
    // disconnect(); swallow the driver echo.
    _disconnect_requested = false;
    _sta_state = WifiStaState::Disconnected;
    return;
  }

  const WifiDisconnectReason reason = map_disconnect_reason(raw_reason);

  // Sweep: single attempt per candidate (retry/backoff suppressed); a
  // failure advances to the next candidate.
  if (_auto_sweeping) {
    AG_LOGW(TAG, "sweep: '%s' failed (reason=%s)", _sta_config.ssid,
            wifi_disconnect_reason_to_string(reason));
    _last_sweep_reason = reason;
    _advance_candidate();
    return;
  }

  // Decide whether to retry. max_retry_count == 0 disables retries
  // entirely.
  const bool can_retry = is_retriable(reason) && _has_sta_config &&
                         _sta_config.max_retry_count > 0 &&
                         _retry_attempt < _sta_config.max_retry_count;
  if (can_retry) {
    const uint32_t delay = compute_backoff_ms(_sta_config.initial_retry_interval_ms,
                                              _sta_config.max_retry_interval_ms, _retry_attempt);
    _retry_attempt += 1;
    // Stay logically "Connecting" while waiting for the next attempt —
    // that keeps scan-while-retrying gated.
    _sta_state = WifiStaState::Connecting;
    _hal.arm_retry_timer(delay);
    return;
  }

  _sta_state = WifiStaState::Disconnected;
  _retry_attempt = 0;
  _emit_disconnected(reason);
}

void WifiManager::_on_hal_got_ip(uint32_t ip) {
  const WifiMode mode = _hal.get_mode();
  if (mode != WifiMode::Sta && mode != WifiMode::ApSta) {
    return; // A queued station event cannot revive work after radio suspension.
  }

  _hal.cancel_dhcp_timeout();
  // First got-IP ends the sweep. _sta_config already holds the winning
  // candidate, so a later link drop reconnects under normal retry/backoff.
  _clear_auto_state();
  _sta_state = WifiStaState::GotIp;
  _retry_attempt = 0;
  AG_LOGI(TAG, "connected to '%s' (got IP)", _sta_config.ssid);
  _start_mdns_auto_if_configured();
  if (_on_got_ip) {
    _on_got_ip(ip);
  }
}

void WifiManager::_on_hal_scan_complete(const WifiScanEntry *results, uint16_t count) {
  // An internal auto-scan is consumed here, not forwarded to the product.
  if (_auto_scan_pending) {
    _auto_scan_pending = false;
    _consume_auto_scan(results, count);
    return;
  }
  if (_on_scan_complete) {
    _on_scan_complete(results, count);
  }
}

void WifiManager::_on_hal_ap_client_joined(const uint8_t mac[6]) {
  if (_on_ap_client_joined) {
    _on_ap_client_joined(mac);
  }
}

void WifiManager::_on_hal_ap_client_left(const uint8_t mac[6]) {
  if (_on_ap_client_left) {
    _on_ap_client_left(mac);
  }
}

void WifiManager::_on_hal_dhcp_timeout() {
  if (_sta_state != WifiStaState::Connected) {
    return;
  }
  // Drop L2 first; ignore the resulting echo (else it double-advances the
  // sweep / arms a retry).
  _swallow_next_disconnect = true;
  _hal.disconnect_sta();
  _stop_mdns_auto_if_running();

  // In a sweep, a DHCP timeout is just another candidate failure.
  if (_auto_sweeping) {
    AG_LOGW(TAG, "sweep: '%s' failed (reason=dhcp_failed)", _sta_config.ssid);
    _last_sweep_reason = WifiDisconnectReason::dhcp_failed;
    _advance_candidate();
    return;
  }

  // Non-retriable disconnect; the caller decides (reprovision, fall back...).
  _sta_state = WifiStaState::Disconnected;
  _retry_attempt = 0;
  _emit_disconnected(WifiDisconnectReason::dhcp_failed);
}

void WifiManager::_on_hal_retry_due() {
  if (_disconnect_requested || _sta_state != WifiStaState::Connecting) {
    return;
  }
  _start_connect_attempt();
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

void WifiManager::_start_connect_attempt() {
  _sta_state = WifiStaState::Connecting;
  if (_auto_sweeping) {
    AG_LOGI(TAG, "sweep: attempting [%u/%u] '%s'", _candidate_index + 1, _candidate_count,
            _sta_config.ssid);
  } else {
    AG_LOGI(TAG, "connecting to '%s'", _sta_config.ssid);
  }
  const WifiStatus status = _hal.connect_sta(_sta_config.ssid, _sta_config.password);
  if (status != WifiStatus::Ok) {
    // HAL refused (bad args, mode race): a candidate failure during a sweep,
    // otherwise fail fast (backoff won't fix it).
    if (_auto_sweeping) {
      AG_LOGW(TAG, "sweep: '%s' connect refused by HAL", _sta_config.ssid);
      _last_sweep_reason = WifiDisconnectReason::unknown;
      _advance_candidate();
      return;
    }
    _sta_state = WifiStaState::Disconnected;
    _retry_attempt = 0;
    _emit_disconnected(WifiDisconnectReason::unknown);
  }
}

// ---------------------------------------------------------------------------
// Auto-connect (scan-best + single-attempt failover)
// ---------------------------------------------------------------------------

WifiStatus WifiManager::_start_auto_connect(const WifiStaConfig &config) {
  WifiCredential saved[WIFI_MAX_SAVED_NETWORKS];
  const uint8_t count = _creds.load_all(saved, WIFI_MAX_SAVED_NETWORKS);
  if (count == 0) {
    AG_LOGW(TAG, "auto-connect: no saved networks");
    return WifiStatus::NotFound;
  }

  // Latch the caller's retry/backoff; ssid/password are set per candidate.
  _sta_config = config;
  _has_sta_config = true;
  _retry_attempt = 0;
  _disconnect_requested = false;
  _auto_sweeping = false;
  _candidate_index = 0;
  _candidate_count = 0;

  if (count == 1) {
    // Nothing to fail over to: connect directly, normal retry/backoff (no sweep).
    AG_LOGI(TAG, "auto-connect: 1 saved network");
    std::strncpy(_sta_config.ssid, saved[0].ssid, sizeof(_sta_config.ssid) - 1);
    _sta_config.ssid[sizeof(_sta_config.ssid) - 1] = '\0';
    std::strncpy(_sta_config.password, saved[0].password, sizeof(_sta_config.password) - 1);
    _sta_config.password[sizeof(_sta_config.password) - 1] = '\0';
    _start_connect_attempt();
    return WifiStatus::Ok;
  }

  // More than one saved network: scan now, rank at scan-complete.
  AG_LOGI(TAG, "auto-connect: %u saved networks, scanning to rank", count);
  _auto_scan_pending = true;
  _sta_state = WifiStaState::Connecting;
  const WifiScanConfig scan_cfg; // show_hidden = false
  if (_hal.start_scan(scan_cfg) != WifiStatus::Ok) {
    _auto_scan_pending = false;
    _sta_state = WifiStaState::Disconnected;
    return WifiStatus::Failed;
  }
  return WifiStatus::Ok;
}

void WifiManager::_consume_auto_scan(const WifiScanEntry *results, uint16_t count) {
  // Re-load here (vs. a member snapshot): the window is brief, the read rare.
  WifiCredential saved[WIFI_MAX_SAVED_NETWORKS];
  const uint8_t saved_count = _creds.load_all(saved, WIFI_MAX_SAVED_NETWORKS);

  // Keep the strongest-RSSI scan entry per saved network (multi-AP networks
  // appear once per BSSID). Hidden APs report an empty SSID and never match.
  bool found[WIFI_MAX_SAVED_NETWORKS] = {};
  int8_t best_rssi[WIFI_MAX_SAVED_NETWORKS] = {};
  for (uint8_t i = 0; i < saved_count; ++i) {
    for (uint16_t j = 0; j < count; ++j) {
      if (results[j].ssid[0] == '\0') {
        continue;
      }
      if (std::strcmp(results[j].ssid, saved[i].ssid) != 0) {
        continue;
      }
      if (!found[i] || results[j].rssi > best_rssi[i]) {
        found[i] = true;
        best_rssi[i] = results[j].rssi;
      }
    }
  }

  // Sort matches by RSSI desc, tie-break by saved position (newest-first).
  uint8_t order[WIFI_MAX_SAVED_NETWORKS];
  uint8_t matched = 0;
  for (uint8_t i = 0; i < saved_count; ++i) {
    if (found[i]) {
      order[matched++] = i;
    }
  }
  for (uint8_t a = 0; a < matched; ++a) {
    for (uint8_t b = static_cast<uint8_t>(a + 1); b < matched; ++b) {
      const bool stronger = best_rssi[order[b]] > best_rssi[order[a]];
      const bool tie_newer = (best_rssi[order[b]] == best_rssi[order[a]]) && (order[b] < order[a]);
      if (stronger || tie_newer) {
        const uint8_t tmp = order[a];
        order[a] = order[b];
        order[b] = tmp;
      }
    }
  }

  if (matched == 0) {
    // No visible saved network; let the product re-scan, wait, or reprovision.
    AG_LOGW(TAG, "auto-connect: no saved network visible in scan");
    _sta_state = WifiStaState::Disconnected;
    _emit_disconnected(WifiDisconnectReason::no_ap_found);
    return;
  }

  _candidate_count = matched;
  AG_LOGI(TAG, "auto-connect: %u candidate(s) ranked by RSSI:", matched);
  for (uint8_t k = 0; k < matched; ++k) {
    _candidates[k] = saved[order[k]];
    AG_LOGI(TAG, "  [%u] '%s' rssi=%d", k, _candidates[k].ssid, best_rssi[order[k]]);
  }
  _candidate_index = 0;
  _auto_sweeping = true;
  _apply_candidate(0);
  _start_connect_attempt();
}

void WifiManager::_apply_candidate(uint8_t index) {
  std::strncpy(_sta_config.ssid, _candidates[index].ssid, sizeof(_sta_config.ssid) - 1);
  _sta_config.ssid[sizeof(_sta_config.ssid) - 1] = '\0';
  std::strncpy(_sta_config.password, _candidates[index].password, sizeof(_sta_config.password) - 1);
  _sta_config.password[sizeof(_sta_config.password) - 1] = '\0';
}

void WifiManager::_advance_candidate() {
  _candidate_index += 1;
  if (_candidate_index < _candidate_count) {
    _apply_candidate(_candidate_index);
    _start_connect_attempt();
    return;
  }
  // Candidate list exhausted: emit disconnected with the last reason.
  const WifiDisconnectReason reason = _last_sweep_reason;
  AG_LOGW(TAG, "auto-connect: all %u candidate(s) exhausted (last reason=%s)", _candidate_count,
          wifi_disconnect_reason_to_string(reason));
  _clear_auto_state();
  _sta_state = WifiStaState::Disconnected;
  _retry_attempt = 0;
  _emit_disconnected(reason);
}

void WifiManager::_clear_auto_state() {
  _auto_scan_pending = false;
  _auto_sweeping = false;
  _candidate_index = 0;
  _candidate_count = 0;
}

bool WifiManager::_is_mdns_profile_valid(const WifiMdnsProfile &profile) const {
  const WifiMdnsConfig &config = profile.config;
  if (profile.lifecycle != WifiMdnsLifecycle::StaIpAuto &&
      profile.lifecycle != WifiMdnsLifecycle::Manual) {
    return false;
  }
  if (config.hostname == nullptr || config.hostname[0] == '\0' ||
      std::strlen(config.hostname) > WIFI_MDNS_MAX_HOSTNAME_LENGTH) {
    return false;
  }
  if (config.service_count > MAX_MDNS_SERVICES ||
      (config.service_count > 0 && config.services == nullptr)) {
    return false;
  }

  for (uint8_t i = 0; i < config.service_count; ++i) {
    const WifiMdnsServiceRecord &service = config.services[i];
    if (service.service_type == nullptr) {
      return false;
    }
    const char *dot = std::strchr(service.service_type, '.');
    if (dot == nullptr || dot == service.service_type || dot[1] == '\0' ||
        static_cast<size_t>(dot - service.service_type) > WIFI_MDNS_MAX_SERVICE_LENGTH ||
        std::strlen(dot + 1) > WIFI_MDNS_MAX_PROTOCOL_LENGTH) {
      return false;
    }
    if (service.txt_count > 0 && (service.txt_keys == nullptr || service.txt_values == nullptr)) {
      return false;
    }
    for (uint8_t txt = 0; txt < service.txt_count; ++txt) {
      if (service.txt_keys[txt] == nullptr || service.txt_values[txt] == nullptr) {
        return false;
      }
    }
  }
  return true;
}

bool WifiManager::_is_mdns_auto() const {
  return _has_mdns_profile && _mdns_profile.lifecycle == WifiMdnsLifecycle::StaIpAuto;
}

void WifiManager::_stop_mdns_auto_if_running() {
  if (_is_mdns_auto()) {
    const WifiStatus status = stop_mdns();
    if (status != WifiStatus::Ok) {
      AG_LOGW(TAG, "automatic mDNS stop failed (%u)", static_cast<unsigned>(status));
    }
  }
}

void WifiManager::_start_mdns_auto_if_configured() {
  if (!_is_mdns_auto()) {
    return;
  }
  const WifiStatus status = start_mdns();
  if (status != WifiStatus::Ok) {
    AG_LOGW(TAG, "automatic mDNS start failed (%u)", static_cast<unsigned>(status));
  }
}

void WifiManager::_emit_disconnected(WifiDisconnectReason reason) {
  if (_on_disconnected) {
    _on_disconnected(reason);
  }
}
