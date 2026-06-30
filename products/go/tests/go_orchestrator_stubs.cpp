/**
 * AirGradient Go — Orchestrator test stubs
 *
 * Provides observable stub implementations for all services the orchestrator
 * calls.  Each stub records its calls into the test_spy namespace so tests
 * can assert on orchestrator–service interactions without constructing real
 * hardware-backed service objects.
 *
 * The stubs replace the real .cpp files at link time — only the orchestrator
 * .cpp (the code under test) and UIManager .cpp (real, pure-logic) are
 * compiled from production sources.
 */

#include "go_ble.h"
#include "go_cloud.h"
#include "go_display.h"
#include "gps/gps_service.h"
#include "go_input.h"
#include "go_ota.h"
#include "go_portable_provisioner.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_storage.h"
#include "go_ulp.h"
#include "services/ag_client.h"
#include "go_wifi.h"

#include <algorithm>
#include <cstring>
#include <set>

// ============================================================================
// test_spy — observable state written by stubs, read by test assertions
// ============================================================================

namespace test_spy {

// --- SensorProducer ---
bool sensor_started = false;
bool sensor_stopped = false;
bool measurement_requested = false;
uint8_t last_iterations = 0;
SensorGroup last_groups = SensorGroup::None;
bool co2_calibration_requested = false;
bool prepare_requested = false;

// --- GpsService ---
bool gps_started = false;
bool gps_stopped = false;
bool gps_stop_and_idle_called = false;
bool gps_idle_called = false;
int gps_posting_interval_ms = 0;
bool gps_aiding_set = false;
GpsAidingData gps_aiding_data{};

// --- InputService ---
bool input_started = false;
bool input_stopped = false;

// --- StorageService ---
bool cache_measurement_called = false;
MeasuresAGo last_cached_measurement{};
bool route_started = false; ///< set when create_route() runs successfully
bool route_resumed = false; ///< set when resume_route() runs successfully
uint32_t route_session_id = 0;
bool route_point_appended = false;
RoutePoint last_route_point{};
bool route_ended = false;
// Models the real _route_file != nullptr state.  Survives reset() so that
// end_route() after reset() still behaves like the real implementation.
bool route_file_open = false;
bool cache_backed_up = false;
bool cache_restored = false;
bool cache_cleared = false;
bool routes_cleared = false;
bool clear_routes_result = true;

// Controls for failure-injection in start_tracking / resume / append flows.
// Default = success; tests set these false to exercise the inline failure
// surfaces (storage-error snackbar, BLE notify_tracking_status with tracking=false,
// BLE command-result with flash_error).
bool create_route_result = true;
bool resume_route_result = true;
bool append_route_point_result = true;

// Session IDs that should appear "already on NAND" to the orchestrator's
// session-ID collision retry probe. Tests pre-populate this set to force
// generate_session_id() to retry; the orchestrator's bounded loop then
// either lands on a free slot or exhausts the budget.
std::set<uint32_t> existing_route_session_ids;

// --- BleService ---
bool ble_init_called = false;
bool ble_deinit_called = false;
bool ble_initialized = false;
bool ble_connected = false;
bool ble_authenticated = false;
bool ble_notify_measures_called = false;
bool ble_update_status_called = false;
bool ble_notify_tracking_status_called = false;
uint32_t ble_notify_tracking_status_count = 0;
bool ble_notify_charging_status_called = false;
uint32_t ble_notify_charging_status_count = 0;
bool ble_notify_disconnect_called = false;
uint32_t ble_notify_disconnect_count = 0;
BleDiscReason ble_last_disc_reason = BleDiscReason::User;
bool ble_last_status_tracking = false;
uint32_t ble_last_status_session = 0;
bool ble_update_config_called = false;
bool ble_notify_config_called = false;
bool ble_notify_command_progress_called = false;
BleCommand ble_progress_command = BleCommand::Unknown;
bool ble_notify_command_result_called = false;
BleCommand ble_last_command = BleCommand::Unknown;
bool ble_last_command_success = false;
const char *ble_last_command_error = nullptr;
bool ble_delete_all_bonds_called = false;
bool ble_delete_all_bonds_result = true;
bool ble_disconnect_observer_set = false; ///< last set_disconnect_observer() arg was non-null
uint32_t ble_disconnect_observer_set_count = 0;
bool ble_history_list_called = false;
bool ble_history_start_called = false;
uint32_t ble_history_start_session = 0;
bool ble_history_fill_called = false;
bool ble_history_end_called = false;
bool ble_history_delete_called = false;
uint32_t ble_history_delete_session = 0;
bool ble_notify_history_error_called = false;
const char *ble_last_history_error = nullptr;
size_t ble_pending_config_len = 0;
size_t ble_pending_history_len = 0;
BleConfigDecodeResult ble_config_decode_result{};
bool ble_decode_updates_settings = false;
GoSettings ble_decoded_settings{};
BleHistoryDecodeResult ble_history_decode_result{};

// --- CloudService ---
uint32_t cloud_start_count = 0;
bool cloud_start_result = true;
uint32_t cloud_stop_count = 0;
uint32_t cloud_arm_count = 0;
bool cloud_last_arm_fire_now = false;
uint32_t cloud_disarm_count = 0;
uint32_t cloud_set_disable_count = 0;
bool cloud_last_disable_cloud = false;
uint32_t cloud_snapshot_count = 0;
MeasuresAGo cloud_last_snapshot{};

// --- WifiService ---
bool wifi_has_saved_networks = false;
bool wifi_connect_saved_called = false;
WifiStaticIpConfig wifi_last_static_ip{};
bool wifi_static_ip_was_null = false;
bool wifi_try_fallback_called = false;
bool wifi_shutdown_called = false;
bool wifi_clear_credentials_called = false;
bool wifi_start_provisioning_called = false;
ProvisioningTransport wifi_start_provisioning_transport = ProvisioningTransport::BleOnly;
bool wifi_switch_transport_called = false;
bool wifi_stop_provisioning_called = false;
bool wifi_tick_called = false;
uint32_t wifi_next_deadline_ms = 0;
bool wifi_is_online = false;
bool wifi_has_been_online = false;
bool wifi_schedule_reconnect_called = false;
int wifi_schedule_reconnect_count = 0;

// --- PortableWifiProvisioner ---
bool portable_attach_called = false;
bool portable_attach_result = true;
bool portable_attached = false;
bool portable_stop_called = false;
bool portable_handle_pending_called = false;
bool portable_on_connected_called = false;
bool portable_on_ble_disconnected_called = false;
bool portable_is_radio_active = false;
uint32_t portable_next_deadline_ms = 0;
bool portable_tick_called = false;

// --- OtaService ---
bool ota_setup_ble_called = false;
bool ota_setup_ble_result = true;
bool ota_teardown_ble_called = false;
uint32_t ota_teardown_ble_count = 0;
bool ota_handle_disconnect_called = false;
bool ota_is_ble_active = false;
uint32_t ota_run_ble_count = 0;
OtaStatus ota_run_ble_result = OtaStatus::Ok;
uint32_t ota_run_wifi_count = 0;
OtaStatus ota_run_wifi_result = OtaStatus::UpToDate;
// When true, run_wifi_check() invokes the passed on_download_started callback
// (simulating the first Downloading tick on a real image pull).
bool ota_run_wifi_invoke_download_started = false;

// --- PowerService ---
bool bms_polled = false;
uint32_t bms_poll_count = 0;
bool shutdown_called = false;
bool state_saved = false;
RtcAppState last_saved_state{};
RtcAppState state_to_load{};        // tests set this before init(Button)
PowerSnapshot snapshot_to_return{}; // tests set this before poll_bms
PowerService::SleepType sleep_type_to_return = PowerService::SleepType::None;
bool pm_power_set = false;
bool pm_power_on = false;
uint32_t pm_power_set_count = 0;
bool pm_sleep_requested = false;
bool ensure_pmid_healthy_called = false;
uint32_t ensure_pmid_healthy_count = 0;

void reset() {
  sensor_started = false;
  sensor_stopped = false;
  measurement_requested = false;
  last_iterations = 0;
  co2_calibration_requested = false;
  prepare_requested = false;

  gps_started = false;
  gps_stopped = false;
  gps_stop_and_idle_called = false;
  gps_idle_called = false;
  gps_posting_interval_ms = 0;
  gps_aiding_set = false;
  gps_aiding_data = GpsAidingData{};

  input_started = false;
  input_stopped = false;

  cache_measurement_called = false;
  last_cached_measurement = MeasuresAGo{};
  route_started = false;
  route_resumed = false;
  route_session_id = 0;
  route_point_appended = false;
  last_route_point = RoutePoint{};
  route_ended = false;
  cache_backed_up = false;
  cache_restored = false;
  cache_cleared = false;
  routes_cleared = false;
  clear_routes_result = true;
  create_route_result = true;
  resume_route_result = true;
  append_route_point_result = true;
  existing_route_session_ids.clear();

  ble_init_called = false;
  ble_deinit_called = false;
  ble_initialized = false;
  ble_connected = false;
  ble_authenticated = false;
  ble_notify_measures_called = false;
  ble_update_status_called = false;
  ble_notify_tracking_status_called = false;
  ble_notify_tracking_status_count = 0;
  ble_notify_charging_status_called = false;
  ble_notify_charging_status_count = 0;
  ble_notify_disconnect_called = false;
  ble_notify_disconnect_count = 0;
  ble_last_disc_reason = BleDiscReason::User;
  ble_last_status_tracking = false;
  ble_last_status_session = 0;
  ble_update_config_called = false;
  ble_notify_config_called = false;
  ble_notify_command_progress_called = false;
  ble_progress_command = BleCommand::Unknown;
  ble_notify_command_result_called = false;
  ble_last_command = BleCommand::Unknown;
  ble_last_command_success = false;
  ble_last_command_error = nullptr;
  ble_delete_all_bonds_called = false;
  ble_delete_all_bonds_result = true;
  ble_disconnect_observer_set = false;
  ble_disconnect_observer_set_count = 0;
  ble_history_list_called = false;
  ble_history_start_called = false;
  ble_history_start_session = 0;
  ble_history_fill_called = false;
  ble_history_end_called = false;
  ble_history_delete_called = false;
  ble_history_delete_session = 0;
  ble_notify_history_error_called = false;
  ble_last_history_error = nullptr;
  ble_pending_config_len = 0;
  ble_pending_history_len = 0;
  ble_config_decode_result = BleConfigDecodeResult{};
  ble_decode_updates_settings = false;
  ble_decoded_settings = GoSettings{};
  ble_history_decode_result = BleHistoryDecodeResult{};

  cloud_start_count = 0;
  cloud_start_result = true;
  cloud_stop_count = 0;
  cloud_arm_count = 0;
  cloud_last_arm_fire_now = false;
  cloud_disarm_count = 0;
  cloud_set_disable_count = 0;
  cloud_last_disable_cloud = false;
  cloud_snapshot_count = 0;
  cloud_last_snapshot = MeasuresAGo{};

  wifi_has_saved_networks = false;
  wifi_connect_saved_called = false;
  wifi_last_static_ip = WifiStaticIpConfig{};
  wifi_static_ip_was_null = false;
  wifi_try_fallback_called = false;
  wifi_shutdown_called = false;
  wifi_clear_credentials_called = false;
  wifi_start_provisioning_called = false;
  wifi_start_provisioning_transport = ProvisioningTransport::BleOnly;
  wifi_switch_transport_called = false;
  wifi_stop_provisioning_called = false;
  wifi_tick_called = false;
  wifi_next_deadline_ms = 0;
  wifi_is_online = false;
  wifi_has_been_online = false;
  wifi_schedule_reconnect_called = false;
  wifi_schedule_reconnect_count = 0;

  portable_attach_called = false;
  portable_attach_result = true;
  portable_attached = false;
  portable_stop_called = false;
  portable_handle_pending_called = false;
  portable_on_connected_called = false;
  portable_on_ble_disconnected_called = false;
  portable_is_radio_active = false;
  portable_next_deadline_ms = 0;
  portable_tick_called = false;

  ota_setup_ble_called = false;
  ota_setup_ble_result = true;
  ota_teardown_ble_called = false;
  ota_teardown_ble_count = 0;
  ota_handle_disconnect_called = false;
  ota_is_ble_active = false;
  ota_run_ble_count = 0;
  ota_run_ble_result = OtaStatus::Ok;
  ota_run_wifi_count = 0;
  ota_run_wifi_result = OtaStatus::UpToDate;
  ota_run_wifi_invoke_download_started = false;

  bms_polled = false;
  bms_poll_count = 0;
  shutdown_called = false;
  state_saved = false;
  last_saved_state = RtcAppState{};
  state_to_load = RtcAppState{};
  snapshot_to_return = PowerSnapshot{};
  sleep_type_to_return = PowerService::SleepType::None;
  pm_power_set = false;
  pm_power_on = false;
  pm_power_set_count = 0;
  pm_sleep_requested = false;
  ensure_pmid_healthy_called = false;
  ensure_pmid_healthy_count = 0;

  DisplayService::spy_deep_sleep_called = false;
  DisplayService::spy_update_count = 0;
  DisplayService::spy_flush_count = 0;
}

} // namespace test_spy

