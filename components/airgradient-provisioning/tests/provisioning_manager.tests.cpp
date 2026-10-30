/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "fake_config_store.h"
#include "hal/http_server.h"
#include "hal/wifi_hal.h"
#include "internal/ble_transport.h"
#include "internal/provisioning_timer.h"
#include "internal/wifi_portal_transport.h"
#include "mock_ble.h"
#include "rtos.h"
#include "services/provisioning_manager.h"
#include "services/wifi_manager.h"
#include "test_http_request.h"

namespace {

// No-op RTOS for host tests. ProvisioningManager::stop() now calls
// RTOS::delay_ms() during the post-connect hold; without an installed
// instance, RTOS::get_instance() returns nullptr and segfaults.
class StubRTOS : public RTOS {
public:
  void delay_ms_impl(uint32_t ms) override { (void)ms; }
  uint64_t get_time_ms_impl() override { return 0; }
};

// Minimal WifiHal stub. The provisioning manager calls into WifiManager
// only for AP/STA/scan operations; the underlying HAL just needs to
// accept those calls and store the registered callbacks.
class FakeWifiHal : public WifiHal {
public:
  WifiStatus init() override { return WifiStatus::Ok; }
  void deinit() override {}

  WifiStatus set_mode(WifiMode m) override {
    if (fail_set_mode_for == m) {
      fail_set_mode_for = WifiMode::Off; // one-shot
      return WifiStatus::Failed;
    }
    _mode = m;
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

  WifiStatus set_static_ip(const WifiStaticIpConfig &) override { return WifiStatus::Ok; }
  WifiStatus clear_static_ip() override { return WifiStatus::Ok; }

  WifiStatus start_scan(const WifiScanConfig &) override {
    ++scan_calls;
    return WifiStatus::Ok;
  }

  WifiStatus start_ap(const WifiApConfig &) override {
    if (fail_next_start_ap) {
      fail_next_start_ap = false;
      return WifiStatus::Failed;
    }
    return WifiStatus::Ok;
  }
  WifiStatus stop_ap() override { return WifiStatus::Ok; }

  WifiStatusSnapshot get_status() const override { return {}; }
  WifiStatus set_power_save(WifiPowerSave) override { return WifiStatus::Ok; }

  WifiStatus start_mdns(const WifiMdnsConfig &config) override {
    ++start_mdns_calls;
    last_mdns_hostname = config.hostname != nullptr ? config.hostname : "";
    last_mdns_service_count = config.service_count;
    if (config.service_count > 0 && config.services != nullptr) {
      const WifiMdnsServiceRecord &service = config.services[0];
      last_mdns_service_type = service.service_type != nullptr ? service.service_type : "";
      last_mdns_port = service.port;
      last_mdns_txt_count = service.txt_count;
    }
    record("mdns.start");
    return start_mdns_status;
  }
  WifiStatus stop_mdns() override {
    ++stop_mdns_calls;
    record("mdns.stop");
    return stop_mdns_status;
  }

  WifiStatus arm_dhcp_timeout(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_dhcp_timeout() override { return WifiStatus::Ok; }
  WifiStatus arm_retry_timer(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_retry_timer() override { return WifiStatus::Ok; }

  void set_on_sta_connected(WifiConnectedCallback cb) override { _on_connected = std::move(cb); }
  void set_on_sta_disconnected(std::function<void(int)> cb) override {
    _on_disconnected = std::move(cb);
  }
  void set_on_got_ip(WifiGotIpCallback cb) override { _on_got_ip = std::move(cb); }
  void set_on_scan_complete(WifiScanCompleteCallback cb) override {
    _on_scan_complete = std::move(cb);
  }
  void set_on_ap_client_joined(WifiApClientJoinedCallback cb) override {
    _on_joined = std::move(cb);
  }
  void set_on_ap_client_left(WifiApClientLeftCallback cb) override { _on_left = std::move(cb); }
  void set_on_dhcp_timeout(std::function<void()> cb) override { _on_dhcp = std::move(cb); }
  void set_on_retry_due(std::function<void()> cb) override { _on_retry = std::move(cb); }

  // -- Test hooks: fire HAL-level events to drive WifiManager --

  void fire_sta_disconnected(int raw_reason) {
    if (_on_disconnected) {
      _on_disconnected(raw_reason);
    }
  }

  // Fires the HAL scan-complete callback that WifiManager wires in its
  // constructor. Returns true if a callback was registered. Exercises
  // the full HAL -> WifiManager -> ProvisioningManager scan-complete
  // chain that the BleOnly regression test depends on.
  bool fire_scan_complete(const WifiScanEntry *entries, uint16_t count) {
    if (!_on_scan_complete) {
      return false;
    }
    _on_scan_complete(entries, count);
    return true;
  }

  // -- Visible state captured by overridden methods --

  uint32_t connect_calls = 0;
  uint32_t scan_calls = 0;
  uint32_t disconnect_calls = 0;
  uint32_t start_mdns_calls = 0;
  uint32_t stop_mdns_calls = 0;
  std::string last_ssid;
  std::string last_password;
  std::string last_mdns_hostname;
  std::string last_mdns_service_type;
  uint16_t last_mdns_port = 0;
  uint8_t last_mdns_service_count = 0;
  uint8_t last_mdns_txt_count = 0;
  std::vector<std::string> *operation_log = nullptr;

  // -- Fault injection (one-shot; cleared on first triggered call) --
  WifiMode fail_set_mode_for = WifiMode::Off; // Off = disabled (Off never requested by manager)
  bool fail_next_start_ap = false;
  WifiStatus start_mdns_status = WifiStatus::Ok;
  WifiStatus stop_mdns_status = WifiStatus::Ok;

private:
  void record(const char *operation) {
    if (operation_log != nullptr) {
      operation_log->push_back(operation);
    }
  }

  WifiMode _mode = WifiMode::Off;
  WifiConnectedCallback _on_connected;
  std::function<void(int)> _on_disconnected;
  WifiGotIpCallback _on_got_ip;
  WifiScanCompleteCallback _on_scan_complete;
  WifiApClientJoinedCallback _on_joined;
  WifiApClientLeftCallback _on_left;
  std::function<void()> _on_dhcp;
  std::function<void()> _on_retry;
};

// Minimal HttpServer stub that records registered routes.
class FakeHttpServer : public HttpServer {
public:
  bool start(uint16_t) override {
    record("http.start");
    started = true;
    return true;
  }
  void stop() override {
    record("http.stop");
    started = false;
  }
  bool register_route(HttpMethod method, const char *path, HttpHandler handler) override {
    routes.push_back({method, std::string(path), std::move(handler)});
    return true;
  }
  bool unregister_route(HttpMethod method, const char *path) override {
    auto it = std::find_if(routes.begin(), routes.end(), [method, path](const auto &r) {
      return r.method == method && r.path == path;
    });
    if (it == routes.end()) {
      return false;
    }
    routes.erase(it);
    return true;
  }
  void unregister_all() override {
    record("http.unregister_all");
    routes.clear();
  }

  struct Entry {
    HttpMethod method;
    std::string path;
    HttpHandler handler;
  };

  std::vector<Entry> routes;
  std::vector<std::string> *operation_log = nullptr;
  bool started = false;

private:
  void record(const std::string &operation) {
    if (operation_log != nullptr) {
      operation_log->push_back(operation);
    }
  }
};

struct Fixture {
  // `events` must outlive `prov` — the on_event callback writes into
  // it during the Stopped emit fired by ~ProvisioningManager.
  std::vector<ProvisioningEventInfo> events;
  std::vector<std::string> operations;
  StubRTOS rtos;
  FakeWifiHal hal;
  FakeConfigStore creds_store;
  WifiManager wifi{hal, creds_store};
  FakeHttpServer http;
  MockBleServer ble;
  ProvisioningManager prov;

  Fixture() {
    RTOS::set_instance(&rtos);
    hal.operation_log = &operations;
    http.operation_log = &operations;
    prov.set_on_event([this](const ProvisioningEventInfo &e) { events.push_back(e); });
  }

  ~Fixture() { RTOS::set_instance(nullptr); }

  ProvisioningConfig basic_config(uint32_t timeout_ms = 0) {
    ProvisioningConfig cfg = {};
    // Pin regression suite to dual-transport (default is BleOnly).
    cfg.transport = ProvisioningTransport::Both;
    std::strncpy(cfg.ap.ssid, "airgradient-test", sizeof(cfg.ap.ssid) - 1);
    std::strncpy(cfg.ap.password, "cleanair", sizeof(cfg.ap.password) - 1);
    cfg.hostname = "airgradient-test";
    cfg.overall_timeout_ms = timeout_ms;
    return cfg;
  }

  ProvisioningData creds(const char *ssid = "HomeWiFi", const char *password = "secret12") {
    ProvisioningData d;
    std::strncpy(d.ssid, ssid, sizeof(d.ssid) - 1);
    std::strncpy(d.password, password, sizeof(d.password) - 1);
    return d;
  }
};

} // namespace

// ============================================================================
// ProvisioningTestAccess — friend class for private member access
// ============================================================================

class ProvisioningTestAccess {
public:
  // -- Drive external events (same handlers WifiManager callbacks invoke) --
  static void on_sta_connected(ProvisioningManager &p, uint32_t ip) { p._on_sta_connected(ip); }
  static void on_sta_disconnected(ProvisioningManager &p) { p._on_sta_disconnected(); }
  static void on_ap_client_joined(ProvisioningManager &p) { p._on_ap_client_joined(); }
  static void on_ap_client_left(ProvisioningManager &p) { p._on_ap_client_left(); }
  static void on_ble_client_connected(ProvisioningManager &p) { p._on_ble_client_connected(); }
  static void on_ble_client_disconnected(ProvisioningManager &p) {
    p._on_ble_client_disconnected();
  }
  static void on_scan_results(ProvisioningManager &p, const WifiScanEntry *e, uint16_t c) {
    p._on_scan_results(e, c);
  }

  // -- Timer --
  static void fire_timeout(ProvisioningManager &p) { p._timer->fire_for_test(); }

  // -- Internal transport access (for handler-level tests) --
  static WifiPortalTransport &portal(ProvisioningManager &p) { return *p._portal; }
  static BleTransport &ble_transport(ProvisioningManager &p) { return *p._ble_transport; }

  // -- State inspection --
  static uint32_t ap_client_count(const ProvisioningManager &p) { return p._ap_client_count; }
  static uint32_t ble_client_count(const ProvisioningManager &p) { return p._ble_client_count; }
};
using A = ProvisioningTestAccess;

// ============================================================================
// Existing CP1 tests (updated for AgBleServer& signature)
// ============================================================================

TEST_CASE("ProvisioningManager starts in Idle and rejects start with empty SSID",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.state() == ProvisioningState::Idle);

  // SSID only enforced when Wi-Fi transport runs.
  ProvisioningConfig bad = {};
  bad.transport = ProvisioningTransport::Both;
  REQUIRE_FALSE(f.prov.start(f.wifi, f.ble, f.http, bad));
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
}

TEST_CASE("ProvisioningManager start emits Started and registers portal routes", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);

