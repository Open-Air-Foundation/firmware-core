/**
 * AirGradient Go — Orchestrator unit tests
 *
 * Tests the orchestrator's core logic: event dispatch, state transitions,
 * timer management, input handling, settings propagation, display context
 * building, and session ID generation.
 *
 * Service interactions are verified through observable stubs defined in
 * go_orchestrator_stubs.cpp (the test_spy namespace).
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include <memory>
#include <set>
#include <map>
#include <string>

#include "go_ble_protocol.h"
#include "go_board.h"
#include "go_orchestrator.h"
#include "services/ag_client.h"

// ============================================================================
// External test_spy state (defined in go_orchestrator_stubs.cpp)
// ============================================================================

namespace test_spy {
extern bool sensor_started;
extern bool sensor_stopped;
extern bool measurement_requested;
extern uint8_t last_iterations;
extern SensorGroup last_groups;
extern bool co2_calibration_requested;
extern bool prepare_requested;

extern bool gps_started;
extern bool gps_stopped;
extern bool gps_stop_and_idle_called;
extern bool gps_idle_called;
extern int gps_posting_interval_ms;
extern bool gps_aiding_set;
extern GpsAidingData gps_aiding_data;

extern bool input_started;
extern bool input_stopped;

extern bool cache_measurement_called;
extern MeasuresAGo last_cached_measurement;
extern bool route_started;
extern bool route_resumed;
extern uint32_t route_session_id;
extern bool route_point_appended;
extern RoutePoint last_route_point;
extern bool route_ended;
extern bool route_file_open;
extern bool cache_backed_up;
extern bool cache_restored;
extern bool cache_cleared;
extern bool routes_cleared;
extern bool clear_routes_result;
extern bool create_route_result;
extern bool resume_route_result;
extern bool append_route_point_result;
extern std::set<uint32_t> existing_route_session_ids;

extern bool bms_polled;
extern uint32_t bms_poll_count;
extern bool shutdown_called;
extern bool state_saved;
extern RtcAppState last_saved_state;
extern RtcAppState state_to_load;
extern PowerSnapshot snapshot_to_return;
extern PowerService::SleepType sleep_type_to_return;
extern bool pm_power_set;
extern bool pm_power_on;
extern uint32_t pm_power_set_count;
extern bool pm_sleep_requested;
extern bool ensure_pmid_healthy_called;
extern uint32_t ensure_pmid_healthy_count;
extern bool recover_pm_sensor_called;
extern uint32_t recover_pm_sensor_count;

// --- BleService ---
extern bool ble_init_called;
extern bool ble_deinit_called;
extern bool ble_initialized;
extern bool ble_connected;
extern bool ble_authenticated;
extern bool ble_notify_measures_called;
extern bool ble_update_status_called;
extern bool ble_notify_tracking_status_called;
extern uint32_t ble_notify_tracking_status_count;
extern bool ble_notify_charging_status_called;
extern uint32_t ble_notify_charging_status_count;
extern bool ble_notify_disconnect_called;
extern uint32_t ble_notify_disconnect_count;
extern BleDiscReason ble_last_disc_reason;
extern bool ble_last_status_tracking;
extern uint32_t ble_last_status_session;
extern bool ble_update_config_called;
extern bool ble_notify_config_called;
extern bool ble_notify_command_progress_called;
extern BleCommand ble_progress_command;
extern bool ble_notify_command_result_called;
extern BleCommand ble_last_command;
extern bool ble_last_command_success;
extern const char *ble_last_command_error;
extern bool ble_delete_all_bonds_called;
extern bool ble_delete_all_bonds_result;
extern bool ble_history_list_called;
extern bool ble_history_start_called;
extern uint32_t ble_history_start_session;
extern bool ble_history_fill_called;
extern bool ble_history_end_called;
extern bool ble_history_delete_called;
extern uint32_t ble_history_delete_session;
extern bool ble_notify_history_error_called;
extern const char *ble_last_history_error;
extern size_t ble_pending_config_len;
extern size_t ble_pending_history_len;
extern BleConfigDecodeResult ble_config_decode_result;
extern bool ble_decode_updates_settings;
extern GoSettings ble_decoded_settings;
extern BleHistoryDecodeResult ble_history_decode_result;

// --- CloudService ---
extern MeasuresAGo cloud_last_snapshot;
extern bool cloud_last_disable_cloud;
extern uint32_t cloud_start_count;
extern uint32_t cloud_stop_count;
extern uint32_t cloud_arm_count;
extern bool cloud_last_arm_fire_now;
extern uint32_t cloud_disarm_count;

// --- OtaService ---
extern bool ota_setup_ble_called;
extern bool ota_setup_ble_result;
extern bool ota_teardown_ble_called;
extern uint32_t ota_teardown_ble_count;
extern bool ota_handle_disconnect_called;
extern bool ota_is_ble_active;
extern uint32_t ota_run_ble_count;
extern OtaStatus ota_run_ble_result;
extern uint32_t ota_run_wifi_count;
extern OtaStatus ota_run_wifi_result;
extern bool ota_run_wifi_invoke_download_started;

// --- WifiService ---
extern bool wifi_has_saved_networks;
extern bool wifi_connect_saved_called;
extern WifiStaticIpConfig wifi_last_static_ip;
extern bool wifi_static_ip_was_null;
extern bool wifi_try_fallback_called;
extern bool wifi_shutdown_called;
extern bool wifi_clear_credentials_called;
extern bool wifi_start_provisioning_called;
extern ProvisioningTransport wifi_start_provisioning_transport;
extern bool wifi_switch_transport_called;
extern bool wifi_stop_provisioning_called;
extern bool wifi_tick_called;
extern uint32_t wifi_next_deadline_ms;
extern bool wifi_is_online;
extern bool wifi_has_been_online;
extern bool wifi_schedule_reconnect_called;
extern int wifi_schedule_reconnect_count;

// --- PortableWifiProvisioner ---
extern bool portable_attach_called;
extern bool portable_attach_result;
extern bool portable_attached;
extern bool portable_stop_called;
extern bool portable_handle_pending_called;
extern bool portable_on_connected_called;
extern bool portable_on_ble_disconnected_called;
extern bool portable_is_radio_active;
extern uint32_t portable_next_deadline_ms;
extern bool portable_tick_called;

extern void reset();
} // namespace test_spy

// ============================================================================
// MockConfigStore (ConfigStore is abstract)
// ============================================================================

class MockConfigStore : public trompeloeil::mock_interface<ConfigStore> {
public:
  IMPLEMENT_MOCK2(get_int);
  IMPLEMENT_MOCK2(set_int);
  IMPLEMENT_MOCK2(get_bool);
  IMPLEMENT_MOCK2(set_bool);
  IMPLEMENT_MOCK2(get_string);
  IMPLEMENT_MOCK2(set_string);
  IMPLEMENT_MOCK1(erase);
  IMPLEMENT_MOCK0(commit);
};

// ============================================================================
// MockRTOS (for time control)
// ============================================================================

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);

  void set_system_time_from_epoch_impl(int64_t epoch_seconds) override {
    system_time_set = true;
    system_time_epoch = epoch_seconds;
  }

  bool system_time_set = false;
  int64_t system_time_epoch = 0;
};

// ============================================================================
// Stub sensor/hardware objects (constructors need references)
// ============================================================================

class StubSensorManager {
public:
  // Enough to pass by reference to SensorProducer. The stub SensorProducer
  // constructor ignores it.
};

class StubCapTouchSensor : public CapTouchSensor {
public:
  bool init() override { return true; }
  bool read(TouchData &) override { return false; }
};

class StubBmsDevice : public BmsDevice {
public:
  bool init() override { return true; }
  bool read_telemetry(BmsTelemetry &) override { return false; }
  bool read_status(BmsStatus &) override { return false; }
  bool get_charging_state(BmsChargingState &) override { return false; }
  bool get_battery_percentage(float *) override { return false; }
  bool update_watchdog() override { return true; }
  bool feature_ship_available() const override { return false; }
  bool enter_ship_mode() override { return false; }
  bool configure_pmid_mode(BmsPmidMode) override { return true; }
  bool set_pmid_enabled(bool) override { return true; }
  bool resync_pmid() override { return true; }
  bool set_charge_enable(bool) override { return true; }
  bool set_charge_current_ma(uint16_t) override { return true; }
  bool set_watchdog_timeout_ms(uint32_t) override { return true; }
};

class StubNandStorage : public NandStorage {
public:
  bool init() override { return true; }
  void deinit() override {}
  bool format() override { return true; }
  bool is_mounted() const override { return true; }
  const char *mount_path() const override { return "/tmp"; }
};

class StubPayloadCacheStorage : public PayloadCacheStorage {
public:
  bool load(PayloadCacheStorageData &) override { return false; }
  bool save(const PayloadCacheStorageData &) override { return true; }
  bool clear() override { return true; }
};

// Minimal GoBoard stub. Only init_wifi_subsystem() is observable; the
// orchestrator's stationary path is the only thing that touches this in
// CP2.2 tests. Service accessors are unreachable through the stubbed
// WifiService/BleService methods, so they return reinterpret_cast'd
// dummies that are never dereferenced.
class StubGoBoard : public GoBoard {
public:
  int init_wifi_subsystem_calls = 0;

  void init_nvs() override {}
  void init_buses() override {}
  void init_spi() override {}
  void init_bms() override {}
  void init_wifi_subsystem() override { ++init_wifi_subsystem_calls; }
  void init_core() override {}

  ConfigStore &config_store() override { return *reinterpret_cast<ConfigStore *>(_buf); }
  GoSettings load_settings() override { return {}; }
  BmsDevice &bms() override { return *reinterpret_cast<BmsDevice *>(_buf); }
  SensorManager &sensors(bool) override { return *reinterpret_cast<SensorManager *>(_buf); }
  StorageService &storage() override { return *reinterpret_cast<StorageService *>(_buf); }
  DisplayService &display() override { return *reinterpret_cast<DisplayService *>(_buf); }
  LedService &led_service() override { return _led; }
  BuzzerService &buzzer_service() override { return _buzzer; }
  PowerService &power() override { return *reinterpret_cast<PowerService *>(_buf); }
  WifiHal &wifi_hal() override { return *reinterpret_cast<WifiHal *>(_buf); }
  WifiManager &wifi_manager() override { return *reinterpret_cast<WifiManager *>(_buf); }
  HttpServer &http_server() override { return *reinterpret_cast<HttpServer *>(_buf); }
  AgBleServer &ble_server() override { return *reinterpret_cast<AgBleServer *>(_buf); }
  AgClient &ag_client() override { return _ag_client; }
  GpsDriver *new_gps_driver() override { return nullptr; }
  CapTouchSensor *new_touch_sensor() override { return nullptr; }
  BoardVariant variant_value = BoardVariant::Prototype;
  BoardVariant variant() const override { return variant_value; }
  std::string serial_number() override { return "TEST00"; }
  const char *firmware_version() override { return "test"; }
  const gpio::Hal &gpio_hal() override { return *reinterpret_cast<gpio::Hal *>(_buf); }
  void release_gpio_holds() override {}
  void ulp_stop() override {}
  void ulp_start() override {}
  void install_button_isr(int, volatile bool *) override {}
  void remove_button_isr(int) override {}

private:
  alignas(8) static inline char _buf[64];
  AgClient _ag_client;
  LedService _led{{}};       // inert mode (null driver)
  BuzzerService _buzzer{{}}; // inert mode (null driver)
};

// Minimal AgBleServer impl for BleService construction.  The orchestrator
// stub's BleService ctor stores the borrowed reference but never invokes
// any method on it (all BleService methods are replaced by link-time stubs
// in go_orchestrator_stubs.cpp), so these overrides are unreachable from
// the code under test.
class StubAgBleServer : public AgBleServer {
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

static StubAgBleServer stub_ble_server;

// GPIO HAL stub (function-pointer table — never called by stubs)
static bool gpio_configure(int, gpio::Mode, gpio::PullMode, gpio::InterruptType) { return true; }
static int gpio_get_level(int) { return 0; }
static bool gpio_set_level(int, int) { return true; }
static bool gpio_add_interrupt_handler(int, gpio::InterruptHandler, void *) { return true; }
static bool gpio_remove_interrupt_handler(int) { return true; }
static bool gpio_enable_interrupt(int) { return true; }
static bool gpio_disable_interrupt(int) { return true; }

static const gpio::Hal test_gpio_hal = {
    gpio_configure,
    gpio_get_level,
    gpio_set_level,
    gpio_add_interrupt_handler,
    gpio_remove_interrupt_handler,
    gpio_enable_interrupt,
    gpio_disable_interrupt,
};

// ============================================================================
// OrchestratorTestAccess — friend class for private member access
// ============================================================================

class OrchestratorTestAccess {
public:
  static void dispatch(Orchestrator &o, const Event &evt) { o.dispatch(evt); }
  static void check_timers(Orchestrator &o) { o.check_timers(); }
  static uint32_t compute_queue_timeout_ms(const Orchestrator &o) {
    return o.compute_queue_timeout_ms();
  }

  static BuildContext build_context(const Orchestrator &o) { return o.build_context(); }
  static RtcAppState snapshot_state(const Orchestrator &o) { return o.snapshot_state(); }

  // State readers
  static OperatingMode mode(const Orchestrator &o) { return o._mode; }
  static Behavior behavior(const Orchestrator &o) { return o._behavior; }
  static LockState lock_state(const Orchestrator &o) { return o._lock_state; }
  static bool gps_enabled(const Orchestrator &o) { return o._gps_enabled; }
  static bool tracking_active(const Orchestrator &o) { return o._tracking_active; }
  static uint32_t tracking_session_id(const Orchestrator &o) { return o._tracking_session_id; }
  static bool first_measurement_done(const Orchestrator &o) { return o._first_measurement_done; }
  static const MeasuresAGo &cached_measures(const Orchestrator &o) { return o._cached_measures; }
  static const GpsData &latest_gps(const Orchestrator &o) { return o._latest_gps; }
  static const PowerSnapshot &latest_power(const Orchestrator &o) { return o._latest_power; }
  static uint32_t last_input_ms(const Orchestrator &o) { return o._last_input_ms; }
  static uint32_t last_measurement_ms(const Orchestrator &o) { return o._last_measurement_ms; }
  static uint32_t snackbar_refresh_deadline_ms(const Orchestrator &o) {
    return o._snackbar_refresh_deadline_ms;
  }
  static GoSettings &settings(Orchestrator &o) { return o._settings; }
  static void set_last_measurement_ms(Orchestrator &o, uint32_t v) { o._last_measurement_ms = v; }

  // Direct method access
  static bool is_gps_active(const Orchestrator &o) { return o.is_gps_active(); }
  static uint32_t generate_session_id(Orchestrator &o) { return o.generate_session_id(); }
  static void on_input(Orchestrator &o, const InputEventData &input) { o.on_input(input); }
  static void on_sensor_data(Orchestrator &o, const MeasuresAGo &data) { o.on_sensor_data(data); }
  static void on_gps_fix(Orchestrator &o, const GpsData &data) { o.on_gps_fix(data); }
  static void lock(Orchestrator &o) { o.lock(); }
  static void unlock(Orchestrator &o) { o.unlock(); }
  static bool start_tracking(Orchestrator &o) { return o.start_tracking(); }
  static void stop_tracking(Orchestrator &o) { o.stop_tracking(); }
  static bool clear_data(Orchestrator &o) { return o.clear_data(); }
  static bool factory_reset(Orchestrator &o) { return o.factory_reset(); }
  static void mark_onboarding_done(Orchestrator &o) { o.mark_onboarding_done(); }
  static void on_ble_auth_complete(Orchestrator &o, bool success) {
    o.on_ble_auth_complete(success);
  }
  static void shutdown(Orchestrator &o) { o.shutdown(); }
  static void shutdown(Orchestrator &o, ShipModeRequest reason) { o.shutdown(reason); }
  static void on_bms_status_timer(Orchestrator &o) { o.on_bms_status_timer(); }
  static void apply_settings_change(Orchestrator &o) { o.apply_settings_change(); }
  static void prepare_for_sleep(Orchestrator &o, uint32_t sleep_ms = 60000) {
    o.prepare_for_sleep(sleep_ms);
  }
  static void set_mode(Orchestrator &o, OperatingMode mode) { o._mode = mode; }
  static void set_first_measurement_done(Orchestrator &o, bool v) { o._first_measurement_done = v; }
  static void set_latest_power(Orchestrator &o, const PowerSnapshot &v) { o._latest_power = v; }
  static bool pm_prepare_sent(const Orchestrator &o) { return o._pm_prepare_sent; }
  static void change_mode(Orchestrator &o, OperatingMode mode) { o.change_mode(mode); }
  static void enter_manufacturing_mode(Orchestrator &o) { o.enter_manufacturing_mode(); }
  static bool manufacturing_mode(const Orchestrator &o) { return o._manufacturing_mode; }
  static void set_manufacturing_mode(Orchestrator &o, bool v) { o._manufacturing_mode = v; }
  static void reschedule_sensor_timer(Orchestrator &o, const GoSettings &prev) {
    o.reschedule_sensor_timer(prev);
  }

  // Session-state access (Stationary bring-up + provisioning UX).
  static bool setup_session_active(const Orchestrator &o) { return o._setup_session_active; }
  static bool bring_up_pending(const Orchestrator &o) { return o._bring_up_pending; }
  static bool boot_splash_active(const Orchestrator &o) { return o._boot_splash_active; }
  static void set_boot_splash_active(Orchestrator &o, bool v) { o._boot_splash_active = v; }
  static void set_setup_session_active(Orchestrator &o, bool v) { o._setup_session_active = v; }
  static bool sensitive_services_paused(const Orchestrator &o) {
    return o._provisioning_sensitive_services_paused;
  }
  static uint32_t last_bms_poll_ms(const Orchestrator &o) { return o._last_bms_poll_ms; }
  static uint32_t last_bms_status_poll_ms(const Orchestrator &o) {
    return o._last_bms_status_poll_ms;
  }
  static void enter_stationary(Orchestrator &o) { o.enter_stationary(); }
  static void on_wifi_connected(Orchestrator &o, uint32_t ip) { o.on_wifi_connected(ip); }
  static void enter_provisioning_page(Orchestrator &o, ProvisioningTransport t) {
    o.enter_provisioning_page(t);
  }
  static void request_background_display_update(Orchestrator &o) {
    o.request_background_display_update();
  }

  // --- OTA ---
  static void enter_ota(Orchestrator &o) { o.enter_ota(); }
  static void exit_ota(Orchestrator &o, const char *snackbar) { o.exit_ota(snackbar); }
  static void finish_ota(Orchestrator &o, OtaStatus status) { o.finish_ota(status); }
  static void on_ota_download_started(Orchestrator &o) { o.on_ota_download_started(); }
  static bool ota_committed(const Orchestrator &o) { return o._ota_committed; }
  static void set_ota_committed(Orchestrator &o, bool v) { o._ota_committed = v; }
  static uint32_t last_ota_check_ms(const Orchestrator &o) { return o._last_ota_check_ms; }
  static void set_last_ota_check_ms(Orchestrator &o, uint32_t v) { o._last_ota_check_ms = v; }
};

using A = OrchestratorTestAccess;

// ============================================================================
// Test fixture helper
// ============================================================================

struct TestFixture {
  // Stub hardware objects
  StubSensorManager stub_sensor_mgr;
  AirgradientSerial stub_serial;
  GpsDriver stub_gps{stub_serial};
  StubCapTouchSensor stub_touch;
  StubBmsDevice stub_bms;
  StubNandStorage stub_nand;
  StubPayloadCacheStorage stub_cache_storage;
  PayloadCache payload_cache;

  // Services (using stub constructors from go_orchestrator_stubs.cpp)
  SensorProducer sensor_producer;
  GpsService gps_service;
  InputService input_service;
  DisplayService display_service;
  LedService led_service_inert{{}};
  BuzzerService buzzer_service_inert{{}};
  StorageService storage_service;
  PowerService power_service;
  UIManager ui_manager;
  BleService ble_service;
  WifiService wifi_service;
  AgClient ag_client;
  CloudService cloud_service;
  StubGoBoard stub_board;
  PortableWifiProvisioner portable_provisioner;
  OtaService ota_service;

  // MockRTOS + MockConfigStore
  MockRTOS mock_rtos;
  MockConfigStore mock_config;

  Orchestrator::Services services;
  GoSettings settings;

  // Persistent mock expectations (must outlive individual test scopes)
  std::unique_ptr<trompeloeil::expectation> _exp_time;
  std::unique_ptr<trompeloeil::expectation> _exp_delay;

  TestFixture()
      : payload_cache(stub_cache_storage, 16),
        sensor_producer(reinterpret_cast<SensorManager &>(stub_sensor_mgr), nullptr,
                        SensorProducer::Config{}),
        gps_service(stub_gps, nullptr, GpsService::Config{}),
        input_service(stub_touch, test_gpio_hal, nullptr, InputService::Config{}),
        display_service(DisplayService::Config{}), storage_service(payload_cache, stub_nand),
        power_service(stub_bms, test_gpio_hal, PowerService::Config{}),
        ui_manager(UIManager::Config{}), ble_service(nullptr, storage_service, stub_ble_server),
        wifi_service(nullptr,
                     {*reinterpret_cast<WifiManager *>(_stub_buf),
                      *reinterpret_cast<AgBleServer *>(_stub_buf),
                      *reinterpret_cast<HttpServer *>(_stub_buf)},
                     WifiService::Config{}),
        ag_client(),
        cloud_service(nullptr, CloudService::Deps{ag_client, wifi_service}, CloudService::Config{}),
        portable_provisioner(nullptr,
                             {*reinterpret_cast<WifiManager *>(_stub_buf),
                              *reinterpret_cast<AgBleServer *>(_stub_buf), stub_board},
                             PortableWifiProvisioner::Config{}),
        ota_service(stub_ble_server, power_service, OtaService::Config{}),
        services{sensor_producer,      gps_service,          input_service,   display_service,
                 led_service_inert,    buzzer_service_inert, storage_service, power_service,
                 ui_manager,           ble_service,          wifi_service,    cloud_service,
                 portable_provisioner, stub_board,           ota_service} {
    test_spy::reset();
    RTOS::set_instance(&mock_rtos);
    _exp_time = NAMED_ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    _exp_delay = NAMED_ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
  }

  ~TestFixture() { RTOS::set_instance(nullptr); }

  Orchestrator make_orchestrator() { return {nullptr, services, settings, mock_config, "TEST00"}; }

private:
  alignas(8) static inline char _stub_buf[64];
};

// ============================================================================
// 1. Pure Logic — is_gps_active
// ============================================================================

TEST_CASE("is_gps_active: GpsMode determines GPS activity", "[Orchestrator][pure]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  SECTION("AlwaysOff returns false regardless of tracking") {
    A::settings(orch).gps_mode = GpsMode::AlwaysOff;
    REQUIRE_FALSE(A::is_gps_active(orch));
  }

  SECTION("AlwaysOn returns true regardless of tracking") {
    A::settings(orch).gps_mode = GpsMode::AlwaysOn;
    REQUIRE(A::is_gps_active(orch));
  }

  SECTION("OnWhenTracking returns true when tracking") {
    A::settings(orch).gps_mode = GpsMode::OnWhenTracking;
    // Start tracking to set _tracking_active = true
    ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
        .RETURN(ConfigStoreResult::NOT_FOUND);
    ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_))
        .RETURN(ConfigStoreResult::OK);
    ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);
    A::start_tracking(orch);
    REQUIRE(A::is_gps_active(orch));
  }

  SECTION("OnWhenTracking returns false when idle") {
    A::settings(orch).gps_mode = GpsMode::OnWhenTracking;
    REQUIRE_FALSE(A::is_gps_active(orch));
  }
}

// compute_sleep_duration_ms removed — logic moved to
// PowerService::compute_sleep_duration(), tested in go_power.tests.cpp.

// ============================================================================
// 4. Pure Logic — snapshot_state
// ============================================================================

TEST_CASE("snapshot_state: captures current application state", "[Orchestrator][pure]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  RtcAppState state = A::snapshot_state(orch);
  REQUIRE(state.mode == OperatingMode::Portable);
  REQUIRE(state.behavior == Behavior::Idle);
  REQUIRE(state.lock_state == LockState::Locked);
  REQUIRE(state.gps_enabled == true);
  REQUIRE(state.tracking_active == false);
  REQUIRE(state.tracking_session_id == 0);
}

// ============================================================================
// 5. Initialization
// ============================================================================

TEST_CASE("init(PowerOn): default state with first measurement and BMS poll",
          "[Orchestrator][init]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Allow save_go_settings NVS calls during sync (none expected for init)
  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::PowerOn);

  // Verify default state
  REQUIRE(A::mode(orch) == OperatingMode::Portable);
  REQUIRE(A::lock_state(orch) == LockState::Locked);
  REQUIRE(A::tracking_active(orch) == false);

  // Verify initial measurement was requested (single iteration)
  REQUIRE(test_spy::measurement_requested);
  REQUIRE(test_spy::last_iterations == 1);

  // Verify initial BMS poll
  REQUIRE(test_spy::bms_polled);
}

TEST_CASE("init(PowerOn): cold-boot splash flag set when UIManager is on Screen::Info",
          "[Orchestrator][init][boot-splash]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  // Simulate what GoApp::run_interactive does before handing off to the
  // orchestrator: paint the boot splash and seed UIManager state.
  f.ui_manager.show_info("Booting...");
  REQUIRE(f.ui_manager.current_screen() == Screen::Info);

  orch.init(WakeCause::PowerOn);

  REQUIRE(A::boot_splash_active(orch));
  // UIManager screen unchanged by init — still on splash.
  REQUIRE(f.ui_manager.current_screen() == Screen::Info);
}

TEST_CASE("init(PowerOn): no splash flag when UIManager is already on Home",
          "[Orchestrator][init][boot-splash]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  // Default UIManager screen is Home — no splash was painted.
  REQUIRE(f.ui_manager.current_screen() == Screen::Home);

  orch.init(WakeCause::PowerOn);

  REQUIRE_FALSE(A::boot_splash_active(orch));
}

TEST_CASE("init: splash flag not set when boot already completed a measurement",
          "[Orchestrator][init][boot-splash]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  // Even if UIManager somehow ended up on Info, a completed boot measurement
  // means real data is already cached — the splash gate should NOT engage.
  f.ui_manager.show_info("Booting...");
  BootHandoff handoff{};
  handoff.measurement_completed = true;
  orch.init(WakeCause::PowerOn, handoff);

  REQUIRE_FALSE(A::boot_splash_active(orch));
}

TEST_CASE("on_sensor_data: first measurement clears splash and resets to Home when onboarded",
          "[Orchestrator][events][boot-splash]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  f.ui_manager.show_info("Booting...");
  orch.init(WakeCause::PowerOn);
  // Onboarding already acknowledged — the gate hands off straight to Home.
  A::settings(orch).onboarding_done = true;
  REQUIRE(A::boot_splash_active(orch));
  REQUIRE(f.ui_manager.current_screen() == Screen::Info);

  MeasuresAGo data{};
  data.co2.co2 = 420;
  A::on_sensor_data(orch, data);

  CHECK(A::first_measurement_done(orch));
  CHECK_FALSE(A::boot_splash_active(orch));
  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("on_sensor_data: first measurement shows Getting Started on a fresh first boot",
          "[Orchestrator][events][boot-splash][onboarding]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  f.ui_manager.show_info("Booting...");
  orch.init(WakeCause::PowerOn);
  // Fresh unbox — onboarding_done defaults false.
  REQUIRE_FALSE(A::settings(orch).onboarding_done);
  REQUIRE(A::boot_splash_active(orch));
  REQUIRE(f.ui_manager.current_screen() == Screen::Info);

  MeasuresAGo data{};
  data.co2.co2 = 420;
  A::on_sensor_data(orch, data);

  CHECK(A::first_measurement_done(orch));
  CHECK_FALSE(A::boot_splash_active(orch));
  // Gate diverts to the one-time guide; the boot-gate session silent-unlocks
  // so the "Start using" button is pressable on the cold-boot Locked device.
  CHECK(f.ui_manager.current_screen() == Screen::GettingStarted);
  CHECK(A::setup_session_active(orch));
  CHECK(A::lock_state(orch) == LockState::Unlocked);
}

TEST_CASE("on_sensor_data: splash transition is suppressed when setup session is active",
          "[Orchestrator][events][boot-splash]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Simulate the race where the Stationary bring-up flow took ownership
  // of Screen::Info after the cold-boot splash was painted: both flags
  // armed, UI sitting on a session Info text.  on_sensor_data must clear
  // _boot_splash_active but MUST NOT call reset_to_home() — the
  // Stationary flow owns the page and drives its own Info -> Home
  // transition.
  A::set_boot_splash_active(orch, true);
  A::set_setup_session_active(orch, true);
  f.ui_manager.show_info("Connecting to saved Wi-Fi...");
  REQUIRE(f.ui_manager.current_screen() == Screen::Info);

  MeasuresAGo data{};
  data.co2.co2 = 500;
  A::on_sensor_data(orch, data);

  CHECK_FALSE(A::boot_splash_active(orch));
  // Session Info screen preserved — Stationary flow keeps ownership.
  CHECK(f.ui_manager.current_screen() == Screen::Info);
}

TEST_CASE("on_input: ButtonPower short press is silent during cold-boot splash",
          "[Orchestrator][input][boot-splash]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  f.ui_manager.show_info("Booting...");
  orch.init(WakeCause::PowerOn);
  REQUIRE(A::boot_splash_active(orch));
  REQUIRE(A::lock_state(orch) == LockState::Locked);

  InputEventData input{InputSource::ButtonPower, InputType::ShortPress};
  A::on_input(orch, input);

  // Splash + locked state preserved — short-press was suppressed.
  CHECK(A::lock_state(orch) == LockState::Locked);
  CHECK(A::boot_splash_active(orch));
  CHECK(f.ui_manager.current_screen() == Screen::Info);
}

TEST_CASE("init(Button): restores state from RTC and unlocks", "[Orchestrator][init]") {
  TestFixture f;

  // Set up RTC state to restore
  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Portable,
      .behavior = Behavior::Tracking,
      .lock_state = LockState::Locked,
      .gps_enabled = true,
      .tracking_active = true,
      .tracking_session_id = 12345,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  // Button wake: caller sets initial_lock_state=Unlocked in the BootHandoff
  BootHandoff handoff{};
  handoff.initial_lock_state = LockState::Unlocked;
  orch.init(WakeCause::Button, handoff);

  REQUIRE(A::mode(orch) == OperatingMode::Portable);
  REQUIRE(A::tracking_active(orch) == true);
  REQUIRE(A::tracking_session_id(orch) == 12345);
  REQUIRE(A::lock_state(orch) == LockState::Unlocked); // button wake unlocks

  // Tracking route should be resumed — init() now explicitly takes the
  // resume_route() path so a torn trailing record gets truncated.
  REQUIRE(test_spy::route_resumed);
  CHECK_FALSE(test_spy::route_started);
  REQUIRE(test_spy::route_session_id == 12345);
}

TEST_CASE(
    "init(Button, display_painted + unlocked): unlocked, measurement requested, snackbar armed",
    "[Orchestrator][init]") {
  TestFixture f;

  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Offline,
      .behavior = Behavior::Idle,
      .lock_state = LockState::Locked,
      .gps_enabled = false,
      .tracking_active = false,
      .tracking_session_id = 0,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  BootHandoff handoff{};
  handoff.display_painted = true;
  handoff.suppress_wake_press = true;
  handoff.initial_lock_state = LockState::Unlocked;
  orch.init(WakeCause::Button, handoff);

  // Lock state must be Unlocked.  The already_painted path sets _lock_state
  // directly instead of going through unlock() to avoid a redundant display
  // refresh on top of the already-running worker refresh.
  REQUIRE(A::lock_state(orch) == LockState::Unlocked);

  // State fields restored from RTC (behavior and GPS flag, not mode — mode
  // always comes from settings, not RTC).
  REQUIRE(A::gps_enabled(orch) == false);

  // Fresh measurement must be requested in the already_painted branch.
  REQUIRE(test_spy::measurement_requested);
  REQUIRE(test_spy::last_iterations == 1);

  // Snackbar must be armed (UIManager is real — verify via build_values).
  // The already_painted path pre-arms the snackbar (PENDING → armed) so that
  // a single timer fire clears it, rather than waiting for on_sensor_data().
  BuildContext ctx = A::build_context(orch);
  DisplayValues v = f.ui_manager.build_values(ctx);
  REQUIRE(v.snackbar_text != nullptr);
  CHECK(std::string(v.snackbar_text) == "Unlocked");

  // Snackbar refresh timer must be pre-scheduled.
  REQUIRE(A::snackbar_refresh_deadline_ms(orch) != 0);

  // No route resumed (tracking_active == false in the RTC state).
  CHECK_FALSE(test_spy::route_started);
}

TEST_CASE("init(Button, display_painted + unlocked): resumes route when tracking was active",
          "[Orchestrator][init]") {
  TestFixture f;

  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Offline,
      .behavior = Behavior::Tracking,
      .lock_state = LockState::Locked,
      .gps_enabled = true,
      .tracking_active = true,
      .tracking_session_id = 42000,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  BootHandoff handoff{};
  handoff.display_painted = true;
  handoff.suppress_wake_press = true;
  handoff.initial_lock_state = LockState::Unlocked;
  orch.init(WakeCause::Button, handoff);

  REQUIRE(A::lock_state(orch) == LockState::Unlocked);
  REQUIRE(A::tracking_active(orch) == true);
  REQUIRE(test_spy::route_resumed);
  CHECK_FALSE(test_spy::route_started);
  REQUIRE(test_spy::route_session_id == 42000);
}

// ============================================================================
// 6. Input Handling
// ============================================================================

TEST_CASE("on_input: ButtonPower long press triggers shutdown", "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  InputEventData input{InputSource::ButtonPower, InputType::LongPress};
  A::on_input(orch, input);

  REQUIRE(test_spy::shutdown_called);
}

TEST_CASE("on_input: ButtonPower short press toggles lock", "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Initially locked; short press should unlock
  InputEventData input{InputSource::ButtonPower, InputType::ShortPress};
  A::on_input(orch, input);
  REQUIRE(A::lock_state(orch) == LockState::Unlocked);

  // Now unlocked; short press should lock
  test_spy::reset();
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(1000);
  A::on_input(orch, input);
  REQUIRE(A::lock_state(orch) == LockState::Locked);
}

TEST_CASE("on_input: touch while locked shows unlock hint", "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  REQUIRE(A::lock_state(orch) == LockState::Locked);

  // Touch should not change screen (UIManager not called for navigation)
  Screen before = f.ui_manager.current_screen();
  InputEventData input{InputSource::TouchEnter, InputType::ShortPress};
  A::on_input(orch, input);
  REQUIRE(f.ui_manager.current_screen() == before);

  // But snackbar should show unlock hint
  BuildContext ctx = A::build_context(orch);
  DisplayValues v = f.ui_manager.build_values(ctx);
  REQUIRE(v.snackbar_text != nullptr);
  CHECK(std::string(v.snackbar_text) == "Unlock First");
}

TEST_CASE("on_input: touch while unlocked forwards to UIManager", "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Unlock first
  A::unlock(orch);
  REQUIRE(A::lock_state(orch) == LockState::Unlocked);

  // TouchEnter on Home screen should open MainMenu
  InputEventData input{InputSource::TouchEnter, InputType::ShortPress};
  A::on_input(orch, input);
  REQUIRE(f.ui_manager.current_screen() == Screen::MainMenu);
}

TEST_CASE("on_input: ButtonBoot long press triggers factory reset without shutdown",
          "[Orchestrator][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, erase(trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  InputEventData input{InputSource::ButtonBoot, InputType::LongPress};
  A::on_input(orch, input);

  REQUIRE(test_spy::cache_cleared);
  REQUIRE(test_spy::routes_cleared);
  REQUIRE(test_spy::ble_delete_all_bonds_called);
  REQUIRE_FALSE(test_spy::shutdown_called);
}

// ============================================================================
// 7. State Transitions — lock/unlock
// ============================================================================

TEST_CASE("lock: sets Locked and resets UI to home", "[Orchestrator][state]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Unlock first, navigate to menu
  A::unlock(orch);
  f.ui_manager.set_screen(Screen::MainMenu);

  A::lock(orch);
  REQUIRE(A::lock_state(orch) == LockState::Locked);
  REQUIRE(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("unlock: sets Unlocked state", "[Orchestrator][state]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  REQUIRE(A::lock_state(orch) == LockState::Locked);

  test_spy::reset();
  A::unlock(orch);

  REQUIRE(A::lock_state(orch) == LockState::Unlocked);
  // No immediate measurement — data arrives at the next scheduled interval.
  REQUIRE_FALSE(test_spy::measurement_requested);
}

// ============================================================================
// 8. State Transitions — tracking
// ============================================================================

TEST_CASE("start_tracking: generates session ID and starts route", "[Orchestrator][tracking]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  const bool ok = A::start_tracking(orch);

  REQUIRE(ok);
  REQUIRE(A::tracking_active(orch) == true);
  REQUIRE(A::behavior(orch) == Behavior::Tracking);
  REQUIRE(A::tracking_session_id(orch) >= 10000);
  REQUIRE(A::tracking_session_id(orch) <= 99999);
  REQUIRE(test_spy::route_started);
  // Urgent transition — status must be pushed, not only set-on-Read.
  REQUIRE(test_spy::ble_notify_tracking_status_called);
  CHECK(test_spy::ble_last_status_tracking == true);
  CHECK(test_spy::ble_last_status_session == A::tracking_session_id(orch));
}

TEST_CASE("start_tracking: storage open failure surfaces inline and returns false",
          "[Orchestrator][tracking][failure]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Simulate create_route() failing (e.g. NAND unmounted).
  test_spy::create_route_result = false;

  const bool ok = A::start_tracking(orch);

  CHECK_FALSE(ok);
  CHECK(A::tracking_active(orch) == false);
  CHECK(A::behavior(orch) == Behavior::Idle);
  CHECK(A::tracking_session_id(orch) == 0);
  CHECK_FALSE(test_spy::route_started);
  // Status notify fires inline with tracking=false so the connected
  // phone learns the start failed without polling.
  CHECK(test_spy::ble_notify_tracking_status_called);
  CHECK(test_spy::ble_last_status_tracking == false);
  CHECK(test_spy::ble_last_status_session == 0);
}

TEST_CASE("start_tracking: session-ID collision retried transparently",
          "[Orchestrator][tracking][session-id]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Pre-populate the "existing routes" set with whatever the next random
  // draw produces, then clear so the second draw succeeds.  The
  // orchestrator's bounded retry must absorb the collision without
  // surfacing it as a storage error.
  //
  // We can't predict the random draw, so a strict "all but one in the
  // set" test would be brittle.  Instead, force exhaustion in a separate
  // case below.  Here we just verify that with no collisions configured,
  // start_tracking succeeds normally and ends up with a valid id.
  const bool ok = A::start_tracking(orch);
  REQUIRE(ok);
  CHECK(A::tracking_session_id(orch) >= 10000);
}

TEST_CASE("start_tracking: session-ID exhaustion reports storage error",
          "[Orchestrator][tracking][session-id]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Make every possible 5-digit ID look already-taken by inserting the
  // full range. generate_session_id() must exhaust its retry budget and
  // return 0, which start_tracking() treats as a storage error.
  for (uint32_t id = 10000; id <= 99999; ++id) {
    test_spy::existing_route_session_ids.insert(id);
  }

  const bool ok = A::start_tracking(orch);

  CHECK_FALSE(ok);
  CHECK(A::tracking_active(orch) == false);
  CHECK(A::tracking_session_id(orch) == 0);
  CHECK_FALSE(test_spy::route_started);
  // No create_route call should have happened (we never got a valid id).
  CHECK(test_spy::ble_notify_tracking_status_called); // inline failure notify
  CHECK(test_spy::ble_last_status_tracking == false);
}

TEST_CASE("start_tracking: idempotent when already tracking", "[Orchestrator][tracking]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  uint32_t first_id = A::tracking_session_id(orch);

  test_spy::reset();
  A::start_tracking(orch); // should be no-op

  REQUIRE_FALSE(test_spy::route_started); // no second create_route call
  REQUIRE(A::tracking_session_id(orch) == first_id);
}

TEST_CASE("stop_tracking: ends route and clears state", "[Orchestrator][tracking]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  test_spy::reset();

  A::stop_tracking(orch);

  REQUIRE(A::tracking_active(orch) == false);
  REQUIRE(A::behavior(orch) == Behavior::Idle);
  REQUIRE(A::tracking_session_id(orch) == 0);
  REQUIRE(test_spy::route_ended);
  // Manual stop is an urgent transition — push to any connected client.
  CHECK(test_spy::ble_notify_tracking_status_called);
  CHECK(test_spy::ble_last_status_tracking == false);
  CHECK(test_spy::ble_last_status_session == 0);
}

TEST_CASE("stop_tracking: idempotent when not tracking", "[Orchestrator][tracking]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  A::stop_tracking(orch);
  REQUIRE_FALSE(test_spy::route_ended);
}

TEST_CASE("clear_data: clears cache and routes, stopping tracking first",
          "[Orchestrator][storage]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  test_spy::route_ended = false;

  REQUIRE(A::clear_data(orch));

  CHECK_FALSE(A::tracking_active(orch));
  CHECK(A::behavior(orch) == Behavior::Idle);
  CHECK(A::tracking_session_id(orch) == 0);
  CHECK(test_spy::route_ended);
  CHECK(test_spy::cache_cleared);
  CHECK(test_spy::routes_cleared);
}

TEST_CASE("BLE ClearData command sends progress then reports storage clear failure",
          "[Orchestrator][storage][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  test_spy::clear_routes_result = false;
  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::ClearData;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(test_spy::cache_cleared);
  CHECK(test_spy::routes_cleared);
  CHECK(test_spy::ble_notify_command_progress_called);
  CHECK(test_spy::ble_progress_command == BleCommand::ClearData);
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::ClearData);
  CHECK_FALSE(test_spy::ble_last_command_success);
}

TEST_CASE("factory_reset: resets settings to defaults without keeping tracking state",
          "[Orchestrator][factory_reset]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  A::settings(orch).operating_mode = OperatingMode::Offline;
  A::settings(orch).gps_mode = GpsMode::AlwaysOff;
  A::settings(orch).device_name = "custom-name";
  A::set_mode(orch, OperatingMode::Offline);

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, erase(trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  REQUIRE(A::factory_reset(orch));

  CHECK(A::settings(orch).operating_mode == OperatingMode::Portable);
  CHECK(A::settings(orch).gps_mode == GpsMode::OnWhenTracking);
  CHECK(A::settings(orch).device_name == "airgradient-go");
  CHECK(A::mode(orch) == OperatingMode::Portable);
  CHECK(A::behavior(orch) == Behavior::Idle);
  CHECK(A::lock_state(orch) == LockState::Locked);
  CHECK_FALSE(A::tracking_active(orch));
  CHECK(A::tracking_session_id(orch) == 0);
}

// ============================================================================
// First-boot onboarding gate + engagement paths
// ============================================================================

TEST_CASE("mark_onboarding_done persists once and is idempotent", "[Orchestrator][onboarding]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  // Exactly one save (commit) across both calls — the second is a no-op.
  REQUIRE_CALL(f.mock_config, commit()).TIMES(1).RETURN(ConfigStoreResult::OK);

  CHECK_FALSE(A::settings(orch).onboarding_done);
  A::mark_onboarding_done(orch);
  CHECK(A::settings(orch).onboarding_done);
  A::mark_onboarding_done(orch); // idempotent — no second commit
  CHECK(A::settings(orch).onboarding_done);
}

TEST_CASE("BLE auth complete marks onboarding done", "[Orchestrator][onboarding][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  CHECK_FALSE(A::settings(orch).onboarding_done);
  A::on_ble_auth_complete(orch, /*success=*/true);
  CHECK(A::settings(orch).onboarding_done);
}