// ============================================================================
// SensorProducer stubs
// ============================================================================

SensorProducer::SensorProducer(SensorManager &manager, RtosQueueHandle event_queue,
                               const Config &config)
    : _manager(manager), _event_queue(event_queue), _config(config) {}

bool SensorProducer::start() {
  test_spy::sensor_started = true;
  return true;
}

void SensorProducer::stop() { test_spy::sensor_stopped = true; }

void SensorProducer::request_measurement(uint8_t iterations, SensorGroup groups) {
  test_spy::measurement_requested = true;
  test_spy::last_iterations = iterations;
  test_spy::last_groups = groups;
}

void SensorProducer::request_co2_calibration() { test_spy::co2_calibration_requested = true; }

void SensorProducer::request_prepare() { test_spy::prepare_requested = true; }

void SensorProducer::request_pm_sleep() { test_spy::pm_sleep_requested = true; }

// ============================================================================
// GpsService stubs
// ============================================================================

GpsService::GpsService(GpsDriver &driver, RtosQueueHandle event_queue, const Config &config)
    : _driver(driver), _event_queue(event_queue), _config(config) {}

GpsService::~GpsService() = default;

bool GpsService::start() {
  test_spy::gps_started = true;
  return true;
}

void GpsService::stop() {
  test_spy::gps_stopped = true;
  _driver.end();
}