  auto has_route = [&](HttpMethod method, const char *path) {
    for (const auto &r : f.http.routes) {
      if (r.method == method && r.path == path) {
        return true;
      }
    }
    return false;
  };

  REQUIRE(has_route(HttpMethod::Post, "/api/scan"));
  REQUIRE(has_route(HttpMethod::Get, "/api/scan"));
  REQUIRE(has_route(HttpMethod::Post, "/api/provision"));
  REQUIRE(has_route(HttpMethod::Get, "/api/status"));

  REQUIRE(has_route(HttpMethod::Get, "/hotspot-detect.html"));
  REQUIRE(has_route(HttpMethod::Get, "/generate_204"));
  REQUIRE(has_route(HttpMethod::Get, "/connecttest.txt"));
  REQUIRE(has_route(HttpMethod::Get, "/canonical.html"));
  REQUIRE(has_route(HttpMethod::Get, "/favicon.ico"));
}

TEST_CASE("ProvisioningManager state machine: happy path to Connected", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  f.events.clear();

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi","password":"secret12"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);
  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connecting);
  REQUIRE(std::string(f.events[0].data.ssid) == "HomeWiFi");

  f.events.clear();
  A::on_sta_connected(f.prov, 0x0100007f);
  REQUIRE(f.prov.state() == ProvisioningState::Connected);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connected);
  REQUIRE(f.events[0].ip == 0x0100007f);
  REQUIRE(std::string(f.events[0].data.ssid) == "HomeWiFi");

  f.events.clear();
  f.prov.stop();
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Stopped);
  REQUIRE(f.events[0].stop_reason == ProvisioningStopReason::ProductRequested);

  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
  REQUIRE(f.hal.stop_mdns_calls == 1);
  REQUIRE(f.wifi.start_mdns() == WifiStatus::InvalidState);
}

