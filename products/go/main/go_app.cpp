/**
 * AirGradient Go — GoApp Implementation
 *
 * Contains all boot-path logic moved from main.cpp.  Hardware access is
 * routed through the GoBoard interface for testability.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_app.h"

#include "ag_log.h"
#include "common.h"
#include "led/go_led_types.h"
#ifndef TEST_HOST
#include "board_config.h"
// For ESP DFS and Auto Light Sleep (via FreeRTOS)
#include "esp_pm.h"
#include <esp_system.h>
#else
// Minimal pin/bus constants for host test compilation.
inline constexpr int PIN_BUTTON_POWER = 5;
inline constexpr int PIN_BUTTON_BOOT = 28;
inline constexpr int PIN_CAP_INT = 1;
inline constexpr int GPS_BAUD = 115200;
// Stub esp_reset_reason for host builds.
enum esp_reset_reason_t { ESP_RST_UNKNOWN = 0 };
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_UNKNOWN; }
#endif
#include "go_ble.h"
#include "go_buzzer.h"
#include "go_cloud.h"
#ifndef TEST_HOST
#include "fg_learning/fg_learning_runner.h"
#endif
#include "go_events.h"
#include "go_input.h"
#include "go_local_api.h"
#include "go_melody_sync.h"
#include "go_orchestrator.h"
#include "go_ota.h"
#include "go_portable_provisioner.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_settings.h"
#include "go_storage.h"
#include "go_ui.h"
#include "go_ulp.h"
#include "go_wifi.h"
#include "gps/gps_service.h"
#include "measurement_corrections.h"
#include "retained_uptime.h"
#include "rtos.h"
#include "serial_command/serial_command.h"
#include "services/local_server.h"
#include "services/sensor_manager.h"

#include <ctime>

static constexpr const char *TAG = "app";

// HTTP host for WiFi pull OTA. Mirrors AgClient's (private) default cloud
// domain; keep in sync with the Cloud service host.
static constexpr const char *OTA_HTTP_DOMAIN = "hw.airgradient.com";

// The GPS receiver remains powered and tracking through Offline deep sleep.
// Allow several NMEA epochs after reconnecting the UART on timer wake.
static constexpr uint32_t GPS_FAST_PATH_READ_TIMEOUT_MS = 3000;

/// A transient BMS communication failure gets one same-boot retry.
static constexpr uint8_t BMS_INIT_MAX_ATTEMPTS = 2;
static constexpr uint32_t BMS_INIT_RETRY_DELAY_MS = 100;

// Strings owned by GoApp that WifiService::Config holds pointers into.
// Stack-allocated in run_*; lifetime = process (functions never return).
namespace {
struct StationaryStrings {
  std::string ap_ssid;               // "airgradient-<12-hex>"
  std::string hostname;              // "airgradient_<12-hex>"
  std::string ble_manufacturer_data; // "P-1PSG#<12-hex>"
  std::string ble_device_name;       // "AirGradient Go <last4>" (matches Portable)
};

StationaryStrings make_stationary_strings(const std::string &serial) {
  char ble_name[BLE_ADV_NAME_BUF_SIZE];
  compute_ble_adv_name(serial.c_str(), ble_name, sizeof(ble_name));
  return {"airgradient-" + serial, "airgradient_" + serial,
          std::string(STATIONARY_AGO_MODEL_CODE) + "#" + serial, ble_name};
}

WifiService::Config make_wifi_service_config(const StationaryStrings &s, const char *serial,
                                             const char *firmware_version) {
  WifiService::Config cfg{};
  cfg.ap_ssid = s.ap_ssid.c_str();
  cfg.ble_device_name = s.ble_device_name.c_str(); // same name as Portable
  cfg.ble_serial_number = serial;
  cfg.ble_firmware_version = firmware_version;
  cfg.ble_model_name = STATIONARY_AGO_MODEL_CODE;
  cfg.ble_manufacturer_data = s.ble_manufacturer_data.c_str();
  cfg.serial_number = serial;
  cfg.firmware_version = firmware_version;
  cfg.model = STATIONARY_AGO_MODEL_CODE;
  cfg.hostname = s.hostname.c_str();
  cfg.http_port = CONFIG_AG_HTTP_PORT;
  return cfg;
}

PortableWifiProvisioner::Config make_portable_prov_config(const char *serial,
                                                          const char *firmware_version) {
  PortableWifiProvisioner::Config cfg{};
  // device_name is log-only in attached mode; DIS fields come from the product.
  cfg.ble_model_name = STATIONARY_AGO_MODEL_CODE; // "P-1PSG"
  cfg.ble_serial_number = serial;
  cfg.ble_firmware_version = firmware_version;
  cfg.radio_idle_ms = CONFIG_GO_PORTABLE_PROV_RADIO_IDLE_MS;
  return cfg;
}
} // namespace

#ifndef TEST_HOST
static esp_err_t configure_esp_power_management();
#endif

// ===========================================================================
// Construction
// ===========================================================================

GoApp::GoApp(GoBoard &board) : _board(board) {}

bool GoApp::init_bms_with_retry() {
  for (uint8_t attempt = 1; attempt <= BMS_INIT_MAX_ATTEMPTS; ++attempt) {
    if (_board.init_bms()) {
      return true;
    }

    AG_LOGE(TAG, "BMS initialization attempt %u/%u failed", static_cast<unsigned int>(attempt),
            static_cast<unsigned int>(BMS_INIT_MAX_ATTEMPTS));
    if (attempt < BMS_INIT_MAX_ATTEMPTS) {
      RTOS::delay_ms(BMS_INIT_RETRY_DELAY_MS);
    }
  }

  AG_LOGE(TAG, "BMS initialization failed after %u attempts",
          static_cast<unsigned int>(BMS_INIT_MAX_ATTEMPTS));
  return false;
}

// ===========================================================================
// GoApp::run() — boot path selection
// ===========================================================================

void GoApp::run() {
  // Immediately after boot is complete, configure power management.
  // No need to print success, pm does that for us.
#ifndef TEST_HOST
  esp_err_t pm_stat = configure_esp_power_management();
  if (pm_stat != ESP_OK) {
    AG_LOGE(TAG,
            "Failed to configure power management (err=0x%x); "
            "attempting to run at default clock.",
            pm_stat);
  }
#endif
  retained_uptime::init();
  RTOS::delay_ms(100);
  log_heap(TAG, "boot:run-entry");

  // Factory fuel-gauge learning pre-empts every normal boot path. Only the
  // lightweight, idempotent init_nvs() is needed to read FactorySettings; the
  // heavy hardware initialization and BMS retry happen inside the factory
  // path. A timer wake, button wake, or charger re-plug during an active run
  // always routes here, which makes resume-across-ship-off automatic.
  _board.init_nvs();
  FactorySettings fs{};
  load_factory_settings(_board.config_store(), fs);
  if (is_factory_learning_stage_active(fs.fg_learning_stage)) {
    run_factory_learning_path(load_rtc_app_state()); // never returns
  }

  WakeCause cause = PowerService::get_wake_cause();
  RtcAppState state = load_rtc_app_state();

  AG_LOGI(TAG,
          "reset_reason=%d wake_cause=%d rtc_state: mode=%d behavior=%d lock=%d gps=%d "
          "tracking=%d session=%u warm=%d",
          static_cast<int>(esp_reset_reason()), static_cast<int>(cause),
          static_cast<int>(state.mode), static_cast<int>(state.behavior),
          static_cast<int>(state.lock_state), state.gps_enabled, state.tracking_active,
          state.tracking_session_id, state.sensors_warm);

  BootPath path = select_boot_path(cause, state);

  switch (path) {
  case BootPath::FastPath:
    run_fast_path(state);
    break; // never reached
  case BootPath::ButtonWake:
    run_button_wake_path(state);
    break; // never reached
  case BootPath::Interactive:
    AG_LOGI(TAG, "Serial number: %s", _board.serial_number().c_str());
    run_interactive(cause, {});
    break; // never reached
  }
}

// ===========================================================================
// Factory fuel-gauge learning path
// ===========================================================================

void GoApp::run_factory_learning_path(const RtcAppState & /*state*/) {
  AG_LOGI(TAG, "run_factory_learning_path: entering factory fuel-gauge learning");
#ifndef TEST_HOST
  _board.init_core();
  _board.init_fuel_gauge();
  if (!init_bms_with_retry()) {
    AG_LOGE(TAG, "BMS unavailable during factory learning; restarting");
    _board.restart();
    return;
  }
  _board.release_gpio_holds();

  FgLearningRunner runner({
      .power = _board.power(),
      .display = _board.display(),
      .led = _board.led_service(),
      .buzzer = _board.buzzer_service(),
      .config_store = _board.config_store(),
      .board = _board,
      .storage = _board.storage(), // mounts NAND (init_spi done by init_core)
  });
  runner.run(); // never returns
#endif
}

