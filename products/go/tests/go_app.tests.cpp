/**
 * AirGradient Go — GoApp Tests
 *
 * Tests for boot path selection, execute_fast_path(), and pure utility
 * functions.  Uses MockBoard + link-time stubs (same pattern as orchestrator
 * tests).
 */

#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <string>
#include <vector>

#include "go_app.h"
#include "go_board.h"
#include "go_ble_advertisement.h"
#include "hal/ble_server.h"
#include "go_events.h"
#include "go_local_api.h"
#include "buzzer/go_buzzer.h"
#include "led/go_led.h"
#include "go_power.h"
#include "go_storage.h"
#include "gps/gps_service.h"
#include "nand_storage.h"
#include "services/ag_client.h"
#include "services/local_server.h"
#include "services/payload_cache.h"
#include "services/sensor_manager.h"

#include "cap_touch_sensor.h"
#include "config_store.h"
#include "hal/bms_device.h"
#include "rtos.h"

// ============================================================================
// RTOS setup — required for RTOS::get_time_ms() and delay_ms() on host
// ============================================================================

class TestClock : public FreeRTOS {
public:
  uint64_t now = 0;
  std::function<void()> on_delay;
  uint64_t get_time_ms_impl() override { return now; }
  void delay_ms_impl(uint32_t ms) override {
    now += ms;
    if (on_delay) {
      on_delay();
    }
  }
};
static TestClock s_rtos;

struct GlobalSetup {
  GlobalSetup() { RTOS::set_instance(&s_rtos); }
  ~GlobalSetup() { RTOS::set_instance(nullptr); }
};

static GlobalSetup s_global_setup;

// Opt-in retained clock override; existing tests keep their normal host RTOS.
class ScopedRetainedClock : public TestClock {
public:
  explicit ScopedRetainedClock(uint64_t now_ms) : retained_ms(now_ms) { RTOS::set_instance(this); }
  ~ScopedRetainedClock() { RTOS::set_instance(&s_rtos); }

  uint64_t get_retained_time_ms_impl() override { return retained_ms; }

  uint64_t retained_ms;
};

// ============================================================================
// External test_spy (from go_app_stubs.cpp)
// ============================================================================

namespace test_spy {
extern RtcAppState rtc_state;
extern RtcDisplaySnapshot rtc_snapshot;
extern bool rtc_snapshot_valid;
extern int warmup_step_count;
extern std::vector<std::string> fast_path_events;
extern bool gas_begin_ok;
extern TVOCNOxData gas_sample;
extern int gas_read_count;
extern int gas_fail_read;
extern int gas_cancel_count;
extern bool light_sleep_failure;
extern int light_sleep_fallback_count;
extern bool fast_path_timer_only;
extern int ext_watchdog_feed_count;
extern uint32_t gps_read_duration_ms;
extern bool pm_sleep_ok;
extern std::function<void()> after_pm_sleep;
extern std::function<void()> after_cache;
extern std::function<void()> before_sensor_measurement;
extern SensorGroup last_sensor_groups;
extern int pm_sleep_count;
extern Measures measures_to_return;
extern bool sensor_started;
extern bool gps_started;
extern bool gps_sleep_requested;
extern bool gps_sleeping_started;
extern bool gps_read_called;
extern uint32_t gps_sleep_duration_ms;
extern uint32_t gps_sleep_request_count;
extern bool gps_sleep_ok;
extern uint32_t gps_remaining_ms;
extern int gps_initial_baud;
extern uint64_t gps_initial_deadline_ms;
extern GpsData gps_data_to_return;
extern bool input_started;
extern bool cache_measurement_called;
extern MeasuresAGo last_cached_measurement;
extern bool route_started;
extern bool create_route_result;
extern bool route_exists_result;
extern std::function<void()> before_create_route;
extern bool route_resumed;
extern uint32_t route_session_id;
extern bool route_point_appended;
extern RoutePoint last_route_point;
extern bool resume_route_result;
extern bool append_route_point_result;
extern bool append_route_point_sync_failure;
extern uint32_t route_point_count;
extern bool route_ended;
extern bool cache_backed_up;
extern bool bms_polled;
extern int bms_poll_count;
extern bool state_saved;
extern RtcAppState last_saved_state;
extern PowerSnapshot snapshot_to_return;
extern PowerService::SleepDecision sleep_decision_to_return;
extern uint32_t last_sleep_awake_ms;
extern bool enter_sleep_called;
extern uint32_t enter_sleep_duration_ms;
extern bool should_hold_pm_result;
extern bool shutdown_called;
extern bool power_on_reset;
extern int power_button_level;
extern bool pm_power_enabled;
extern bool ext_watchdog_fed;
extern bool lp_feeder_started;
extern bool shutdown_prepared;
extern int light_sleep_count;
extern uint32_t light_sleep_duration_ms;
extern bool light_sleep_prepared;
extern std::function<bool()> light_sleep_callback;
extern WakeCause wake_cause;
extern bool orchestrator_init_called;
extern bool orchestrator_run_called;
extern WakeCause orchestrator_wake_cause;
extern BootHandoff orchestrator_handoff;
extern RtcAppState orchestrator_rtc_state;
extern RtosQueueHandle orchestrator_event_queue;
extern GoLocalApiService *orchestrator_local_api;
extern SystemInfo orchestrator_local_api_system_info;
extern LocalServer *wifi_local_server;
extern LocalServer *generic_local_server;
extern std::string wifi_serial_number;
extern std::string wifi_firmware_version;
extern std::string wifi_model;
extern std::string wifi_ap_ssid;
extern std::string wifi_hostname;
extern uint16_t wifi_http_port;
extern HttpServer *generic_local_http;
extern MeasuresProvider *generic_local_measures;
extern ConfigProvider *generic_local_config;
extern ActionHandler *generic_local_actions;
extern ConfigAccess generic_local_config_access;
extern uint32_t serial_command_start_count;
extern float bms_battery_pct;
extern void reset();
} // namespace test_spy

// ============================================================================
// Stub BmsDevice for MockBoard
// ============================================================================

class StubBmsDevice : public BmsDevice {
public:
  BmsStatus status{};
  int status_failures_remaining = 0;
  int status_reads = 0;
  std::function<bool(BmsStatus &)> status_callback;
  bool init() override { return true; }
  bool read_telemetry(BmsTelemetry &out) override {
    out = BmsTelemetry{};
    return true;
  }
  bool read_status(BmsStatus &out) override {
    ++status_reads;
    out = status;
    if (status_callback) {
      return status_callback(out);
    }
    if (status_failures_remaining > 0) {
      --status_failures_remaining;
      return false;
    }
    return true;
  }
  bool get_charging_state(BmsChargingState &state) override {
    state = BmsChargingState::NotCharging;
    return true;
  }
  bool get_battery_percentage(float *pct) override {
    *pct = test_spy::bms_battery_pct;
    return true;
  }
  bool update_watchdog() override { return true; }
  bool feature_ship_available() const override { return true; }
  bool enter_ship_mode() override { return true; }
  bool configure_pmid_mode(BmsPmidMode) override { return true; }
  bool set_pmid_enabled(bool) override { return true; }
  bool resync_pmid() override { return true; }
  bool set_charge_enable(bool) override { return true; }
  bool set_charge_current_ma(uint16_t) override { return true; }
  bool set_watchdog_timeout_ms(uint32_t) override { return true; }
};

// ============================================================================
// Stub CapTouchSensor for MockBoard
// ============================================================================

class StubTouch : public CapTouchSensor {
public:
  bool init() override { return true; }
  bool read(TouchData &out) override {
    out = {};
    return true;
  }
};

// ============================================================================
// Stub GPIO Hal
// ============================================================================

static bool stub_configure(int, gpio::Mode, gpio::PullMode, gpio::InterruptType) { return true; }
static int stub_get_level(int) { return test_spy::power_button_level; }
static bool stub_set_level(int, int) { return true; }
static bool stub_add_handler(int, gpio::InterruptHandler, void *) { return true; }
static bool stub_remove_handler(int) { return true; }
static bool stub_enable_int(int) { return true; }
static bool stub_disable_int(int) { return true; }

static const gpio::Hal stub_gpio_hal = {
    .configure = stub_configure,
    .get_level = stub_get_level,
    .set_level = stub_set_level,
    .add_interrupt_handler = stub_add_handler,
    .remove_interrupt_handler = stub_remove_handler,
    .enable_interrupt = stub_enable_int,
    .disable_interrupt = stub_disable_int,
};

// ============================================================================
// MockBoard
// ============================================================================

class EmptyConfigStore : public ConfigStore {
public:
  bool tracking_start = false;
  bool fail_tracking_write = false;
  bool fail_commit = false;
  bool tracking_committed = false;
  ConfigStoreResult get_int(const char *, int &) override { return ConfigStoreResult::NOT_FOUND; }
  ConfigStoreResult get_bool(const char *key, bool &out) override {
    if (std::string(key) == "trk_start" && tracking_start) {
      out = true;
      return ConfigStoreResult::OK;
    }
    return ConfigStoreResult::NOT_FOUND;
  }
  ConfigStoreResult get_string(const char *, std::string &) override {
    return ConfigStoreResult::NOT_FOUND;
  }
  ConfigStoreResult get_float(const char *, float &) override {
    return ConfigStoreResult::NOT_FOUND;
  }
  ConfigStoreResult set_int(const char *, int) override { return ConfigStoreResult::ERROR; }
  ConfigStoreResult set_bool(const char *key, bool value) override {
    if (std::string(key) != "trk_start" || fail_tracking_write) {
      return ConfigStoreResult::ERROR;
    }
    tracking_start = value;
    return ConfigStoreResult::OK;
  }
  ConfigStoreResult set_string(const char *, const std::string &) override {
    return ConfigStoreResult::ERROR;
  }
  ConfigStoreResult set_float(const char *, float) override { return ConfigStoreResult::ERROR; }
  ConfigStoreResult erase(const char *) override { return ConfigStoreResult::ERROR; }
  ConfigStoreResult commit() override {
    tracking_committed = !fail_commit;
    return fail_commit ? ConfigStoreResult::ERROR : ConfigStoreResult::OK;
  }
};

class MockBleServer : public AgBleServer {
public:
  std::vector<std::string> calls;
  std::string fail_at;
  std::string name;
  std::string uuid;
  std::vector<uint8_t> payload;
  uint32_t interval_us = 0;
  uint32_t init_delay_ms = 0;
  uint32_t deinit_delay_ms = 0;
  uint64_t started_ms = 0;
  uint64_t stopped_ms = 0;
  bool active = false;
  bool initialized = false;

  bool init(const char *value) override {
    calls.push_back("init");
    name = value;
    RTOS::delay_ms(init_delay_ms);
    initialized = fail_at != "init";
    return initialized;
  }
  void deinit() override {
    calls.push_back("deinit");
    RTOS::delay_ms(deinit_delay_ms);
    active = initialized = false;
  }
  bool set_service_broadcast(const char *value, const uint8_t *data, size_t len) override {
    calls.push_back("payload");
    uuid = value;
    payload.assign(data, data + len);
    return fail_at != "payload";
  }
  bool set_advertising_interval(uint32_t value) override {
    calls.push_back("interval");
    interval_us = value;
    return fail_at != "interval";
  }
  bool start_advertising() override {
    calls.push_back("start");
    CHECK(test_spy::cache_backed_up);
    CHECK(test_spy::bms_polled);
    active = fail_at != "start";
    started_ms = RTOS::get_time_ms();
    return active;
  }
  bool stop_advertising() override {
    calls.push_back("stop");
    stopped_ms = RTOS::get_time_ms();
    if (fail_at == "stop") {
      return false;
    }
    active = false;
    return true;
  }
  bool set_security(AgBleIoCapability, uint8_t) override { return true; }
  bool delete_all_bonds() override { return true; }
  AgBleGattService *add_service(const char *) override { return nullptr; }
  bool set_advertising_name(const char *) override { return true; }
  bool add_advertised_service_uuid(const char *) override { return true; }
  bool set_manufacturer_data(const uint8_t *, size_t) override { return true; }
  void set_connect_callback(AgBleConnectCallback) override {}
  void set_disconnect_callback(AgBleDisconnectCallback) override {}
  void set_passkey_display_callback(AgBlePasskeyDisplayCallback) override {}
  void set_auth_complete_callback(AgBleAuthCompleteCallback) override {}
};

class MockBoard : public GoBoard {
public:
  bool sleep_touch() override {
    call_log.push_back("sleep_touch");
    return true;
  }
  // Call ordering log — records every method call in sequence.
  std::vector<std::string> call_log;

  // Init tracking
  bool nvs_init_called = false;
  bool buses_init_called = false;
  bool spi_init_called = false;
  bool fuel_gauge_init_called = false;
  bool bms_init_called = false;
  int bms_init_attempts = 0;
  int bms_failures_remaining = 0;
  std::function<void()> bms_init_callback;
  bool core_init_called = false;
  bool sensors_warm_arg = false;
  bool sensors_called = false;
  bool restart_called = false;

  // ISR
  volatile bool *isr_flag = nullptr;
  int isr_pin = -1;
  bool isr_installed = false;
  bool isr_removed = false;
  bool press_button_on_isr_install = false;
  bool return_null_bms = false;

  // GPS
  bool new_gps_driver_called = false;

