/**
 * AirGradient Go — GoApp Tests
 *
 * Tests for boot path selection, execute_fast_path(), and pure utility
 * functions.  Uses MockBoard + link-time stubs (same pattern as orchestrator
 * tests).
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "go_app.h"
#include "go_board.h"
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
#include "hal/bms_device.h"
#include "rtos.h"

// ============================================================================
// RTOS setup — required for RTOS::get_time_ms() and delay_ms() on host
// ============================================================================

static FreeRTOS s_rtos;

struct GlobalSetup {
  GlobalSetup() { RTOS::set_instance(&s_rtos); }
  ~GlobalSetup() { RTOS::set_instance(nullptr); }
};

static GlobalSetup s_global_setup;

// ============================================================================
// External test_spy (from go_app_stubs.cpp)
// ============================================================================

namespace test_spy {
extern RtcAppState rtc_state;
extern RtcDisplaySnapshot rtc_snapshot;
extern bool rtc_snapshot_valid;
extern int warmup_step_count;
extern int pm_sleep_count;
extern Measures measures_to_return;
extern bool sensor_started;
extern bool gps_started;
extern bool gps_idle_called;
extern GpsData gps_data_to_return;
extern bool input_started;
extern bool cache_measurement_called;
extern MeasuresAGo last_cached_measurement;
extern bool route_started;
extern bool route_resumed;
extern uint32_t route_session_id;
extern bool route_point_appended;
extern bool resume_route_result;
extern bool append_route_point_result;
extern bool route_ended;
extern bool cache_backed_up;
extern bool bms_polled;
extern bool state_saved;
extern RtcAppState last_saved_state;
extern PowerSnapshot snapshot_to_return;
extern PowerService::SleepDecision sleep_decision_to_return;
extern bool enter_sleep_called;
extern uint32_t enter_sleep_duration_ms;
extern bool should_hold_pm_result;
extern bool shutdown_called;
extern bool orchestrator_init_called;
extern bool orchestrator_run_called;
extern WakeCause orchestrator_wake_cause;
extern BootHandoff orchestrator_handoff;
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
extern float bms_battery_pct;
extern void reset();
} // namespace test_spy

// ============================================================================
// Stub BmsDevice for MockBoard
// ============================================================================

class StubBmsDevice : public BmsDevice {
public:
  bool init() override { return true; }
  bool read_telemetry(BmsTelemetry &out) override {
    out = BmsTelemetry{};
    return true;
  }
  bool read_status(BmsStatus &out) override {
    out = BmsStatus{};
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
static int stub_get_level(int) { return 0; }
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

class MockBoard : public GoBoard {
public:
  // Call ordering log — records every method call in sequence.
  std::vector<std::string> call_log;

  // Init tracking
  bool nvs_init_called = false;
  bool buses_init_called = false;
  bool spi_init_called = false;
  bool bms_init_called = false;
  bool core_init_called = false;
  bool sensors_warm_arg = false;
  bool sensors_called = false;

  // ISR
  volatile bool *isr_flag = nullptr;
  int isr_pin = -1;
  bool isr_installed = false;
  bool isr_removed = false;

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
  void init_bms() override {
    call_log.push_back("init_bms");
    bms_init_called = true;
  }
  void init_core() override {
    call_log.push_back("init_core");
    core_init_called = true;
    init_nvs();
    init_buses();
    init_spi();
    init_bms();
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
    return *reinterpret_cast<ConfigStore *>(&_config_store_buf);
  }
  GoSettings load_settings() override {
    call_log.push_back("load_settings");
    return settings;
  }
  BmsDevice *bms() override {
    call_log.push_back("bms");
    return &_bms;
  }
  SensorManager &sensors(bool warm) override {
    call_log.push_back("sensors");
    sensors_warm_arg = warm;
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

  // Radio accessors return reinterpret_cast'd dummies.  CP1 production
  // code never invokes these on MockBoard (Portable boots take only
  // ble_server(), which goes through BleService stubs that ignore it).
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
    return *reinterpret_cast<AgBleServer *>(&_ble_server_buf);
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
  const gpio::Hal &gpio_hal() override { return stub_gpio_hal; }
  void release_gpio_holds() override { call_log.push_back("release_gpio_holds"); }
  void ulp_stop() override {}
  void ulp_start() override {}

  void install_button_isr(int pin, volatile bool *flag) override {
    isr_pin = pin;
    isr_flag = flag;
    isr_installed = true;
  }
  void remove_button_isr(int /*pin*/) override { isr_removed = true; }

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

