/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>

#include <cstring>

#include "../hal/wifi_hal.h"
#include "../services/wifi_manager.h"
#include "fake_config_store.h"

namespace {

class FakeWifiHal : public WifiHal {
public:
  // -- Lifecycle --
  WifiStatus init() override { return WifiStatus::Ok; }
  void deinit() override {}

  WifiStatus set_mode(WifiMode mode) override {
    set_mode_calls += 1;
    last_mode_set = mode;
    _mode = mode;
    return WifiStatus::Ok;
  }
  WifiMode get_mode() const override { return _mode; }

  WifiStatus connect_sta(const char *ssid, const char *password) override {
    connect_calls += 1;
    last_ssid = ssid != nullptr ? ssid : "";
    last_password = password != nullptr ? password : "";
    return connect_status;
  }
  WifiStatus disconnect_sta() override {
    disconnect_calls += 1;
    return WifiStatus::Ok;
  }

  WifiStatus set_static_ip(const WifiStaticIpConfig &) override {
    set_static_ip_calls += 1;
    return WifiStatus::Ok;
  }
  WifiStatus clear_static_ip() override {
    clear_static_ip_calls += 1;
    return WifiStatus::Ok;
  }

  WifiStatus start_scan(const WifiScanConfig &) override {
    scan_calls += 1;
    return scan_status;
  }

  WifiStatus start_ap(const WifiApConfig &) override {
    start_ap_calls += 1;
    return WifiStatus::Ok;
  }
  WifiStatus stop_ap() override { return WifiStatus::Ok; }

  WifiStatusSnapshot get_status() const override {
    WifiStatusSnapshot snap;
    snap.mode = _mode;
    return snap;
  }

  WifiStatus set_power_save(WifiPowerSave) override { return WifiStatus::Ok; }

  WifiStatus start_mdns(const WifiMdnsConfig &cfg) override {
    start_mdns_calls += 1;
    last_mdns_hostname = (cfg.hostname != nullptr) ? cfg.hostname : "";
    last_mdns_service_type = (cfg.service_count > 0 && cfg.services != nullptr &&
                              cfg.services[0].service_type != nullptr)
                                 ? cfg.services[0].service_type
                                 : "";
    return start_mdns_status;
  }
  WifiStatus stop_mdns() override {
    stop_mdns_calls += 1;
    return stop_mdns_status;
  }

  WifiStatus arm_dhcp_timeout(uint32_t timeout_ms) override {
    dhcp_armed_calls += 1;
    last_dhcp_timeout_ms = timeout_ms;
    return WifiStatus::Ok;
  }
  WifiStatus cancel_dhcp_timeout() override {
    dhcp_cancel_calls += 1;
    return WifiStatus::Ok;
  }
  WifiStatus arm_retry_timer(uint32_t delay_ms) override {
    retry_armed_calls += 1;
    last_retry_delay_ms = delay_ms;
    return WifiStatus::Ok;
  }
  WifiStatus cancel_retry_timer() override {
    retry_cancel_calls += 1;
    return WifiStatus::Ok;
  }

  void set_on_sta_connected(WifiConnectedCallback cb) override { sta_connected_cb = std::move(cb); }
  void set_on_sta_disconnected(std::function<void(int)> cb) override {
    sta_disconnected_cb = std::move(cb);
  }
  void set_on_got_ip(WifiGotIpCallback cb) override { got_ip_cb = std::move(cb); }
  void set_on_scan_complete(WifiScanCompleteCallback cb) override {
    scan_complete_cb = std::move(cb);
  }
  void set_on_ap_client_joined(WifiApClientJoinedCallback cb) override {
    ap_join_cb = std::move(cb);
  }
  void set_on_ap_client_left(WifiApClientLeftCallback cb) override { ap_leave_cb = std::move(cb); }
  void set_on_dhcp_timeout(std::function<void()> cb) override { dhcp_timeout_cb = std::move(cb); }
  void set_on_retry_due(std::function<void()> cb) override { retry_due_cb = std::move(cb); }

  // Counters
  int set_mode_calls = 0;
  int connect_calls = 0;
  int disconnect_calls = 0;
  int set_static_ip_calls = 0;
  int clear_static_ip_calls = 0;
  int scan_calls = 0;
  int start_ap_calls = 0;
  int start_mdns_calls = 0;
  int stop_mdns_calls = 0;
  int dhcp_armed_calls = 0;
  int dhcp_cancel_calls = 0;
  int retry_armed_calls = 0;
  int retry_cancel_calls = 0;

  WifiStatus connect_status = WifiStatus::Ok;
  WifiStatus scan_status = WifiStatus::Ok;
  WifiStatus start_mdns_status = WifiStatus::Ok;
  WifiStatus stop_mdns_status = WifiStatus::Ok;
  WifiMode last_mode_set = WifiMode::Off;
  std::string last_ssid;
  std::string last_password;
  std::string last_mdns_hostname;
  std::string last_mdns_service_type;
  uint32_t last_dhcp_timeout_ms = 0;
  uint32_t last_retry_delay_ms = 0;

  // Captured callbacks (so tests can drive events)
  WifiConnectedCallback sta_connected_cb;
  std::function<void(int)> sta_disconnected_cb;
  WifiGotIpCallback got_ip_cb;
  WifiScanCompleteCallback scan_complete_cb;
  WifiApClientJoinedCallback ap_join_cb;
  WifiApClientLeftCallback ap_leave_cb;
  std::function<void()> dhcp_timeout_cb;
  std::function<void()> retry_due_cb;

private:
  WifiMode _mode = WifiMode::Off;
};

WifiStaConfig make_sta_config(const char *ssid, uint8_t max_retry = 3) {
  WifiStaConfig cfg;
  std::strncpy(cfg.ssid, ssid, sizeof(cfg.ssid) - 1);
  std::strncpy(cfg.password, "secret", sizeof(cfg.password) - 1);
  cfg.max_retry_count = max_retry;
  cfg.initial_retry_interval_ms = 1000;
  cfg.max_retry_interval_ms = 8000;
  return cfg;
}

WifiMdnsProfile make_mdns_profile(const char *hostname,
                                  WifiMdnsLifecycle lifecycle = WifiMdnsLifecycle::StaIpAuto) {
  WifiMdnsProfile profile;
  profile.config.hostname = hostname;
  profile.lifecycle = lifecycle;
  return profile;
}

} // namespace

// ---------------------------------------------------------------------------
// Disconnect reason mapping
// ---------------------------------------------------------------------------