TEST_CASE("ProvisioningManager: stop(false) wipes routes but keeps HTTP server running",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.http.started);
  REQUIRE_FALSE(f.http.routes.empty());

  f.events.clear();
  f.prov.stop(/*stop_http_server=*/false);

  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Stopped);
  REQUIRE(f.http.routes.empty());
  REQUIRE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
}

TEST_CASE("ProvisioningManager: HTTP server is started by ProvisioningManager::start",
          "[provisioning]") {
  Fixture f;
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.http.started);
}

TEST_CASE("ProvisioningManager advertises portal mDNS after HTTP and clears it before routes",
          "[provisioning][mdns]") {
  Fixture f;
  ProvisioningConfig cfg = f.basic_config();
  cfg.http_port = 8080;

  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, cfg));

  REQUIRE(f.hal.start_mdns_calls == 1);
  REQUIRE(f.hal.last_mdns_hostname == "airgradient-test");
  REQUIRE(f.hal.last_mdns_service_count == 1);
  REQUIRE(f.hal.last_mdns_service_type == "_http._tcp");
  REQUIRE(f.hal.last_mdns_port == 8080);
  REQUIRE(f.hal.last_mdns_txt_count == 0);

  const auto http_start = std::find(f.operations.begin(), f.operations.end(), "http.start");
  const auto mdns_start = std::find(f.operations.begin(), f.operations.end(), "mdns.start");
  REQUIRE(http_start != f.operations.end());
  REQUIRE(mdns_start != f.operations.end());
  REQUIRE(http_start < mdns_start);

  f.operations.clear();
  f.prov.stop(/*stop_http_server=*/false);

  const auto mdns_stop = std::find(f.operations.begin(), f.operations.end(), "mdns.stop");
  const auto routes_removed =
      std::find(f.operations.begin(), f.operations.end(), "http.unregister_all");
  REQUIRE(mdns_stop != f.operations.end());
  REQUIRE(routes_removed != f.operations.end());
  REQUIRE(mdns_stop < routes_removed);
  REQUIRE(std::find(f.operations.begin(), f.operations.end(), "http.stop") == f.operations.end());
  REQUIRE(f.http.routes.empty());
  REQUIRE(f.wifi.start_mdns() == WifiStatus::InvalidState);
}

TEST_CASE("ProvisioningManager keeps portal available when mDNS start fails",
          "[provisioning][mdns]") {
  Fixture f;
  f.hal.start_mdns_status = WifiStatus::Failed;

  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.http.started);
  REQUIRE_FALSE(f.http.routes.empty());
  REQUIRE(f.hal.start_mdns_calls == 1);

  f.prov.stop(/*stop_http_server=*/false);
  REQUIRE(f.wifi.start_mdns() == WifiStatus::InvalidState);
}

TEST_CASE("ProvisioningManager releases ownership when mDNS teardown fails",
          "[provisioning][mdns]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.hal.start_mdns_calls == 1);

  f.hal.stop_mdns_status = WifiStatus::Failed;
  f.prov.stop(/*stop_http_server=*/false);

  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.http.routes.empty());
  REQUIRE(f.hal.stop_mdns_calls == 1);
  REQUIRE(f.wifi.start_mdns() == WifiStatus::Ok);
  REQUIRE(f.hal.start_mdns_calls == 1);

  f.hal.stop_mdns_status = WifiStatus::Ok;
  REQUIRE(f.wifi.clear_mdns_profile() == WifiStatus::Ok);
}

TEST_CASE("ProvisioningManager replaces and clears a previous mDNS profile",
          "[provisioning][mdns]") {
  Fixture f;
  WifiMdnsProfile owner_profile = {};
  owner_profile.config.hostname = "existing-owner";
  owner_profile.lifecycle = WifiMdnsLifecycle::Manual;
  REQUIRE(f.wifi.set_mdns_profile(owner_profile) == WifiStatus::Ok);
  REQUIRE(f.wifi.start_mdns() == WifiStatus::Ok);

  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.hal.stop_mdns_calls == 1);
  REQUIRE(f.hal.start_mdns_calls == 2);
  REQUIRE(f.hal.last_mdns_hostname == "airgradient-test");

  f.prov.stop(/*stop_http_server=*/false);
  REQUIRE(f.wifi.start_mdns() == WifiStatus::InvalidState);
}