private:
  // Stub service instances.
  // The stub constructors store refs but never dereference them, so
  // we use a helper to produce "valid" dummy references for construction.
  StubBmsDevice _bms;
  Sensors _sensors_struct{};
  SensorManager _sensor_manager{_sensors_struct};
  StubTouch _touch;

  // Dummy byte arrays used to create "valid" references for stub constructors.
  // The stubs never actually use these objects.
  alignas(PayloadCache) static inline char s_cache_buf[sizeof(PayloadCache)];
  alignas(NandStorage) static inline char s_nand_buf[sizeof(NandStorage)];
  alignas(8) static inline char _config_store_buf[64];
  alignas(8) static inline char _gps_driver_buf[512];
  // Radio dummy buffers — never dereferenced through the abstract types.
  alignas(8) static inline char _wifi_hal_buf[64];
  alignas(8) static inline char _wifi_manager_buf[64];
  alignas(8) static inline char _http_server_buf[64];
  alignas(8) static inline char _ble_server_buf[64];
  AgClient _ag_client{};

  StorageService _storage{*reinterpret_cast<PayloadCache *>(s_cache_buf),
                          *reinterpret_cast<NandStorage *>(s_nand_buf)};
  DisplayService _display{{}};
  LedService _led{{}};       // inert mode (null driver)
  BuzzerService _buzzer{{}}; // inert mode (null driver)
  PowerService _power{&_bms, stub_gpio_hal, {}};
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

  FastPathResult execute_fast_path(const RtcAppState &state, const volatile bool &button,
                                   const RtcDisplaySnapshot *snapshot = nullptr,
                                   bool snapshot_valid = false) {
    return _app.execute_fast_path(state, button, snapshot, snapshot_valid);
  }

  void run_button_wake_path(const RtcAppState &state) { _app.run_button_wake_path(state); }

  void run_fast_path(const RtcAppState &state) { _app.run_fast_path(state); }

  void run_interactive(WakeCause cause, BootHandoff handoff = {}) {
    _app.run_interactive(cause, handoff);
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
  CHECK(select_boot_path(WakeCause::Timer, state) == BootPath::FastPath);
}

TEST_CASE("select_boot_path: Timer + Unlocked -> Interactive") {
  RtcAppState state{};
  state.lock_state = LockState::Unlocked;
  CHECK(select_boot_path(WakeCause::Timer, state) == BootPath::Interactive);
}

TEST_CASE("select_boot_path: Button + Offline -> ButtonWake") {
  RtcAppState state{};
  state.mode = OperatingMode::Offline;
  CHECK(select_boot_path(WakeCause::Button, state) == BootPath::ButtonWake);
}

TEST_CASE("select_boot_path: Button + Portable -> Interactive") {
  RtcAppState state{};
  state.mode = OperatingMode::Portable;
  CHECK(select_boot_path(WakeCause::Button, state) == BootPath::Interactive);
}

TEST_CASE("select_boot_path: PowerOn -> Interactive") {
  RtcAppState state{};
  CHECK(select_boot_path(WakeCause::PowerOn, state) == BootPath::Interactive);
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
  state.tracking_active = true;
  CHECK(is_gps_active_at_boot(s, state) == false);
}

TEST_CASE("is_gps_active_at_boot: OnWhenTracking + tracking -> true") {
  GoSettings s{};
  s.gps_mode = GpsMode::OnWhenTracking;
  RtcAppState state{};
  state.tracking_active = true;
  CHECK(is_gps_active_at_boot(s, state) == true);
}

