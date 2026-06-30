/**
 * AirGradient Go — Orchestrator implementation
 *
 * Central event loop, event dispatch, timer management, state transitions,
 * display updates, and sleep cycle management.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_orchestrator.h"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <ctime>
#include <utility>

#include "ag_log.h"
#include "common.h"
#include "go_ble_protocol.h"
#include "go_board.h"
#include "go_melody_sync.h"
#include "rtos.h"

static constexpr const char *TAG = "Orchestrator";

static constexpr uint8_t SESSION_ID_LENGTH = 5;

// 5 random draws against ~90k slots: collision probability is negligible.
static constexpr int SESSION_ID_MAX_RETRIES = 5;

// --- OTA poll cadences (unified _last_ota_check_ms baseline) ---
// Stationary WiFi OTA availability-check cadence (1 h).
static constexpr uint32_t OTA_WIFI_CHECK_INTERVAL_MS = 3600000;
// Portable BLE is_ble_active() start-edge poll cadence (2 s), gated on an
// authenticated client or a still-latched is_ble_active().
static constexpr uint32_t OTA_BLE_POLL_INTERVAL_MS = 2000;

static void log_sensor_snapshot(const MeasuresAGo &d) {
  AG_LOGI(TAG,
          "MeasuresAGo:\n"
          "temperature: %.2f\n"
          "humidity: %.2f\n"
          "pm_01: %.1f\n"
          "pm_25: %.1f\n"
          "pm_10: %.1f\n"
          "pm_05_pc: %.0f\n"
          "pm_01_pc: %.0f\n"
          "pm_25_pc: %.0f\n"
          "pm_10_pc: %.0f\n"
          "co2: %d\n"
          "tvoc_index: %d\n"
          "tvoc_raw: %d\n"
          "nox_index: %d\n"
          "nox_raw: %d\n"
          "battery_voltage: %.2f\n"
          "charging_voltage: %.2f\n"
          "pressure: %.1f\n"
          "altitude: %.1f",
          static_cast<double>(d.temp_hum_a.temperature), static_cast<double>(d.temp_hum_a.humidity),
          static_cast<double>(d.pm_a.pm_01), static_cast<double>(d.pm_a.pm_25),
          static_cast<double>(d.pm_a.pm_10), static_cast<double>(d.pm_a.pm_05_pc),
          static_cast<double>(d.pm_a.pm_01_pc), static_cast<double>(d.pm_a.pm_25_pc),
          static_cast<double>(d.pm_a.pm_10_pc), d.co2.co2, d.tvoc_nox.tvoc_index,
          d.tvoc_nox.tvoc_raw, d.tvoc_nox.nox_index, d.tvoc_nox.nox_raw,
          static_cast<double>(d.power.battery_voltage),
          static_cast<double>(d.power.charging_voltage), static_cast<double>(d.pressure.pressure),
          static_cast<double>(d.pressure.altitude));
}

// Map an operating-mode change leaving Portable to the BLE disconnect reason.
// Only Stationary / Offline are reachable here (entering Portable never drops
// a live BLE link).
static BleDiscReason disc_reason_for_mode(OperatingMode new_mode) {
  return new_mode == OperatingMode::Stationary ? BleDiscReason::OpStationary
                                               : BleDiscReason::OpOffline;
}

// Map a shutdown (ship-mode) reason to the BLE disconnect reason.
static BleDiscReason disc_reason_for_shutdown(ShipModeRequest reason) {
  switch (reason) {
  case ShipModeRequest::OverTemperature:
    return BleDiscReason::Overheat;
  case ShipModeRequest::OverDischarge:
    return BleDiscReason::LowBatt;
  case ShipModeRequest::None:
  default:
    return BleDiscReason::User;
  }
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

Orchestrator::Orchestrator(RtosQueueHandle event_queue, const Services &services,
                           GoSettings settings, ConfigStore &config_store, const char *serial)
    : _event_queue(event_queue), _svc(services), _settings(std::move(settings)),
      _config_store(config_store), _serial(serial), _cached_measures() {}

// ---------------------------------------------------------------------------
// Boot initialization
// ---------------------------------------------------------------------------

void Orchestrator::init(WakeCause cause, const BootHandoff &handoff) {
  AG_LOGI(TAG,
          "init: wake_cause=%d display_painted=%d measurement_done=%d "
          "lock=%d",
          static_cast<int>(cause), static_cast<int>(handoff.display_painted),
          static_cast<int>(handoff.measurement_completed),
          static_cast<int>(handoff.initial_lock_state));

  // Mode always comes from persisted settings (NVS) — single source of truth
  _mode = _settings.operating_mode;

  // --- Restore RTC state for wake-from-sleep cases ---
  if (cause != WakeCause::PowerOn) {
    RtcAppState state = _svc.power_service.load_state();
    _behavior = state.behavior;
    _gps_enabled = state.gps_enabled;
    _tracking_active = state.tracking_active;
    _tracking_session_id = state.tracking_session_id;
  }

  // --- Apply initial lock state ---
  if (handoff.initial_lock_state == LockState::Unlocked) {
    if (handoff.display_painted) {
      // Display already shows unlocked UI — set state directly.
      // Do NOT call unlock() which would trigger update_display().
      _lock_state = LockState::Unlocked;
      _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());
      _svc.ui_manager.show_snackbar("Unlocked");
      uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
      _svc.ui_manager.clear_expired_snackbar(now_ms);
      _snackbar_refresh_deadline_ms = now_ms + SNACKBAR_DURATION_MS + 200;
    } else {
      // Display not yet showing unlocked UI — unlock triggers
      // update_display() which will paint the unlocked frame.
      unlock();
    }
  }

  // --- Seed cached measures from boot ---
  // Fresh measurements take priority over stale RTC snapshot.
  if (handoff.fast_path_measures != nullptr) {
    _cached_measures = *handoff.fast_path_measures;
  } else if (handoff.display_snapshot != nullptr) {
    _cached_measures.co2.co2 = handoff.display_snapshot->co2_ppm;
    _cached_measures.pm_a.pm_25 = handoff.display_snapshot->pm25_ugm3;
    _cached_measures.temp_hum_a.temperature = handoff.display_snapshot->temperature_c;
    _cached_measures.temp_hum_a.humidity = handoff.display_snapshot->humidity_pct;
    _cached_measures.tvoc_nox.tvoc_index = handoff.display_snapshot->tvoc_index;
    _cached_measures.tvoc_nox.nox_index = handoff.display_snapshot->nox_index;
    _cached_measures.pressure.pressure = handoff.display_snapshot->pressure_hpa;
    _cached_measures.pressure.altitude = handoff.display_snapshot->altitude_m;
  }

  // --- Mark first measurement done if boot already measured ---
  if (handoff.measurement_completed) {
    _first_measurement_done = true;
  }

  // Cold-boot splash gate: run_interactive seeds UIManager via show_info()
  // before this point, so detect the splash from the current screen rather
  // than handoff.display_painted (already flipped to true).
  if (!handoff.measurement_completed && handoff.fast_path_measures == nullptr &&
      _svc.ui_manager.current_screen() == Screen::Info) {
    _boot_splash_active = true;
  }

  // --- Resume route if tracking was active before sleep ---
  // BLE isn't up yet, so the snackbar is the only inline signal; clients
  // observe the cleared state via Read on connect.
  if (_tracking_active) {
    if (!_svc.storage_service.resume_route(_tracking_session_id)) {
      AG_LOGE(TAG, "init: resume_route failed for session %" PRIu32, _tracking_session_id);
      _tracking_active = false;
      _tracking_session_id = 0;
      _behavior = Behavior::Idle;
      _svc.ui_manager.show_snackbar("Tracking stopped — storage");
    }
  }

  // --- LED brightness from settings ---
  _svc.led_service.front_set_brightness(_settings.front_led_brightness);
  _svc.led_service.back_set_brightness(_settings.back_led_brightness);
  _svc.led_service.touch_set_intensity(_settings.touch_led_intensity);

  // --- Common tail ---
  _svc.ui_manager.sync_settings(_settings);

  if (!handoff.measurement_completed) {
    _svc.sensor_producer.request_measurement(1, SensorGroup::All);
  }

  _latest_power = _svc.power_service.poll_bms(_first_measurement_done &&
                                              !_cached_measures.pm_a.is_pm_25_valid());

  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  _last_measurement_ms = now;
  _last_bms_poll_ms = now;
  _last_bms_status_poll_ms = now;
  _last_ext_wdt_ms = now;
  if (handoff.initial_lock_state != LockState::Unlocked || !handoff.display_painted) {
    _last_input_ms = now;
  }

  init_ble_if_portable();
  if (_settings.operating_mode == OperatingMode::Stationary) {
    enter_stationary();
  }
}

// ---------------------------------------------------------------------------
// Main event loop
// ---------------------------------------------------------------------------

void Orchestrator::run() {
  AG_LOGI(TAG, "run: entering main event loop");

  while (true) {
    // Sleep check: enter sleep when locked and first measurement is done
    if (_lock_state == LockState::Locked && _first_measurement_done) {
      try_enter_sleep(); // Returns only when sleep conditions are not met
    }

    uint32_t timeout = compute_queue_timeout_ms();
    Event evt{};
    if (RTOS::queue_receive(_event_queue, &evt, timeout)) {
      dispatch(evt);
    }

    check_timers();
  }
}

// ---------------------------------------------------------------------------
// Timer management
// ---------------------------------------------------------------------------

uint32_t Orchestrator::compute_queue_timeout_ms() const {
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  uint32_t next = UINT32_MAX;

  // Sensor measurement + BMS deadlines are paused while sensitive services
  // are paused (Provisioning / ProvisioningConfirm).  Their _last_*_ms
  // values stay frozen and are rebased to "now" on resume so the next
  // deadline lands a full interval into the future, not back-to-back to
  // catch up on missed cycles.
  uint32_t interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (!_provisioning_sensitive_services_paused) {
    // Sensor timer deadline
    {
      uint32_t deadline = _last_measurement_ms + interval_ms;
      uint32_t remaining = deadline - now;
      next = std::min(next, remaining);
    }

    // BMS full-telemetry deadline
    uint32_t bms_remaining = (_last_bms_poll_ms + BMS_POLL_INTERVAL_MS) - now;
    next = std::min(next, bms_remaining);

    // BMS fast charging-status deadline
    uint32_t bms_status_remaining = (_last_bms_status_poll_ms + BMS_STATUS_POLL_INTERVAL_MS) - now;
    next = std::min(next, bms_status_remaining);
  }

  // Inactivity deadline (only when unlocked, auto-lock enabled, and not
  // in a setup session — users on Info / Provisioning / ProvisioningConfirm
  // must not get auto-locked out mid-setup).
  if (_lock_state == LockState::Unlocked && _settings.auto_lock_seconds > 0 &&
      !_setup_session_active) {
    uint32_t inact_interval = static_cast<uint32_t>(_settings.auto_lock_seconds) * 1000;
    uint32_t inact_remaining = (_last_input_ms + inact_interval) - now;
    next = std::min(next, inact_remaining);
  }

  // PM pre-wake deadline (not Offline, interval above threshold, prepare
  // not yet sent, not in the sensitive-services pause).
  bool pm_sleep_eligible =
      _mode != OperatingMode::Offline && _svc.power_service.should_sleep_pm_sensor(interval_ms);
  if (pm_sleep_eligible && !_pm_prepare_sent && !_provisioning_sensitive_services_paused) {
    uint32_t measure_deadline = _last_measurement_ms + interval_ms;
    uint32_t prepare_deadline = measure_deadline - CONFIG_SENSOR_WARMUP_DURATION_MS;
    uint32_t pm_remaining = prepare_deadline - now;
    next = std::min(next, pm_remaining);
  }

  // Snackbar refresh deadline — moot during a setup session because
  // snackbars are suppressed on session screens, but explicitly skipped
  // here too so the orchestrator does not wake just to clear a no-op.
  if (_snackbar_refresh_deadline_ms != 0 && !_provisioning_sensitive_services_paused) {
    uint32_t sb_remaining = _snackbar_refresh_deadline_ms - now;
    next = std::min(next, sb_remaining);
  }

  // External watchdog deadline — always a candidate (ext WDT is never
  // suppressed during a session, per spec).  Without this, the gated
  // session path can leave `next == UINT32_MAX`, which the overdue clamp
  // below misinterprets as 0 and busy-spins the main loop, starving the
  // IDLE task and tripping the task WDT (~5 s).
  uint32_t ext_wdt_remaining = (_last_ext_wdt_ms + EXT_WDT_INTERVAL_MS) - now;
  next = std::min(next, ext_wdt_remaining);

  // Wi-Fi initial-connect / fallback deadline
  uint32_t wifi_deadline = _svc.wifi.next_deadline_ms();
  if (wifi_deadline != 0) {
    next = std::min(next, wifi_deadline - now);
  }

  // Portable provisioning radio-idle deadline
  uint32_t prov_deadline = _svc.portable_provisioner.next_deadline_ms();
  if (prov_deadline != 0) {
    next = std::min(next, prov_deadline - now);
  }

  // Unified OTA poll deadline — mode-selected interval/gate. Candidate only
  // while eligible: an overdue baseline while ineligible would clamp to 0 and
  // busy-spin the loop.
  uint32_t ota_interval = 0;
  bool ota_eligible = false;
  if (_mode == OperatingMode::Stationary && _svc.wifi.is_online() && !_setup_session_active) {
    ota_interval = OTA_WIFI_CHECK_INTERVAL_MS;
    ota_eligible = true;
  } else if (_mode == OperatingMode::Portable &&
             (_svc.ble_service.is_authenticated() || _svc.ota.is_ble_active())) {
    ota_interval = OTA_BLE_POLL_INTERVAL_MS;
    ota_eligible = true;
  }
  if (ota_eligible) {
    uint32_t ota_remaining = (_last_ota_check_ms + ota_interval) - now;
    next = std::min(next, ota_remaining);
  }

  // If any deadline already passed, the unsigned subtraction yields a large
  // number — clamp to 0 so check_timers() fires immediately.
  if (next > MAX_REASONABLE_TIMEOUT_MS) {
    next = 0;
  }

  return next;
}

void Orchestrator::check_timers() {
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());

  uint32_t interval = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;

  // The following timers are all skipped while sensitive services are
  // paused (Provisioning / ProvisioningConfirm).  Skipping the firing
  // branch — and not the read of _last_*_ms — means the deadline
  // effectively freezes; rebase_periodic_clocks() rolls _last_*_ms
  // forward on session leave so we don't fire back-to-back catch-up.
  if (!_provisioning_sensitive_services_paused) {
    // --- PM pre-wake timer (fires warmup_duration before next measurement) ---
    bool pm_sleep_eligible =
        _mode != OperatingMode::Offline && _svc.power_service.should_sleep_pm_sensor(interval);
    if (pm_sleep_eligible && !_pm_prepare_sent) {
      uint32_t measure_deadline = _last_measurement_ms + interval;
      uint32_t prepare_deadline = measure_deadline - CONFIG_SENSOR_WARMUP_DURATION_MS;
      if ((now - prepare_deadline) < MAX_REASONABLE_TIMEOUT_MS) {
        AG_LOGI(TAG, "PM pre-wake: powering on and requesting prepare");
        _svc.power_service.set_pm_power(true);
        _svc.sensor_producer.request_prepare();
        _pm_prepare_sent = true;
      }
    }

    // --- Sensor timer (single) ---
    if ((now - _last_measurement_ms) >= interval) {
      _svc.sensor_producer.request_measurement(1, SensorGroup::All);
      _last_measurement_ms = now;
      _pm_prepare_sent = false;
    }

    // --- BMS full telemetry timer ---
    if ((now - _last_bms_poll_ms) >= BMS_POLL_INTERVAL_MS) {
      on_bms_timer();
    }

    // --- BMS fast charging-status timer (between full polls) ---
    if ((now - _last_bms_status_poll_ms) >= BMS_STATUS_POLL_INTERVAL_MS) {
      on_bms_status_timer();
    }

    // --- Snackbar refresh timer ---
    // Snackbars are suppressed on session screens, so this is a no-op
    // there in any case; the explicit gate avoids spurious wake-ups.
    if (_snackbar_refresh_deadline_ms != 0 &&
        (now - _snackbar_refresh_deadline_ms) < MAX_REASONABLE_TIMEOUT_MS) {
      _snackbar_refresh_deadline_ms = 0;
      request_background_display_update();
    }
  }

  // --- External watchdog timer (never suppressed — must keep running
  // during the session or the device reboots mid-setup) ---
  if ((now - _last_ext_wdt_ms) >= EXT_WDT_INTERVAL_MS) {
    _svc.power_service.reset_ext_watchdog();
    _last_ext_wdt_ms = now;
  }

  // --- Auto-lock timer (suppressed across the entire setup session,
  // including Screen::Info where sensitive services keep running, and on
  // focus screens like PairingPasskey where a lock-and-redraw would
  // interrupt the user mid-action) ---
  if (_lock_state == LockState::Unlocked && _settings.auto_lock_seconds > 0 &&
      !_setup_session_active && !_svc.ui_manager.is_focus_screen()) {
    uint32_t inact_interval = static_cast<uint32_t>(_settings.auto_lock_seconds) * 1000;
    if ((now - _last_input_ms) >= inact_interval) {
      on_inactivity_timeout();
    }
  }

  // --- Wi-Fi service tick (clears expired deadline / fires synthetic disco) ---
  _svc.wifi.tick(now);

  // --- Portable provisioning radio-idle tick (drops the radio on timeout) ---
  _svc.portable_provisioner.tick(now);

  // --- Unified OTA poll (mode-selected interval/gate) ---
  // The blocking run_*()/finish_ota() below own the orchestrator task until the
  // transfer terminates; the main loop does not iterate meanwhile.
  if (_mode == OperatingMode::Stationary && _svc.wifi.is_online() && !_setup_session_active) {
    if ((now - _last_ota_check_ms) >= OTA_WIFI_CHECK_INTERVAL_MS) {
      _last_ota_check_ms = now;
      // Speculative check: don't quiesce yet. Only pause cloud + feed the
      // watchdog; enter_ota() + paint are deferred to on_ota_download_started().
      _ota_committed = false;
      _svc.cloud.disarm();
      _svc.power_service.reset_ext_watchdog();
      finish_ota(_svc.ota.run_wifi_check([this] { on_ota_download_started(); }));
    }
  } else if (_mode == OperatingMode::Portable &&
             (_svc.ble_service.is_authenticated() || _svc.ota.is_ble_active())) {
    if ((now - _last_ota_check_ms) >= OTA_BLE_POLL_INTERVAL_MS) {
      _last_ota_check_ms = now;
      if (_svc.ota.is_ble_active()) {
        // Latched START is a committed transfer — quiesce + paint up front.
        _ota_committed = false;
        enter_ota();
        paint_updating_firmware();
        _ota_committed = true;
        finish_ota(_svc.ota.run_ble());
      }
    }
  }
}

void Orchestrator::on_bms_timer() {
  log_heap(TAG, "bms.timer:tick");
  _latest_power = _svc.power_service.poll_bms(_first_measurement_done &&
                                              !_cached_measures.pm_a.is_pm_25_valid());
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  _last_bms_poll_ms = now;
  _last_bms_status_poll_ms = now; // Full poll subsumes the fast status check.

  if (_latest_power.ship_mode_request != ShipModeRequest::None) {
    shutdown(_latest_power.ship_mode_request);
    return; // system is shutting down — skip further processing
  }

  // Steady-state Status refresh (value only; urgent transitions push via notify_tracking_status()).
  if (_svc.ble_service.is_initialized()) {
    _svc.ble_service.update_status(_latest_power, _latest_gps, is_recording(),
                                   _tracking_session_id);
  }
}

void Orchestrator::on_bms_status_timer() {
  BmsStatus status{};
  if (_svc.power_service.poll_status(status)) {
    bool was_charging = is_bms_charging(_latest_power.charging_status);
    const BmsPowerSource previous_power_source = _latest_power.charger_status.power_source;
    _latest_power.charging_status = status.charging_state;
    _latest_power.charger_status = status;
    bool now_charging = is_bms_charging(status.charging_state);
    if (was_charging != now_charging || previous_power_source != status.power_source) {
      AG_LOGI(
          TAG, "charger status changed: charge=%s -> %s source=%s -> %s",
          was_charging ? "charging" : "not charging", now_charging ? "charging" : "not charging",
          bms_power_source_str(previous_power_source), bms_power_source_str(status.power_source));
      request_background_display_update();

      // Re-kick PMID boost if the unplug transition collapsed the rail.
      _svc.power_service.ensure_pmid_healthy();

      // Push the charging delta so clients reflect the change without polling
      // (also refreshes the full Status snapshot).
      if (_svc.ble_service.is_initialized()) {
        _svc.ble_service.notify_charging_status(_latest_power, _latest_gps, is_recording(),
                                                _tracking_session_id);
      }
    }
  }

  _last_bms_status_poll_ms = static_cast<uint32_t>(RTOS::get_time_ms());
}

void Orchestrator::on_inactivity_timeout() { lock(); }

void Orchestrator::reschedule_sensor_timer(const GoSettings &previous_settings) {
  if (previous_settings.measure_interval_seconds == _settings.measure_interval_seconds) {
    return;
  }
  _last_measurement_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // Reconcile PM state with the new interval.
  uint32_t new_interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (_mode != OperatingMode::Offline &&
      _svc.power_service.should_sleep_pm_sensor(new_interval_ms)) {
    _svc.sensor_producer.request_pm_sleep();
  } else {
    // Back to always-on: connect now, then wake + warm the (possibly asleep) sensor.
    _svc.power_service.set_pm_power(true);
    _svc.sensor_producer.request_prepare();
  }
}

// ---------------------------------------------------------------------------
// Event dispatch
// ---------------------------------------------------------------------------

void Orchestrator::dispatch(const Event &event) {
  switch (event.type) {
  case EventType::SensorDataReady:
    on_sensor_data(event.sensor_data);
    break;
  case EventType::PmSensorAsleep:
    // PM sleep finished while still connected — isolate the bus now.
    _svc.power_service.set_pm_power(false);
    break;
  case EventType::GpsFixUpdate:
    on_gps_fix(event.gps_data);
    break;
  case EventType::InputPress:
    on_input(event.input);
    break;

  // BLE events
  case EventType::BleConnected:
    on_ble_connected();
    break;
  case EventType::BleDisconnected:
    on_ble_disconnected();
    break;
  case EventType::BleConfigWrite:
    on_ble_config_write();
    break;
  case EventType::BleHistoryWrite:
    on_ble_history_write();
    break;
  case EventType::BlePairingRequest:
    on_ble_pairing_request(event.ble_passkey);
    break;
  case EventType::BleAuthComplete:
    on_ble_auth_complete(event.ble_auth_ok);
    break;

  // Calibration events
  case EventType::Co2CalibrationDone:
    on_co2_calibration_done(static_cast<Co2CalibrationResult>(event.co2_cal_result));
    break;

  // UI action events (reserved for future programmatic triggers)
  case EventType::UserStartTracking:
    start_tracking();
    break;
  case EventType::UserStopTracking:
    stop_tracking();
    break;
  case EventType::UserChangeMode:
    change_mode(event.mode_change);
    break;
  case EventType::UserToggleGps:
    _gps_enabled = event.gps_enabled;
    break;
  case EventType::SettingsChanged:
    apply_settings_change();
    break;
  case EventType::ClearData:
    clear_data();
    break;
  case EventType::SaveTag:
    save_tag(event.tag_index, nullptr);
    break;

  // System events
  case EventType::InactivityTimeout:
    on_inactivity_timeout();
    break;
  case EventType::MeasurementTimer:
    check_timers(); // legacy event: just re-check all timers
    break;
  case EventType::WakeFromSleep:
    break; // wake handled in init()

  // Wi-Fi
  case EventType::WifiConnected:
    on_wifi_connected(event.wifi_ip);
    break;
  case EventType::WifiDisconnected:
    on_wifi_disconnected(static_cast<WifiDisconnectReason>(event.wifi_disconnect_reason));
    break;

  case EventType::ProvisioningStateChanged:
    on_provisioning_state_changed(event.prov);
    break;

  case EventType::PortableProvRequest:
    _svc.portable_provisioner.handle_pending_request();
    break;

  case EventType::PostMeasuresResult:
    AG_LOGI(TAG, "post_measures result=%d", static_cast<int>(event.cloud_result));
    break;

  case EventType::FetchConfigResult:
    AG_LOGI(TAG, "fetch_config result=%d", static_cast<int>(event.cloud_result));
    break;
  }
}

// ---------------------------------------------------------------------------
// Event handlers
// ---------------------------------------------------------------------------

void Orchestrator::on_sensor_data(const MeasuresAGo &data) {
  // Always overwrite all fields — single interval, no group-based gating.
  _cached_measures.pm_a = data.pm_a;
  _cached_measures.co2 = data.co2;
  _cached_measures.temp_hum_a = data.temp_hum_a;
  _cached_measures.tvoc_nox = data.tvoc_nox;
  _cached_measures.pressure = data.pressure;
  _cached_measures.power.battery_voltage = _latest_power.battery_voltage;
  _cached_measures.power.charging_voltage = _latest_power.charging_voltage;

  _first_measurement_done = true;

  // Back AQI LEDs — show color for valid PM2.5, off for invalid.
  if (_cached_measures.pm_a.is_pm_25_valid()) {
    _svc.led_service.back_update_aqi(_cached_measures.pm_a.pm_25);
  } else {
    _svc.led_service.back_clear_aqi();
  }

  // Hand off the "Booting..." splash. Skip if a setup session owns Info
  // (Stationary drives its own Info -> Home). First-boot gate diverts to
  // the guide until onboarding is acked.
  if (_boot_splash_active) {
    _boot_splash_active = false;
    if (!_setup_session_active && _svc.ui_manager.current_screen() == Screen::Info) {
      if (!_settings.onboarding_done) {
        // begin_session_if_needed() silent-unlocks so the Locked device can
        // press "Start using"; sensors/BLE keep running (no service pause).
        begin_session_if_needed();
        _svc.ui_manager.show_getting_started(/*from_boot=*/true);
        update_display(/*wait=*/true);
      } else {
        _svc.ui_manager.reset_to_home();
      }
    }
  }

  log_sensor_snapshot(_cached_measures);

  _svc.storage_service.cache_measurement(_cached_measures);

  // Unconditional — snapshot is ready for the next Stationary arm.
  _svc.cloud.update_measures_snapshot(_cached_measures);

  if (_tracking_active) {
    RoutePoint p{};
    p.timestamp = time(nullptr);
    p.gps = _latest_gps;
    p.sensors = _cached_measures;
    p.battery_percentage = _latest_power.battery_percentage;
    // Failures are logged by the storage layer; the session keeps running
    // and re-attempts on each subsequent measurement.
    (void)_svc.storage_service.append_route_point(p);
  }

  // Update BLE measures characteristic (always for READ; notifies when connected)
  _svc.ble_service.notify_measures(_cached_measures, _latest_gps, time(nullptr));

  // Sleep PM sensor after measurement when interval justifies power-cycling.
  // The producer sleeps the sensor, then posts PmSensorAsleep so we isolate.
  uint32_t interval_ms = static_cast<uint32_t>(_settings.measure_interval_seconds) * 1000;
  if (_mode != OperatingMode::Offline && _svc.power_service.should_sleep_pm_sensor(interval_ms)) {
    _svc.sensor_producer.request_pm_sleep();
  }

  request_background_display_update();
}