TEST_CASE("map_disconnect_reason normalises raw ESP-IDF codes", "[wifi-manager][mapping]") {
  using R = WifiDisconnectReason;
  REQUIRE(WifiManager::map_disconnect_reason(2) == R::auth_failed);        // AUTH_EXPIRE
  REQUIRE(WifiManager::map_disconnect_reason(202) == R::auth_failed);      // AUTH_FAIL
  REQUIRE(WifiManager::map_disconnect_reason(201) == R::no_ap_found);      // NO_AP_FOUND
  REQUIRE(WifiManager::map_disconnect_reason(210) == R::no_ap_found);      // _W_COMPATIBLE_SECURITY
  REQUIRE(WifiManager::map_disconnect_reason(5) == R::assoc_failed);       // ASSOC_TOOMANY
  REQUIRE(WifiManager::map_disconnect_reason(8) == R::ap_disconnected);    // ASSOC_LEAVE
  REQUIRE(WifiManager::map_disconnect_reason(206) == R::ap_disconnected);  // AP_TSF_RESET
  REQUIRE(WifiManager::map_disconnect_reason(200) == R::connection_lost);  // BEACON_TIMEOUT
  REQUIRE(WifiManager::map_disconnect_reason(208) == R::connection_lost);  // ASSOC_COMEBACK_TIME...
  REQUIRE(WifiManager::map_disconnect_reason(15) == R::handshake_failed);  // 4WAY_HANDSHAKE_TIMEOUT
  REQUIRE(WifiManager::map_disconnect_reason(204) == R::handshake_failed); // HANDSHAKE_TIMEOUT
  REQUIRE(WifiManager::map_disconnect_reason(9999) == R::unknown);
}

TEST_CASE("is_retriable matches the spec policy", "[wifi-manager][mapping]") {
  using R = WifiDisconnectReason;
  REQUIRE(WifiManager::is_retriable(R::ap_disconnected));
  REQUIRE(WifiManager::is_retriable(R::connection_lost));
  REQUIRE(WifiManager::is_retriable(R::handshake_failed));
  REQUIRE(WifiManager::is_retriable(R::unknown));
  // A transient missed-AP sweep must spend the retry budget, not bail.
  REQUIRE(WifiManager::is_retriable(R::no_ap_found));

  REQUIRE_FALSE(WifiManager::is_retriable(R::auth_failed));
  REQUIRE_FALSE(WifiManager::is_retriable(R::assoc_failed));
  REQUIRE_FALSE(WifiManager::is_retriable(R::dhcp_failed));
  REQUIRE_FALSE(WifiManager::is_retriable(R::requested_by_user));
}

TEST_CASE("compute_backoff_ms doubles and caps", "[wifi-manager][backoff]") {
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 0) == 1000);
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 1) == 2000);
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 2) == 4000);
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 3) == 8000);
  REQUIRE(WifiManager::compute_backoff_ms(1000, 8000, 4) == 8000); // capped
  REQUIRE(WifiManager::compute_backoff_ms(0, 8000, 3) == 0);       // disabled
}

// ---------------------------------------------------------------------------
// Mode state machine
// ---------------------------------------------------------------------------

TEST_CASE("set_mode is idempotent", "[wifi-manager][mode]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  REQUIRE(mgr.set_mode(WifiMode::Off) == WifiStatus::Ok);
  REQUIRE(hal.set_mode_calls == 0); // already Off
  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(hal.set_mode_calls == 1);
  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(hal.set_mode_calls == 1); // still 1 — idempotent
}

TEST_CASE("set_mode tears down mDNS and timers when leaving STA", "[wifi-manager][mode]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  mgr.set_mdns_profile(make_mdns_profile("test-host"));

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x0100007FU);
  REQUIRE(hal.start_mdns_calls == 1);

  mgr.set_mode(WifiMode::Off);
  REQUIRE(hal.stop_mdns_calls == 1);
  REQUIRE(hal.dhcp_cancel_calls >= 1);
  REQUIRE(hal.retry_cancel_calls >= 1);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Disconnected);
}

TEST_CASE("set_mode(Ap) while GotIp emits requested_by_user once",
          "[wifi-manager][mode][disconnect]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  REQUIRE(fired == 0);

  const int prior_retry_armed = hal.retry_armed_calls;
  REQUIRE(mgr.set_mode(WifiMode::Ap) == WifiStatus::Ok);
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::requested_by_user);
  REQUIRE(hal.retry_armed_calls == prior_retry_armed);
}

TEST_CASE("set_mode(Ap) while GotIp swallows the driver-echo disconnect",
          "[wifi-manager][mode][disconnect]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { fired += 1; });

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);

  mgr.set_mode(WifiMode::Ap);
  REQUIRE(fired == 1);

  // Driver echo after esp_wifi_set_mode must be swallowed, not retried.
  const int prior_retry_armed = hal.retry_armed_calls;
  hal.sta_disconnected_cb(8); // ASSOC_LEAVE, normally retriable
  REQUIRE(fired == 1);
  REQUIRE(hal.retry_armed_calls == prior_retry_armed);
}

TEST_CASE("set_mode(Off) from GotIp emits requested_by_user once",
          "[wifi-manager][mode][disconnect]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);

  REQUIRE(mgr.set_mode(WifiMode::Off) == WifiStatus::Ok);
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::requested_by_user);
}

TEST_CASE("set_mode(Ap) from Disconnected emits nothing", "[wifi-manager][mode][disconnect]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { fired += 1; });

  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.set_mode(WifiMode::Ap) == WifiStatus::Ok);
  REQUIRE(fired == 0);
}

TEST_CASE("status_snapshot zeros STA-only fields when Disconnected", "[wifi-manager][snapshot]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);

  mgr.set_mode(WifiMode::Off);
  const WifiStatusSnapshot snap = mgr.status_snapshot();
  REQUIRE(snap.sta_state == WifiStaState::Disconnected);
  REQUIRE(snap.ip == WIFI_IP_INVALID);
  REQUIRE(snap.rssi == WIFI_RSSI_INVALID);
  REQUIRE(snap.channel == 0);
  REQUIRE(snap.ssid[0] == '\0');
  for (uint8_t i = 0; i < 6; ++i) {
    REQUIRE(snap.bssid[i] == 0);
  }
}

// ---------------------------------------------------------------------------
// Mode enforcement
// ---------------------------------------------------------------------------

TEST_CASE("connect requires STA or APSTA mode", "[wifi-manager][enforcement]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Ap);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::Ok);
}

TEST_CASE("connect works in ApSta mode", "[wifi-manager][enforcement]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::ApSta);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::Ok);
}

TEST_CASE("connect with empty SSID and no saved creds returns NotFound",
          "[wifi-manager][enforcement]") {
  // Empty SSID is no longer an "invalid argument": it opts into the
  // "use NVS-saved credentials" convention. With nothing in NVS the
  // call surfaces NotFound so the caller can route to a fallback.
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);
  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::NotFound);
}

