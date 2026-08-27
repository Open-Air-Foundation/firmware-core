/**
 * AirGradient Go — PortableWifiProvisioner host tests
 *
 * Exercises the attached Portable Wi-Fi provisioning mechanics against a
 * real ProvisioningManager + WifiManager (fake HAL) + MockBleServer and a
 * minimal fake board. Verifies request marshaling, lazy radio bring-up
 * (STA only), idle-timeout drop, verify-then-drop, BLE-disconnect drop, and
 * that stop() never deinitialises the borrowed server.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "go_board.h"
#include "go_portable_provisioner.h"
#include "go_settings.h"
#include "fake_config_store.h"
#include "hal/ble_server.h"
#include "hal/wifi_hal.h"
#include "mock_ble.h"
#include "rtos.h"
#include "services/provisioning_manager.h"
#include "services/wifi_manager.h"

class PortableProvTestAccess {
public:
  static ProvisioningManager *provisioning_manager(const PortableWifiProvisioner &p) {
    return p._manager;
  }
};

namespace {

constexpr const char *PROV_SERVICE_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
constexpr const char *CRED_STATUS_CHAR_UUID = "703fa252-3d2a-4da9-a05c-83b0d9cacb8e";
constexpr const char *SCAN_CHAR_UUID = "467a080f-e50f-42c9-b9b2-a2ab14d82725";

// Controllable host RTOS: settable clock, default in-memory queue ops.
class ControllableRTOS : public RTOS {
public:
  void delay_ms_impl(uint32_t) override {}
  uint64_t get_time_ms_impl() override { return now_ms; }
  uint64_t now_ms = 0;
};

// Minimal WifiHal fake: tracks mode + scan/connect; can fail set_mode(Sta)
// and fire got_ip / disconnected to drive WifiManager.
class FakeWifiHal : public WifiHal {
public:
  WifiStatus init() override { return WifiStatus::Ok; }
  void deinit() override {}

  WifiStatus set_mode(WifiMode m) override {
    if (fail_set_mode_for == m) {
      fail_set_mode_for = WifiMode::Ap; // one-shot (Ap never requested here)
      return WifiStatus::Failed;
    }
    _mode = m;
    ++set_mode_calls;
    return WifiStatus::Ok;
  }
  WifiMode get_mode() const override { return _mode; }

  WifiStatus connect_sta(const char *ssid, const char *password) override {
    ++connect_calls;
    last_ssid = ssid != nullptr ? ssid : "";
    (void)password;
    return WifiStatus::Ok;
  }
  WifiStatus disconnect_sta() override { return WifiStatus::Ok; }
  WifiStatus set_static_ip(const WifiStaticIpConfig &) override { return WifiStatus::Ok; }
  WifiStatus clear_static_ip() override { return WifiStatus::Ok; }
  WifiStatus start_scan(const WifiScanConfig &) override {
    ++scan_calls;
    return WifiStatus::Ok;
  }
  WifiStatus start_ap(const WifiApConfig &) override { return WifiStatus::Ok; }
  WifiStatus stop_ap() override { return WifiStatus::Ok; }
  WifiStatusSnapshot get_status() const override { return {}; }
  WifiStatus set_power_save(WifiPowerSave) override { return WifiStatus::Ok; }
  WifiStatus start_mdns(const WifiMdnsConfig &) override { return WifiStatus::Ok; }
  WifiStatus stop_mdns() override { return WifiStatus::Ok; }
  WifiStatus arm_dhcp_timeout(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_dhcp_timeout() override { return WifiStatus::Ok; }
  WifiStatus arm_retry_timer(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_retry_timer() override { return WifiStatus::Ok; }

  void set_on_sta_connected(WifiConnectedCallback cb) override { _on_connected = std::move(cb); }
  void set_on_sta_disconnected(std::function<void(int)> cb) override {
    _on_disconnected = std::move(cb);
  }
  void set_on_got_ip(WifiGotIpCallback cb) override { _on_got_ip = std::move(cb); }
  void set_on_scan_complete(WifiScanCompleteCallback cb) override { _on_scan = std::move(cb); }
  void set_on_ap_client_joined(WifiApClientJoinedCallback cb) override {
    _on_joined = std::move(cb);
  }
  void set_on_ap_client_left(WifiApClientLeftCallback cb) override { _on_left = std::move(cb); }
  void set_on_dhcp_timeout(std::function<void()> cb) override { _on_dhcp = std::move(cb); }
  void set_on_retry_due(std::function<void()> cb) override { _on_retry = std::move(cb); }

  void fire_got_ip(uint32_t ip) {
    if (_on_got_ip) {
      _on_got_ip(ip);
    }
  }

  uint32_t set_mode_calls = 0;
  uint32_t scan_calls = 0;
  uint32_t connect_calls = 0;
  std::string last_ssid;
  WifiMode fail_set_mode_for = WifiMode::Ap; // Ap = disabled sentinel

private:
  WifiMode _mode = WifiMode::Off;
  WifiConnectedCallback _on_connected;
  std::function<void(int)> _on_disconnected;
  WifiGotIpCallback _on_got_ip;
  WifiScanCompleteCallback _on_scan;
  WifiApClientJoinedCallback _on_joined;
  WifiApClientLeftCallback _on_left;
  std::function<void()> _on_dhcp;
  std::function<void()> _on_retry;
};

// Minimal GoBoard: only init_wifi_subsystem() is observable. All service
// accessors are unreachable from the provisioner (it uses the direct deps),
// so they return reinterpret_cast'd dummies that are never dereferenced.
class FakeBoard : public GoBoard {
public:
  int init_wifi_subsystem_calls = 0;

  void init_nvs() override {}
  void init_buses() override {}
  void init_spi() override {}
  void init_fuel_gauge() override {}
  bool init_bms() override { return true; }
  void init_wifi_subsystem() override { ++init_wifi_subsystem_calls; }
  void init_core() override {}

  ConfigStore &config_store() override { return *reinterpret_cast<ConfigStore *>(_buf); }
  GoSettings load_settings() override { return {}; }
  BmsDevice *bms() override { return reinterpret_cast<BmsDevice *>(_buf); }
  SensorManager &sensors(bool) override { return *reinterpret_cast<SensorManager *>(_buf); }
  StorageService &storage() override { return *reinterpret_cast<StorageService *>(_buf); }
  DisplayService &display() override { return *reinterpret_cast<DisplayService *>(_buf); }
  LedService &led_service() override { return *reinterpret_cast<LedService *>(_buf); }
  BuzzerService &buzzer_service() override { return *reinterpret_cast<BuzzerService *>(_buf); }
  PowerService &power() override { return *reinterpret_cast<PowerService *>(_buf); }
  WifiHal &wifi_hal() override { return *reinterpret_cast<WifiHal *>(_buf); }
  WifiManager &wifi_manager() override { return *reinterpret_cast<WifiManager *>(_buf); }
  HttpServer &http_server() override { return *reinterpret_cast<HttpServer *>(_buf); }
  AgBleServer &ble_server() override { return *reinterpret_cast<AgBleServer *>(_buf); }
  AgClient &ag_client() override { return *reinterpret_cast<AgClient *>(_buf); }
  GpsDriver *new_gps_driver() override { return nullptr; }
  CapTouchSensor *new_touch_sensor() override { return nullptr; }
  AccelSensor *new_accel_sensor() override { return nullptr; }
  BoardVariant variant() const override { return BoardVariant::Prototype; }
  std::string serial_number() override { return "TEST00"; }
  const char *firmware_version() override { return "test"; }
  const gpio::Hal &gpio_hal() override { return *reinterpret_cast<gpio::Hal *>(_buf); }
  void release_gpio_holds() override {}
  void ulp_stop() override {}
  void ulp_start() override {}
  void restart() override {}
  void install_button_isr(int, volatile bool *) override {}
  void remove_button_isr(int) override {}

private:
  alignas(8) static inline char _buf[64];
};

struct Fixture {
  ControllableRTOS rtos;
  FakeWifiHal hal;
  FakeConfigStore creds_store;
  WifiManager wifi{hal, creds_store};
  MockBleServer ble;
  FakeBoard board;
  RtosQueueHandle queue;
  PortableWifiProvisioner prov;

  Fixture() : queue(nullptr), prov(make_queue(), {wifi, ble, board}, make_cfg()) {
    // queue created in make_queue() before prov; store handle.
  }

  ~Fixture() { RTOS::set_instance(nullptr); }

  static PortableWifiProvisioner::Config make_cfg() {
    PortableWifiProvisioner::Config cfg{};
    cfg.ble_model_name = "P-1PSG";
    cfg.ble_serial_number = "TEST00";
    cfg.ble_firmware_version = "test";
    cfg.radio_idle_ms = 90000;
    return cfg;
  }

  RtosQueueHandle make_queue() {
    RTOS::set_instance(&rtos);
    // Generous depth; the provisioner only posts a signal byte (item_size 4),
    // and the tests never drain the queue.
    queue = RTOS::queue_create(64, sizeof(int));
    return queue;
  }

  MockBleCharacteristic *cred_char() {
    return ble.find_char(PROV_SERVICE_UUID, CRED_STATUS_CHAR_UUID);
  }
  MockBleCharacteristic *scan_char() { return ble.find_char(PROV_SERVICE_UUID, SCAN_CHAR_UUID); }
};

} // namespace

TEST_CASE("PortableProv constructs provisioning manager only when attaching",
          "[portable_prov][lifecycle]") {
  Fixture f;

  CHECK(PortableProvTestAccess::provisioning_manager(f.prov) == nullptr);

  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.attach());
  ProvisioningManager *const manager = PortableProvTestAccess::provisioning_manager(f.prov);
  REQUIRE(manager != nullptr);

  f.prov.stop();
  CHECK(PortableProvTestAccess::provisioning_manager(f.prov) == manager);
}

TEST_CASE("PortableProv: attach registers prov service, radio off", "[portable_prov]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));

  REQUIRE(f.prov.attach());
  REQUIRE(f.prov.is_attached());
  REQUIRE_FALSE(f.prov.is_radio_active());
  REQUIRE(f.hal.get_mode() == WifiMode::Off);
  REQUIRE(f.prov.next_deadline_ms() == 0);

  // Provisioning service registered on the borrowed server (no re-init).
  REQUIRE(f.ble.find_service(PROV_SERVICE_UUID) != nullptr);
  REQUIRE(f.ble.init_count == 1);
  REQUIRE(f.ble.start_advertising_count == 0);

  f.prov.stop();
}

TEST_CASE("PortableProv: scan request brings up radio (STA) and triggers scan", "[portable_prov]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.attach());

  // App writes the scan characteristic (NimBLE task path).
  f.scan_char()->simulate_write("1");

  // Radio not up yet — work happens on the orchestrator task.
  REQUIRE_FALSE(f.prov.is_radio_active());
  REQUIRE(f.hal.scan_calls == 0);

  f.prov.handle_pending_request();

  REQUIRE(f.board.init_wifi_subsystem_calls == 1); // lazy, idempotent
  REQUIRE(f.hal.get_mode() == WifiMode::Sta);      // STA only
  REQUIRE(f.prov.is_radio_active());
  REQUIRE(f.hal.scan_calls == 1);
  REQUIRE(f.prov.next_deadline_ms() != 0); // idle timer armed

  f.prov.stop();
}

TEST_CASE("PortableProv: credentials request connects after radio is up", "[portable_prov]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.attach());

  f.cred_char()->simulate_write(R"({"ssid":"HomeWiFi","password":"secret12"})");
  REQUIRE(f.hal.connect_calls == 0); // not on the NimBLE path

  f.prov.handle_pending_request();

  REQUIRE(f.prov.is_radio_active());
  REQUIRE(f.hal.connect_calls == 1);
  REQUIRE(f.hal.last_ssid == "HomeWiFi");

  f.prov.stop();
}

TEST_CASE("PortableProv: ensure_wifi_ready failure rejects request (no radio)", "[portable_prov]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.attach());

  f.hal.fail_set_mode_for = WifiMode::Sta; // one-shot failure
  f.scan_char()->simulate_write("1");
  f.prov.handle_pending_request();

  REQUIRE_FALSE(f.prov.is_radio_active());
  REQUIRE(f.hal.scan_calls == 0);
  REQUIRE(f.prov.next_deadline_ms() == 0);

  f.prov.stop();
}

TEST_CASE("PortableProv: on_connected drops radio and re-opens session", "[portable_prov]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.attach());

  f.cred_char()->simulate_write(R"({"ssid":"HomeWiFi","password":"secret12"})");
  f.prov.handle_pending_request();
  REQUIRE(f.prov.is_radio_active());

  // Simulate got IP → manager reaches Connected → orchestrator calls on_connected.
  f.hal.fire_got_ip(0x0100007f);

  f.prov.on_connected();
  REQUIRE_FALSE(f.prov.is_radio_active());
  REQUIRE(f.hal.get_mode() == WifiMode::Off);
  REQUIRE(f.prov.next_deadline_ms() == 0);

  // Session re-opened: a later credentials request is accepted again.
  f.cred_char()->simulate_write(R"({"ssid":"Other","password":"pw345678"})");
  f.prov.handle_pending_request();
  REQUIRE(f.hal.connect_calls == 2);

  f.prov.stop();
}

TEST_CASE("PortableProv: radio-idle timeout drops the radio while client connected",
          "[portable_prov]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.attach());

  f.rtos.now_ms = 1000;
  f.scan_char()->simulate_write("1");
  f.prov.handle_pending_request();
  REQUIRE(f.prov.is_radio_active());
  const uint32_t deadline = f.prov.next_deadline_ms();
  REQUIRE(deadline == 1000 + 90000);

  // Before deadline: no drop.
  f.rtos.now_ms = deadline - 1;
  f.prov.tick(static_cast<uint32_t>(f.rtos.now_ms));
  REQUIRE(f.prov.is_radio_active());

  // At/after deadline: radio dropped.
  f.rtos.now_ms = deadline;
  f.prov.tick(static_cast<uint32_t>(f.rtos.now_ms));
  REQUIRE_FALSE(f.prov.is_radio_active());
  REQUIRE(f.prov.next_deadline_ms() == 0);

  f.prov.stop();
}

TEST_CASE("PortableProv: on_ble_disconnected drops the radio", "[portable_prov]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.attach());

  f.scan_char()->simulate_write("1");
  f.prov.handle_pending_request();
  REQUIRE(f.prov.is_radio_active());

  f.prov.on_ble_disconnected();
  REQUIRE_FALSE(f.prov.is_radio_active());

  // Disconnect while radio off is a no-op.
  f.prov.on_ble_disconnected();
  REQUIRE_FALSE(f.prov.is_radio_active());

  f.prov.stop();
}

TEST_CASE("PortableProv: stop does NOT deinit the borrowed server", "[portable_prov]") {
  Fixture f;
  REQUIRE(f.ble.init("AirGradient Go ef0e"));
  REQUIRE(f.prov.attach());

  f.scan_char()->simulate_write("1");
  f.prov.handle_pending_request();
  REQUIRE(f.prov.is_radio_active());

  f.prov.stop();
  REQUIRE_FALSE(f.prov.is_attached());
  REQUIRE_FALSE(f.prov.is_radio_active());
  REQUIRE(f.hal.get_mode() == WifiMode::Off);
  REQUIRE(f.ble.deinit_count == 0); // owner deinits, not the provisioner
}
