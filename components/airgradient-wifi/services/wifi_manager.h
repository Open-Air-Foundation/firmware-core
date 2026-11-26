/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_WIFI_MANAGER_H
#define AG_WIFI_MANAGER_H

#include "../types/wifi_types.h"
#include "wifi_credential_store.h"

class WifiHal;
class ConfigStore;

/// High-level Wi-Fi manager. This is the product-facing API.
///
/// Owns: mode state machine, connection retry with exponential backoff,
/// mDNS profile lifecycle, disconnect reason normalisation, DHCP
/// acquisition timeout policy.
///
/// Pure C++ logic — host-testable when constructed with a mock WifiHal.
///
/// Callbacks fire in the ESP-IDF system event loop task context.
/// Callers must not block inside callbacks or call back into WifiManager,
/// except the credential APIs (add_network / remove_network /
/// list_networks / has_saved_networks / clear_networks / select_saved_network), which only touch
/// the ConfigStore and are safe to call from a callback.
///
/// ISR-safe: no
/// Thread-safe: no
/// Blocking: no (all outcomes delivered via callbacks)
class WifiManager {
public:
  static constexpr uint16_t MAX_MDNS_SERVICES = 4;

  /// The store backs the saved-network credential API and auto-connect.
  WifiManager(WifiHal &hal, ConfigStore &store);
  ~WifiManager();

  WifiManager(const WifiManager &) = delete;
  WifiManager &operator=(const WifiManager &) = delete;

  // -- Configuration (call before connect) --

  /// Compatibility wrapper for a StaIpAuto profile.
  WifiStatus set_mdns_config(const WifiMdnsConfig &config);

  /// Validate and retain an mDNS profile. The hostname and service records
  /// are copied; service-type strings, TXT pointer arrays, and TXT strings
  /// remain caller-owned. Replacing a running profile first stops it.
  /// Validation or stop failure leaves the previous profile retained.
  WifiStatus set_mdns_profile(const WifiMdnsProfile &profile);

  /// Start the retained mDNS profile. Idempotent while already running.
  /// Returns InvalidState if no profile is retained.
  WifiStatus start_mdns();

  /// Stop mDNS without forgetting the retained profile. Idempotent while
  /// stopped. A HAL failure leaves the profile marked as running for retry.
  WifiStatus stop_mdns();

  /// Stop mDNS, then forget the retained profile. A stop failure leaves the
  /// running profile intact.
  WifiStatus clear_mdns_profile();

  /// Configure a static IP for STA connections. Persists until
  /// clear_static_ip() is called.
  WifiStatus set_static_ip(const WifiStaticIpConfig &config);

  /// Clear the static IP configuration. Revert to DHCP for
  /// subsequent connections.
  WifiStatus clear_static_ip();

  /// Set the Wi-Fi power save mode. Only meaningful in STA mode.
  /// ESP-IDF default: WifiPowerSave::MinModem.
  WifiStatus set_power_save(WifiPowerSave mode);

  /// Override the DHCP acquisition timeout. Defaults to
  /// CONFIG_AG_WIFI_DHCP_TIMEOUT_MS (or WIFI_DEFAULT_DHCP_TIMEOUT_MS on
  /// host builds).
  void set_dhcp_timeout_ms(uint32_t timeout_ms);

  // -- Mode Control --

  /// Set the Wi-Fi operating mode. All transitions are legal.
  /// Idempotent: setting the current mode returns Ok.
  /// Setting Off tears down STA, AP, and mDNS. Manual mDNS profiles otherwise
  /// do not follow STA connection state.
  WifiStatus set_mode(WifiMode mode);

  /// Return the current Wi-Fi operating mode.
  WifiMode get_mode() const;

  /// Idempotent driver allocation and release, separate from radio mode.
  WifiStatus initialize();
  WifiStatus deinitialize();

  // -- STA Operations --