  // Init methods
  void init_nvs() override {
    call_log.push_back("init_nvs");
    nvs_init_called = true;
  }
  void init_buses() override {
    call_log.push_back("init_buses");
    buses_init_called = true;
  }
  void init_spi() override {
    call_log.push_back("init_spi");
    spi_init_called = true;
  }
  void init_fuel_gauge() override {
    call_log.push_back("init_fuel_gauge");
    fuel_gauge_init_called = true;
  }
  bool init_bms() override {
    call_log.push_back("init_bms");
    bms_init_called = true;
    if (bms_init_callback) {
      bms_init_callback();
    }
    if (bms_available) {
      return true;
    }
    ++bms_init_attempts;
    if (bms_failures_remaining > 0) {
      --bms_failures_remaining;
      bms_available = false;
      return false;
    }
    bms_available = true;
    _power.set_bms(&_bms);
    return true;
  }
  void init_core() override {
    call_log.push_back("init_core");
    core_init_called = true;
    init_nvs();
    init_buses();
    init_spi();
  }

  // Radio subsystem init.  CP1 does not exercise this from GoApp — it is
  // driven by the orchestrator on Stationary entry — but the override
  // must exist so MockBoard is concrete.
  bool wifi_subsystem_init_called = false;
  void init_wifi_subsystem() override {
    call_log.push_back("init_wifi_subsystem");
    wifi_subsystem_init_called = true;
  }

  // Service accessors
  ConfigStore &config_store() override {
    call_log.push_back("config_store");
    return _config_store;
  }
  GoSettings load_settings() override {
    call_log.push_back("load_settings");
    return settings;
  }
  BmsDevice *bms() override {
    call_log.push_back("bms");
    return bms_available && !return_null_bms ? &_bms : nullptr;
  }
  uint32_t sensor_init_ms = 0;
  SensorManager &sensors(bool warm) override {
    call_log.push_back("sensors");
    sensors_warm_arg = warm;
    RTOS::delay_ms(sensor_init_ms);
    sensors_called = true;
    return _sensor_manager;
  }
  StorageService &storage() override {
    call_log.push_back("storage");
    return _storage;
  }
  DisplayService &display() override {
    call_log.push_back("display");
    return _display;
  }
  LedService &led_service() override {
    call_log.push_back("led_service");
    return _led;
  }
  BuzzerService &buzzer_service() override {
    call_log.push_back("buzzer_service");
    return _buzzer;
  }
  PowerService &power() override {
    call_log.push_back("power");
    return _power;
  }

  // Network accessors return reinterpret_cast'd dummies.
  // Stationary entry — which would dereference these — is not exercised
  // until CP2.
  WifiHal &wifi_hal() override {
    call_log.push_back("wifi_hal");
    return *reinterpret_cast<WifiHal *>(&_wifi_hal_buf);
  }
  WifiManager &wifi_manager() override {
    call_log.push_back("wifi_manager");
    return *reinterpret_cast<WifiManager *>(&_wifi_manager_buf);
  }
  HttpServer &http_server() override {
    call_log.push_back("http_server");
    return *reinterpret_cast<HttpServer *>(&_http_server_buf);
  }
  AgBleServer &ble_server() override {
    call_log.push_back("ble_server");
    return ble;
  }
  AgClient &ag_client() override {
    call_log.push_back("ag_client");
    return _ag_client;
  }

  GpsDriver *new_gps_driver() override {
    call_log.push_back("new_gps_driver");
    new_gps_driver_called = true;
    return reinterpret_cast<GpsDriver *>(&_gps_driver_buf);
  }
  CapTouchSensor *new_touch_sensor() override {
    call_log.push_back("new_touch_sensor");
    return &_touch;
  }
  AccelSensor *new_accel_sensor() override {
    call_log.push_back("new_accel_sensor");
    return nullptr;
  }

  BoardVariant variant() const override { return BoardVariant::Prototype; }
  std::string serial_number() override { return "test-serial"; }
  const char *firmware_version() override { return "0.0.0-test"; }
  const gpio::Hal &gpio_hal() override {
    call_log.push_back("gpio_hal");
    return stub_gpio_hal;
  }
  void release_gpio_holds() override { call_log.push_back("release_gpio_holds"); }
  void ulp_stop() override {
    call_log.push_back("ulp_stop");
    test_spy::lp_feeder_started = false;
  }
  void ulp_start() override {
    call_log.push_back("ulp_start");
    test_spy::lp_feeder_started = true;
  }
  void restart() override {
    call_log.push_back("restart");
    restart_called = true;
  }

  void install_button_isr(int pin, volatile bool *flag) override {
    isr_pin = pin;
    isr_flag = flag;
    isr_installed = true;
    if (press_button_on_isr_install) {
      *flag = true;
    }
  }
  void remove_button_isr(int /*pin*/) override {
    isr_removed = true;
    isr_flag = nullptr;
  }

  // --- Test helpers ---
  void press_button() {
    if (isr_flag)
      *isr_flag = true;
  }

  /// Find index of first occurrence of name in call_log.
  /// Returns -1 if not found.
  int call_index(const std::string &name) const {
    for (size_t i = 0; i < call_log.size(); i++) {
      if (call_log[i] == name)
        return static_cast<int>(i);
    }
    return -1;
  }

  // Configurable test state
  GoSettings settings{};
  MockBleServer ble;
  bool bms_available = false;
  StubBmsDevice _bms;

private:
  // Stub service instances.
  // The stub constructors store refs but never dereference them, so
  // we use a helper to produce "valid" dummy references for construction.
  Sensors _sensors_struct{};
  SensorManager _sensor_manager{_sensors_struct};
  StubTouch _touch;

  // Dummy byte arrays used to create "valid" references for stub constructors.
  // The stubs never actually use these objects.
  alignas(PayloadCache) static inline char s_cache_buf[sizeof(PayloadCache)];
  alignas(NandStorage) static inline char s_nand_buf[sizeof(NandStorage)];
  EmptyConfigStore _config_store;
  alignas(8) static inline char _gps_driver_buf[512];
  // Radio dummy buffers — never dereferenced through the abstract types.
  alignas(8) static inline char _wifi_hal_buf[64];
  alignas(8) static inline char _wifi_manager_buf[64];
  alignas(8) static inline char _http_server_buf[64];
  AgClient _ag_client{};

  StorageService _storage{*reinterpret_cast<PayloadCache *>(s_cache_buf),
                          *reinterpret_cast<NandStorage *>(s_nand_buf)};
  DisplayService _display{{}};
  LedService _led{{}};       // inert mode (null driver)
  BuzzerService _buzzer{{}}; // inert mode (null driver)
  PowerService _power{nullptr, stub_gpio_hal, {}};
};

// ============================================================================
// GoAppTestAccess (friend class)
// ============================================================================

class GoAppTestAccess {
public:
  // Re-export private types for test assertions
  using FastPathResult = GoApp::FastPathResult;
  using Outcome = GoApp::FastPathResult::Outcome;

  explicit GoAppTestAccess(GoApp &app) : _app(app) {}

  bool detect_charging_only_boot() {
    BmsStatus status{};
    return _app.detect_charging_only_boot(status);
  }

  FastPathResult execute_fast_path(const RtcAppState &state, const volatile bool &button,
                                   const RtcDisplaySnapshot *snapshot = nullptr,
                                   bool snapshot_valid = false) {
    return _app.execute_fast_path(state, button, snapshot, snapshot_valid);
  }

  void run_button_wake_path(const RtcAppState &state) { _app.run_button_wake_path(state); }

  void run_fast_path(const RtcAppState &state) { _app.run_fast_path(state); }

  void run_interactive(WakeCause cause, BootHandoff handoff = {}, RtcAppState state = {}) {
    _app.run_interactive(cause, state, handoff);
  }

private:
  GoApp &_app;
};

// ============================================================================
// Tests: select_boot_path
// ============================================================================

TEST_CASE("select_boot_path: Timer + Locked -> FastPath") {
  RtcAppState state{};
  state.lock_state = LockState::Locked;
  CHECK(select_boot_path(WakeCause::Timer, state, false) == BootPath::FastPath);
}

TEST_CASE("select_boot_path: Timer + Unlocked -> Interactive") {
  RtcAppState state{};
  state.lock_state = LockState::Unlocked;
  CHECK(select_boot_path(WakeCause::Timer, state, false) == BootPath::Interactive);
}

TEST_CASE("select_boot_path: Button + Offline -> ButtonWake") {
  RtcAppState state{};
  state.mode = OperatingMode::Offline;
  CHECK(select_boot_path(WakeCause::Button, state, false) == BootPath::ButtonWake);
}

TEST_CASE("select_boot_path: Button + Portable -> Interactive") {
  RtcAppState state{};
  state.mode = OperatingMode::Portable;
  CHECK(select_boot_path(WakeCause::Button, state, false) == BootPath::Interactive);
}

TEST_CASE("select_boot_path: PowerOn -> Interactive") {
  RtcAppState state{};
  CHECK(select_boot_path(WakeCause::PowerOn, state, false) == BootPath::Interactive);
}

TEST_CASE("select_boot_path: eligible USB cold boot -> ChargingOnly") {
  RtcAppState state{};
  CHECK(select_boot_path(WakeCause::PowerOn, state, true) == BootPath::ChargingOnly);
}

TEST_CASE("select_boot_path: shutdown request overrides timer fast path") {
  RtcAppState state{};
  state.set_flag(RtcAppFlag::ChargingOnlyRequested, true);
  CHECK(select_boot_path(WakeCause::Timer, state, false) == BootPath::ChargingOnly);
}

// ============================================================================
// Tests: charging-only boot
// ============================================================================

TEST_CASE("Charging-only detection: probes eligibility without running a boot path") {
  test_spy::reset();
  test_spy::power_on_reset = true;
  MockBoard board;
  board._bms.status.power_source = BmsPowerSource::UsbDcp;
  GoApp app(board);
  GoAppTestAccess access(app);

  SECTION("button released") { test_spy::power_button_level = 1; }
  SECTION("button held") { test_spy::power_button_level = 0; }

  REQUIRE(access.detect_charging_only_boot());
  CHECK(board.bms_init_attempts == 1);
  CHECK(DisplayService::spy_init_count == 0);
  CHECK(test_spy::light_sleep_count == 0);
  CHECK_FALSE(test_spy::shutdown_called);
  CHECK_FALSE(test_spy::orchestrator_run_called);
  CHECK_FALSE(board.sensors_called);
  CHECK(board.call_index("gpio_hal") == -1);
  CHECK_FALSE(board.isr_installed);
}

TEST_CASE("GoApp::run: ineligible charging boots start the application normally") {
  test_spy::reset();
  MockBoard board;
  SECTION("software reset with USB connected") {
    board._bms.status.power_source = BmsPowerSource::UsbDcp;
  }
  SECTION("cold boot on battery") {
    test_spy::power_on_reset = true;
    board._bms.status.power_source = BmsPowerSource::None;
  }
  GoApp app(board);
  app.run();

  CHECK(test_spy::orchestrator_run_called);
  CHECK(test_spy::orchestrator_wake_cause == WakeCause::PowerOn);
  CHECK(test_spy::light_sleep_count == 0);
  CHECK_FALSE(test_spy::shutdown_called);
  CHECK(board.bms_init_attempts == 1);
  CHECK(board.isr_flag == nullptr);
}

TEST_CASE("Charging-only boot: light sleep retains charger until unplug, then paints before ship") {
  for (auto source : {BmsPowerSource::UsbSdp, BmsPowerSource::UsbCdp, BmsPowerSource::UsbDcp,
                      BmsPowerSource::UnknownAdapter, BmsPowerSource::NonStandard}) {
    for (auto charge : {BmsChargingState::FastCharge, BmsChargingState::NotCharging,
                        BmsChargingState::ChargeTerminationDone}) {
      test_spy::reset();
      test_spy::power_on_reset = true;
      MockBoard board;
      board._bms.status.power_source = source;
      board._bms.status.charging_state = charge;
      test_spy::light_sleep_callback = [&] {
        if (test_spy::light_sleep_count == 2) {
          board._bms.status.power_source = BmsPowerSource::None;
        }
        CHECK(DisplayService::spy_last_screen == (charge == BmsChargingState::ChargeTerminationDone
                                                      ? Screen::ShutdownCharged
                                                      : Screen::ShutdownCharging));
        CHECK(DisplayService::spy_sync_update_count == 0);
        return false;
      };
      GoApp app(board);
      app.run();
      CHECK(test_spy::shutdown_called);
      CHECK(test_spy::shutdown_prepared);
      CHECK(DisplayService::spy_last_screen == Screen::ShutdownUser);
      CHECK(DisplayService::spy_sync_update_count == 1);
      CHECK(test_spy::light_sleep_count == 2);
      CHECK(test_spy::light_sleep_duration_ms == 2000);
      CHECK(test_spy::light_sleep_prepared);
      CHECK(board.bms_init_attempts == 1);
      CHECK(board._bms.status_reads == 3);
      CHECK(board.call_index("ulp_start") == -1);
      CHECK_FALSE(board.isr_installed);
      CHECK_FALSE(board.sensors_called);
      CHECK_FALSE(board.new_gps_driver_called);
      CHECK_FALSE(test_spy::gps_sleep_requested);
      CHECK_FALSE(test_spy::gps_read_called);
      CHECK_FALSE(board.wifi_subsystem_init_called);
      CHECK(board.call_index("storage") == -1);
      CHECK(board.call_index("ble_server") == -1);
      CHECK(board.call_index("led_service") == -1);
      CHECK(board.call_index("buzzer_service") == -1);
      CHECK_FALSE(test_spy::bms_polled);         // No runtime charging-policy poll.
      CHECK_FALSE(test_spy::enter_sleep_called); // No measurement timer sleep.
    }
  }
}