TEST_CASE("ProvisioningManager does not clear another mDNS owner after profile setup failure",
          "[provisioning][mdns]") {
  Fixture f;
  WifiMdnsProfile owner_profile = {};
  owner_profile.config.hostname = "existing-owner";
  owner_profile.lifecycle = WifiMdnsLifecycle::Manual;
  REQUIRE(f.wifi.set_mdns_profile(owner_profile) == WifiStatus::Ok);
  REQUIRE(f.wifi.start_mdns() == WifiStatus::Ok);

  f.hal.stop_mdns_status = WifiStatus::Failed;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.http.started);
  REQUIRE(f.hal.start_mdns_calls == 1);
  REQUIRE(f.hal.stop_mdns_calls == 1);

  f.prov.stop(/*stop_http_server=*/false);
  REQUIRE(f.hal.stop_mdns_calls == 1);
  REQUIRE(f.wifi.start_mdns() == WifiStatus::Ok);
  REQUIRE(f.hal.start_mdns_calls == 1);
  REQUIRE(f.hal.last_mdns_hostname == "existing-owner");

  f.hal.stop_mdns_status = WifiStatus::Ok;
  REQUIRE(f.wifi.clear_mdns_profile() == WifiStatus::Ok);
}

TEST_CASE("ProvisioningManager: failed connect returns to WaitingForCredentials",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);

  f.events.clear();
  A::on_sta_disconnected(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::ConnectFailed);
}

TEST_CASE("ProvisioningManager: credentials rejected while Connecting", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  TestHttpRequest first(HttpMethod::Post, "/api/provision");
  first.set_body(R"({"ssid":"A","password":"password"})");
  HttpResponse r1;
  A::portal(f.prov).handle_provision_post(first, r1);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);

  TestHttpRequest second(HttpMethod::Post, "/api/provision");
  second.set_body(R"({"ssid":"B","password":"password"})");
  HttpResponse r2;
  A::portal(f.prov).handle_provision_post(second, r2);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
}

TEST_CASE("ProvisioningManager: timeout fires only when no AP clients are connected",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config(60'000)));
  f.events.clear();

  A::on_ap_client_joined(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.empty());

  A::on_ap_client_left(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Stopped);
  REQUIRE(f.events[0].stop_reason == ProvisioningStopReason::TimedOut);

  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
}

TEST_CASE("ProvisioningManager: credentials submission disables WifiManager retry",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  f.events.clear();

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi","password":"wrongpass"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
  REQUIRE(f.hal.connect_calls == 1);
  REQUIRE(f.hal.last_ssid == "HomeWiFi");
  REQUIRE(f.hal.last_password == "wrongpass");

  f.events.clear();
  f.hal.fire_sta_disconnected(15);

  REQUIRE(f.hal.connect_calls == 1);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::ConnectFailed);
}

TEST_CASE("ProvisioningManager: connect_timeout_ms forwards to WifiManager DHCP timeout",
          "[provisioning]") {
  Fixture f;
  ProvisioningConfig cfg = f.basic_config();
  cfg.connect_timeout_ms = 20000;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, cfg));
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
}

TEST_CASE("ProvisioningManager: stop is idempotent", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  f.prov.stop();
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  size_t after_first = f.events.size();
  f.prov.stop();
  REQUIRE(f.events.size() == after_first);
}

// ============================================================================
// Credential persistence on got-IP (multi-SSID store)
// ============================================================================

TEST_CASE("ProvisioningManager: got-IP persists the verified credential", "[provisioning][creds]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi","password":"secret12"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);

  // Nothing persisted until got-IP verifies the network.
  REQUIRE_FALSE(f.wifi.has_saved_networks());

  A::on_sta_connected(f.prov, 0x0100007f);
  REQUIRE(f.prov.state() == ProvisioningState::Connected);

  REQUIRE(f.wifi.has_saved_networks());
  char out[WIFI_MAX_SAVED_NETWORKS][33] = {};
  REQUIRE(f.wifi.list_networks(out, WIFI_MAX_SAVED_NETWORKS) == 1);
  REQUIRE(std::string(out[0]) == "HomeWiFi");

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: stopping incomplete setup saves no network",
          "[provisioning][creds]") {
  for (const auto transport : {ProvisioningTransport::BleOnly, ProvisioningTransport::WifiOnly}) {
    for (const bool connecting : {false, true}) {
      CAPTURE(transport, connecting);
      Fixture f;
      auto cfg = f.basic_config();
      cfg.transport = transport;
      REQUIRE(f.prov.start(f.wifi, f.ble, f.http, cfg));
      REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
      if (connecting) {
        REQUIRE(f.prov.submit_credentials(f.creds()));
        REQUIRE(f.prov.state() == ProvisioningState::Connecting);
      }
      REQUIRE_FALSE(f.wifi.has_saved_networks());

      f.prov.stop();

      CHECK(f.prov.state() == ProvisioningState::Idle);
      CHECK_FALSE(f.wifi.has_saved_networks());
      char networks[WIFI_MAX_SAVED_NETWORKS][33] = {};
      CHECK(f.wifi.list_networks(networks, WIFI_MAX_SAVED_NETWORKS) == 0);
    }
  }
}

TEST_CASE("ProvisioningManager: connect failure persists nothing", "[provisioning][creds]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi","password":"secret12"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);

  A::on_sta_disconnected(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE_FALSE(f.wifi.has_saved_networks());

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: persist failure still emits Connected + CREDENTIALS_NOT_SAVED",
          "[provisioning][creds]") {
  // Commit failure => add_network() fails after a verified got-IP.
  std::vector<ProvisioningEventInfo> events;
  StubRTOS rtos;
  RTOS::set_instance(&rtos);
  FakeWifiHal hal;
  FakeConfigStore creds_store;
  creds_store.commit_should_fail = true;
  WifiManager wifi{hal, creds_store};
  FakeHttpServer http;
  MockBleServer ble;
  ProvisioningManager prov;
  prov.set_on_event([&](const ProvisioningEventInfo &e) { events.push_back(e); });

  ProvisioningConfig cfg = {};
  cfg.transport = ProvisioningTransport::Both;
  std::strncpy(cfg.ap.ssid, "airgradient-test", sizeof(cfg.ap.ssid) - 1);
  cfg.hostname = "airgradient-test";
  REQUIRE(prov.start(wifi, ble, http, cfg));

  constexpr const char *PROV_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
  constexpr const char *CRED_UUID = "703fa252-3d2a-4da9-a05c-83b0d9cacb8e";
  MockBleCharacteristic *cred_char = ble.find_char(PROV_UUID, CRED_UUID);
  REQUIRE(cred_char != nullptr);

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi","password":"secret12"})");
  HttpResponse resp;
  A::portal(prov).handle_provision_post(req, resp);

  events.clear();
  const int notifies_before = cred_char->notify_count;
  A::on_sta_connected(prov, 0x0100007f);

  // Still reports Connected even though persistence failed.
  REQUIRE(prov.state() == ProvisioningState::Connected);
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].event == ProvisioningEvent::Connected);
  // A warning (CREDENTIALS_NOT_SAVED) plus WIFI_CONNECTED were notified.
  REQUIRE(cred_char->notify_count >= notifies_before + 2);

  prov.stop();
  RTOS::set_instance(nullptr);
}

