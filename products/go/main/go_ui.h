#ifndef GO_UI_H
#define GO_UI_H

#include <cstdint>

#include "go_display.h"
#include "go_melody.h"
#include "go_settings.h"
#include "go_types.h"
#include "gps/gps_types.h"
#include "measures_types.h"
#include "types/provisioning_types.h"
#include "types/wifi_types.h"

/// Chart buffer size — matches the payload cache capacity (Kconfig default 16).
inline constexpr uint8_t UI_CHART_BUF_SIZE = 16;

/// Snackbar display duration in milliseconds.
inline constexpr uint32_t SNACKBAR_DURATION_MS = 3000;

/// Human-readable reason phrase for the bring-up "Wi-Fi failed" Info
/// screen. Covers the reasons the bring-up disconnect policy acts on.
const char *wifi_failure_text(WifiDisconnectReason reason);

// ---------------------------------------------------------------------------
// UIAction — returned by handle_input to signal application-level changes.
// ---------------------------------------------------------------------------

enum class UIAction : uint8_t {
  None,
  StartTracking,
  StopTracking,
  ChangeMode,      ///< Accompanied by UIActionResult::new_mode.
  SettingsChanged, ///< UI Manager updated internal settings state.
  ClearData,
  CalibrateCo2,
  SaveTag,                            ///< Accompanied by UIActionResult::tag_index.
  PlayMelody,                         ///< Accompanied by UIActionResult::melody.
  ConfirmSwitchProvisioningTransport, ///< User confirmed Yes on switch-transport overlay.
  ConfirmCancelProvisioning,          ///< User confirmed Yes on cancel-setup overlay.
  AckOnboarding,                      ///< 'Start using' pressed on first-boot Getting Started.
  ArmFgLearning,      ///< Confirmed: write factory state and reboot into FG learning.
  RunPeripheralTest,  ///< Start the guided actuator + AQ peripheral test flow.
  PeripheralStepPass, ///< Operator confirmed the current actuator step.
  PeripheralStepFail, ///< Operator marked the current actuator step failed.
  PeripheralTestExit, ///< Leave the peripheral test summary (restore hardware).
  OpenGpsTest,        ///< Enter the live GPS test screen (start receiver, TTFF timer).
  OpenAccelTest,      ///< Enter the live accelerometer test screen.
};

/// View state for Screen::PeripheralTest. The orchestrator owns the flow and
/// pushes this snapshot to the UI each step; UIManager only renders it and
/// emits Pass/Fail on the actuator steps.
struct PeripheralTestView {
  enum class Kind : uint8_t {
    Actuator, ///< Operator-guided step: show `prompt`, offer Pass/Fail.
    Testing,  ///< Automatic AQ sweep running ("Testing sensors...").
    Summary,  ///< Final results table; any tap exits.
  };

  Kind kind = Kind::Actuator;
  const char *prompt = nullptr; ///< Actuator prompt (static string), Actuator kind only.

  // Summary results (Summary kind only). true = pass.
  bool front_led = false;
  bool back_led = false;
  bool touch_led = false;
  bool buzzer = false;
  bool temp_hum = false;
  bool co2 = false;
  bool pm = false;
  bool tvoc_nox = false;
  bool pressure = false;
  bool overall = false;
};

/// Provisioning-screen status state. UIManager maps to display text,
/// reading the current transport for transport-specific phrasing.
enum class ProvisioningUiState : uint8_t {
  Idle,                  ///< No status line
  WaitingForCredentials, ///< Transport-specific instructions
  SwitchingTransport,    ///< Transient during BLE <-> Wi-Fi swap
  Connecting,            ///< Credentials received; STA connect in progress
  ConnectFailed,         ///< Last attempt failed; still listening
  Connected,             ///< Provisioning success: status line shows "Connected! a.b.c.d"
};

struct UIActionResult {
  UIAction action = UIAction::None;
  OperatingMode new_mode = OperatingMode::Offline;
  uint8_t tag_index = 0;
  const char *tag_label = nullptr; ///< Points to static tag label string.
  MelodySelect melody = MelodySelect::Off;
};