void Orchestrator::on_co2_calibration_done(Co2CalibrationResult result) {
  switch (result) {
  case Co2CalibrationResult::Success:
    AG_LOGI(TAG, "CO2 calibration succeeded");
    _svc.ui_manager.show_snackbar("CO2 cal. done");
    _svc.ble_service.notify_command_result(BleCommand::Co2Calibration, true);
    break;
  case Co2CalibrationResult::Unsupported:
    AG_LOGW(TAG, "CO2 calibration unsupported by sensor");
    _svc.ui_manager.show_snackbar("CO2 cal. unsupported");
    _svc.ble_service.notify_command_result(BleCommand::Co2Calibration, false,
                                           BLE_VAL_ERR_UNSUPPORTED);
    break;
  case Co2CalibrationResult::Failed:
    AG_LOGW(TAG, "CO2 calibration failed");
    _svc.ui_manager.show_snackbar("CO2 cal. failed");
    _svc.ble_service.notify_command_result(BleCommand::Co2Calibration, false,
                                           BLE_VAL_ERR_CALIBRATION_FAILED);
    break;
  }
  update_display();
}

void Orchestrator::on_gps_fix(const GpsData &data) {
  if (!is_gps_active()) {
    return; // GPS disabled in settings; ignore
  }
  _latest_gps = data;

  AG_LOGI(TAG, "gps_fix: lat=%.6f lon=%.6f alt=%.1f fix=%d sat=%d hdop=%.1f",
          data.position.latitude, data.position.longitude, data.altitude_m,
          static_cast<int>(data.fix.fix_type), data.fix.satellite_count, data.fix.hdop);
}

