/**
 * AirGradient Go — WifiService
 *
 * Product service that owns the Stationary Wi-Fi lifecycle: saved-
 * credential connect, factory-default fallback, interactive provisioning,
 * post-online disconnect state, and credential clearing.
 *
 * The orchestrator owns mode policy and disconnect routing; WifiService
 * owns mechanics, deadlines, and callback adapters.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_WIFI_H
#define GO_WIFI_H

#include <atomic>
#include <cstdint>

#include "go_events.h"
#include "hal/ble_types.h"
#include "rtos.h"
#include "types/provisioning_types.h"
#include "types/wifi_types.h"

class AgBleServer;
class HttpServer;
class LocalServer;
class ProvisioningManager;
class WifiManager;

class WifiService {
public:
  struct Deps {
    WifiManager &wifi;
    AgBleServer &ble;
    HttpServer &http;
    LocalServer &local_server;
  };

  struct Config {
    // Captive-portal AP (Wi-Fi provisioning transport)
    const char *ap_ssid = nullptr; // required; usually "airgradient-<12-hex>"
    const char *ap_password = "cleanair";
    uint8_t ap_channel = 1;

    // BLE provisioning transport
    const char *ble_device_name = "AirGradient";
    const char *ble_model_name = nullptr;
    const char *ble_serial_number = nullptr;
    const char *ble_firmware_version = nullptr;
    const char *ble_manufacturer_data = nullptr;
    uint8_t ble_auth_flags = AgBleAuth::SC; // Just Works SC, no BOND, no MITM

    // Saved-credentials and factory-default deadlines
    uint32_t initial_connect_window_ms = 30000;
    const char *fallback_ssid = "airgradient";
    const char *fallback_password = "cleanair";
    uint32_t fallback_connect_window_ms = 15000;

    // Delay between runtime reconnect cycles; spaces out instant-fail
    // reasons (auth / assoc / dhcp) so they can't tight-loop the radio.
    uint32_t reconnect_delay_ms = 5000;

    // Local HTTP API and mDNS identity. All strings must outlive this service.
    const char *serial_number = nullptr;
    const char *firmware_version = nullptr;
    const char *model = nullptr;
    const char *hostname = nullptr;
    uint16_t http_port = 80;
  };

  WifiService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg);
  ~WifiService();

  WifiService(const WifiService &) = delete;
  WifiService &operator=(const WifiService &) = delete;

  // --- Credential queries ---

  bool has_saved_networks() const;

  // --- Connection attempts (STA only; no SoftAP, no provisioning) ---

  /// Auto-connect from the saved networks (best-RSSI scan + single-attempt
  /// failover). Arms the initial-connect deadline. Applies @p static_ip when
  /// non-null, otherwise clears any previously set static IP. Posts synthetic
  /// WifiDisconnected when the WifiManager reports NotFound up front.
  void connect_with_saved_credentials(const WifiStaticIpConfig *static_ip = nullptr);

  /// Arm a runtime reconnect, issued from tick() after reconnect_delay_ms.
  /// Reconnects saved networks when present, otherwise retries the transient
  /// factory-default network. Keeps has_been_online() latched (stays
  /// "runtime") and arms no connect window.
  void schedule_reconnect(const WifiStaticIpConfig *static_ip = nullptr);

  /// Connect to the factory-default airgradient/cleanair AP. Explicit SSID,
  /// so the connect is transient and never written to the saved-networks
  /// store. Single-shot per Stationary entry, bounded by the fallback window.
  void try_default_fallback_credentials();

  // --- Provisioning (full impl lands in CP2.3) ---

  /// Allocates provisioning infrastructure on the first call.
  void start_provisioning(ProvisioningTransport transport = ProvisioningTransport::BleOnly);
  void switch_provisioning_transport();
  void stop_provisioning(bool stop_http_server = true);

  // --- Local endpoint ---

  /// Register local API routes and ensure the configured listener is active.
  bool ensure_local_http();

  /// Install and explicitly start the local StaIpAuto mDNS profile.
  bool ensure_local_mdns();

  /// Stop/clear mDNS, stop the listener, then unregister local API routes.
  void stop_local_endpoint();

  // --- Lifecycle ---

  /// Drop STA, set Wi-Fi mode Off, detach callbacks, reset latches.
  /// Called by the orchestrator on Stationary teardown.
  void shutdown();

  /// Erase all saved networks and reset online latches.
  void clear_credentials();

  // --- State queries (all lock-free) ---

  bool is_online() const;
  bool is_connecting() const;
  bool is_provisioning() const;
  ProvisioningTransport current_transport() const;
  uint32_t ip() const;
  int rssi() const;
  WifiDisconnectReason last_disconnect_reason() const;

  /// Latches true on the first IP for the current Stationary entry;
  /// resets on shutdown / clear_credentials / fresh connect attempts.
  bool has_been_online() const;

  // --- Timer integration (driven by orchestrator's event loop) ---

  /// Absolute ms of the next armed deadline, or 0 when no deadline.
  uint32_t next_deadline_ms() const;

  /// Retry retained provisioning success delivery, consume pending deadline
  /// clears, and fire synthetic disconnects on expiry. Idempotent.
  void tick(uint32_t now_ms);

#ifdef TEST_HOST
  friend class WifiServiceTestAccess;
#endif

private:
  // Bind on_got_ip / on_disconnected to WifiManager. Cleared by passing
  // null callbacks. Owned by WifiService at all times outside provisioning.
  void _install_wifi_callbacks();
  void _detach_wifi_callbacks();

  // WifiManager callback adapters (run in ESP-IDF event-loop task).
  void _on_got_ip(uint32_t ip);
  void _on_disconnected(WifiDisconnectReason reason);

  // ProvisioningManager event adapter (run in NimBLE / portal task).
  // Forwards to the orchestrator queue, except the intermediate Stopped
  // event during a transport switch.
  void _on_provisioning_event(const struct ProvisioningEventInfo &info);

  // Shared saved-credentials connect. Both flags true = fresh bring-up;
  // both false = runtime reconnect (keeps has_been_online(), no window).
  void _connect_saved_internal(const WifiStaticIpConfig *static_ip, bool reset_online_latches,
                               bool arm_window);
  void _connect_fallback_internal(bool reset_online_latches, bool arm_window);

  void _reset_deadline();
  void _arm_deadline(uint32_t window_ms);
  void _reset_online_latches();
  void _post_wifi_disconnected(WifiDisconnectReason reason);
  void _post_provisioning_event(const struct ProvisioningEventInfo &info);
  void _ensure_provisioning_manager();
  bool _start_provisioning_internal(ProvisioningTransport transport);

  RtosQueueHandle _event_queue;
  WifiManager &_wifi;
  AgBleServer &_ble;
  HttpServer &_http;
  LocalServer &_local_server;
  Config _cfg;

  static constexpr uint8_t LOCAL_MDNS_TXT_COUNT = 5;
  const char *_mdns_txt_keys[LOCAL_MDNS_TXT_COUNT] = {"vendor", "model", "serialno", "fw_ver",
                                                      "api"};
  const char *_mdns_txt_values[LOCAL_MDNS_TXT_COUNT] = {};
  bool _local_http_active = false;
  bool _local_mdns_profile_installed = false;

  // Raw pointer keeps ProvisioningManager forward-declarable so test stubs
  // need not pull in the provisioning header. go_wifi.cpp owns its lazy
  // new/delete lifecycle.
  ProvisioningManager *_prov = nullptr;

  // Wi-Fi state mirrors — written by WifiManager callbacks, read by
  // the orchestrator task. Atomic to allow lock-free reads.
  std::atomic<bool> _online{false};
  std::atomic<bool> _has_been_online{false};
  std::atomic<uint32_t> _ip{0};
  std::atomic<int> _rssi{WIFI_RSSI_INVALID};
  std::atomic<uint8_t> _last_disconnect_reason{static_cast<uint8_t>(WifiDisconnectReason::unknown)};

  // Initial-connect deadline. Single-writer: only mutated by shutdown(),
  // connect_with_saved_credentials(), try_default_fallback_credentials(),
  // and tick(). The on_got_ip adapter sets _clear_deadline_pending so
  // the next tick() can clear without racing.
  uint32_t _initial_connect_deadline_ms = 0;
  std::atomic<bool> _clear_deadline_pending{false};

  // Runtime reconnect timer (single-writer, orchestrator task): armed by
  // schedule_reconnect(), fired/cleared by tick(). Carries the static IP to
  // reapply when reconnecting saved networks.
  uint32_t _reconnect_at_ms = 0;
  WifiStaticIpConfig _reconnect_static_ip{};
  bool _reconnect_has_static_ip = false;

  // Provisioning state
  bool _provisioning_active = false;
  ProvisioningTransport _transport = ProvisioningTransport::BleOnly;
  // Connected is the provisioning success transition and must survive central
  // queue backpressure. The callback publishes the built event to tick().
  Event _pending_provisioning_connected_event{};
  std::atomic<bool> _provisioning_connected_event_pending{false};
  // True only inside switch_provisioning_transport(); guarantees no
  // external suspension point can observe the latch held.
  bool _switching_transport = false;
};

#endif // GO_WIFI_H