// ---------------------------------------------------------------------------
// BuildContext — all external state for building a DisplayValues snapshot.
// ---------------------------------------------------------------------------

struct BuildContext {
  const Measures &sensor_data;

  // Battery
  uint8_t battery_pct; // 0xFF = no data
  bool is_battery_charging;
  bool is_plugged_in;

  // Status flags
  bool locked;
  bool ble_enabled;
  bool ble_connected;
  bool wifi_enabled;
  bool wifi_connected;
  bool gps_enabled;
  bool gps_fix;
  bool tracking_active;

  // Settings-derived flags
  bool display_off;
  bool use_fahrenheit;
  bool use_feet;
  bool pm_use_usaqi;

  // Temporary measurement cache (for chart rendering)
  const MeasuresAGo *cache;
  uint8_t cache_count;

  // Current timestamp for snackbar expiry
  uint32_t now_ms;

  // --- GPS test screen (Screen::GpsTest only) ---
  /// Latest parsed fix to render (borrowed; orchestrator owns).  Null = none.
  const GpsData *gps_data = nullptr;
  /// Seconds since GPS test entry, frozen once the first valid fix latches.
  uint32_t gps_ttff_secs = 0;
  /// True once the first valid fix has latched the TTFF value.
  bool gps_ttff_fixed = false;

  // --- Accelerometer test screen (Screen::AccelTest only) ---
  /// Raw WHO_AM_I byte read from the device (0 when absent / read failed).
  uint8_t accel_who_am_i = 0;
  /// True when accel_who_am_i matches the device's expected identity.
  bool accel_id_ok = false;
  /// Latest 3-axis sample in milli-g (meaningful only when accel_read_ok).
  int16_t accel_x_mg = 0;
  int16_t accel_y_mg = 0;
  int16_t accel_z_mg = 0;
  /// Vector magnitude in milli-g, for display (orchestrator-computed).
  uint16_t accel_magnitude_mg = 0;
  /// True when the last accelerometer read() succeeded.
  bool accel_read_ok = false;
  /// Overall PASS: identity matched, read ok, and magnitude within tolerance.
  bool accel_pass = false;
};

// ---------------------------------------------------------------------------
// UIManager — pure state machine for screen navigation and UI logic.
//
// The UI Manager has ZERO hardware or RTOS dependencies. It is driven
// entirely by the orchestrator which calls handle_input() and build_values().
// ---------------------------------------------------------------------------

class UIManager {
public:
  struct Config {
    const char *firmware_version; // e.g. "0.1.0"
    const char *serial_number;    // 12-char hex string
    /// Captive-portal AP password — must match WifiService::Config::ap_password
    /// so the Wi-Fi QR and instruction line agree.
    const char *ap_password = "cleanair";
  };

  explicit UIManager(const Config &config);

  /// Process an input event. Returns an action if the input triggered
  /// an application-level state change (start tracking, change mode, etc.).
  /// The orchestrator should not call this when the device is locked.
  UIActionResult handle_input(InputSource source, InputType type);

  /// Build a complete DisplayValues snapshot for the Display Service.
  /// The orchestrator passes in all external state via BuildContext.
  DisplayValues build_values(const BuildContext &ctx) const;

  /// Force the screen to a specific value. Used by the orchestrator
  /// for Shutdown and for restoring state after deep sleep wake.
  void set_screen(Screen screen);

  /// Get the current screen (for orchestrator decisions).
  Screen current_screen() const;

  /// True when the user is on any menu-navigation screen (MainMenu, Settings,
  /// SettingsChoice, TagList, Confirm, About).  Used by the orchestrator to
  /// suppress background display updates that would interrupt menu interaction.
  bool is_on_menu_screen() const;

  /// True when the user is on a modal focus screen (PairingPasskey).  Used by
  /// the orchestrator to suppress background display updates and auto-lock
  /// while the user is acting on the on-screen content.
  bool is_focus_screen() const;