void GpsService::stop_and_idle_gnss() { test_spy::gps_stop_and_idle_called = true; }

void GpsService::idle_gnss() { test_spy::gps_idle_called = true; }

GpsData GpsService::get_latest_fix() const { return GpsData{}; }

void GpsService::set_posting_interval_ms(int interval_ms) {
  test_spy::gps_posting_interval_ms = interval_ms;
}

void GpsService::set_aiding_data(const GpsAidingData &data) {
  test_spy::gps_aiding_set = true;
  test_spy::gps_aiding_data = data;
}

GpsData gps_read_once(GpsDriver & /*driver*/, int /*baud_rate*/, uint32_t /*timeout_ms*/,
                      const volatile bool & /*abort*/) {
  return GpsData{};
}

// ============================================================================
// InputService stubs
// ============================================================================

InputService::InputService(CapTouchSensor &touch, const gpio::Hal &gpio,
                           RtosQueueHandle event_queue, const Config &config)
    : _touch(touch), _gpio(gpio), _event_queue(event_queue), _config(config) {}

InputService::~InputService() = default;

bool InputService::start() {
  test_spy::input_started = true;
  return true;
}

void InputService::stop() { test_spy::input_stopped = true; }

void InputService::pause() {}
void InputService::resume() {}

// ============================================================================
// StorageService stubs
// ============================================================================

StorageService::StorageService(PayloadCache &cache, NandStorage &nand)
    : _cache(cache), _nand(nand) {}

bool StorageService::init() { return true; }

void StorageService::cache_measurement(const MeasuresAGo &m) {
  test_spy::cache_measurement_called = true;
  test_spy::last_cached_measurement = m;
}

uint16_t StorageService::read_cached_field(CacheField /*field*/, float * /*out*/,
                                           uint16_t /*max_count*/) const {
  return 0;
}

uint16_t StorageService::read_cache(MeasuresAGo * /*out*/, uint16_t /*max_count*/) const {
  return 0;
}

uint16_t StorageService::cached_count() const { return 0; }

void StorageService::backup_cache() const { test_spy::cache_backed_up = true; }

void StorageService::restore_cache() { test_spy::cache_restored = true; }

void StorageService::clear_cache() { test_spy::cache_cleared = true; }

bool StorageService::create_route(uint32_t session_id) {
  if (!test_spy::create_route_result) {
    return false;
  }
  test_spy::route_started = true;
  test_spy::route_file_open = true;
  test_spy::route_session_id = session_id;
  return true;
}