TEST_CASE("Charging-only polling: repaint only on completion and charging resumption") {
  for (auto resumed :
       {BmsChargingState::TrickleCharge, BmsChargingState::PreCharge, BmsChargingState::FastCharge,
        BmsChargingState::TaperCharge, BmsChargingState::TopOffTimerActiveCharging}) {
    test_spy::reset();
    test_spy::power_on_reset = true;
    MockBoard board;
    board._bms.status.power_source = BmsPowerSource::UsbDcp;
    board._bms.status.charging_state = BmsChargingState::FastCharge;
    test_spy::light_sleep_callback = [&] {
      const auto count = test_spy::light_sleep_count;
      REQUIRE(count <= 9);
      CHECK(DisplayService::spy_last_screen ==
            (count >= 3 && count <= 7 ? Screen::ShutdownCharged : Screen::ShutdownCharging));
      CHECK(DisplayService::spy_sync_update_count == (count <= 2 ? 0 : count <= 7 ? 1 : 2));
      switch (count) {
      case 1:
      case 6:
        board._bms.status.charging_state = BmsChargingState::NotCharging;
        break;
      case 2:
      case 3:
        board._bms.status.charging_state = BmsChargingState::ChargeTerminationDone;
        break;
      case 4:
        board._bms.status.charging_state = resumed;
        board._bms.status_failures_remaining = 1;
        break;
      case 5:
        board._bms.status.charging_state = BmsChargingState::Unknown;
        break;
      case 7:
      case 8:
        board._bms.status.charging_state = resumed;
        break;
      case 9:
        board._bms.status.power_source = BmsPowerSource::None;
        break;
      }
      return false;
    };
    GoApp app(board);
    app.run();

    CHECK(test_spy::shutdown_prepared);
    CHECK(DisplayService::spy_sync_update_count == 3); // Full, charging, powered off.
    CHECK(test_spy::light_sleep_prepared);
    CHECK(board.bms_init_attempts == 1);
    CHECK(board._bms.status_reads == 10);
  }
}

TEST_CASE("Charging-only shutdown handoff: consumes the request before entering the loop") {
  test_spy::reset();
  test_spy::wake_cause = WakeCause::Timer;
  test_spy::rtc_state.set_flag(RtcAppFlag::ChargingOnlyRequested, true);
  MockBoard board;
  board._bms.status.power_source = BmsPowerSource::UsbDcp;
  SECTION("charging") { board._bms.status.charging_state = BmsChargingState::FastCharge; }
  SECTION("full") { board._bms.status.charging_state = BmsChargingState::ChargeTerminationDone; }
  test_spy::light_sleep_callback = [&] {
    CHECK_FALSE(test_spy::rtc_state.has_flag(RtcAppFlag::ChargingOnlyRequested));
    CHECK(DisplayService::spy_init_count == 1);
    const bool full = board._bms.status.charging_state == BmsChargingState::ChargeTerminationDone;
    CHECK(DisplayService::spy_last_screen ==
          (full ? Screen::ShutdownCharged : Screen::ShutdownCharging));
    CHECK(DisplayService::spy_sync_update_count == 0);
    return true; // Power button restarts into normal operation.
  };
  GoApp app(board);
  app.run();
  CHECK_FALSE(test_spy::rtc_state.has_flag(RtcAppFlag::ChargingOnlyRequested));
  CHECK_FALSE(test_spy::orchestrator_run_called);
  CHECK_FALSE(board.sensors_called);
  CHECK(board.restart_called);
  CHECK_FALSE(test_spy::shutdown_called);
  CHECK(board.bms_init_attempts == 1);
  // The following software restart has no request and starts normally.
  test_spy::wake_cause = WakeCause::PowerOn;
  app.run();
  CHECK(test_spy::orchestrator_run_called);
  CHECK(test_spy::light_sleep_count == 1);
}

TEST_CASE("Charging-only boot: non-power-on resets leave the boot path untouched") {
  test_spy::reset();
  MockBoard board;
  board._bms.status.power_source = BmsPowerSource::UsbDcp;
  GoApp app(board);
  GoAppTestAccess access(app);

  CHECK_FALSE(access.detect_charging_only_boot());
  CHECK(board.call_log.empty());
  CHECK_FALSE(board.isr_installed);
  CHECK_FALSE(test_spy::shutdown_called);
}

TEST_CASE("Charging-only boot: unconfirmed USB input falls through to normal startup") {
  test_spy::reset();
  test_spy::power_on_reset = true;
  MockBoard board;
  board._bms.status.power_source = BmsPowerSource::UsbDcp;

  SECTION("battery") { board._bms.status.power_source = BmsPowerSource::None; }
  SECTION("OTG is not external power") { board._bms.status.power_source = BmsPowerSource::OtgMode; }
  SECTION("unknown status") { board._bms.status.power_source = BmsPowerSource::Unknown; }
  SECTION("failed reads cannot use populated status") { board._bms.status_failures_remaining = 10; }
  SECTION("charger init fails") { board.bms_failures_remaining = 10; }
  SECTION("null charger") { board.return_null_bms = true; }

  GoApp app(board);
  GoAppTestAccess access(app);
  CHECK_FALSE(access.detect_charging_only_boot());
  CHECK_FALSE(test_spy::shutdown_called);
  CHECK_FALSE(test_spy::lp_feeder_started);
  CHECK(DisplayService::spy_init_count == 0);
  CHECK(board._bms.status_reads <= 3);
  CHECK(board.bms_init_attempts <= 2);
  CHECK_FALSE(board.isr_installed);
  CHECK(board.call_index("gpio_hal") == -1);
}

TEST_CASE("Charging-only boot: transient charger failures recover within the boot") {
  test_spy::reset();
  test_spy::power_on_reset = true;
  MockBoard board;
  board._bms.status.power_source = BmsPowerSource::UsbDcp;
  board.bms_failures_remaining = 1;
  board._bms.status_failures_remaining = 1;
  test_spy::light_sleep_callback = [&] {
    board._bms.status.power_source = BmsPowerSource::None;
    return false;
  };
  GoApp app(board);
  app.run();
  CHECK(board.bms_init_attempts == 2);
  CHECK(board._bms.status_reads == 3); // Two boot attempts and the unplug poll.
  CHECK(test_spy::shutdown_prepared);
}

TEST_CASE("Charging-only boot: a startup button press does not override USB detection") {
  test_spy::reset();
  test_spy::power_on_reset = true;
  test_spy::power_button_level = 0;
  MockBoard board;
  board._bms.status.power_source = BmsPowerSource::UsbDcp;
  board._bms.status_callback = [&](BmsStatus &) {
    // Release the startup press during detection, before normal sleep wake is armed.
    test_spy::power_button_level = 1;
    return true;
  };
  test_spy::light_sleep_callback = [&] {
    CHECK(DisplayService::spy_last_screen == Screen::ShutdownCharging);
    board._bms.status.power_source = BmsPowerSource::None;
    return false;
  };

  GoApp app(board);
  app.run();
  CHECK_FALSE(board.isr_installed);
  CHECK(test_spy::shutdown_prepared);
  CHECK_FALSE(test_spy::lp_feeder_started);
  CHECK(test_spy::light_sleep_count == 1);
  CHECK_FALSE(test_spy::orchestrator_run_called);
}

TEST_CASE("Charging-only button wake: restarts for normal startup") {
  test_spy::reset();
  test_spy::power_on_reset = true;
  MockBoard board;
  board._bms.status.power_source = BmsPowerSource::UsbDcp;
  SECTION("charging") { board._bms.status.charging_state = BmsChargingState::FastCharge; }
  SECTION("fully charged") {
    board._bms.status.charging_state = BmsChargingState::ChargeTerminationDone;
  }
  test_spy::light_sleep_callback = [&] {
    if (test_spy::light_sleep_count != 3) {
      return false;
    }
    return true; // Button wake even if released before GPIO is sampled.
  };
  GoApp app(board);
  app.run();

  CHECK_FALSE(test_spy::shutdown_called);
  CHECK_FALSE(test_spy::lp_feeder_started);
  CHECK(board.bms_init_attempts == 1);
  CHECK(test_spy::light_sleep_count == 3);
  CHECK(test_spy::light_sleep_prepared);
  CHECK(DisplayService::spy_sync_update_count == 0);
  CHECK(board.restart_called);
  CHECK_FALSE(board.sensors_called);
  CHECK_FALSE(test_spy::orchestrator_run_called);
}

TEST_CASE("Charging-only polling: failed and unknown readings keep the application off") {
  test_spy::reset();
  test_spy::power_on_reset = true;
  MockBoard board;
  board._bms.status.power_source = BmsPowerSource::UsbDcp;
  test_spy::light_sleep_callback = [&] {
    REQUIRE(test_spy::light_sleep_count <= 4);
    CHECK_FALSE(test_spy::shutdown_called);
    CHECK(DisplayService::spy_last_screen == Screen::ShutdownCharging);
    switch (test_spy::light_sleep_count) {
    case 1:
      board._bms.status.power_source = BmsPowerSource::None;
      board._bms.status_failures_remaining = 1;
      break;
    case 2:
      board._bms.status.power_source = BmsPowerSource::Unknown;
      break;
    case 3:
      board._bms.status.power_source = BmsPowerSource::UsbDcp;
      break;
    default:
      board._bms.status.power_source = BmsPowerSource::None;
      break;
    }
    return false;
  };
  GoApp app(board);
  app.run();
  CHECK(test_spy::shutdown_prepared);
  CHECK(test_spy::light_sleep_count == 4);
  CHECK(test_spy::light_sleep_prepared);
  CHECK(board.bms_init_attempts == 1);
  CHECK_FALSE(board.sensors_called);
}

// ============================================================================
// Tests: is_gps_active_at_boot
// ============================================================================

TEST_CASE("is_gps_active_at_boot: AlwaysOn -> true") {
  GoSettings s{};
  s.gps_mode = GpsMode::AlwaysOn;
  RtcAppState state{};
  CHECK(is_gps_active_at_boot(s, state) == true);
}

TEST_CASE("is_gps_active_at_boot: AlwaysOff -> false") {
  GoSettings s{};
  s.gps_mode = GpsMode::AlwaysOff;
  RtcAppState state{};
  state.tracking_state = TrackingState::Recording;
  CHECK(is_gps_active_at_boot(s, state) == false);
}

TEST_CASE("is_gps_active_at_boot: OnWhenTracking + tracking -> true") {
  GoSettings s{};
  s.gps_mode = GpsMode::OnWhenTracking;
  RtcAppState state{};
  state.tracking_state = TrackingState::Recording;
  CHECK(is_gps_active_at_boot(s, state) == true);
}

TEST_CASE("is_gps_active_at_boot: OnWhenTracking + idle -> false") {
  GoSettings s{};
  s.gps_mode = GpsMode::OnWhenTracking;
  RtcAppState state{};
  state.tracking_state = TrackingState::Idle;
  CHECK(is_gps_active_at_boot(s, state) == false);
}

// ============================================================================
// Tests: measures_to_ago
// ============================================================================

TEST_CASE("measures_to_ago: maps valid fields") {
  Measures m{};
  m.co2.co2 = 450;
  m.temp_hum_a.temperature = 22.5f;
  m.temp_hum_a.humidity = 55.0f;
  m.pm_a.pm_25 = 12.3f;
  m.tvoc_nox.tvoc_index = 120;
  m.tvoc_nox.tvoc_raw = 25000;
  m.tvoc_nox.nox_index = 30;
  m.tvoc_nox.nox_raw = 18000;
  m.pressure.pressure = 1013.25f;

  MeasuresAGo ago = measures_to_ago(m);

  CHECK(ago.co2.co2 == 450);
  CHECK(ago.temp_hum_a.temperature == 22.5f);
  CHECK(ago.temp_hum_a.humidity == 55.0f);
  CHECK(ago.pm_a.pm_25 == 12.3f);
  // measures_to_ago passes through tvoc_nox fields as-is (no raw-to-index copy)
  CHECK(ago.tvoc_nox.tvoc_index == 120);
  CHECK(ago.tvoc_nox.tvoc_raw == 25000);
  CHECK(ago.tvoc_nox.nox_index == 30);
  CHECK(ago.tvoc_nox.nox_raw == 18000);
  CHECK(ago.pressure.pressure == 1013.25f);
}

