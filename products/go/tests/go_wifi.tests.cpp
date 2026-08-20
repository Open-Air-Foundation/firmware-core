/**
 * AirGradient Go — WifiService unit tests
 *
 * Drives saved-credentials, factory-fallback, callback adapters, the
 * initial-connect deadline, and shutdown behaviour through friend-class
 * access. WifiManager is the real one, linked against a fake WifiHal.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_wifi.h"

#include "fake_config_store.h"
#include "go_events.h"
#include "hal/ble_server.h"
#include "hal/http_server.h"
#include "hal/wifi_hal.h"
#include "rtos.h"
#include "services/local_server.h"
#include "services/wifi_manager.h"
#include "types/provisioning_types.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int LOCAL_API_ROUTE_COUNT = 6;

// ---------------------------------------------------------------------------
// FakeWifiHal — minimal in-memory HAL for WifiManager
// ---------------------------------------------------------------------------

class FakeWifiHal : public WifiHal {
public:
  WifiStatus init() override { return WifiStatus::Ok; }
  void deinit() override {}

  WifiStatus set_mode(WifiMode mode) override {
    _mode = mode;
    last_mode_set = mode;
    if (mode == WifiMode::Off && call_log != nullptr) {
      call_log->push_back("wifi_off");
    }
    return WifiStatus::Ok;
  }
  WifiMode get_mode() const override { return _mode; }

  WifiStatus connect_sta(const char *ssid, const char *password) override {
    ++connect_calls;
    last_ssid = ssid != nullptr ? ssid : "";
    last_password = password != nullptr ? password : "";
    return WifiStatus::Ok;
  }
  WifiStatus disconnect_sta() override {
    ++disconnect_calls;
    return WifiStatus::Ok;
  }

  WifiStatus set_static_ip(const WifiStaticIpConfig &cfg) override {
    last_static_ip = cfg;
    ++set_static_ip_calls;
    return WifiStatus::Ok;
  }
  WifiStatus clear_static_ip() override {
    ++clear_static_ip_calls;
    return WifiStatus::Ok;
  }

  WifiStatus start_scan(const WifiScanConfig &) override { return WifiStatus::Ok; }
  WifiStatus start_ap(const WifiApConfig &) override { return WifiStatus::Ok; }
  WifiStatus stop_ap() override { return WifiStatus::Ok; }

  WifiStatusSnapshot get_status() const override {
    WifiStatusSnapshot s;
    s.mode = _mode;
    s.rssi = -55;
    return s;
  }
  WifiStatus set_power_save(WifiPowerSave mode) override {
    ++set_power_save_calls;
    last_power_save = mode;
    return set_power_save_status;
  }
  WifiStatus start_mdns(const WifiMdnsConfig &config) override {
    ++start_mdns_calls;
    last_mdns_hostname = config.hostname != nullptr ? config.hostname : "";
    last_mdns_services.clear();
    for (uint8_t i = 0; i < config.service_count; ++i) {
      const WifiMdnsServiceRecord &service = config.services[i];
      CapturedMdnsService captured{};
      captured.service_type = service.service_type != nullptr ? service.service_type : "";
      captured.port = service.port;
      for (uint8_t j = 0; j < service.txt_count; ++j) {
        captured.txt.emplace_back(service.txt_keys[j], service.txt_values[j]);
      }
      last_mdns_services.push_back(captured);
    }
    if (call_log != nullptr) {
      call_log->push_back("mdns_start");
    }
    return start_mdns_status;
  }
  WifiStatus stop_mdns() override {
    ++stop_mdns_calls;
    if (call_log != nullptr) {
      call_log->push_back("mdns_stop");
    }
    return WifiStatus::Ok;
  }

  WifiStatus arm_dhcp_timeout(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_dhcp_timeout() override { return WifiStatus::Ok; }
  WifiStatus arm_retry_timer(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_retry_timer() override { return WifiStatus::Ok; }

  void set_on_sta_connected(WifiConnectedCallback cb) override { sta_connected_cb = std::move(cb); }
  void set_on_sta_disconnected(std::function<void(int)> cb) override {
    sta_disconnected_cb = std::move(cb);
  }
  void set_on_got_ip(WifiGotIpCallback cb) override { got_ip_cb = std::move(cb); }
  void set_on_scan_complete(WifiScanCompleteCallback) override {}
  void set_on_ap_client_joined(WifiApClientJoinedCallback) override {}
  void set_on_ap_client_left(WifiApClientLeftCallback) override {}
  void set_on_dhcp_timeout(std::function<void()>) override {}
  void set_on_retry_due(std::function<void()>) override {}

  // Observables
  int connect_calls = 0;
  int disconnect_calls = 0;
  int set_static_ip_calls = 0;
  int clear_static_ip_calls = 0;
  int set_power_save_calls = 0;
  WifiMode last_mode_set = WifiMode::Off;
  WifiPowerSave last_power_save = WifiPowerSave::MinModem;
  WifiStatus set_power_save_status = WifiStatus::Ok;
  std::string last_ssid;
  std::string last_password;
  WifiStaticIpConfig last_static_ip{};
  struct CapturedMdnsService {
    std::string service_type;
    uint16_t port = 0;
    std::vector<std::pair<std::string, std::string>> txt;
  };
  WifiStatus start_mdns_status = WifiStatus::Ok;
  int start_mdns_calls = 0;
  int stop_mdns_calls = 0;
  std::string last_mdns_hostname;
  std::vector<CapturedMdnsService> last_mdns_services;
  std::vector<std::string> *call_log = nullptr;

  WifiConnectedCallback sta_connected_cb;
  std::function<void(int)> sta_disconnected_cb;
  WifiGotIpCallback got_ip_cb;

private:
  WifiMode _mode = WifiMode::Off;
};

// ---------------------------------------------------------------------------
// No-op stubs for the borrowed BLE / HTTP server refs.
// CP2.2 never touches them; CP2.3 wires provisioning.
// ---------------------------------------------------------------------------

class StubBleServer : public AgBleServer {
public:
  bool init(const char *) override { return true; }
  void deinit() override {}
  bool set_security(AgBleIoCapability, uint8_t) override { return true; }
  bool delete_all_bonds() override { return true; }
  AgBleGattService *add_service(const char *) override { return nullptr; }
  bool set_advertising_name(const char *) override { return true; }
  bool add_advertised_service_uuid(const char *) override { return true; }
  bool set_manufacturer_data(const uint8_t *, size_t) override { return true; }
  bool start_advertising() override { return true; }
  bool stop_advertising() override { return true; }
  void set_connect_callback(AgBleConnectCallback) override {}
  void set_disconnect_callback(AgBleDisconnectCallback) override {}
  void set_passkey_display_callback(AgBlePasskeyDisplayCallback) override {}
  void set_auth_complete_callback(AgBleAuthCompleteCallback) override {}
};

class StubHttpServer : public HttpServer {
public:
  bool start(uint16_t port) override {
    ++start_calls;
    last_port = port;
    if (call_log != nullptr) {
      call_log->push_back("http_start");
    }
    return start_result;
  }
  void stop() override {
    ++stop_calls;
    if (call_log != nullptr) {
      call_log->push_back("http_stop");
    }
  }
  bool register_route(HttpMethod, const char *, HttpHandler) override {
    ++register_calls;
    const bool success =
        register_result && (register_fail_on == 0 || register_calls != register_fail_on);
    if (success) {
      ++active_routes;
    }
    return success;
  }
  bool unregister_route(HttpMethod, const char *) override {
    ++unregister_calls;
    if (active_routes > 0) {
      --active_routes;
    }
    if (call_log != nullptr) {
      call_log->push_back("route_end");
    }
    return true;
  }
  void unregister_all() override {
    ++unregister_all_calls;
    active_routes = 0;
  }

  bool start_result = true;
  bool register_result = true;
  int register_fail_on = 0;
  int start_calls = 0;
  int stop_calls = 0;
  int register_calls = 0;
  int unregister_calls = 0;
  int unregister_all_calls = 0;
  int active_routes = 0;
  uint16_t last_port = 0;
  std::vector<std::string> *call_log = nullptr;
};

class StubLocalProviders : public MeasuresProvider, public ConfigProvider, public ActionHandler {
public:
  Measures get_measures() override { return {}; }
  SystemInfo get_system_info() override { return {}; }
  LocalServerConfig get_config() override { return {}; }
  ConfigSubmitResult submit_config(const LocalServerConfig &) override {
    return {ConfigSubmitStatus::Accepted, ConfigFieldId::None};
  }
  ActionResult trigger(ActionId) override { return {ActionStatus::Dispatched}; }
};

// ---------------------------------------------------------------------------
// FakeRTOS — controls queue admission and time, captures accepted events.
// ---------------------------------------------------------------------------

class FakeRTOS : public FreeRTOS {
public:
  uint64_t get_time_ms_impl() override { return now_ms; }
  bool queue_send_impl(RtosQueueHandle, const void *item, uint32_t timeout_ms) override {
    ++queue_send_calls;
    last_queue_send_timeout_ms = timeout_ms;
    if (item == nullptr || !accept_queue_sends) {
      return false;
    }
    captured.push_back(*static_cast<const Event *>(item));
    return true;
  }

  void set_now(uint64_t ms) { now_ms = ms; }
  void advance(uint64_t ms) { now_ms += ms; }
  bool has_event(EventType t) const {
    for (const auto &e : captured) {
      if (e.type == t)
        return true;
    }
    return false;
  }
  const Event *first_event(EventType t) const {
    for (const auto &e : captured) {
      if (e.type == t)
        return &e;
    }
    return nullptr;
  }

  uint64_t now_ms = 0;
  bool accept_queue_sends = true;
  uint32_t queue_send_calls = 0;
  uint32_t last_queue_send_timeout_ms = UINT32_MAX;
  std::vector<Event> captured;
};

} // namespace

// ---------------------------------------------------------------------------
// WifiServiceTestAccess — friend for private state inspection.
// ---------------------------------------------------------------------------

class WifiServiceTestAccess {
public:
  static ProvisioningManager *provisioning_manager(const WifiService &s) { return s._prov; }
  static uint32_t deadline(const WifiService &s) { return s._initial_connect_deadline_ms; }
  static uint32_t reconnect_at(const WifiService &s) { return s._reconnect_at_ms; }
  static bool clear_pending(const WifiService &s) { return s._clear_deadline_pending.load(); }
  static bool provisioning_connected_event_pending(const WifiService &s) {
    return s._provisioning_connected_event_pending.load();
  }
  static bool provisioning_active(const WifiService &s) { return s._provisioning_active; }
  static bool local_http_active(const WifiService &s) { return s._local_http_active; }
  static void set_provisioning_active(WifiService &s, bool active) {
    s._provisioning_active = active;
  }
  static void set_switching_transport(WifiService &s, bool on) { s._switching_transport = on; }
  static void set_transport(WifiService &s, ProvisioningTransport t) { s._transport = t; }
  static void on_provisioning_event(WifiService &s, const ProvisioningEventInfo &info) {
    s._on_provisioning_event(info);
  }
};

namespace {

uint8_t event_queue_sentinel = 0;

// Common fixture: real WifiManager backed by FakeWifiHal + fake store.
struct Fixture {
  FakeRTOS rtos;
  FakeWifiHal hal;
  FakeConfigStore store;
  WifiManager wifi{hal, store};
  StubBleServer ble;
  StubHttpServer http;
  StubLocalProviders providers;
  LocalServer local_server{http, {providers, &providers, ConfigAccess::ReadWrite, &providers}};
  WifiService svc;

  explicit Fixture(const WifiService::Config &config = {})
      : svc(&event_queue_sentinel, WifiService::Deps{wifi, ble, http, local_server}, config) {
    RTOS::set_instance(&rtos);
  }

  ~Fixture() { RTOS::set_instance(nullptr); }

  // Seed one saved network so auto-connect resolves directly (no scan).
  void seed_network() { wifi.add_network("saved", "pw"); }
};

} // namespace

// ---------------------------------------------------------------------------
// Saved-credential connect
// ---------------------------------------------------------------------------

TEST_CASE("has_saved_networks forwards to WifiManager", "[go_wifi][creds]") {
  Fixture f;
  CHECK_FALSE(f.svc.has_saved_networks());
  f.seed_network();
  CHECK(f.svc.has_saved_networks());
}

TEST_CASE("connect_with_saved_credentials sets STA mode, calls connect, arms 30s deadline",
          "[go_wifi][saved]") {
  Fixture f;
  f.seed_network();
  f.rtos.set_now(1000);

  f.svc.connect_with_saved_credentials();

  CHECK(f.hal.last_mode_set == WifiMode::Sta);
  CHECK(f.hal.set_power_save_calls == 1);
  CHECK(f.hal.last_power_save == WifiPowerSave::None);
  CHECK(f.hal.connect_calls == 1);
  CHECK(f.hal.last_ssid == "saved"); // single saved network resolved directly
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 1000 + 30000);
  CHECK(f.svc.is_connecting());
  CHECK_FALSE(f.svc.is_online());
}

TEST_CASE("connect_with_saved_credentials applies static IP when provided",
          "[go_wifi][saved][static_ip]") {
  Fixture f;
  f.seed_network();

  WifiStaticIpConfig ip{};
  ip.ip = 0x0100A8C0;
  ip.netmask = 0x00FFFFFF;
  f.svc.connect_with_saved_credentials(&ip);

  CHECK(f.hal.set_static_ip_calls == 1);
  CHECK(f.hal.last_static_ip.ip == 0x0100A8C0);
}

TEST_CASE("connect_with_saved_credentials clears static IP when nullptr",
          "[go_wifi][saved][static_ip]") {
  Fixture f;
  f.seed_network();
  f.svc.connect_with_saved_credentials(nullptr);
  CHECK(f.hal.clear_static_ip_calls == 1);
  CHECK(f.hal.set_static_ip_calls == 0);
}

TEST_CASE("connect_with_saved_credentials with no saved networks posts WifiDisconnected, "
          "no deadline",
          "[go_wifi][saved][disconnect]") {
  Fixture f; // store empty

  f.svc.connect_with_saved_credentials();

  // WifiManager::connect() returned NotFound — no driver call.
  CHECK(f.hal.connect_calls == 0);
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);

  const Event *evt = f.rtos.first_event(EventType::WifiDisconnected);
  REQUIRE(evt != nullptr);
  CHECK(evt->wifi_disconnect_reason == static_cast<uint8_t>(WifiDisconnectReason::no_ap_found));
}

// ---------------------------------------------------------------------------
// Factory-default fallback
// ---------------------------------------------------------------------------

TEST_CASE("try_default_fallback_credentials connects with airgradient/cleanair transiently, "
          "arms 15s deadline",
          "[go_wifi][fallback]") {
  Fixture f;
  f.rtos.set_now(5000);

  f.svc.try_default_fallback_credentials();

  CHECK(f.hal.last_mode_set == WifiMode::Sta);
  CHECK(f.hal.set_power_save_calls == 1);
  CHECK(f.hal.last_power_save == WifiPowerSave::None);
  REQUIRE(f.hal.connect_calls == 1);
  CHECK(f.hal.last_ssid == "airgradient");
  CHECK(f.hal.last_password == "cleanair");
  // Explicit SSID is transient: nothing written to the saved-networks store.
  CHECK_FALSE(f.svc.has_saved_networks());
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 5000 + 15000);
}

TEST_CASE("Stationary connect continues when disabling power save fails",
          "[go_wifi][saved][power-save]") {
  Fixture f;
  f.seed_network();
  f.hal.set_power_save_status = WifiStatus::Failed;

  f.svc.connect_with_saved_credentials();

  CHECK(f.hal.set_power_save_calls == 1);
  CHECK(f.hal.connect_calls == 1);
}

TEST_CASE("fallback path does not request static IP", "[go_wifi][fallback]") {
  Fixture f;
  f.svc.try_default_fallback_credentials();
  CHECK(f.hal.set_static_ip_calls == 0);
  CHECK(f.hal.clear_static_ip_calls == 1);
}

// ---------------------------------------------------------------------------
// Callback adapters
// ---------------------------------------------------------------------------

TEST_CASE("on_got_ip updates online state, latches deadline-clear, posts WifiConnected",
          "[go_wifi][callbacks]") {
  Fixture f;
  f.seed_network();
  f.svc.connect_with_saved_credentials();
  REQUIRE(WifiServiceTestAccess::deadline(f.svc) != 0);

  REQUIRE(f.hal.got_ip_cb);
  f.hal.got_ip_cb(0x0100A8C0);

  CHECK(f.svc.is_online());
  CHECK(f.svc.has_been_online());
  CHECK(f.svc.ip() == 0x0100A8C0);
  CHECK(WifiServiceTestAccess::clear_pending(f.svc));

  const Event *evt = f.rtos.first_event(EventType::WifiConnected);
  REQUIRE(evt != nullptr);
  CHECK(evt->wifi_ip == 0x0100A8C0);
}

TEST_CASE("on_disconnected clears online and posts WifiDisconnected with reason",
          "[go_wifi][callbacks]") {
  Fixture f;
  f.seed_network();
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x0100A8C0);
  f.rtos.captured.clear();

  // Drive WifiManager's disconnect path through raw ESP-IDF code 2
  // (WIFI_REASON_AUTH_EXPIRE) — maps to auth_failed.
  REQUIRE(f.hal.sta_disconnected_cb);
  f.hal.sta_disconnected_cb(/*WIFI_REASON_AUTH_EXPIRE*/ 2);

  CHECK_FALSE(f.svc.is_online());

  const Event *evt = f.rtos.first_event(EventType::WifiDisconnected);
  REQUIRE(evt != nullptr);
  CHECK(evt->wifi_disconnect_reason == static_cast<uint8_t>(WifiDisconnectReason::auth_failed));
}