  /// True when the user is on any Hardware Test screen (submenu or a live
  /// flow).  Used by the orchestrator to suppress auto-lock so an operator
  /// running a test is not returned Home mid-flow.
  bool is_hardware_test_screen() const;

  /// Show a snackbar message. Duration is armed on the next
  /// clear_expired_snackbar() call (3 seconds from that point).
  void show_snackbar(const char *text);

  /// Clear snackbar if expired. Called by orchestrator before build_values.
  void clear_expired_snackbar(uint32_t now_ms);

  /// Synchronize internal option indices from persisted GoSettings.
  /// Called by the orchestrator once after loading settings from NVS.
  void sync_settings(const GoSettings &settings);

  /// Convert internal option indices back to GoSettings field values.
  /// Reverse of sync_settings().  Called by the orchestrator when a
  /// setting is changed through the UI.
  void apply_to_settings(GoSettings &settings) const;

  /// Reset to Home screen with no metric selected. Used on auto-lock.
  void reset_to_home();

  /// Show the BLE pairing passkey on a dedicated screen.
  /// The passkey is displayed until dismiss_pairing_passkey() is called
  /// or the user navigates away.
  void show_pairing_passkey(uint32_t passkey);

  /// Dismiss the pairing passkey screen and return to Home.
  void dismiss_pairing_passkey();

  // --- Stationary networking surface ---

  /// Set the active provisioning transport on the currently-open
  /// Provisioning page.  Keeps the cursor at action row 0 (the inactive
  /// transport's switch button).  See `open_provisioning()` for the
  /// idempotent session-entry path.
  void set_provisioning_transport(ProvisioningTransport t);
  ProvisioningTransport provisioning_transport() const;

  /// Provisioning-screen status state. UIManager picks the display
  /// text based on the active transport.
  void set_provisioning_ui_state(ProvisioningUiState s);

  /// Enter the Getting Started guide. Encodes the setup QR; from_boot
  /// selects "Start using" -> AckOnboarding vs "Back" -> About.
  void show_getting_started(bool from_boot);

  /// Push the current peripheral-test view snapshot for Screen::PeripheralTest.
  /// The orchestrator calls this on each flow step; UIManager renders it.
  void set_peripheral_test_view(const PeripheralTestView &view);

  /// Show the generic Info screen with the given ASCII text.  The text
  /// is copied into an internal buffer; the caller does not need to keep
  /// `text` alive.  Sets _screen = Screen::Info.  Null or empty text
  /// renders a blank canvas.
  void show_info(const char *text);

  /// Enter the Provisioning page with the given active transport.
  /// Idempotently resets per-session UI sub-state to a clean baseline:
  ///   _provisioning_connected_ip  = 0
  ///   _provisioning_ui_state      = WaitingForCredentials
  ///   _provisioning_confirm_kind  = 0
  ///   _provisioning_confirm_index = 0   (No)
  ///   _provisioning_row_index     = 0
  /// Sets _provisioning_transport = active and _screen = Screen::Provisioning.
  /// The reset runs on every entry (not only on leave) so a re-opened
  /// session never inherits stale "Connected! a.b.c.d", stale Yes-highlight,
  /// or stale Connecting status from a prior session.
  void open_provisioning(ProvisioningTransport active);

  /// Open the Yes/No confirmation overlay for a provisioning action.
  /// Sets _provisioning_confirm_kind = kind, resets
  /// _provisioning_confirm_index = 0 (No default), and switches to
  /// Screen::ProvisioningConfirm.  kind: 0 = switch transport,
  /// 1 = cancel setup.
  void open_provisioning_confirm(uint8_t kind);

  /// Latch the IP for the Provisioning-page success state.  Pass a
  /// non-zero network-byte-order IPv4 to set; pass 0 to clear when
  /// leaving the page.
  void set_provisioning_connected(uint32_t ip);

private:
  Config _config;

  // Pre-formatted about screen text
  char _about_firmware[32] = {};
  char _about_serial[32] = {};

