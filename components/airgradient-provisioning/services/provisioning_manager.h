/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_MANAGER_H
#define AG_PROVISIONING_MANAGER_H

#include <cstdint>
#include <memory>

#include "../types/provisioning_types.h"
#include "rtos.h"

class WifiManager;
class AgBleServer;
class HttpServer;
class WifiPortalTransport;
class BleTransport;
class CaptiveDnsResponder;
class ProvisioningTimer;

/// Wi-Fi provisioning manager.
///
/// Owns the provisioning state machine and coordinates two transports
/// (Wi-Fi captive portal and BLE GATT) to collect Wi-Fi credentials
/// from the user.
///
/// Product code creates WifiManager, AgBleServer, and HttpServer, then
/// passes them to start(). The provisioning manager borrows these
/// objects for the duration of the provisioning session. Product code
/// resumes full ownership after stop() returns.
///
/// Lifecycle events are delivered via a single callback set with
/// set_on_event(). Callbacks fire from various task contexts (WiFi
/// event loop, HTTP server, NimBLE). Callers must not block inside
/// callbacks or call back into ProvisioningManager.
///
/// ISR-safe: no
/// Thread-safe: yes (internal mutex)
/// Blocking: no (all outcomes delivered via callbacks)
class ProvisioningManager {
public:
  ProvisioningManager();
  ~ProvisioningManager();

  ProvisioningManager(const ProvisioningManager &) = delete;
  ProvisioningManager &operator=(const ProvisioningManager &) = delete;

  /// Register the event callback. Must be called before start().
  void set_on_event(ProvisioningEventCallback cb);

  /// Start provisioning. Non-blocking.
  ///
  /// Switches Wi-Fi to ApSta mode, starts the AP, registers HTTP
  /// routes, starts the captive DNS responder and HTTP server on
  /// `config.http_port`, then advertises a manual mDNS HTTP profile.
  /// It also initialises the BLE server, creates GATT services, and
  /// begins BLE advertising.
  ///
  /// Contract: when the Wi-Fi transport is selected, HttpServer must have
  /// no registered routes. Its listener may be retained on config.http_port
  /// if the backend supports live route registration and idempotent start()
  /// (as IdfHttpServer does); stop it before changing ports. When the BLE
  /// transport is selected, AgBleServer must NOT be initialised yet.
  /// ProvisioningManager owns the selected transports' HTTP route table,
  /// mDNS profile, and BLE server lifecycle for the session. It attempts to replace
  /// any previously retained mDNS profile; the caller must reinstall its
  /// profile after stop() when replacement succeeds.
  ///
  /// @param wifi   Wi-Fi manager — borrowed for AP, scan, STA connect
  /// @param ble    BLE server — borrowed for GATT provisioning service
  /// @param http   HTTP server — empty route table when Wi-Fi transport is selected
  /// @param config provisioning configuration
  /// @return false if already running or arguments are invalid
  bool start(WifiManager &wifi, AgBleServer &ble, HttpServer &http,
             const ProvisioningConfig &config);

  /// Register provisioning on a borrowed BLE server before its owner advertises.
  /// No HttpServer, BLE initialization/security changes, or Wi-Fi mode changes.
  /// Scan/credential writes go to the attached request hook for product admission.
  /// The owner supplies Device Information. stop() detaches without server deinit.
  ///
  /// Requires config.transport == BleAttached and a non-null ble.device_name.
  /// Returns false if already running or arguments are invalid.
  /// With own_wifi_callbacks=false, the product retains Wi-Fi callbacks and
  /// forwards only provisioning results through handle_wifi_* / handle_scan_results.
  bool start_attached(WifiManager &wifi, AgBleServer &ble, const ProvisioningConfig &config,
                      bool own_wifi_callbacks = true);

  void handle_wifi_connected(uint32_t ip);
  void handle_wifi_disconnected();
  void handle_scan_results(const struct WifiScanEntry *entries, uint16_t count);

  /// Register the attached request hook. Invoked on the NimBLE task for each
  /// scan/credentials write, OUTSIDE the mutex (it does queue_send() and the
  /// caller re-enters under the lock).
  void set_attached_request_hook(AttachedRequestCallback hook);