bool StorageService::resume_route(uint32_t session_id) {
  if (!test_spy::resume_route_result) {
    return false;
  }
  test_spy::route_resumed = true;
  test_spy::route_file_open = true;
  test_spy::route_session_id = session_id;
  return true;
}

bool StorageService::route_file_exists(uint32_t session_id) const {
  return test_spy::existing_route_session_ids.count(session_id) > 0;
}

bool StorageService::append_route_point(const RoutePoint &point) {
  test_spy::route_point_appended = true;
  test_spy::last_route_point = point;
  return test_spy::append_route_point_result;
}

void StorageService::end_route() {
  if (!test_spy::route_file_open) {
    return; // no-op, same as real end_route() when _route_file == nullptr
  }
  test_spy::route_file_open = false;
  test_spy::route_ended = true;
}

bool StorageService::is_route_active() const { return test_spy::route_file_open; }

uint32_t StorageService::current_route_point_count() const { return 0; }

bool StorageService::delete_route(uint32_t /*session_id*/) { return true; }

uint32_t StorageService::current_route_session_id() const { return test_spy::route_session_id; }

bool StorageService::clear_routes() {
  test_spy::routes_cleared = true;
  return test_spy::clear_routes_result;
}

// ============================================================================
// PowerService stubs
// ============================================================================

PowerService::PowerService(BmsDevice &bms, const gpio::Hal &gpio, const Config &config)
    : _bms(bms), _gpio(gpio), _config(config) {}

void PowerService::set_fuel_gauge(FuelGaugeDevice * /*fg*/) {}

PowerSnapshot PowerService::poll_bms(bool /*pm_invalid_hint*/) {
  test_spy::bms_polled = true;
  ++test_spy::bms_poll_count;
  return test_spy::snapshot_to_return;
}

bool PowerService::poll_charging_status(BmsChargingState &state) {
  state = test_spy::snapshot_to_return.charging_status;
  return true;
}

bool PowerService::poll_status(BmsStatus &status) {
  status = test_spy::snapshot_to_return.charger_status;
  if (status.charging_state == BmsChargingState::Unknown) {
    status.charging_state = test_spy::snapshot_to_return.charging_status;
  }
  return true;
}

bool PowerService::rekick_pmid_if_collapsed(const BmsTelemetry & /*t*/, BmsPowerSource /*src*/) {
  return false;
}

bool PowerService::ensure_pmid_healthy() {
  test_spy::ensure_pmid_healthy_called = true;
  ++test_spy::ensure_pmid_healthy_count;
  return false;
}

bool PowerService::reset_watchdog() { return true; }

void PowerService::shutdown() { test_spy::shutdown_called = true; }

bool PowerService::set_watchdog_timeout_ms(uint32_t /*timeout_ms*/) { return true; }

void PowerService::save_state(const RtcAppState &state) {
  test_spy::state_saved = true;
  test_spy::last_saved_state = state;
}

RtcAppState PowerService::load_state() const { return test_spy::state_to_load; }

PowerService::SleepDecision PowerService::decide_sleep(const GoSettings & /*settings*/,
                                                       LockState /*lock_state*/,
                                                       OperatingMode /*mode*/,
                                                       uint32_t /*awake_ms*/) const {
  return {test_spy::sleep_type_to_return, 10000};
}

bool PowerService::should_hold_pm_sensor(uint32_t sleep_duration_ms) const {
  return _config.pin_pm_power >= 0 && sleep_duration_ms < _config.sensor_hold_max_sleep_ms;
}

bool PowerService::should_sleep_pm_sensor(uint32_t measure_interval_ms) const {
  return _config.pin_pm_power >= 0 && measure_interval_ms >= _config.pm_sleep_threshold_ms;
}

void PowerService::set_pm_power(bool on) {
  test_spy::pm_power_set = true;
  test_spy::pm_power_on = on;
  ++test_spy::pm_power_set_count;
}

void PowerService::enter_sleep(uint32_t /*sleep_duration_ms*/) {}

WakeCause PowerService::get_wake_cause() { return WakeCause::PowerOn; }

bool PowerService::is_fast_path_wake(WakeCause /*cause*/, const RtcAppState & /*state*/) {
  return false;
}

void PowerService::release_sleep_gpio_holds(int /*pin_pm_power*/) {}

void PowerService::init_ext_watchdog() {}

void PowerService::reset_ext_watchdog() {}

// ============================================================================
// Free functions from go_power.h
// ============================================================================

RtcAppState load_rtc_app_state() { return test_spy::state_to_load; }

// ============================================================================
// Private stubs that are never called but must link
// ============================================================================

void PowerService::configure_wake_sources(uint32_t /*timer_ms*/) {}

bool StorageService::ensure_route_dir() const { return true; }

// SensorProducer private methods
void SensorProducer::task_entry(void * /*arg*/) {}
void SensorProducer::run() {}
void SensorProducer::handle_calibration() {}
void SensorProducer::handle_prepare() {}
void SensorProducer::handle_measurement(uint32_t /*notify_value*/) {}
void SensorProducer::handle_sampler_tick() {}

// GpsService private methods
void GpsService::task_entry(void * /*arg*/) {}
void GpsService::run() {}
void GpsService::update_latest_fix(const GpsData & /*data*/) {}
void GpsService::post_fix_event() {}
void GpsService::sync_system_clock(const GpsTimestamp & /*ts*/) {}