TEST_CASE("measures_to_ago: invalid fields preserved") {
  Measures m{};
  // Set fields to explicitly invalid sentinel values
  m.co2.co2 = MeasuresInvalid::CO2;
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.pm_a.pm_25 = MeasuresInvalid::PM;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.tvoc_raw = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.tvoc_nox.nox_raw = MeasuresInvalid::NOX;
  m.pressure.pressure = MeasuresInvalid::PRESSURE;

  MeasuresAGo ago = measures_to_ago(m);

  CHECK(ago.co2.co2 == MeasuresInvalid::CO2);
  CHECK(ago.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
  CHECK(ago.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
  CHECK(ago.pm_a.pm_25 == MeasuresInvalid::PM);
  CHECK(ago.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
  CHECK(ago.tvoc_nox.tvoc_raw == MeasuresInvalid::TVOC);
  CHECK(ago.tvoc_nox.nox_index == MeasuresInvalid::NOX);
  CHECK(ago.tvoc_nox.nox_raw == MeasuresInvalid::NOX);
  CHECK(ago.pressure.pressure == MeasuresInvalid::PRESSURE);
}

// ============================================================================
// Tests: build_fast_path_display
// ============================================================================

TEST_CASE("build_fast_path_display: valid sensors -> values populated") {
  MeasuresAGo m{};
  m.co2.co2 = 600;
  m.pm_a.pm_25 = 15.0f;
  m.temp_hum_a.temperature = 25.0f;
  m.temp_hum_a.humidity = 60.0f;
  m.tvoc_nox.tvoc_index = 120;
  m.tvoc_nox.nox_index = 30;
  m.pressure.pressure = 1015.0f;
  m.pressure.altitude = 100.0f;

  GpsData gps{};
  PowerSnapshot bms{};
  bms.battery_percentage = 75.0f;
  GoSettings settings{};
  settings.use_fahrenheit = true;
  settings.use_feet = true;
  settings.pm_use_usaqi = true;

  DisplayValues v = build_fast_path_display(m, gps, bms, settings, TrackingState::Recording);

  CHECK(v.co2_ppm == 600);
  CHECK(v.pm25_ugm3 == 15.0f);
  CHECK(v.temperature_c == 25.0f);
  CHECK(v.humidity_pct == 60.0f);
  CHECK(v.battery_pct == 75);
  CHECK(v.locked == true);
  CHECK(v.tracking_state == TrackingState::Recording);
  CHECK(v.use_fahrenheit == true);
  CHECK(v.use_feet == true);
  CHECK(v.pm_use_usaqi == true);
}

TEST_CASE("build_fast_path_display: invalid sensors -> sentinels preserved") {
  MeasuresAGo m{};
  m.co2.co2 = MeasuresInvalid::CO2;
  m.pm_a.pm_25 = MeasuresInvalid::PM;
  m.temp_hum_a.temperature = MeasuresInvalid::TEMPERATURE;
  m.temp_hum_a.humidity = MeasuresInvalid::HUMIDITY;
  m.tvoc_nox.tvoc_index = MeasuresInvalid::TVOC;
  m.tvoc_nox.nox_index = MeasuresInvalid::NOX;
  m.pressure.pressure = MeasuresInvalid::PRESSURE;
  m.pressure.altitude = MeasuresInvalid::ALTITUDE;

  GpsData gps{};
  PowerSnapshot bms{};
  bms.battery_percentage = -1.0f; // no data
  GoSettings settings{};

  DisplayValues v = build_fast_path_display(m, gps, bms, settings, TrackingState::Idle);

  // Invalid sensor data should NOT overwrite DisplayValues defaults
  CHECK(v.co2_ppm == MeasuresInvalid::CO2);
  CHECK(v.pm25_ugm3 == MeasuresInvalid::PM);
  CHECK(v.temperature_c == MeasuresInvalid::TEMPERATURE);
  CHECK(v.humidity_pct == MeasuresInvalid::HUMIDITY);
  CHECK(v.tvoc_index == MeasuresInvalid::TVOC);
  CHECK(v.nox_index == MeasuresInvalid::NOX);
  CHECK(v.pressure_hpa == MeasuresInvalid::PRESSURE);
  CHECK(v.altitude_m == MeasuresInvalid::ALTITUDE);
  CHECK(v.battery_pct == 0xFF); // no battery data
  CHECK(v.gps_fix == false);
  CHECK(v.tracking_state == TrackingState::Idle);
  CHECK(v.use_fahrenheit == false);
  CHECK(v.use_feet == false);
  CHECK(v.pm_use_usaqi == false);
}

TEST_CASE("build_fast_path_display: applies persisted corrections only to presentation") {
  MeasuresAGo m{};
  m.pm_a.pm_25 = 10.0f;
  m.temp_hum_a.temperature = 20.0f;
  m.temp_hum_a.humidity = 50.0f;

  GoSettings settings{};
  settings.corrections.pm25.algorithm = Pm25CorrectionAlgorithm::CustomViaPm25Raw;
  settings.corrections.pm25.scaling_factor = 2.0f;
  settings.corrections.pm25.intercept = 1.0f;
  settings.corrections.temperature.algorithm = LinearCorrectionAlgorithm::Custom;
  settings.corrections.temperature.scaling_factor = 1.5f;
  settings.corrections.temperature.intercept = -1.0f;

  const DisplayValues values =
      build_fast_path_display(m, GpsData{}, PowerSnapshot{}, settings, TrackingState::Idle);

  CHECK(values.pm25_ugm3 == 21.0f);
  CHECK(values.temperature_c == 29.0f);
  CHECK(values.humidity_pct == 50.0f);
  CHECK(m.pm_a.pm_25 == 10.0f);
  CHECK(m.temp_hum_a.temperature == 20.0f);
}

// ============================================================================
// Tests: build_wake_values
// ============================================================================

TEST_CASE("build_wake_values: snapshot valid -> values seeded") {
  RtcDisplaySnapshot snap{};
  snap.co2_ppm = 500;
  snap.pm25_ugm3 = 10.0f;
  snap.temperature_c = 21.0f;
  snap.humidity_pct = 50.0f;
  snap.use_feet = true;

  DisplayValues v = build_wake_values(snap, true);

  CHECK(v.co2_ppm == 500);
  CHECK(v.pm25_ugm3 == 10.0f);
  CHECK(v.use_feet);
  CHECK(v.locked == false);
  CHECK(v.snackbar_text != nullptr);
}

TEST_CASE("build_wake_values: snapshot invalid -> defaults, unlocked") {
  RtcDisplaySnapshot snap{};

  DisplayValues v = build_wake_values(snap, false);

  CHECK(v.locked == false);
  CHECK(v.screen == Screen::Home);
  CHECK_FALSE(v.use_feet);
  CHECK(v.snackbar_text != nullptr);
}

// ============================================================================
// Tests: build_boot_splash_values
// ============================================================================

TEST_CASE("build_boot_splash_values: shows Booting on Screen::Info, locked") {
  DisplayValues v = build_boot_splash_values();

  CHECK(v.screen == Screen::Info);
  REQUIRE(v.info_text != nullptr);
  CHECK(std::string(v.info_text) == BOOT_SPLASH_TEXT);
  CHECK(v.locked == true);
  CHECK(v.display_off == false);
  // Sensor sentinels stay untouched — Info does not render them.
  CHECK(v.co2_ppm == MeasuresInvalid::CO2);
  CHECK(v.pm25_ugm3 == MeasuresInvalid::PM);
}

// ============================================================================
// Tests: execute_fast_path (via GoAppTestAccess)
// ============================================================================

TEST_CASE("execute_fast_path: warm sensors, measure, sleep") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};
  test_spy::should_hold_pm_result = true;

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(result.has_measures == true);
  CHECK(result.sleep_duration_ms == 60000);
  CHECK(result.sensors_warm == true);
  CHECK(board.sensors_warm_arg == true);
  CHECK(board.core_init_called == true);
  CHECK(test_spy::pm_sleep_count == 0); // held warm — sensor not slept
}

TEST_CASE("execute_fast_path: cold sensors full warmup, measure, sleep") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 30000};
  test_spy::should_hold_pm_result = false;

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, false);
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(result.has_measures == true);
  CHECK(result.sensors_warm == false);
  CHECK(board.sensors_warm_arg == false);
  CHECK(test_spy::pm_sleep_count == 1); // long sleep — fan stopped for the window
  // Warmup is elapsed time, not discarded sensor reads.
  CHECK(test_spy::warmup_step_count == 0);
  CHECK(test_spy::light_sleep_count > 0);
}