// ===========================================================================
// Fast-path boot (timer wake, locked)
// ===========================================================================

void GoApp::run_fast_path(const RtcAppState &state) {
  AG_LOGI(TAG, "run_fast_path: entering fast-path boot (sensors_warm=%d)", state.sensors_warm);
  log_heap(TAG, "boot:fast-path:enter");

  // ISR for button detection during blocking operations.
  volatile bool button_pressed = false;
  _board.install_button_isr(PIN_BUTTON_POWER, &button_pressed);

  _board.ulp_stop();

  // Load snapshot here so it stays on this (non-returning) stack.
  // execute_fast_path may reference it via pointer in the handoff.
  RtcDisplaySnapshot snapshot{};
  bool snapshot_valid = load_rtc_display_snapshot(&snapshot);

  auto result = execute_fast_path(state, button_pressed, &snapshot, snapshot_valid);

  _board.remove_button_isr(PIN_BUTTON_POWER);

  if (result.outcome == FastPathResult::Outcome::Shutdown) {
    _board.power().shutdown();
    return;
  }

  if (result.outcome == FastPathResult::Outcome::Sleep) {
    RtcAppState save = state;
    save.sensors_warm = result.sensors_warm;
    _board.power().save_state(save);

    _board.display().stop();
    _board.display().deep_sleep();
    _board.ulp_start();
    log_heap(TAG, "boot:fast-path:before-sleep");
    _board.power().enter_sleep(result.sleep_duration_ms);
    // Never returns — CPU reboots on wake.
    return;
  }

  if (result.outcome == FastPathResult::Outcome::Promote && _board.bms() == nullptr &&
      !init_bms_with_retry()) {
    AG_LOGE(TAG, "BMS unavailable during fast-path promotion; restarting");
    _board.restart();
    return;
  }

  // Promotion to interactive — wire fast_path_measures pointer into
  // result struct (lives on this non-returning stack).
  if (result.has_measures) {
    result.handoff.fast_path_measures = &result.measures;
  }

  AG_LOGI(TAG, "fast-path promoting to interactive (button=%d)", static_cast<int>(button_pressed));
  run_interactive(WakeCause::Timer, result.handoff);
}