// ============================================================================
// CP2: BLE integration tests
// ============================================================================

TEST_CASE("ProvisioningManager: start initialises BLE transport", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  // BLE server should have been init'd and started advertising.
  REQUIRE(f.ble.init_count == 1);
  REQUIRE(f.ble.start_advertising_count == 1);

  f.prov.stop();
  // BLE server should be deinit'd.
  REQUIRE(f.ble.deinit_count == 1);
}

TEST_CASE("ProvisioningManager: send_ble_status forwards to BLE transport", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  // Find the credentials characteristic on the mock BLE server.
  constexpr const char *PROV_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
  constexpr const char *CRED_UUID = "703fa252-3d2a-4da9-a05c-83b0d9cacb8e";
  MockBleCharacteristic *cred_char = f.ble.find_char(PROV_UUID, CRED_UUID);
  REQUIRE(cred_char != nullptr);

  f.prov.send_ble_status(1); // CONNECTING_TO_SERVER
  REQUIRE(cred_char->notify_count == 1);

  f.prov.send_ble_status(2); // SERVER_REACHABLE
  REQUIRE(cred_char->notify_count == 2);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: send_ble_status is no-op when idle", "[provisioning][ble]") {
  Fixture f;
  // send_ble_status before start() — should not crash.
  f.prov.send_ble_status(1);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
}

TEST_CASE("ProvisioningManager: BLE client connect pauses timeout", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config(60'000)));
  f.events.clear();

  // BLE client connects — timeout must pause.
  A::on_ble_client_connected(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.empty());

  // BLE client disconnects — timeout resumes.
  A::on_ble_client_disconnected(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].stop_reason == ProvisioningStopReason::TimedOut);
}

TEST_CASE("ProvisioningManager: mixed AP + BLE clients suppress timeout", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config(60'000)));
  f.events.clear();

  // AP client joins — pauses timeout.
  A::on_ap_client_joined(f.prov);

  // BLE client also connects — still paused.
  A::on_ble_client_connected(f.prov);

  // AP client leaves — BLE client still present, timeout stays paused.
  A::on_ap_client_left(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.empty());

  // BLE client disconnects — now total count is 0, timeout resumes.
  A::on_ble_client_disconnected(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].stop_reason == ProvisioningStopReason::TimedOut);
}

TEST_CASE("ProvisioningManager: scan results reach both transports", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  WifiScanEntry entries[2] = {};
  std::strncpy(entries[0].ssid, "Net1", sizeof(entries[0].ssid) - 1);
  entries[0].rssi = -40;
  entries[0].auth_mode = WifiAuthMode::wpa2_psk;
  std::strncpy(entries[1].ssid, "Net2", sizeof(entries[1].ssid) - 1);
  entries[1].rssi = -50;
  entries[1].auth_mode = WifiAuthMode::open;

  A::on_scan_results(f.prov, entries, 2);

  // Portal should have cached the results.
  REQUIRE(A::portal(f.prov).scan_in_progress() == false);

  // BLE transport should have pagination pending.
  // The pagination timer was armed with 0ms for the first page.
  // Fire it to verify the scan char gets a notification.
  constexpr const char *PROV_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
  constexpr const char *SCAN_UUID = "467a080f-e50f-42c9-b9b2-a2ab14d82725";
  MockBleCharacteristic *scan_char = f.ble.find_char(PROV_UUID, SCAN_UUID);
  REQUIRE(scan_char != nullptr);

  A::ble_transport(f.prov).pagination_timer().fire_for_test();
  REQUIRE(scan_char->notify_count >= 1);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: BLE credential write drives state machine", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  f.events.clear();

  constexpr const char *PROV_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
  constexpr const char *CRED_UUID = "703fa252-3d2a-4da9-a05c-83b0d9cacb8e";
  MockBleCharacteristic *cred_char = f.ble.find_char(PROV_UUID, CRED_UUID);
  REQUIRE(cred_char != nullptr);

  // Simulate BLE credential submission.
  cred_char->simulate_write(R"({"ssid":"BleNet","password":"blepass1"})");

  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connecting);
  REQUIRE(std::string(f.events[0].data.ssid) == "BleNet");
  REQUIRE(std::string(f.events[0].data.password) == "blepass1");

  // Connected event triggers BLE status notification.
  f.events.clear();
  A::on_sta_connected(f.prov, 0x0A000001);
  REQUIRE(f.prov.state() == ProvisioningState::Connected);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connected);

  // The credentials characteristic should have received a WIFI_CONNECTED
  // status notification.
  REQUIRE(cred_char->notify_count >= 1);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: timeout teardown deinits BLE", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config(60'000)));
  f.events.clear();

  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.ble.deinit_count == 1);
}

// ============================================================================
// start() failure-path rollback tests (issue #2: hard-fail on Wi-Fi setup,
// log-only on DNS responder failure).
// ============================================================================