TEST_CASE("start_ap requires AP or APSTA mode", "[wifi-manager][enforcement]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  WifiApConfig cfg;
  std::strncpy(cfg.ssid, "MyAP", sizeof(cfg.ssid) - 1);
  REQUIRE(mgr.start_ap(cfg) == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.start_ap(cfg) == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Ap);
  REQUIRE(mgr.start_ap(cfg) == WifiStatus::Ok);
}

TEST_CASE("start_scan rejected in Off and AP modes", "[wifi-manager][scan]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Ap);
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.start_scan() == WifiStatus::Ok);
  REQUIRE(hal.scan_calls == 1);
}

TEST_CASE("start_scan rejected until the STA connection has an IP", "[wifi-manager][scan]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  // While Connecting
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
  hal.sta_connected_cb();
  // While Connected (no IP yet)
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
  CHECK(hal.scan_calls == 0);
}

TEST_CASE("start_scan delivers results without dropping an established connection",
          "[wifi-manager][scan]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  WifiScanEntry entry{};
  std::strcpy(entry.ssid, "OtherNet");
  bool delivered = false;
  mgr.set_on_scan_complete([&](const WifiScanEntry *results, uint16_t count) {
    REQUIRE(count == 1);
    CHECK(std::strcmp(results[0].ssid, "OtherNet") == 0);
    delivered = true;
  });
  REQUIRE(mgr.start_scan() == WifiStatus::Ok);
  REQUIRE(hal.scan_calls == 1);
  hal.scan_complete_cb(&entry, 1);
  CHECK(delivered);
  CHECK(hal.disconnect_calls == 0);
  CHECK(hal.connect_calls == 1);
}

// ---------------------------------------------------------------------------
// Connect flow + got-IP + mDNS
// ---------------------------------------------------------------------------

TEST_CASE("got_ip arms mDNS and dhcp timeout cancellation", "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_dhcp_timeout_ms(5000);

  WifiMdnsServiceRecord service = {};
  service.service_type = "_http._tcp";
  WifiMdnsConfig config = {};
  config.hostname = "ag-1";
  config.services = &service;
  config.service_count = 1;
  REQUIRE(mgr.set_mdns_config(config) == WifiStatus::Ok);

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  REQUIRE(hal.connect_calls == 1);

  bool connected_fired = false;
  bool got_ip_fired = false;
  uint32_t ip_seen = 0;
  mgr.set_on_connected([&] { connected_fired = true; });
  mgr.set_on_got_ip([&](uint32_t ip) {
    got_ip_fired = true;
    ip_seen = ip;
  });

  hal.sta_connected_cb();
  REQUIRE(connected_fired);
  REQUIRE(hal.dhcp_armed_calls == 1);
  REQUIRE(hal.last_dhcp_timeout_ms == 5000);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Connected);

  hal.got_ip_cb(0xC0A80164U);
  REQUIRE(got_ip_fired);
  REQUIRE(ip_seen == 0xC0A80164U);
  REQUIRE(hal.dhcp_cancel_calls >= 1);
  REQUIRE(hal.start_mdns_calls == 1);
  REQUIRE(hal.last_mdns_hostname == "ag-1");
  REQUIRE(hal.last_mdns_service_type == "_http._tcp");
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::GotIp);
}

TEST_CASE("disconnect after got_ip stops mDNS and reports RequestedByUser",
          "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mdns_profile(make_mdns_profile("ag-1"));

  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  bool fired = false;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired = true;
  });

  REQUIRE(mgr.disconnect() == WifiStatus::Ok);
  REQUIRE(fired);
  REQUIRE(last_reason == WifiDisconnectReason::requested_by_user);
  REQUIRE(hal.stop_mdns_calls == 1);
  REQUIRE(hal.disconnect_calls == 1);

  // Driver echo should be swallowed.
  fired = false;
  hal.sta_disconnected_cb(8); // ASSOC_LEAVE
  REQUIRE_FALSE(fired);
}

// ---------------------------------------------------------------------------
// Retry policy
// ---------------------------------------------------------------------------

TEST_CASE("disconnect echo cannot cancel a replacement connection", "[wifi-manager][connect]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("OldNet", 0));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  REQUIRE(mgr.disconnect() == WifiStatus::Ok);
  int failures = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { ++failures; });

  SECTION("echo arrives before connecting") {
    hal.sta_disconnected_cb(8);
    REQUIRE(mgr.connect(make_sta_config("NewNet", 0)) == WifiStatus::Ok);
  }
  SECTION("echo arrives after connecting") {
    REQUIRE(mgr.connect(make_sta_config("NewNet", 0)) == WifiStatus::Ok);
    hal.sta_disconnected_cb(8);
  }
  CHECK(failures == 0);
  CHECK(mgr.status_snapshot().sta_state == WifiStaState::Connecting);
  CHECK(hal.connect_calls == 2);
  CHECK(hal.disconnect_calls == 1);
  hal.sta_connected_cb();
  hal.got_ip_cb(0x02020202);
  CHECK(mgr.status_snapshot().sta_state == WifiStaState::GotIp);
  hal.sta_disconnected_cb(200);
  CHECK(failures == 1);
}

TEST_CASE("leaving STA clears a pending disconnect echo", "[wifi-manager][connect]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("OldNet", 0));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  mgr.disconnect();
  mgr.set_mode(WifiMode::Off);
  hal.sta_disconnected_cb(8); // Ignored while STA is off.
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("NewNet", 0));
  int failures = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { ++failures; });
  hal.sta_disconnected_cb(202);
  CHECK(failures == 1);
  CHECK(mgr.status_snapshot().sta_state == WifiStaState::Disconnected);
}

TEST_CASE("transient disconnect arms exponential retry timer", "[wifi-manager][retry]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);
  mgr.connect(make_sta_config("Net", /*max_retry=*/3));

  hal.sta_connected_cb();
  // Beacon timeout -> retriable
  hal.sta_disconnected_cb(200);
  REQUIRE(hal.retry_armed_calls == 1);
  REQUIRE(hal.last_retry_delay_ms == 1000);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Connecting);

  // Fire the retry; manager attempts to reconnect.
  hal.retry_due_cb();
  REQUIRE(hal.connect_calls == 2);

  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200);
  REQUIRE(hal.retry_armed_calls == 2);
  REQUIRE(hal.last_retry_delay_ms == 2000); // doubled

  hal.retry_due_cb();
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200);
  REQUIRE(hal.retry_armed_calls == 3);
  REQUIRE(hal.last_retry_delay_ms == 4000);
}