// ===========================================================================
// GoApp::execute_fast_path() — testable core
// ===========================================================================

GoApp::FastPathResult GoApp::execute_fast_path(const RtcAppState &state,
                                               const volatile bool &button_pressed,
                                               const RtcDisplaySnapshot *snapshot,
                                               bool snapshot_valid) {
  const uint32_t boot_time_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // --- Core init (NVS must be ready before load_settings) ---
  _board.init_core();
  _board.init_fuel_gauge();
  if (!init_bms_with_retry()) {
    AG_LOGW(TAG, "fast-path continuing without BMS");
  }
  _board.release_gpio_holds();
  _board.power().set_pm_power(true);

  GoSettings settings = _board.load_settings();

  SensorManager &sm = _board.sensors(state.sensors_warm);

  bool promote = false;

  // --- Warmup (interruptible) ---
  if (state.sensors_warm) {
    AG_LOGI(TAG, "fast-path: sensors warm — skipping warmup");
    RTOS::delay_ms(200);
  } else {
    const int warmup_iters = CONFIG_SENSOR_WARMUP_DURATION_MS / CONFIG_SENSOR_WARMUP_INTERVAL_MS;
    AG_LOGI(TAG, "fast-path: warmup %d iterations (%d ms interval)", warmup_iters,
            CONFIG_SENSOR_WARMUP_INTERVAL_MS);
    for (int i = 0; i < warmup_iters && !promote; i++) {
      AG_LOGI(TAG, "fast-path: warmup iteration %d/%d", i + 1, warmup_iters);
      uint64_t start = RTOS::get_time_ms();
      sm.warmup_step();

      if (button_pressed) {
        promote = true;
        break;
      }

      uint64_t elapsed = RTOS::get_time_ms() - start;
      if (elapsed < CONFIG_SENSOR_WARMUP_INTERVAL_MS) {
        RTOS::delay_ms(static_cast<uint32_t>(CONFIG_SENSOR_WARMUP_INTERVAL_MS - elapsed));
      }
      if (button_pressed) {
        promote = true;
        break;
      }
    }
  }

  // --- One-shot measurement ---
  MeasuresAGo ago{};
  bool has_measures = false;
  if (!promote) {
    Measures measures = sm.start_measures(1, SensorGroup::All);
    // UX placeholder: fast path never configures the gas-index algorithm
    // (no SensorProducer), so index fields stay at invalid sentinels.
    // Overwrite with raw ticks so the display and stored route points
    // still show a value during Offline fast-path cycles.
    measures.tvoc_nox.tvoc_index = measures.tvoc_nox.tvoc_raw;
    measures.tvoc_nox.nox_index = measures.tvoc_nox.nox_raw;
    ago = measures_to_ago(measures);
    has_measures = true;
    AG_LOGI(TAG,
            "fast-path: temp=%.1f hum=%.1f pm25=%.1f co2=%d tvoc=%d nox=%d "
            "tvoc_raw=%d nox_raw=%d pres=%.1f",
            ago.temp_hum_a.temperature, ago.temp_hum_a.humidity, ago.pm_a.pm_25, ago.co2.co2,
            ago.tvoc_nox.tvoc_index, ago.tvoc_nox.nox_index, ago.tvoc_nox.tvoc_raw,
            ago.tvoc_nox.nox_raw, ago.pressure.pressure);
    if (button_pressed) {
      promote = true;
    }
  }

  // --- One-shot GPS ---
  GpsData gps{};
  const bool gps_active = is_gps_active_at_boot(settings, state);

  if (!promote && state.tracking_active && gps_active) {
    auto *gps_driver = _board.new_gps_driver();
    gps = gps_read_once(*gps_driver, GPS_BAUD, GPS_FAST_PATH_READ_TIMEOUT_MS, button_pressed);
    AG_LOGI(TAG, "fast-path: gps fix_type=%d sat=%d lat=%.6f lon=%.6f alt=%.1f hdop=%.1f",
            static_cast<int>(gps.fix.fix_type), gps.fix.satellite_count, gps.position.latitude,
            gps.position.longitude, gps.altitude_m, gps.fix.hdop);
    if (button_pressed) {
      promote = true;
    }
  }

  // --- Storage + cache ---
  // `storage_failure_promote` records that promotion happened BEFORE the
  // display block, so the handoff builder knows the display was not
  // painted. tracking_active is left intact in RTC so the orchestrator's
  // init() retries resume_route() and surfaces persistent faults there.
  bool storage_failure_promote = false;
  bool power_polled = false;
  PowerSnapshot power_snapshot{};
  if (!promote) {
    StorageService &stor = _board.storage();
    stor.cache_measurement(ago);

    if (state.tracking_active) {
      // Fast path can only resume — only prepare_for_sleep sets tracking_active.
      if (!stor.resume_route(state.tracking_session_id)) {
        AG_LOGW(TAG, "fast-path: resume_route failed → promote");
        promote = true;
        storage_failure_promote = true;
      } else {
        power_snapshot = _board.power().poll_bms();
        power_polled = true;
        RoutePoint point{};
        point.timestamp = time(nullptr);
        point.gps = gps;
        point.sensors = ago;
        point.battery_percentage = power_snapshot.battery_percentage;
        if (!stor.append_route_point(point)) {
          AG_LOGW(TAG, "fast-path: append_route_point failed → promote");
          promote = true;
          storage_failure_promote = true;
        }
        stor.end_route(); // best-effort close even on append failure
      }
    }
    _board.storage().backup_cache();
  }

  // --- Display + sleep decision ---
  if (!promote) {
    PowerService &pwr = _board.power();
    if (!power_polled) {
      power_snapshot = pwr.poll_bms();
    }

    DisplayService &disp = _board.display();
    DisplayValues values =
        build_fast_path_display(ago, gps, power_snapshot, settings, state.tracking_active);
    if (power_snapshot.ship_mode_request == ShipModeRequest::OverTemperature ||
        power_snapshot.ship_mode_request == ShipModeRequest::UnderTemperature) {
      values.screen = power_snapshot.ship_mode_request == ShipModeRequest::UnderTemperature
                          ? Screen::ShutdownTemperatureLow
                          : Screen::ShutdownTemperature;
      disp.init(values);
      return {
          .outcome = FastPathResult::Outcome::Shutdown,
          .handoff = {},
          .measures = ago,
          .has_measures = has_measures,
          .sleep_duration_ms = 0,
          .sensors_warm = false,
      };
    }
    disp.init(values);

    uint32_t awake_ms = static_cast<uint32_t>(RTOS::get_time_ms()) - boot_time_ms;
    auto decision = pwr.decide_sleep(settings, LockState::Locked, OperatingMode::Offline, awake_ms);

    if (decision.type == PowerService::SleepType::Deep) {
      const bool warm = pwr.should_hold_pm_sensor(decision.duration_ms);
      if (!warm) {
        // Long sleep: stop the PM fan for the sleep window. Next boot's
        // init() wakes the sensor and re-warms (sensors_warm=false).
        sm.pm_sleep();
      }
      return {
          .outcome = FastPathResult::Outcome::Sleep,
          .handoff = {},
          .measures = ago,
          .has_measures = has_measures,
          .sleep_duration_ms = decision.duration_ms,
          .sensors_warm = warm,
      };
    }
    // Sleep too short — fall through to promotion.
    promote = true;
  }

  // --- Build promotion handoff ---
  //
  // NOTE: handoff.fast_path_measures is NOT set here.  The MeasuresAGo
  // value is returned in FastPathResult::measures.  The caller
  // (run_fast_path) sets fast_path_measures to point into the result
  // struct, which lives on the caller's non-returning stack.
  const bool button_caused = button_pressed;

  BootHandoff handoff{};
  handoff.measurement_completed = has_measures;
  // fast_path_measures left null — caller wires the pointer (see note above)

  if (button_caused) {
    handoff.initial_lock_state = LockState::Unlocked;
    handoff.suppress_wake_press = true;
    handoff.display_snapshot = snapshot_valid ? snapshot : nullptr;
    handoff.display_painted = false;
  } else {
    // "Sleep too short" runs AFTER disp.init() (painted); storage-failure
    // promotion runs BEFORE the display block (not painted).
    handoff.initial_lock_state = LockState::Locked;
    handoff.display_painted = !storage_failure_promote;
  }

  return {
      .outcome = FastPathResult::Outcome::Promote,
      .handoff = handoff,
      .measures = ago,
      .has_measures = has_measures,
      .sleep_duration_ms = 0,
      .sensors_warm = false,
  };
}