TEST_CASE("fast path BLE: advertises raw measurements despite configured corrections") {
  test_spy::reset();
  MockBoard board;
  board.settings.corrections.pm25.algorithm = Pm25CorrectionAlgorithm::CustomViaPm25Raw;
  board.settings.corrections.pm25.scaling_factor = 2.0f;
  board.settings.corrections.pm25.intercept = 1.0f;
  board.settings.corrections.temperature.algorithm = LinearCorrectionAlgorithm::Custom;
  board.settings.corrections.temperature.scaling_factor = 1.5f;
  board.settings.corrections.temperature.intercept = -1.0f;
  board.settings.corrections.humidity.algorithm = LinearCorrectionAlgorithm::Custom;
  board.settings.corrections.humidity.intercept = -10.0f;
  test_spy::measures_to_return.temp_hum_a = {20.0f, 50.6f};
  test_spy::measures_to_return.pm_a.pm_25 = 10.0f;
  test_spy::measures_to_return.co2.co2 = 400;
  test_spy::measures_to_return.pressure = {1013.25f, -42.5f};
  test_spy::snapshot_to_return.battery_percentage = 74.6f;
  GoApp app(board);
  GoAppTestAccess access(app);
  volatile bool button = false;

  const auto result = access.execute_fast_path(RtcAppState{}, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(board.ble.name.empty());
  CHECK(board.ble.uuid == GoBleAdvertisement::SERVICE_UUID);
  CHECK(board.ble.interval_us == 300000);
  CHECK(board.ble.payload == std::vector<uint8_t>{0x01, 0xD0, 0x07, 0x33, 0x64, 0x00, 0x90, 0x01,
                                                  0x95, 0x27, 0xD5, 0xFF, 0x4B});
  CHECK(board.ble.calls ==
        std::vector<std::string>{"init", "payload", "interval", "start", "stop", "deinit"});
  CHECK_FALSE(board.ble.initialized);
  CHECK_FALSE(board.ble.active);
  CHECK(test_spy::last_cached_measurement.pm_a.pm_25 == 10.0f);
  CHECK(test_spy::last_cached_measurement.temp_hum_a.temperature == 20.0f);
  CHECK(result.measures.pm_a.pm_25 == 10.0f);
  CHECK(test_spy::bms_poll_count == 1);
}

TEST_CASE("fast path BLE: window follows display duration and all overhead reaches sleep policy") {
  test_spy::reset();
  ScopedRetainedClock clock(0);
  MockBoard board;
  board.ble.init_delay_ms = 75;
  board.ble.deinit_delay_ms = 25;
  uint32_t display_ms = 3000;
  SECTION("normal refresh") {}
  SECTION("slow refresh") { display_ms = 4500; }
  SECTION("immediate display return adds no advertising wait") { display_ms = 0; }
  DisplayService::spy_init_delay_ms = display_ms;
  bool observed_display = false;
  clock.on_delay = [&] {
    if (DisplayService::spy_init_count == 1 && !observed_display) {
      observed_display = true;
      CHECK(board.ble.active);
    }
  };
  GoApp app(board);
  GoAppTestAccess access(app);
  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  volatile bool button = false;

  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(observed_display);
  const uint32_t advertising_ms = display_ms;
  CHECK(board.ble.started_ms == 75);
  CHECK(board.ble.stopped_ms - board.ble.started_ms == advertising_ms);
  CHECK(clock.now == advertising_ms + 100);
  CHECK(test_spy::last_sleep_awake_ms == clock.now);
  CHECK(test_spy::light_sleep_count == 0);
  CHECK_FALSE(board.ble.initialized);

  GoBleAdvertisement::WakeReport report{};
  REQUIRE(GoBleAdvertisement::take_wake_report(report));
  CHECK(report.outcome == GoBleAdvertisement::WakeOutcome::Advertised);
  CHECK(report.advertising_started_ms == 75);
  CHECK(report.advertising_duration_ms == advertising_ms);
  CHECK(report.awake_ms == clock.now);
  CHECK(report.has_payload);
  CHECK(std::vector<uint8_t>(report.payload.begin(), report.payload.end()) == board.ble.payload);
  CHECK_FALSE(GoBleAdvertisement::take_wake_report(report));
}

TEST_CASE("fast path BLE: known promotion and shutdown paths skip broadcasting") {
  test_spy::reset();
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  RtcAppState state{};
  volatile bool button = false;
  using GoBleAdvertisement::WakeOutcome;
  WakeOutcome expected = WakeOutcome::Advertised;
  SECTION("button already latched") {
    button = true;
    expected = WakeOutcome::SkippedButton;
  }
  SECTION("sleep already too short") {
    test_spy::sleep_decision_to_return = {PowerService::SleepType::None, 0};
    expected = WakeOutcome::SkippedSleepTooShort;
  }
  SECTION("route storage failure") {
    state.tracking_state = TrackingState::Recording;
    state.tracking_session_id = 12345;
    test_spy::resume_route_result = false;
    expected = WakeOutcome::SkippedPromotion;
  }
  SECTION("over-temperature") {
    test_spy::snapshot_to_return.ship_mode_request = ShipModeRequest::OverTemperature;
    expected = WakeOutcome::SkippedShipMode;
  }
  SECTION("under-temperature") {
    test_spy::snapshot_to_return.ship_mode_request = ShipModeRequest::UnderTemperature;
    expected = WakeOutcome::SkippedShipMode;
  }
  SECTION("over-discharge") {
    test_spy::snapshot_to_return.ship_mode_request = ShipModeRequest::OverDischarge;
    expected = WakeOutcome::SkippedShipMode;
  }
  access.execute_fast_path(state, button);
  CHECK(board.call_index("ble_server") == -1);
  CHECK(board.ble.calls.empty());
  CHECK(test_spy::cache_backed_up);

  GoBleAdvertisement::WakeReport report{};
  REQUIRE(GoBleAdvertisement::take_wake_report(report));
  CHECK(report.outcome == expected);
  CHECK(report.advertising_started_ms == 0);
  CHECK_FALSE(report.has_payload);
}

TEST_CASE("fast path BLE: setup and stop failures preserve display storage and sleep") {
  test_spy::reset();
  MockBoard board;
  using GoBleAdvertisement::SetupStep;
  using GoBleAdvertisement::WakeOutcome;
  WakeOutcome expected_outcome = WakeOutcome::SetupFailed;
  SetupStep expected_step = SetupStep::None;
  SECTION("initialization") {
    board.ble.fail_at = "init";
    expected_step = SetupStep::Init;
  }
  SECTION("payload") {
    board.ble.fail_at = "payload";
    expected_step = SetupStep::Payload;
  }
  SECTION("interval") {
    board.ble.fail_at = "interval";
    expected_step = SetupStep::Interval;
  }
  SECTION("start") {
    board.ble.fail_at = "start";
    expected_step = SetupStep::Start;
  }
  SECTION("stop") {
    board.ble.fail_at = "stop";
    expected_outcome = WakeOutcome::StopFailed;
  }
  GoApp app(board);
  GoAppTestAccess access(app);
  volatile bool button = false;

  const auto result = access.execute_fast_path(RtcAppState{}, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(test_spy::cache_backed_up);
  CHECK(DisplayService::spy_init_count == 1);
  REQUIRE(board.ble.calls.size() >= 2);
  CHECK(board.ble.calls[board.ble.calls.size() - 2] == board.ble.fail_at);
  CHECK(board.ble.calls.back() == "deinit");
  CHECK_FALSE(board.ble.active);
  CHECK_FALSE(board.ble.initialized);

  GoBleAdvertisement::WakeReport report{};
  REQUIRE(GoBleAdvertisement::take_wake_report(report));
  CHECK(report.outcome == expected_outcome);
  CHECK(report.failed_step == expected_step);
}

TEST_CASE("fast path BLE: promotion after display always releases BLE") {
  test_spy::reset();
  ScopedRetainedClock clock(0);
  DisplayService::spy_init_delay_ms = 3000;
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  volatile bool button = false;
  bool button_caused = false;
  SECTION("button during display") { button_caused = true; }
  SECTION("elapsed time makes sleep too short") {}
  clock.on_delay = [&] {
    if (DisplayService::spy_init_count == 1 && board.ble.active) {
      if (button_caused) {
        button = true;
      } else {
        test_spy::sleep_decision_to_return = {PowerService::SleepType::None, 0};
      }
    }
  };

  const auto result = access.execute_fast_path(RtcAppState{}, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK(result.handoff.initial_lock_state ==
        (button_caused ? LockState::Unlocked : LockState::Locked));
  CHECK(board.ble.calls ==
        std::vector<std::string>{"init", "payload", "interval", "start", "stop", "deinit"});
  CHECK_FALSE(board.ble.active);
  CHECK_FALSE(board.ble.initialized);
}

TEST_CASE("fast path BLE: unavailable measurements and battery retain wire sentinels") {
  test_spy::reset();
  MockBoard board;
  board.bms_failures_remaining = 2;
  GoApp app(board);
  GoAppTestAccess access(app);
  volatile bool button = false;

  const auto result = access.execute_fast_path(RtcAppState{}, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(board.ble.payload == std::vector<uint8_t>{0x01, 0x00, 0x80, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                  0xFF, 0xFF, 0x00, 0x80, 0xFF});
  CHECK_FALSE(board.ble.initialized);
}

TEST_CASE("fast path: GoApp starts PM warmup after initialization and overlaps GPS") {
  test_spy::reset();
  MockBoard board;
  board.settings.gps_mode = GpsMode::AlwaysOn;
  board.sensor_init_ms = 1700;
  test_spy::gps_read_duration_ms = 3000;
  GoApp app(board);
  GoAppTestAccess access(app);
  RtcAppState state{};
  volatile bool button = false;
  SECTION("only the remaining interval sleeps") {
    const auto result = access.execute_fast_path(state, button);
    CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
    CHECK(test_spy::light_sleep_count == 1);
    CHECK(test_spy::light_sleep_duration_ms == 7000);
  }
  SECTION("slow initialization does not consume the warmup budget") {
    board.sensor_init_ms = 11000;
    const auto result = access.execute_fast_path(state, button);
    CHECK(result.has_measures);
    CHECK(test_spy::light_sleep_count == 1);
    CHECK(test_spy::light_sleep_duration_ms == 7000);
  }
  SECTION("retained warm sensors skip PM warmup") {
    state.set_flag(RtcAppFlag::SensorsWarm, true);
    const auto result = access.execute_fast_path(state, button);
    CHECK(result.has_measures);
    CHECK(test_spy::light_sleep_count == 0);
  }
  CHECK(test_spy::warmup_step_count == 0);
}

TEST_CASE("fast path: gas finishes before other sampling and PM stops before storage") {
  test_spy::reset();
  test_spy::gas_begin_ok = true;
  test_spy::gas_sample.tvoc_raw = 21000;
  test_spy::gas_sample.nox_raw = 22000;
  test_spy::measures_to_return.co2.co2 = 650;
  test_spy::measures_to_return.pm_a.pm_25 = 12;
  MockBoard board;
  board.settings.gps_mode = GpsMode::AlwaysOn;
  GoApp app(board);
  GoAppTestAccess access(app);
  RtcAppState state{};
  SECTION("cold PM warmup is covered by gas conditioning") {}
  SECTION("PM was already warm") { state.set_flag(RtcAppFlag::SensorsWarm, true); }
  volatile bool button = false;
  const uint64_t started_ms = RTOS::get_time_ms();
  test_spy::before_sensor_measurement = [&] { CHECK(RTOS::get_time_ms() - started_ms == 10400); };
  const auto result = access.execute_fast_path(state, button);
  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(test_spy::fast_path_events == std::vector<std::string>{"gps", "gas_begin", "sleep",
                                                               "gas_read", "gas_read", "gas_stop",
                                                               "measure", "pm_sleep", "cache"});
  CHECK(test_spy::last_sensor_groups == (SensorGroup::PM | SensorGroup::Other));
  CHECK(result.measures.co2.co2 == 650);
  CHECK(result.measures.pm_a.pm_25 == 12);
  CHECK(result.measures.tvoc_nox.tvoc_raw == 21000);
  CHECK(result.measures.tvoc_nox.nox_raw == 22000);
  CHECK(result.measures.tvoc_nox.tvoc_index == 21000);
  CHECK(result.measures.tvoc_nox.nox_index == 22000);
  CHECK(test_spy::last_cached_measurement.tvoc_nox.tvoc_raw == 21000);
  CHECK(test_spy::last_cached_measurement.tvoc_nox.tvoc_index == 21000);
  CHECK(test_spy::last_cached_measurement.tvoc_nox.nox_index == 22000);
  const auto display =
      build_fast_path_display(result.measures, {}, {}, board.settings, TrackingState::Idle);
  CHECK(display.tvoc_index == 21000);
  CHECK(display.nox_index == 22000);
  CHECK_FALSE(result.sensors_warm);
  CHECK(test_spy::light_sleep_count == 1);
  CHECK(test_spy::fast_path_timer_only);
  CHECK(test_spy::ext_watchdog_feed_count == 0);
}

TEST_CASE("fast path: latched button is handled after gas sampling, storage, and display") {
  test_spy::reset();
  test_spy::gas_begin_ok = true;
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  volatile bool button = false;
  test_spy::light_sleep_callback = [&] {
    // Model a latched interrupt observed after the timer wait, not button wake.
    RTOS::delay_ms(test_spy::light_sleep_duration_ms);
    button = true;
    return false;
  };
  const auto result = access.execute_fast_path(RtcAppState{}, button);
  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK(result.handoff.initial_lock_state == LockState::Unlocked);
  CHECK(result.handoff.suppress_wake_press);
  CHECK(result.has_measures);
  CHECK(test_spy::gas_read_count == 2);
  CHECK(test_spy::gas_cancel_count == 1);
  CHECK(test_spy::cache_measurement_called);
  CHECK(DisplayService::spy_init_count == 1);
  CHECK(test_spy::fast_path_timer_only);
  CHECK(test_spy::ext_watchdog_feed_count == 0);
}

TEST_CASE("fast path: gas acquisition cleans up late and failed reads") {
  test_spy::reset();
  test_spy::gas_begin_ok = true;
  int expected_reads = 1;
  SECTION("first read fails") { test_spy::gas_fail_read = 1; }
  SECTION("late wake skips raw sampling and stops the heater") {
    expected_reads = 0;
    test_spy::light_sleep_callback = [] {
      RTOS::delay_ms(test_spy::light_sleep_duration_ms + 200);
      return false;
    };
  }
  SECTION("retained read fails") {
    test_spy::gas_fail_read = 2;
    expected_reads = 2;
  }
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  volatile bool button = false;
  const uint64_t started_ms = RTOS::get_time_ms();
  test_spy::before_sensor_measurement = [&] {
    CHECK(RTOS::get_time_ms() - started_ms >= CONFIG_SENSOR_WARMUP_DURATION_MS);
  };
  const auto result = access.execute_fast_path(RtcAppState{}, button);
  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(test_spy::gas_read_count == expected_reads);
  CHECK(test_spy::gas_cancel_count == 1);
  // A failed first read leaves only 100 ms of PM warmup: use a normal delay.
  CHECK(test_spy::light_sleep_count == 1);
  CHECK_FALSE(result.measures.tvoc_nox.is_tvoc_raw_valid());
  CHECK_FALSE(result.measures.tvoc_nox.is_nox_raw_valid());
  CHECK_FALSE(result.measures.tvoc_nox.is_tvoc_index_valid());
  CHECK_FALSE(result.measures.tvoc_nox.is_nox_index_valid());
}

TEST_CASE("fast path: failed light sleep delays and completes acquisition") {
  test_spy::reset();
  test_spy::light_sleep_failure = true;
  SECTION("gas conditioning covers cold PM warmup") { test_spy::gas_begin_ok = true; }
  SECTION("missing gas uses the PM warmup fallback") {}
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  volatile bool button = false;
  const auto result = access.execute_fast_path(RtcAppState{}, button);
  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(result.has_measures);
  CHECK(test_spy::light_sleep_fallback_count == 1);
  CHECK(test_spy::gas_read_count == (test_spy::gas_begin_ok ? 2 : 0));
  CHECK(test_spy::cache_measurement_called);
  CHECK(test_spy::fast_path_timer_only);
  CHECK(test_spy::ext_watchdog_feed_count == 0);
}

TEST_CASE("fast path: button is honored on warm entry and after early PM sleep") {
  test_spy::reset();
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  volatile bool button = false;
  SECTION("already held on warm entry") { button = true; }
  SECTION("interrupt after PM sleep") {
    test_spy::after_pm_sleep = [&] { button = true; };
  }
  SECTION("interrupt during storage") {
    test_spy::after_cache = [&] { button = true; };
  }
  SECTION("interrupt during final PM sleep after interval changes") {
    test_spy::should_hold_pm_result = true;
    test_spy::after_cache = [] { test_spy::should_hold_pm_result = false; };
    test_spy::after_pm_sleep = [&] { button = true; };
  }
  const auto result = access.execute_fast_path(state, button);
  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK(result.has_measures);
  CHECK(result.handoff.initial_lock_state == LockState::Unlocked);
  CHECK(result.handoff.suppress_wake_press);
  CHECK_FALSE(result.handoff.display_painted);
  CHECK_FALSE(result.sensors_warm);
}

TEST_CASE("fast path: an attempted PM sleep never becomes a retained warm resume") {
  test_spy::reset();
  SECTION("interval becomes short after the PM sleep command") {
    test_spy::after_pm_sleep = [] { test_spy::should_hold_pm_result = true; };
  }
  SECTION("sleep command failed after possibly stopping measurement") {
    test_spy::pm_sleep_ok = false;
  }
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  RtcAppState state{};
  volatile bool button = false;
  const auto result = access.execute_fast_path(state, button);
  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK_FALSE(result.sensors_warm);
  CHECK(test_spy::pm_sleep_count == 1);
}

TEST_CASE("Offline fast path: temperature trip paints shutdown screen and shuts down") {
  const auto run_trip = [](ShipModeRequest request, Screen expected_screen) {
    test_spy::reset();
    test_spy::snapshot_to_return.ship_mode_request = request;

    MockBoard board;
    GoApp app(board);
    GoAppTestAccess access(app);

    RtcAppState state{};
    state.mode = OperatingMode::Offline;
    state.lock_state = LockState::Locked;
    state.set_flag(RtcAppFlag::SensorsWarm, true);

    access.run_fast_path(state);

    CHECK(test_spy::shutdown_called);
    CHECK(DisplayService::spy_init_count == 1);
    CHECK(DisplayService::spy_last_screen == expected_screen);
    CHECK(board.isr_removed);
    CHECK_FALSE(test_spy::enter_sleep_called);
    CHECK_FALSE(test_spy::orchestrator_init_called);
  };

  SECTION("over-temperature uses the overheated page") {
    run_trip(ShipModeRequest::OverTemperature, Screen::ShutdownTemperature);
  }

  SECTION("under-temperature uses the low-temperature page") {
    run_trip(ShipModeRequest::UnderTemperature, Screen::ShutdownTemperatureLow);
  }
}

TEST_CASE("execute_fast_path: latched button promotes unlocked after acquisition") {
  test_spy::reset();

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, false);
  volatile bool button = true; // already pressed

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK(result.handoff.initial_lock_state == LockState::Unlocked);
  CHECK(result.handoff.suppress_wake_press == true);
  CHECK(result.has_measures == true);
}

TEST_CASE("execute_fast_path: sleep too short -> promote locked") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::None, 0};

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK(result.handoff.initial_lock_state == LockState::Locked);
  CHECK(result.handoff.display_painted == true);
  CHECK(result.has_measures == true);
}

TEST_CASE("execute_fast_path: tracking + GPS active -> route point stored") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};
  test_spy::snapshot_to_return.battery_percentage = 80.0f;
  test_spy::snapshot_to_return.battery_percent_source = BatteryPercentSource::FuelGauge;

  MockBoard board;
  board.settings.gps_mode = GpsMode::AlwaysOn;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  state.tracking_state = TrackingState::Recording;
  state.tracking_session_id = 12345;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  // Fast path can never start a new session — it always resumes.
  CHECK(test_spy::route_resumed == true);
  CHECK(test_spy::route_started == false);
  CHECK(test_spy::route_session_id == 12345);
  CHECK(test_spy::route_point_appended == true);
  CHECK(test_spy::last_route_point.battery_percentage == 80.0f);
  CHECK(test_spy::route_ended == true);
  CHECK(test_spy::bms_poll_count == 1);
  CHECK(board.new_gps_driver_called == true);
}