TEST_CASE("on_disconnected suppresses requested_by_user (no event)", "[go_wifi][callbacks]") {
  Fixture f;
  // disconnect() before any connect emits requested_by_user synthetically;
  // service must swallow it because the service's own teardown drives it.
  f.svc.connect_with_saved_credentials(); // arms internal state
  f.rtos.captured.clear();
  f.wifi.disconnect();
  CHECK_FALSE(f.rtos.has_event(EventType::WifiDisconnected));
}

// ---------------------------------------------------------------------------
// tick() — deadline lifecycle
// ---------------------------------------------------------------------------

TEST_CASE("tick clears deadline after on_got_ip latches the pending flag", "[go_wifi][tick]") {
  Fixture f;
  f.seed_network();
  f.rtos.set_now(1000);
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x01010101);
  REQUIRE(WifiServiceTestAccess::clear_pending(f.svc));

  f.svc.tick(2000);

  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
  CHECK_FALSE(WifiServiceTestAccess::clear_pending(f.svc));
}

TEST_CASE("tick fires synthetic WifiDisconnected{connection_lost} on deadline expiry",
          "[go_wifi][tick]") {
  Fixture f;
  f.rtos.set_now(0);
  f.svc.try_default_fallback_credentials(); // arms 15s deadline at t=0
  REQUIRE(WifiServiceTestAccess::deadline(f.svc) == 15000);
  f.rtos.captured.clear();

  f.svc.tick(14999);
  CHECK_FALSE(f.rtos.has_event(EventType::WifiDisconnected));

  f.svc.tick(15000);
  const Event *evt = f.rtos.first_event(EventType::WifiDisconnected);
  REQUIRE(evt != nullptr);
  CHECK(evt->wifi_disconnect_reason == static_cast<uint8_t>(WifiDisconnectReason::connection_lost));
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
}