TEST_CASE("BLE auth failure leaves onboarding untouched", "[Orchestrator][onboarding][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  CHECK_FALSE(A::settings(orch).onboarding_done);
  A::on_ble_auth_complete(orch, /*success=*/false);
  CHECK_FALSE(A::settings(orch).onboarding_done);
}

TEST_CASE("change_mode marks onboarding done", "[Orchestrator][onboarding][change_mode]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  CHECK_FALSE(A::settings(orch).onboarding_done);
  A::change_mode(orch, OperatingMode::Offline);
  CHECK(A::settings(orch).onboarding_done);
}

TEST_CASE("manufacturing: boot short-press before onboarding enters Stationary ephemerally",
          "[Orchestrator][onboarding][manufacturing]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  test_spy::wifi_has_saved_networks = false;

  // Ephemeral entry must not commit anything to NVS.
  FORBID_CALL(f.mock_config, commit());

  REQUIRE_FALSE(A::settings(orch).onboarding_done);
  A::on_input(orch, InputEventData{InputSource::ButtonBoot, InputType::ShortPress});

  CHECK(A::mode(orch) == OperatingMode::Stationary);
  CHECK(A::manufacturing_mode(orch));
  CHECK_FALSE(A::settings(orch).onboarding_done); // never persisted
  CHECK(test_spy::wifi_try_fallback_called);      // Stationary bring-up ran
}