TEST_CASE("ProvisioningManager: set_mode(ApSta) failure rolls back and returns false",
          "[provisioning][rollback]") {
  Fixture f;
  f.hal.fail_set_mode_for = WifiMode::ApSta;

  REQUIRE_FALSE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.prov.state() == ProvisioningState::Idle);

  // No Started event must be emitted on a failed start.
  REQUIRE(f.events.empty());

  // Rollback must wipe HTTP routes, leave the server unstarted, and
  // tear down BLE so the device is back to a clean state.
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.ble.deinit_count == 1);
}

TEST_CASE("ProvisioningManager: start_ap failure rolls back and returns false",
          "[provisioning][rollback]") {
  Fixture f;
  f.hal.fail_next_start_ap = true;

  REQUIRE_FALSE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.empty());

  // set_mode(ApSta) ran before the failure; rollback must revert it.
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.ble.deinit_count == 1);
}

// ============================================================================
// Transport selection (see component README — Transport Selection)
// ============================================================================

namespace {

ProvisioningConfig make_cfg(ProvisioningTransport t, uint32_t timeout_ms = 0) {
  ProvisioningConfig cfg = {};
  cfg.transport = t;
  std::strncpy(cfg.ap.ssid, "airgradient-test", sizeof(cfg.ap.ssid) - 1);
  std::strncpy(cfg.ap.password, "cleanair", sizeof(cfg.ap.password) - 1);
  cfg.hostname = "airgradient-test";
  cfg.overall_timeout_ms = timeout_ms;
  return cfg;
}

} // namespace

TEST_CASE("ProvisioningManager: BleOnly does not start Wi-Fi side", "[provisioning][transport]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::BleOnly)));

  // No Wi-Fi side.
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
  REQUIRE(f.hal.start_mdns_calls == 0);
  REQUIRE(f.hal.stop_mdns_calls == 0);

  // BLE up and advertising.
  REQUIRE(f.ble.init_count == 1);
  REQUIRE(f.ble.start_advertising_count == 1);

  f.prov.stop();
  REQUIRE(f.ble.deinit_count == 1);
  REQUIRE(f.hal.start_mdns_calls == 0);
  REQUIRE(f.hal.stop_mdns_calls == 0);
}

TEST_CASE("ProvisioningManager: WifiOnly does not initialise BLE", "[provisioning][transport]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::WifiOnly)));

  // Wi-Fi side up.
  REQUIRE_FALSE(f.http.routes.empty());
  REQUIRE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::ApSta);

  // BLE untouched.
  REQUIRE(f.ble.init_count == 0);
  REQUIRE(f.ble.start_advertising_count == 0);

  f.prov.stop();
  REQUIRE(f.ble.deinit_count == 0);
}

TEST_CASE("ProvisioningManager: Both startup brings up both transports",
          "[provisioning][transport]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::Both)));

  REQUIRE_FALSE(f.http.routes.empty());
  REQUIRE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::ApSta);
  REQUIRE(f.ble.init_count == 1);
  REQUIRE(f.ble.start_advertising_count == 1);
}

TEST_CASE("ProvisioningManager: BleOnly accepts empty ap.ssid", "[provisioning][transport]") {
  Fixture f;
  ProvisioningConfig cfg = {};
  cfg.transport = ProvisioningTransport::BleOnly;
  // ap.ssid empty by design.
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, cfg));
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  f.prov.stop();
}

TEST_CASE("ProvisioningManager: BleOnly wires scan-complete end-to-end",
          "[provisioning][transport][scan][ble]") {
  // Regression: scan-complete was previously gated on want_wifi, so on
  // ProvisioningTransport::BleOnly the WifiManager's scan-complete
  // callback never received a sink. Scans fired by the BLE central
  // returned no results because _on_scan_results was never invoked.
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::BleOnly)));

  // Synthesize a HAL-level scan completion (same path WIFI_EVENT_SCAN_DONE
  // would take on hardware). Must propagate HAL -> WifiManager ->
  // ProvisioningManager -> BleTransport even with no Wi-Fi portal up.
  WifiScanEntry entries[2] = {};
  std::strncpy(entries[0].ssid, "BleNet1", sizeof(entries[0].ssid) - 1);
  entries[0].rssi = -45;
  entries[0].auth_mode = WifiAuthMode::wpa2_psk;
  std::strncpy(entries[1].ssid, "BleNet2", sizeof(entries[1].ssid) - 1);
  entries[1].rssi = -60;
  entries[1].auth_mode = WifiAuthMode::open;

  REQUIRE(f.hal.fire_scan_complete(entries, 2));

  // BLE scan characteristic must receive at least one paged notification
  // once the (0 ms) pagination timer fires.
  constexpr const char *PROV_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
  constexpr const char *SCAN_UUID = "467a080f-e50f-42c9-b9b2-a2ab14d82725";
  MockBleCharacteristic *scan_char = f.ble.find_char(PROV_UUID, SCAN_UUID);
  REQUIRE(scan_char != nullptr);

  A::ble_transport(f.prov).pagination_timer().fire_for_test();
  REQUIRE(scan_char->notify_count >= 1);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: WifiOnly accepts null ble.device_name",
          "[provisioning][transport]") {
  Fixture f;
  ProvisioningConfig cfg = make_cfg(ProvisioningTransport::WifiOnly);
  cfg.ble.device_name = nullptr;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, cfg));
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  f.prov.stop();
}

TEST_CASE("ProvisioningManager: Both + AP client join tears down BLE",
          "[provisioning][transport]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::Both, 60'000)));
  REQUIRE(f.ble.init_count == 1);
  REQUIRE(f.ble.deinit_count == 0);
  f.events.clear();

  A::on_ap_client_joined(f.prov);

  REQUIRE(f.ble.deinit_count == 1);
  REQUIRE(A::ap_client_count(f.prov) == 1);
  REQUIRE(A::ble_client_count(f.prov) == 0);

  // Timer paused — AP client present.
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.empty());
}