TEST_CASE("tick is a no-op when no deadline armed", "[go_wifi][tick]") {
  Fixture f;
  f.svc.tick(60000);
  CHECK(f.rtos.captured.empty());
}

// ---------------------------------------------------------------------------
// Runtime reconnect
// ---------------------------------------------------------------------------

TEST_CASE("schedule_reconnect arms the reconnect timer reconnect_delay_ms out",
          "[go_wifi][reconnect]") {
  Fixture f;
  f.seed_network();
  f.rtos.set_now(1000);

  f.svc.schedule_reconnect();

  // Default reconnect_delay_ms == 5000.
  CHECK(WifiServiceTestAccess::reconnect_at(f.svc) == 1000 + 5000);
  // No connect issued yet — that happens from tick().
  CHECK(f.hal.connect_calls == 0);
}

TEST_CASE("schedule_reconnect arms the reconnect timer without saved networks",
          "[go_wifi][reconnect][fallback]") {
  Fixture f; // store empty
  f.rtos.set_now(1000);

  f.svc.schedule_reconnect();

  CHECK(WifiServiceTestAccess::reconnect_at(f.svc) == 1000 + 5000);
}

TEST_CASE("tick fires the reconnect without resetting has_been_online", "[go_wifi][reconnect]") {
  Fixture f;
  f.seed_network();
  // First online, then a runtime drop -> schedule reconnect.
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x01010101);
  REQUIRE(f.svc.has_been_online());

  f.rtos.set_now(1000);
  f.svc.schedule_reconnect();
  REQUIRE(WifiServiceTestAccess::reconnect_at(f.svc) == 1000 + 5000);
  const int connects_before = f.hal.connect_calls;

  // Not yet due (reconnect_at == 6000).
  f.svc.tick(5999);
  CHECK(f.hal.connect_calls == connects_before);

  // Due: re-issues the saved connect, no connect window, latch preserved.
  f.svc.tick(6000);
  CHECK(f.hal.connect_calls == connects_before + 1);
  CHECK(f.hal.last_ssid == "saved");
  CHECK(WifiServiceTestAccess::reconnect_at(f.svc) == 0);
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0); // runtime reconnect arms no window
  CHECK(f.svc.has_been_online());                     // not reset
}