// ===========================================================================
// Button-wake boot path
// ===========================================================================

void GoApp::run_button_wake_path(const RtcAppState &state) {
  AG_LOGI(TAG, "run_button_wake_path: entering button-wake boot");

  // -----------------------------------------------------------------------
  // Phase 1: Early paint (~10 ms)
  // -----------------------------------------------------------------------

  _board.init_spi();
  DisplayService &disp = _board.display();

  RtcDisplaySnapshot snapshot{};
  const bool snapshot_valid = load_rtc_display_snapshot(&snapshot);

  AG_LOGI(TAG,
          "button_wake: snapshot_valid=%d co2=%d pm25=%.1f temp=%.1f hum=%.1f "
          "tvoc=%d nox=%d pres=%.1f alt=%.1f batt=%d charging=%d "
          "gps_en=%d gps_fix=%d tracking=%d ble=%d fahrenheit=%d feet=%d usaqi=%d",
          snapshot_valid, snapshot.co2_ppm, snapshot.pm25_ugm3, snapshot.temperature_c,
          snapshot.humidity_pct, snapshot.tvoc_index, snapshot.nox_index, snapshot.pressure_hpa,
          snapshot.altitude_m, snapshot.battery_pct, snapshot.is_battery_charging,
          snapshot.gps_enabled, snapshot.gps_fix, snapshot.tracking_active, snapshot.ble_enabled,
          snapshot.use_fahrenheit, snapshot.use_feet, snapshot.pm_use_usaqi);

  DisplayValues wake_values = build_wake_values(snapshot, snapshot_valid);

  // Returns in ~10 ms.  Worker task handles the SPI full refresh (~3 s).
  disp.init(wake_values, /* defer_refresh= */ true);

  // Stop LP Core after SPI/display init, before I2C init.
  _board.ulp_stop();

  // -----------------------------------------------------------------------
  // Phase 2: Parallel init (~300 ms)
  // Non-SPI peripherals — runs while the display refreshes in background.
  // -----------------------------------------------------------------------

  _board.init_core();
  _board.init_fuel_gauge();
  if (!init_bms_with_retry()) {
    AG_LOGE(TAG, "BMS unavailable during button wake; restarting");
    _board.restart();
    return;
  }
  _board.release_gpio_holds();
  _board.power().set_pm_power(true);

  GoSettings settings = _board.load_settings();

  SensorManager &sm = _board.sensors();

  // GPS driver (UART — no SPI contention)
  auto *gps_driver = _board.new_gps_driver();

  // Touch sensor (I2C)
  auto *touch = _board.new_touch_sensor();

  // Event queue
  RtosQueueHandle event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  // Construct producer services
  auto *sensor_producer =
      new SensorProducer(sm, event_queue,
                         {
                             .task_stack_size = 4096,
                             .task_priority = 5,
                             .co2_abc_days = settings.co2_abc_days,
                             .tvoc_learning_offset = settings.tvoc_learning_offset,
                             .nox_learning_offset = settings.nox_learning_offset,
                         });

  auto *gps_service = new GpsService(*gps_driver, event_queue,
                                     {
                                         .baud_rate = GPS_BAUD,
                                         .task_stack_size = 4096,
                                         .task_priority = 3,
                                     });

  // InputService: suppress the first ButtonPower event (the wake press)
  auto *input_service = new InputService(*touch, _board.gpio_hal(), event_queue,
                                         {
                                             .pin_cap_int = PIN_CAP_INT,
                                             .pin_button_power = PIN_BUTTON_POWER,
                                             .pin_button_boot = PIN_BUTTON_BOOT,
                                             .suppress_button_wake = true,
                                         });

  PowerService &pwr = _board.power();

  std::string serial = _board.serial_number();
  const char *firmware_version = _board.firmware_version();
  AG_LOGI(TAG, "Serial number: %s", serial.c_str());

  auto *local_api_service =
      new GoLocalApiService(event_queue, {
                                             .serial_number = serial.c_str(),
                                             .firmware_version = firmware_version,
                                             .model = STATIONARY_AGO_MODEL_CODE,
                                         });
  auto *local_server =
      new LocalServer(_board.http_server(), {*local_api_service, local_api_service,
                                             ConfigAccess::ReadWrite, local_api_service});

  auto *ui_manager = new UIManager({
      .firmware_version = firmware_version,
      .serial_number = serial.c_str(),
  });

  // Determine whether GPS should be active based on settings and RTC state.
  const bool gps_active = is_gps_active_at_boot(settings, state);

  // Start producer tasks — touch and sensors operational from here (~310 ms)
  sensor_producer->start();
  if (gps_active) {
    gps_service->start();
  } else {
    gps_service->idle_gnss();
  }
  input_service->start();
  log_heap(TAG, "boot:button-wake:phase2-end");

  // -----------------------------------------------------------------------
  // Phase 3: Storage init (blocks on SPI until display refresh finishes)
  // -----------------------------------------------------------------------

  StorageService &stor = _board.storage();

  // BleService construction requires StorageService and borrows the
  // shared BLE server from the board (Portable mode owns it for now;
  // Stationary provisioning will borrow it under mutual exclusion).
  auto *ble_service = new BleService(event_queue, stor, _board.ble_server());

  // OtaService borrows the shared server + PowerService and owns the writer.
  // The WiFi paint callback is passed per-call by the orchestrator.
  auto *ota_service = new OtaService(_board.ble_server(), pwr,
                                     {
                                         .serial_number = serial.c_str(),
                                         .firmware_version = firmware_version,
                                         .http_domain = OTA_HTTP_DOMAIN,
                                     });

  // WifiService owns the Stationary networking lifecycle. Borrows
  // wifi/ble/http from the board; no driver init until enter_stationary().
  auto *stationary_strings = new StationaryStrings(make_stationary_strings(serial));
  auto *wifi_service = new WifiService(
      event_queue,
      {_board.wifi_manager(), _board.ble_server(), _board.http_server(), *local_server},
      make_wifi_service_config(*stationary_strings, serial.c_str(), firmware_version));

  // Attached Portable provisioning; borrows the same wifi/ble + board. Idle
  // until the app writes a scan/credential request.
  auto *portable_provisioner =
      new PortableWifiProvisioner(event_queue, {_board.wifi_manager(), _board.ble_server(), _board},
                                  make_portable_prov_config(serial.c_str(), firmware_version));

  // Inert until start(); heap claimed only when Stationary + online.
  auto *cloud_service =
      new CloudService(event_queue, {_board.ag_client(), *wifi_service}, CloudService::Config{});
  auto *serial_command_channel = new UsbSerialCommandChannel();
  auto *serial_command_service = new SerialCommandService(event_queue, *serial_command_channel);
  if (!settings.onboarding_done && !serial_command_service->start()) {
    AG_LOGE(TAG, "failed to start serial command service during onboarding");
  }
  // LED service — init and start before orchestrator.
  LedService &led = _board.led_service();
  led.init();
  led.start();

  // Buzzer service — init and start before orchestrator.
  BuzzerService &buzzer = _board.buzzer_service();
  buzzer.init();
  buzzer.start();
  buzzer.set_enabled(settings.buzzer_enabled);

  log_heap(TAG, "boot:button-wake:phase3-end");

  // -----------------------------------------------------------------------
  // Phase 4: Orchestrator — display + all services ready
  // -----------------------------------------------------------------------

  Orchestrator::Services services = {
      .sensor_producer = *sensor_producer,
      .gps_service = *gps_service,
      .input_service = *input_service,
      .display_service = disp,
      .led_service = led,
      .buzzer_service = buzzer,
      .storage_service = stor,
      .power_service = pwr,
      .ui_manager = *ui_manager,
      .ble_service = *ble_service,
      .wifi = *wifi_service,
      .cloud = *cloud_service,
      .local_api = *local_api_service,
      .serial_command = *serial_command_service,
      .portable_provisioner = *portable_provisioner,
      .board = _board,
      .ota = *ota_service,
  };

  auto *orchestrator =
      new Orchestrator(event_queue, services, settings, _board.config_store(), serial.c_str());

  BootHandoff handoff{};
  handoff.display_painted = true;
  handoff.suppress_wake_press = true;
  handoff.initial_lock_state = LockState::Unlocked;
  handoff.display_snapshot = snapshot_valid ? &snapshot : nullptr;
  orchestrator->init(WakeCause::Button, handoff);
  orchestrator->run(); // Never returns.
}