TEST_CASE("retry exhaustion fires on_disconnected with normalised reason",
          "[wifi-manager][retry]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.connect(make_sta_config("Net", /*max_retry=*/2));

  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200); // attempt 1 -> retry
  REQUIRE(fired == 0);
  hal.retry_due_cb();
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200); // attempt 2 -> retry
  REQUIRE(fired == 0);
  hal.retry_due_cb();
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200); // exhausted -> emit
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::connection_lost);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Disconnected);
}

TEST_CASE("non-retriable reasons emit immediately", "[wifi-manager][retry]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.connect(make_sta_config("Net", /*max_retry=*/5));
  hal.sta_disconnected_cb(202); // AUTH_FAIL -> AuthFailed
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::auth_failed);
  REQUIRE(hal.retry_armed_calls == 0);
}

TEST_CASE("max_retry_count == 0 disables auto-retry", "[wifi-manager][retry]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);

  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { fired += 1; });

  mgr.connect(make_sta_config("Net", /*max_retry=*/0));
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200); // retriable, but retries disabled
  REQUIRE(hal.retry_armed_calls == 0);
  REQUIRE(fired == 1);
}

TEST_CASE("disconnect() cancels pending retry timer", "[wifi-manager][retry]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);

  mgr.connect(make_sta_config("Net", /*max_retry=*/3));
  hal.sta_connected_cb();
  hal.sta_disconnected_cb(200);
  REQUIRE(hal.retry_armed_calls == 1);

  mgr.disconnect();
  REQUIRE(hal.retry_cancel_calls >= 1);
  // Late-firing retry callback should be a no-op.
  hal.retry_due_cb();
  REQUIRE(hal.connect_calls == 1); // unchanged
}

// ---------------------------------------------------------------------------
// DHCP timeout policy
// ---------------------------------------------------------------------------

TEST_CASE("DHCP timeout disconnects with DhcpFailed reason (non-retriable)",
          "[wifi-manager][dhcp]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  REQUIRE(hal.dhcp_armed_calls == 1);

  hal.dhcp_timeout_cb();
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::dhcp_failed);
  REQUIRE(hal.disconnect_calls == 1);
  REQUIRE_FALSE(WifiManager::is_retriable(WifiDisconnectReason::dhcp_failed));
}

TEST_CASE("Stale DHCP timeout after got_ip is ignored", "[wifi-manager][dhcp]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);

  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { fired += 1; });

  mgr.connect(make_sta_config("Net"));
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  hal.dhcp_timeout_cb(); // late-arriving callback
  REQUIRE(fired == 0);
}

// ---------------------------------------------------------------------------
// Scan + AP client events
// ---------------------------------------------------------------------------

TEST_CASE("scan results pass through to product callback", "[wifi-manager][scan]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);

  WifiScanEntry entries[2] = {};
  std::strncpy(entries[0].ssid, "AP1", sizeof(entries[0].ssid) - 1);
  std::strncpy(entries[1].ssid, "AP2", sizeof(entries[1].ssid) - 1);

  uint16_t seen_count = 0;
  std::string first_ssid;
  mgr.set_on_scan_complete([&](const WifiScanEntry *r, uint16_t c) {
    seen_count = c;
    if (c > 0)
      first_ssid = r[0].ssid;
  });

  REQUIRE(mgr.start_scan() == WifiStatus::Ok);
  hal.scan_complete_cb(entries, 2);
  REQUIRE(seen_count == 2);
  REQUIRE(first_ssid == "AP1");
}

TEST_CASE("AP client join/leave callbacks pass through", "[wifi-manager][ap]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Ap);

  int joined = 0;
  int left = 0;
  mgr.set_on_ap_client_joined([&](const uint8_t *) { joined += 1; });
  mgr.set_on_ap_client_left([&](const uint8_t *) { left += 1; });

  const uint8_t mac[6] = {1, 2, 3, 4, 5, 6};
  hal.ap_join_cb(mac);
  hal.ap_leave_cb(mac);
  REQUIRE(joined == 1);
  REQUIRE(left == 1);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST_CASE("connect while connecting returns AlreadyInProgress", "[wifi-manager][edge]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);
  REQUIRE(mgr.connect(make_sta_config("A")) == WifiStatus::Ok);
  REQUIRE(mgr.connect(make_sta_config("B")) == WifiStatus::AlreadyInProgress);
}

TEST_CASE("mDNS profile validation is transactional", "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  WifiMdnsServiceRecord original_service;
  original_service.service_type = "_http._tcp";
  WifiMdnsProfile original = make_mdns_profile("original");
  original.config.services = &original_service;
  original.config.service_count = 1;
  REQUIRE(mgr.set_mdns_profile(original) == WifiStatus::Ok);

  WifiMdnsProfile invalid = make_mdns_profile(nullptr);
  REQUIRE(mgr.set_mdns_profile(invalid) == WifiStatus::InvalidArgument);

  char long_hostname[WIFI_MDNS_MAX_HOSTNAME_LENGTH + 2];
  std::memset(long_hostname, 'a', sizeof(long_hostname) - 1);
  long_hostname[sizeof(long_hostname) - 1] = '\0';
  invalid = make_mdns_profile(long_hostname);
  REQUIRE(mgr.set_mdns_profile(invalid) == WifiStatus::InvalidArgument);

  invalid = make_mdns_profile("replacement");
  invalid.config.service_count = 1;
  REQUIRE(mgr.set_mdns_profile(invalid) == WifiStatus::InvalidArgument);

  invalid.config.service_count = static_cast<uint8_t>(WifiManager::MAX_MDNS_SERVICES + 1);
  REQUIRE(mgr.set_mdns_profile(invalid) == WifiStatus::InvalidArgument);
  invalid.config.service_count = 1;

  WifiMdnsServiceRecord invalid_service;
  invalid_service.service_type = "missing-protocol";
  invalid.config.services = &invalid_service;
  REQUIRE(mgr.set_mdns_profile(invalid) == WifiStatus::InvalidArgument);

  invalid_service.service_type = "_http._tcp";
  invalid_service.txt_count = 1;
  REQUIRE(mgr.set_mdns_profile(invalid) == WifiStatus::InvalidArgument);

  const char *const txt_keys[] = {nullptr};
  const char *const txt_values[] = {"value"};
  invalid_service.txt_keys = txt_keys;
  invalid_service.txt_values = txt_values;
  REQUIRE(mgr.set_mdns_profile(invalid) == WifiStatus::InvalidArgument);

  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.last_mdns_hostname == "original");
  REQUIRE(hal.last_mdns_service_type == "_http._tcp");

  hal.stop_mdns_status = WifiStatus::Failed;
  REQUIRE(mgr.set_mdns_profile(make_mdns_profile("replacement")) == WifiStatus::Failed);
  hal.stop_mdns_status = WifiStatus::Ok;
  REQUIRE(mgr.stop_mdns() == WifiStatus::Ok);
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.last_mdns_hostname == "original");
}