TEST_CASE("execute_fast_path: paused session keeps GPS and display but writes no route") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};
  MockBoard board;
  board.settings.gps_mode = GpsMode::OnWhenTracking;
  GoApp app(board);
  GoAppTestAccess access(app);
  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  state.tracking_state = TrackingState::Paused;
  state.tracking_session_id = 12345;
  volatile bool button = false;
  CHECK(is_gps_active_at_boot(board.settings, state));
  auto result = access.execute_fast_path(state, button);
  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK_FALSE(test_spy::route_resumed);
  CHECK_FALSE(test_spy::route_started);
  CHECK_FALSE(test_spy::route_point_appended);
  CHECK(board.new_gps_driver_called);
  CHECK(state.tracking_state == TrackingState::Paused);
  CHECK(state.tracking_session_id == 12345);
  const auto display = build_fast_path_display(MeasuresAGo{}, GpsData{}, PowerSnapshot{},
                                               board.settings, state.tracking_state);
  CHECK(display.tracking_state == TrackingState::Paused);
}

TEST_CASE("execute_fast_path: degraded route uses fuel-gauge battery snapshot") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};
  test_spy::snapshot_to_return.battery_percentage = 74.0f;
  test_spy::snapshot_to_return.battery_percent_source = BatteryPercentSource::FuelGauge;

  MockBoard board;
  board.bms_failures_remaining = 2;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  state.tracking_state = TrackingState::Recording;
  state.tracking_session_id = 12345;
  volatile bool button = false;

  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK_FALSE(board.bms_available);
  CHECK(test_spy::route_point_appended);
  CHECK(test_spy::last_route_point.battery_percentage == 74.0f);
  CHECK(test_spy::bms_poll_count == 1);
}

TEST_CASE("execute_fast_path: resume_route failure -> promote, no display painted") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};
  test_spy::resume_route_result = false; // simulate persistent NAND fault

  MockBoard board;
  board.settings.gps_mode = GpsMode::AlwaysOn;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  state.tracking_state = TrackingState::Recording;
  state.tracking_session_id = 12345;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  // tracking_active stays set in the inbound state so the orchestrator
  // retries the resume during init() and surfaces the failure there.
  CHECK(state.tracking_state == TrackingState::Recording);
  CHECK(result.handoff.initial_lock_state == LockState::Locked);
  // Storage failed before the display block — display was NOT painted.
  CHECK(result.handoff.display_painted == false);
  // We never wrote a point because resume failed.
  CHECK(test_spy::route_point_appended == false);
}

TEST_CASE("execute_fast_path: append_route_point failure -> promote, no display painted") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};
  test_spy::append_route_point_result = false;

  MockBoard board;
  board.settings.gps_mode = GpsMode::AlwaysOn;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  state.tracking_state = TrackingState::Recording;
  state.tracking_session_id = 12345;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK(state.tracking_state == TrackingState::Recording);
  CHECK(result.handoff.initial_lock_state == LockState::Locked);
  CHECK(result.handoff.display_painted == false);
  // Resume succeeded; end_route still ran as best-effort close.
  CHECK(test_spy::route_resumed == true);
  CHECK(test_spy::route_point_appended == true);
  CHECK(test_spy::route_ended == true);
}

TEST_CASE("execute_fast_path: tracking + GPS off -> no GPS read") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  board.settings.gps_mode = GpsMode::AlwaysOff;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  state.tracking_state = TrackingState::Recording;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK_FALSE(test_spy::gps_read_called);
  CHECK(test_spy::gps_sleep_requested);
}

TEST_CASE("execute_fast_path: no tracking -> no route") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  state.tracking_state = TrackingState::Idle;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(test_spy::route_started == false);
}

// ============================================================================
// Tests: fast-path retained tracking timing
// ============================================================================

static RtcAppState recording_rtc_state() {
  constexpr uint32_t RESERVED_RTC_FLAG = uint32_t{1} << 31;
  RtcAppState state{};
  state.mode = OperatingMode::Offline;
  state.behavior = Behavior::Tracking;
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  state.flags |= RESERVED_RTC_FLAG;
  state.tracking_state = TrackingState::Recording;
  state.tracking_session_id = 12345;
  state.tracking_timing = {100, 20, 150, 160};
  return state;
}

static void check_tracking_timing(const TrackingTiming &actual, const TrackingTiming &expected) {
  CHECK(actual.session_started_s == expected.session_started_s);
  CHECK(actual.recording_accumulated_s == expected.recording_accumulated_s);
  CHECK(actual.recording_started_s == expected.recording_started_s);
  CHECK(actual.last_record_s == expected.last_record_s);
}

static void check_rtc_state(const RtcAppState &actual, const RtcAppState &expected) {
  CHECK(actual.mode == expected.mode);
  CHECK(actual.behavior == expected.behavior);
  CHECK(actual.lock_state == expected.lock_state);
  CHECK(actual.tracking_state == expected.tracking_state);
  CHECK(actual.tracking_session_id == expected.tracking_session_id);
  CHECK(actual.flags == expected.flags);
  check_tracking_timing(actual.tracking_timing, expected.tracking_timing);
}

TEST_CASE("execute_fast_path: accepted write carries retained seconds into Sleep or late Promote") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  test_spy::route_point_count = 7;
  auto expected_outcome = GoAppTestAccess::Outcome::Sleep;
  bool expected_display_painted = false;
  SECTION("sleep after accepted write") {}
  SECTION("late promotion after accepted write") {
    test_spy::sleep_decision_to_return = {PowerService::SleepType::None, 0};
    expected_outcome = GoAppTestAccess::Outcome::Promote;
    expected_display_painted = true;
  }
  SECTION("sync error after accepted write still timestamps and promotes") {
    test_spy::append_route_point_sync_failure = true;
    expected_outcome = GoAppTestAccess::Outcome::Promote;
  }

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  const auto state = recording_rtc_state();
  volatile bool button = false;
  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == expected_outcome);
  CHECK(result.handoff.display_painted == expected_display_painted);
  CHECK(result.has_measures);
  CHECK(test_spy::route_point_appended);
  CHECK(test_spy::route_ended);
  CHECK(test_spy::route_point_count == 0); // Closing resets the writer, not the accepted timestamp.
  auto expected = state.tracking_timing;
  expected.last_record_s = 200;
  check_tracking_timing(result.tracking_timing, expected);
  CHECK(state.tracking_timing.last_record_s == 160); // Input remains a snapshot.
}

TEST_CASE("execute_fast_path: button promotion records timing while rejected writes preserve it") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  test_spy::route_point_count = 7;
  auto state = recording_rtc_state();
  volatile bool button = false;
  bool append_attempted = false;
  SECTION("button interrupts warmup") {
    state.set_flag(RtcAppFlag::SensorsWarm, false);
    button = true;
    append_attempted = true;
  }
  SECTION("button after warm measurement") {
    button = true;
    append_attempted = true;
  }
  SECTION("route reopen failure") { test_spy::resume_route_result = false; }
  SECTION("write rejected with existing nonzero count") {
    test_spy::append_route_point_result = false;
    append_attempted = true;
  }

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK_FALSE(result.handoff.display_painted);
  CHECK(test_spy::route_point_appended == append_attempted);
  CHECK(test_spy::route_ended == append_attempted);
  CHECK(result.handoff.initial_lock_state == (button ? LockState::Unlocked : LockState::Locked));
  auto expected = state.tracking_timing;
  if (button) {
    expected.last_record_s = 200;
  }
  check_tracking_timing(result.tracking_timing, expected);
}

TEST_CASE("execute_fast_path: paused timing is preserved and inactive timing is cleared") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  auto state = recording_rtc_state();
  state.tracking_state = TrackingState::Paused;
  state.tracking_timing.recording_accumulated_s = 30;
  state.tracking_timing.recording_started_s = TRACKING_TIME_INVALID_S;
  auto expected = state.tracking_timing;
  volatile bool button = false;
  SECTION("paused sleep") {}
  SECTION("paused early promotion") { button = true; }
  SECTION("paused with live marker invalidates active duration") {
    state.tracking_timing.recording_started_s = 150;
    expected.recording_accumulated_s = TRACKING_TIME_INVALID_S;
  }
  SECTION("inactive sleep clears stale timing") {
    state.tracking_state = TrackingState::Idle;
    expected = {};
  }
  SECTION("inactive early promotion clears stale timing") {
    state.tracking_state = TrackingState::Idle;
    expected = {};
    button = true;
  }

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome ==
        (button ? GoAppTestAccess::Outcome::Promote : GoAppTestAccess::Outcome::Sleep));
  CHECK_FALSE(test_spy::route_resumed);
  CHECK_FALSE(test_spy::route_point_appended);
  check_tracking_timing(result.tracking_timing, expected);
}

TEST_CASE("execute_fast_path: button promotion normalizes mismatched state and segment") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  auto state = recording_rtc_state();
  SECTION("Recording with paused marker") {
    state.tracking_timing.recording_started_s = TRACKING_TIME_INVALID_S;
  }
  SECTION("Paused with live marker") { state.tracking_state = TrackingState::Paused; }

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  volatile bool button = true;
  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK(test_spy::route_point_appended == (state.tracking_state == TrackingState::Recording));
  auto expected = state.tracking_timing;
  expected.recording_accumulated_s = TRACKING_TIME_INVALID_S;
  expected.recording_started_s = TRACKING_TIME_INVALID_S;
  if (state.tracking_state == TrackingState::Recording) {
    expected.last_record_s = 200;
  }
  check_tracking_timing(result.tracking_timing, expected);
}

TEST_CASE("execute_fast_path: unknown active duration allows accepted-record timestamps") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  auto state = recording_rtc_state();
  SECTION("Recording with paused marker") {
    state.tracking_timing.recording_started_s = TRACKING_TIME_INVALID_S;
  }
  SECTION("accumulated duration is unknown") {
    state.tracking_timing.recording_accumulated_s = TRACKING_TIME_INVALID_S;
  }
  SECTION("entire active duration is unknown") {
    state.tracking_timing.recording_accumulated_s = TRACKING_TIME_INVALID_S;
    state.tracking_timing.recording_started_s = TRACKING_TIME_INVALID_S;
  }
  for (bool sync_failure : {false, true}) {
    CAPTURE(sync_failure);
    test_spy::reset();
    test_spy::append_route_point_sync_failure = sync_failure;
    MockBoard board;
    GoApp app(board);
    GoAppTestAccess access(app);
    volatile bool button = false;
    const auto result = access.execute_fast_path(state, button);

    CHECK(result.outcome ==
          (sync_failure ? GoAppTestAccess::Outcome::Promote : GoAppTestAccess::Outcome::Sleep));
    CHECK(test_spy::route_point_appended);
    CHECK(test_spy::route_ended);
    auto expected = state.tracking_timing;
    expected.recording_accumulated_s = TRACKING_TIME_INVALID_S;
    expected.last_record_s = 200;
    check_tracking_timing(result.tracking_timing, expected);
  }
}

TEST_CASE("execute_fast_path: invalid accepted-record time invalidates only last_record_s") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  auto state = recording_rtc_state();
  auto expected_outcome = GoAppTestAccess::Outcome::Sleep;
  SECTION("clock exceeds representable seconds") {
    clock.retained_ms = static_cast<uint64_t>(TRACKING_TIME_INVALID_S) * 1000;
  }
  SECTION("clock regresses behind last accepted record") { clock.retained_ms = 159999; }
  SECTION("invalid time and accepted write with sync failure") {
    clock.retained_ms = 99999;
    test_spy::append_route_point_sync_failure = true;
    expected_outcome = GoAppTestAccess::Outcome::Promote;
  }

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  volatile bool button = false;
  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == expected_outcome);
  CHECK(test_spy::route_point_appended);
  auto expected = state.tracking_timing;
  expected.last_record_s = TRACKING_TIME_INVALID_S;
  check_tracking_timing(result.tracking_timing, expected);
}

TEST_CASE("execute_fast_path: Shutdown result clears timing without changing route lifecycle") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  test_spy::snapshot_to_return.ship_mode_request = ShipModeRequest::OverTemperature;
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  const auto state = recording_rtc_state();
  volatile bool button = false;
  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Shutdown);
  check_tracking_timing(result.tracking_timing, TrackingTiming{});
  CHECK(state.tracking_state == TrackingState::Recording);
  CHECK(state.tracking_session_id == 12345);
  CHECK_FALSE(test_spy::state_saved);
}

TEST_CASE("run_fast_path: Sleep saves returned timing with SensorsWarm") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  SECTION("hold sensors warm") { test_spy::should_hold_pm_result = true; }
  SECTION("power down sensors") { test_spy::should_hold_pm_result = false; }
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);
  const auto state = recording_rtc_state();
  test_spy::rtc_state = state;

  access.run_fast_path(state);

  REQUIRE(test_spy::state_saved);
  CHECK(test_spy::enter_sleep_called);
  CHECK_FALSE(test_spy::orchestrator_init_called);
  CHECK(board.call_index("sleep_touch") == -1);
  CHECK(board.call_index("new_touch_sensor") == -1);
  auto expected = state;
  expected.tracking_timing.last_record_s = 200;
  expected.set_flag(RtcAppFlag::SensorsWarm, test_spy::should_hold_pm_result);
  check_rtc_state(test_spy::last_saved_state, expected);
  check_rtc_state(test_spy::rtc_state, expected);
}