// ===========================================================================
// run_interactive — unified interactive entry
// ===========================================================================

void GoApp::run_interactive(WakeCause cause, BootHandoff handoff) {
  // --- Early display paint ---
  // Start the e-paper refresh before the slower I2C, BMS, sensor, and NAND
  // initialization. The display worker owns SPI while it refreshes; NAND
  // initialization naturally waits for that refresh before using the bus.
  bool boot_splash_requested = false;
  if (!handoff.display_painted) {
    _board.init_spi();
    DisplayService &early_display = _board.display();

    DisplayValues initial_values{};
    if (handoff.display_snapshot != nullptr) {
      initial_values = build_wake_values(*handoff.display_snapshot, true);
    } else {
      initial_values = build_boot_splash_values();
      boot_splash_requested = true;
    }

    early_display.init(initial_values, /* defer_refresh= */ true);
    handoff.display_painted = true;
  }

  // --- Complete any missing core init (idempotent) ---
  _board.init_core();
  _board.init_fuel_gauge();
  if (!init_bms_with_retry()) {
    AG_LOGE(TAG, "BMS unavailable during interactive boot; restarting");
    _board.restart();
    return;
  }
  _board.release_gpio_holds();
  _board.power().set_pm_power(true);

  GoSettings settings = _board.load_settings();

  // --- LED boot animation (fire early so it feels immediate) ---
  LedService &led = _board.led_service();
  led.init();
  led.start();

  // --- Buzzer service ---
  BuzzerService &buzzer = _board.buzzer_service();
  buzzer.init();
  buzzer.start();
  buzzer.set_enabled(settings.buzzer_enabled);

  SensorManager &sm = _board.sensors();

  // --- GPS driver (never done in fast path) ---
  auto *gps_driver = _board.new_gps_driver();

  // --- Touch sensor (never done in fast path) ---
  auto *touch = _board.new_touch_sensor();

  // --- Storage ---
  // NAND shares the display SPI bus. Finish the early splash refresh before
  // mounting storage so its initialization cannot contend with the EPD.
  DisplayService &disp = _board.display();
  disp.flush();
  StorageService &stor = _board.storage();

  // --- Event queue ---
  RtosQueueHandle event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  std::string serial = _board.serial_number();
  const char *firmware_version = _board.firmware_version();
  AG_LOGI(TAG, "Serial number: %s", serial.c_str());

  auto *local_api_service =
      new GoLocalApiService(event_queue, {
                                             .serial_number = serial.c_str(),
                                             .firmware_version = firmware_version,
                                             .model = STATIONARY_AGO_MODEL_CODE,
                                         });
  auto *local_server =
      new LocalServer(_board.http_server(), {*local_api_service, local_api_service,
                                             ConfigAccess::ReadWrite, local_api_service});

  // --- BLE ---
  // Borrow the shared AgBleServer from the board so Stationary
  // provisioning can later reuse the same instance under orchestrator-
  // enforced mutual exclusion.
  auto *ble_service = new BleService(event_queue, stor, _board.ble_server());

  // --- WifiService ---
  auto *stationary_strings = new StationaryStrings(make_stationary_strings(serial));
  auto *wifi_service = new WifiService(
      event_queue,
      {_board.wifi_manager(), _board.ble_server(), _board.http_server(), *local_server},
      make_wifi_service_config(*stationary_strings, serial.c_str(), firmware_version));

  // --- PortableWifiProvisioner (attached Portable Wi-Fi provisioning) ---
  auto *portable_provisioner =
      new PortableWifiProvisioner(event_queue, {_board.wifi_manager(), _board.ble_server(), _board},
                                  make_portable_prov_config(serial.c_str(), firmware_version));

  // --- CloudService (inert until start()) ---
  auto *cloud_service =
      new CloudService(event_queue, {_board.ag_client(), *wifi_service}, CloudService::Config{});
  auto *serial_command_channel = new UsbSerialCommandChannel();
  auto *serial_command_service = new SerialCommandService(event_queue, *serial_command_channel);
  if (!settings.onboarding_done && !serial_command_service->start()) {
    AG_LOGE(TAG, "failed to start serial command service during onboarding");
  }

  // --- Service construction ---
  auto *sensor_producer = new SensorProducer(sm, event_queue,
                                             {.task_stack_size = 4096,
                                              .task_priority = 5,
                                              .co2_abc_days = settings.co2_abc_days,
                                              .tvoc_learning_offset = settings.tvoc_learning_offset,
                                              .nox_learning_offset = settings.nox_learning_offset});

  auto *gps_service =
      new GpsService(*gps_driver, event_queue,
                     {.baud_rate = GPS_BAUD, .task_stack_size = 4096, .task_priority = 3});

  auto *input_service = new InputService(*touch, _board.gpio_hal(), event_queue,
                                         {.pin_cap_int = PIN_CAP_INT,
                                          .pin_button_power = PIN_BUTTON_POWER,
                                          .pin_button_boot = PIN_BUTTON_BOOT,
                                          .suppress_button_wake = handoff.suppress_wake_press});

  PowerService &pwr = _board.power();

  auto *ui_manager = new UIManager({
      .firmware_version = firmware_version,
      .serial_number = serial.c_str(),
  });

  // Seed UI state after the early splash paint, so Orchestrator retains the
  // splash until the first completed measurement.
  if (boot_splash_requested) {
    ui_manager->show_info(BOOT_SPLASH_TEXT);
  }

  // --- OtaService (borrows the shared server + PowerService; owns the writer) ---
  auto *ota_service = new OtaService(_board.ble_server(), pwr,
                                     {
                                         .serial_number = serial.c_str(),
                                         .firmware_version = firmware_version,
                                         .http_domain = OTA_HTTP_DOMAIN,
                                     });

  // Boot animation — runs after the splash refresh is scheduled so the screen
  // can update in parallel with the chime/LED.
  if (cause == WakeCause::PowerOn) {
    if (!settings.onboarding_done) {
      // Fresh unit defaults buzzer + back LED off; force a one-time synced
      // chime + LED welcome, then restore the persisted settings.
      buzzer.set_enabled(true);
      play_synced(buzzer, led, MelodySelect::Chime);
      buzzer.set_enabled(settings.buzzer_enabled);
      led.back_set_brightness(settings.back_led_brightness);
    } else {
      led.back_set_brightness(settings.back_led_brightness);
      led.back_animate(BackAnimation::Boot);
      buzzer.play(PATTERN_BOOT, PATTERN_BOOT_COUNT);
    }
  }

  // --- Determine whether GPS should be active ---
  // On fresh power-on, tracking is always inactive (no RTC state).
  // On wake from sleep, load the persisted tracking state.
  RtcAppState boot_state{};
  if (cause != WakeCause::PowerOn) {
    boot_state = load_rtc_app_state();
  }
  const bool gps_active = is_gps_active_at_boot(settings, boot_state);

  // --- Start producer tasks ---
  sensor_producer->start();
  if (gps_active) {
    gps_service->start();
  } else {
    gps_service->idle_gnss();
  }
  input_service->start();

  // --- Orchestrator ---
  Orchestrator::Services services = {
      .sensor_producer = *sensor_producer,
      .gps_service = *gps_service,
      .input_service = *input_service,
      .display_service = disp,
      .led_service = led,
      .buzzer_service = buzzer,
      .storage_service = stor,
      .power_service = pwr,
      .ui_manager = *ui_manager,
      .ble_service = *ble_service,
      .wifi = *wifi_service,
      .cloud = *cloud_service,
      .local_api = *local_api_service,
      .serial_command = *serial_command_service,
      .portable_provisioner = *portable_provisioner,
      .board = _board,
      .ota = *ota_service,
  };

  auto *orchestrator =
      new Orchestrator(event_queue, services, settings, _board.config_store(), serial.c_str());
  orchestrator->init(cause, handoff);
  log_heap(TAG, "boot:interactive:before-run");
  orchestrator->run(); // Never returns.
}