TEST_CASE("explicit mDNS lifecycle is idempotent and failures are retryable",
          "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  REQUIRE(mgr.set_mdns_profile(make_mdns_profile("manual", WifiMdnsLifecycle::Manual)) ==
          WifiStatus::Ok);

  hal.start_mdns_status = WifiStatus::Failed;
  REQUIRE(mgr.start_mdns() == WifiStatus::Failed);
  REQUIRE(hal.start_mdns_calls == 1);
  hal.start_mdns_status = WifiStatus::Ok;
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.start_mdns_calls == 2);
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.start_mdns_calls == 2);

  hal.stop_mdns_status = WifiStatus::Failed;
  REQUIRE(mgr.stop_mdns() == WifiStatus::Failed);
  REQUIRE(hal.stop_mdns_calls == 1);
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.start_mdns_calls == 2);
  hal.stop_mdns_status = WifiStatus::Ok;
  REQUIRE(mgr.stop_mdns() == WifiStatus::Ok);
  REQUIRE(hal.stop_mdns_calls == 2);
  REQUIRE(mgr.stop_mdns() == WifiStatus::Ok);
  REQUIRE(hal.stop_mdns_calls == 2);
}

TEST_CASE("automatic mDNS start failure remains explicitly retryable", "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  REQUIRE(mgr.set_mdns_profile(make_mdns_profile("automatic")) == WifiStatus::Ok);
  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::Ok);
  hal.sta_connected_cb();
  hal.start_mdns_status = WifiStatus::Failed;
  hal.got_ip_cb(0x01010101);
  REQUIRE(hal.start_mdns_calls == 1);

  hal.start_mdns_status = WifiStatus::Ok;
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.start_mdns_calls == 2);
}

TEST_CASE("automatic mDNS stop failure remains explicitly retryable", "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  REQUIRE(mgr.set_mdns_profile(make_mdns_profile("automatic")) == WifiStatus::Ok);
  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::Ok);
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  REQUIRE(hal.start_mdns_calls == 1);

  hal.stop_mdns_status = WifiStatus::Failed;
  hal.sta_disconnected_cb(200);
  REQUIRE(hal.stop_mdns_calls == 1);
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.start_mdns_calls == 1);

  hal.stop_mdns_status = WifiStatus::Ok;
  REQUIRE(mgr.stop_mdns() == WifiStatus::Ok);
  REQUIRE(hal.stop_mdns_calls == 2);
}

TEST_CASE("StaIpAuto mDNS profile stops and restarts across reconnect", "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  REQUIRE(mgr.set_mdns_profile(make_mdns_profile("automatic")) == WifiStatus::Ok);
  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::Ok);
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  REQUIRE(hal.start_mdns_calls == 1);

  hal.sta_disconnected_cb(200);
  REQUIRE(hal.stop_mdns_calls == 1);
  hal.retry_due_cb();
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010102);
  REQUIRE(hal.start_mdns_calls == 2);
  REQUIRE(hal.last_mdns_hostname == "automatic");
}

TEST_CASE("Manual mDNS profile survives STA disconnect but stops on mode Off",
          "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  REQUIRE(mgr.set_mdns_profile(make_mdns_profile("manual", WifiMdnsLifecycle::Manual)) ==
          WifiStatus::Ok);
  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(mgr.connect(make_sta_config("Net", 0)) == WifiStatus::Ok);
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  REQUIRE(hal.start_mdns_calls == 0);
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.start_mdns_calls == 1);

  hal.sta_disconnected_cb(200);
  REQUIRE(hal.stop_mdns_calls == 0);
  REQUIRE(mgr.set_mode(WifiMode::Ap) == WifiStatus::Ok);
  REQUIRE(hal.stop_mdns_calls == 0);
  REQUIRE(mgr.set_mode(WifiMode::Off) == WifiStatus::Ok);
  REQUIRE(hal.stop_mdns_calls == 1);
}

TEST_CASE("clear mDNS profile stops before forgetting it", "[wifi-manager][mdns]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  REQUIRE(mgr.set_mdns_profile(make_mdns_profile("clear-me", WifiMdnsLifecycle::Manual)) ==
          WifiStatus::Ok);
  REQUIRE(mgr.set_mode(WifiMode::Ap) == WifiStatus::Ok);
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);

  hal.stop_mdns_status = WifiStatus::Failed;
  REQUIRE(mgr.clear_mdns_profile() == WifiStatus::Failed);
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.start_mdns_calls == 1);

  hal.stop_mdns_status = WifiStatus::Ok;
  REQUIRE(mgr.stop_mdns() == WifiStatus::Ok);
  REQUIRE(mgr.start_mdns() == WifiStatus::Ok);
  REQUIRE(hal.last_mdns_hostname == "clear-me");
  REQUIRE(mgr.clear_mdns_profile() == WifiStatus::Ok);
  REQUIRE(hal.stop_mdns_calls == 3);
  REQUIRE(mgr.start_mdns() == WifiStatus::InvalidState);
}

// ---------------------------------------------------------------------------
// Credential store API (delegates to WifiCredentialStore)
// ---------------------------------------------------------------------------

TEST_CASE("credential API delegates to the store", "[wifi-manager][creds]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);

  REQUIRE_FALSE(mgr.has_saved_networks());
  REQUIRE(mgr.add_network("Net1", "pass1") == WifiStatus::Ok);
  REQUIRE(mgr.add_network("Net2", "pass2") == WifiStatus::Ok);
  REQUIRE(mgr.has_saved_networks());

  char out[WIFI_MAX_SAVED_NETWORKS][33] = {};
  REQUIRE(mgr.list_networks(out, WIFI_MAX_SAVED_NETWORKS) == 2);
  REQUIRE(std::string(out[0]) == "Net2"); // newest-first
  REQUIRE(std::string(out[1]) == "Net1");

  REQUIRE(mgr.remove_network("Net1") == WifiStatus::Ok);
  REQUIRE(mgr.remove_network("Missing") == WifiStatus::NotFound);
  REQUIRE(mgr.clear_networks() == WifiStatus::Ok);
  REQUIRE_FALSE(mgr.has_saved_networks());
}

// ---------------------------------------------------------------------------
// Auto-connect (empty SSID): 0 / 1 / >1 saved networks
// ---------------------------------------------------------------------------

namespace {

WifiScanEntry make_scan_entry(const char *ssid, int8_t rssi) {
  WifiScanEntry e = {};
  std::strncpy(e.ssid, ssid, sizeof(e.ssid) - 1);
  e.rssi = rssi;
  return e;
}

} // namespace