static const char *input_source_str(InputSource s) {
  switch (s) {
  case InputSource::TouchUp:
    return "TouchUp";
  case InputSource::TouchDown:
    return "TouchDown";
  case InputSource::TouchEnter:
    return "TouchEnter";
  case InputSource::ButtonPower:
    return "BtnPower";
  case InputSource::ButtonBoot:
    return "BtnBoot";
  default:
    return "Unknown";
  }
}

static const char *input_type_str(InputType t) {
  switch (t) {
  case InputType::ShortPress:
    return "short";
  case InputType::LongPress:
    return "long";
  case InputType::DoublePress:
    return "double";
  default:
    return "unknown";
  }
}

void Orchestrator::on_input(const InputEventData &input) {
  AG_LOGI(TAG, "input: source=%s type=%s lock=%s", input_source_str(input.source),
          input_type_str(input.type), _lock_state == LockState::Locked ? "locked" : "unlocked");

  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // Touch LED flash — fires for any accepted touch event regardless of
  // lock state.  ButtonPower and ButtonBoot do not trigger flashes.
  switch (input.source) {
  case InputSource::TouchEnter:
    _svc.led_service.touch_flash(TouchPad::Select);
    break;
  case InputSource::TouchUp:
    _svc.led_service.touch_flash(TouchPad::Left);
    break;
  case InputSource::TouchDown:
    _svc.led_service.touch_flash(TouchPad::Right);
    break;
  default:
    break;
  }

  // TouchEnter long-press (exit to Home) and double-press (back one level) are
  // handled in UIManager::handle_input below, gated by the lock check.

  // Shutdown: long press on power button (any lock state)
  if (input.source == InputSource::ButtonPower && input.type == InputType::LongPress) {
    shutdown();
    return;
  }

  // Factory reset: long press on boot button
  if (input.source == InputSource::ButtonBoot && input.type == InputType::LongPress) {
    if (factory_reset()) {
      AG_LOGI(TAG, "Rebooting in 2s");
      RTOS::delay_ms(2000);
      reboot();
    }
    return;
  }

  // Arming: a second boot short-press while already in manufacturing mode
  // persists a fuel-gauge learning run and reboots into the dedicated factory
  // path. This is the orchestrator's ONLY learning touch point — no tick, no
  // resume, no verify, no ship hook, no dashboard.
  if (input.source == InputSource::ButtonBoot && input.type == InputType::ShortPress &&
      _manufacturing_mode) {
    AG_LOGI(TAG, "arming fuel-gauge learning run -> reboot into factory path");
    save_factory_settings(_config_store, FactorySettings{FgLearningStage::Charge, 1, 0});
    RTOS::delay_ms(500);
    reboot();
    return;
  }

  // Manufacturing: boot short-press before onboarding skips the guide and
  // enters Stationary ephemerally, so production can re-test a fresh unit.
  if (input.source == InputSource::ButtonBoot && input.type == InputType::ShortPress &&
      !_settings.onboarding_done) {
    enter_manufacturing_mode();
    return;
  }

  // Power short-press: lock toggle is suppressed on session screens so a
  // mid-setup press cannot lock the device and hide the on-phone
  // instructions the user is following.  Power long-press shutdown (above)
  // and Boot long-press factory-reset (above) remain functional as
  // recovery paths.
  if (input.source == InputSource::ButtonPower && input.type == InputType::ShortPress) {
    if (_setup_session_active || _boot_splash_active) {
      return; // suppressed on setup-session screens and during cold-boot splash
    }
    if (_lock_state == LockState::Locked) {
      unlock();
    } else {
      lock();
    }
    return;
  }

  // Locked: show hint on touch input, ignore otherwise
  if (_lock_state == LockState::Locked) {
    if (input.source == InputSource::TouchUp || input.source == InputSource::TouchDown ||
        input.source == InputSource::TouchEnter) {
      _svc.ui_manager.show_snackbar("Unlock First");
      update_display();
    }
    return;
  }

  // Unlocked: forward to UI Manager
  UIActionResult result = _svc.ui_manager.handle_input(input.source, input.type);

  switch (result.action) {
  case UIAction::StartTracking:
    start_tracking();
    break;
  case UIAction::StopTracking:
    stop_tracking();
    break;
  case UIAction::ChangeMode:
    change_mode(result.new_mode);
    break;
  case UIAction::SettingsChanged:
    apply_settings_change();
    break;
  case UIAction::ClearData:
    clear_data();
    break;
  case UIAction::CalibrateCo2:
    _svc.sensor_producer.request_co2_calibration();
    _svc.ui_manager.show_snackbar("Calibrating CO2...");
    break;
  case UIAction::SaveTag:
    save_tag(result.tag_index, result.tag_label);
    break;
  case UIAction::PlayMelody: {
    // Suppress input during blocking synced play so accumulated
    // touch/button events don't fire after the melody returns.
    // Temporarily force buzzer on so the preview always plays.
    _svc.input_service.pause();
    _svc.buzzer_service.set_enabled(true);
    play_synced(_svc.buzzer_service, _svc.led_service, result.melody);
    _svc.buzzer_service.set_enabled(_settings.buzzer_enabled);
    _svc.input_service.resume();
    // Restore back-LED brightness and AQI state.
    _svc.led_service.back_set_brightness(_settings.back_led_brightness);
    if (_cached_measures.pm_a.is_pm_25_valid()) {
      _svc.led_service.back_update_aqi(_cached_measures.pm_a.pm_25);
    } else {
      _svc.led_service.back_clear_aqi();
    }
    break;
  }
  case UIAction::AckOnboarding:
    // "Start using": persist the flag and leave the session to Home.
    mark_onboarding_done();
    leave_session_to_home();
    return;
  case UIAction::ConfirmCancelProvisioning:
    // Cancel-setup confirmed — drop back to Portable via the session
    // leave path so battery / clocks / snackbar state are all restored
    // cleanly.  The helper does its own update_display + flush, so we
    // return before the catch-all tail render below.
    leave_session_to_portable();
    return;
  case UIAction::ConfirmSwitchProvisioningTransport:
    // Latch the transient "Switching to ..." state on the page first so
    // the user sees an ack before switch_provisioning_transport()'s
    // back-to-back stop()+start() fires further status events that
    // would overwrite the transient frame.  wait=true + flush() guarantee
    // the ack is painted before the transport flips.  Returns early to
    // bypass the catch-all tail render.
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::SwitchingTransport);
    update_display(/*wait=*/true);
    _svc.display_service.flush();
    _svc.wifi.switch_provisioning_transport();
    return;
  case UIAction::None:
    break;
  }

  // wait=true: handle_input() already advanced the model, so a dropped paint
  // (worker mid-refresh) leaves the screen stale until the next input. Queue
  // drop-free instead.
  update_display(/*wait=*/true);
}