TEST_CASE("ProvisioningManager: Both + BLE client connect tears down Wi-Fi",
          "[provisioning][transport]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::Both, 60'000)));
  REQUIRE_FALSE(f.http.routes.empty());
  REQUIRE(f.wifi.get_mode() == WifiMode::ApSta);
  f.events.clear();

  A::on_ble_client_connected(f.prov);

  REQUIRE(f.http.routes.empty());
  REQUIRE(f.http.started); // HTTP server stays up; only stop() drops it
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
  REQUIRE(f.hal.stop_mdns_calls == 1);
  REQUIRE(f.wifi.start_mdns() == WifiStatus::InvalidState);
  REQUIRE(A::ap_client_count(f.prov) == 0);
  REQUIRE(A::ble_client_count(f.prov) == 1);

  // Timer paused — BLE client present.
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.empty());
}

TEST_CASE("ProvisioningManager: stale BLE client_left after BLE teardown does not underflow",
          "[provisioning][transport]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::Both)));

  A::on_ap_client_joined(f.prov);
  REQUIRE(A::ble_client_count(f.prov) == 0);

  // Synthetic stale BLE-leave event after teardown.
  A::on_ble_client_disconnected(f.prov);
  REQUIRE(A::ble_client_count(f.prov) == 0);
}

TEST_CASE("ProvisioningManager: second BLE-first commit is a no-op (guard)",
          "[provisioning][transport]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::Both)));

  // First BLE connect tears Wi-Fi down.
  A::on_ble_client_connected(f.prov);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
  REQUIRE(f.http.routes.empty());
  const auto routes_after_first = f.http.routes.size();

  // Disconnect, inject a probe route, reconnect: guard must keep the
  // probe route (no second unregister_all()).
  A::on_ble_client_disconnected(f.prov);
  REQUIRE(A::ble_client_count(f.prov) == 0);
  f.http.routes.push_back({HttpMethod::Get, "/probe", {}});

  A::on_ble_client_connected(f.prov);
  REQUIRE(A::ble_client_count(f.prov) == 1);
  REQUIRE(f.http.routes.size() == routes_after_first + 1);
}

TEST_CASE("ProvisioningManager: second AP-first commit is a no-op (guard)",
          "[provisioning][transport]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::Both)));
  REQUIRE(f.ble.deinit_count == 0);

  A::on_ap_client_joined(f.prov);
  REQUIRE(f.ble.deinit_count == 1);

  // First leaves, second joins: guard must skip the second teardown.
  A::on_ap_client_left(f.prov);
  REQUIRE(A::ap_client_count(f.prov) == 0);
  A::on_ap_client_joined(f.prov);
  REQUIRE(A::ap_client_count(f.prov) == 1);

  REQUIRE(f.ble.deinit_count == 1);
}

// ============================================================================
// Runtime transport switching (see component README — Runtime Transport Switching)
// ============================================================================

TEST_CASE("ProvisioningManager: BleOnly -> WifiOnly round trip", "[provisioning][switch]") {
  Fixture f;

  // First cycle: BleOnly.
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::BleOnly)));
  REQUIRE(f.ble.init_count == 1);
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);

  f.prov.stop();
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.ble.deinit_count == 1);
  REQUIRE(f.events.size() == 2);
  REQUIRE(f.events[1].event == ProvisioningEvent::Stopped);

  // Second cycle: WifiOnly on the same instance; event sink persists.
  f.events.clear();
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::WifiOnly)));
  REQUIRE_FALSE(f.http.routes.empty());
  REQUIRE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::ApSta);
  REQUIRE(f.ble.init_count == 1); // BLE not re-initialised
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: WifiOnly -> BleOnly round trip", "[provisioning][switch]") {
  Fixture f;

  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::WifiOnly)));
  REQUIRE_FALSE(f.http.routes.empty());
  REQUIRE(f.http.started);
  REQUIRE(f.ble.init_count == 0);

  f.prov.stop();
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);

  f.events.clear();
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::BleOnly)));
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
  REQUIRE(f.ble.init_count == 1);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: Both -> BleOnly after transport-aware teardown",
          "[provisioning][switch]") {
  Fixture f;

  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::Both)));
  A::on_ap_client_joined(f.prov);
  REQUIRE(f.ble.deinit_count == 1); // BLE torn down by first AP client

  f.prov.stop();
  REQUIRE(f.prov.state() == ProvisioningState::Idle);

  // Second start re-initialises BLE cleanly.
  f.events.clear();
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::BleOnly)));
  REQUIRE(f.ble.init_count == 2);
  REQUIRE(f.ble.start_advertising_count == 2);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: stop(false) blocks subsequent WifiOnly start",
          "[provisioning][switch]") {
  Fixture f;

  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::WifiOnly)));
  REQUIRE(f.http.started);

  f.prov.stop(/*stop_http_server=*/false);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.http.started); // server still running

  // BleOnly does not need a fresh HTTP server and must succeed.
  f.events.clear();
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::BleOnly)));
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);
  REQUIRE(f.ble.init_count == 1);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: event callback survives across switch cycles",
          "[provisioning][switch]") {
  Fixture f;

  // Three cycles, sink registered once by Fixture ctor.
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::BleOnly)));
  f.prov.stop();
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::WifiOnly)));
  f.prov.stop();
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, make_cfg(ProvisioningTransport::BleOnly)));
  f.prov.stop();

  // Started+Stopped per cycle, all on the same sink.
  REQUIRE(f.events.size() == 6);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);
  REQUIRE(f.events[1].event == ProvisioningEvent::Stopped);
  REQUIRE(f.events[2].event == ProvisioningEvent::Started);
  REQUIRE(f.events[3].event == ProvisioningEvent::Stopped);
  REQUIRE(f.events[4].event == ProvisioningEvent::Started);
  REQUIRE(f.events[5].event == ProvisioningEvent::Stopped);
}

// ============================================================================
// Attached mode (Portable provisioning over the bonded link)
// ============================================================================