TEST_CASE("tick reconnects a fallback-only session without saving credentials",
          "[go_wifi][reconnect][fallback]") {
  Fixture f;
  f.rtos.set_now(1000);

  f.svc.try_default_fallback_credentials();
  f.hal.got_ip_cb(0x01010101);
  f.svc.tick(1000); // consume the deferred initial-deadline clear
  REQUIRE(f.svc.has_been_online());
  REQUIRE(WifiServiceTestAccess::deadline(f.svc) == 0);
  REQUIRE_FALSE(f.svc.has_saved_networks());

  f.rtos.captured.clear();
  REQUIRE(f.hal.sta_disconnected_cb);
  f.hal.sta_disconnected_cb(/*WIFI_REASON_AUTH_EXPIRE*/ 2);
  REQUIRE_FALSE(f.svc.is_online());
  REQUIRE(f.rtos.has_event(EventType::WifiDisconnected));

  WifiStaticIpConfig static_ip{};
  static_ip.ip = 0x0200A8C0;
  f.svc.schedule_reconnect(&static_ip);
  REQUIRE(WifiServiceTestAccess::reconnect_at(f.svc) == 1000 + 5000);
  const int connects_before = f.hal.connect_calls;

  f.svc.tick(5999);
  CHECK(f.hal.connect_calls == connects_before);

  f.svc.tick(6000);
  CHECK(f.hal.connect_calls == connects_before + 1);
  CHECK(f.hal.last_ssid == "airgradient");
  CHECK(f.hal.last_password == "cleanair");
  CHECK(f.hal.set_static_ip_calls == 0);
  CHECK(f.hal.clear_static_ip_calls == 2);
  CHECK(WifiServiceTestAccess::reconnect_at(f.svc) == 0);
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
  CHECK(f.svc.has_been_online());
  CHECK_FALSE(f.svc.has_saved_networks());

  // A terminal failure of the runtime attempt can drive another identical
  // orchestrator-scheduled cycle without falling back to bring-up policy.
  f.hal.sta_disconnected_cb(/*WIFI_REASON_AUTH_EXPIRE*/ 2);
  f.rtos.set_now(6000);
  f.svc.schedule_reconnect(&static_ip);
  f.svc.tick(11000);
  CHECK(f.hal.connect_calls == connects_before + 2);
  CHECK(f.hal.last_ssid == "airgradient");
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
  CHECK(f.svc.has_been_online());
}