// InputService private methods
void InputService::process_touch_interrupt() {}
void InputService::process_button_event(InputSource /*source*/, uint64_t /*timestamp_ms*/) {}
void InputService::check_pending_long_press() {}
uint32_t InputService::compute_queue_timeout_ms() const { return UINT32_MAX; }
void InputService::post_input_event(InputSource /*source*/, InputType /*type*/) {}
void InputService::cap_int_isr(void * /*arg*/) {}
void InputService::button_power_isr(void * /*arg*/) {}
void InputService::button_boot_isr(void * /*arg*/) {}
void InputService::task_entry(void * /*arg*/) {}
void InputService::run() {}
int InputService::pin_for_button_index(int /*idx*/) const { return -1; }

// ============================================================================
// BleService stubs
// ============================================================================

BleService::BleService(RtosQueueHandle /*event_queue*/, StorageService &storage,
                       AgBleServer & /*ble_server*/)
    : _event_queue(nullptr), _storage(storage), _server(nullptr) {}

bool BleService::init(const char * /*serial*/) {
  test_spy::ble_init_called = true;
  test_spy::ble_initialized = true;
  return true;
}

bool BleService::init_stack_and_register(const char * /*serial*/) {
  test_spy::ble_init_called = true;
  return true;
}

bool BleService::start_advertising() {
  test_spy::ble_initialized = true;
  return true;
}

void BleService::deinit() {
  test_spy::ble_deinit_called = true;
  test_spy::ble_initialized = false;
  test_spy::ble_connected = false;
  test_spy::ble_authenticated = false;
}

void BleService::set_disconnect_observer(AgBleDisconnectCallback cb) {
  test_spy::ble_disconnect_observer_set = static_cast<bool>(cb);
  ++test_spy::ble_disconnect_observer_set_count;
}

bool BleService::is_initialized() const { return test_spy::ble_initialized; }

bool BleService::is_connected() const { return test_spy::ble_connected; }

bool BleService::is_authenticated() const { return test_spy::ble_authenticated; }

void BleService::notify_measures(const MeasuresAGo & /*m*/, const GpsData & /*gps*/,
                                 time_t /*ts*/) {
  test_spy::ble_notify_measures_called = true;
}

void BleService::update_status(const PowerSnapshot & /*power*/, const GpsData & /*gps*/,
                               bool tracking, uint32_t session_id) {
  test_spy::ble_update_status_called = true;
  test_spy::ble_last_status_tracking = tracking;
  test_spy::ble_last_status_session = session_id;
}

void BleService::notify_tracking_status(const PowerSnapshot & /*power*/, const GpsData & /*gps*/,
                                        bool tracking, uint32_t session_id) {
  test_spy::ble_notify_tracking_status_called = true;
  ++test_spy::ble_notify_tracking_status_count;
  test_spy::ble_last_status_tracking = tracking;
  test_spy::ble_last_status_session = session_id;
}

void BleService::notify_charging_status(const PowerSnapshot & /*power*/, const GpsData & /*gps*/,
                                        bool tracking, uint32_t session_id) {
  test_spy::ble_notify_charging_status_called = true;
  ++test_spy::ble_notify_charging_status_count;
  test_spy::ble_last_status_tracking = tracking;
  test_spy::ble_last_status_session = session_id;
}

void BleService::notify_disconnect(BleDiscReason reason) {
  test_spy::ble_notify_disconnect_called = true;
  ++test_spy::ble_notify_disconnect_count;
  test_spy::ble_last_disc_reason = reason;
}

void BleService::update_config(const GoSettings & /*settings*/) {
  test_spy::ble_update_config_called = true;
}

void BleService::notify_config(const GoSettings & /*prev*/, const GoSettings & /*cur*/) {
  test_spy::ble_notify_config_called = true;
}

void BleService::notify_command_progress(BleCommand cmd) {
  test_spy::ble_notify_command_progress_called = true;
  test_spy::ble_progress_command = cmd;
}

void BleService::notify_command_result(BleCommand cmd, bool success, const char *error) {
  test_spy::ble_notify_command_result_called = true;
  test_spy::ble_last_command = cmd;
  test_spy::ble_last_command_success = success;
  test_spy::ble_last_command_error = error;
}

bool BleService::delete_all_bonds() {
  test_spy::ble_delete_all_bonds_called = true;
  return test_spy::ble_delete_all_bonds_result;
}

size_t BleService::take_pending_config_write(uint8_t * /*buf*/, size_t /*buf_size*/) {
  return test_spy::ble_pending_config_len;
}

size_t BleService::take_pending_history_write(uint8_t * /*buf*/, size_t /*buf_size*/) {
  return test_spy::ble_pending_history_len;
}

void BleService::handle_history_list() { test_spy::ble_history_list_called = true; }

void BleService::handle_history_start(uint32_t session_id) {
  test_spy::ble_history_start_called = true;
  test_spy::ble_history_start_session = session_id;
}

void BleService::handle_history_fill(const uint32_t * /*indices*/, size_t /*count*/) {
  test_spy::ble_history_fill_called = true;
}

void BleService::handle_history_end() { test_spy::ble_history_end_called = true; }

void BleService::handle_history_delete(uint32_t session_id) {
  test_spy::ble_history_delete_called = true;
  test_spy::ble_history_delete_session = session_id;
}

void BleService::notify_history_error(const char *err) {
  test_spy::ble_notify_history_error_called = true;
  test_spy::ble_last_history_error = err;
}

BleConfigDecodeResult BleService::decode_config_write(const uint8_t * /*buf*/, size_t /*len*/,
                                                      GoSettings &settings) {
  if (test_spy::ble_decode_updates_settings) {
    settings = test_spy::ble_decoded_settings;
  }
  return test_spy::ble_config_decode_result;
}

BleHistoryDecodeResult BleService::decode_history_write(const uint8_t * /*buf*/, size_t /*len*/) {
  return test_spy::ble_history_decode_result;
}