TEST_CASE("auto-connect with 0 saved returns NotFound", "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);

  WifiStaConfig cfg; // empty SSID
  REQUIRE(mgr.connect(cfg) == WifiStatus::NotFound);
  REQUIRE(hal.connect_calls == 0);
  REQUIRE(hal.scan_calls == 0);
}

TEST_CASE("auto-connect with 1 saved connects directly without scanning", "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("HomeNet", "homepass");
  mgr.set_mode(WifiMode::Sta);

  WifiStaConfig cfg; // empty SSID, default retry
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  REQUIRE(hal.scan_calls == 0); // single network skips the scan
  REQUIRE(hal.connect_calls == 1);
  REQUIRE(hal.last_ssid == "HomeNet");
  REQUIRE(hal.last_password == "homepass");

  // Single-network path is not a sweep: normal retry/backoff applies.
  hal.sta_disconnected_cb(200); // BEACON_TIMEOUT (retriable)
  REQUIRE(hal.retry_armed_calls == 1);
}

TEST_CASE("auto-connect with 1 saved retries no_ap_found before bailing",
          "[wifi-manager][auto][retry]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("HomeNet", "homepass");
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last_reason = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    fired += 1;
  });

  WifiStaConfig cfg = make_sta_config("", /*max_retry=*/2); // empty SSID
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  REQUIRE(hal.scan_calls == 0); // single network skips the scan

  // A missed connect-time sweep reports no_ap_found — now retriable, so it
  // spends the budget instead of bailing on the first sweep.
  hal.sta_disconnected_cb(201); // NO_AP_FOUND, attempt 1 -> retry
  REQUIRE(hal.retry_armed_calls == 1);
  REQUIRE(fired == 0);
  hal.retry_due_cb();

  hal.sta_disconnected_cb(201); // attempt 2 -> retry
  REQUIRE(hal.retry_armed_calls == 2);
  REQUIRE(fired == 0);
  hal.retry_due_cb();

  // Budget exhausted: now the terminal no_ap_found surfaces to the caller.
  hal.sta_disconnected_cb(201);
  REQUIRE(fired == 1);
  REQUIRE(last_reason == WifiDisconnectReason::no_ap_found);
}

TEST_CASE("auto-connect with >1 saved scans then connects strongest", "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("Net1", "p1");
  mgr.add_network("Net2", "p2");
  mgr.add_network("Net3", "p3");
  mgr.set_mode(WifiMode::Sta);

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  REQUIRE(hal.scan_calls == 1);
  REQUIRE(hal.connect_calls == 0); // waiting on scan results

  // Net1 visible (weak), Net2 visible (strong), Net3 not visible.
  WifiScanEntry entries[] = {
      make_scan_entry("Net1", -70),
      make_scan_entry("Net2", -40),
  };
  hal.scan_complete_cb(entries, 2);

  REQUIRE(hal.connect_calls == 1);
  REQUIRE(hal.last_ssid == "Net2"); // strongest RSSI
  REQUIRE(hal.last_password == "p2");
}

TEST_CASE("auto-connect dedups per SSID and breaks ties newest-first", "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("Older", "po"); // saved first (oldest)
  mgr.add_network("Newer", "pn"); // saved last (newest)
  mgr.set_mode(WifiMode::Sta);

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);

  // Both at equal RSSI; "Older" appears twice (multi-AP). Tie-break must
  // pick the newest saved entry ("Newer").
  WifiScanEntry entries[] = {
      make_scan_entry("Older", -50),
      make_scan_entry("Older", -55),
      make_scan_entry("Newer", -50),
  };
  hal.scan_complete_cb(entries, 3);

  REQUIRE(hal.connect_calls == 1);
  REQUIRE(hal.last_ssid == "Newer");
}

TEST_CASE("auto-connect single-attempt failover across candidates", "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("Weak", "pw");
  mgr.add_network("Strong", "ps");
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last_reason = WifiDisconnectReason::requested_by_user;
  int disconnected = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last_reason = r;
    disconnected += 1;
  });

  WifiStaConfig cfg = make_sta_config("", /*max_retry=*/5);
  cfg.ssid[0] = '\0';
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);

  WifiScanEntry entries[] = {
      make_scan_entry("Strong", -40),
      make_scan_entry("Weak", -70),
  };
  hal.scan_complete_cb(entries, 2);
  REQUIRE(hal.last_ssid == "Strong");
  REQUIRE(hal.connect_calls == 1);

  // Strongest fails: single attempt, no retry timer; advance to next.
  hal.sta_disconnected_cb(202); // AUTH_FAIL
  REQUIRE(hal.retry_armed_calls == 0);
  REQUIRE(hal.connect_calls == 2);
  REQUIRE(hal.last_ssid == "Weak");
  REQUIRE(disconnected == 0);

  // Last candidate fails too: sweep exhausted -> emit disconnected.
  hal.sta_disconnected_cb(202);
  REQUIRE(hal.retry_armed_calls == 0);
  REQUIRE(disconnected == 1);
  REQUIRE(last_reason == WifiDisconnectReason::auth_failed);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Disconnected);
}

TEST_CASE("auto-connect: no visible saved network emits disconnected", "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("Net1", "p1");
  mgr.add_network("Net2", "p2");
  mgr.set_mode(WifiMode::Sta);

  int disconnected = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { disconnected += 1; });

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);

  WifiScanEntry entries[] = {make_scan_entry("Stranger", -40)};
  hal.scan_complete_cb(entries, 1);

  REQUIRE(hal.connect_calls == 0);
  REQUIRE(disconnected == 1);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::Disconnected);
}

TEST_CASE("auto-connect: hidden APs (empty SSID) never match", "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("Net1", "p1");
  mgr.add_network("Net2", "p2");
  mgr.set_mode(WifiMode::Sta);

  int disconnected = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { disconnected += 1; });

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);

  WifiScanEntry entries[] = {make_scan_entry("", -30)}; // hidden AP
  hal.scan_complete_cb(entries, 1);

  REQUIRE(hal.connect_calls == 0);
  REQUIRE(disconnected == 1);
}

TEST_CASE("auto-connect: scan-start failure returns Failed and clears pending",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("Net1", "p1");
  mgr.add_network("Net2", "p2");
  mgr.set_mode(WifiMode::Sta);

  hal.scan_status = WifiStatus::Failed;
  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Failed);
  REQUIRE(hal.scan_calls == 1);
  REQUIRE(hal.connect_calls == 0);

  // _auto_scan_pending must be cleared: a fresh connect is accepted.
  hal.scan_status = WifiStatus::Ok;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  REQUIRE(hal.scan_calls == 2);
}