// ===========================================================================
// Pure utility functions
// ===========================================================================

BootPath select_boot_path(WakeCause cause, const RtcAppState &state) {
  if (cause == WakeCause::Timer) {
    if (PowerService::is_fast_path_wake(cause, state)) {
      return BootPath::FastPath;
    }
  }
  if (cause == WakeCause::Button && state.mode == OperatingMode::Offline) {
    return BootPath::ButtonWake;
  }
  return BootPath::Interactive;
}

bool is_gps_active_at_boot(const GoSettings &settings, const RtcAppState &state) {
  return (settings.gps_mode == GpsMode::AlwaysOn) ||
         (settings.gps_mode == GpsMode::OnWhenTracking && state.tracking_active);
}

MeasuresAGo measures_to_ago(const Measures &m) {
  MeasuresAGo ago{};
  ago.temp_hum_a = m.temp_hum_a;
  ago.pm_a = m.pm_a;
  ago.co2 = m.co2;
  ago.tvoc_nox = m.tvoc_nox;
  ago.power = m.power;
  ago.pressure = m.pressure;
  return ago;
}

DisplayValues build_fast_path_display(const MeasuresAGo &measures, const GpsData &gps,
                                      const PowerSnapshot &bms, const GoSettings &settings,
                                      bool tracking_active) {
  const MeasuresAGo corrected = apply_measurement_corrections(measures, settings.corrections);
  DisplayValues v{};

  if (corrected.co2.is_valid()) {
    v.co2_ppm = corrected.co2.co2;
  }
  if (corrected.pm_a.is_pm_25_valid()) {
    v.pm25_ugm3 = corrected.pm_a.pm_25;
  }
  if (corrected.temp_hum_a.is_temp_valid()) {
    v.temperature_c = corrected.temp_hum_a.temperature;
  }
  if (corrected.temp_hum_a.is_hum_valid()) {
    v.humidity_pct = corrected.temp_hum_a.humidity;
  }
  if (corrected.tvoc_nox.is_tvoc_index_valid()) {
    v.tvoc_index = corrected.tvoc_nox.tvoc_index;
  }
  if (corrected.tvoc_nox.is_nox_index_valid()) {
    v.nox_index = corrected.tvoc_nox.nox_index;
  }
  if (corrected.pressure.is_pressure_valid()) {
    v.pressure_hpa = corrected.pressure.pressure;
  }
  if (corrected.pressure.is_altitude_valid()) {
    v.altitude_m = corrected.pressure.altitude;
  }

  v.gps_fix = is_fix_valid(gps.fix);

  if (bms.battery_percentage >= 0.0f) {
    v.battery_pct = static_cast<uint8_t>(bms.battery_percentage);
  }
  v.is_battery_charging = is_bms_charging(bms.charging_status);
  v.is_plugged_in = bms_power_source_has_external_input(bms.charger_status.power_source);

  v.locked = true;
  v.screen = Screen::Home;
  v.tracking_active = tracking_active;

  v.use_fahrenheit = settings.use_fahrenheit;
  v.use_feet = settings.use_feet;
  v.pm_use_usaqi = settings.pm_use_usaqi;
  v.display_off = false;

  return v;
}