// BleService private methods (never called in orchestrator tests)
void BleService::on_connect(uint16_t /*handle*/) {}
void BleService::on_disconnect(uint16_t /*handle*/, int /*reason*/) {}
void BleService::on_config_write(const uint8_t * /*data*/, size_t /*len*/) {}
void BleService::on_history_write(const uint8_t * /*data*/, size_t /*len*/) {}
void BleService::on_passkey_request(uint32_t /*passkey*/) {}
bool BleService::send_history_cbor(const uint8_t * /*data*/, size_t /*len*/) { return false; }
bool BleService::send_history_binary(uint16_t /*idx*/, const uint8_t * /*data*/, size_t /*len*/) {
  return false;
}
void BleService::route_point_to_wire(const RoutePoint & /*point*/, uint8_t * /*out*/) {}
size_t BleService::encode_measures(uint8_t * /*buf*/, size_t /*sz*/, const MeasuresAGo & /*m*/,
                                   const GpsData & /*gps*/, time_t /*ts*/) {
  return 0;
}
size_t BleService::encode_status(uint8_t * /*buf*/, size_t /*sz*/, const PowerSnapshot & /*p*/,
                                 const GpsData & /*g*/, bool /*t*/, uint32_t /*s*/) {
  return 0;
}
size_t BleService::encode_config(uint8_t * /*buf*/, size_t /*sz*/, const GoSettings & /*s*/) {
  return 0;
}
const char *BleService::charging_state_to_str(BmsChargingState /*s*/) { return "unknown"; }
const char *BleService::gps_mode_to_str(GpsMode /*m*/) { return "tracking"; }
const char *BleService::operating_mode_to_str(OperatingMode /*m*/) { return "offline"; }

// ============================================================================
// ULP stubs (go_ulp.h — LP Core not available on host)
// ============================================================================

void ulp_wdt_start() {}
void ulp_wdt_stop() {}

// StorageService read methods (declared in go_storage.h, stubbed for linker)
uint16_t StorageService::session_count() const { return 0; }
uint16_t StorageService::list_sessions(uint32_t * /*out*/, uint16_t /*max*/) const { return 0; }
uint32_t StorageService::get_session_point_count(uint32_t /*id*/) const { return 0; }
uint16_t StorageService::read_route_points(uint32_t /*id*/, uint32_t /*off*/, RoutePoint * /*out*/,
                                           uint16_t /*cnt*/) const {
  return 0;
}
time_t StorageService::get_session_start_time(uint32_t /*id*/) const { return 0; }
uint32_t StorageService::total_capacity_kb() const { return 0; }
uint32_t StorageService::used_kb() const { return 0; }

// ============================================================================
// WifiService stubs
// ============================================================================

WifiService::WifiService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg)
    : _event_queue(event_queue), _wifi(deps.wifi), _ble(deps.ble), _http(deps.http), _cfg(cfg) {}

WifiService::~WifiService() = default;

bool WifiService::has_saved_networks() const { return test_spy::wifi_has_saved_networks; }

void WifiService::connect_with_saved_credentials(const WifiStaticIpConfig *static_ip) {
  test_spy::wifi_connect_saved_called = true;
  if (static_ip != nullptr) {
    test_spy::wifi_last_static_ip = *static_ip;
    test_spy::wifi_static_ip_was_null = false;
  } else {
    test_spy::wifi_last_static_ip = WifiStaticIpConfig{};
    test_spy::wifi_static_ip_was_null = true;
  }
}

void WifiService::schedule_reconnect(const WifiStaticIpConfig *static_ip) {
  test_spy::wifi_schedule_reconnect_called = true;
  test_spy::wifi_schedule_reconnect_count += 1;
  if (static_ip != nullptr) {
    test_spy::wifi_last_static_ip = *static_ip;
    test_spy::wifi_static_ip_was_null = false;
  } else {
    test_spy::wifi_last_static_ip = WifiStaticIpConfig{};
    test_spy::wifi_static_ip_was_null = true;
  }
}

void WifiService::try_default_fallback_credentials() { test_spy::wifi_try_fallback_called = true; }

void WifiService::start_provisioning(ProvisioningTransport t) {
  test_spy::wifi_start_provisioning_called = true;
  test_spy::wifi_start_provisioning_transport = t;
}

void WifiService::switch_provisioning_transport() { test_spy::wifi_switch_transport_called = true; }

void WifiService::stop_provisioning() { test_spy::wifi_stop_provisioning_called = true; }

void WifiService::shutdown() { test_spy::wifi_shutdown_called = true; }

void WifiService::clear_credentials() { test_spy::wifi_clear_credentials_called = true; }

bool WifiService::is_online() const { return test_spy::wifi_is_online; }
bool WifiService::is_connecting() const { return false; }
bool WifiService::is_provisioning() const { return false; }
ProvisioningTransport WifiService::current_transport() const {
  return ProvisioningTransport::BleOnly;
}
uint32_t WifiService::ip() const { return 0; }
int WifiService::rssi() const { return WIFI_RSSI_INVALID; }
WifiDisconnectReason WifiService::last_disconnect_reason() const {
  return WifiDisconnectReason::unknown;
}
bool WifiService::has_been_online() const { return test_spy::wifi_has_been_online; }
uint32_t WifiService::next_deadline_ms() const { return test_spy::wifi_next_deadline_ms; }
void WifiService::tick(uint32_t /*now_ms*/) { test_spy::wifi_tick_called = true; }