TEST_CASE("run_fast_path: promotion saves timing before BMS retry and interactive init") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  test_spy::sleep_decision_to_return = {PowerService::SleepType::None, 0};
  MockBoard board;
  board.bms_failures_remaining = 2;
  const auto state = recording_rtc_state();
  test_spy::rtc_state = state;
  auto expected = state;
  expected.tracking_timing.last_record_s = 200;
  bool expect_restart = false;
  SECTION("late promotion hands updated RTC to init") {}
  SECTION("sync failure hands accepted timestamp to init") {
    test_spy::append_route_point_sync_failure = true;
  }
  SECTION("button promotion hands accepted timestamp to init") {
    board.press_button_on_isr_install = true;
  }
  SECTION("BMS retry failure retains accepted timestamp before restart") {
    board.bms_failures_remaining = 4;
    expect_restart = true;
  }
  // Promotion requires normal sensor preparation, including after early PM sleep.
  expected.set_flag(RtcAppFlag::SensorsWarm, false);
  bool checked_retry_state = false;
  board.bms_init_callback = [&] {
    if (board.bms_init_attempts >= 2) {
      REQUIRE(test_spy::state_saved);
      check_rtc_state(test_spy::rtc_state, expected);
      checked_retry_state = true;
    }
  };
  GoApp app(board);
  GoAppTestAccess access(app);

  access.run_fast_path(state);

  CHECK(checked_retry_state);
  CHECK(board.restart_called == expect_restart);
  CHECK(test_spy::orchestrator_init_called == !expect_restart);
  CHECK_FALSE(test_spy::enter_sleep_called);
  check_rtc_state(test_spy::last_saved_state, expected);
  if (!expect_restart) {
    check_rtc_state(test_spy::orchestrator_rtc_state, expected);
    CHECK(test_spy::orchestrator_wake_cause == WakeCause::Timer);
  }
}

// ============================================================================
// Tests: call ordering in execute_fast_path
// ============================================================================

TEST_CASE("execute_fast_path: retries BMS initialization once") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  board.bms_failures_remaining = 1;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  volatile bool button = false;

  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(board.bms_init_attempts == 2);
  CHECK(board.sensors_called);
}

TEST_CASE("execute_fast_path: exhausted BMS retries continue without BMS") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  board.bms_failures_remaining = 2;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  volatile bool button = false;

  const auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(board.bms_init_attempts == 2);
  CHECK_FALSE(board.bms_available);
  CHECK(board.call_index("release_gpio_holds") >= 0);
  CHECK(board.call_index("power") >= 0);
  CHECK(board.call_index("sensors") >= 0);
  CHECK(board.call_index("storage") >= 0);
}

TEST_CASE("fast-path promotion retries BMS for required interactive boot") {
  test_spy::reset();

  MockBoard board;
  board.bms_failures_remaining = 2;
  board.press_button_on_isr_install = true;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  access.run_fast_path(state);

  CHECK(board.bms_init_attempts == 3);
  CHECK(board.bms_available);
  CHECK_FALSE(board.restart_called);
  CHECK(test_spy::orchestrator_init_called);
  CHECK(test_spy::orchestrator_run_called);
}

TEST_CASE("fast-path promotion restarts when BMS retry is exhausted") {
  test_spy::reset();

  MockBoard board;
  board.bms_failures_remaining = 4;
  board.press_button_on_isr_install = true;
  GoApp app(board);
  GoAppTestAccess access(app);

  access.run_fast_path(RtcAppState{});

  CHECK(board.bms_init_attempts == 4);
  CHECK_FALSE(board.bms_available);
  CHECK(board.restart_called);
  CHECK_FALSE(test_spy::orchestrator_init_called);
  CHECK_FALSE(test_spy::orchestrator_run_called);
}

TEST_CASE("fast-path sleep does not retry unavailable BMS during handoff") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  board.bms_failures_remaining = 2;
  GoApp app(board);
  GoAppTestAccess access(app);

  access.run_fast_path(RtcAppState{});

  CHECK(board.bms_init_attempts == 2);
  CHECK_FALSE(board.bms_available);
  CHECK(test_spy::enter_sleep_called);
  CHECK_FALSE(board.restart_called);
  CHECK_FALSE(test_spy::orchestrator_init_called);
}

TEST_CASE("execute_fast_path: init ordering — init_core before load_settings before sensors") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  volatile bool button = false;

  access.execute_fast_path(state, button);

  // Core infrastructure, fuel gauge, and BMS attempt precede services.
  CHECK(board.call_index("init_core") >= 0);
  CHECK(board.call_index("init_fuel_gauge") >= 0);
  CHECK(board.call_index("init_bms") >= 0);
  CHECK(board.call_index("load_settings") >= 0);
  CHECK(board.call_index("sensors") >= 0);
  CHECK(board.call_index("init_core") < board.call_index("init_fuel_gauge"));
  CHECK(board.call_index("init_fuel_gauge") < board.call_index("init_bms"));
  CHECK(board.call_index("init_core") < board.call_index("init_bms"));
  CHECK(board.call_index("init_bms") < board.call_index("load_settings"));
  CHECK(board.call_index("init_core") < board.call_index("load_settings"));
  CHECK(board.call_index("load_settings") < board.call_index("sensors"));
}

TEST_CASE("execute_fast_path: sleep path ordering — sensors before storage before display") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  volatile bool button = false;

  access.execute_fast_path(state, button);

  // Full sleep path: power (set_pm_power) → sensors → storage → display
  // power() is called before sensors() to arm PMID.
  CHECK(board.call_index("power") < board.call_index("sensors"));
  CHECK(board.call_index("sensors") < board.call_index("storage"));
  CHECK(board.call_index("sensors") < board.call_index("display"));
}

TEST_CASE("execute_fast_path: release_gpio_holds after init_core") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.set_flag(RtcAppFlag::SensorsWarm, true);
  volatile bool button = false;

  access.execute_fast_path(state, button);

  CHECK(board.call_index("init_core") < board.call_index("release_gpio_holds"));
  CHECK(board.call_index("release_gpio_holds") < board.call_index("sensors"));
}

TEST_CASE("run_interactive wires a valid local API with shared identity and queue") {
  test_spy::reset();
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  access.run_interactive(WakeCause::PowerOn);

  CHECK(board.call_index("init_spi") >= 0);
  CHECK(board.call_index("display") >= 0);
  CHECK(board.call_index("init_core") >= 0);
  CHECK(board.call_index("init_spi") < board.call_index("display"));
  CHECK(board.call_index("display") < board.call_index("init_core"));
  CHECK(DisplayService::spy_init_count == 1);
  CHECK(DisplayService::spy_last_screen == Screen::Info);
  CHECK(DisplayService::spy_last_init_deferred);
  CHECK(DisplayService::spy_flush_count == 1);

  REQUIRE(test_spy::orchestrator_init_called);
  REQUIRE(test_spy::orchestrator_run_called);
  CHECK(test_spy::orchestrator_handoff.display_painted);
  REQUIRE(test_spy::orchestrator_local_api != nullptr);
  REQUIRE(test_spy::orchestrator_event_queue != nullptr);
  CHECK(test_spy::orchestrator_local_api->is_valid());
  CHECK(std::string(test_spy::orchestrator_local_api_system_info.serial_number) == "test-serial");
  CHECK(std::string(test_spy::orchestrator_local_api_system_info.model) == "P-1PSG");
  CHECK(std::string(test_spy::orchestrator_local_api_system_info.firmware) == "0.0.0-test");
  REQUIRE(test_spy::wifi_local_server != nullptr);
  CHECK(test_spy::wifi_local_server == test_spy::generic_local_server);
  REQUIRE(test_spy::generic_local_http != nullptr);
  CHECK(test_spy::generic_local_measures ==
        static_cast<MeasuresProvider *>(test_spy::orchestrator_local_api));
  CHECK(test_spy::generic_local_config ==
        static_cast<ConfigProvider *>(test_spy::orchestrator_local_api));
  CHECK(test_spy::generic_local_actions ==
        static_cast<ActionHandler *>(test_spy::orchestrator_local_api));
  CHECK(test_spy::generic_local_config_access == ConfigAccess::ReadWrite);
  CHECK(test_spy::wifi_serial_number == "test-serial");
  CHECK(test_spy::wifi_firmware_version == "0.0.0-test");
  CHECK(test_spy::wifi_model == STATIONARY_AGO_MODEL_CODE);
  CHECK(test_spy::wifi_ap_ssid == "airgradient-test-serial");
  CHECK(test_spy::wifi_hostname == "airgradient_test-serial");
  CHECK(test_spy::wifi_http_port == 80);
  CHECK(test_spy::serial_command_start_count == 1);

  test_spy::orchestrator_local_api->set_access(ConfigAccess::ReadWrite);
  CHECK(test_spy::orchestrator_local_api->trigger(ActionId::CalibrateCo2).status ==
        ActionStatus::Dispatched);
  LocalServerConfig partial{};
  partial.pm_standard = "us-aqi";
  CHECK(test_spy::orchestrator_local_api->submit_config(partial).status ==
        ConfigSubmitStatus::Accepted);
  Event event{};
  REQUIRE(RTOS::queue_receive(test_spy::orchestrator_event_queue, &event, 0));
  CHECK(event.type == EventType::LocalApiRequestReady);
}

TEST_CASE("run_interactive: exhausted BMS retries restart before orchestrator") {
  test_spy::reset();
  MockBoard board;
  board.bms_failures_remaining = 2;
  GoApp app(board);
  GoAppTestAccess access(app);

  access.run_interactive(WakeCause::PowerOn);

  CHECK(board.bms_init_attempts == 2);
  CHECK(board.fuel_gauge_init_called);
  CHECK(board.restart_called);
  CHECK_FALSE(test_spy::orchestrator_init_called);
  CHECK_FALSE(test_spy::orchestrator_run_called);
  CHECK(board.call_index("power") < 0);
  CHECK(board.call_index("sensors") < 0);
}

TEST_CASE("button wake path wires a valid local API with shared identity") {
  test_spy::reset();
  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  access.run_button_wake_path(RtcAppState{});

  REQUIRE(test_spy::orchestrator_init_called);
  REQUIRE(test_spy::orchestrator_run_called);
  REQUIRE(test_spy::orchestrator_local_api != nullptr);
  CHECK(test_spy::orchestrator_local_api->is_valid());
  CHECK(std::string(test_spy::orchestrator_local_api_system_info.serial_number) == "test-serial");
  CHECK(std::string(test_spy::orchestrator_local_api_system_info.model) == "P-1PSG");
  CHECK(std::string(test_spy::orchestrator_local_api_system_info.firmware) == "0.0.0-test");
  REQUIRE(test_spy::wifi_local_server != nullptr);
  CHECK(test_spy::wifi_local_server == test_spy::generic_local_server);
  CHECK(test_spy::generic_local_config_access == ConfigAccess::ReadWrite);
  CHECK(test_spy::wifi_serial_number == "test-serial");
  CHECK(test_spy::wifi_firmware_version == "0.0.0-test");
  CHECK(test_spy::wifi_model == STATIONARY_AGO_MODEL_CODE);
  CHECK(test_spy::wifi_ap_ssid == "airgradient-test-serial");
  CHECK(test_spy::wifi_hostname == "airgradient_test-serial");
  CHECK(test_spy::wifi_http_port == 80);
  CHECK(test_spy::serial_command_start_count == 1);
  test_spy::orchestrator_local_api->set_access(ConfigAccess::ReadWrite);
  CHECK(test_spy::orchestrator_local_api->trigger(ActionId::CalibrateCo2).status ==
        ActionStatus::Dispatched);
}

TEST_CASE("interactive paths do not start serial commands after onboarding") {
  SECTION("power-on interactive path") {
    test_spy::reset();
    MockBoard board;
    board.settings.onboarding_done = true;
    GoApp app(board);
    GoAppTestAccess access(app);

    access.run_interactive(WakeCause::PowerOn);

    REQUIRE(test_spy::orchestrator_init_called);
    CHECK(test_spy::serial_command_start_count == 0);
  }

  SECTION("button-wake path") {
    test_spy::reset();
    MockBoard board;
    board.settings.onboarding_done = true;
    GoApp app(board);
    GoAppTestAccess access(app);

    access.run_button_wake_path(RtcAppState{});

    REQUIRE(test_spy::orchestrator_init_called);
    CHECK(test_spy::serial_command_start_count == 0);
  }
}

TEST_CASE("button wake path: exhausted BMS retries restart before orchestrator") {
  test_spy::reset();
  MockBoard board;
  board.bms_failures_remaining = 2;
  GoApp app(board);
  GoAppTestAccess access(app);

  access.run_button_wake_path(RtcAppState{});

  CHECK(board.bms_init_attempts == 2);
  CHECK(board.fuel_gauge_init_called);
  CHECK(board.restart_called);
  CHECK_FALSE(test_spy::orchestrator_init_called);
  CHECK_FALSE(test_spy::orchestrator_run_called);
  CHECK(board.call_index("power") < 0);
  CHECK(board.call_index("sensors") < 0);
}

TEST_CASE("GoApp: interactive boot leaves GPS startup to orchestrator") {
  test_spy::reset();
  MockBoard board;
  board._bms.status.power_source = BmsPowerSource::None;
  board.settings.gps_mode = GpsMode::OnWhenTracking;
  static_cast<EmptyConfigStore &>(board.config_store()).tracking_start = true;
  GoApp app(board);

  SECTION("battery reset follows normal boot routing") { app.run(); }
  SECTION("interactive entry") {
    GoAppTestAccess access(app);
    access.run_interactive(WakeCause::PowerOn);
  }
  SECTION("Always Off") {
    board.settings.gps_mode = GpsMode::AlwaysOff;
    GoAppTestAccess access(app);
    access.run_interactive(WakeCause::PowerOn);
  }
  SECTION("button wake") {
    GoAppTestAccess access(app);
    access.run_button_wake_path(RtcAppState{});
  }

  CHECK(test_spy::orchestrator_run_called);
  CHECK(static_cast<EmptyConfigStore &>(board.config_store()).tracking_start);
  CHECK_FALSE(test_spy::gps_started);
  CHECK_FALSE(test_spy::gps_sleeping_started);
  CHECK_FALSE(test_spy::gps_sleep_requested);
  CHECK(test_spy::light_sleep_count == 0);
}