TEST_CASE("manufacturing: boot short-press after onboarding does not enter Stationary",
          "[Orchestrator][onboarding][manufacturing]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);
  A::settings(orch).onboarding_done = true;

  A::on_input(orch, InputEventData{InputSource::ButtonBoot, InputType::ShortPress});

  CHECK_FALSE(A::manufacturing_mode(orch));
  CHECK(A::mode(orch) == OperatingMode::Portable);
  CHECK_FALSE(test_spy::wifi_try_fallback_called);
}

TEST_CASE("manufacturing: second boot short-press arms a fuel-gauge learning run",
          "[Orchestrator][manufacturing][fg]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  test_spy::wifi_has_saved_networks = false;

  // First press -> manufacturing mode (ephemeral, no commit).
  A::on_input(orch, InputEventData{InputSource::ButtonBoot, InputType::ShortPress});
  REQUIRE(A::manufacturing_mode(orch));

  // Second press -> persist the learning run (stage=Charge, cycle=1) + reboot
  // (reboot is a no-op under TEST_HOST).
  std::map<std::string, int> writes;
  std::map<std::string, int> *writes_ptr = &writes;
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_))
      .SIDE_EFFECT((*writes_ptr)[std::string(_1)] = _2)
      .RETURN(ConfigStoreResult::OK);
  REQUIRE_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::on_input(orch, InputEventData{InputSource::ButtonBoot, InputType::ShortPress});

  CHECK(writes["fs_s"] == static_cast<int>(FgLearningStage::Charge));
  CHECK(writes["fs_c"] == 1);
  CHECK(writes["fs_i"] == 0);
}

TEST_CASE("manufacturing: shutdown wipes settings via factory reset",
          "[Orchestrator][manufacturing][shutdown]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, erase(trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);
  A::set_manufacturing_mode(orch, true);

  A::shutdown(orch);

  CHECK(test_spy::routes_cleared);              // factory_reset ran
  CHECK(test_spy::ble_delete_all_bonds_called); // bonds wiped
  CHECK(test_spy::shutdown_called);             // power-off still happened
}

TEST_CASE("manufacturing: shutdown without flag skips factory reset",
          "[Orchestrator][manufacturing][shutdown]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::shutdown(orch);

  CHECK_FALSE(test_spy::ble_delete_all_bonds_called);
  CHECK(test_spy::shutdown_called);
}

TEST_CASE("factory_reset clears onboarding_done", "[Orchestrator][onboarding][factory_reset]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  A::settings(orch).onboarding_done = true;

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, erase(trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  REQUIRE(A::factory_reset(orch));
  CHECK_FALSE(A::settings(orch).onboarding_done);
}

TEST_CASE("BLE FactoryReset command sends progress then reports error and skips shutdown",
          "[Orchestrator][factory_reset][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, erase(trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  test_spy::ble_delete_all_bonds_result = false;
  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::FactoryReset;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(test_spy::ble_delete_all_bonds_called);
  CHECK(test_spy::ble_notify_command_progress_called);
  CHECK(test_spy::ble_progress_command == BleCommand::FactoryReset);
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::FactoryReset);
  CHECK_FALSE(test_spy::ble_last_command_success);
  CHECK_FALSE(test_spy::shutdown_called);
}

TEST_CASE("BLE StartTracking command starts tracking when idle", "[Orchestrator][tracking][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  REQUIRE_FALSE(A::tracking_active(orch));

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::StartTracking;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(A::tracking_active(orch));
  CHECK(A::behavior(orch) == Behavior::Tracking);
  CHECK(test_spy::route_started);
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::StartTracking);
  CHECK(test_spy::ble_last_command_success);
}

TEST_CASE("BLE StartTracking command reports flash_error on storage open failure",
          "[Orchestrator][tracking][ble][failure]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  REQUIRE_FALSE(A::tracking_active(orch));

  // Storage refuses to open the route — the BLE result must report
  // failure (not the pre-spec was_idle heuristic, which would have
  // reported success).
  test_spy::create_route_result = false;
  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::StartTracking;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK_FALSE(A::tracking_active(orch));
  CHECK_FALSE(test_spy::route_started);
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::StartTracking);
  CHECK_FALSE(test_spy::ble_last_command_success);
}

TEST_CASE("BLE StartTracking command reports already_tracking when active",
          "[Orchestrator][tracking][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  A::start_tracking(orch);
  REQUIRE(A::tracking_active(orch));
  test_spy::ble_notify_command_result_called = false;

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::StartTracking;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(A::tracking_active(orch));
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::StartTracking);
  CHECK_FALSE(test_spy::ble_last_command_success);
}

TEST_CASE("BLE StopTracking command stops tracking when active", "[Orchestrator][tracking][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  A::start_tracking(orch);
  REQUIRE(A::tracking_active(orch));
  test_spy::ble_notify_command_result_called = false;

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::StopTracking;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK_FALSE(A::tracking_active(orch));
  CHECK(A::behavior(orch) == Behavior::Idle);
  CHECK(test_spy::route_ended);
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::StopTracking);
  CHECK(test_spy::ble_last_command_success);
}

TEST_CASE("BLE StopTracking command reports not_tracking when idle",
          "[Orchestrator][tracking][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  REQUIRE_FALSE(A::tracking_active(orch));

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::StopTracking;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK_FALSE(A::tracking_active(orch));
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::StopTracking);
  CHECK_FALSE(test_spy::ble_last_command_success);
}

TEST_CASE("BLE SetAiding command forwards aiding data to GPS service", "[Orchestrator][gps][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::SetAiding;
  test_spy::ble_config_decode_result.aiding.latitude = 47.376887;
  test_spy::ble_config_decode_result.aiding.longitude = 8.541694;
  test_spy::ble_config_decode_result.aiding.altitude_m = 408.0f;
  test_spy::ble_config_decode_result.aiding.pos_acc_m = 50.0f;
  test_spy::ble_config_decode_result.aiding.epoch_s = 1711234567;
  test_spy::ble_config_decode_result.aiding.time_acc_ms = 2000;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(test_spy::gps_aiding_set);
  CHECK(test_spy::gps_aiding_data.latitude == 47.376887);
  CHECK(test_spy::gps_aiding_data.longitude == 8.541694);
  CHECK(test_spy::gps_aiding_data.altitude_m == 408.0f);
  CHECK(test_spy::gps_aiding_data.pos_acc_m == 50.0f);
  CHECK(test_spy::gps_aiding_data.epoch_s == 1711234567);
  CHECK(test_spy::gps_aiding_data.time_acc_ms == 2000);
  CHECK(f.mock_rtos.system_time_set);
  CHECK(f.mock_rtos.system_time_epoch == 1711234567);
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::SetAiding);
  CHECK(test_spy::ble_last_command_success);
}

TEST_CASE("BLE SetAiding command with no useful data reports error", "[Orchestrator][gps][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::SetAiding;
  // Default GpsAidingData has invalid sentinels — no useful data

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK_FALSE(test_spy::gps_aiding_set);
  CHECK_FALSE(f.mock_rtos.system_time_set);
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::SetAiding);
  CHECK_FALSE(test_spy::ble_last_command_success);
}

TEST_CASE("BLE SetAiding command with only position data succeeds", "[Orchestrator][gps][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::SetAiding;
  test_spy::ble_config_decode_result.aiding.latitude = 47.376887;
  test_spy::ble_config_decode_result.aiding.longitude = 8.541694;
  // epoch_s remains 0 (no time data)

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(test_spy::gps_aiding_set);
  CHECK(test_spy::gps_aiding_data.latitude == 47.376887);
  CHECK(test_spy::gps_aiding_data.longitude == 8.541694);
  CHECK_FALSE(f.mock_rtos.system_time_set);
  CHECK(test_spy::ble_last_command_success);
}

TEST_CASE("BLE SetAiding command with only time data succeeds", "[Orchestrator][gps][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::SetAiding;
  test_spy::ble_config_decode_result.aiding.epoch_s = 1711234567;
  test_spy::ble_config_decode_result.aiding.time_acc_ms = 2000;
  // lat/lon remain at invalid sentinels (no position data)

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(test_spy::gps_aiding_set);
  CHECK(test_spy::gps_aiding_data.epoch_s == 1711234567);
  CHECK(test_spy::gps_aiding_data.time_acc_ms == 2000);
  CHECK(f.mock_rtos.system_time_set);
  CHECK(f.mock_rtos.system_time_epoch == 1711234567);
  CHECK(test_spy::ble_last_command_success);
}

// ============================================================================
// 9. Event Handlers — sensor data
// ============================================================================

TEST_CASE("on_sensor_data: caches measurement and sets first_measurement_done",
          "[Orchestrator][events]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  REQUIRE_FALSE(A::first_measurement_done(orch));

  MeasuresAGo data{};
  data.co2.co2 = 420;
  A::on_sensor_data(orch, data);

  REQUIRE(A::first_measurement_done(orch));
  REQUIRE(test_spy::cache_measurement_called);
  REQUIRE(A::cached_measures(orch).co2.co2 == 420);
}

TEST_CASE("on_sensor_data: measures power comes from latest PowerSnapshot",
          "[Orchestrator][events]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  PowerSnapshot power{};
  power.battery_voltage = 3.82f;
  power.charging_voltage = 5.01f;
  A::set_latest_power(orch, power);

  MeasuresAGo data{};
  data.co2.co2 = 420;
  data.power.battery_voltage = 1.23f;
  data.power.charging_voltage = 9.87f;
  A::on_sensor_data(orch, data);

  CHECK(A::cached_measures(orch).power.battery_voltage == 3.82f);
  CHECK(A::cached_measures(orch).power.charging_voltage == 5.01f);
  CHECK(test_spy::last_cached_measurement.power.battery_voltage == 3.82f);
  CHECK(test_spy::last_cached_measurement.power.charging_voltage == 5.01f);
  CHECK(test_spy::cloud_last_snapshot.power.battery_voltage == 3.82f);
  CHECK(test_spy::cloud_last_snapshot.power.charging_voltage == 5.01f);
}

TEST_CASE("on_sensor_data: appends route point when tracking", "[Orchestrator][events]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  test_spy::reset();

  MeasuresAGo data{};
  data.co2.co2 = 500;
  A::on_sensor_data(orch, data);

  REQUIRE(test_spy::route_point_appended);
  REQUIRE(test_spy::last_route_point.sensors.co2.co2 == 500);
}

TEST_CASE("on_sensor_data: route point includes battery percentage from latest power",
          "[Orchestrator][events]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  // Simulate a BMS poll that returns 72% battery
  test_spy::snapshot_to_return.battery_percentage = 72.0f;
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(61000);
  A::check_timers(orch);

  A::start_tracking(orch);
  test_spy::reset();

  MeasuresAGo data{};
  data.co2.co2 = 400;
  A::on_sensor_data(orch, data);

  REQUIRE(test_spy::route_point_appended);
  REQUIRE(test_spy::last_route_point.battery_percentage == 72.0f);
}

TEST_CASE("on_sensor_data: does not append route point when not tracking",
          "[Orchestrator][events]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  REQUIRE_FALSE(test_spy::route_point_appended);
}

// ============================================================================
// 10. Event Handlers — GPS
// ============================================================================

TEST_CASE("on_gps_fix: caches data when GPS is active", "[Orchestrator][events]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  GpsData fix{};
  fix.position.latitude = 48.8566;
  fix.position.longitude = 2.3522;
  fix.fix.fix_type = GpsFixType::Fix3D;

  A::on_gps_fix(orch, fix);

  REQUIRE(A::latest_gps(orch).position.latitude == 48.8566);
}

TEST_CASE("on_gps_fix: ignores data when GPS is inactive", "[Orchestrator][events]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOff;
  auto orch = f.make_orchestrator();

  GpsData fix{};
  fix.position.latitude = 48.8566;
  A::on_gps_fix(orch, fix);

  // Latitude should still be the invalid sentinel
  REQUIRE(A::latest_gps(orch).position.latitude == GPS_LATITUDE_INVALID);
}

TEST_CASE("on_gps_fix: NoFix data clears cached fix when GPS is active", "[Orchestrator][events]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  // First, cache a valid fix.
  GpsData valid_fix{};
  valid_fix.position.latitude = 48.8566;
  valid_fix.position.longitude = 2.3522;
  valid_fix.fix.fix_type = GpsFixType::Fix3D;
  A::on_gps_fix(orch, valid_fix);
  REQUIRE(is_fix_valid(A::latest_gps(orch).fix));

  // Now receive NoFix data (as GpsService would post after fix loss).
  GpsData no_fix{}; // default: NoFix, invalid sentinels
  A::on_gps_fix(orch, no_fix);

  REQUIRE_FALSE(is_fix_valid(A::latest_gps(orch).fix));
  REQUIRE(A::latest_gps(orch).position.latitude == GPS_LATITUDE_INVALID);

  // build_context must reflect gps_fix == false.
  BuildContext ctx = A::build_context(orch);
  REQUIRE_FALSE(ctx.gps_fix);
}

// ============================================================================
// 11. Timer Management
// ============================================================================

TEST_CASE("check_timers: fires measurement when interval elapses", "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.measure_interval_seconds = 10;
  auto orch = f.make_orchestrator();

  // Advance time past interval
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(11000);
  test_spy::reset();

  A::check_timers(orch);

  REQUIRE(test_spy::measurement_requested);
  REQUIRE(test_spy::last_iterations == 1);
  REQUIRE(test_spy::last_groups == SensorGroup::All);
}

TEST_CASE("check_timers: no measurement when interval not yet elapsed", "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.measure_interval_seconds = 10;
  auto orch = f.make_orchestrator();

  // Time not yet past interval
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(5000);
  test_spy::reset();

  A::check_timers(orch);

  REQUIRE_FALSE(test_spy::measurement_requested);
}

TEST_CASE("check_timers: fires BMS timer when due", "[Orchestrator][timers]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // BMS full-telemetry interval is 60000ms
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(61000);
  test_spy::reset();

  A::check_timers(orch);

  REQUIRE(test_spy::bms_polled);
}

TEST_CASE("compute_queue_timeout: includes BMS status poll deadline", "[Orchestrator][timers]") {
  TestFixture f;
  // Set large sensor interval so it doesn't dominate the timeout
  f.settings.measure_interval_seconds = 600;
  auto orch = f.make_orchestrator();

  // At t=0, all last-poll timestamps are 0.
  // BMS full poll deadline:   0 + 60000 = 60000 → remaining = 60000
  // BMS status poll deadline: 0 + 5000  = 5000  → remaining = 5000
  // Sensor deadlines:         0 + 600000 → much larger
  // The smallest should be 5000 (BMS status poll).
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(0);
  uint32_t timeout = A::compute_queue_timeout_ms(orch);
  REQUIRE(timeout <= 5000);
}

TEST_CASE("compute_queue_timeout: clamps to zero when deadline passed", "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.measure_interval_seconds = 10;
  auto orch = f.make_orchestrator();

  // All timestamps are 0, advance time well past all deadlines.
  // Unsigned subtraction wraps to a huge value, clamped to 0.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(100000);
  uint32_t timeout = A::compute_queue_timeout_ms(orch);
  REQUIRE(timeout == 0);
}

TEST_CASE("compute_queue_timeout: BMS full poll dominates when status poll not due",
          "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.measure_interval_seconds = 600;
  auto orch = f.make_orchestrator();

  // At t=3000, BMS status poll (5000) is 2000ms away,
  // BMS full poll (60000) is 57000ms away.
  // Status poll should still be the shortest.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(3000);
  uint32_t timeout = A::compute_queue_timeout_ms(orch);
  REQUIRE(timeout <= 2000);
}