// Private — never called via orchestrator stubs
void WifiService::_install_wifi_callbacks() {}
void WifiService::_detach_wifi_callbacks() {}
void WifiService::_on_got_ip(uint32_t /*ip*/) {}
void WifiService::_on_disconnected(WifiDisconnectReason /*r*/) {}
void WifiService::_reset_deadline() {}
void WifiService::_arm_deadline(uint32_t /*window_ms*/) {}
void WifiService::_reset_online_latches() {}
void WifiService::_post_wifi_disconnected(WifiDisconnectReason /*r*/) {}

// ============================================================================
// PortableWifiProvisioner stubs
// ============================================================================

PortableWifiProvisioner::PortableWifiProvisioner(RtosQueueHandle event_queue, const Deps &deps,
                                                 const Config &cfg)
    : _event_queue(event_queue), _wifi(deps.wifi), _ble(deps.ble), _board(deps.board), _cfg(cfg) {}

PortableWifiProvisioner::~PortableWifiProvisioner() = default;

bool PortableWifiProvisioner::attach() {
  test_spy::portable_attach_called = true;
  test_spy::portable_attached = test_spy::portable_attach_result;
  return test_spy::portable_attach_result;
}

void PortableWifiProvisioner::stop() {
  test_spy::portable_stop_called = true;
  test_spy::portable_attached = false;
}

void PortableWifiProvisioner::handle_pending_request() {
  test_spy::portable_handle_pending_called = true;
}

void PortableWifiProvisioner::on_connected() { test_spy::portable_on_connected_called = true; }

void PortableWifiProvisioner::on_ble_disconnected() {
  test_spy::portable_on_ble_disconnected_called = true;
}

bool PortableWifiProvisioner::is_radio_active() const { return test_spy::portable_is_radio_active; }

bool PortableWifiProvisioner::is_attached() const { return test_spy::portable_attached; }

uint32_t PortableWifiProvisioner::next_deadline_ms() const {
  return test_spy::portable_next_deadline_ms;
}

void PortableWifiProvisioner::tick(uint32_t /*now_ms*/) { test_spy::portable_tick_called = true; }

// Private — never called via orchestrator stubs.
void PortableWifiProvisioner::_on_attached_request(const AttachedRequest & /*req*/) {}
void PortableWifiProvisioner::_on_provisioning_event(const ProvisioningEventInfo & /*info*/) {}
bool PortableWifiProvisioner::_ensure_wifi_ready() { return true; }
void PortableWifiProvisioner::_drop_radio() {}
void PortableWifiProvisioner::_arm_idle_timer() {}
void PortableWifiProvisioner::_cancel_idle_timer() {}

// ============================================================================
// AgClient stubs (concrete class — no virtuals; link-time replacement)
// ============================================================================

bool AgClient::begin(const char * /*serial_number*/, NetworkType /*network*/,
                     CellularModem * /*modem*/) {
  return true;
}

AgClientResult AgClient::http_fetch_config(char * /*config_out*/, size_t /*config_size*/,
                                           size_t *bytes_written) {
  if (bytes_written != nullptr) {
    *bytes_written = 0;
  }
  return AgClientResult::Ok;
}

AgClientResult AgClient::http_post_measures(const Measures & /*measures*/, int /*signal*/) {
  return AgClientResult::Ok;
}

AgClientResult AgClient::http_post_measures(const MeasuresBasic & /*measures*/, int /*signal*/) {
  return AgClientResult::Ok;
}

AgClientResult AgClient::http_post_measures(const MeasuresAGo & /*measures*/, int /*signal*/) {
  return AgClientResult::Ok;
}

// ============================================================================
// CloudService stubs — orchestrator wiring only.  The real cloud-task logic
// is exercised in go_cloud.tests.cpp against a separate test_support library.
// ============================================================================

CloudService::CloudService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg)
    : _event_queue(event_queue), _client(deps.client), _wifi(deps.wifi), _cfg(cfg),
      _disable_cloud(cfg.disable_cloud) {}

CloudService::~CloudService() = default;

bool CloudService::start() {
  ++test_spy::cloud_start_count;
  return test_spy::cloud_start_result;
}

void CloudService::stop() { ++test_spy::cloud_stop_count; }

void CloudService::arm(bool fire_now) {
  ++test_spy::cloud_arm_count;
  test_spy::cloud_last_arm_fire_now = fire_now;
}

void CloudService::disarm() { ++test_spy::cloud_disarm_count; }

void CloudService::set_disable_cloud(bool disable) {
  ++test_spy::cloud_set_disable_count;
  test_spy::cloud_last_disable_cloud = disable;
}

void CloudService::update_measures_snapshot(const MeasuresAGo &m) {
  ++test_spy::cloud_snapshot_count;
  test_spy::cloud_last_snapshot = m;
}

// CloudService private methods — never reached through orchestrator stubs.
void CloudService::_run() {}
void CloudService::_task_entry(void * /*arg*/) {}
uint32_t CloudService::_run_iteration(uint32_t /*now*/) { return 0; }
void CloudService::_wake() {}
void CloudService::_do_post(uint32_t /*now_ms*/) {}
void CloudService::_do_fetch(uint32_t /*now_ms*/) {}
MeasuresAGo CloudService::_snapshot_copy() { return _latest_snapshot; }

// ============================================================================
// OtaService stubs — orchestrator wiring only.  The real run_ble() /
// run_wifi_check() are thin shells over NimBLE / ESP-IDF (compiled out under
// TEST_HOST) and are HIL-verified.  The by-value EspOtaImageWriter +
// OtaBleService members construct from their host shells (linked into the
// orchestrator test target) but are never driven here.
// ============================================================================

OtaService::OtaService(AgBleServer &server, PowerService &power, const Config &cfg)
    : _server(server), _power(power), _config(cfg), _writer(), _ble_ota(_server, _writer) {}

bool OtaService::setup_ble() {
  test_spy::ota_setup_ble_called = true;
  return test_spy::ota_setup_ble_result;
}