// ---------------------------------------------------------------------------
// State transitions
// ---------------------------------------------------------------------------

void Orchestrator::lock() {
  AG_LOGI(TAG, "lock");
  _svc.ui_manager.show_snackbar("Locked");
  _lock_state = LockState::Locked;
  _svc.ui_manager.reset_to_home();
  update_display();
}

void Orchestrator::unlock() {
  AG_LOGI(TAG, "unlock");
  _svc.ui_manager.show_snackbar("Unlocked");
  _lock_state = LockState::Unlocked;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());
  update_display();
}

bool Orchestrator::start_tracking() {
  if (_tracking_active) {
    return false; // caller treats as "already tracking"
  }

  AG_LOGI(TAG, "start_tracking");

  const bool was_gps_active = is_gps_active(); // snapshot before any state flip

  const uint32_t session_id = generate_session_id();
  if (session_id == 0 || !_svc.storage_service.create_route(session_id)) {
    AG_LOGE(TAG, "start_tracking: failed to open route (session=%" PRIu32 ")", session_id);
    _svc.ui_manager.show_snackbar("Storage error — can't track");
    update_display();
    _svc.ble_service.notify_tracking_status(_latest_power, _latest_gps, is_recording(),
                                            _svc.storage_service.current_route_session_id());
    return false;
  }

  _tracking_session_id = session_id;
  _tracking_active = true;
  _behavior = Behavior::Tracking;

  if (!was_gps_active && is_gps_active()) {
    _svc.gps_service.start();
  }

  char msg[48];
  (void)snprintf(msg, sizeof(msg), "Tracking start = %05" PRIu32, _tracking_session_id);
  _svc.ui_manager.show_snackbar(msg);
  update_display();
  _svc.ble_service.notify_tracking_status(_latest_power, _latest_gps, is_recording(),
                                          _tracking_session_id);
  return true;
}

void Orchestrator::stop_tracking() {
  if (!_tracking_active) {
    return;
  }

  AG_LOGI(TAG, "stop_tracking");
  const bool was_gps_active = is_gps_active();
  const uint32_t ended_session_id = _tracking_session_id;
  _svc.storage_service.end_route();
  _tracking_active = false;
  _tracking_session_id = 0;
  _behavior = Behavior::Idle;

  if (was_gps_active && !is_gps_active()) {
    deactivate_gps();
  }

  char msg[48];
  (void)snprintf(msg, sizeof(msg), "Tracking stop = %05" PRIu32, ended_session_id);
  _svc.ui_manager.show_snackbar(msg);
  update_display();
  _svc.ble_service.notify_tracking_status(_latest_power, _latest_gps, is_recording(),
                                          _tracking_session_id);
}

void Orchestrator::mark_onboarding_done() {
  if (_settings.onboarding_done) {
    return; // idempotent — no redundant write
  }
  AG_LOGI(TAG, "mark_onboarding_done");
  _settings.onboarding_done = true;
  save_go_settings(_config_store, _settings);
}

void Orchestrator::enter_manufacturing_mode() {
  AG_LOGI(TAG, "enter_manufacturing_mode: skip onboarding, Stationary (ephemeral)");
  _manufacturing_mode = true;
  change_mode(OperatingMode::Stationary, /*persist=*/false);
}

void Orchestrator::change_mode(OperatingMode new_mode, bool persist) {
  OperatingMode old_mode = _mode;
  AG_LOGI(TAG, "change_mode: %d -> %d (persist=%d)", static_cast<int>(old_mode),
          static_cast<int>(new_mode), persist);
  if (persist) {
    mark_onboarding_done(); // mode change implies engagement
  }
  log_heap(TAG, "mode.change:enter");
  _mode = new_mode;
  _settings.operating_mode = new_mode;
  if (persist) {
    save_go_settings(_config_store, _settings);
  }
  // Keep UIManager's cached _setting_mode index in lockstep with the
  // persisted GoSettings::operating_mode.  Without this, paths that change
  // the mode without going through apply_setting_choice (e.g. the
  // cancel-from-provisioning leave routing through change_mode(Portable))
  // leave the Settings menu showing the previously-selected option.
  _svc.ui_manager.sync_settings(_settings);

  // Two-phase: tear down outgoing mode, then bring up incoming.
  if (old_mode == OperatingMode::Portable && new_mode != OperatingMode::Portable) {
    // BLE is only up in Portable. Tell a connected client the link is dropping
    // (and why) and let the notice drain before teardown — notifications are
    // fire-and-forget (no completion signal), so a brief settle covers a few
    // connection intervals. Covers UI-, event-, and BLE-initiated mode changes.
    if (_svc.ble_service.is_connected()) {
      _svc.ble_service.notify_disconnect(disc_reason_for_mode(new_mode));
      RTOS::delay_ms(BLE_MODE_CHANGE_NOTIFY_SETTLE_MS);
    }
    _svc.ui_manager.dismiss_pairing_passkey();
    // Drop the OTA observer + GATT registration before the server is released
    // (idempotent; no-op if OTA was never registered).
    _svc.ble_service.set_disconnect_observer(nullptr);
    _svc.ota.teardown_ble();
    // Mandatory order: release the server before BleService deinitialises it.
    _svc.portable_provisioner.stop();
    _svc.ble_service.deinit();
  }
  if (old_mode == OperatingMode::Stationary && new_mode != OperatingMode::Stationary) {
    // Cloud before Wi-Fi: drain in-flight HTTP while socket is alive.
    _svc.cloud.disarm();
    _svc.cloud.stop();
    _svc.wifi.shutdown();
    resume_provisioning_sensitive_services();
    _cloud_first_post_pending = false;
  }
  log_heap(TAG, "mode.change:after-teardown");

  if (new_mode == OperatingMode::Portable && old_mode != OperatingMode::Portable) {
    init_ble_if_portable();
  }

  // Ensure PM sensor is connected and awake — covers mode changes away
  // from Portable while PM was slept/isolated.  Must fire before the
  // Stationary early-return below so Stationary entry restores PM after a
  // prior Portable session may have slept it.
  _svc.power_service.set_pm_power(true);
  _svc.sensor_producer.request_prepare();

  if (new_mode == OperatingMode::Stationary && old_mode != OperatingMode::Stationary) {
    // enter_stationary() opens Screen::Info with the bring-up text and
    // calls update_display(wait=true) itself.  Skip the generic
    // "Mode changed" snackbar + update_display() so the Info text is
    // not stomped.
    enter_stationary();
    return;
  }

  _svc.ui_manager.show_snackbar("Mode changed");
  update_display();
  log_heap(TAG, "mode.change:after-bringup");
}