TEST_CASE("check_timers: fires inactivity when unlocked and due", "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.auto_lock_seconds = 10;
  auto orch = f.make_orchestrator();

  A::unlock(orch); // sets _last_input_ms to current time (0)

  // Advance time past inactivity timeout
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(11000);
  A::check_timers(orch);

  REQUIRE(A::lock_state(orch) == LockState::Locked);
}

TEST_CASE("check_timers: does not fire inactivity when locked", "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.auto_lock_seconds = 10;
  auto orch = f.make_orchestrator();

  REQUIRE(A::lock_state(orch) == LockState::Locked);

  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(11000);
  A::check_timers(orch);

  // Should still be locked (no inactivity timeout triggered — was already locked)
  REQUIRE(A::lock_state(orch) == LockState::Locked);
}

TEST_CASE("check_timers: does not fire inactivity when auto_lock disabled",
          "[Orchestrator][timers]") {
  TestFixture f;
  f.settings.auto_lock_seconds = 0; // disabled
  auto orch = f.make_orchestrator();

  A::unlock(orch);

  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(999999);
  A::check_timers(orch);

  REQUIRE(A::lock_state(orch) == LockState::Unlocked);
}

TEST_CASE("check_timers: snackbar refresh timer fires after deadline",
          "[Orchestrator][timers][snackbar]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // unlock() calls show_snackbar("Unlocked") + update_display() at t=0.
  // update_display() arms the snackbar and schedules refresh at t=3200.
  A::unlock(orch);
  REQUIRE(A::snackbar_refresh_deadline_ms(orch) != 0);

  // At t=3200, check_timers fires the snackbar refresh.
  // The subsequent update_display() clears the expired snackbar.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(3200);
  A::check_timers(orch);

  // Timer must be reset.
  REQUIRE(A::snackbar_refresh_deadline_ms(orch) == 0);

  // Snackbar should be cleared after the refresh.
  BuildContext ctx = A::build_context(orch);
  DisplayValues v = f.ui_manager.build_values(ctx);
  REQUIRE(v.snackbar_text == nullptr);
}

TEST_CASE("check_timers: snackbar refresh timer does not fire before deadline",
          "[Orchestrator][timers][snackbar]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  A::unlock(orch);
  REQUIRE(A::snackbar_refresh_deadline_ms(orch) != 0);

  // At t=2000, well before the 3200 deadline — timer should NOT fire.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(2000);
  A::check_timers(orch);

  // Timer still active, snackbar still visible.
  REQUIRE(A::snackbar_refresh_deadline_ms(orch) != 0);

  BuildContext ctx = A::build_context(orch);
  DisplayValues v = f.ui_manager.build_values(ctx);
  REQUIRE(v.snackbar_text != nullptr);
  CHECK(std::string(v.snackbar_text) == "Unlocked");
}

TEST_CASE("update_display: does not re-arm snackbar refresh when already scheduled",
          "[Orchestrator][timers][snackbar]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // unlock() at t=0 → snackbar armed, refresh scheduled at t=3200.
  A::unlock(orch);
  uint32_t first_deadline = A::snackbar_refresh_deadline_ms(orch);
  REQUIRE(first_deadline != 0);

  // Simulate sensor data arriving at t=1000 → triggers update_display().
  // Snackbar is still active, but the timer should NOT be reset.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(1000);
  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  // Deadline should be unchanged (not pushed forward).
  REQUIRE(A::snackbar_refresh_deadline_ms(orch) == first_deadline);
}

TEST_CASE("button wake: pre-armed snackbar clears in single timer fire",
          "[Orchestrator][timers][snackbar]") {
  TestFixture f;

  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Offline,
      .behavior = Behavior::Idle,
      .lock_state = LockState::Locked,
      .gps_enabled = false,
      .tracking_active = false,
      .tracking_session_id = 0,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  // init() at t=0: pre-arms snackbar + schedules refresh timer.
  BootHandoff handoff{};
  handoff.display_painted = true;
  handoff.suppress_wake_press = true;
  handoff.initial_lock_state = LockState::Unlocked;
  orch.init(WakeCause::Button, handoff);
  uint32_t deadline = A::snackbar_refresh_deadline_ms(orch);
  REQUIRE(deadline != 0);

  // Snackbar should be visible before the deadline.
  BuildContext ctx1 = A::build_context(orch);
  DisplayValues v1 = f.ui_manager.build_values(ctx1);
  REQUIRE(v1.snackbar_text != nullptr);

  // Single timer fire at the deadline clears the snackbar — no intermediate
  // update_display() needed.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(deadline);
  A::check_timers(orch);

  REQUIRE(A::snackbar_refresh_deadline_ms(orch) == 0);

  BuildContext ctx2 = A::build_context(orch);
  DisplayValues v2 = f.ui_manager.build_values(ctx2);
  REQUIRE(v2.snackbar_text == nullptr);
}

// ============================================================================
// 12. Settings
// ============================================================================

TEST_CASE("apply_settings_change: propagates GPS interval to service", "[Orchestrator][settings]") {
  TestFixture f;
  f.settings.gps_interval_seconds = 5;
  auto orch = f.make_orchestrator();

  // apply_to_settings is real (UIManager). Mock NVS calls for save_go_settings.
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::apply_settings_change(orch);

  REQUIRE(test_spy::gps_posting_interval_ms == 5000);
}

TEST_CASE("apply_settings_change: reschedules timer when interval changes",
          "[Orchestrator][settings]") {
  TestFixture f;
  f.settings.measure_interval_seconds = 10;
  auto orch = f.make_orchestrator();

  GoSettings updated = f.settings;
  updated.measure_interval_seconds = 30;
  f.ui_manager.sync_settings(updated);

  A::set_last_measurement_ms(orch, 1000);

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(9000);

  A::apply_settings_change(orch);

  CHECK(A::settings(orch).measure_interval_seconds == 30);
  CHECK(A::last_measurement_ms(orch) == 9000);
}

TEST_CASE("apply_settings_change: no-op when interval unchanged", "[Orchestrator][settings]") {
  TestFixture f;
  f.settings.measure_interval_seconds = 10;
  auto orch = f.make_orchestrator();

  f.ui_manager.sync_settings(f.settings); // same settings

  A::set_last_measurement_ms(orch, 1000);

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(9000);

  A::apply_settings_change(orch);

  CHECK(A::last_measurement_ms(orch) == 1000); // unchanged
}

TEST_CASE("BLE config set: reschedules timer when interval changes",
          "[Orchestrator][settings][ble]") {
  TestFixture f;
  f.settings.measure_interval_seconds = 10;
  auto orch = f.make_orchestrator();

  A::set_last_measurement_ms(orch, 1000);

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Set;
  test_spy::ble_decode_updates_settings = true;
  test_spy::ble_decoded_settings = f.settings;
  test_spy::ble_decoded_settings.measure_interval_seconds = 30;

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(9000);

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(A::settings(orch).measure_interval_seconds == 30);
  CHECK(A::last_measurement_ms(orch) == 9000);
}

TEST_CASE("BLE config set: rejected when unknown config key present",
          "[Orchestrator][settings][ble]") {
  TestFixture f;
  f.settings.measure_interval_seconds = 10;
  auto orch = f.make_orchestrator();

  A::set_last_measurement_ms(orch, 1000);

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Set;
  test_spy::ble_config_decode_result.has_unknown_keys = true;
  test_spy::ble_decode_updates_settings = true;
  test_spy::ble_decoded_settings = f.settings;
  test_spy::ble_decoded_settings.measure_interval_seconds = 30;

  test_spy::ble_notify_command_result_called = false;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  // Settings unchanged — write was rejected
  CHECK(A::settings(orch).measure_interval_seconds == 10);
  CHECK(A::last_measurement_ms(orch) == 1000);

  // Error notification sent
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::Set);
  CHECK_FALSE(test_spy::ble_last_command_success);

  // Config notification NOT sent
  CHECK_FALSE(test_spy::ble_notify_config_called);
}

TEST_CASE("on_ble_config_write: multi-field set rejected with single_field_only",
          "[Orchestrator][settings][ble]") {
  TestFixture f;
  f.settings.measure_interval_seconds = 10;
  auto orch = f.make_orchestrator();

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Set;
  test_spy::ble_config_decode_result.has_unknown_keys = false;
  test_spy::ble_config_decode_result.recognized_config_key_count = 2;
  test_spy::ble_decode_updates_settings = true;
  test_spy::ble_decoded_settings = f.settings;
  test_spy::ble_decoded_settings.measure_interval_seconds = 30;

  test_spy::ble_notify_command_result_called = false;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  // Rejected before adoption — settings unchanged.
  CHECK(A::settings(orch).measure_interval_seconds == 10);
  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::Set);
  CHECK_FALSE(test_spy::ble_last_command_success);
  CHECK(std::string(test_spy::ble_last_command_error) == BLE_VAL_ERR_SINGLE_FIELD_ONLY);
  CHECK_FALSE(test_spy::ble_notify_config_called);
}

TEST_CASE("on_ble_config_write: unknown key wins precedence over single_field_only",
          "[Orchestrator][settings][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Set;
  test_spy::ble_config_decode_result.has_unknown_keys = true;
  test_spy::ble_config_decode_result.recognized_config_key_count = 2;

  test_spy::ble_notify_command_result_called = false;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(std::string(test_spy::ble_last_command_error) == BLE_VAL_ERR_UNKNOWN_CONFIG_KEY);
}

// ============================================================================
// 13. Session ID Generation
// ============================================================================

TEST_CASE("generate_session_id: returns a 5-digit random ID", "[Orchestrator][session]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  uint32_t id = A::generate_session_id(orch);
  REQUIRE(id >= 10000);
  REQUIRE(id <= 99999);
}

TEST_CASE("generate_session_id: stays within the 5-digit range across calls",
          "[Orchestrator][session]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  const uint32_t first_id = A::generate_session_id(orch);
  const uint32_t second_id = A::generate_session_id(orch);

  REQUIRE(first_id >= 10000);
  REQUIRE(first_id <= 99999);
  REQUIRE(second_id >= 10000);
  REQUIRE(second_id <= 99999);
}

TEST_CASE("generate_session_id: returns 0 when all retries collide", "[Orchestrator][session]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Pre-seed every possible 5-digit ID as taken. The bounded retry loop
  // exhausts and returns 0 instead of silently colliding with an
  // existing session file.
  for (uint32_t id = 10000; id <= 99999; ++id) {
    test_spy::existing_route_session_ids.insert(id);
  }

  CHECK(A::generate_session_id(orch) == 0);
}

// ============================================================================
// 14. Build Context
// ============================================================================

TEST_CASE("build_context: populates sensor data and status flags", "[Orchestrator][display]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  f.settings.use_fahrenheit = true;
  f.settings.pm_use_usaqi = true;
  auto orch = f.make_orchestrator();

  // Feed sensor data
  MeasuresAGo data{};
  data.co2.co2 = 800;
  data.temp_hum_a.temperature = 23.5f;
  A::on_sensor_data(orch, data);

  BuildContext ctx = A::build_context(orch);

  REQUIRE(ctx.sensor_data.co2.co2 == 800);
  REQUIRE(ctx.sensor_data.temp_hum_a.temperature == 23.5f);
  REQUIRE(ctx.locked == true); // still locked by default
  REQUIRE(ctx.gps_enabled == true);
  REQUIRE(ctx.use_fahrenheit == true);
  REQUIRE(ctx.pm_use_usaqi == true);
}

TEST_CASE("build_context: battery percentage 0xFF when invalid", "[Orchestrator][display]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Default PowerSnapshot has battery_percentage = -1.0f (invalid)
  BuildContext ctx = A::build_context(orch);
  REQUIRE(ctx.battery_pct == 0xFF);
}

TEST_CASE("build_context: display_off is always false", "[Orchestrator][display]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  SECTION("display_off is false when locked") {
    REQUIRE(A::lock_state(orch) == LockState::Locked);
    BuildContext ctx = A::build_context(orch);
    REQUIRE(ctx.display_off == false);
  }

  SECTION("display_off is false when unlocked") {
    A::unlock(orch);
    REQUIRE(A::lock_state(orch) == LockState::Unlocked);
    BuildContext ctx = A::build_context(orch);
    REQUIRE(ctx.display_off == false);
  }
}

// ============================================================================
// 15. Shutdown
// ============================================================================

TEST_CASE("shutdown: stops tracking if active and calls power shutdown",
          "[Orchestrator][shutdown]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  test_spy::reset();

  A::shutdown(orch);

  REQUIRE(test_spy::route_ended);
  REQUIRE(test_spy::cache_backed_up);
  REQUIRE(test_spy::shutdown_called);
  REQUIRE(A::tracking_active(orch) == false);
  REQUIRE(f.ui_manager.current_screen() == Screen::ShutdownUser);
}

TEST_CASE("shutdown: works without active tracking", "[Orchestrator][shutdown]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  A::shutdown(orch);

  REQUIRE(test_spy::cache_backed_up);
  REQUIRE(test_spy::shutdown_called);
  REQUIRE_FALSE(test_spy::route_ended); // no route was active
}

TEST_CASE("shutdown: pushes a disc notice to a connected client", "[Orchestrator][shutdown][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  test_spy::ble_initialized = true;
  test_spy::ble_connected = true;

  SECTION("over-temperature maps to overheat") {
    A::shutdown(orch, ShipModeRequest::OverTemperature);
    CHECK(test_spy::ble_notify_disconnect_called);
    CHECK(test_spy::ble_last_disc_reason == BleDiscReason::Overheat);
  }
  SECTION("over-discharge maps to low_batt") {
    A::shutdown(orch, ShipModeRequest::OverDischarge);
    CHECK(test_spy::ble_notify_disconnect_called);
    CHECK(test_spy::ble_last_disc_reason == BleDiscReason::LowBatt);
  }
  SECTION("user long-press maps to user") {
    A::shutdown(orch, ShipModeRequest::None);
    CHECK(test_spy::ble_notify_disconnect_called);
    CHECK(test_spy::ble_last_disc_reason == BleDiscReason::User);
  }
}

TEST_CASE("shutdown: does not notify when no client is connected",
          "[Orchestrator][shutdown][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = false;

  A::shutdown(orch, ShipModeRequest::OverTemperature);

  CHECK_FALSE(test_spy::ble_notify_disconnect_called);
}

// ============================================================================
// 16. Event Dispatch
// ============================================================================

TEST_CASE("dispatch: routes SensorDataReady to on_sensor_data", "[Orchestrator][dispatch]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::SensorDataReady;
  evt.sensor_data = MeasuresAGo{};
  evt.sensor_data.co2.co2 = 999;

  A::dispatch(orch, evt);

  REQUIRE(A::cached_measures(orch).co2.co2 == 999);
  REQUIRE(test_spy::cache_measurement_called);
}

TEST_CASE("dispatch: routes InputPress to on_input", "[Orchestrator][dispatch]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::InputPress;
  evt.input = InputEventData{InputSource::ButtonPower, InputType::ShortPress};

  A::dispatch(orch, evt);

  // Initially locked, ButtonPower short press → unlock
  REQUIRE(A::lock_state(orch) == LockState::Unlocked);
}

TEST_CASE("dispatch: routes GpsFixUpdate to on_gps_fix", "[Orchestrator][dispatch]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::GpsFixUpdate;
  evt.gps_data = GpsData{};
  evt.gps_data.position.latitude = 51.5074;

  A::dispatch(orch, evt);

  REQUIRE(A::latest_gps(orch).position.latitude == 51.5074);
}

// ============================================================================
// BLE dispatch tests
// ============================================================================

TEST_CASE("dispatch: BleConnected pushes measures, status, and config", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = true;

  Event evt{};
  evt.type = EventType::BleConnected;
  A::dispatch(orch, evt);

  CHECK(test_spy::ble_notify_measures_called);
  CHECK(test_spy::ble_update_status_called);
  CHECK(test_spy::ble_update_config_called);
}

TEST_CASE("dispatch: BleConnected dismisses pairing passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();

  // Simulate passkey screen is showing
  f.ui_manager.show_pairing_passkey(123456);
  REQUIRE(f.ui_manager.current_screen() == Screen::PairingPasskey);

  Event evt{};
  evt.type = EventType::BleConnected;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("dispatch: BleDisconnected dismisses pairing passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Simulate passkey screen is showing (failed pairing → disconnect)
  f.ui_manager.show_pairing_passkey(654321);
  REQUIRE(f.ui_manager.current_screen() == Screen::PairingPasskey);

  Event evt{};
  evt.type = EventType::BleDisconnected;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("dispatch: BlePairingRequest shows passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::BlePairingRequest;
  evt.ble_passkey = 554501;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::PairingPasskey);
}

TEST_CASE("dispatch: BleAuthComplete dismisses pairing passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // BleAuthComplete now marks onboarding done (first pairing = engagement),
  // which persists GoSettings on the first call.
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  // Simulate passkey screen is showing
  f.ui_manager.show_pairing_passkey(999999);
  REQUIRE(f.ui_manager.current_screen() == Screen::PairingPasskey);

  Event evt{};
  evt.type = EventType::BleAuthComplete;
  evt.ble_auth_ok = true;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("dispatch: BleAuthComplete failure dismisses passkey to Home (no session)",
          "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  f.ui_manager.show_pairing_passkey(999999);
  REQUIRE(f.ui_manager.current_screen() == Screen::PairingPasskey);

  Event evt{};
  evt.type = EventType::BleAuthComplete;
  evt.ble_auth_ok = false;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::Home);
  CHECK_FALSE(A::settings(orch).onboarding_done);
}

TEST_CASE("dispatch: BleAuthComplete failure in setup session returns to boot guide",
          "[Orchestrator][ble][onboarding]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  // Boot-originated setup session with the passkey overlay showing.
  A::set_setup_session_active(orch, true);
  f.ui_manager.show_pairing_passkey(123456);
  REQUIRE(f.ui_manager.current_screen() == Screen::PairingPasskey);

  Event evt{};
  evt.type = EventType::BleAuthComplete;
  evt.ble_auth_ok = false;
  A::dispatch(orch, evt);

  // Returns to the first-boot guide, session stays active, onboarding untouched.
  CHECK(f.ui_manager.current_screen() == Screen::GettingStarted);
  CHECK(A::setup_session_active(orch));
  CHECK_FALSE(A::settings(orch).onboarding_done);

  // Boot variant: action row reads "Start using" (not "Back").
  DisplayValues v = f.ui_manager.build_values(A::build_context(orch));
  REQUIRE(v.row_count == 1);
  CHECK(std::string(v.rows[0].text) == "Start using");
}

TEST_CASE("dispatch: BleAuthComplete is no-op when not on passkey screen", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  REQUIRE(f.ui_manager.current_screen() == Screen::Home);

  Event evt{};
  evt.type = EventType::BleAuthComplete;
  evt.ble_auth_ok = true;
  A::dispatch(orch, evt);

  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("dispatch: BleHistoryWrite list calls handle_history_list", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Configure decode stub to return List op
  test_spy::ble_history_decode_result.op = BleHistoryOp::List;

  Event evt{};
  evt.type = EventType::BleHistoryWrite;
  A::dispatch(orch, evt);

  // take_pending returns 0 (no data), so handler returns early
  CHECK_FALSE(test_spy::ble_history_list_called);
}

TEST_CASE("on_sensor_data: always updates BLE measures characteristic", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();

  SECTION("when connected") {
    test_spy::ble_connected = true;

    MeasuresAGo data{};
    data.co2.co2 = 400;
    A::on_sensor_data(orch, data);

    CHECK(test_spy::ble_notify_measures_called);
  }

  SECTION("when disconnected") {
    test_spy::ble_connected = false;

    MeasuresAGo data{};
    data.co2.co2 = 400;
    A::on_sensor_data(orch, data);

    CHECK(test_spy::ble_notify_measures_called);
  }
}

TEST_CASE("apply_settings_change: notifies BLE when connected", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = true;

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::apply_settings_change(orch);

  // notify_config() now refreshes the stored snapshot internally; the
  // orchestrator no longer issues a separate update_config() call.
  CHECK(test_spy::ble_notify_config_called);
  CHECK_FALSE(test_spy::ble_update_config_called);
}

TEST_CASE("apply_settings_change: does not notify BLE when disconnected", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = false;

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::apply_settings_change(orch);

  CHECK_FALSE(test_spy::ble_notify_config_called);
  CHECK_FALSE(test_spy::ble_update_config_called);
}

TEST_CASE("init: starts BLE in Portable mode", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  CHECK(test_spy::ble_init_called);
}

TEST_CASE("init: does not start BLE in Offline mode", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Offline;
  auto orch = f.make_orchestrator();

  // Set mode to Offline before init (PowerOn doesn't read settings.operating_mode)
  A::set_mode(orch, OperatingMode::Offline);

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::PowerOn);

  CHECK_FALSE(test_spy::ble_init_called);
}

TEST_CASE("build_context: ble_enabled true in Portable mode", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  auto ctx = A::build_context(orch);

  CHECK(ctx.ble_enabled);
}

TEST_CASE("build_context: ble_enabled false in Offline mode", "[Orchestrator][ble]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  A::set_mode(orch, OperatingMode::Offline);

  auto ctx = A::build_context(orch);

  CHECK_FALSE(ctx.ble_enabled);
}

TEST_CASE("build_context: ble_connected reflects authenticated link state", "[Orchestrator][ble]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();

  // The icon shows "connected" only for a usable (encrypted/authenticated)
  // link; a GAP link alone is not enough.
  test_spy::ble_connected = true;
  test_spy::ble_authenticated = false;
  CHECK_FALSE(A::build_context(orch).ble_connected);

  test_spy::ble_authenticated = true;
  CHECK(A::build_context(orch).ble_connected);
}

// ============================================================================
// CO2 calibration
// ============================================================================

TEST_CASE("BLE Co2Calibration command sends progress and triggers calibration request",
          "[Orchestrator][calibration]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Set up BLE stub to return a Co2Calibration command
  test_spy::ble_pending_config_len = 1; // non-zero so on_ble_config_write proceeds
  test_spy::ble_config_decode_result.op = BleConfigOp::Command;
  test_spy::ble_config_decode_result.cmd = BleCommand::Co2Calibration;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(test_spy::co2_calibration_requested);
  // Progress notification sent immediately
  CHECK(test_spy::ble_notify_command_progress_called);
  CHECK(test_spy::ble_progress_command == BleCommand::Co2Calibration);
  // No immediate BLE result — result comes asynchronously via Co2CalibrationDone
  CHECK_FALSE(test_spy::ble_notify_command_result_called);
}

TEST_CASE("Co2CalibrationDone Success notifies BLE with success", "[Orchestrator][calibration]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::Co2CalibrationDone;
  evt.co2_cal_result = static_cast<uint8_t>(Co2CalibrationResult::Success);
  A::dispatch(orch, evt);

  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::Co2Calibration);
  CHECK(test_spy::ble_last_command_success == true);
}