TEST_CASE("auto-connect: DHCP timeout during sweep advances to next candidate",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("A", "pa");
  mgr.add_network("B", "pb");
  mgr.set_mode(WifiMode::Sta);

  int disconnected = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { disconnected += 1; });

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  WifiScanEntry entries[] = {make_scan_entry("A", -40), make_scan_entry("B", -50)};
  hal.scan_complete_cb(entries, 2);
  REQUIRE(hal.last_ssid == "A");

  // L2 up, then DHCP times out: treat as candidate failure (advance, no
  // disconnected emit).
  hal.sta_connected_cb();
  hal.dhcp_timeout_cb();
  REQUIRE(disconnected == 0);
  REQUIRE(hal.connect_calls == 2);
  REQUIRE(hal.last_ssid == "B");
}

TEST_CASE("auto-connect: retry/backoff suppressed during sweep, active after got-IP",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("A", "pa");
  mgr.add_network("B", "pb");
  mgr.set_mode(WifiMode::Sta);

  WifiStaConfig cfg = make_sta_config("", /*max_retry=*/3);
  cfg.ssid[0] = '\0';
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  WifiScanEntry entries[] = {make_scan_entry("A", -40), make_scan_entry("B", -50)};
  hal.scan_complete_cb(entries, 2);

  // Winner gets IP: sweep ends, normal retry policy now governs.
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  REQUIRE(mgr.status_snapshot().sta_state == WifiStaState::GotIp);

  // A later link drop reconnects to the winning AP under normal retry.
  hal.sta_disconnected_cb(200); // BEACON_TIMEOUT (retriable)
  REQUIRE(hal.retry_armed_calls == 1);
  hal.retry_due_cb();
  REQUIRE(hal.last_ssid == "A"); // reconnect to the winner
}

TEST_CASE("auto-connect: internal scan results not forwarded to product callback",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("Net1", "p1");
  mgr.add_network("Net2", "p2");
  mgr.set_mode(WifiMode::Sta);

  int product_scans = 0;
  mgr.set_on_scan_complete([&](const WifiScanEntry *, uint16_t) { product_scans += 1; });

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  WifiScanEntry entries[] = {make_scan_entry("Net1", -50)};
  hal.scan_complete_cb(entries, 1);
  REQUIRE(product_scans == 0); // consumed internally
}

TEST_CASE("auto-connect: product start_scan rejected while auto cycle owns the radio",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("Net1", "p1");
  mgr.add_network("Net2", "p2");
  mgr.set_mode(WifiMode::Sta);

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok); // internal scan in flight
  REQUIRE(mgr.start_scan() == WifiStatus::InvalidState);
  // A second connect during the scan window is rejected too.
  REQUIRE(mgr.connect(cfg) == WifiStatus::AlreadyInProgress);
}

TEST_CASE("auto-connect: disconnect() during scan window cancels the cycle",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("Net1", "p1");
  mgr.add_network("Net2", "p2");
  mgr.set_mode(WifiMode::Sta);

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  mgr.disconnect();

  // A late internal scan-complete must not restart connecting.
  WifiScanEntry entries[] = {make_scan_entry("Net1", -40)};
  hal.scan_complete_cb(entries, 1);
  REQUIRE(hal.connect_calls == 0);
}

TEST_CASE("auto-connect: connect() during an active sweep returns AlreadyInProgress",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("A", "pa");
  mgr.add_network("B", "pb");
  mgr.set_mode(WifiMode::Sta);

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  WifiScanEntry entries[] = {make_scan_entry("A", -40), make_scan_entry("B", -50)};
  hal.scan_complete_cb(entries, 2); // _auto_sweeping now true, attempting A
  REQUIRE(mgr.connect(make_sta_config("X")) == WifiStatus::AlreadyInProgress);
}

// ---------------------------------------------------------------------------
// Self-induced disconnect echo (manager-triggered disconnect_sta on DHCP
// timeout). The driver emits its own STA_DISCONNECTED afterwards; the
// manager must ignore that one echo.
// ---------------------------------------------------------------------------

TEST_CASE("non-sweep DHCP timeout: driver echo does not arm a retry", "[wifi-manager][dhcp]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last = WifiDisconnectReason::unknown;
  int fired = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last = r;
    fired += 1;
  });

  mgr.connect(make_sta_config("Net", /*max_retry=*/5));
  hal.sta_connected_cb(); // L2 up, DHCP armed
  hal.dhcp_timeout_cb();  // DHCP fails -> emit dhcp_failed + disconnect_sta()
  REQUIRE(fired == 1);
  REQUIRE(last == WifiDisconnectReason::dhcp_failed);

  // The disconnect_sta() echo (ASSOC_LEAVE, normally retriable) is swallowed:
  // no spurious retry, no second disconnected.
  hal.sta_disconnected_cb(8); // ASSOC_LEAVE echo
  REQUIRE(hal.retry_armed_calls == 0);
  REQUIRE(fired == 1);
}

TEST_CASE("auto-connect: DHCP-timeout sweep ignores self-disconnect echo (no candidate skip)",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("A", "pa");
  mgr.add_network("B", "pb");
  mgr.add_network("C", "pc");
  mgr.set_mode(WifiMode::Sta);

  WifiDisconnectReason last = WifiDisconnectReason::unknown;
  int disconnected = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason r) {
    last = r;
    disconnected += 1;
  });

  WifiStaConfig cfg; // empty SSID
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  // RSSI order C > B > A => candidate order C, B, A.
  WifiScanEntry entries[] = {
      make_scan_entry("A", -70),
      make_scan_entry("B", -60),
      make_scan_entry("C", -50),
  };
  hal.scan_complete_cb(entries, 3);
  REQUIRE(hal.connect_calls == 1);
  REQUIRE(hal.last_ssid == "C");

  // Each candidate associates, DHCP-times-out (manager calls disconnect_sta),
  // then the driver echo arrives. Exactly one attempt per candidate, no skip.
  hal.sta_connected_cb();
  hal.dhcp_timeout_cb();      // C fails -> advance to B
  hal.sta_disconnected_cb(8); // C's disconnect_sta echo -> no-op
  REQUIRE(hal.connect_calls == 2);
  REQUIRE(hal.last_ssid == "B");
  REQUIRE(disconnected == 0);

  hal.sta_connected_cb();
  hal.dhcp_timeout_cb();      // B fails -> advance to A
  hal.sta_disconnected_cb(8); // echo -> no-op
  REQUIRE(hal.connect_calls == 3);
  REQUIRE(hal.last_ssid == "A");
  REQUIRE(disconnected == 0);

  hal.sta_connected_cb();
  hal.dhcp_timeout_cb();      // A is last -> sweep exhausted
  hal.sta_disconnected_cb(8); // echo -> no-op
  REQUIRE(disconnected == 1);
  REQUIRE(last == WifiDisconnectReason::dhcp_failed);
  REQUIRE(hal.connect_calls == 3); // no extra attempts
}