void Orchestrator::apply_settings_change() {
  const bool was_gps_active = is_gps_active();
  const GoSettings previous_settings = _settings;
  _svc.ui_manager.apply_to_settings(_settings);
  save_go_settings(_config_store, _settings);

  // Propagate runtime changes to services
  reschedule_sensor_timer(previous_settings);
  _svc.gps_service.set_posting_interval_ms(_settings.gps_interval_seconds * 1000);

  if (_settings.disable_cloud != previous_settings.disable_cloud) {
    _svc.cloud.set_disable_cloud(_settings.disable_cloud);
  }

  const bool is_gps_active_now = is_gps_active();
  if (!was_gps_active && is_gps_active_now) {
    _svc.gps_service.start();
  } else if (was_gps_active && !is_gps_active_now) {
    deactivate_gps();
  }

  _gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff);

  // Apply LED settings immediately
  _svc.led_service.front_set_brightness(_settings.front_led_brightness);
  _svc.led_service.back_set_brightness(_settings.back_led_brightness);
  _svc.led_service.touch_set_intensity(_settings.touch_led_intensity);

  // Apply buzzer setting
  _svc.buzzer_service.set_enabled(_settings.buzzer_enabled);

  // Notify connected BLE client of config change. notify_config() refreshes
  // the stored snapshot (READ) internally before sending the delta.
  if (_svc.ble_service.is_connected()) {
    _svc.ble_service.notify_config(previous_settings, _settings);
  }
}

bool Orchestrator::clear_data() {
  if (_tracking_active) {
    stop_tracking();
  }

  _svc.storage_service.clear_cache();
  const bool routes_cleared = _svc.storage_service.clear_routes();

  if (_svc.ble_service.is_connected()) {
    _svc.ble_service.update_status(_latest_power, _latest_gps, is_recording(),
                                   _tracking_session_id);
  }

  _svc.ui_manager.show_snackbar(routes_cleared ? "Data cleared" : "Data clear failed");
  update_display();

  return routes_cleared;
}

bool Orchestrator::factory_reset() {
  AG_LOGI(TAG, "factory_reset");

  // Erase temporary cache data and delete all persisted route files.
  const bool data_cleared = clear_data();

  const GoSettings defaults{};

  // Overwrite persisted product settings with their default values.
  // Zeros disable_cloud + static_ip as a side effect.
  const bool settings_saved = save_go_settings(_config_store, defaults);

  // Erase all saved networks and reset online latches.
  _svc.wifi.clear_credentials();

  // Delete all stored BLE bond information.
  const bool bonds_cleared = _svc.ble_service.delete_all_bonds();

  const bool success = data_cleared && settings_saved && bonds_cleared;

  if (!success) {
    _svc.ui_manager.show_snackbar("Factory reset failed");
    update_display();
    return false;
  }

  AG_LOGI(TAG, "Factory reset success");

  _settings = defaults;
  _mode = _settings.operating_mode;
  _behavior = Behavior::Idle;
  _lock_state = LockState::Locked;
  _gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff);
  _tracking_active = false;
  _tracking_session_id = 0;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  _svc.ui_manager.sync_settings(_settings);
  _svc.ui_manager.reset_to_home();
  update_display();

  return true;
}

void Orchestrator::save_tag(uint8_t tag_index, const char *tag_label) {
  (void)tag_index;
  (void)tag_label;
  // TODO: persist tag association with current route point via StorageService
  _svc.ui_manager.show_snackbar("Tag saved");
  update_display();
}

void Orchestrator::shutdown(ShipModeRequest reason) {
  AG_LOGI(TAG, "shutdown (reason=%d)", static_cast<int>(reason));

  // Manufacturing units ship clean: wipe any settings / Wi-Fi / bonds the
  // production team changed while testing.
  if (_manufacturing_mode) {
    AG_LOGI(TAG, "shutdown: manufacturing mode — factory reset before power off");
    factory_reset();
  }

  // Tell a connected BLE client the link is about to drop (and why). Sent
  // early so the fire-and-forget notice drains before BMS ship mode cuts power.
  if (_svc.ble_service.is_connected()) {
    _svc.ble_service.notify_disconnect(disc_reason_for_shutdown(reason));
  }

  // 1. Map shutdown reason to the matching screen variant.
  Screen screen;
  switch (reason) {
  case ShipModeRequest::OverDischarge:
    screen = Screen::ShutdownDischarge;
    break;
  case ShipModeRequest::OverTemperature:
    screen = Screen::ShutdownTemperature;
    break;
  case ShipModeRequest::None:
  default:
    screen = Screen::ShutdownUser;
    break;
  }
  _svc.ui_manager.set_screen(screen);
  update_display(true); // queue shutdown frame without dropping it
  _svc.display_service.flush();

  // 2. Persist state.
  if (_tracking_active) {
    stop_tracking();
  }
  _svc.storage_service.backup_cache();

  // 3. Disable peripherals to reduce power draw before final shutdown.
  _svc.power_service.set_pm_power(false);
  // TODO: stop GPS when the method is available

  // 4. Slow down before power cut so the painted reason screen is visible and
  // the BLE disconnect notice can drain.
  RTOS::delay_ms(SHUTDOWN_POWER_OFF_SETTLE_MS);

  // 5. Ship mode → deep sleep fallback.
  _svc.power_service.shutdown(); // BMS QoN — does not return
}

// ---------------------------------------------------------------------------
// BLE event handlers
// ---------------------------------------------------------------------------

void Orchestrator::on_ble_connected() {
  AG_LOGI(TAG, "BLE client connected");

  // Dismiss pairing passkey screen if it was showing
  _svc.ui_manager.dismiss_pairing_passkey();

  // Seed characteristics for the new client (Read is authoritative on connect).
  _svc.ble_service.notify_measures(_cached_measures, _latest_gps, time(nullptr));
  _svc.ble_service.update_status(_latest_power, _latest_gps, is_recording(), _tracking_session_id);
  _svc.ble_service.update_config(_settings);

  request_background_display_update();
}

void Orchestrator::on_ble_disconnected() {
  AG_LOGI(TAG, "BLE client disconnected");
  // The provisioner has no transport disconnect callback; forward so it
  // drops the radio.
  _svc.portable_provisioner.on_ble_disconnected();
  _svc.ui_manager.dismiss_pairing_passkey();
  request_background_display_update();
}

void Orchestrator::on_ble_auth_complete(bool success) {
  AG_LOGI(TAG, "BLE auth complete: %s", success ? "OK" : "FAILED");

  // Only an encrypted pairing counts as engagement; a failed/empty-PIN attempt
  // leaves onboarding untouched so a retry re-shows a fresh PIN.
  if (success) {
    mark_onboarding_done();
  }

  if (_setup_session_active) {
    if (success) {
      leave_session_to_home();
    } else {
      // Back to the first-boot guide (session is boot-originated); keep the
      // session so a retry can succeed or "Start using" can dismiss.
      _svc.ui_manager.show_getting_started(/*from_boot=*/true);
      update_display(/*wait=*/true);
    }
    return;
  }

  _svc.ui_manager.dismiss_pairing_passkey();
  request_background_display_update();
}

void Orchestrator::on_ble_config_write() {
  uint8_t buf[BLE_WRITE_BUF_SIZE];
  size_t len = _svc.ble_service.take_pending_config_write(buf, sizeof(buf));
  if (len == 0) {
    return;
  }

  // Decode into a copy of current settings (merge approach)
  GoSettings temp = _settings;
  BleConfigDecodeResult result = BleService::decode_config_write(buf, len, temp);

  // A GATT write stores the raw written bytes as the characteristic value, so a
  // READ would otherwise echo the write (a rejected "set", or an "op:cmd"
  // payload). Re-assert the config snapshot as the stored READ value. The "set"
  // success path refreshes it again via notify_config(); command and reject
  // paths rely on this call to keep READ returning the config snapshot.
  _svc.ble_service.update_config(_settings);

  // Reject before adopting any value, in precedence order (first match wins):
  // 1. unknown config key, 2. more than one config field per write (NOTIFY
  // deltas are bounded to a single field per event).
  if (result.op == BleConfigOp::Set && result.has_unknown_keys) {
    AG_LOGW(TAG, "BLE config set rejected: unknown config key");
    _svc.ble_service.notify_command_result(BleCommand::Set, false, BLE_VAL_ERR_UNKNOWN_CONFIG_KEY);
    return;
  }
  if (result.op == BleConfigOp::Set && result.recognized_config_key_count > 1) {
    AG_LOGW(TAG, "BLE config set rejected: single field only");
    _svc.ble_service.notify_command_result(BleCommand::Set, false, BLE_VAL_ERR_SINGLE_FIELD_ONLY);
    return;
  }

  switch (result.op) {
  case BleConfigOp::Set: {
    AG_LOGI(TAG, "BLE config set");
    const bool was_gps_active = is_gps_active();
    const GoSettings previous_settings = _settings;
    _settings = temp;
    save_go_settings(_config_store, _settings);

    // Propagate runtime changes
    reschedule_sensor_timer(previous_settings);
    _svc.gps_service.set_posting_interval_ms(_settings.gps_interval_seconds * 1000);
    _svc.ui_manager.sync_settings(_settings);

    const bool mode_changing = _settings.operating_mode != _mode;

    // Notify BLE client of the change. notify_config() refreshes the stored
    // snapshot (READ) internally before sending the single-field delta. An
    // op_mode change drops the BLE link instead, so change_mode() announces it
    // via a `disc` Status notice; skip the Config delta here for that case.
    if (!mode_changing) {
      _svc.ble_service.notify_config(previous_settings, _settings);
    }

    const bool is_gps_active_now = is_gps_active();
    if (!was_gps_active && is_gps_active_now) {
      _svc.gps_service.start();
    } else if (was_gps_active && !is_gps_active_now) {
      deactivate_gps();
    }
    _gps_enabled = (_settings.gps_mode != GpsMode::AlwaysOff);

    if (mode_changing) {
      change_mode(_settings.operating_mode);
    }

    request_background_display_update();
    break;
  }
  case BleConfigOp::Command: {
    AG_LOGI(TAG, "BLE command: %d", static_cast<int>(result.cmd));

    switch (result.cmd) {
    case BleCommand::Co2Calibration:
      // Runs asynchronously on the SensorProducer task.
      // Result arrives via Co2CalibrationDone event.
      _svc.ble_service.notify_command_progress(result.cmd);
      _svc.sensor_producer.request_co2_calibration();
      break;
    case BleCommand::ClearData: {
      _svc.ble_service.notify_command_progress(result.cmd);
      const bool cleared = clear_data();
      _svc.ble_service.notify_command_result(result.cmd, cleared,
                                             cleared ? nullptr : BLE_VAL_ERR_CLEAR_FAILED);
    } break;
    case BleCommand::FactoryReset: {
      _svc.ble_service.notify_command_progress(result.cmd);
      const bool reset = factory_reset();
      _svc.ble_service.notify_command_result(result.cmd, reset,
                                             reset ? nullptr : BLE_VAL_ERR_FACTORY_RESET_FAILED);
      if (reset) {
        AG_LOGI(TAG, "Rebooting in 2s");
        RTOS::delay_ms(2000);
        reboot();
      }
    } break;
    case BleCommand::StartTracking: {
      if (_tracking_active) {
        _svc.ble_service.notify_command_result(result.cmd, false, BLE_VAL_ERR_ALREADY_TRACKING);
        break;
      }
      // start_tracking() already fired the inline snackbar + Status notify
      // on failure; just propagate the bool to the command result.
      const bool ok = start_tracking();
      _svc.ble_service.notify_command_result(result.cmd, ok,
                                             ok ? nullptr : BLE_VAL_ERR_FLASH_ERROR);
    } break;
    case BleCommand::StopTracking: {
      const bool was_tracking = _tracking_active;
      stop_tracking();
      _svc.ble_service.notify_command_result(result.cmd, was_tracking,
                                             was_tracking ? nullptr : BLE_VAL_ERR_NOT_TRACKING);
    } break;
    case BleCommand::SetAiding: {
      const bool has_time = has_aiding_time(result.aiding);
      const bool has_data = has_aiding_position(result.aiding) || has_time;
      if (has_data) {
        _svc.gps_service.set_aiding_data(result.aiding);
      }
      if (has_time) {
        // GPSService might delay the execution, hence if time available, still can sync it
        RTOS::set_system_time_from_epoch(result.aiding.epoch_s);
      }
      _svc.ble_service.notify_command_result(result.cmd, has_data,
                                             has_data ? nullptr : BLE_VAL_ERR_NO_AIDING_DATA);
    } break;
    case BleCommand::Set:
      // Not reachable via Command op — handled above as config-set rejection
      break;
    case BleCommand::Unknown:
      AG_LOGW(TAG, "BLE unknown command");
      _svc.ble_service.notify_command_result(result.cmd, false, BLE_VAL_ERR_UNKNOWN_COMMAND);
      break;
    }
    break;
  }
  case BleConfigOp::Invalid:
    AG_LOGW(TAG, "BLE config write: invalid CBOR");
    break;
  }
}