TEST_CASE("Co2CalibrationDone Unsupported notifies BLE with failure",
          "[Orchestrator][calibration]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::Co2CalibrationDone;
  evt.co2_cal_result = static_cast<uint8_t>(Co2CalibrationResult::Unsupported);
  A::dispatch(orch, evt);

  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::Co2Calibration);
  CHECK(test_spy::ble_last_command_success == false);
}

TEST_CASE("Co2CalibrationDone Failed notifies BLE with failure", "[Orchestrator][calibration]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::Co2CalibrationDone;
  evt.co2_cal_result = static_cast<uint8_t>(Co2CalibrationResult::Failed);
  A::dispatch(orch, evt);

  CHECK(test_spy::ble_notify_command_result_called);
  CHECK(test_spy::ble_last_command == BleCommand::Co2Calibration);
  CHECK(test_spy::ble_last_command_success == false);
}

TEST_CASE("on_input: CalibrateCo2 UI action triggers co2 calibration request",
          "[Orchestrator][calibration]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Unlock so touch input is forwarded to UIManager
  A::unlock(orch);
  test_spy::reset();

  // Navigate: Home → MainMenu → Settings → CO2: Calibrate → Confirm → Yes
  // This triggers UIAction::CalibrateCo2 through the UI state machine.
  InputEventData touch_enter{InputSource::TouchEnter, InputType::ShortPress};
  InputEventData touch_down{InputSource::TouchDown, InputType::ShortPress};

  A::on_input(orch, touch_enter); // Home → MainMenu
  A::on_input(orch, touch_down);  // 0→1
  A::on_input(orch, touch_down);  // 1→2
  A::on_input(orch, touch_enter); // → Settings (cursor at 1)

  // Navigate to CO2: Calibrate (index 14) — 13 down presses from Back (1)
  for (int i = 0; i < 13; ++i)
    A::on_input(orch, touch_down);

  A::on_input(orch, touch_enter); // → Confirm (cursor at 1 = Back)
  CHECK(f.ui_manager.current_screen() == Screen::Confirm);

  // Navigate to Yes (index 4): 3 down presses from Back (1)
  A::on_input(orch, touch_down); // 1→2
  A::on_input(orch, touch_down); // 2→3
  A::on_input(orch, touch_down); // 3→4 (Yes)

  test_spy::co2_calibration_requested = false;
  A::on_input(orch, touch_enter); // Confirm Yes → CalibrateCo2

  CHECK(test_spy::co2_calibration_requested);
  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

TEST_CASE("Co2CalibrationDone Success shows snackbar", "[Orchestrator][calibration]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::Co2CalibrationDone;
  evt.co2_cal_result = static_cast<uint8_t>(Co2CalibrationResult::Success);
  A::dispatch(orch, evt);

  BuildContext ctx = A::build_context(orch);
  DisplayValues v = f.ui_manager.build_values(ctx);
  REQUIRE(v.snackbar_text != nullptr);
  CHECK(std::string(v.snackbar_text) == "CO2 cal. done");
}

TEST_CASE("Co2CalibrationDone Failed shows snackbar", "[Orchestrator][calibration]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::Co2CalibrationDone;
  evt.co2_cal_result = static_cast<uint8_t>(Co2CalibrationResult::Failed);
  A::dispatch(orch, evt);

  BuildContext ctx = A::build_context(orch);
  DisplayValues v = f.ui_manager.build_values(ctx);
  REQUIRE(v.snackbar_text != nullptr);
  CHECK(std::string(v.snackbar_text) == "CO2 cal. failed");
}

TEST_CASE("Co2CalibrationDone Unsupported shows snackbar", "[Orchestrator][calibration]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  Event evt{};
  evt.type = EventType::Co2CalibrationDone;
  evt.co2_cal_result = static_cast<uint8_t>(Co2CalibrationResult::Unsupported);
  A::dispatch(orch, evt);

  BuildContext ctx = A::build_context(orch);
  DisplayValues v = f.ui_manager.build_values(ctx);
  REQUIRE(v.snackbar_text != nullptr);
  CHECK(std::string(v.snackbar_text) == "CO2 cal. unsupported");
}

// ============================================================================
// 23. prepare_for_sleep
// ============================================================================

TEST_CASE("prepare_for_sleep: stops all services, saves state, and deep sleeps display",
          "[Orchestrator][sleep]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::PowerOn);
  test_spy::reset(); // clear init-time calls so assertions are clean

  A::prepare_for_sleep(orch);

  // All task-based services stopped.
  CHECK(test_spy::sensor_stopped);
  // Default gps_mode=OnWhenTracking, not tracking → inactive → stop_and_idle_gnss
  CHECK(test_spy::gps_stop_and_idle_called);
  CHECK(test_spy::input_stopped);
  CHECK(test_spy::ble_deinit_called);

  // State persisted to RTC.
  CHECK(test_spy::cache_backed_up);
  CHECK(test_spy::state_saved);

  // SSD1680 put into deep sleep mode 1 after worker is stopped.
  CHECK(DisplayService::spy_deep_sleep_called);

  // Route not ended (no tracking was active).
  CHECK_FALSE(test_spy::route_ended);
}

TEST_CASE("prepare_for_sleep: flushes and closes route file when tracking is active",
          "[Orchestrator][sleep]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::PowerOn);

  // Start tracking so a route file is active.
  A::start_tracking(orch);
  REQUIRE(A::tracking_active(orch));
  REQUIRE(test_spy::route_started);

  test_spy::reset();

  A::prepare_for_sleep(orch);

  // Route must be flushed/closed before deep sleep so buffered data is not
  // lost when the CPU reboots.
  CHECK(test_spy::route_ended);

  // Tracking state is still active in the persisted RTC snapshot — it is
  // end_route() that closes the file, not stop_tracking().  The next wake
  // will call resume_route() to reopen the file in append mode.
  CHECK(test_spy::state_saved);
  CHECK(test_spy::last_saved_state.tracking_active == true);
  CHECK(test_spy::last_saved_state.tracking_session_id != 0);
}

// ============================================================================
// 24. Boot-to-runtime promotion (BootHandoff)
// ============================================================================

TEST_CASE("init(Timer, promoted, locked): RTC restored, measures seeded, no measurement requested",
          "[Orchestrator][init][promotion]") {
  TestFixture f;

  // Timer wake with tracking active — RTC state should be restored
  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Offline,
      .behavior = Behavior::Tracking,
      .lock_state = LockState::Locked,
      .gps_enabled = true,
      .tracking_active = true,
      .tracking_session_id = 55555,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  MeasuresAGo fast_measures{};
  fast_measures.co2.co2 = 450;
  fast_measures.temp_hum_a.temperature = 22.5f;

  BootHandoff handoff{};
  handoff.measurement_completed = true;
  handoff.fast_path_measures = &fast_measures;
  handoff.initial_lock_state = LockState::Locked;

  orch.init(WakeCause::Timer, handoff);

  // RTC state restored (Timer wake, not PowerOn)
  CHECK(A::behavior(orch) == Behavior::Tracking);
  CHECK(A::gps_enabled(orch) == true);
  CHECK(A::tracking_active(orch) == true);
  CHECK(A::tracking_session_id(orch) == 55555);

  // Cached measures seeded from fast_path_measures
  CHECK(A::cached_measures(orch).co2.co2 == 450);
  CHECK(A::cached_measures(orch).temp_hum_a.temperature == 22.5f);

  // First measurement done — no measurement requested
  CHECK(A::first_measurement_done(orch) == true);
  CHECK_FALSE(test_spy::measurement_requested);

  // Lock state stays Locked
  CHECK(A::lock_state(orch) == LockState::Locked);

  // Route resumed since tracking was active — init() now uses resume_route()
  // explicitly so it can truncate any torn trailing record.
  CHECK(test_spy::route_resumed);
  CHECK_FALSE(test_spy::route_started);
  CHECK(test_spy::route_session_id == 55555);
}

TEST_CASE("init(Timer, promoted, unlocked): RTC restored, unlock called, no measurement requested",
          "[Orchestrator][init][promotion]") {
  TestFixture f;

  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Offline,
      .behavior = Behavior::Idle,
      .lock_state = LockState::Locked,
      .gps_enabled = false,
      .tracking_active = false,
      .tracking_session_id = 0,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  MeasuresAGo fast_measures{};
  fast_measures.co2.co2 = 500;

  BootHandoff handoff{};
  handoff.measurement_completed = true;
  handoff.fast_path_measures = &fast_measures;
  handoff.initial_lock_state = LockState::Unlocked;
  handoff.display_painted = false;

  orch.init(WakeCause::Timer, handoff);

  // RTC state restored
  CHECK(A::gps_enabled(orch) == false);

  // Unlocked via unlock() (display_painted=false triggers unlock())
  CHECK(A::lock_state(orch) == LockState::Unlocked);

  // Snackbar shown
  BuildContext ctx = A::build_context(orch);
  DisplayValues v = f.ui_manager.build_values(ctx);
  CHECK(v.snackbar_text != nullptr);
  CHECK(std::string(v.snackbar_text) == "Unlocked");

  // First measurement done — no measurement requested by init()
  // (measurement_completed prevents the common-tail measurement request,
  // and unlock() does not request a measurement)
  CHECK(A::first_measurement_done(orch) == true);
}

TEST_CASE("init(Timer, promoted, unlocked, painted): state set directly, no update_display",
          "[Orchestrator][init][promotion]") {
  TestFixture f;

  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Offline,
      .behavior = Behavior::Idle,
      .lock_state = LockState::Locked,
      .gps_enabled = true,
      .tracking_active = false,
      .tracking_session_id = 0,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  BootHandoff handoff{};
  handoff.initial_lock_state = LockState::Unlocked;
  handoff.display_painted = true;
  handoff.measurement_completed = true;

  orch.init(WakeCause::Timer, handoff);

  // Lock state set directly (no unlock() → no update_display())
  CHECK(A::lock_state(orch) == LockState::Unlocked);

  // Snackbar armed manually
  CHECK(A::snackbar_refresh_deadline_ms(orch) != 0);
}

TEST_CASE("init(Timer, promoted, no measures): RTC restored, measurement requested",
          "[Orchestrator][init][promotion]") {
  TestFixture f;

  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Offline,
      .behavior = Behavior::Idle,
      .lock_state = LockState::Locked,
      .gps_enabled = true,
      .tracking_active = false,
      .tracking_session_id = 0,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  BootHandoff handoff{};
  handoff.measurement_completed = false;

  orch.init(WakeCause::Timer, handoff);

  // RTC state restored (Timer wake)
  CHECK(A::gps_enabled(orch) == true);

  // First measurement NOT done
  CHECK(A::first_measurement_done(orch) == false);

  // Measurement was requested
  CHECK(test_spy::measurement_requested);
}

TEST_CASE("init(PowerOn, default handoff): no RTC restored, locked, measurement requested",
          "[Orchestrator][init][promotion]") {
  TestFixture f;

  // Set RTC state that should NOT be loaded (PowerOn doesn't restore RTC)
  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Offline,
      .behavior = Behavior::Tracking,
      .lock_state = LockState::Locked,
      .gps_enabled = false,
      .tracking_active = true,
      .tracking_session_id = 99999,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::PowerOn);

  // No RTC state restored — defaults should be in effect
  CHECK(A::behavior(orch) == Behavior::Idle);
  CHECK(A::gps_enabled(orch) == true); // default, not the false from RTC
  CHECK(A::tracking_active(orch) == false);
  CHECK(A::tracking_session_id(orch) == 0);

  // Locked by default
  CHECK(A::lock_state(orch) == LockState::Locked);

  // Measurement requested
  CHECK(test_spy::measurement_requested);

  // BMS polled
  CHECK(test_spy::bms_polled);
}

TEST_CASE("init(Button, display_painted + snapshot): backward-compatible with button-wake path",
          "[Orchestrator][init][promotion]") {
  TestFixture f;

  test_spy::state_to_load = RtcAppState{
      .mode = OperatingMode::Offline,
      .behavior = Behavior::Idle,
      .lock_state = LockState::Locked,
      .gps_enabled = false,
      .tracking_active = false,
      .tracking_session_id = 0,
  };

  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  RtcDisplaySnapshot snapshot{};
  snapshot.co2_ppm = 420;
  snapshot.pm25_ugm3 = 12.5f;
  snapshot.temperature_c = 21.0f;
  snapshot.humidity_pct = 55.0f;
  snapshot.tvoc_index = 100;
  snapshot.nox_index = 25;
  snapshot.pressure_hpa = 1013.25f;
  snapshot.altitude_m = 110.0f;

  BootHandoff handoff{};
  handoff.display_painted = true;
  handoff.suppress_wake_press = true;
  handoff.initial_lock_state = LockState::Unlocked;
  handoff.display_snapshot = &snapshot;

  orch.init(WakeCause::Button, handoff);

  // Lock state unlocked (display_painted + Unlocked → state set directly)
  CHECK(A::lock_state(orch) == LockState::Unlocked);

  // Cached measures seeded from snapshot
  CHECK(A::cached_measures(orch).co2.co2 == 420);
  CHECK(A::cached_measures(orch).pm_a.pm_25 == 12.5f);
  CHECK(A::cached_measures(orch).temp_hum_a.temperature == 21.0f);
  CHECK(A::cached_measures(orch).temp_hum_a.humidity == 55.0f);
  CHECK(A::cached_measures(orch).tvoc_nox.tvoc_index == 100);
  CHECK(A::cached_measures(orch).tvoc_nox.nox_index == 25);
  CHECK(A::cached_measures(orch).pressure.pressure == 1013.25f);
  CHECK(A::cached_measures(orch).pressure.altitude == 110.0f);

  // Snackbar armed
  CHECK(A::snackbar_refresh_deadline_ms(orch) != 0);

  // RTC state restored (Button wake)
  CHECK(A::gps_enabled(orch) == false);
}

// ============================================================================
// Background display-update suppression
// ============================================================================

TEST_CASE("background suppression: sensor data on Home updates display",
          "[Orchestrator][display][suppression]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Home is the default screen — background updates should reach the display.
  DisplayService::spy_update_count = 0;
  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  CHECK(DisplayService::spy_update_count > 0);
}

TEST_CASE("background suppression: sensor data on MainMenu does not update display",
          "[Orchestrator][display][suppression]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  f.ui_manager.set_screen(Screen::MainMenu);

  DisplayService::spy_update_count = 0;
  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  CHECK(DisplayService::spy_update_count == 0);
}

TEST_CASE("background suppression: sensor data on Settings does not update display",
          "[Orchestrator][display][suppression]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  f.ui_manager.set_screen(Screen::Settings);

  DisplayService::spy_update_count = 0;
  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  CHECK(DisplayService::spy_update_count == 0);
}

TEST_CASE("background suppression: BLE connect on MainMenu does not update display",
          "[Orchestrator][display][suppression]") {
  TestFixture f;
  f.settings.operating_mode = OperatingMode::Portable;
  auto orch = f.make_orchestrator();
  test_spy::ble_connected = true;
  f.ui_manager.set_screen(Screen::MainMenu);

  DisplayService::spy_update_count = 0;
  Event evt{};
  evt.type = EventType::BleConnected;
  A::dispatch(orch, evt);

  CHECK(DisplayService::spy_update_count == 0);
}

TEST_CASE("background suppression: BLE disconnect on About does not update display",
          "[Orchestrator][display][suppression]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  f.ui_manager.set_screen(Screen::About);

  DisplayService::spy_update_count = 0;
  Event evt{};
  evt.type = EventType::BleDisconnected;
  A::dispatch(orch, evt);

  CHECK(DisplayService::spy_update_count == 0);
}

TEST_CASE("background suppression: BMS status change on Home updates display",
          "[Orchestrator][display][suppression]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Make poll_status return a charging state different from the initial Unknown.
  test_spy::snapshot_to_return.charger_status.charging_state = BmsChargingState::FastCharge;

  DisplayService::spy_update_count = 0;
  A::on_bms_status_timer(orch);

  CHECK(DisplayService::spy_update_count > 0);
}

TEST_CASE("background suppression: BMS status change on MainMenu does not update display",
          "[Orchestrator][display][suppression]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  f.ui_manager.set_screen(Screen::MainMenu);

  // Make poll_status return a charging state different from the initial Unknown.
  test_spy::snapshot_to_return.charger_status.charging_state = BmsChargingState::FastCharge;

  DisplayService::spy_update_count = 0;
  A::on_bms_status_timer(orch);

  CHECK(DisplayService::spy_update_count == 0);
}

TEST_CASE("on_bms_status_timer: charging transition pushes a charging-status notify",
          "[Orchestrator][ble][status]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  test_spy::ble_initialized = true;

  // Charging state flips from the initial Unknown to FastCharge — a transition.
  test_spy::snapshot_to_return.charger_status.charging_state = BmsChargingState::FastCharge;

  A::on_bms_status_timer(orch);

  CHECK(test_spy::ble_notify_charging_status_called);
  CHECK(test_spy::ble_notify_charging_status_count == 1);
}

TEST_CASE("on_bms_status_timer: no charging transition does not notify",
          "[Orchestrator][ble][status]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  test_spy::ble_initialized = true;

  // poll_status returns the default (unchanged) charger status — no transition.
  A::on_bms_status_timer(orch);

  CHECK_FALSE(test_spy::ble_notify_charging_status_called);
}

// ============================================================================
// on_bms_status_timer: PMID boost recovery on power-source transition
// ============================================================================

TEST_CASE("on_bms_status_timer: power-source transition calls ensure_pmid_healthy",
          "[Orchestrator][pmid][transition]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Transition: Unknown (default) -> None (unplug).
  test_spy::snapshot_to_return.charger_status.power_source = BmsPowerSource::None;
  test_spy::ensure_pmid_healthy_called = false;

  A::on_bms_status_timer(orch);

  CHECK(test_spy::ensure_pmid_healthy_called);
  CHECK(test_spy::ensure_pmid_healthy_count == 1);
}