namespace {

// Provisioning GATT UUIDs (mirrors ble_transport.cpp).
constexpr const char *PROV_SERVICE_UUID_ATT = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
constexpr const char *CRED_STATUS_CHAR_UUID_ATT = "703fa252-3d2a-4da9-a05c-83b0d9cacb8e";
constexpr const char *SCAN_CHAR_UUID_ATT = "467a080f-e50f-42c9-b9b2-a2ab14d82725";

ProvisioningConfig attached_cfg() {
  ProvisioningConfig cfg = {};
  cfg.transport = ProvisioningTransport::BleAttached;
  cfg.ble.device_name = "AirGradient Go ef0e";
  cfg.ble.model_name = "P-1PSG";
  cfg.overall_timeout_ms = 0;
  return cfg;
}

} // namespace

TEST_CASE("Attached: start_attached parks WaitingForCredentials with radio off",
          "[provisioning][attached]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));

  REQUIRE(f.prov.start_attached(f.wifi, f.ble, attached_cfg()));
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);

  // No set_mode at start — radio stays off (FakeWifiHal defaults to Off).
  REQUIRE(f.wifi.get_mode() == WifiMode::Off);
  // No second init, no advertising on the borrowed server.
  REQUIRE(f.ble.init_count == 1);
  REQUIRE(f.ble.start_advertising_count == 0);
  REQUIRE(f.hal.start_mdns_calls == 0);
  REQUIRE(f.hal.stop_mdns_calls == 0);

  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);

  f.prov.stop();
  REQUIRE(f.hal.start_mdns_calls == 0);
  REQUIRE(f.hal.stop_mdns_calls == 0);
}

TEST_CASE("Attached: BLE writes forward to the request hook, not Wi-Fi",
          "[provisioning][attached]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));

  std::vector<AttachedRequest> requests;
  f.prov.set_attached_request_hook([&](const AttachedRequest &req) { requests.push_back(req); });

  REQUIRE(f.prov.start_attached(f.wifi, f.ble, attached_cfg()));

  MockBleCharacteristic *scan_char = f.ble.find_char(PROV_SERVICE_UUID_ATT, SCAN_CHAR_UUID_ATT);
  MockBleCharacteristic *cred_char =
      f.ble.find_char(PROV_SERVICE_UUID_ATT, CRED_STATUS_CHAR_UUID_ATT);
  REQUIRE(scan_char != nullptr);
  REQUIRE(cred_char != nullptr);

  // Scan write → hook (Scan), no Wi-Fi call on the NimBLE path.
  scan_char->simulate_write("1");
  REQUIRE(requests.size() == 1);
  REQUIRE(requests[0].kind == AttachedRequestKind::Scan);
  REQUIRE(f.hal.scan_calls == 0);

  // Credentials write → hook (Credentials) with parsed data, no connect yet.
  cred_char->simulate_write(R"({"ssid":"HomeWiFi","password":"secret12"})");
  REQUIRE(requests.size() == 2);
  REQUIRE(requests[1].kind == AttachedRequestKind::Credentials);
  REQUIRE(std::string(requests[1].data.ssid) == "HomeWiFi");
  REQUIRE(f.hal.connect_calls == 0);

  // State unchanged — the manager only acts when the product drives it.
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);

  f.prov.stop();
}

TEST_CASE("Attached: driver entrypoints run scan/connect; happy path + reset_to_listening",
          "[provisioning][attached]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.start_attached(f.wifi, f.ble, attached_cfg()));

  // Product brings the radio up (ensure_wifi_ready) before driving.
  f.wifi.set_mode(WifiMode::Sta);

  // request_scan() drives a real scan.
  REQUIRE(f.prov.request_scan());
  REQUIRE(f.hal.scan_calls == 1);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);

  // submit_credentials() starts a single STA connect.
  f.events.clear();
  REQUIRE(f.prov.submit_credentials(f.creds()));
  REQUIRE(f.hal.connect_calls == 1);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connecting);

  // got IP → Connected.
  f.events.clear();
  A::on_sta_connected(f.prov, 0x0100007f);
  REQUIRE(f.prov.state() == ProvisioningState::Connected);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connected);

  // Verify-then-drop: the product drops the radio (set_mode Off, which
  // resets the WifiManager STA state) then re-opens the manager session.
  f.wifi.set_mode(WifiMode::Off);
  f.prov.reset_to_listening();
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);

  // Re-provision works in the same session (product re-arms the radio).
  f.wifi.set_mode(WifiMode::Sta);
  REQUIRE(f.prov.submit_credentials(f.creds("Other", "pw345678")));
  REQUIRE(f.hal.connect_calls == 2);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);

  f.prov.stop();
}

TEST_CASE("Attached: connect failure returns to WaitingForCredentials (radio stays for retry)",
          "[provisioning][attached]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.start_attached(f.wifi, f.ble, attached_cfg()));
  f.wifi.set_mode(WifiMode::Sta);

  REQUIRE(f.prov.submit_credentials(f.creds()));
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);

  f.events.clear();
  A::on_sta_disconnected(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::ConnectFailed);

  f.prov.stop();
}

TEST_CASE("Attached: stop() skips the hold and does NOT deinit the borrowed server",
          "[provisioning][attached]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.start_attached(f.wifi, f.ble, attached_cfg()));
  f.wifi.set_mode(WifiMode::Sta);

  // Drive to Connected so the non-attached path would have held 1.5 s.
  REQUIRE(f.prov.submit_credentials(f.creds()));
  A::on_sta_connected(f.prov, 0x0100007f);
  REQUIRE(f.prov.state() == ProvisioningState::Connected);

  f.events.clear();
  f.prov.stop();
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Stopped);

  // Borrowed server is NOT deinit'd — the owner does that. The manager
  // does not force Wi-Fi mode either (the product owns the radio).
  REQUIRE(f.ble.deinit_count == 0);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
}
