/**
 * AirGradient Go — CloudService test stubs
 *
 * Hand-rolled link-time stubs for AgClient (concrete class — no virtuals,
 * Trompeloeil cannot mock it without a refactor) and WifiService (mocks
 * only rssi() — the other methods abort if reached, which they should
 * never be in cloud tests).
 *
 * The observable spy state lives in the cloud_spy namespace; tests
 * drive it via friend-class helpers and atomic-state setters.
 */

#include <atomic>
#include <cstddef>
#include <cstring>

#include "services/ag_client.h"
#include "types/wifi_types.h"
#include "go_wifi.h"

// ============================================================================
// cloud_spy — observable state for go_cloud tests
// ============================================================================

namespace cloud_spy {

// --- AgClient ---
uint32_t post_call_count = 0;
uint32_t fetch_call_count = 0;

// Last POST snapshot (and the RSSI value the cloud task forwarded)
MeasuresAGo last_post_snapshot{};
int last_post_signal = 0;
uint32_t last_post_boot = 0;

// Last FETCH parameters
char *last_fetch_buf = nullptr;
size_t last_fetch_buf_size = 0;

// Configurable return values
AgClientResult next_post_result = AgClientResult::Ok;
AgClientResult next_fetch_result = AgClientResult::Ok;

// Bytes written into the caller's buffer during fetch (writes a small
// canned body so the cloud task's logging branch is exercised).
size_t fetch_bytes_to_write = 0;
const char *fetch_body_to_write = "";

// Optional hooks that fire during the synchronous call.  Tests use them
// to simulate a long-running call (advance clock) or to flip atomic
// state mid-call.
void (*on_post_hook)() = nullptr;
void (*on_fetch_hook)() = nullptr;

// --- WifiService ---
int wifi_rssi = -55;

void reset() {
  post_call_count = 0;
  fetch_call_count = 0;
  last_post_snapshot = MeasuresAGo{};
  last_post_signal = 0;
  last_post_boot = 0;
  last_fetch_buf = nullptr;
  last_fetch_buf_size = 0;
  next_post_result = AgClientResult::Ok;
  next_fetch_result = AgClientResult::Ok;
  fetch_bytes_to_write = 0;
  fetch_body_to_write = "";
  on_post_hook = nullptr;
  on_fetch_hook = nullptr;
  wifi_rssi = -55;
}

} // namespace cloud_spy

// ============================================================================
// AgClient stubs (concrete class — link-time replacement)
// ============================================================================

bool AgClient::begin(const char * /*serial_number*/, NetworkType /*network*/,
                     CellularModem * /*modem*/) {
  return true;
}

AgClientResult AgClient::http_post_measures(const Measures & /*m*/, int /*signal*/,
                                            uint32_t /*boot*/) {
  return AgClientResult::Ok;
}

AgClientResult AgClient::http_post_measures(const MeasuresBasic & /*m*/, int /*signal*/,
                                            uint32_t /*boot*/) {
  return AgClientResult::Ok;
}

AgClientResult AgClient::http_post_measures(const MeasuresAGo &m, int signal, uint32_t boot) {
  cloud_spy::post_call_count += 1;
  cloud_spy::last_post_snapshot = m;
  cloud_spy::last_post_signal = signal;
  cloud_spy::last_post_boot = boot;
  if (cloud_spy::on_post_hook != nullptr) {
    cloud_spy::on_post_hook();
  }
  return cloud_spy::next_post_result;
}

AgClientResult AgClient::http_fetch_config(char *config_out, size_t config_size,
                                           size_t *bytes_written) {
  cloud_spy::fetch_call_count += 1;
  cloud_spy::last_fetch_buf = config_out;
  cloud_spy::last_fetch_buf_size = config_size;

  size_t to_write = cloud_spy::fetch_bytes_to_write;
  if (to_write > config_size) {
    to_write = config_size;
  }
  if (to_write > 0 && config_out != nullptr && cloud_spy::fetch_body_to_write != nullptr) {
    std::memcpy(config_out, cloud_spy::fetch_body_to_write, to_write);
  }
  if (bytes_written != nullptr) {
    *bytes_written = cloud_spy::fetch_bytes_to_write;
  }

  if (cloud_spy::on_fetch_hook != nullptr) {
    cloud_spy::on_fetch_hook();
  }
  return cloud_spy::next_fetch_result;
}

// ============================================================================
// WifiService stubs (only rssi() is exercised; everything else aborts to
// surface accidental use during cloud-task testing)
// ============================================================================

WifiService::WifiService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg)
    : _event_queue(event_queue), _wifi(deps.wifi), _ble(deps.ble), _http(deps.http),
      _local_server(deps.local_server), _cfg(cfg) {}

WifiService::~WifiService() = default;

int WifiService::rssi() const { return cloud_spy::wifi_rssi; }

// All other public methods are unused by CloudService.  Provide trivial
// implementations to satisfy the linker without aborting in case
// something pulls them in via the header.
bool WifiService::has_saved_networks() const { return false; }
void WifiService::connect_with_saved_credentials(const WifiStaticIpConfig * /*static_ip*/) {}
void WifiService::schedule_reconnect(const WifiStaticIpConfig * /*static_ip*/) {}
void WifiService::try_default_fallback_credentials() {}
void WifiService::start_provisioning(ProvisioningTransport /*t*/) {}
void WifiService::switch_provisioning_transport() {}
void WifiService::stop_provisioning(bool /*stop_http_server*/) {}
bool WifiService::ensure_local_http() { return false; }
bool WifiService::ensure_local_mdns() { return false; }
void WifiService::stop_local_endpoint() {}
void WifiService::shutdown() {}
void WifiService::clear_credentials() {}
bool WifiService::is_online() const { return false; }
bool WifiService::is_connecting() const { return false; }
bool WifiService::is_provisioning() const { return false; }
ProvisioningTransport WifiService::current_transport() const {
  return ProvisioningTransport::BleOnly;
}
uint32_t WifiService::ip() const { return 0; }
WifiDisconnectReason WifiService::last_disconnect_reason() const {
  return WifiDisconnectReason::unknown;
}
bool WifiService::has_been_online() const { return false; }
uint32_t WifiService::next_deadline_ms() const { return 0; }
void WifiService::tick(uint32_t /*now*/) {}

void WifiService::_install_wifi_callbacks() {}
void WifiService::_detach_wifi_callbacks() {}
void WifiService::_on_got_ip(uint32_t /*ip*/) {}
void WifiService::_on_disconnected(WifiDisconnectReason /*r*/) {}
void WifiService::_reset_deadline() {}
void WifiService::_arm_deadline(uint32_t /*window_ms*/) {}
void WifiService::_reset_online_latches() {}
bool WifiService::_post_wifi_disconnected(WifiDisconnectReason /*r*/) { return true; }