  /// Start a STA connection. Non-blocking. Requires Sta or ApSta mode
  /// (returns InvalidState otherwise).
  ///
  /// max_retry_count = 0 disables auto-retry.
  ///
  /// Explicit config.ssid => one-shot transient connect; never writes the
  /// store. Empty config.ssid => auto-connect from the saved networks:
  ///   - 0 saved        => NotFound (no driver call).
  ///   - 1 saved        => connect it directly with the caller's retry /
  ///                       backoff (no scan, no single-attempt sweep).
  ///   - >1 saved       => scan, intersect with saved networks, rank by
  ///                       RSSI, and sweep candidates with a single attempt
  ///                       each, failing over on failure.
  ///
  /// AlreadyInProgress when a connect or auto cycle is already running.
  ///
  /// Outcome delivered via on_connected / on_got_ip / on_disconnected.
  WifiStatus connect(const WifiStaConfig &config);

  /// Connect directly to one saved SSID using its stored password, without scanning.
  WifiStatus connect_saved_network(const char *ssid, const WifiStaConfig &config = {});

  /// Disconnect from the current AP and cancel any pending retry or auto
  /// cycle.
  WifiStatus disconnect();

  // -- Scan --

  /// Trigger an async Wi-Fi scan. Results delivered via
  /// on_scan_complete callback. Requires Sta or ApSta mode AND the STA
  /// must be disconnected (no scan-while-connected at v1).
  WifiStatus start_scan(const WifiScanConfig &config = {});

  /// Select the strongest visible saved SSID; ties prefer the newest saved entry.
  /// Clears ssid when no match exists. Safe to call from the scan callback.
  bool select_saved_network(const WifiScanEntry *results, uint16_t count, char (&ssid)[33]) const;

  // -- AP Operations --

  /// Start the soft-AP. Requires Ap or ApSta mode (returns
  /// InvalidState otherwise). SSID is required (must not be empty).
  WifiStatus start_ap(const WifiApConfig &config);

  // -- Credential Storage --
  //
  // Up to WIFI_MAX_SAVED_NETWORKS networks, newest-first, persisted via the
  // injected ConfigStore. Safe to call from a callback (e.g. got-IP): these
  // only touch the store and never re-enter the connection state machine.
  // Mutations perform a bounded blocking flash commit.

  /// Add (or refresh) a saved network and mark it newest. See
  /// WifiCredentialStore::add for validation and return codes.
  WifiStatus add_network(const char *ssid, const char *password);

  /// Remove a saved network by SSID. See WifiCredentialStore::remove.
  WifiStatus remove_network(const char *ssid);

  /// Copy up to max saved SSIDs (newest-first) into out. Returns the saved
  /// entry count.
  uint8_t list_networks(char (*out)[33], uint8_t max) const;

  /// True when at least one network is saved.
  bool has_saved_networks() const;

  /// Erase all saved networks. For factory-reset and reprovisioning.
  WifiStatus clear_networks();

  // -- Status --

  /// Return a snapshot of the current Wi-Fi state (mode, STA state,
  /// IP, RSSI, BSSID, channel, AP client count).
  WifiStatusSnapshot status_snapshot() const;

  // -- Product-Facing Callbacks --

  /// Invoked when STA associates with an AP (L2 link up).
  void set_on_connected(WifiConnectedCallback cb);

  /// Invoked when STA disconnects after retry exhaustion or explicit
  /// disconnect(). Reason is normalised from raw ESP-IDF codes.
  void set_on_disconnected(WifiDisconnectedCallback cb);

  /// Invoked when STA acquires an IP address (DHCP or static).
  /// A StaIpAuto mDNS profile is started before this callback fires.
  void set_on_got_ip(WifiGotIpCallback cb);

  /// Invoked when a scan completes. Buffer valid only during callback.
  void set_on_scan_complete(WifiScanCompleteCallback cb);

  /// Invoked when a client joins the soft-AP.
  void set_on_ap_client_joined(WifiApClientJoinedCallback cb);