TEST_CASE("GoApp: Offline tracking reset creates a session and runs the cold fast path") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  MockBoard board;
  board.settings.operating_mode = OperatingMode::Offline;
  board._bms.status.power_source = BmsPowerSource::OtgMode;
  SECTION("GPS when tracking") { board.settings.gps_mode = GpsMode::OnWhenTracking; }
  SECTION("GPS always on") { board.settings.gps_mode = GpsMode::AlwaysOn; }
  SECTION("GPS always off") { board.settings.gps_mode = GpsMode::AlwaysOff; }
  auto &store = static_cast<EmptyConfigStore &>(board.config_store());
  store.tracking_start = true;
  test_spy::rtc_state = recording_rtc_state();
  test_spy::rtc_state.gps_wake_deadline_ms = clock.retained_ms + 300000;
  test_spy::before_create_route = [&] {
    CHECK_FALSE(store.tracking_start);
    CHECK(store.tracking_committed);
  };

  GoApp app(board);
  app.run();

  CHECK_FALSE(store.tracking_start);
  CHECK(test_spy::route_started);
  CHECK(test_spy::route_resumed);
  CHECK(test_spy::route_point_appended);
  CHECK(test_spy::route_ended);
  CHECK_FALSE(board.sensors_warm_arg);
  CHECK(board.call_index("sleep_touch") >= 0);
  CHECK(test_spy::warmup_step_count == 0);
  CHECK(test_spy::light_sleep_count > 0);
  CHECK(test_spy::gps_read_called == (board.settings.gps_mode != GpsMode::AlwaysOff));
  CHECK_FALSE(test_spy::orchestrator_init_called);
  CHECK(test_spy::enter_sleep_called);
  const auto &state = test_spy::last_saved_state;
  CHECK(state.mode == OperatingMode::Offline);
  CHECK(state.behavior == Behavior::Tracking);
  CHECK(state.tracking_state == TrackingState::Recording);
  CHECK(state.tracking_session_id == test_spy::route_session_id);
  CHECK(state.tracking_session_id >= 10000);
  CHECK(state.tracking_session_id <= 99999);
  CHECK(state.tracking_timing.session_started_s == 200);
  CHECK(state.tracking_timing.last_record_s == 200);
  if (board.settings.gps_mode != GpsMode::AlwaysOff) {
    CHECK(state.gps_wake_deadline_ms == 0);
    CHECK_FALSE(test_spy::gps_sleep_requested);
  }
}

TEST_CASE("GoApp: failed Offline tracking recovery returns to interactive without recording") {
  test_spy::reset();
  MockBoard board;
  board.settings.operating_mode = OperatingMode::Offline;
  auto &store = static_cast<EmptyConfigStore &>(board.config_store());
  store.tracking_start = true;
  SECTION("Cannot clear intent") { store.fail_tracking_write = true; }
  SECTION("Cannot commit cleared intent") { store.fail_commit = true; }
  SECTION("Route creation fails") { test_spy::create_route_result = false; }
  SECTION("Session IDs exhausted") { test_spy::route_exists_result = true; }
  GoApp app(board);
  app.run();
  CHECK(test_spy::orchestrator_init_called);
  CHECK_FALSE(test_spy::route_started);
  CHECK_FALSE(test_spy::route_point_appended);
  CHECK_FALSE(test_spy::enter_sleep_called);
}

TEST_CASE("GoApp: recovered Offline tracking can promote on a button press") {
  test_spy::reset();
  MockBoard board;
  board.settings.operating_mode = OperatingMode::Offline;
  board.press_button_on_isr_install = true;
  auto &store = static_cast<EmptyConfigStore &>(board.config_store());
  store.tracking_start = true;
  int created = 0;
  test_spy::before_create_route = [&] { ++created; };
  GoApp app(board);
  app.run();
  CHECK(created == 1);
  CHECK_FALSE(store.tracking_start);
  CHECK(test_spy::orchestrator_init_called);
  CHECK(test_spy::orchestrator_rtc_state.tracking_state == TrackingState::Recording);
  CHECK(test_spy::orchestrator_rtc_state.tracking_session_id == test_spy::route_session_id);
  CHECK(test_spy::route_point_appended);
  CHECK_FALSE(test_spy::enter_sleep_called);
}

TEST_CASE("GoApp: ordinary Offline power-on keeps interactive boot") {
  test_spy::reset();
  MockBoard board;
  board.settings.operating_mode = OperatingMode::Offline;
  GoApp app(board);
  app.run();
  CHECK(test_spy::orchestrator_init_called);
  CHECK_FALSE(test_spy::route_started);
  CHECK_FALSE(test_spy::enter_sleep_called);
}

TEST_CASE("GoApp: pending tracking start does not override USB charging-only boot") {
  test_spy::reset();
  test_spy::power_on_reset = true;
  MockBoard board;
  board.settings.operating_mode = OperatingMode::Offline;
  board._bms.status.power_source = BmsPowerSource::UsbDcp;
  auto &store = static_cast<EmptyConfigStore &>(board.config_store());
  store.tracking_start = true;
  test_spy::light_sleep_callback = [&] {
    board._bms.status.power_source = BmsPowerSource::None;
    return false;
  };
  GoApp app(board);
  app.run();

  CHECK_FALSE(test_spy::orchestrator_run_called);
  CHECK(test_spy::light_sleep_count == 1);
  CHECK(store.tracking_start);
}

TEST_CASE("run_fast_path: early inactive GPS sleep uses the measurement interval") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  MockBoard board;
  board.settings.operating_mode = OperatingMode::Offline;
  board.settings.gps_mode = GpsMode::OnWhenTracking;
  board.settings.measure_interval_seconds = 180;
  constexpr uint32_t planned_sleep_ms = 120000;
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, planned_sleep_ms};
  bool expect_sleep_request = true;
  uint32_t expected_gps_sleep_ms = 180000;
  RtcAppState state{};
  state.mode = OperatingMode::Offline;
  state.set_flag(RtcAppFlag::SensorsWarm, true);

  SECTION("inactive GPS sleeps for the measurement interval") {}
  SECTION("an existing shorter GPS timer does not shorten ESP sleep") {
    test_spy::gps_remaining_ms = 60000;
    state.gps_wake_deadline_ms = clock.retained_ms + 60000;
    expect_sleep_request = false;
  }
  SECTION("Always Off sleeps for the measurement interval") {
    board.settings.gps_mode = GpsMode::AlwaysOff;
  }
  SECTION("tracking keeps GPS active when mode permits") {
    state.tracking_state = TrackingState::Recording;
    expect_sleep_request = false;
  }
  SECTION("paused tracking also keeps GPS active") {
    state.tracking_state = TrackingState::Paused;
    expect_sleep_request = false;
  }
  SECTION("Always On keeps GPS active without tracking") {
    board.settings.gps_mode = GpsMode::AlwaysOn;
    expect_sleep_request = false;
  }

  GoApp app(board);
  GoAppTestAccess access(app);
  access.run_fast_path(state);

  REQUIRE(test_spy::enter_sleep_called);
  CHECK(test_spy::enter_sleep_duration_ms == planned_sleep_ms);
  CHECK(test_spy::gps_sleep_requested == expect_sleep_request);
  if (expect_sleep_request) {
    CHECK(test_spy::gps_sleep_request_count == 1);
    CHECK(test_spy::gps_sleep_duration_ms == expected_gps_sleep_ms);
    CHECK(test_spy::last_saved_state.gps_wake_deadline_ms ==
          clock.retained_ms + expected_gps_sleep_ms);
  }
}

TEST_CASE("GoApp: deep-sleep wake restores GPS deadline and derives baud from application state") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  MockBoard board;
  board.settings.operating_mode = OperatingMode::Offline;
  board.settings.gps_mode = GpsMode::OnWhenTracking;
  RtcAppState state{};
  state.mode = OperatingMode::Offline;
  state.gps_wake_deadline_ms = clock.retained_ms + 60000;
  int expected_baud = GpsDriver::MODULE_DEFAULT_BAUD;
  SECTION("inactive GPS retains its sleep deadline") {}
  SECTION("tracking receiver remains at active baud") {
    state.tracking_state = TrackingState::Recording;
    state.gps_wake_deadline_ms = 0;
    expected_baud = GpsDriver::MODULE_ACTIVE_BAUD;
  }
  SECTION("Always On remains at active baud without tracking") {
    board.settings.gps_mode = GpsMode::AlwaysOn;
    state.gps_wake_deadline_ms = 0;
    expected_baud = GpsDriver::MODULE_ACTIVE_BAUD;
  }
  GoApp app(board);
  GoAppTestAccess access(app);
  access.run_button_wake_path(state);
  CHECK(test_spy::gps_initial_baud == expected_baud);
  CHECK(test_spy::gps_initial_deadline_ms == state.gps_wake_deadline_ms);
}

TEST_CASE("GoApp: interactive GPS initialization uses the supplied boot state") {
  test_spy::reset();
  ScopedRetainedClock clock(200999);
  MockBoard board;
  board.settings.gps_mode = GpsMode::OnWhenTracking;
  RtcAppState state{};
  WakeCause cause = WakeCause::Timer;
  int expected_baud = GpsDriver::MODULE_DEFAULT_BAUD;
  uint64_t expected_deadline_ms = 0;
  SECTION("supplied tracking state overrides stale RTC storage") {
    state.tracking_state = TrackingState::Recording;
    expected_baud = GpsDriver::MODULE_ACTIVE_BAUD;
  }
  SECTION("supplied sleep deadline is restored") {
    state.gps_wake_deadline_ms = clock.retained_ms + 60000;
    expected_deadline_ms = state.gps_wake_deadline_ms;
  }
  SECTION("cold boot uses default baud even with Always On") {
    cause = WakeCause::PowerOn;
    board.settings.gps_mode = GpsMode::AlwaysOn;
  }
  GoApp app(board);
  GoAppTestAccess access(app);
  access.run_interactive(cause, {}, state);
  CHECK(test_spy::gps_initial_baud == expected_baud);
  CHECK(test_spy::gps_initial_deadline_ms == expected_deadline_ms);
}

TEST_CASE("inactive GPS sleeps before warmup and preserves its deadline on promotion",
          "[GoApp][gps]") {
  test_spy::reset();
  ScopedRetainedClock clock(200000);
  MockBoard board;
  board.settings.operating_mode = OperatingMode::Offline;
  board.settings.gps_mode = GpsMode::AlwaysOff;
  board.settings.measure_interval_seconds = 60;
  test_spy::gas_begin_ok = true;
  RtcAppState state{};
  state.mode = OperatingMode::Offline;
  SECTION("button promotion") { board.press_button_on_isr_install = true; }
  SECTION("short interval promotion") {
    test_spy::sleep_decision_to_return = {PowerService::SleepType::None, 0};
  }
  SECTION("storage failure promotion") {
    state.tracking_state = TrackingState::Recording;
    test_spy::resume_route_result = false;
  }
  test_spy::before_sensor_measurement = [&] {
    REQUIRE(test_spy::gps_sleep_request_count == 1);
    CHECK(test_spy::gps_sleep_duration_ms == 60000);
  };
  GoApp app(board);
  GoAppTestAccess access(app);
  access.run_fast_path(state);
  CHECK_FALSE(test_spy::enter_sleep_called);
  CHECK_FALSE(test_spy::gps_read_called);
  REQUIRE(test_spy::orchestrator_init_called);
  CHECK(test_spy::last_saved_state.gps_wake_deadline_ms == 260000);
  CHECK(test_spy::orchestrator_rtc_state.gps_wake_deadline_ms == 260000);
  const auto &events = test_spy::fast_path_events;
  CHECK(std::find(events.begin(), events.end(), "gps_sleep") <
        std::find(events.begin(), events.end(), "gas_begin"));
}

TEST_CASE("inactive GPS final fallback uses ESP sleep duration", "[GoApp][gps]") {
  test_spy::reset();
  ScopedRetainedClock clock(200000);
  MockBoard board;
  board.settings.operating_mode = OperatingMode::Offline;
  board.settings.gps_mode = GpsMode::AlwaysOff;
  board.settings.measure_interval_seconds = 60;
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 40000};
  RtcAppState state{};
  state.mode = OperatingMode::Offline;
  bool early_attempt = true;
  bool final_success = true;
  SECTION("early request fails, final request succeeds") { test_spy::gps_sleep_ok = false; }
  SECTION("both requests fail") {
    test_spy::gps_sleep_ok = false;
    final_success = false;
  }
  SECTION("existing sleep expires during warmup") {
    state.gps_wake_deadline_ms = clock.retained_ms + 1000;
    test_spy::gps_remaining_ms = 1000;
    early_attempt = false;
  }
  SECTION("early sleep expires during a slow cycle") {}
  test_spy::before_sensor_measurement = [&] {
    CHECK(test_spy::gps_sleep_request_count == (early_attempt ? 1 : 0));
    if (early_attempt) {
      CHECK(test_spy::gps_sleep_duration_ms == 60000);
    }
    // Simulate elapsed retained time and the receiver's timer expiring.
    clock.retained_ms += 70000;
    test_spy::gps_remaining_ms = 0;
    test_spy::gps_sleep_ok = final_success;
  };
  GoApp app(board);
  GoAppTestAccess access(app);
  access.run_fast_path(state);
  CHECK(test_spy::enter_sleep_called);
  CHECK(test_spy::enter_sleep_duration_ms == 40000);
  CHECK(test_spy::gps_sleep_request_count == (early_attempt ? 2 : 1));
  CHECK(test_spy::gps_sleep_duration_ms == 40000);
  CHECK(test_spy::last_saved_state.gps_wake_deadline_ms ==
        (final_success ? clock.retained_ms + 40000 : 0));
}