TEST_CASE("next_deadline_ms returns the nearer of connect window and reconnect timer",
          "[go_wifi][reconnect]") {
  Fixture f;
  f.seed_network();
  f.rtos.set_now(0);

  // Only the connect window armed.
  f.svc.connect_with_saved_credentials();
  CHECK(f.svc.next_deadline_ms() == 30000);

  // Arm a nearer reconnect timer; it should win.
  f.svc.schedule_reconnect();
  CHECK(f.svc.next_deadline_ms() == 5000);
}

TEST_CASE("shutdown clears a pending reconnect timer", "[go_wifi][reconnect][shutdown]") {
  Fixture f;
  f.seed_network();
  f.svc.schedule_reconnect();
  REQUIRE(WifiServiceTestAccess::reconnect_at(f.svc) != 0);

  f.svc.shutdown();

  CHECK(WifiServiceTestAccess::reconnect_at(f.svc) == 0);
}

// ---------------------------------------------------------------------------
// shutdown / clear_credentials
// ---------------------------------------------------------------------------

TEST_CASE("shutdown zeros the deadline and resets online state", "[go_wifi][shutdown]") {
  Fixture f;
  f.seed_network();
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x01010101);
  REQUIRE(f.svc.is_online());
  REQUIRE(f.svc.has_been_online());

  f.svc.shutdown();

  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
  CHECK_FALSE(f.svc.is_online());
  CHECK_FALSE(f.svc.has_been_online());
  CHECK(f.hal.last_mode_set == WifiMode::Off);
}

TEST_CASE("connect after shutdown restores WifiService callbacks", "[go_wifi][shutdown][saved]") {
  Fixture f;
  f.seed_network();

  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x01010101);
  REQUIRE(f.svc.is_online());

  f.svc.shutdown();
  REQUIRE_FALSE(f.svc.is_online());
  f.rtos.captured.clear();

  f.svc.connect_with_saved_credentials();
  REQUIRE(f.hal.got_ip_cb);
  f.hal.got_ip_cb(0x0200A8C0);

  CHECK(f.svc.is_online());
  CHECK(f.svc.has_been_online());
  CHECK(f.svc.ip() == 0x0200A8C0);

  const Event *evt = f.rtos.first_event(EventType::WifiConnected);
  REQUIRE(evt != nullptr);
  CHECK(evt->wifi_ip == 0x0200A8C0);
}

TEST_CASE("clear_credentials erases saved networks and resets has_been_online",
          "[go_wifi][clear_credentials]") {
  Fixture f;
  f.seed_network();
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x01010101);
  REQUIRE(f.svc.has_been_online());

  f.svc.clear_credentials();

  CHECK_FALSE(f.svc.has_saved_networks());
  CHECK_FALSE(f.svc.has_been_online());
}

// ---------------------------------------------------------------------------
// Provisioning event mapping + transport-switch latch
// ---------------------------------------------------------------------------

TEST_CASE("WifiService constructs provisioning manager only when provisioning begins",
          "[go_wifi][provisioning][lifecycle]") {
  Fixture f;
  f.seed_network();

  CHECK(WifiServiceTestAccess::provisioning_manager(f.svc) == nullptr);

  f.svc.connect_with_saved_credentials();
  CHECK(WifiServiceTestAccess::provisioning_manager(f.svc) == nullptr);

  f.svc.try_default_fallback_credentials();
  CHECK(WifiServiceTestAccess::provisioning_manager(f.svc) == nullptr);

  f.svc.start_provisioning(ProvisioningTransport::BleOnly);
  ProvisioningManager *const manager = WifiServiceTestAccess::provisioning_manager(f.svc);
  REQUIRE(manager != nullptr);

  f.svc.stop_provisioning();
  CHECK(WifiServiceTestAccess::provisioning_manager(f.svc) == manager);
}

TEST_CASE("on_provisioning_event Connected maps disable_cloud and static_ip into payload",
          "[go_wifi][provisioning]") {
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_transport(f.svc, ProvisioningTransport::WifiOnly);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Connected;
  info.ip = 0xC0A80164; // 100.168.192.192 (any LE value)
  info.data.disable_cloud = true;
  info.data.static_ip.ip = 0x0100A8C0;
  info.data.static_ip.netmask = 0x00FFFFFF;

  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  const Event *evt = f.rtos.first_event(EventType::ProvisioningStateChanged);
  REQUIRE(evt != nullptr);
  CHECK(evt->prov.event == static_cast<uint8_t>(ProvisioningEvent::Connected));
  CHECK(evt->prov.transport == static_cast<uint8_t>(ProvisioningTransport::WifiOnly));
  CHECK(evt->prov.ip == 0xC0A80164);
  CHECK(evt->prov.disable_cloud == true);
  CHECK(evt->prov.static_ip.ip == 0x0100A8C0);
  CHECK(evt->prov.static_ip.netmask == 0x00FFFFFF);
}

TEST_CASE("on_provisioning_event swallows Stopped during transport switch",
          "[go_wifi][provisioning][switch]") {
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_switching_transport(f.svc, true);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Stopped;
  info.stop_reason = ProvisioningStopReason::ProductRequested;

  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  CHECK_FALSE(f.rtos.has_event(EventType::ProvisioningStateChanged));
}

