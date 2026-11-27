/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_BLE_TRANSPORT_H
#define AG_PROVISIONING_BLE_TRANSPORT_H

#include <cstdint>

#include "../types/provisioning_types.h"
#include "provisioning_timer.h"
#include "types/wifi_types.h"

class AgBleServer;
class AgBleCharacteristic;

// Internal BLE transport for ProvisioningManager.
//
// Owns: GATT service/characteristic setup on the borrowed AgBleServer,
// BLE credential parsing, paginated scan notifications, status
// notifications. Does NOT own the AgBleServer, the provisioning state
// machine, or the scan filter logic (uses ScanFilter as a utility).
//
// The manager wires it up:
//   * provides callbacks for credential submission and scan triggering
//   * provides callbacks for BLE client connect/disconnect events
//   * pushes scan results into update_scan_results() when WifiManager
//     reports them
//
// GATT layout:
//   - AirGradient Provisioning Service (acbcfea8-...)
//     - Credentials/Status characteristic (703fa252-...) WRITE_ENC, READ_ENC, NOTIFY
//     - Wi-Fi Scan characteristic (467a080f-...) WRITE_ENC, NOTIFY
//   - Device Information Service (180A)
//     - Model Number (2A24), Serial Number (2A25), Firmware Rev (2A26),
//       Manufacturer Name (2A29) — all READ, READ_ENC
class BleTransport {
public:
  using CredentialsCallback = std::function<bool(const ProvisioningData &)>;
  using ScanRequestCallback = std::function<bool()>;
  using ClientCallback = std::function<void()>;

  BleTransport();
  ~BleTransport();

  BleTransport(const BleTransport &) = delete;
  BleTransport &operator=(const BleTransport &) = delete;

  // Wire callbacks invoked by characteristic write handlers.
  void set_on_credentials(CredentialsCallback cb) { _on_credentials = std::move(cb); }
  void set_on_scan_request(ScanRequestCallback cb) { _on_scan_request = std::move(cb); }
  void set_on_client_connected(ClientCallback cb) { _on_client_connected = std::move(cb); }
  void set_on_client_disconnected(ClientCallback cb) { _on_client_disconnected = std::move(cb); }

  // Initialise the BLE server, create GATT services, set security,
  // configure advertising, and start advertising.
  //
  // Calls ble.init() — the server must NOT be initialised yet.
  // Returns false on failure.
  bool setup(AgBleServer &ble, const ProvisioningBleConfig &config);

  // Attached mode: register provisioning on an initialized, borrowed server.
  // No init/set_security, no connect/disconnect callbacks, no advertising —
  // the owner supplies Device Information and deinits later. Teardown via detach(), never
  // deinit. Returns false on registration failure.
  bool setup_on_server(AgBleServer &ble, const ProvisioningBleConfig &config);

  // Stop advertising, disconnect clients, deinit BLE server.
  // Safe to call when not set up.
  void teardown();

  // Attached-mode teardown: clear char write callbacks + cancel the page
  // timer and drop the borrowed server ref WITHOUT deinit. Safe when detached.
  void detach();

  // Update scan results from WifiManager. Triggers paginated BLE
  // notifications via internal timer. Entries are filtered by ScanFilter
  // before caching.
  void update_scan_results(const WifiScanEntry *entries, uint16_t count);

  // Send a status notification on the Credentials/Status characteristic.
  // {"status":<code>}
  void send_status(uint8_t status_code);

  // Send the next page of scan results. Called by the pagination timer.
  void send_next_scan_page();

  // True if pagination has more pages to send.
  bool has_more_scan_pages() const;

  // Access the pagination timer for test hooks.
  ProvisioningTimer &pagination_timer() { return _page_timer; }

private:
  // Register provisioning (plus DIS when owned) on the assigned _ble. Shared by
  // setup() / setup_on_server(); cleanup is the caller's (teardown/detach).
  bool _register_gatt(const ProvisioningBleConfig &config);

  void _on_credentials_write(const uint8_t *data, size_t len);
  void _on_scan_write(const uint8_t *data, size_t len);
  void _on_connect(uint16_t conn_handle);
  void _on_disconnect(uint16_t conn_handle, int reason);

  CredentialsCallback _on_credentials;
  ScanRequestCallback _on_scan_request;
  ClientCallback _on_client_connected;
  ClientCallback _on_client_disconnected;

  AgBleServer *_ble = nullptr;
  // setup() owns the server (teardown deinits); setup_on_server() borrows it.
  bool _owns_server = false;
  AgBleCharacteristic *_cred_char = nullptr;
  AgBleCharacteristic *_scan_char = nullptr;

  // Scan pagination state.
  static constexpr size_t MAX_CACHED_SCAN = 30;
  WifiScanEntry _scan_cache[MAX_CACHED_SCAN] = {};
  size_t _scan_cache_size = 0;
  size_t _current_page = 0;
  size_t _total_pages = 0;

  ProvisioningTimer _page_timer;

  // Pagination delay between pages in milliseconds.
  static constexpr uint32_t PAGE_DELAY_MS = 100;
};

#endif // AG_PROVISIONING_BLE_TRANSPORT_H