  /// Driver entrypoint (attached): run a scan / start a single STA connect.
  /// Called by the product after the radio is ready. False if not
  /// WaitingForCredentials.
  bool request_scan();
  bool submit_credentials(const ProvisioningData &data);

  /// Reopen an attached session after success (Connected ->
  /// WaitingForCredentials) so a later write is accepted. No-op otherwise.
  void reset_to_listening();

  /// Stop provisioning. Stops and clears provisioning mDNS, wipes all
  /// HTTP routes, reverts Wi-Fi to STA mode (drops the AP), tears down
  /// the DNS responder, and deinits the BLE server. Fires the Stopped
  /// event.
  ///
  /// When called from the Connected state, blocks for a short hold
  /// (~1.5 s) before teardown so the captive-portal browser can see
  /// the success state on /api/status before the AP is dropped.
  ///
  /// @param stop_http_server when true (default), also stops the HTTP
  ///        server. Pass false to keep the server running so the
  ///        product can register its own routes after stop() returns
  ///        without a bind/unbind cycle.
  void stop(bool stop_http_server = true);

  /// Current provisioning state.
  ProvisioningState state() const;

  /// Send an application-level status code over BLE. Valid between
  /// the Connected event and stop(). No-op when provisioning is not
  /// running.
  void send_ble_status(uint8_t status_code);

private:
#ifdef TEST_HOST
  friend class ProvisioningTestAccess;
#endif

  // -- Event handlers (wired to WifiManager / BLE callbacks in start()) --
  void _on_sta_connected(uint32_t ip);
  void _on_sta_disconnected();
  void _on_ap_client_joined(const uint8_t *mac = nullptr);
  void _on_ap_client_left(const uint8_t *mac = nullptr);
  void _on_ble_client_connected();
  void _on_ble_client_disconnected();
  void _on_scan_results(const WifiScanEntry *entries, uint16_t count);

  void _emit(const ProvisioningEventInfo &info);
  // Invoke the attached hook outside the lock (mirrors _emit's discipline).
  void _dispatch_attached_request(const AttachedRequest &req);
  void _set_state_locked(ProvisioningState s);
  bool _accept_credentials(const ProvisioningData &data);
  bool _trigger_scan();
  void _on_timeout();
  void _maybe_arm_timeout_locked();
  void _pause_timeout_locked();
  void _resume_timeout_locked();
  uint32_t _total_client_count_locked() const;

  // Roll back partial state established by start() before a failure
  // exit. Idempotent and safe to call after any subset of: portal
  // routes registered, BLE transport set up, Wi-Fi callbacks wired,
  // mode changed, AP started, DNS started.
  void _rollback_start_locked(WifiManager &wifi, HttpServer &http);
  void _teardown_mdns_locked();

  // First-client-wins teardown helpers (Both mode only). Zero the
  // torn-down side's counter and re-evaluate the inactivity timer.
  void _teardown_ble_transport_locked();
  void _teardown_wifi_transport_locked();

  mutable RtosMutex _mutex;
  ProvisioningState _state = ProvisioningState::Idle;
  ProvisioningEventCallback _on_event;
  // Attached-mode request hook (set via set_attached_request_hook()).
  AttachedRequestCallback _attached_hook;

  // Borrowed dependencies — set in start(), cleared in stop().
  WifiManager *_wifi = nullptr;
  HttpServer *_http = nullptr;

  ProvisioningConfig _config = {};
  ProvisioningData _pending_data = {};

  uint32_t _ap_client_count = 0;
  uint32_t _ble_client_count = 0;
  bool _timeout_armed = false;
  // Per-side bring-up flags. Short-circuit teardown on repeat client
  // commits (e.g. second BLE central after the first disconnected).
  bool _ble_active = false;
  bool _wifi_active = false;
  bool _mdns_profile_installed = false;
  bool _owns_wifi_callbacks = true;

  std::unique_ptr<WifiPortalTransport> _portal;
  std::unique_ptr<BleTransport> _ble_transport;
  std::unique_ptr<CaptiveDnsResponder> _dns;
  std::unique_ptr<ProvisioningTimer> _timer;
};

#endif // AG_PROVISIONING_MANAGER_H