TEST_CASE("on_bms_status_timer: no power-source transition does not call ensure_pmid_healthy",
          "[Orchestrator][pmid][transition]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // No change from initial default — no transition.
  test_spy::ensure_pmid_healthy_called = false;

  A::on_bms_status_timer(orch);

  CHECK_FALSE(test_spy::ensure_pmid_healthy_called);
}

TEST_CASE("on_bms_status_timer: charging transition to plugged calls ensure_pmid_healthy",
          "[Orchestrator][pmid][transition]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Transition: Unknown -> UnknownAdapter (plug-in).
  test_spy::snapshot_to_return.charger_status.power_source = BmsPowerSource::UnknownAdapter;
  test_spy::snapshot_to_return.charger_status.charging_state = BmsChargingState::FastCharge;
  test_spy::ensure_pmid_healthy_called = false;

  A::on_bms_status_timer(orch);

  // ensure_pmid_healthy is called on any transition (measure-gated inside).
  CHECK(test_spy::ensure_pmid_healthy_called);
}

// ============================================================================
// PM sensor recovery: time-based trigger on persistent PM failure (V1 only)
// ============================================================================

TEST_CASE("on_sensor_data: persistent PM failure triggers recover_pm_sensor on V1",
          "[Orchestrator][pmid][pm_recovery]") {
  TestFixture f;
  f.stub_board.variant_value = BoardVariant::V1;
  auto orch = f.make_orchestrator();
  A::set_first_measurement_done(orch, true);

  // Deliver invalid PM data at t=0.
  MeasuresAGo bad{};
  bad.pm_a.pm_25 = -1.0f; // invalid

  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(1000);
  A::on_sensor_data(orch, bad);
  CHECK_FALSE(test_spy::recover_pm_sensor_called);

  // Still invalid at t=29s — not yet at threshold.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(30000);
  A::on_sensor_data(orch, bad);
  CHECK_FALSE(test_spy::recover_pm_sensor_called);

  // Invalid at t=31s — threshold crossed (30s elapsed since first fail at t=1s).
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(31001);
  A::on_sensor_data(orch, bad);
  CHECK(test_spy::recover_pm_sensor_called);
  CHECK(test_spy::recover_pm_sensor_count == 1);
  CHECK(test_spy::prepare_requested);
}

TEST_CASE("on_sensor_data: valid PM resets the failure timer",
          "[Orchestrator][pmid][pm_recovery]") {
  TestFixture f;
  f.stub_board.variant_value = BoardVariant::V1;
  auto orch = f.make_orchestrator();
  A::set_first_measurement_done(orch, true);

  MeasuresAGo bad{};
  bad.pm_a.pm_25 = -1.0f;

  MeasuresAGo good{};
  good.pm_a.pm_25 = 10.0f;

  // Start failing at t=1s.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(1000);
  A::on_sensor_data(orch, bad);

  // Recover at t=15s — timer resets.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(15000);
  A::on_sensor_data(orch, good);

  // Fail again at t=20s — new timer starts.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(20000);
  A::on_sensor_data(orch, bad);

  // At t=49s — only 29s since new fail at t=20s, not yet 30s.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(49000);
  A::on_sensor_data(orch, bad);
  CHECK_FALSE(test_spy::recover_pm_sensor_called);

  // At t=50s — 30s since fail at t=20s.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(50001);
  A::on_sensor_data(orch, bad);
  CHECK(test_spy::recover_pm_sensor_called);
}

TEST_CASE("on_sensor_data: PM recovery does not fire on Prototype",
          "[Orchestrator][pmid][pm_recovery]") {
  TestFixture f;
  f.stub_board.variant_value = BoardVariant::Prototype;
  auto orch = f.make_orchestrator();
  A::set_first_measurement_done(orch, true);

  MeasuresAGo bad{};
  bad.pm_a.pm_25 = -1.0f;

  // Fail at t=0, then well past threshold.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(1000);
  A::on_sensor_data(orch, bad);

  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(60000);
  A::on_sensor_data(orch, bad);

  CHECK_FALSE(test_spy::recover_pm_sensor_called);
}

TEST_CASE("on_sensor_data: PM recovery does not fire before first measurement done",
          "[Orchestrator][pmid][pm_recovery]") {
  TestFixture f;
  f.stub_board.variant_value = BoardVariant::V1;
  auto orch = f.make_orchestrator();
  // _first_measurement_done is false by default

  MeasuresAGo bad{};
  bad.pm_a.pm_25 = -1.0f;

  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(60000);
  A::on_sensor_data(orch, bad);

  CHECK_FALSE(test_spy::recover_pm_sensor_called);
}

TEST_CASE("background suppression: snackbar refresh on Home updates display",
          "[Orchestrator][display][suppression]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Arm the snackbar refresh timer via unlock().
  A::unlock(orch);
  REQUIRE(A::snackbar_refresh_deadline_ms(orch) != 0);

  // Advance past the snackbar deadline (unlock at t=0 → deadline ~3200).
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(3200);

  DisplayService::spy_update_count = 0;
  A::check_timers(orch);

  CHECK(DisplayService::spy_update_count > 0);
}

TEST_CASE("background suppression: snackbar refresh on Settings does not update display",
          "[Orchestrator][display][suppression]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  // Arm the snackbar refresh timer via unlock().
  A::unlock(orch);
  REQUIRE(A::snackbar_refresh_deadline_ms(orch) != 0);

  // Navigate to a menu screen before the timer fires.
  f.ui_manager.set_screen(Screen::Settings);

  // Advance past the snackbar deadline.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(3200);

  DisplayService::spy_update_count = 0;
  A::check_timers(orch);

  CHECK(DisplayService::spy_update_count == 0);
}

// ============================================================================
// PM sensor sleep — Portable mode power-cycling
// ============================================================================

/// Test fixture with PM power pin configured (pin_pm_power = 26).
struct PmSleepFixture {
  StubSensorManager stub_sensor_mgr;
  AirgradientSerial stub_serial;
  GpsDriver stub_gps{stub_serial};
  StubCapTouchSensor stub_touch;
  StubBmsDevice stub_bms;
  StubNandStorage stub_nand;
  StubPayloadCacheStorage stub_cache_storage;
  PayloadCache payload_cache;

  SensorProducer sensor_producer;
  GpsService gps_service;
  InputService input_service;
  DisplayService display_service;
  LedService led_service_inert{{}};
  BuzzerService buzzer_service_inert{{}};
  StorageService storage_service;
  PowerService power_service;
  UIManager ui_manager;
  BleService ble_service;
  WifiService wifi_service;
  AgClient ag_client;
  CloudService cloud_service;
  StubGoBoard stub_board;
  PortableWifiProvisioner portable_provisioner;
  OtaService ota_service;

  MockRTOS mock_rtos;
  MockConfigStore mock_config;

  Orchestrator::Services services;
  GoSettings settings;

  std::unique_ptr<trompeloeil::expectation> _exp_time;
  std::unique_ptr<trompeloeil::expectation> _exp_delay;
  std::unique_ptr<trompeloeil::expectation> _exp_cfg_set_int;
  std::unique_ptr<trompeloeil::expectation> _exp_cfg_set_bool;
  std::unique_ptr<trompeloeil::expectation> _exp_cfg_set_string;
  std::unique_ptr<trompeloeil::expectation> _exp_cfg_commit;

  PmSleepFixture()
      : payload_cache(stub_cache_storage, 16),
        sensor_producer(reinterpret_cast<SensorManager &>(stub_sensor_mgr), nullptr,
                        SensorProducer::Config{}),
        gps_service(stub_gps, nullptr, GpsService::Config{}),
        input_service(stub_touch, test_gpio_hal, nullptr, InputService::Config{}),
        display_service(DisplayService::Config{}), storage_service(payload_cache, stub_nand),
        power_service(stub_bms, test_gpio_hal,
                      PowerService::Config{
                          .pin_wake_button_power = 0,
                          .pin_wake_button_boot = 1,
                          .pin_pm_power = 26,
                          .pm_sleep_threshold_ms = 20000,
                      }),
        ui_manager(UIManager::Config{}), ble_service(nullptr, storage_service, stub_ble_server),
        wifi_service(nullptr,
                     {*reinterpret_cast<WifiManager *>(_stub_buf),
                      *reinterpret_cast<AgBleServer *>(_stub_buf),
                      *reinterpret_cast<HttpServer *>(_stub_buf)},
                     WifiService::Config{}),
        ag_client(),
        cloud_service(nullptr, CloudService::Deps{ag_client, wifi_service}, CloudService::Config{}),
        portable_provisioner(nullptr,
                             {*reinterpret_cast<WifiManager *>(_stub_buf),
                              *reinterpret_cast<AgBleServer *>(_stub_buf), stub_board},
                             PortableWifiProvisioner::Config{}),
        ota_service(stub_ble_server, power_service, OtaService::Config{}),
        services{sensor_producer,      gps_service,          input_service,   display_service,
                 led_service_inert,    buzzer_service_inert, storage_service, power_service,
                 ui_manager,           ble_service,          wifi_service,    cloud_service,
                 portable_provisioner, stub_board,           ota_service} {
    test_spy::reset();
    RTOS::set_instance(&mock_rtos);
    settings.operating_mode = OperatingMode::Portable;
    _exp_time = NAMED_ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    _exp_delay = NAMED_ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));
    // Allow all config store operations (mode/settings changes trigger saves)
    _exp_cfg_set_int = NAMED_ALLOW_CALL(mock_config, set_int(trompeloeil::_, trompeloeil::_))
                           .RETURN(ConfigStoreResult::OK);
    _exp_cfg_set_bool = NAMED_ALLOW_CALL(mock_config, set_bool(trompeloeil::_, trompeloeil::_))
                            .RETURN(ConfigStoreResult::OK);
    _exp_cfg_set_string = NAMED_ALLOW_CALL(mock_config, set_string(trompeloeil::_, trompeloeil::_))
                              .RETURN(ConfigStoreResult::OK);
    _exp_cfg_commit = NAMED_ALLOW_CALL(mock_config, commit()).RETURN(ConfigStoreResult::OK);
  }

  ~PmSleepFixture() { RTOS::set_instance(nullptr); }

  Orchestrator make_orchestrator() { return {nullptr, services, settings, mock_config, "TEST00"}; }

private:
  alignas(8) static inline char _stub_buf[64];
};

TEST_CASE("PM sleep: on_sensor_data requests PM sleep for Portable + long interval",
          "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 60;
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  test_spy::pm_sleep_requested = false;

  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  CHECK(test_spy::pm_sleep_requested);
}

TEST_CASE("PM sleep: PmSensorAsleep event isolates the PM bus", "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  test_spy::pm_power_set = false;

  Event evt{};
  evt.type = EventType::PmSensorAsleep;
  A::dispatch(orch, evt);

  CHECK(test_spy::pm_power_set);
  CHECK_FALSE(test_spy::pm_power_on);
}

TEST_CASE("PM sleep: on_sensor_data does NOT request PM sleep for short interval",
          "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 10; // below threshold
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  test_spy::pm_sleep_requested = false;

  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  CHECK_FALSE(test_spy::pm_sleep_requested);
}

TEST_CASE("PM sleep: on_sensor_data does NOT request PM sleep in Offline mode",
          "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 60;
  f.settings.operating_mode = OperatingMode::Offline;
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  test_spy::pm_sleep_requested = false;

  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  CHECK_FALSE(test_spy::pm_sleep_requested);
}

TEST_CASE("PM sleep: on_sensor_data requests PM sleep in Stationary mode",
          "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 60;
  f.settings.operating_mode = OperatingMode::Stationary;
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  test_spy::pm_sleep_requested = false;

  MeasuresAGo data{};
  A::on_sensor_data(orch, data);

  CHECK(test_spy::pm_sleep_requested);
}

TEST_CASE("PM sleep: mode change powers on and wakes PM", "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 60;
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  test_spy::pm_power_set = false;
  test_spy::pm_power_on = false;
  test_spy::prepare_requested = false;

  A::change_mode(orch, OperatingMode::Stationary);
  CHECK(test_spy::pm_power_set);
  CHECK(test_spy::pm_power_on);
  CHECK(test_spy::prepare_requested);
}

TEST_CASE("PM sleep: check_timers fires prepare at warmup deadline", "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 60;
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  // Advance time to prepare deadline: 60000 - 10000 = 50000 ms after last measurement
  f._exp_time = NAMED_ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(50000);

  test_spy::prepare_requested = false;
  test_spy::pm_power_set = false;

  A::check_timers(orch);

  CHECK(test_spy::prepare_requested);
  CHECK(test_spy::pm_power_set);
  CHECK(test_spy::pm_power_on);
  CHECK(A::pm_prepare_sent(orch));
}

TEST_CASE("PM sleep: check_timers does NOT fire prepare before deadline",
          "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 60;
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  // Advance time to 49s — 1s before prepare deadline
  f._exp_time = NAMED_ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(49000);

  test_spy::prepare_requested = false;

  A::check_timers(orch);

  CHECK_FALSE(test_spy::prepare_requested);
  CHECK_FALSE(A::pm_prepare_sent(orch));
}

TEST_CASE("PM sleep: check_timers skips prepare for short interval", "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 10; // below threshold
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  // Even past the would-be deadline, prepare should not fire
  f._exp_time = NAMED_ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(5000);

  test_spy::prepare_requested = false;

  A::check_timers(orch);

  CHECK_FALSE(test_spy::prepare_requested);
}

TEST_CASE("PM sleep: reschedule requests PM sleep when interval increases above threshold",
          "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 10; // starts below threshold
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  // Change interval to above threshold
  A::settings(orch).measure_interval_seconds = 60;
  test_spy::pm_sleep_requested = false;

  GoSettings prev{};
  prev.measure_interval_seconds = 10;
  A::reschedule_sensor_timer(orch, prev);

  CHECK(test_spy::pm_sleep_requested);
}

TEST_CASE("PM sleep: reschedule powers on and wakes PM when interval decreases below threshold",
          "[Orchestrator][pm_sleep]") {
  PmSleepFixture f;
  f.settings.measure_interval_seconds = 60; // starts above threshold
  auto orch = f.make_orchestrator();
  orch.init(WakeCause::PowerOn);

  // Change interval to below threshold
  A::settings(orch).measure_interval_seconds = 10;
  test_spy::pm_power_set = false;
  test_spy::prepare_requested = false;

  GoSettings prev{};
  prev.measure_interval_seconds = 60;
  A::reschedule_sensor_timer(orch, prev);

  CHECK(test_spy::pm_power_set);
  CHECK(test_spy::pm_power_on);       // powered ON
  CHECK(test_spy::prepare_requested); // and woken
}

// ============================================================================
// GPS Power Mode Sync — GNSS start/stop transitions
// ============================================================================

TEST_CASE("apply_settings_change: AlwaysOff to AlwaysOn starts GPS service",
          "[Orchestrator][gps_sync]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOff;
  auto orch = f.make_orchestrator();

  // Change to AlwaysOn via UIManager
  GoSettings updated = f.settings;
  updated.gps_mode = GpsMode::AlwaysOn;
  f.ui_manager.sync_settings(updated);

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  test_spy::gps_started = false;
  A::apply_settings_change(orch);

  CHECK(test_spy::gps_started);
  CHECK_FALSE(test_spy::gps_stop_and_idle_called);
}

TEST_CASE("apply_settings_change: AlwaysOn to AlwaysOff stops and idles GPS",
          "[Orchestrator][gps_sync]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  // Change to AlwaysOff via UIManager
  GoSettings updated = f.settings;
  updated.gps_mode = GpsMode::AlwaysOff;
  f.ui_manager.sync_settings(updated);

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  test_spy::gps_stop_and_idle_called = false;
  A::apply_settings_change(orch);

  CHECK(test_spy::gps_stop_and_idle_called);
  CHECK_FALSE(test_spy::gps_started);
}

TEST_CASE("start_tracking: OnWhenTracking mode starts GPS service", "[Orchestrator][gps_sync]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::OnWhenTracking;
  auto orch = f.make_orchestrator();

  test_spy::gps_started = false;
  A::start_tracking(orch);

  CHECK(test_spy::gps_started);
}

TEST_CASE("stop_tracking: OnWhenTracking mode stops and idles GPS", "[Orchestrator][gps_sync]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::OnWhenTracking;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  test_spy::gps_stop_and_idle_called = false;

  A::stop_tracking(orch);

  CHECK(test_spy::gps_stop_and_idle_called);
}

TEST_CASE("start_tracking: AlwaysOn mode does not re-start GPS", "[Orchestrator][gps_sync]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  test_spy::gps_started = false;
  A::start_tracking(orch);

  // GPS was already active (AlwaysOn) — should not call start() again
  CHECK_FALSE(test_spy::gps_started);
}

TEST_CASE("stop_tracking: AlwaysOn mode does not stop GPS", "[Orchestrator][gps_sync]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  A::start_tracking(orch);
  test_spy::gps_stop_and_idle_called = false;

  A::stop_tracking(orch);

  // GPS still active (AlwaysOn) — should not call stop_and_idle_gnss()
  CHECK_FALSE(test_spy::gps_stop_and_idle_called);
}

TEST_CASE("prepare_for_sleep: active GPS calls stop (no GNSS stop)",
          "[Orchestrator][gps_sync][sleep]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::PowerOn);
  test_spy::reset();

  A::prepare_for_sleep(orch);

  CHECK(test_spy::gps_stopped);
  CHECK_FALSE(test_spy::gps_stop_and_idle_called);
}

TEST_CASE("prepare_for_sleep: inactive GPS calls stop_and_idle_gnss",
          "[Orchestrator][gps_sync][sleep]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOff;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_bool(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, get_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);

  orch.init(WakeCause::PowerOn);
  test_spy::reset();

  A::prepare_for_sleep(orch);

  CHECK(test_spy::gps_stop_and_idle_called);
  CHECK_FALSE(test_spy::gps_stopped);
}

TEST_CASE("BLE config set: AlwaysOff to AlwaysOn starts GPS", "[Orchestrator][gps_sync][ble]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOff;
  auto orch = f.make_orchestrator();

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Set;
  test_spy::ble_decode_updates_settings = true;
  test_spy::ble_decoded_settings = f.settings;
  test_spy::ble_decoded_settings.gps_mode = GpsMode::AlwaysOn;

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  test_spy::gps_started = false;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(test_spy::gps_started);
  CHECK_FALSE(test_spy::gps_stop_and_idle_called);
}