  // Screen state
  Screen _screen = Screen::Home;
  Metric _active_metric = Metric::None;

  // Menu selection indices (per screen)
  uint8_t _menu_index = 0;
  uint8_t _settings_index = 1;
  uint8_t _settings_choice_index = 1;
  uint8_t _about_index = 1;
  uint8_t _confirm_index = 1;
  uint8_t _tag_list_index = 1;
  uint8_t _hardware_test_index = 1;
  uint8_t _peripheral_index = 0; // Actuator step: 0 = Pass, 1 = Fail.

  // Peripheral test view snapshot (owned by the orchestrator flow).
  PeripheralTestView _peripheral_view = {};

  // Scroll state
  uint8_t _settings_scroll_start = 0;
  uint8_t _settings_choice_scroll_start = 0;
  uint8_t _tag_scroll_start = 0;

  // Active settings choice context
  uint8_t _editing_setting_id = 0;

  // Active confirm context (which setting opened the confirm dialog)
  uint8_t _confirm_source_setting = 0;

  // Internal settings state (option indices).
  // Synced from GoSettings via sync_settings() at startup.
  uint8_t _setting_units = 0;            // 0=C, 1=F
  uint8_t _setting_altitude_unit = 0;    // 0=m, 1=ft
  uint8_t _setting_pm_display = 0;       // 0=ug/m3, 1=USAQI
  uint8_t _setting_measure_interval = 1; // default index 1 = "10s"
  uint8_t _setting_gps_mode = 1;         // 1="On When Tracking"
  uint8_t _setting_mode = 1;             // 1="Portable"
  uint8_t _setting_auto_lock = 0;        // 0="Off"
  uint8_t _setting_display_led = 0;      // 0="Off"
  uint8_t _setting_aqi_led = 0;          // 0="Off"
  uint8_t _setting_touch_led = 0;        // 0="Off"
  uint8_t _setting_buzzer_volume = 0;    // 0="Off"
  uint8_t _setting_melody = 0;           // 0="Off"

  // Snackbar
  char _snackbar_text[48] = {};
  uint32_t _snackbar_deadline_ms = 0;

  // BLE pairing
  uint32_t _ble_passkey = 0;

  // Stationary networking UI state
  ProvisioningTransport _provisioning_transport = ProvisioningTransport::BleOnly;
  /// Cursor on the Provisioning page: 0 = switch transport, 1 = cancel setup.
  uint8_t _provisioning_row_index = 0;
  ProvisioningUiState _provisioning_ui_state = ProvisioningUiState::Idle;
  /// ProvisioningConfirm overlay state.  0 = switch transport, 1 = cancel.
  uint8_t _provisioning_confirm_kind = 0;
  /// ProvisioningConfirm cursor.  0 = No (default highlight), 1 = Yes.
  uint8_t _provisioning_confirm_index = 0;
  /// Connected-state IPv4 (network byte order).  0 = no success yet.
  uint32_t _provisioning_connected_ip = 0;

  /// Captive-portal AP SSID ("airgradient-<serial>"), built once at ctor.
  /// Used by both the instruction line and the Wi-Fi QR encoder.
  char _ap_ssid_buf[36] = {};

  /// Shared QR for Provisioning / Getting Started (mutually exclusive).
  /// Re-encoded on screen entry / transport switch.
  AirgradientProvisioning::QrCode _qr = {};

  // true = boot gate (Start using -> Home); false = About -> Setup Guide (Back).
  bool _getting_started_from_boot = false;

  // Info screen text (UIManager owns the storage; the renderer borrows
  // the pointer via DisplayValues::info_text).
  static constexpr size_t INFO_TEXT_CAPACITY = 96;
  char _info_text[INFO_TEXT_CAPACITY] = {};

  // Cached "Connected! a.b.c.d" string (built on demand from the IP).
  // 11 prefix chars + 15 dotted-decimal + NUL = 27.  Padded to 32 for
  // future formatting tweaks.
  mutable char _provisioning_connected_text[32] = {};