TEST_CASE("on_provisioning_event forwards Started during transport switch",
          "[go_wifi][provisioning][switch]") {
  // Started on the new transport must reach the orchestrator so it can
  // update the UI even though the latch is held.
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_switching_transport(f.svc, true);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Started;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  CHECK(f.rtos.has_event(EventType::ProvisioningStateChanged));
}

TEST_CASE("on_provisioning_event Stopped outside switch forwards normally",
          "[go_wifi][provisioning]") {
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Stopped;
  info.stop_reason = ProvisioningStopReason::ProductRequested;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  const Event *evt = f.rtos.first_event(EventType::ProvisioningStateChanged);
  REQUIRE(evt != nullptr);
  CHECK(evt->prov.event == static_cast<uint8_t>(ProvisioningEvent::Stopped));
}

TEST_CASE("failed provisioning Connected event retries from tick until delivered once",
          "[go_wifi][provisioning][queue_retry]") {
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_transport(f.svc, ProvisioningTransport::WifiOnly);
  f.rtos.accept_queue_sends = false;

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Connected;
  info.ip = 0xC0A80164;
  info.stop_reason = ProvisioningStopReason::TimedOut;
  info.data.disable_cloud = true;
  info.data.static_ip.ip = 0x0100A8C0;
  info.data.static_ip.netmask = 0x00FFFFFF;
  info.data.static_ip.gateway = 0x0100A8C0;
  info.data.static_ip.dns_primary = 0x08080808;
  info.data.static_ip.dns_secondary = 0x04040808;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  CHECK(f.rtos.queue_send_calls == 1);
  CHECK(f.rtos.captured.empty());
  CHECK(WifiServiceTestAccess::provisioning_connected_event_pending(f.svc));
  CHECK(f.svc.next_deadline_ms() == 0);

  f.svc.tick(1);
  f.svc.tick(2);
  CHECK(f.rtos.queue_send_calls == 3);
  CHECK(f.rtos.captured.empty());
  CHECK(WifiServiceTestAccess::provisioning_connected_event_pending(f.svc));

  f.rtos.accept_queue_sends = true;
  f.svc.tick(3);

  REQUIRE(f.rtos.captured.size() == 1);
  const Event &evt = f.rtos.captured.front();
  CHECK(evt.type == EventType::ProvisioningStateChanged);
  CHECK(evt.prov.event == static_cast<uint8_t>(ProvisioningEvent::Connected));
  CHECK(evt.prov.transport == static_cast<uint8_t>(ProvisioningTransport::WifiOnly));
  CHECK(evt.prov.stop_reason == static_cast<uint8_t>(ProvisioningStopReason::TimedOut));
  CHECK(evt.prov.ip == info.ip);
  CHECK(evt.prov.disable_cloud == info.data.disable_cloud);
  CHECK(evt.prov.static_ip.ip == info.data.static_ip.ip);
  CHECK(evt.prov.static_ip.netmask == info.data.static_ip.netmask);
  CHECK(evt.prov.static_ip.gateway == info.data.static_ip.gateway);
  CHECK(evt.prov.static_ip.dns_primary == info.data.static_ip.dns_primary);
  CHECK(evt.prov.static_ip.dns_secondary == info.data.static_ip.dns_secondary);
  CHECK(f.rtos.last_queue_send_timeout_ms == 0);
  CHECK_FALSE(WifiServiceTestAccess::provisioning_connected_event_pending(f.svc));

  const uint32_t sends_after_delivery = f.rtos.queue_send_calls;
  f.svc.tick(4);
  CHECK(f.rtos.queue_send_calls == sends_after_delivery);
  CHECK(f.rtos.captured.size() == 1);
}

TEST_CASE("failed non-Connected provisioning event is not retried",
          "[go_wifi][provisioning][queue_retry]") {
  Fixture f;
  f.rtos.accept_queue_sends = false;

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Started;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  CHECK(f.rtos.queue_send_calls == 1);
  CHECK_FALSE(WifiServiceTestAccess::provisioning_connected_event_pending(f.svc));

  f.rtos.accept_queue_sends = true;
  f.svc.tick(1);
  CHECK(f.rtos.queue_send_calls == 1);
  CHECK(f.rtos.captured.empty());
}

TEST_CASE("fresh provisioning start clears a retained Connected event",
          "[go_wifi][provisioning][queue_retry][lifecycle]") {
  Fixture f;
  f.rtos.accept_queue_sends = false;

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Connected;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);
  REQUIRE(WifiServiceTestAccess::provisioning_connected_event_pending(f.svc));

  f.svc.start_provisioning(ProvisioningTransport::BleOnly);
  CHECK_FALSE(WifiServiceTestAccess::provisioning_connected_event_pending(f.svc));

  f.rtos.accept_queue_sends = true;
  f.svc.tick(1);
  CHECK(f.rtos.captured.empty());
}

TEST_CASE("shutdown clears a retained provisioning Connected event",
          "[go_wifi][provisioning][queue_retry][shutdown]") {
  Fixture f;
  f.rtos.accept_queue_sends = false;

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Connected;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);
  REQUIRE(WifiServiceTestAccess::provisioning_connected_event_pending(f.svc));

  f.svc.shutdown();
  CHECK_FALSE(WifiServiceTestAccess::provisioning_connected_event_pending(f.svc));

  f.rtos.accept_queue_sends = true;
  f.svc.tick(1);
  CHECK(f.rtos.captured.empty());
}

TEST_CASE("start_provisioning calls _wifi.disconnect and zeros the deadline",
          "[go_wifi][provisioning]") {
  Fixture f;
  // Arm a deadline via a saved-creds connect.
  f.seed_network();
  f.rtos.set_now(1000);
  f.svc.connect_with_saved_credentials();
  REQUIRE(WifiServiceTestAccess::deadline(f.svc) != 0);
  const int hal_disconnects_before = f.hal.disconnect_calls;

  f.svc.start_provisioning(ProvisioningTransport::BleOnly);

  CHECK(f.hal.disconnect_calls > hal_disconnects_before);
  CHECK(WifiServiceTestAccess::deadline(f.svc) == 0);
}