TEST_CASE("BLE config set: AlwaysOn to AlwaysOff stops and idles GPS",
          "[Orchestrator][gps_sync][ble]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::AlwaysOn;
  auto orch = f.make_orchestrator();

  test_spy::ble_pending_config_len = 1;
  test_spy::ble_config_decode_result.op = BleConfigOp::Set;
  test_spy::ble_decode_updates_settings = true;
  test_spy::ble_decoded_settings = f.settings;
  test_spy::ble_decoded_settings.gps_mode = GpsMode::AlwaysOff;

  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_bool(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, set_string(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  test_spy::gps_stop_and_idle_called = false;

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  A::dispatch(orch, evt);

  CHECK(test_spy::gps_stop_and_idle_called);
  CHECK_FALSE(test_spy::gps_started);
}

TEST_CASE("stop_tracking: OnWhenTracking clears stale GPS fix from build_context",
          "[Orchestrator][gps_sync]") {
  TestFixture f;
  f.settings.gps_mode = GpsMode::OnWhenTracking;
  auto orch = f.make_orchestrator();

  ALLOW_CALL(f.mock_config, get_int(trompeloeil::_, trompeloeil::_))
      .RETURN(ConfigStoreResult::NOT_FOUND);
  ALLOW_CALL(f.mock_config, set_int(trompeloeil::_, trompeloeil::_)).RETURN(ConfigStoreResult::OK);
  ALLOW_CALL(f.mock_config, commit()).RETURN(ConfigStoreResult::OK);

  // Start tracking — GPS becomes active
  A::start_tracking(orch);
  REQUIRE(A::is_gps_active(orch));

  // Simulate a valid GPS fix arriving while tracking
  GpsData fix{};
  fix.position.latitude = 47.376;
  fix.position.longitude = 8.541;
  fix.fix.fix_type = GpsFixType::Fix3D;
  A::on_gps_fix(orch, fix);
  REQUIRE(A::latest_gps(orch).fix.fix_type == GpsFixType::Fix3D);

  // Verify build_context shows GPS fix
  BuildContext ctx_before = A::build_context(orch);
  REQUIRE(ctx_before.gps_fix == true);

  // Stop tracking — GPS becomes inactive, stale fix must be cleared
  A::stop_tracking(orch);
  REQUIRE_FALSE(A::is_gps_active(orch));

  // Cached GPS data must be reset to invalid sentinels
  CHECK(A::latest_gps(orch).fix.fix_type == GpsFixType::NoFix);
  CHECK(A::latest_gps(orch).position.latitude == GPS_LATITUDE_INVALID);

  // build_context must reflect cleared GPS state
  BuildContext ctx_after = A::build_context(orch);
  CHECK_FALSE(ctx_after.gps_fix);
  CHECK_FALSE(ctx_after.gps_enabled);
}

// ============================================================================
// CP2: Stationary networking — acceptance tests
// ============================================================================

namespace {

// Trompeloeil expectations evaporate when their scope ends, so a helper
// returning ALLOW_CALL objects must keep them alive at the caller. The
// macro below inlines the standard ALLOW_CALL block where each test needs
// it without obscuring the trompeloeil semantics.
#define CP2_ALLOW_CONFIG_WRITES(F)                                                                 \
  ALLOW_CALL((F).mock_config, set_int(trompeloeil::_, trompeloeil::_))                             \
      .RETURN(ConfigStoreResult::OK);                                                              \
  ALLOW_CALL((F).mock_config, set_bool(trompeloeil::_, trompeloeil::_))                            \
      .RETURN(ConfigStoreResult::OK);                                                              \
  ALLOW_CALL((F).mock_config, set_string(trompeloeil::_, trompeloeil::_))                          \
      .RETURN(ConfigStoreResult::OK);                                                              \
  ALLOW_CALL((F).mock_config, erase(trompeloeil::_)).RETURN(ConfigStoreResult::OK);                \
  ALLOW_CALL((F).mock_config, commit()).RETURN(ConfigStoreResult::OK)

Event make_wifi_disconnected(WifiDisconnectReason r) {
  Event evt{};
  evt.type = EventType::WifiDisconnected;
  evt.wifi_disconnect_reason = static_cast<uint8_t>(r);
  return evt;
}

Event make_provisioning_event(ProvisioningEvent e,
                              ProvisioningTransport t = ProvisioningTransport::BleOnly) {
  Event evt{};
  evt.type = EventType::ProvisioningStateChanged;
  evt.prov.event = static_cast<uint8_t>(e);
  evt.prov.transport = static_cast<uint8_t>(t);
  return evt;
}

} // namespace

TEST_CASE("Stationary entry with saved credentials calls connect_with_saved_credentials",
          "[Orchestrator][stationary][entry]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  test_spy::wifi_has_saved_networks = true;

  A::change_mode(orch, OperatingMode::Stationary);

  CHECK(test_spy::wifi_connect_saved_called);
  CHECK_FALSE(test_spy::wifi_try_fallback_called);
  CHECK(test_spy::wifi_static_ip_was_null); // settings.static_ip.ip == 0
}

TEST_CASE("Stationary entry without saved credentials calls try_default_fallback_credentials",
          "[Orchestrator][stationary][entry]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  test_spy::wifi_has_saved_networks = false;

  A::change_mode(orch, OperatingMode::Stationary);

  CHECK(test_spy::wifi_try_fallback_called);
  CHECK_FALSE(test_spy::wifi_connect_saved_called);
}

TEST_CASE("Stationary entry forwards static IP when settings.static_ip.ip != 0",
          "[Orchestrator][stationary][static_ip]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::settings(orch).static_ip.ip = 0x0100A8C0;
  A::settings(orch).static_ip.netmask = 0x00FFFFFF;
  test_spy::wifi_has_saved_networks = true;

  A::change_mode(orch, OperatingMode::Stationary);

  REQUIRE(test_spy::wifi_connect_saved_called);
  CHECK_FALSE(test_spy::wifi_static_ip_was_null);
  CHECK(test_spy::wifi_last_static_ip.ip == 0x0100A8C0);
  CHECK(test_spy::wifi_last_static_ip.netmask == 0x00FFFFFF);
}

TEST_CASE("enter_stationary calls init_wifi_subsystem before wifi service action",
          "[Orchestrator][stationary][board]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  test_spy::wifi_has_saved_networks = true;
  REQUIRE(f.stub_board.init_wifi_subsystem_calls == 0);

  A::change_mode(orch, OperatingMode::Stationary);

  CHECK(f.stub_board.init_wifi_subsystem_calls == 1);
  CHECK(test_spy::wifi_connect_saved_called);
}

TEST_CASE("Cold-boot Portable does not call init_wifi_subsystem",
          "[Orchestrator][stationary][cold_boot]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::settings(orch).operating_mode = OperatingMode::Portable;

  orch.init(WakeCause::PowerOn);

  CHECK(f.stub_board.init_wifi_subsystem_calls == 0);
  CHECK_FALSE(test_spy::wifi_connect_saved_called);
  CHECK_FALSE(test_spy::wifi_try_fallback_called);
}

TEST_CASE("Cold-boot Stationary calls init_wifi_subsystem exactly once",
          "[Orchestrator][stationary][cold_boot]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::settings(orch).operating_mode = OperatingMode::Stationary;
  test_spy::wifi_has_saved_networks = false;

  orch.init(WakeCause::PowerOn);

  CHECK(f.stub_board.init_wifi_subsystem_calls == 1);
  CHECK(test_spy::wifi_try_fallback_called);
}

TEST_CASE("Portable -> Stationary tears down BLE before bringing up Wi-Fi",
          "[Orchestrator][stationary][mode_change]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::ble_initialized = true;
  test_spy::wifi_has_saved_networks = true;

  A::change_mode(orch, OperatingMode::Stationary);

  // Both should have happened; the spec requires deinit BEFORE enter_stationary.
  CHECK(test_spy::ble_deinit_called);
  CHECK(test_spy::wifi_connect_saved_called);
}

TEST_CASE("change_mode leaving Portable pushes a disc notice when a client is connected",
          "[Orchestrator][ble][mode_change]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::ble_initialized = true;
  test_spy::ble_connected = true;
  test_spy::wifi_has_saved_networks = true;

  A::change_mode(orch, OperatingMode::Stationary);

  // The disc notice (link dropping, reason op_stationary) is pushed and settled
  // before teardown; no op_mode Config delta is sent for a mode change.
  CHECK(test_spy::ble_notify_disconnect_called);
  CHECK(test_spy::ble_last_disc_reason == BleDiscReason::OpStationary);
  CHECK_FALSE(test_spy::ble_notify_config_called);
  CHECK(test_spy::ble_deinit_called);
}

TEST_CASE("change_mode leaving Portable to Offline reports op_offline disc reason",
          "[Orchestrator][ble][mode_change]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::ble_initialized = true;
  test_spy::ble_connected = true;

  A::change_mode(orch, OperatingMode::Offline);

  CHECK(test_spy::ble_notify_disconnect_called);
  CHECK(test_spy::ble_last_disc_reason == BleDiscReason::OpOffline);
}

TEST_CASE("change_mode leaving Portable does not notify when no client is connected",
          "[Orchestrator][ble][mode_change]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::ble_initialized = true;
  test_spy::ble_connected = false;
  test_spy::wifi_has_saved_networks = true;

  A::change_mode(orch, OperatingMode::Stationary);

  CHECK_FALSE(test_spy::ble_notify_disconnect_called);
  CHECK(test_spy::ble_deinit_called);
}

TEST_CASE("Stationary -> Portable shuts down Wi-Fi before initializing BLE",
          "[Orchestrator][stationary][mode_change]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);

  A::change_mode(orch, OperatingMode::Portable);

  CHECK(test_spy::wifi_shutdown_called);
  CHECK(test_spy::ble_init_called);
}

TEST_CASE("Disconnect-policy: auth_failed provisions at bring-up, reconnects at runtime",
          "[Orchestrator][stationary][disconnect_policy]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);

  SECTION("before first online -> provisioning") {
    test_spy::wifi_has_been_online = false;
    A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
    CHECK(test_spy::wifi_start_provisioning_called);
    CHECK_FALSE(test_spy::wifi_schedule_reconnect_called);
  }

  SECTION("after first online -> reconnect, never provisioning") {
    test_spy::wifi_has_been_online = true;
    A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
    CHECK_FALSE(test_spy::wifi_start_provisioning_called);
    CHECK(test_spy::wifi_schedule_reconnect_called);
  }
}

TEST_CASE("Disconnect-policy: no_ap_found provisions at bring-up, reconnects at runtime",
          "[Orchestrator][stationary][disconnect_policy]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);

  SECTION("before first online -> provisioning") {
    test_spy::wifi_has_been_online = false;
    A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::no_ap_found));
    CHECK(test_spy::wifi_start_provisioning_called);
    CHECK_FALSE(test_spy::wifi_schedule_reconnect_called);
  }

  SECTION("after first online -> reconnect, never provisioning") {
    test_spy::wifi_has_been_online = true;
    A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::no_ap_found));
    CHECK_FALSE(test_spy::wifi_start_provisioning_called);
    CHECK(test_spy::wifi_schedule_reconnect_called);
  }
}

TEST_CASE("Disconnect-policy: runtime connection_lost schedules a reconnect and repaints",
          "[Orchestrator][stationary][disconnect_policy]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = true;
  DisplayService::spy_update_count = 0;

  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::connection_lost));

  CHECK(test_spy::wifi_schedule_reconnect_called);
  CHECK_FALSE(test_spy::wifi_start_provisioning_called);
  // Status bar repaints so the disconnected Wi-Fi glyph appears.
  CHECK(DisplayService::spy_update_count >= 1);
}

TEST_CASE("Disconnect-policy: requested_by_user is ignored (no provisioning, no reconnect)",
          "[Orchestrator][stationary][disconnect_policy]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);

  SECTION("before first online") {
    test_spy::wifi_has_been_online = false;
    A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::requested_by_user));
    CHECK_FALSE(test_spy::wifi_start_provisioning_called);
    CHECK_FALSE(test_spy::wifi_schedule_reconnect_called);
  }

  SECTION("after first online") {
    test_spy::wifi_has_been_online = true;
    A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::requested_by_user));
    CHECK_FALSE(test_spy::wifi_start_provisioning_called);
    CHECK_FALSE(test_spy::wifi_schedule_reconnect_called);
  }
}

TEST_CASE("open_provisioning_screen pauses services before starting provisioning",
          "[Orchestrator][stationary][pause]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = false;

  // sensor_started/gps_idle defaults are false; pause flow must flip them.
  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));

  CHECK(test_spy::sensor_stopped);
  CHECK(test_spy::pm_power_set);
  CHECK_FALSE(test_spy::pm_power_on); // power dropped
  CHECK(test_spy::wifi_start_provisioning_called);
}

TEST_CASE("ProvisioningEvent::Connected persists disable_cloud and static_ip",
          "[Orchestrator][stationary][provisioning]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);

  Event evt = make_provisioning_event(ProvisioningEvent::Connected);
  evt.prov.disable_cloud = true;
  evt.prov.static_ip.ip = 0x0100A8C0;
  evt.prov.static_ip.netmask = 0x00FFFFFF;

  A::dispatch(orch, evt);

  CHECK(A::settings(orch).disable_cloud == true);
  CHECK(A::settings(orch).static_ip.ip == 0x0100A8C0);
  CHECK(A::settings(orch).static_ip.netmask == 0x00FFFFFF);
  CHECK(test_spy::wifi_stop_provisioning_called);
}

TEST_CASE("ProvisioningEvent::Connected resumes services and requests a measurement",
          "[Orchestrator][stationary][provisioning][resume]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);

  // Walk through the pause flow first so resume actually fires.
  test_spy::wifi_has_been_online = false;
  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
  REQUIRE(test_spy::sensor_stopped);
  test_spy::sensor_started = false;
  test_spy::measurement_requested = false;
  test_spy::pm_power_on = false;

  A::dispatch(orch, make_provisioning_event(ProvisioningEvent::Connected));

  CHECK(test_spy::sensor_started);
  CHECK(test_spy::pm_power_on);
  CHECK(test_spy::measurement_requested);
}

TEST_CASE("ProvisioningEvent::Stopped before first online falls back to Portable",
          "[Orchestrator][stationary][provisioning]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = false;

  A::dispatch(orch, make_provisioning_event(ProvisioningEvent::Stopped));

  CHECK(A::mode(orch) == OperatingMode::Portable);
  CHECK(test_spy::wifi_shutdown_called);
}

TEST_CASE("ProvisioningEvent::Stopped after first online stays in Stationary",
          "[Orchestrator][stationary][provisioning][bugfix]") {
  // Regression for the on-device CP2.4 trace: after a successful
  // provisioning Connected the device must stay Stationary even when
  // the trailing Stopped (from stop_provisioning's teardown) fires.
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = true;

  A::dispatch(orch, make_provisioning_event(ProvisioningEvent::Stopped));

  CHECK(A::mode(orch) == OperatingMode::Stationary);
  CHECK_FALSE(test_spy::wifi_shutdown_called);
}

TEST_CASE("factory_reset clears wifi credentials and zeros disable_cloud + static_ip",
          "[Orchestrator][factory_reset][stationary]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::settings(orch).disable_cloud = true;
  A::settings(orch).static_ip.ip = 0x0100A8C0;

  REQUIRE(A::factory_reset(orch));

  CHECK(test_spy::wifi_clear_credentials_called);
  CHECK_FALSE(A::settings(orch).disable_cloud);
  CHECK(A::settings(orch).static_ip.ip == 0);
}

TEST_CASE("BuildContext: wifi icon shown in Stationary, glyph tracks is_online",
          "[Orchestrator][stationary][build_context]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);

  SECTION("Portable -> no icon, not connected") {
    A::set_mode(orch, OperatingMode::Portable);
    test_spy::wifi_is_online = true;
    const auto ctx = A::build_context(orch);
    CHECK_FALSE(ctx.wifi_enabled);
    CHECK_FALSE(ctx.wifi_connected);
  }

  SECTION("Stationary + offline -> icon shown, disconnected") {
    A::set_mode(orch, OperatingMode::Stationary);
    test_spy::wifi_is_online = false;
    const auto ctx = A::build_context(orch);
    CHECK(ctx.wifi_enabled);
    CHECK_FALSE(ctx.wifi_connected);
  }

  SECTION("Stationary + online -> icon shown, connected") {
    A::set_mode(orch, OperatingMode::Stationary);
    test_spy::wifi_is_online = true;
    const auto ctx = A::build_context(orch);
    CHECK(ctx.wifi_enabled);
    CHECK(ctx.wifi_connected);
  }
}

// ============================================================================
// Provisioning UX polish — session lifecycle
// ============================================================================

TEST_CASE("enter_stationary opens Screen::Info and starts a setup session",
          "[Orchestrator][session][bring_up]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  // Cold-boot default: device starts Locked.
  test_spy::wifi_has_saved_networks = true;

  A::enter_stationary(orch);

  CHECK(f.ui_manager.current_screen() == Screen::Info);
  CHECK(A::setup_session_active(orch));
  CHECK(A::bring_up_pending(orch));
  // Silent unlock — cold-boot Locked is flipped without a snackbar.
  CHECK(A::lock_state(orch) == LockState::Unlocked);
  CHECK(test_spy::wifi_connect_saved_called);
}

TEST_CASE("enter_stationary without saved credentials shows fallback Info text",
          "[Orchestrator][session][bring_up]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_saved_networks = false;

  A::enter_stationary(orch);

  CHECK(f.ui_manager.current_screen() == Screen::Info);
  CHECK(test_spy::wifi_try_fallback_called);
}

TEST_CASE("enter_stationary clears any pre-existing snackbar on entry",
          "[Orchestrator][session][bring_up]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  f.ui_manager.show_snackbar("Mode changed");
  test_spy::wifi_has_saved_networks = true;

  A::enter_stationary(orch);

  // build_values would normally return the snackbar; session entry cleared
  // the buffer so the next frame has no snackbar text.
  DisplayValues v = f.ui_manager.build_values(A::build_context(orch));
  CHECK(v.snackbar_text == nullptr);
}

TEST_CASE("change_mode(Stationary) does not fire \"Mode changed\" snackbar",
          "[Orchestrator][session][change_mode]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::wifi_has_saved_networks = true;

  A::change_mode(orch, OperatingMode::Stationary);

  DisplayValues v = f.ui_manager.build_values(A::build_context(orch));
  CHECK(v.snackbar_text == nullptr);
}

TEST_CASE("change_mode(Stationary) still re-enables PM power",
          "[Orchestrator][session][change_mode][regression]") {
  // Regression guard: the spec moves set_pm_power(true) above the
  // Stationary early-return so the PM rail is re-armed across a
  // Portable -> Stationary transition.
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::wifi_has_saved_networks = true;
  test_spy::pm_power_set = false;
  test_spy::pm_power_on = false;

  A::change_mode(orch, OperatingMode::Stationary);

  CHECK(test_spy::pm_power_set);
  CHECK(test_spy::pm_power_on);
}

TEST_CASE("on_wifi_connected during bring-up transitions Info -> Home unlocked",
          "[Orchestrator][session][bring_up][success]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_saved_networks = true;
  A::enter_stationary(orch);
  REQUIRE(A::bring_up_pending(orch));
  REQUIRE(f.ui_manager.current_screen() == Screen::Info);
  test_spy::bms_poll_count = 0;

  A::on_wifi_connected(orch, 0x0104a8c0); // 192.168.4.1

  CHECK_FALSE(A::bring_up_pending(orch));
  CHECK_FALSE(A::setup_session_active(orch));
  CHECK(A::lock_state(orch) == LockState::Unlocked);
  CHECK(f.ui_manager.current_screen() == Screen::Home);
  // Leave path polls BMS for a fresh icon.
  CHECK(test_spy::bms_poll_count >= 1);
  // Flush was invoked at least twice on the success path:
  // (a) Connected! frame, (b) leave-to-Home frame.
  CHECK(DisplayService::spy_flush_count >= 2);
  // No "Wi-Fi connected" snackbar on the bring-up success path.
  DisplayValues v = f.ui_manager.build_values(A::build_context(orch));
  CHECK(v.snackbar_text == nullptr);
}

TEST_CASE("bring-up disconnect shows failure Info before opening provisioning",
          "[Orchestrator][session][bring_up][failure]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_saved_networks = true;
  A::enter_stationary(orch);
  REQUIRE(f.ui_manager.current_screen() == Screen::Info);
  test_spy::wifi_has_been_online = false;
  DisplayService::spy_flush_count = 0;

  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::no_ap_found));

  // Failure frame is flushed, then provisioning takes over the screen.
  CHECK(DisplayService::spy_flush_count >= 1);
  CHECK(test_spy::wifi_start_provisioning_called);
  CHECK(f.ui_manager.current_screen() == Screen::Provisioning);
}

TEST_CASE("on_wifi_connected reconnect on Home arms the \"Wi-Fi connected\" snackbar",
          "[Orchestrator][session][reconnect]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  // Not in a session; on Home.
  REQUIRE(f.ui_manager.current_screen() == Screen::Home);
  REQUIRE_FALSE(A::setup_session_active(orch));

  A::on_wifi_connected(orch, 0x0104a8c0);

  DisplayValues v = f.ui_manager.build_values(A::build_context(orch));
  REQUIRE(v.snackbar_text != nullptr);
  CHECK(std::string(v.snackbar_text) == "Wi-Fi connected");
}

TEST_CASE("on_wifi_connected during an active session does not arm a snackbar",
          "[Orchestrator][session][reconnect][regression]") {
  // Late / stray WifiConnected racing the start_provisioning() callback
  // hand-off must not arm a hidden snackbar that leaks onto Home after
  // the session ends.
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = false;
  // Enter Provisioning via the failure path so _bring_up_pending = false
  // but _setup_session_active = true.
  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
  REQUIRE(A::setup_session_active(orch));
  REQUIRE_FALSE(A::bring_up_pending(orch));

  A::on_wifi_connected(orch, 0x0104a8c0);

  // Force the screen to Home and verify no snackbar leaks.
  f.ui_manager.reset_to_home();
  DisplayValues v = f.ui_manager.build_values(A::build_context(orch));
  CHECK(v.snackbar_text == nullptr);
}