void Orchestrator::on_ble_history_write() {
  uint8_t buf[BLE_WRITE_BUF_SIZE];
  size_t len = _svc.ble_service.take_pending_history_write(buf, sizeof(buf));
  if (len == 0) {
    return;
  }

  BleHistoryDecodeResult result = BleService::decode_history_write(buf, len);

  // Reject the blocking export reads while the provisioning radio is active
  // so provisioning notifies aren't starved; control/cleanup ops still run.
  if (_svc.portable_provisioner.is_radio_active() &&
      (result.op == BleHistoryOp::List || result.op == BleHistoryOp::Start ||
       result.op == BleHistoryOp::Fill)) {
    AG_LOGW(TAG, "history export rejected — provisioning radio active");
    _svc.ble_service.notify_history_error(BLE_VAL_ERR_BUSY);
    return;
  }

  switch (result.op) {
  case BleHistoryOp::List:
    AG_LOGI(TAG, "BLE history: list");
    _svc.ble_service.handle_history_list();
    break;
  case BleHistoryOp::Start:
    AG_LOGI(TAG, "BLE history: start session %" PRIu32, result.session_id);
    _svc.ble_service.handle_history_start(result.session_id);
    break;
  case BleHistoryOp::Fill:
    AG_LOGI(TAG, "BLE history: fill %u points", static_cast<unsigned>(result.point_count));
    _svc.ble_service.handle_history_fill(result.point_indices, result.point_count);
    break;
  case BleHistoryOp::End:
    AG_LOGI(TAG, "BLE history: end");
    _svc.ble_service.handle_history_end();
    break;
  case BleHistoryOp::Delete:
    AG_LOGI(TAG, "BLE history: delete session %" PRIu32, result.session_id);
    if (_tracking_active && _tracking_session_id == result.session_id) {
      _svc.ble_service.notify_history_error(BLE_VAL_ERR_SESSION_ACTIVE);
    } else {
      _svc.ble_service.handle_history_delete(result.session_id);
      _svc.ble_service.update_status(_latest_power, _latest_gps, is_recording(),
                                     _tracking_session_id);
    }
    break;
  case BleHistoryOp::Invalid:
    AG_LOGW(TAG, "BLE history write: invalid CBOR");
    break;
  }
}

void Orchestrator::on_ble_pairing_request(uint32_t passkey) {
  AG_LOGI(TAG, "BLE pairing request: passkey=%06" PRIu32, passkey);
  _svc.ui_manager.show_pairing_passkey(passkey);
  update_display();
}

void Orchestrator::init_ble_if_portable() {
  if (_mode != OperatingMode::Portable) {
    return;
  }

  if (_svc.ble_service.is_initialized()) {
    return; // already running
  }

  log_heap(TAG, "ble.init:pre");
  // Two-phase init so the provisioner can register prov + DIS before
  // advertising (all GATT services must be registered first).
  if (!_svc.ble_service.init_stack_and_register(_serial)) {
    AG_LOGE(TAG, "BLE stack init/register failed");
    return;
  }
  if (!_svc.portable_provisioner.attach()) {
    AG_LOGW(TAG, "portable provisioner attach failed — advertising data service only");
  }

  // Co-register OTA between the register phase and advertising. Non-fatal on
  // failure (mirrors attach()): drop any partial registration, advertise without OTA.
  bool ota_ready = _svc.ota.setup_ble();
  if (!ota_ready) {
    AG_LOGW(TAG, "OTA setup failed — advertising without OTA");
    _svc.ota.teardown_ble(); // idempotent
  } else {
    // Forward central disconnects to OTA synchronously (host task) so an
    // in-flight transfer aborts before advertising restarts.
    _svc.ble_service.set_disconnect_observer(
        [this](uint16_t, int) { _svc.ota.handle_disconnect(); });
  }

  if (!_svc.ble_service.start_advertising()) {
    AG_LOGE(TAG, "BLE start_advertising failed");
    if (ota_ready) {
      // Roll back so neither the observer nor OTA outlives the released server.
      _svc.ble_service.set_disconnect_observer(nullptr);
      _svc.ota.teardown_ble();
    }
    _svc.portable_provisioner.stop(); // release the (now deinit'd) server
  }
  log_heap(TAG, "ble.init:post");
}

// ---------------------------------------------------------------------------
// Stationary Wi-Fi
// ---------------------------------------------------------------------------

void Orchestrator::enter_stationary() {
  log_heap(TAG, "wifi.enter-stationary:enter");
  // Idempotent — cheap no-op on warm Stationary re-entry. Portable-only
  // boots never reach this line.
  _svc.board.init_wifi_subsystem();
  log_heap(TAG, "wifi.enter-stationary:after-init");

  // Silent unlock + snackbar clear.  Required so a cold-boot Locked
  // device can interact with the session screens (Info / Provisioning),
  // and so leftover snackbars cannot leak onto session screens or fire
  // when we eventually return to Home.  Idempotent — see
  // begin_session_if_needed().
  begin_session_if_needed();

  _bring_up_pending = true;

  // Configure cloud state now; defer start() to the first-online callback
  // so the heap-heavy task doesn't exist during provisioning.
  _cloud_first_post_pending = true;
  _svc.cloud.set_disable_cloud(_settings.disable_cloud);

  // Seed the OTA baseline a full interval in the past so the first WiFi check
  // is due as soon as the connection settles, not one hour after entry.
  _last_ota_check_ms = static_cast<uint32_t>(RTOS::get_time_ms()) - OTA_WIFI_CHECK_INTERVAL_MS;

  if (_svc.wifi.has_saved_networks()) {
    const WifiStaticIpConfig *ip = _settings.static_ip.ip != 0 ? &_settings.static_ip : nullptr;
    AG_LOGI(TAG, "stationary: saved credentials %s static IP", ip != nullptr ? "with" : "without");
    _svc.ui_manager.show_info("Connecting to saved Wi-Fi...");
    _svc.wifi.connect_with_saved_credentials(ip);
  } else {
    AG_LOGI(TAG, "stationary: no credentials — trying default fallback");
    _svc.ui_manager.show_info("Trying default Wi-Fi...");
    _svc.wifi.try_default_fallback_credentials();
  }

  // Full refresh — entering the setup session boundary.  wait=true so
  // the Info frame is queued even if the worker is still painting a
  // prior frame.
  update_display(/*wait=*/true);
}

void Orchestrator::begin_session_if_needed() {
  if (_setup_session_active) {
    return; // Already inside the session (Info -> Provisioning transition).
  }
  _setup_session_active = true;

  // Silent unlock — no snackbar, no display side effect.  Guarantees a
  // cold-boot Locked device can interact with the session screens, and
  // prevents the leave-to-Home transition from firing the "Unlocked"
  // snackbar on success.
  _lock_state = LockState::Unlocked;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  // Clear any pending snackbar so a leftover "Mode changed", "Locked",
  // "Unlocked", or stale "Wi-Fi connected" cannot leak onto the session
  // screens or fire when the device eventually returns to Home.
  _svc.ui_manager.show_snackbar(nullptr);
  _snackbar_refresh_deadline_ms = 0;
}

void Orchestrator::enter_provisioning_page(ProvisioningTransport transport) {
  AG_LOGI(TAG, "enter_provisioning_page: transport=%u", static_cast<unsigned>(transport));
  // Idempotent — no-op if Info already set up the session; otherwise
  // performs silent unlock + snackbar clear so a post-online auth_failed
  // entry from Home lands on the page in a clean state.
  begin_session_if_needed();

  // Stop the on-Info bring-up arm from acting on any further events
  // (a late WifiConnected post-handoff would otherwise try to render
  // "Connected!" on the wrong screen).
  _bring_up_pending = false;

  pause_provisioning_sensitive_services();
  _svc.ui_manager.open_provisioning(transport);
  log_heap(TAG, "prov.enter-page:pre");
  _svc.wifi.start_provisioning(transport);
  log_heap(TAG, "prov.enter-page:post");
  // Full refresh — session boundary, or Info -> Provisioning jump (the
  // refresh policy in DisplayService::update() picks Full in both cases).
  update_display(/*wait=*/true);
}

void Orchestrator::rebase_periodic_clocks() {
  const uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  _last_measurement_ms = now;
  _last_bms_poll_ms = now;
  _last_bms_status_poll_ms = now;
  // _last_ext_wdt_ms is deliberately not rebased — ext WDT was not paused.
  // _last_input_ms is set by lock()/unlock() and on_input(); not touched here.
}

void Orchestrator::leave_session_to_home() {
  AG_LOGI(TAG, "leave_session_to_home");
  _svc.ui_manager.set_provisioning_connected(0);
  _svc.ui_manager.reset_to_home();
  _bring_up_pending = false;

  // Fresh battery snapshot before resume requests an immediate measurement.
  _latest_power = _svc.power_service.poll_bms(_first_measurement_done &&
                                              !_cached_measures.pm_a.is_pm_25_valid());

  // Keep _setup_session_active = true through resume so any background
  // render path (e.g. the immediate measurement that resume requests, a
  // stray BLE-status event) still no-ops through the suppression gate.
  // Cleared just before the final blocking render below.
  resume_provisioning_sensitive_services(); // no-op if not paused (Info exit)
  rebase_periodic_clocks();

  // Silent unlock — page already showed success; no "Unlocked" snackbar.
  _lock_state = LockState::Unlocked;
  _last_input_ms = static_cast<uint32_t>(RTOS::get_time_ms());

  _setup_session_active = false; // gate cleared after teardown completes
  update_display(true);
  _svc.display_service.flush(); // paint completes before caller returns
}