  /// Invoked when a client leaves the soft-AP.
  void set_on_ap_client_left(WifiApClientLeftCallback cb);

  // -- Visible for tests --

  /// Map a raw ESP-IDF wifi_err_reason_t (int) to a normalised
  /// WifiDisconnectReason. Pure function; static so tests can call it
  /// without instantiating a manager.
  static WifiDisconnectReason map_disconnect_reason(int raw_reason);

  /// True if the given normalised reason is eligible for auto-retry.
  static bool is_retriable(WifiDisconnectReason reason);

  /// Compute the next retry backoff (capped exponential, doubling each
  /// attempt). attempt is 0-based.
  static uint32_t compute_backoff_ms(uint32_t initial_ms, uint32_t cap_ms, uint32_t attempt);

private:
  // -- HAL event handlers (registered with hal during construction) --
  void _on_hal_sta_connected();
  void _on_hal_sta_disconnected(int raw_reason);
  void _on_hal_got_ip(uint32_t ip);
  void _on_hal_scan_complete(const WifiScanEntry *results, uint16_t count);
  void _on_hal_ap_client_joined(const uint8_t mac[6]);
  void _on_hal_ap_client_left(const uint8_t mac[6]);
  void _on_hal_dhcp_timeout();
  void _on_hal_retry_due();

  // -- Internal helpers --
  void _start_connect_attempt();
  bool _is_mdns_profile_valid(const WifiMdnsProfile &profile) const;
  bool _is_mdns_auto() const;
  void _stop_mdns_auto_if_running();
  void _start_mdns_auto_if_configured();
  void _emit_disconnected(WifiDisconnectReason reason);

  // Auto-connect (empty SSID) helpers.
  WifiStatus _start_auto_connect(const WifiStaConfig &config);
  void _consume_auto_scan(const WifiScanEntry *results, uint16_t count);
  void _apply_candidate(uint8_t index);
  void _advance_candidate();
  void _clear_auto_state();

  WifiHal &_hal;
  WifiCredentialStore _creds;

  // Configuration / latched state
  WifiStaConfig _sta_config = {};
  bool _has_sta_config = false;
  WifiMdnsProfile _mdns_profile = {};
  WifiMdnsServiceRecord _mdns_services[MAX_MDNS_SERVICES] = {};
  char _mdns_hostname[WIFI_MDNS_MAX_HOSTNAME_LENGTH + 1] = {};
  bool _has_mdns_profile = false;
  bool _mdns_running = false;
  uint32_t _dhcp_timeout_ms = WIFI_DEFAULT_DHCP_TIMEOUT_MS;

  // Live state
  WifiStaState _sta_state = WifiStaState::Disconnected;
  uint32_t _retry_attempt = 0; // 0-based count of retries already used
  bool _disconnect_requested = false;
  // One-shot: ignore the echo from a manager-initiated disconnect_sta()
  // (DHCP teardown) so it can't double-advance a sweep or arm a retry.
  bool _swallow_next_disconnect = false;

  // Auto-connect (scan-best + single-attempt failover) state.
  bool _auto_scan_pending = false; // internal scan in flight
  bool _auto_sweeping = false;     // iterating candidates before first got-IP
  WifiCredential _candidates[WIFI_MAX_SAVED_NETWORKS] = {}; // ranked, best-first
  uint8_t _candidate_index = 0;
  uint8_t _candidate_count = 0;
  WifiDisconnectReason _last_sweep_reason = WifiDisconnectReason::unknown;

  // Product-facing callbacks
  WifiConnectedCallback _on_connected;
  WifiDisconnectedCallback _on_disconnected;
  WifiGotIpCallback _on_got_ip;
  WifiScanCompleteCallback _on_scan_complete;
  WifiApClientJoinedCallback _on_ap_client_joined;
  WifiApClientLeftCallback _on_ap_client_left;
};

#endif // AG_WIFI_MANAGER_H