// ---------------------------------------------------------------------------
// Local HTTP and mDNS endpoint
// ---------------------------------------------------------------------------

TEST_CASE("local endpoint starts routes before listener and advertises exact identity",
          "[go_wifi][local_endpoint]") {
  WifiService::Config config{};
  config.serial_number = "ABCDEF123456";
  config.firmware_version = "4.2.0";
  config.model = "P-1PSG";
  config.hostname = "airgradient-ABCDEF123456";
  config.http_port = 8080;
  Fixture f(config);
  f.seed_network();
  f.svc.connect_with_saved_credentials();
  REQUIRE(f.hal.got_ip_cb);
  f.hal.got_ip_cb(0x0100A8C0);

  REQUIRE(f.svc.ensure_local_http());
  CHECK(f.http.register_calls == LOCAL_API_ROUTE_COUNT);
  CHECK(f.http.start_calls == 1);
  CHECK(f.http.last_port == 8080);
  CHECK(WifiServiceTestAccess::local_http_active(f.svc));
  CHECK(f.svc.ensure_local_http());
  CHECK(f.http.register_calls == LOCAL_API_ROUTE_COUNT);
  CHECK(f.http.start_calls == 1);

  REQUIRE(f.svc.ensure_local_mdns());
  CHECK(f.hal.last_mdns_hostname == "airgradient-ABCDEF123456");
  REQUIRE(f.hal.last_mdns_services.size() == 1);
  const auto &service = f.hal.last_mdns_services[0];
  CHECK(service.service_type == "_airgradient._tcp");
  CHECK(service.port == 8080);
  REQUIRE(service.txt.size() == 5);
  CHECK(service.txt[0] == std::make_pair(std::string("vendor"), std::string("AirGradient")));
  CHECK(service.txt[1] == std::make_pair(std::string("model"), std::string("P-1PSG")));
  CHECK(service.txt[2] == std::make_pair(std::string("serialno"), std::string("ABCDEF123456")));
  CHECK(service.txt[3] == std::make_pair(std::string("fw_ver"), std::string("4.2.0")));
  CHECK(service.txt[4] == std::make_pair(std::string("api"), std::string("1")));
  const int mdns_starts = f.hal.start_mdns_calls;
  CHECK(f.svc.ensure_local_mdns());
  CHECK(f.hal.start_mdns_calls == mdns_starts);
}

TEST_CASE("partial local route failure rolls back and suppresses listener",
          "[go_wifi][local_endpoint][rollback]") {
  Fixture f;
  f.http.register_fail_on = 3;

  CHECK_FALSE(f.svc.ensure_local_http());
  CHECK(f.http.register_calls == 3);
  CHECK(f.http.unregister_calls == 2);
  CHECK(f.http.start_calls == 0);
  CHECK_FALSE(WifiServiceTestAccess::local_http_active(f.svc));
}

TEST_CASE("listener start failure rolls back local routes", "[go_wifi][local_endpoint][rollback]") {
  Fixture f;
  f.http.start_result = false;

  CHECK_FALSE(f.svc.ensure_local_http());
  CHECK_FALSE(WifiServiceTestAccess::local_http_active(f.svc));
  CHECK(f.http.register_calls == LOCAL_API_ROUTE_COUNT);
  CHECK(f.http.unregister_calls == LOCAL_API_ROUTE_COUNT);
  CHECK(f.hal.start_mdns_calls == 0);
}

TEST_CASE("mDNS failure retains active HTTP and can be retried independently",
          "[go_wifi][local_endpoint][mdns][retry]") {
  WifiService::Config config{};
  config.serial_number = "ABCDEF123456";
  config.firmware_version = "4.2.0";
  config.model = "P-1PSG";
  config.hostname = "airgradient-ABCDEF123456";
  Fixture f(config);
  f.seed_network();
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x0100A8C0);
  REQUIRE(f.svc.ensure_local_http());
  f.hal.start_mdns_status = WifiStatus::Failed;

  CHECK_FALSE(f.svc.ensure_local_mdns());
  CHECK(WifiServiceTestAccess::local_http_active(f.svc));
  const int starts_after_failure = f.hal.start_mdns_calls;

  f.hal.start_mdns_status = WifiStatus::Ok;
  CHECK(f.svc.ensure_local_mdns());
  CHECK(f.hal.start_mdns_calls == starts_after_failure + 1);
  CHECK(WifiServiceTestAccess::local_http_active(f.svc));
}

TEST_CASE("local endpoint teardown orders mDNS, listener, then routes",
          "[go_wifi][local_endpoint][teardown]") {
  WifiService::Config config{};
  config.serial_number = "ABCDEF123456";
  config.firmware_version = "4.2.0";
  config.model = "P-1PSG";
  config.hostname = "airgradient-ABCDEF123456";
  Fixture f(config);
  f.seed_network();
  f.svc.connect_with_saved_credentials();
  f.hal.got_ip_cb(0x0100A8C0);
  REQUIRE(f.svc.ensure_local_http());
  REQUIRE(f.svc.ensure_local_mdns());
  std::vector<std::string> calls;
  f.hal.call_log = &calls;
  f.http.call_log = &calls;

  f.svc.shutdown();

  REQUIRE(calls.size() >= 4);
  CHECK(calls[0] == "mdns_stop");
  CHECK(calls[1] == "http_stop");
  CHECK(calls[2] == "route_end");
  CHECK(calls.back() == "wifi_off");
  CHECK_FALSE(WifiServiceTestAccess::local_http_active(f.svc));
}