void OtaService::handle_disconnect() { test_spy::ota_handle_disconnect_called = true; }

bool OtaService::is_ble_active() const { return test_spy::ota_is_ble_active; }

OtaStatus OtaService::run_ble() {
  ++test_spy::ota_run_ble_count;
  return test_spy::ota_run_ble_result;
}

OtaStatus OtaService::run_wifi_check(const std::function<void()> &on_download_started) {
  ++test_spy::ota_run_wifi_count;
  if (test_spy::ota_run_wifi_invoke_download_started && on_download_started) {
    on_download_started(); // simulate the first Downloading tick (real image pull)
  }
  return test_spy::ota_run_wifi_result;
}

void OtaService::teardown_ble() {
  test_spy::ota_teardown_ble_called = true;
  ++test_spy::ota_teardown_ble_count;
}

// ============================================================================
// LedService stubs
// ============================================================================

#include "led/go_led.h"

LedService::LedService(const Config & /*config*/) {}
LedService::~LedService() = default;

bool LedService::init() { return true; }
bool LedService::start() { return true; }

void LedService::front_set_brightness(LedBrightness /*brightness*/) {}

void LedService::back_solid(Rgb /*color*/) {}
void LedService::back_blink(Rgb /*color*/, uint32_t /*period_ms*/) {}
void LedService::back_breathe(Rgb /*color*/, uint32_t /*period_ms*/) {}
void LedService::back_fade_to(Rgb /*color*/, uint32_t /*duration_ms*/) {}
void LedService::back_chase(Rgb /*color*/, uint32_t /*step_ms*/) {}
void LedService::back_off() {}
void LedService::back_set_brightness(LedBrightness /*brightness*/) {}
void LedService::back_play(const BackStep * /*steps*/, uint8_t /*count*/) {}
void LedService::back_animate(BackAnimation /*animation*/) {}
void LedService::back_update_aqi(float /*pm25_ugm3*/) {}
void LedService::back_clear_aqi() {}

void LedService::touch_flash(TouchPad /*pad*/) {}
void LedService::touch_set_intensity(TouchLedIntensity /*intensity*/) {}

bool LedService::_is_inert() const { return true; }
void LedService::_enqueue(const Cmd & /*cmd*/) {}
void LedService::_process_cmd(const Cmd & /*cmd*/, uint32_t /*now_ms*/) {}
void LedService::_tick_back(uint32_t /*now_ms*/) {}
void LedService::_tick_touch(uint32_t /*now_ms*/) {}
void LedService::_render_front() {}
void LedService::_render_back() {}
void LedService::_render_touch() {}
bool LedService::_is_back_static() const { return true; }
Rgb LedService::_compute_back_frame(uint32_t /*now_ms*/) { return {}; }
Rgb LedService::_compute_primitive_frame(BackEffectState::Type /*type*/, Rgb /*color*/,
                                         uint32_t /*param_ms*/, Rgb /*fade_from*/,
                                         uint32_t /*elapsed_ms*/) const {
  return {};
}
bool LedService::_is_primitive_done(BackEffectState::Type /*type*/, uint32_t /*param_ms*/,
                                    uint32_t /*elapsed_ms*/) const {
  return true;
}
uint32_t LedService::_sequence_step_duration(const BackStep & /*step*/) const { return 0; }
void LedService::_advance_sequence(uint32_t /*now_ms*/) {}
void LedService::_apply_auto_restore(uint32_t /*now_ms*/) {}
void LedService::_start_back_effect(BackEffectState::Type /*type*/, Rgb /*color*/,
                                    uint32_t /*param_ms*/, uint32_t /*now_ms*/) {}
void LedService::_clear_saved_effect() {}
LedService::BackEffectState::Type LedService::_step_to_type(BackStep::Effect /*e*/) {
  return BackEffectState::Type::Off;
}

void LedService::_task_entry(void * /*arg*/) {}
void LedService::_run() {}

#ifdef TEST_HOST
void LedService::pump_for_test(uint32_t /*now_ms*/) {}
#endif

// ============================================================================
// BuzzerService stubs
// ============================================================================

#include "buzzer/go_buzzer.h"

BuzzerService::BuzzerService(const Config & /*config*/) {}
BuzzerService::~BuzzerService() = default;

bool BuzzerService::init() { return true; }
bool BuzzerService::start() { return true; }

void BuzzerService::play(const Note * /*notes*/, uint8_t /*count*/) {}
void BuzzerService::beep(uint32_t /*freq_hz*/, uint32_t /*duration_ms*/) {}
void BuzzerService::set_enabled(bool /*enabled*/) {}
void BuzzerService::stop() {}
bool BuzzerService::is_playing() const { return false; }
bool BuzzerService::enabled() const { return false; }

bool BuzzerService::_is_inert() const { return true; }
void BuzzerService::_enqueue(const Cmd & /*cmd*/) {}
void BuzzerService::_process_cmd(const Cmd & /*cmd*/, uint32_t /*now_ms*/) {}
void BuzzerService::_tick_playback(uint32_t /*now_ms*/) {}
void BuzzerService::_start_note(uint32_t /*now_ms*/) {}
void BuzzerService::_stop_playback(uint32_t /*now_ms*/) {}
void BuzzerService::_drain_queue() {}

void BuzzerService::_task_entry(void * /*arg*/) {}
void BuzzerService::_run() {}

#ifdef TEST_HOST
void BuzzerService::pump_for_test(uint32_t /*now_ms*/) {}
#endif

// ============================================================================
// play_synced stub
// ============================================================================

#include "go_melody_sync.h"

uint32_t play_synced(BuzzerService & /*buzzer*/, LedService & /*led*/, MelodySelect /*melody*/) {
  return 0;
}