void Orchestrator::leave_session_to_portable() {
  AG_LOGI(TAG, "leave_session_to_portable");
  _svc.ui_manager.set_provisioning_connected(0);
  // reset_to_home() first so change_mode()'s update_display() and the
  // subsequent explicit render below both build values against
  // Screen::Home (change_mode does not change the UI screen on its own).
  _svc.ui_manager.reset_to_home();
  _bring_up_pending = false;
  _latest_power = _svc.power_service.poll_bms(_first_measurement_done &&
                                              !_cached_measures.pm_a.is_pm_25_valid());

  // Keep _setup_session_active = true through change_mode() so any
  // background-render path that fires mid-teardown (resume's immediate
  // measurement, a stray BLE event during init_ble_if_portable()) still
  // no-ops through the suppression gate.
  change_mode(OperatingMode::Portable);
  rebase_periodic_clocks();

  _setup_session_active = false; // gate cleared after teardown completes
  update_display(/*wait=*/true); // rescues any drop from change_mode's render
  _svc.display_service.flush();  // paint completes before caller returns
}

void Orchestrator::on_wifi_connected(uint32_t ip) {
  AG_LOGI(TAG, "wifi connected: ip=0x%08x", static_cast<unsigned>(ip));
  if (_mode != OperatingMode::Stationary) {
    return; // ignore stray late events on non-Stationary modes
  }

  if (_bring_up_pending) {
    // Initial Stationary bring-up STA success: show "Connected!\n<ip>"
    // on Screen::Info, hold STA_RESULT_HOLD_MS post-paint, then leave
    // the session to Home unlocked.  No snackbar — the on-page text
    // already conveys success.
    _bring_up_pending = false;

    char ip_str[16];
    format_ipv4_be(ip, ip_str);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "Connected!\n%s", ip_str);
    _svc.ui_manager.show_info(buf);
    update_display(/*wait=*/true); // queue the success frame, no drop
    _svc.display_service.flush();  // wait until paint completes
    RTOS::delay_ms(STA_RESULT_HOLD_MS);

    leave_session_to_home();
  } else if (!_setup_session_active && _svc.ui_manager.current_screen() == Screen::Home) {
    // Post-online reconnect on Home — keep the existing snackbar.
    AG_LOGI(TAG, "wifi reconnected");
    _svc.ui_manager.show_snackbar("Wi-Fi connected");
    update_display();
  }
  // Cloud arm: unconditional on every Stationary IP transition.
  // start() is idempotent (no-op on reconnect, heap-claim on first call).
  if (!_svc.cloud.start()) {
    AG_LOGE(TAG, "cloud.start() failed; cloud transport offline");
    return;
  }
  _svc.cloud.arm(_cloud_first_post_pending);
  _cloud_first_post_pending = false;
  log_heap(TAG, "wifi.connected:after-cloud-start");
}

void Orchestrator::on_wifi_disconnected(WifiDisconnectReason reason) {
  AG_LOGI(TAG, "wifi disconnected: %s", wifi_disconnect_reason_to_string(reason));
  log_heap(TAG, "wifi.disconnected:enter");
  if (_mode != OperatingMode::Stationary) {
    return;
  }

  // Disarm before policy routing; skip requested_by_user (own teardown).
  if (reason != WifiDisconnectReason::requested_by_user) {
    _svc.cloud.disarm();
  }

  // Runtime (already online this entry): never provision, never give up —
  // reconnect on any reason except our own teardown (requested_by_user).
  if (_svc.wifi.has_been_online()) {
    if (reason != WifiDisconnectReason::requested_by_user) {
      AG_LOGI(TAG, "runtime link lost; scheduling reconnect");
      const WifiStaticIpConfig *ip = _settings.static_ip.ip != 0 ? &_settings.static_ip : nullptr;
      _svc.wifi.schedule_reconnect(ip);
      update_display();
    }
    return;
  }

  // Bring-up (before first IP): provisioning is the fallback. The connectivity
  // reasons reach here only after the WifiManager retry budget / connect
  // window are spent; auth_failed routes here immediately (bad creds).
  // ap_disconnected / handshake_failed / unknown stay — the window
  // synthesizes connection_lost on expiry. requested_by_user is ignored.
  bool open_provisioning = false;
  switch (reason) {
  case WifiDisconnectReason::auth_failed:
  case WifiDisconnectReason::no_ap_found:
  case WifiDisconnectReason::assoc_failed:
  case WifiDisconnectReason::dhcp_failed:
  case WifiDisconnectReason::connection_lost:
    open_provisioning = true;
    break;
  case WifiDisconnectReason::ap_disconnected:
  case WifiDisconnectReason::handshake_failed:
  case WifiDisconnectReason::unknown:
  case WifiDisconnectReason::requested_by_user:
    break;
  }

  if (open_provisioning) {
    // Surface the failure + reason on Screen::Info before handing off to
    // provisioning, so the user sees why the saved connect failed.
    char buf[48];
    std::snprintf(buf, sizeof(buf), "Wi-Fi failed\n%s", wifi_failure_text(reason));
    _svc.ui_manager.show_info(buf);
    update_display(/*wait=*/true);
    _svc.display_service.flush();
    RTOS::delay_ms(STA_RESULT_HOLD_MS);

    enter_provisioning_page(ProvisioningTransport::BleOnly);
  }
}

void Orchestrator::on_provisioning_state_changed(const ProvisioningEventPayload &payload) {
  const auto event = static_cast<ProvisioningEvent>(payload.event);
  AG_LOGI(TAG, "provisioning event=%u transport=%u", static_cast<unsigned>(event),
          payload.transport);

  // Portable attached provisioning is silent (no on-device UI). Only
  // Connected needs action: persist metadata, then verify-then-drop.
  if (static_cast<ProvisioningTransport>(payload.transport) == ProvisioningTransport::BleAttached) {
    switch (event) {
    case ProvisioningEvent::Connected:
      // Creds saved by ProvisioningManager on got-IP; persist product metadata.
      _settings.disable_cloud = payload.disable_cloud;
      _settings.static_ip = payload.static_ip;
      save_go_settings(_config_store, _settings);
      _svc.cloud.set_disable_cloud(_settings.disable_cloud);
      _svc.portable_provisioner.on_connected();
      break;
    case ProvisioningEvent::ConnectFailed:
      // Nothing to persist; manager stays listening, radio up for retry.
      break;
    case ProvisioningEvent::Started:
    case ProvisioningEvent::Connecting:
    case ProvisioningEvent::Stopped:
      break;
    }
    return;
  }

  switch (event) {
  case ProvisioningEvent::Started:
    // Transport up; payload carries which one (post-switch update).
    _svc.ui_manager.set_provisioning_transport(
        static_cast<ProvisioningTransport>(payload.transport));
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::WaitingForCredentials);
    update_display();
    break;

  case ProvisioningEvent::Connecting:
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::Connecting);
    update_display();
    break;

  case ProvisioningEvent::ConnectFailed:
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::ConnectFailed);
    update_display();
    break;

  case ProvisioningEvent::Connected:
    _settings.disable_cloud = payload.disable_cloud;
    _settings.static_ip = payload.static_ip;
    save_go_settings(_config_store, _settings);

    _svc.cloud.set_disable_cloud(_settings.disable_cloud);

    // Render "Connected! a.b.c.d" on the Provisioning page first.  The
    // wait=true + flush() pair guarantees the success frame is painted
    // before stop_provisioning()'s internal POST_CONNECT_HOLD_MS (~1.5 s)
    // starts running against the prior frame.
    _svc.ui_manager.set_provisioning_connected(payload.ip);
    _svc.ui_manager.set_provisioning_ui_state(ProvisioningUiState::Connected);
    update_display(/*wait=*/true);
    _svc.display_service.flush();

    // Tear down the provisioning transport.  ProvisioningManager::stop()
    // blocks for POST_CONNECT_HOLD_MS (~1.5 s) when called after
    // Connected, which doubles as the on-page hold now that the success
    // page is actually visible.  No snackbar — the page already shows it.
    _svc.wifi.stop_provisioning();

    leave_session_to_home();

    // Provisioning heap freed above; safe to claim cloud task stack now.
    if (!_svc.cloud.start()) {
      AG_LOGE(TAG, "cloud.start() failed; cloud transport offline");
      break;
    }
    _svc.cloud.arm(/*fire_now=*/true);
    break;

  case ProvisioningEvent::Stopped:
    // User abort, timeout, or transport-switch start failure. With no
    // prior online state, fall back to Portable so the device is never
    // stranded on the Provisioning screen with no active transport.
    if (!_svc.wifi.has_been_online()) {
      leave_session_to_portable();
    }
    break;
  }
}

void Orchestrator::pause_provisioning_sensitive_services() {
  if (_provisioning_sensitive_services_paused) {
    return;
  }
  AG_LOGI(TAG, "pausing network-sensitive services");
  _svc.sensor_producer.stop();
  if (is_gps_active()) {
    _svc.gps_service.stop_and_idle_gnss();
  }
  _svc.power_service.set_pm_power(false);
  _provisioning_sensitive_services_paused = true;
  log_heap(TAG, "prov.pause-sensitive:exit");
}

void Orchestrator::resume_provisioning_sensitive_services() {
  if (!_provisioning_sensitive_services_paused) {
    return;
  }
  AG_LOGI(TAG, "resuming network-sensitive services");
  _svc.power_service.set_pm_power(true);
  _svc.sensor_producer.start();
  if (is_gps_active()) {
    _svc.gps_service.start();
  }
  _provisioning_sensitive_services_paused = false;
  // One immediate measurement so the display refreshes promptly after
  // the resume rather than waiting for the next scheduled tick.
  _svc.sensor_producer.request_measurement(1, SensorGroup::All);
  log_heap(TAG, "prov.resume-sensitive:exit");
}

// ---------------------------------------------------------------------------
// OTA
// ---------------------------------------------------------------------------

void Orchestrator::enter_ota() {
  AG_LOGI(TAG, "enter_ota: quiescing services for OTA");
  pause_provisioning_sensitive_services(); // stop sensor, idle GPS, drop PM rail

  // Disarm cloud (parks the task, heap kept; resume is a plain arm()). Not
  // stop() — avoids heap churn for no real gain. See docs/cloud_service.md.
  if (_mode == OperatingMode::Stationary) {
    _svc.cloud.disarm();
  }
  // Portable BLE traffic is suppressed implicitly (loop blocked).

  _svc.power_service.reset_ext_watchdog(); // cover the gap to the first tick
}

void Orchestrator::paint_updating_firmware() {
  _svc.ui_manager.show_info("Updating firmware...");
  update_display(/*wait=*/true);
  _svc.display_service.flush();
}

