/**
 * AirGradient Go — Orchestrator
 *
 * Central event loop that consumes typed events from producers, updates
 * state machines (app state, UI state, power policy), and calls consumers
 * directly.  See ARCHITECTURE.md §3 for the high-level design.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "config_store.h"
#include "go_ble.h"
#include "buzzer/go_buzzer.h"
#include "go_cloud.h"
#include "go_display.h"
#include "go_events.h"
#include "go_input.h"
#include "led/go_led.h"
#include "go_ota.h"
#include "go_portable_provisioner.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_settings.h"
#include "go_storage.h"
#include "go_types.h"
#include "go_ui.h"
#include "go_ulp.h"
#include "gps/gps_service.h"
#include "rtos.h"
#include "go_wifi.h"

#include <cstdint>

struct GoBoard;

class Orchestrator {
public:
  /// References to all product services.  All referenced objects must outlive
  /// the Orchestrator instance.
  struct Services {
    SensorProducer &sensor_producer;
    GpsService &gps_service;
    InputService &input_service;
    DisplayService &display_service;
    LedService &led_service;
    BuzzerService &buzzer_service;
    StorageService &storage_service;
    PowerService &power_service;
    UIManager &ui_manager;
    BleService &ble_service;
    WifiService &wifi;
    CloudService &cloud;
    PortableWifiProvisioner &portable_provisioner; // attached Portable Wi-Fi provisioning
    GoBoard &board;  // borrowed for init_wifi_subsystem() in Stationary entry
    OtaService &ota; // per-mode OTA wiring (BLE push / WiFi pull)
  };

  /// Construct the orchestrator.
  ///
  /// @param event_queue  RTOS queue handle for the central event queue.
  /// @param services     References to all product services.
  /// @param settings     Product settings (owned copy — orchestrator may
  ///                     update on SettingsChanged events).
  /// @param config_store Config store for persisting setting changes.
  /// @param serial       Device serial string (e.g., "AABBCCDDEEFF"),
  ///                     used for BLE advertising name.  Must remain
  ///                     valid for the lifetime of the Orchestrator.
  Orchestrator(RtosQueueHandle event_queue, const Services &services, GoSettings settings,
               ConfigStore &config_store, const char *serial);

  /// Set initial state from boot context and perform first-boot actions.
  /// Call once before run().
  ///
  /// @param cause   Wake cause from PowerService::get_wake_cause().
  /// @param handoff Boot-to-runtime handoff describing what boot has
  ///                already done. Default is a fresh power-on.
  void init(WakeCause cause, const BootHandoff &handoff = {});

  /// Main event loop.  Does not return.
  void run();

private:
#ifdef TEST_HOST
  friend class OrchestratorTestAccess;
#endif

  RtosQueueHandle _event_queue;
  Services _svc;
  GoSettings _settings;
  ConfigStore &_config_store;
  const char *_serial; ///< Device serial string for BLE advertising

  // --- Application state ---
  OperatingMode _mode = OperatingMode::Portable;
  Behavior _behavior = Behavior::Idle;
  LockState _lock_state = LockState::Locked;
  bool _gps_enabled = true;
  bool _tracking_active = false;
  uint32_t _tracking_session_id = 0;

  // --- Cached data ---
  MeasuresAGo _cached_measures{}; ///< Merged sensor results (invalid sentinels by default)
  GpsData _latest_gps{};
  PowerSnapshot _latest_power{};

  // --- Timer tracking (millisecond timestamps) ---
  uint32_t _last_measurement_ms = 0;
  uint32_t _last_bms_poll_ms = 0;
  uint32_t _last_bms_status_poll_ms = 0;
  uint32_t _last_ext_wdt_ms = 0;
  uint32_t _last_input_ms = 0;                ///< Reset on every input; drives inactivity
  uint32_t _snackbar_refresh_deadline_ms = 0; ///< 0 = inactive; non-zero = absolute deadline
  bool _first_measurement_done = false;

  // --- OTA ---
  /// Unified OTA poll-timer baseline: 2 s BLE is_ble_active() poll (Portable),
  /// 1 h WiFi check (Stationary).  Re-armed to now after each poll/check.
  uint32_t _last_ota_check_ms = 0;
  /// True once a transfer has committed — the full enter_ota() quiesce +
  /// "Updating firmware…" paint ran (BLE always; WiFi once a download started).
  /// Gates exit_ota()'s full-resume + queue-drain vs the lightweight cloud
  /// re-arm, and the Screen::Home restore.  Reset at each OTA poll branch.
  bool _ota_committed = false;

  // --- PM sensor sleep (Portable mode power-cycling) ---
  bool _pm_prepare_sent = false; ///< PREPARE already sent for the current measurement cycle

  // --- PM sensor recovery (V1: boost-kill power cycle on persistent failure) ---
  uint32_t _pm_first_fail_ms = 0; ///< 0 = no failure in progress

  // --- Stationary networking ---
  bool _provisioning_sensitive_services_paused = false;

  /// True between the entering-session boundary (Screen::Info on Stationary
  /// entry, or Screen::Provisioning on post-online auth_failed) and the
  /// leaving-session boundary (Home or Portable).  Gates power-button
  /// short-press suppression, auto-lock suppression, touch-driven
  /// post-input drop-free renders, background-update suppression, and
  /// (transitively via _provisioning_sensitive_services_paused) the
  /// sensor/BMS deadline gating.
  bool _setup_session_active = false;

  /// True while Screen::Info shows the STA-attempt narration text; lets
  /// on_wifi_connected() distinguish the on-Info success path (show
  /// "Connected!" then Home) from the post-online reconnect path (snackbar
  /// on Home, no page transition).
  bool _bring_up_pending = false;

  /// True from cold-boot until the first sensor measurement arrives.
  /// Gates the Info -> Home transition in on_sensor_data() and suppresses
  /// the power short-press lock toggle while "Booting..." is on screen.
  bool _boot_splash_active = false;

  /// Set on Stationary entry, consumed by on_wifi_connected().
  /// true → first cloud arm fires immediately; reconnects pass false.
  bool _cloud_first_post_pending = false;

  /// True once the boot-button manufacturing shortcut entered ephemeral
  /// Stationary (onboarding skipped, nothing persisted). On shutdown this
  /// forces a factory_reset() so test units ship clean.
  bool _manufacturing_mode = false;

  // --- Display buffers (mutable for const build_context) ---
  mutable Measures _display_measures{};
  mutable MeasuresAGo _cache_buf[UI_CHART_BUF_SIZE]{};

  // --- Constants ---
  static constexpr uint32_t BMS_POLL_INTERVAL_MS = 30000;
  static constexpr uint32_t BMS_STATUS_POLL_INTERVAL_MS = 5000;
  static constexpr uint32_t EXT_WDT_INTERVAL_MS = 60000;
  static constexpr uint32_t MAX_REASONABLE_TIMEOUT_MS = 3600000;
  static constexpr uint32_t PM_RECOVERY_TIMEOUT_MS = 30000;
  static constexpr uint32_t BLE_COMMAND_RESULT_DELAY_MS = 200;
  /// Settle window between pushing the op_mode Config delta and tearing down
  /// the Portable BLE link, so the notification drains before disconnect.
  /// Notifications are fire-and-forget; this covers a few connection intervals.
  static constexpr uint32_t BLE_MODE_CHANGE_NOTIFY_SETTLE_MS = 200;
  /// Post-paint dwell before cutting power.  DisplayService::flush() already
  /// waits for e-paper completion; this intentionally slows final shutdown so
  /// the user sees the reason screen and BLE disconnect notice can drain.
  static constexpr uint32_t SHUTDOWN_POWER_OFF_SETTLE_MS = 500;
  /// Post-paint dwell for the STA bring-up result Info frame (Connected!
  /// on success, "Wi-Fi failed" on failure) before leaving the page.
  static constexpr uint32_t STA_RESULT_HOLD_MS = 1000;

  // --- Event dispatch ---
  void dispatch(const Event &event);

  // --- Event handlers ---
  void on_sensor_data(const MeasuresAGo &data);
  void on_gps_fix(const GpsData &data);
  void on_input(const InputEventData &input);
  void on_co2_calibration_done(Co2CalibrationResult result);

  // --- BLE event handlers ---
  void on_ble_connected();
  void on_ble_disconnected();
  void on_ble_config_write();
  void on_ble_history_write();
  void on_ble_pairing_request(uint32_t passkey);
  void on_ble_auth_complete(bool success);

  // --- State transitions ---
  void lock();
  void unlock();
  /// Begin a new tracking session. Returns false on session-id exhaustion
  /// or storage-open failure; the snackbar + BLE notify fire inline first.
  bool start_tracking();
  void stop_tracking();
  /// persist=false skips onboarding + settings writes (manufacturing path).
  void change_mode(OperatingMode new_mode, bool persist = true);
  /// Boot-button manufacturing shortcut: skip onboarding and enter
  /// Stationary ephemerally (no NVS persist) for production testing.
  void enter_manufacturing_mode();
  /// Persist the onboarding flag on first engagement. Idempotent (no
  /// redundant NVS write).
  void mark_onboarding_done();
  void apply_settings_change();
  bool clear_data();
  bool factory_reset();
  void save_tag(uint8_t tag_index, const char *tag_label);
  void shutdown(ShipModeRequest reason = ShipModeRequest::None);

  // --- Timer management ---
  uint32_t compute_queue_timeout_ms() const;
  void check_timers();
  void on_bms_timer();
  void on_bms_status_timer();
  void on_inactivity_timeout();
  void reschedule_sensor_timer(const GoSettings &previous_settings);

  // --- Display ---
  void update_display();
  /// Drop-free render variant.  Forwards to DisplayService::update(values,
  /// wait); when wait=true the new frame is queued without being dropped
  /// even if the worker is mid-paint on a prior frame.  Paired with
  /// DisplayService::flush() at call sites that need post-paint guarantees.
  void update_display(bool wait);
  void request_background_display_update();
  BuildContext build_context() const;

  // --- Sleep ---
  void try_enter_sleep();
  void prepare_for_sleep(uint32_t sleep_duration_ms);

  // --- BLE ---
  void init_ble_if_portable();
  static constexpr size_t BLE_WRITE_BUF_SIZE = 256;

  // --- OTA ---
  /// Full quiesce around a committed transfer: pause sensitive services
  /// (sensor / GPS / PM rail), stop cloud (Stationary), feed the external
  /// watchdog once.  No display work.  Called up front for BLE, lazily for
  /// WiFi (on_ota_download_started).
  void enter_ota();
  /// Non-rebooting terminal resume.  Branches on _ota_committed: committed →
  /// drain queue + resume services + reconcile cloud + snackbar/Home render;
  /// not committed → guarded no-op resume + cloud re-arm + optional snackbar.
  /// snackbar == nullptr is silent.
  void exit_ota(const char *snackbar);
  /// Terminal dispatcher: Ok → "Restarting…" + reboot(); else exit_ota() with
  /// the matching snackbar.  Only place reboot-vs-resume is decided.
  void finish_ota(OtaStatus status);
  /// Paint the Screen::Info "Updating firmware…" frame, blocking (wait + flush).
  void paint_updating_firmware();
  /// WiFi commit edge (first Downloading tick): enter_ota() + paint + set
  /// _ota_committed.  Wired into run_wifi_check() via a thin forwarder.
  void on_ota_download_started();

  // --- Stationary Wi-Fi ---
  void enter_stationary();
  void on_wifi_connected(uint32_t ip);
  void on_wifi_disconnected(WifiDisconnectReason reason);
  void on_provisioning_state_changed(const ProvisioningEventPayload &payload);
  void pause_provisioning_sensitive_services();
  void resume_provisioning_sensitive_services();

  // --- Setup-session helpers (Stationary bring-up + provisioning) ---

  /// Idempotent session-entry preamble: silent unlock + snackbar clear.
  /// No-op when _setup_session_active is already true.  Called by both
  /// enter_stationary() (Info path) and enter_provisioning_page()
  /// (post-online auth_failed path).
  void begin_session_if_needed();

  /// Open Screen::Provisioning, pause sensitive services, start the
  /// requested transport, and render wait=true.  Called from both the
  /// bring-up failure path (Info -> Provisioning, session already active)
  /// and post-online auth_failed (Home -> Provisioning, session starts).
  void enter_provisioning_page(ProvisioningTransport transport);

  /// Session-leave helpers.  Both poll the battery once for a fresh
  /// icon, resume services if needed, rebase periodic clocks, and end
  /// with a drop-free wait=true render + flush() so the leaving frame is
  /// painted before the helper returns.
  void leave_session_to_home();
  void leave_session_to_portable();

  /// Rebase the sensor measurement / BMS / BMS-status deadlines to now,
  /// so post-resume timers do not fire back-to-back catching up on
  /// missed cycles.
  void rebase_periodic_clocks();

  // --- Helpers ---
  bool is_gps_active() const;
  void deactivate_gps();

  /// User intent AND file actually open. Drives every BLE status push so
  /// the wire never reports tracking when no file is open.
  bool is_recording() const;

  /// Random 5-digit ID with bounded collision retry. Returns 0 on
  /// exhaustion (start_tracking treats that as a fatal failure).
  uint32_t generate_session_id();

  RtcAppState snapshot_state() const;
};