DisplayValues build_boot_splash_values() {
  DisplayValues v{};
  v.screen = Screen::Info;
  v.info_text = BOOT_SPLASH_TEXT;
  v.locked = true; // Info hides the status bar; kept for semantic correctness.
  v.display_off = false;
  return v;
}

DisplayValues build_wake_values(const RtcDisplaySnapshot &snapshot, bool snapshot_valid) {
  DisplayValues v{};

  if (snapshot_valid) {
    v.co2_ppm = snapshot.co2_ppm;
    v.pm25_ugm3 = snapshot.pm25_ugm3;
    v.temperature_c = snapshot.temperature_c;
    v.humidity_pct = snapshot.humidity_pct;
    v.tvoc_index = snapshot.tvoc_index;
    v.nox_index = snapshot.nox_index;
    v.pressure_hpa = snapshot.pressure_hpa;
    v.altitude_m = snapshot.altitude_m;
    v.battery_pct = snapshot.battery_pct;
    v.is_battery_charging = snapshot.is_battery_charging;
    v.gps_enabled = snapshot.gps_enabled;
    v.gps_fix = snapshot.gps_fix;
    v.tracking_active = snapshot.tracking_active;
    v.ble_enabled = snapshot.ble_enabled;
    v.use_fahrenheit = snapshot.use_fahrenheit;
    v.use_feet = snapshot.use_feet;
    v.pm_use_usaqi = snapshot.pm_use_usaqi;
  }

  v.screen = Screen::Home;
  v.locked = false;
  v.display_off = false;
  v.snackbar_text = "Unlocked";

  return v;
}

#ifndef TEST_HOST
static esp_err_t configure_esp_power_management(void) {

  esp_pm_config_t esp_pm = {
      .max_freq_mhz = max_esp32c5_cpu,
      .min_freq_mhz = min_esp32c5_cpu,
      .light_sleep_enable = false,
  };

  AG_LOGI(TAG, "Applying PM config: max=%d MHz, min=%d MHz", esp_pm.max_freq_mhz,
          esp_pm.min_freq_mhz);

  return esp_pm_configure(&esp_pm);
}
#endif