void Orchestrator::on_ota_download_started() {
  // First WiFi Downloading tick (real image): commit the deferred quiesce + paint.
  enter_ota();
  paint_updating_firmware();
  _ota_committed = true;
}

void Orchestrator::finish_ota(OtaStatus status) {
  switch (status) {
  case OtaStatus::Ok:
    // Render "Restarting…" before the reboot tears the panel worker down.
    _svc.ui_manager.show_info("Restarting...");
    update_display(/*wait=*/true);
    _svc.display_service.flush();
    reboot(); // does not return
    return;
  case OtaStatus::UpToDate:
  case OtaStatus::Declined:
    exit_ota(nullptr); // silent resume
    break;
  case OtaStatus::Aborted:
    exit_ota("Update cancelled"); // explicit phone ABORT only
    break;
  case OtaStatus::TransportError:
  case OtaStatus::FlashError:
  case OtaStatus::InvalidImage:
  case OtaStatus::ServerError:
  case OtaStatus::InvalidArgument:
    exit_ota("Update failed");
    break;
  }
}

void Orchestrator::exit_ota(const char *snackbar) {
  if (_ota_committed) {
    // Drain first — events buffered during the blocked transfer are obsolete,
    // and draining before resume keeps the resume's own events.
    Event drop{};
    while (RTOS::queue_receive(_event_queue, &drop, 0)) {
    }

    resume_provisioning_sensitive_services(); // restart PM/sensor/GPS + 1 measurement
    rebase_periodic_clocks();

    // Cloud was only disarmed; re-arm when still online (no start() needed).
    if (_mode == OperatingMode::Stationary && _svc.wifi.is_online()) {
      _svc.cloud.arm(/*fire_now=*/false);
    }

    // Snackbar before render (update_display reads it); replace the stuck frame.
    _svc.ui_manager.show_snackbar(snackbar); // nullptr = silent
    _svc.ui_manager.reset_to_home();
    update_display(/*wait=*/true);
    _ota_committed = false;
  } else {
    // Lightweight resume (speculative check, never quiesced): no drain —
    // buffered events are real user intent, not OTA debris.
    resume_provisioning_sensitive_services(); // guarded no-op

    if (_mode == OperatingMode::Stationary && _svc.wifi.is_online()) {
      _svc.cloud.arm(/*fire_now=*/false); // re-arm (only disarmed)
    }

    // Render over the current screen only if a check failure set a snackbar.
    if (snackbar != nullptr) {
      _svc.ui_manager.show_snackbar(snackbar);
      update_display(/*wait=*/true);
    }
  }
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------

void Orchestrator::update_display() { update_display(false); }

void Orchestrator::update_display(bool wait) {
  uint32_t now_ms = static_cast<uint32_t>(RTOS::get_time_ms());
  _svc.ui_manager.clear_expired_snackbar(now_ms);
  BuildContext ctx = build_context();
  DisplayValues values = _svc.ui_manager.build_values(ctx);
  _svc.display_service.update(values, wait);

  // Schedule a follow-up refresh to visually clear the snackbar after it
  // expires.  Only arm once per snackbar — intermediate update_display()
  // calls (from sensor data, input, etc.) that happen before the deadline
  // naturally clear it and the timer fires as a harmless no-op.
  if (values.snackbar_text != nullptr && _snackbar_refresh_deadline_ms == 0) {
    _snackbar_refresh_deadline_ms = now_ms + SNACKBAR_DURATION_MS + 200;
  }

  AG_LOGI(TAG, "display update done");
}

void Orchestrator::request_background_display_update() {
  if (_setup_session_active) {
    // Session screens (Info / Provisioning / ProvisioningConfirm) only
    // re-render on explicit setup state transitions.  Suppressing the
    // background path here prevents sensor / BMS / BLE-status events from
    // racing the orchestrator's deliberate wait=true renders.
    return;
  }
  if (_svc.ui_manager.is_focus_screen()) {
    // Focus screens (PairingPasskey) render constant content while the
    // user is acting on it.  Sensor / BMS events would cause Fast
    // refreshes of an identical frame and eventually trigger the
    // anti-ghost Full refresh.
    return;
  }
  if (!_svc.ui_manager.is_on_menu_screen()) {
    update_display();
  }
}

BuildContext Orchestrator::build_context() const {
  // Convert MeasuresAGo to Measures for the BuildContext reference
  _display_measures = Measures{};
  _display_measures.temp_hum_a = _cached_measures.temp_hum_a;
  _display_measures.pm_a = _cached_measures.pm_a;
  _display_measures.co2 = _cached_measures.co2;
  _display_measures.tvoc_nox = _cached_measures.tvoc_nox;
  _display_measures.power = _cached_measures.power;
  _display_measures.pressure = _cached_measures.pressure;

  // Read chart data cache
  uint16_t cache_count = _svc.storage_service.read_cache(_cache_buf, UI_CHART_BUF_SIZE);

  // Extract battery data
  uint8_t battery_pct = 0xFF;
  if (_latest_power.battery_percentage >= 0.0f) {
    battery_pct = static_cast<uint8_t>(_latest_power.battery_percentage);
  }

  bool is_charging = is_bms_charging(_latest_power.charging_status);

  return BuildContext{
      .sensor_data = _display_measures,
      .battery_pct = battery_pct,
      .is_battery_charging = is_charging,
      .is_plugged_in =
          bms_power_source_has_external_input(_latest_power.charger_status.power_source),
      .locked = (_lock_state == LockState::Locked),
      .ble_enabled = (_mode == OperatingMode::Portable),
      // "connected" icon = usable (encrypted) link, not merely GAP-linked.
      .ble_connected = _svc.ble_service.is_authenticated(),
      // Icon shown for the whole Stationary session; glyph reflects link state.
      .wifi_enabled = (_mode == OperatingMode::Stationary),
      .wifi_connected = (_mode == OperatingMode::Stationary) && _svc.wifi.is_online(),
      .gps_enabled = is_gps_active(),
      .gps_fix = is_fix_valid(_latest_gps.fix),
      .tracking_active = _tracking_active,
      .display_off = false,
      .use_fahrenheit = _settings.use_fahrenheit,
      .pm_use_usaqi = _settings.pm_use_usaqi,
      .cache = _cache_buf,
      .cache_count = static_cast<uint8_t>(cache_count),
      .now_ms = static_cast<uint32_t>(RTOS::get_time_ms()),
  };
}

// ---------------------------------------------------------------------------
// Sleep
// ---------------------------------------------------------------------------

void Orchestrator::try_enter_sleep() {
  uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  uint32_t awake_ms = now - _last_measurement_ms;

  auto decision = _svc.power_service.decide_sleep(_settings, _lock_state, _mode, awake_ms);

  if (decision.type == PowerService::SleepType::None) {
    return;
  }

  // decision.type == Deep
  prepare_for_sleep(decision.duration_ms);
  AG_LOGI(TAG, "entering deep sleep (%lu ms)", static_cast<unsigned long>(decision.duration_ms));
  _svc.power_service.enter_sleep(decision.duration_ms);
  // Never returns — CPU reboots on wake.
}

void Orchestrator::prepare_for_sleep(uint32_t sleep_duration_ms) {
  AG_LOGI(TAG, "prepare_for_sleep");
  log_heap(TAG, "sleep.prepare:enter");

  // Ensure pending display refresh completes before stopping worker
  _svc.ui_manager.clear_expired_snackbar(static_cast<uint32_t>(RTOS::get_time_ms()));
  BuildContext ctx = build_context();
  DisplayValues values = _svc.ui_manager.build_values(ctx);
  _svc.display_service.update(values, true); // wait = true

  // Save RTC display snapshot so the next button wake can render immediately
  // without NVS or sensor reads.
  save_rtc_display_snapshot(values);

  _svc.ble_service.deinit();
  _svc.sensor_producer.stop();

  // Active GPS: stop task only — leave TAU1113 tracking for hot-start.
  // Inactive GPS: stop task and send GNSS stop before sleep.
  if (is_gps_active()) {
    _svc.gps_service.stop();
  } else {
    _svc.gps_service.stop_and_idle_gnss();
  }

  _svc.input_service.stop();
  _svc.display_service.stop();

  // Put SSD1680 into deep sleep mode 1 after stopping the worker task.
  // Reduces quiescent current from ~100 µA to <1 µA during ESP deep sleep.
  // driver_hw_init_full() exits deep sleep on next init() via hardware reset.
  _svc.display_service.deep_sleep();

  // Flush + close the route file before the CPU reboots; resume_route()
  // truncates any torn tail and continues the session on next wake.
  _svc.storage_service.end_route();

  _svc.storage_service.backup_cache();

  // Persist RTC state with the warm-sensor flag for the next wake cycle.
  RtcAppState state = snapshot_state();
  state.sensors_warm = _svc.power_service.should_hold_pm_sensor(sleep_duration_ms);
  _svc.power_service.save_state(state);

  // Reset external watchdog last — gives it the full timeout window during sleep.
  _svc.power_service.reset_ext_watchdog();

  // Start LP Core to keep pulsing the external watchdog during deep sleep.
  ulp_wdt_start();
  log_heap(TAG, "sleep.prepare:before-sleep");
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void Orchestrator::deactivate_gps() {
  _svc.gps_service.stop_and_idle_gnss();
  _latest_gps = GpsData{};
}

bool Orchestrator::is_gps_active() const {
  if (_settings.gps_mode == GpsMode::AlwaysOff) {
    return false;
  }
  if (_settings.gps_mode == GpsMode::AlwaysOn) {
    return true;
  }
  // GpsMode::OnWhenTracking
  return _tracking_active;
}

bool Orchestrator::is_recording() const {
  return _tracking_active && _svc.storage_service.is_route_active();
}

uint32_t Orchestrator::generate_session_id() {
  // Probe each random draw against existing route files so a collision
  // never surfaces as a user-visible storage error.
  for (int i = 0; i < SESSION_ID_MAX_RETRIES; ++i) {
    const uint32_t id = generate_random_number(SESSION_ID_LENGTH);
    if (!_svc.storage_service.route_file_exists(id)) {
      AG_LOGI(TAG, "generate_session_id: %" PRIu32 " (try %d)", id, i + 1);
      return id;
    }
    AG_LOGW(TAG, "generate_session_id: collision on %" PRIu32 " (try %d)", id, i + 1);
  }
  AG_LOGE(TAG, "generate_session_id: exhausted %d retries", SESSION_ID_MAX_RETRIES);
  return 0; // start_tracking treats 0 as a fatal generate failure
}

RtcAppState Orchestrator::snapshot_state() const {
  return RtcAppState{
      .mode = _mode,
      .behavior = _behavior,
      .lock_state = _lock_state,
      .gps_enabled = _gps_enabled,
      .tracking_active = _tracking_active,
      .tracking_session_id = _tracking_session_id,
  };
}