TEST_CASE("auto-connect: disconnect() during sweep cancels; late candidate echo is a no-op",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("A", "pa");
  mgr.add_network("B", "pb");
  mgr.set_mode(WifiMode::Sta);

  int disconnected = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { disconnected += 1; });

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  WifiScanEntry entries[] = {make_scan_entry("A", -40), make_scan_entry("B", -50)};
  hal.scan_complete_cb(entries, 2);
  REQUIRE(hal.last_ssid == "A"); // sweeping, attempting A
  const int connects_before = hal.connect_calls;

  mgr.disconnect(); // cancel mid-sweep
  REQUIRE(disconnected == 1);

  // A late candidate-failure echo must not resurrect the sweep.
  hal.sta_disconnected_cb(202); // AUTH_FAIL echo
  REQUIRE(hal.connect_calls == connects_before);
  REQUIRE(disconnected == 1);
}

TEST_CASE("auto-connect: set_mode leaving STA during sweep cancels; late echo no-op",
          "[wifi-manager][auto]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("A", "pa");
  mgr.add_network("B", "pb");
  mgr.set_mode(WifiMode::Sta);

  int disconnected = 0;
  mgr.set_on_disconnected([&](WifiDisconnectReason) { disconnected += 1; });

  WifiStaConfig cfg;
  REQUIRE(mgr.connect(cfg) == WifiStatus::Ok);
  WifiScanEntry entries[] = {make_scan_entry("A", -40), make_scan_entry("B", -50)};
  hal.scan_complete_cb(entries, 2);
  const int connects_before = hal.connect_calls;

  REQUIRE(mgr.set_mode(WifiMode::Ap) == WifiStatus::Ok); // leaves STA mid-sweep
  REQUIRE(disconnected == 1);

  hal.sta_disconnected_cb(8); // late echo -> swallowed
  REQUIRE(hal.connect_calls == connects_before);
  REQUIRE(disconnected == 1);
}

// ---------------------------------------------------------------------------
// Explicit connect never writes the credential store
// ---------------------------------------------------------------------------

TEST_CASE("explicit connect leaves the saved-network store untouched", "[wifi-manager][creds]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  REQUIRE(mgr.add_network("Saved", "savedpass") == WifiStatus::Ok);
  const int commits_after_add = backend.commit_count;
  mgr.set_mode(WifiMode::Sta);

  // Explicit-SSID connect to a different network must not touch the store.
  REQUIRE(mgr.connect(make_sta_config("Other")) == WifiStatus::Ok);
  REQUIRE(hal.last_ssid == "Other");
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101); // even a full success persists nothing here

  REQUIRE(backend.commit_count == commits_after_add); // no new writes
  char out[WIFI_MAX_SAVED_NETWORKS][33] = {};
  REQUIRE(mgr.list_networks(out, WIFI_MAX_SAVED_NETWORKS) == 1);
  REQUIRE(std::string(out[0]) == "Saved");
}

TEST_CASE("Suspended station ignores queued link events and retry deadlines",
          "[wifi-manager][handoff]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  REQUIRE(mgr.set_mode(WifiMode::Sta) == WifiStatus::Ok);
  REQUIRE(mgr.connect(make_sta_config("Net")) == WifiStatus::Ok);
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  REQUIRE(mgr.set_mode(WifiMode::Off) == WifiStatus::Ok);
  int connected = 0;
  int disconnected = 0;
  mgr.set_on_got_ip([&](uint32_t) { ++connected; });
  mgr.set_on_disconnected([&](WifiDisconnectReason) { ++disconnected; });
  const auto attempts = hal.connect_calls;
  hal.sta_connected_cb();
  hal.got_ip_cb(0x01010101);
  hal.sta_disconnected_cb(200);
  hal.retry_due_cb();
  hal.dhcp_timeout_cb();
  CHECK(mgr.status_snapshot().sta_state == WifiStaState::Disconnected);
  CHECK(connected == 0);
  CHECK(disconnected == 0);
  CHECK(hal.connect_calls == attempts);
}

TEST_CASE("Scan selection returns one saved SSID without connecting", "[wifi-manager][scan]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("HomeNet", "homepass");
  mgr.add_network("Office", "officepass");
  char ssid[33] = "old result";
  SECTION("strongest visible saved network wins") {
    const WifiScanEntry results[] = {
        make_scan_entry("Unknown", -10), make_scan_entry("HomeNet", -50),
        make_scan_entry("Office", -70), make_scan_entry("HomeNet", -40)};
    REQUIRE(mgr.select_saved_network(results, 4, ssid));
    CHECK(std::string(ssid) == "HomeNet");
  }
  SECTION("equal signal prefers the newest saved entry") {
    const WifiScanEntry results[] = {make_scan_entry("HomeNet", -50),
                                     make_scan_entry("Office", -50)};
    REQUIRE(mgr.select_saved_network(results, 2, ssid));
    CHECK(std::string(ssid) == "Office");
  }
  SECTION("absent and hidden networks do not match") {
    const WifiScanEntry results[] = {make_scan_entry("Unknown", -10), make_scan_entry("", -20)};
    CHECK_FALSE(mgr.select_saved_network(results, 2, ssid));
    CHECK(ssid[0] == '\0');
  }
  SECTION("empty scan clears the previous selection") {
    CHECK_FALSE(mgr.select_saved_network(nullptr, 0, ssid));
    CHECK(ssid[0] == '\0');
  }
  CHECK(hal.connect_calls == 0);
  CHECK(hal.scan_calls == 0);
}

TEST_CASE("Selected saved network connects directly using its current credentials",
          "[wifi-manager][scan]") {
  FakeWifiHal hal;
  FakeConfigStore backend;
  WifiManager mgr(hal, backend);
  mgr.add_network("HomeNet", "homepass");
  mgr.add_network("Office", "officepass");
  mgr.set_mode(WifiMode::Sta);
  SECTION("selected network uses stored password without another scan") {
    REQUIRE(mgr.connect_saved_network("HomeNet") == WifiStatus::Ok);
    CHECK(hal.connect_calls == 1);
    CHECK(hal.last_ssid == "HomeNet");
    CHECK(hal.last_password == "homepass");
  }
  SECTION("removed selection does not connect to another saved network") {
    mgr.remove_network("HomeNet");
    CHECK(mgr.connect_saved_network("HomeNet") == WifiStatus::NotFound);
    CHECK(hal.connect_calls == 0);
  }
  SECTION("empty selection cannot trigger an automatic scan") {
    CHECK(mgr.connect_saved_network("") == WifiStatus::InvalidArgument);
    CHECK(mgr.connect_saved_network(nullptr) == WifiStatus::InvalidArgument);
    CHECK(hal.connect_calls == 0);
  }
  CHECK(hal.scan_calls == 0);
}