  // Chart output buffer (mutable: written by const build_values)
  mutable float _chart_buf[UI_CHART_BUF_SIZE] = {};

  // Cached from last build_values call (mutable for const correctness)
  mutable bool _tracking_active = false;

  // --- Input dispatch (per screen) ---
  UIActionResult dispatch_home(InputSource source, InputType type);
  UIActionResult dispatch_menu(InputSource source, InputType type);
  UIActionResult dispatch_settings(InputSource source, InputType type);
  UIActionResult dispatch_settings_choice(InputSource source, InputType type);
  UIActionResult dispatch_about(InputSource source, InputType type);
  UIActionResult dispatch_confirm(InputSource source, InputType type);
  UIActionResult dispatch_tag_list(InputSource source, InputType type);
  UIActionResult dispatch_provisioning(InputSource source, InputType type);
  UIActionResult dispatch_provisioning_confirm(InputSource source, InputType type);
  UIActionResult dispatch_getting_started(InputSource source, InputType type);
  UIActionResult dispatch_hardware_test(InputSource source, InputType type);
  UIActionResult dispatch_peripheral_test(InputSource source, InputType type);
  UIActionResult dispatch_gps_test(InputSource source, InputType type);
  UIActionResult dispatch_accel_test(InputSource source, InputType type);

  // --- Navigation helpers ---
  void go_home();
  /// Navigate one level up to the parent screen (the same target as each
  /// screen's on-screen "Back" row, including parent-cursor restore).  No-op
  /// on Home, setup-session screens, and non-interactive screens.
  void navigate_back();
  void open_main_menu();
  void open_settings();
  void open_settings_choice(uint8_t setting_id);
  void open_about();
  void open_tag_list();
  void open_confirm(uint8_t source_setting);
  void open_hardware_test();
  void open_peripheral_test();
  void open_gps_test();
  void open_accel_test();

  // --- Movement helpers ---
  void move_menu(int delta);
  void move_settings(int delta);
  void move_settings_choice(int delta);
  void move_tag_list(int delta);
  void move_about(int delta);
  void move_confirm(int delta);
  void move_hardware_test(int delta);
  void move_provisioning(int delta);
  void move_provisioning_confirm(int delta);
  void browse_metric(int delta);

  // --- Row population ---
  void populate_menu_rows(DisplayValues &v) const;
  void populate_settings_rows(DisplayValues &v) const;
  void populate_settings_choice_rows(DisplayValues &v) const;
  void populate_about_rows(DisplayValues &v) const;
  void populate_confirm_rows(DisplayValues &v) const;
  void populate_tag_list_rows(DisplayValues &v) const;
  void populate_provisioning_rows(DisplayValues &v) const;
  void populate_provisioning_confirm_rows(DisplayValues &v) const;
  void populate_getting_started_rows(DisplayValues &v) const;
  void populate_hardware_test_rows(DisplayValues &v) const;
  void populate_peripheral_test_rows(DisplayValues &v) const;
  void populate_gps_test_rows(DisplayValues &v, const BuildContext &ctx) const;
  void populate_accel_test_rows(DisplayValues &v, const BuildContext &ctx) const;

  // --- Chart extraction ---
  void populate_chart(DisplayValues &v, const MeasuresAGo *cache, uint8_t cache_count) const;

  // --- Settings choice helpers ---
  uint8_t setting_option_count(uint8_t setting_id) const;
  uint8_t setting_current_option(uint8_t setting_id) const;
  void apply_setting_choice(uint8_t option_index);
  void sync_choice_scroll();

  // --- Internal queries ---
  bool snackbar_active() const;

  // --- Status-text mappers (presentation lives here, not in callers) ---
  const char *provisioning_status_text() const;

  // --- Provisioning QR ---
  /// Re-encode _qr per the active provisioning transport.  BleOnly ->
  /// companion-app URL; WifiOnly -> WIFI: descriptor from _ap_ssid_buf
  /// and _config.ap_password.  Failure leaves the QR empty.
  void _encode_provisioning_qr();
};

#endif // GO_UI_H