TEST_CASE("is_gps_active_at_boot: OnWhenTracking + idle -> false") {
  GoSettings s{};
  s.gps_mode = GpsMode::OnWhenTracking;
  RtcAppState state{};
  state.tracking_active = false;
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

  DisplayValues v = build_fast_path_display(m, gps, bms, settings, true);

  CHECK(v.co2_ppm == 600);
  CHECK(v.pm25_ugm3 == 15.0f);
  CHECK(v.temperature_c == 25.0f);
  CHECK(v.humidity_pct == 60.0f);
  CHECK(v.battery_pct == 75);
  CHECK(v.locked == true);
  CHECK(v.tracking_active == true);
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

  DisplayValues v = build_fast_path_display(m, gps, bms, settings, false);

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
  CHECK(v.tracking_active == false);
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
      build_fast_path_display(m, GpsData{}, PowerSnapshot{}, settings, false);

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
  state.sensors_warm = true;
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
  state.sensors_warm = false;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(result.has_measures == true);
  CHECK(result.sensors_warm == false);
  CHECK(board.sensors_warm_arg == false);
  CHECK(test_spy::pm_sleep_count == 1); // long sleep — fan stopped for the window
  // Warmup iterations should have been called
  CHECK(test_spy::warmup_step_count > 0);
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
    state.sensors_warm = true;

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

TEST_CASE("execute_fast_path: button during warmup -> promote unlocked") {
  test_spy::reset();

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.sensors_warm = false;
  volatile bool button = true; // already pressed

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK(result.handoff.initial_lock_state == LockState::Unlocked);
  CHECK(result.handoff.suppress_wake_press == true);
  CHECK(result.has_measures == false);
}

TEST_CASE("execute_fast_path: sleep too short -> promote locked") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::None, 0};

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.sensors_warm = true;
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
  test_spy::bms_battery_pct = 80.0f;

  MockBoard board;
  board.settings.gps_mode = GpsMode::AlwaysOn;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.sensors_warm = true;
  state.tracking_active = true;
  state.tracking_session_id = 12345;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  // Fast path can never start a new session — it always resumes.
  CHECK(test_spy::route_resumed == true);
  CHECK(test_spy::route_started == false);
  CHECK(test_spy::route_session_id == 12345);
  CHECK(test_spy::route_point_appended == true);
  CHECK(test_spy::route_ended == true);
  CHECK(board.new_gps_driver_called == true);
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
  state.sensors_warm = true;
  state.tracking_active = true;
  state.tracking_session_id = 12345;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  // tracking_active stays set in the inbound state so the orchestrator
  // retries the resume during init() and surfaces the failure there.
  CHECK(state.tracking_active == true);
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
  state.sensors_warm = true;
  state.tracking_active = true;
  state.tracking_session_id = 12345;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Promote);
  CHECK(state.tracking_active == true);
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
  state.sensors_warm = true;
  state.tracking_active = true;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(board.new_gps_driver_called == false);
}

TEST_CASE("execute_fast_path: no tracking -> no route") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.sensors_warm = true;
  state.tracking_active = false;
  volatile bool button = false;

  auto result = access.execute_fast_path(state, button);

  CHECK(result.outcome == GoAppTestAccess::Outcome::Sleep);
  CHECK(test_spy::route_started == false);
}

// ============================================================================
// Tests: call ordering in execute_fast_path
// ============================================================================

TEST_CASE("execute_fast_path: init ordering — init_core before load_settings before sensors") {
  test_spy::reset();
  test_spy::sleep_decision_to_return = {PowerService::SleepType::Deep, 60000};

  MockBoard board;
  GoApp app(board);
  GoAppTestAccess access(app);

  RtcAppState state{};
  state.sensors_warm = true;
  volatile bool button = false;

  access.execute_fast_path(state, button);

  // init_core must happen before load_settings (NVS prerequisite)
  CHECK(board.call_index("init_core") >= 0);
  CHECK(board.call_index("load_settings") >= 0);
  CHECK(board.call_index("sensors") >= 0);
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
  state.sensors_warm = true;
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
  state.sensors_warm = true;
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
  test_spy::orchestrator_local_api->set_access(ConfigAccess::ReadWrite);
  CHECK(test_spy::orchestrator_local_api->trigger(ActionId::CalibrateCo2).status ==
        ActionStatus::Dispatched);
}