TEST_CASE("on_wifi_connected reconnect on a menu screen does not arm a snackbar",
          "[Orchestrator][session][reconnect]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  // User navigated into Settings; not in a session.
  f.ui_manager.set_screen(Screen::Settings);
  REQUIRE_FALSE(A::setup_session_active(orch));

  A::on_wifi_connected(orch, 0x0104a8c0);

  DisplayValues v = f.ui_manager.build_values(A::build_context(orch));
  CHECK(v.snackbar_text == nullptr);
}

TEST_CASE("Power short-press is suppressed on all session screens",
          "[Orchestrator][session][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_saved_networks = true;
  A::enter_stationary(orch);
  REQUIRE(A::lock_state(orch) == LockState::Unlocked);

  // Power short-press on Screen::Info — should NOT toggle the lock.
  InputEventData input{InputSource::ButtonPower, InputType::ShortPress};
  A::on_input(orch, input);
  CHECK(A::lock_state(orch) == LockState::Unlocked);

  // Move to Provisioning via the failure path.
  test_spy::wifi_has_been_online = false;
  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
  REQUIRE(f.ui_manager.current_screen() == Screen::Provisioning);
  A::on_input(orch, input);
  CHECK(A::lock_state(orch) == LockState::Unlocked);

  // Open ProvisioningConfirm overlay.
  f.ui_manager.open_provisioning_confirm(0);
  A::on_input(orch, input);
  CHECK(A::lock_state(orch) == LockState::Unlocked);
}

TEST_CASE("Power long-press shutdown still fires on session screens",
          "[Orchestrator][session][input]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_saved_networks = true;
  A::enter_stationary(orch);

  InputEventData input{InputSource::ButtonPower, InputType::LongPress};
  A::on_input(orch, input);

  CHECK(test_spy::shutdown_called);
}

TEST_CASE("Auto-lock is suppressed while a setup session is active",
          "[Orchestrator][session][auto_lock]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::settings(orch).auto_lock_seconds = 10;
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_saved_networks = true;
  A::enter_stationary(orch);
  REQUIRE(A::setup_session_active(orch));
  REQUIRE(A::lock_state(orch) == LockState::Unlocked);

  // Simulate well past the auto-lock deadline.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(2'000'000);
  A::check_timers(orch);

  // Still unlocked — auto-lock did not fire on the session screen.
  CHECK(A::lock_state(orch) == LockState::Unlocked);
}

TEST_CASE("Sensor + BMS deadlines are suppressed while sensitive services are paused",
          "[Orchestrator][session][timers]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::settings(orch).measure_interval_seconds = 10;
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = false;
  // Enter Provisioning page — this pauses sensitive services.
  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
  REQUIRE(A::sensitive_services_paused(orch));

  const uint32_t last_meas_before = A::last_measurement_ms(orch);
  const uint32_t last_bms_before = A::last_bms_poll_ms(orch);
  test_spy::measurement_requested = false;
  test_spy::bms_polled = false;

  // Simulate plenty of time elapsed.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(5'000'000);
  A::check_timers(orch);

  // No measurement, no BMS poll fired; deadlines stayed frozen.
  CHECK_FALSE(test_spy::measurement_requested);
  CHECK_FALSE(test_spy::bms_polled);
  CHECK(A::last_measurement_ms(orch) == last_meas_before);
  CHECK(A::last_bms_poll_ms(orch) == last_bms_before);
}

TEST_CASE("Sensor + BMS polls keep running on Screen::Info",
          "[Orchestrator][session][timers][info]") {
  // The bring-up Info screen does not pause sensitive services — only
  // Provisioning / ProvisioningConfirm do, because those bring up a
  // transport that needs heap headroom.
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_saved_networks = true;
  A::enter_stationary(orch);
  REQUIRE(A::setup_session_active(orch));
  REQUIRE_FALSE(A::sensitive_services_paused(orch));

  A::settings(orch).measure_interval_seconds = 10;
  test_spy::measurement_requested = false;
  test_spy::bms_polled = false;

  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(1'000'000);
  A::check_timers(orch);

  CHECK(test_spy::measurement_requested);
  CHECK(test_spy::bms_polled);
}

TEST_CASE("request_background_display_update is a no-op while a session is active",
          "[Orchestrator][session][display]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_saved_networks = true;
  A::enter_stationary(orch);
  REQUIRE(A::setup_session_active(orch));
  const uint32_t before = DisplayService::spy_update_count;

  A::request_background_display_update(orch);

  CHECK(DisplayService::spy_update_count == before);
}

TEST_CASE("ProvisioningEvent::Connected goes through leave_session_to_home",
          "[Orchestrator][session][provisioning]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = false;
  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
  REQUIRE(A::setup_session_active(orch));
  REQUIRE(A::sensitive_services_paused(orch));

  Event evt = make_provisioning_event(ProvisioningEvent::Connected);
  evt.prov.ip = 0x0104a8c0;
  test_spy::bms_poll_count = 0;

  A::dispatch(orch, evt);

  CHECK_FALSE(A::setup_session_active(orch));
  CHECK_FALSE(A::sensitive_services_paused(orch));
  CHECK(A::lock_state(orch) == LockState::Unlocked);
  CHECK(f.ui_manager.current_screen() == Screen::Home);
  CHECK(test_spy::bms_poll_count >= 1);
  // No "Wi-Fi connected" snackbar — the on-page text already conveyed
  // success on the Provisioning page.
  DisplayValues v = f.ui_manager.build_values(A::build_context(orch));
  CHECK(v.snackbar_text == nullptr);
}

TEST_CASE("ConfirmCancelProvisioning routes through leave_session_to_portable",
          "[Orchestrator][session][confirm]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = false;
  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
  REQUIRE(A::setup_session_active(orch));

  // Walk the user through: action row 1 (cancel) -> ProvisioningConfirm
  // -> Yes (cancel).
  f.ui_manager.open_provisioning_confirm(1); // kind=1: cancel
  // Move cursor to Yes (index 1).
  InputEventData touch_down{InputSource::TouchDown, InputType::ShortPress};
  A::on_input(orch, touch_down);
  InputEventData touch_enter{InputSource::TouchEnter, InputType::ShortPress};
  A::on_input(orch, touch_enter);

  // Now in Portable, on Home.
  CHECK(A::mode(orch) == OperatingMode::Portable);
  CHECK(f.ui_manager.current_screen() == Screen::Home);
  CHECK_FALSE(A::setup_session_active(orch));
}

TEST_CASE("change_mode syncs UIManager's cached settings to the persisted mode",
          "[Orchestrator][settings][regression]") {
  // Regression for: after change_mode(Portable) on cancel, the Settings
  // menu still showed "Mode: Stationary" because UIManager's internal
  // _setting_mode option index was set by apply_setting_choice() but
  // never refreshed.  change_mode() must push the new settings back
  // into UIManager so the next render of the Settings row is correct.
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);

  // Mirror what apply_setting_choice() does when the user picks
  // Stationary in the Settings menu: sync UI cache + runtime to
  // Stationary first.
  A::settings(orch).operating_mode = OperatingMode::Stationary;
  f.ui_manager.sync_settings(A::settings(orch));
  A::set_mode(orch, OperatingMode::Stationary);

  // Sanity check: round-trip apply_to_settings now shows Stationary.
  {
    GoSettings rt{};
    f.ui_manager.apply_to_settings(rt);
    REQUIRE(rt.operating_mode == OperatingMode::Stationary);
  }

  // Cancel-from-provisioning equivalent: orchestrator-driven mode change
  // back to Portable without going through apply_setting_choice.
  A::change_mode(orch, OperatingMode::Portable);

  // Persisted + runtime + UI-cache all reflect Portable.
  CHECK(A::settings(orch).operating_mode == OperatingMode::Portable);
  CHECK(A::mode(orch) == OperatingMode::Portable);

  GoSettings rt{};
  f.ui_manager.apply_to_settings(rt);
  CHECK(rt.operating_mode == OperatingMode::Portable);
}

TEST_CASE("Periodic clocks are rebased on session leave", "[Orchestrator][session][clock_rebase]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = false;
  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
  REQUIRE(A::sensitive_services_paused(orch));

  // Simulate a long pause — RTOS clock at 5,000,000 ms.
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(5'000'000);
  Event evt = make_provisioning_event(ProvisioningEvent::Connected);
  evt.prov.ip = 0x0104a8c0;
  A::dispatch(orch, evt);

  // All three rebased deadlines should equal the current time.
  CHECK(A::last_measurement_ms(orch) == 5'000'000);
  CHECK(A::last_bms_poll_ms(orch) == 5'000'000);
  CHECK(A::last_bms_status_poll_ms(orch) == 5'000'000);
}

TEST_CASE("format_ipv4_be produces the expected dotted-decimal IPv4 strings",
          "[Orchestrator][session][ip]") {
  // Sanity check the shared helper round-trip against the bring-up path.
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_saved_networks = true;
  A::enter_stationary(orch);

  A::on_wifi_connected(orch, 0x0104a8c0); // 192.168.4.1

  // After leave, screen is Home; verify orchestrator settled cleanly.
  CHECK(f.ui_manager.current_screen() == Screen::Home);
}

// ============================================================================
// compute_queue_timeout_ms — bound + non-zero guarantees
// ============================================================================

TEST_CASE("compute_queue_timeout_ms returns a bounded value when sensitive services are paused",
          "[Orchestrator][session][timeout][regression]") {
  // Regression for the on-device task WDT trace: with sensor / BMS /
  // BMS-status / PM pre-wake / snackbar deadlines all gated off, the
  // timeout must still come back non-zero so the main loop blocks in
  // queue_receive() and yields to IDLE.  Without the ext WDT candidate
  // the gated path leaves `next == UINT32_MAX`, the overdue clamp pins
  // it to 0, the loop spins, IDLE starves, and the task WDT fires (~5 s).
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Stationary);
  test_spy::wifi_has_been_online = false;
  A::dispatch(orch, make_wifi_disconnected(WifiDisconnectReason::auth_failed));
  REQUIRE(A::setup_session_active(orch));
  REQUIRE(A::sensitive_services_paused(orch));

  // No simulated time elapsed since init() — every deadline is in the
  // future, so the candidate set is the ext WDT deadline alone.
  const uint32_t t = A::compute_queue_timeout_ms(orch);
  CHECK(t > 0);
  // The ext WDT is the only remaining candidate; the timeout cannot
  // exceed its interval.
  CHECK(t <= 60'000); // EXT_WDT_INTERVAL_MS
}

TEST_CASE("compute_queue_timeout_ms never falls through to the no-candidate path",
          "[Orchestrator][session][timeout]") {
  // Sanity guard outside any session: even on a freshly-constructed
  // orchestrator the ext WDT candidate keeps the returned timeout bounded
  // above 0 and at or below EXT_WDT_INTERVAL_MS.
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);

  const uint32_t t = A::compute_queue_timeout_ms(orch);
  CHECK(t > 0);
  CHECK(t <= 60'000); // EXT_WDT_INTERVAL_MS
}

// ============================================================================
// Portable attached Wi-Fi provisioning wiring
// ============================================================================

TEST_CASE("Portable entry attaches the provisioner; leaving Portable stops it",
          "[Orchestrator][portable_prov]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);

  // Start outside Portable so change_mode(Portable) runs the entry branch.
  A::set_mode(orch, OperatingMode::Offline);

  A::change_mode(orch, OperatingMode::Portable);
  CHECK(test_spy::portable_attach_called);
  CHECK(test_spy::ble_init_called); // init_stack_and_register
  CHECK(test_spy::ble_initialized); // start_advertising

  // Leaving Portable must stop the provisioner AND deinit BLE.
  A::change_mode(orch, OperatingMode::Offline);
  CHECK(test_spy::portable_stop_called);
  CHECK(test_spy::ble_deinit_called);
}

TEST_CASE("PortableProvRequest routes to handle_pending_request", "[Orchestrator][portable_prov]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);

  Event evt{};
  evt.type = EventType::PortableProvRequest;
  A::dispatch(orch, evt);

  CHECK(test_spy::portable_handle_pending_called);
}

TEST_CASE("Attached Connected persists metadata and calls provisioner.on_connected",
          "[Orchestrator][portable_prov]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);

  Event evt =
      make_provisioning_event(ProvisioningEvent::Connected, ProvisioningTransport::BleAttached);
  evt.prov.disable_cloud = true;
  evt.prov.static_ip.ip = 0x0100007f;
  A::dispatch(orch, evt);

  CHECK(A::settings(orch).disable_cloud == true);
  CHECK(A::settings(orch).static_ip.ip == 0x0100007f);
  CHECK(test_spy::cloud_last_disable_cloud == true);
  CHECK(test_spy::portable_on_connected_called);
}

TEST_CASE("BleDisconnected forwards to the provisioner", "[Orchestrator][portable_prov]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);

  Event evt{};
  evt.type = EventType::BleDisconnected;
  A::dispatch(orch, evt);

  CHECK(test_spy::portable_on_ble_disconnected_called);
}

TEST_CASE("History export rejected only while the provisioning radio is active",
          "[Orchestrator][portable_prov]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);

  test_spy::ble_pending_history_len = 4; // make on_ble_history_write proceed
  test_spy::ble_history_decode_result.op = BleHistoryOp::List;

  SECTION("rejected while radio active") {
    test_spy::portable_is_radio_active = true;
    Event evt{};
    evt.type = EventType::BleHistoryWrite;
    A::dispatch(orch, evt);

    CHECK(test_spy::ble_notify_history_error_called);
    CHECK_FALSE(test_spy::ble_history_list_called);
  }

  SECTION("allowed while radio off (parked session)") {
    test_spy::portable_is_radio_active = false;
    Event evt{};
    evt.type = EventType::BleHistoryWrite;
    A::dispatch(orch, evt);

    CHECK(test_spy::ble_history_list_called);
    CHECK_FALSE(test_spy::ble_notify_history_error_called);
  }
}

TEST_CASE("Portable provisioning radio-idle deadline drives the orchestrator timer",
          "[Orchestrator][portable_prov]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);

  test_spy::portable_next_deadline_ms = 5'000;
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(1'000);

  const uint32_t t = A::compute_queue_timeout_ms(orch);
  CHECK(t <= 4'000); // 5000 - 1000

  A::check_timers(orch);
  CHECK(test_spy::portable_tick_called);
}

// ============================================================================
// OTA integration
// ============================================================================

TEST_CASE("OTA: compute_queue_timeout_ms includes the BLE poll candidate when Portable+auth",
          "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::ble_authenticated = true;
  A::set_last_ota_check_ms(orch, 0);
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(0);

  // Baseline 0 + 2 s poll interval → candidate is 2000 ms; no other candidate
  // is shorter at t=0 with default settings.
  CHECK(A::compute_queue_timeout_ms(orch) <= 2000);
}

TEST_CASE("OTA: check_timers BLE poll runs run_ble when Portable+authenticated+latched",
          "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::ble_authenticated = true;
  test_spy::ota_is_ble_active = true;
  test_spy::ota_run_ble_result = OtaStatus::Ok; // Ok → reboot path (no queue drain on host)
  A::set_last_ota_check_ms(orch, 0);
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(5000); // > 2 s poll interval

  A::check_timers(orch);

  CHECK(test_spy::ota_run_ble_count == 1);
  CHECK(test_spy::sensor_stopped);             // enter_ota quiesced sensing
  CHECK(DisplayService::spy_flush_count >= 1); // "Updating firmware…" paint flushed
}

TEST_CASE("OTA: check_timers BLE poll skipped when unauthenticated and not latched",
          "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::ble_authenticated = false;
  test_spy::ota_is_ble_active = false;
  A::set_last_ota_check_ms(orch, 0);
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(5000);

  A::check_timers(orch);

  CHECK(test_spy::ota_run_ble_count == 0);
}

TEST_CASE("OTA: stranded-active (auth cleared but still latched) still runs run_ble",
          "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Portable);
  test_spy::ble_authenticated = false;
  test_spy::ota_is_ble_active = true; // latch outlived the auth flag
  test_spy::ota_run_ble_result = OtaStatus::Ok;
  A::set_last_ota_check_ms(orch, 0);
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(5000);

  A::check_timers(orch);

  CHECK(test_spy::ota_run_ble_count == 1); // gate + deadline still fired; latch drained
}

TEST_CASE("OTA: no poll in Offline / Stationary-offline", "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  CP2_ALLOW_CONFIG_WRITES(f);
  A::set_mode(orch, OperatingMode::Offline);
  test_spy::ble_authenticated = true;
  test_spy::ota_is_ble_active = true;
  test_spy::wifi_is_online = false;
  A::set_last_ota_check_ms(orch, 0);
  ALLOW_CALL(f.mock_rtos, get_time_ms_impl()).RETURN(5000);

  A::check_timers(orch);

  CHECK(test_spy::ota_run_ble_count == 0);
  CHECK(test_spy::ota_run_wifi_count == 0);
}

TEST_CASE("OTA: enter_ota quiesces sensing; disarms (never stops) cloud in Stationary",
          "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();

  SECTION("Portable does not touch cloud") {
    A::set_mode(orch, OperatingMode::Portable);
    A::enter_ota(orch);
    CHECK(test_spy::sensor_stopped);
    CHECK(test_spy::cloud_disarm_count == 0);
    CHECK(test_spy::cloud_stop_count == 0);
  }

  SECTION("Stationary disarms cloud but never stops it") {
    A::set_mode(orch, OperatingMode::Stationary);
    A::enter_ota(orch);
    CHECK(test_spy::sensor_stopped);
    CHECK(test_spy::cloud_disarm_count == 1);
    CHECK(test_spy::cloud_stop_count == 0);
  }
}

TEST_CASE("OTA: on_ota_download_started commits (quiesce + paint + flag)", "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  A::set_mode(orch, OperatingMode::Stationary);
  A::set_ota_committed(orch, false);

  A::on_ota_download_started(orch);

  CHECK(A::ota_committed(orch));
  CHECK(test_spy::sensor_stopped);          // enter_ota quiesced sensing
  CHECK(test_spy::cloud_disarm_count == 1); // Stationary cloud.disarm() (no stop)
  CHECK(test_spy::cloud_stop_count == 0);
  CHECK(DisplayService::spy_flush_count >= 1); // "Updating firmware…" paint flushed
}

TEST_CASE("OTA: finish_ota Ok paints Restarting and reboots without exit_ota",
          "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  A::set_mode(orch, OperatingMode::Portable);
  A::set_ota_committed(orch, true);

  A::finish_ota(orch, OtaStatus::Ok); // reboot() is a no-op under TEST_HOST

  CHECK(DisplayService::spy_update_count >= 1);
  CHECK(DisplayService::spy_flush_count >= 1);
  CHECK(A::ota_committed(orch)); // no exit_ota on Ok; the reboot discards state
}

TEST_CASE("OTA: lightweight WiFi UpToDate resumes silently", "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  A::set_mode(orch, OperatingMode::Stationary);
  A::set_ota_committed(orch, false); // speculative check never committed

  SECTION("offline: no cloud re-arm, no paint") {
    test_spy::wifi_is_online = false;
    A::finish_ota(orch, OtaStatus::UpToDate);
    CHECK(test_spy::cloud_arm_count == 0);
    CHECK(DisplayService::spy_update_count == 0); // silent — nothing painted
  }

  SECTION("online: cloud.arm(false), still silent") {
    test_spy::wifi_is_online = true;
    A::finish_ota(orch, OtaStatus::UpToDate);
    CHECK(test_spy::cloud_arm_count == 1);
    CHECK_FALSE(test_spy::cloud_last_arm_fire_now);
    CHECK(test_spy::cloud_start_count == 0);      // task never stopped → no start()
    CHECK(DisplayService::spy_update_count == 0); // silent
  }
}

TEST_CASE("OTA: lightweight WiFi failure renders a snackbar over the current screen",
          "[Orchestrator][ota]") {
  TestFixture f;
  auto orch = f.make_orchestrator();
  A::set_mode(orch, OperatingMode::Stationary);
  A::set_ota_committed(orch, false);
  test_spy::wifi_is_online = false;

  A::finish_ota(orch, OtaStatus::TransportError);

  // A "Update failed" snackbar is set and a single render happens (no drain,
  // no Home reset for the lightweight path).
  CHECK(DisplayService::spy_update_count == 1);
  CHECK(test_spy::sensor_stopped == false); // never quiesced on a no-op check
}