TEST_CASE("provisioning success handoff retains listener for local routes",
          "[go_wifi][provisioning][local_endpoint][handoff]") {
  WifiService::Config config{};
  config.ap_ssid = "airgradient-ABCDEF123456";
  config.hostname = "airgradient-ABCDEF123456";
  config.http_port = 80;
  Fixture f(config);

  f.svc.start_provisioning(ProvisioningTransport::WifiOnly);
  REQUIRE(WifiServiceTestAccess::provisioning_active(f.svc));
  REQUIRE(f.http.start_calls == 1);
  REQUIRE(f.http.active_routes > 0);
  CHECK(f.http.last_port == 80);
  CHECK(f.hal.last_mdns_hostname == "airgradient-ABCDEF123456");
  const int stop_calls_before_handoff = f.http.stop_calls;

  f.svc.stop_provisioning(/*stop_http_server=*/false);
  CHECK(f.http.stop_calls == stop_calls_before_handoff);
  CHECK(f.http.unregister_all_calls == 1);
  CHECK(f.http.active_routes == 0);
  CHECK(f.svc.ensure_local_http());
  CHECK(f.http.start_calls == 2);
  CHECK(f.http.active_routes == LOCAL_API_ROUTE_COUNT);
  CHECK(WifiServiceTestAccess::local_http_active(f.svc));
}

TEST_CASE("local endpoint survives STA reconnect without route or listener churn",
          "[go_wifi][local_endpoint][reconnect]") {
  WifiService::Config config{};
  config.serial_number = "ABCDEF123456";
  config.firmware_version = "4.2.0";
  config.model = "P-1PSG";
  config.hostname = "airgradient-ABCDEF123456";
  Fixture f(config);
  f.seed_network();
  f.svc.connect_with_saved_credentials();
  REQUIRE(f.hal.sta_connected_cb);
  REQUIRE(f.hal.got_ip_cb);
  f.hal.sta_connected_cb();
  f.hal.got_ip_cb(0x0100A8C0);
  REQUIRE(f.svc.ensure_local_http());
  REQUIRE(f.svc.ensure_local_mdns());
  REQUIRE(f.http.active_routes == LOCAL_API_ROUTE_COUNT);
  REQUIRE(f.hal.start_mdns_calls == 1);

  REQUIRE(f.hal.sta_disconnected_cb);
  f.hal.sta_disconnected_cb(200);
  CHECK(f.hal.stop_mdns_calls == 1);
  CHECK(f.http.stop_calls == 0);
  CHECK(f.http.unregister_calls == 0);
  CHECK(f.http.active_routes == LOCAL_API_ROUTE_COUNT);

  f.hal.sta_connected_cb();
  f.hal.got_ip_cb(0x0200A8C0);
  CHECK(f.hal.start_mdns_calls == 2);
  CHECK(f.svc.ensure_local_http());
  CHECK(f.svc.ensure_local_mdns());
  CHECK(f.http.start_calls == 1);
  CHECK(f.http.register_calls == LOCAL_API_ROUTE_COUNT);
  CHECK(f.http.active_routes == LOCAL_API_ROUTE_COUNT);
}

TEST_CASE("shutdown stops a listener retained by Wi-Fi to BLE provisioning switch",
          "[go_wifi][provisioning][switch][teardown]") {
  WifiService::Config config{};
  config.ap_ssid = "airgradient-ABCDEF123456";
  config.hostname = "airgradient-ABCDEF123456";
  Fixture f(config);

  f.svc.start_provisioning(ProvisioningTransport::WifiOnly);
  REQUIRE(WifiServiceTestAccess::provisioning_active(f.svc));
  REQUIRE(f.http.start_calls == 1);
  const int stop_calls_before_handoff = f.http.stop_calls;

  f.svc.stop_provisioning(/*stop_http_server=*/false);
  CHECK(f.http.stop_calls == stop_calls_before_handoff);
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_transport(f.svc, ProvisioningTransport::BleOnly);

  f.svc.shutdown();
  CHECK(f.http.stop_calls == stop_calls_before_handoff + 1);
}

// ---------------------------------------------------------------------------
// Bug fixes from on-device CP2.4 trace
// ---------------------------------------------------------------------------

TEST_CASE("on_provisioning_event Connected latches online and has_been_online",
          "[go_wifi][provisioning][bugfix]") {
  // Provisioning owns the WifiManager callback slot during a session, so
  // on_got_ip never fires on WifiService. The Connected event must mirror
  // those state updates itself, otherwise has_been_online() stays false
  // and the orchestrator falls back to Portable on the subsequent Stopped.
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  REQUIRE_FALSE(f.svc.is_online());
  REQUIRE_FALSE(f.svc.has_been_online());

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Connected;
  info.ip = 0x0100A8C0;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  CHECK(f.svc.is_online());
  CHECK(f.svc.has_been_online());
  CHECK(f.svc.ip() == 0x0100A8C0);
}

TEST_CASE("on_provisioning_event Started during a transport switch reports the destination",
          "[go_wifi][provisioning][bugfix]") {
  // Regression for the on-device trace: while switching BLE -> Wi-Fi, the
  // Started event fired by the inner _prov->start() must reflect the
  // destination transport. Previously _transport was updated only after
  // start() returned, so Started carried the stale source transport.
  Fixture f;
  WifiServiceTestAccess::set_provisioning_active(f.svc, true);
  WifiServiceTestAccess::set_transport(f.svc, ProvisioningTransport::WifiOnly);
  WifiServiceTestAccess::set_switching_transport(f.svc, true);

  ProvisioningEventInfo info{};
  info.event = ProvisioningEvent::Started;
  WifiServiceTestAccess::on_provisioning_event(f.svc, info);

  const Event *evt = f.rtos.first_event(EventType::ProvisioningStateChanged);
  REQUIRE(evt != nullptr);
  CHECK(evt->prov.event == static_cast<uint8_t>(ProvisioningEvent::Started));
  CHECK(evt->prov.transport == static_cast<uint8_t>(ProvisioningTransport::WifiOnly));
}
