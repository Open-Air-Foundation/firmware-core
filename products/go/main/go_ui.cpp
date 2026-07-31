#include "go_ui.h"

#include <cstdio>
#include <cstring>

#include "common.h"
#include "services/provisioning_qr.h"

// First-boot setup QR target. Short redirect -> low QR version, scannable
// on the 128x250 panel.
static constexpr const char *SETUP_GUIDE_URL = "https://l.airgradient.net/GO";

// ---------------------------------------------------------------------------
// Setting option labels
// ---------------------------------------------------------------------------

static const char *const UNITS_OPTIONS[] = {"C", "F"};
static constexpr uint8_t UNITS_COUNT = 2;

static const char *const PM_DISPLAY_OPTIONS[] = {"ug/m3", "USAQI"};
static constexpr uint8_t PM_DISPLAY_COUNT = 2;

static const char *const MEASURE_INTERVAL_OPTIONS[] = {"3s", "10s", "30s", "60s",
                                                       "5m", "15m", "1h"};
static constexpr uint8_t MEASURE_INTERVAL_COUNT = 7;

static const char *const GPS_MODE_OPTIONS[] = {"Always Off", "On When Tracking", "Always On"};
static constexpr uint8_t GPS_MODE_COUNT = 3;

static const char *const MODE_OPTIONS[] = {"Stationary", "Portable", "Offline / Airplane Mode"};
static constexpr uint8_t MODE_COUNT = 3;

static const char *const AUTO_LOCK_OPTIONS[] = {"Off", "10 Seconds", "30 Seconds", "60 Seconds"};
static constexpr uint8_t AUTO_LOCK_COUNT = 4;

static const char *const LED_BRIGHTNESS_OPTIONS[] = {"Off", "Dim", "Mid", "Bright"};
static constexpr uint8_t LED_BRIGHTNESS_COUNT = 4;

static const char *const TOUCH_LED_OPTIONS[] = {"Off", "Dim", "Bright"};
static constexpr uint8_t TOUCH_LED_COUNT = 3;

static const char *const BUZZER_OPTIONS[] = {"Off", "On"};
static constexpr uint8_t BUZZER_COUNT = 2;

static const char *const MELODY_OPTIONS[] = {"Chime", "Tetris"};
static constexpr uint8_t MELODY_COUNT = 2;

// Tag labels (indices 2..11 in the tag list screen)
static const char *const TAG_LABELS[] = {
    "Traffic Emissions", "Road Dust",         "Construction Work", "Biomass Burning",
    "Garbage Burning",   "Factory Emissions", "Smoking/Vaping",    "Cooking",
    "Paint/Solvents",    "Other Pollution",
};
static constexpr uint8_t TAG_COUNT = 10;

// ---------------------------------------------------------------------------
// Settings row index constants
// ---------------------------------------------------------------------------

static constexpr uint8_t SETTING_SETUP_GUIDE = 2;
static constexpr uint8_t SETTING_PAIR_WATCH = 3;
static constexpr uint8_t SETTING_UNITS = 4;
static constexpr uint8_t SETTING_PM_DISPLAY = 5;
static constexpr uint8_t SETTING_MEASURE_INTERVAL = 6;
static constexpr uint8_t SETTING_GPS_MODE = 7;
static constexpr uint8_t SETTING_MODE = 8;
static constexpr uint8_t SETTING_AUTO_LOCK = 9;
static constexpr uint8_t SETTING_DISPLAY_LED = 10;
static constexpr uint8_t SETTING_AQI_LED = 11;
static constexpr uint8_t SETTING_TOUCH_LED = 12;
static constexpr uint8_t SETTING_BUZZER = 13;
static constexpr uint8_t SETTING_PLAY_MELODY = 14;
static constexpr uint8_t SETTING_CO2_CALIBRATION = 15;
static constexpr uint8_t SETTING_CLEAR_DATA = 16;
static constexpr uint8_t SETTING_HARDWARE_TEST = 17; // navigation row -> Hardware Test submenu

static constexpr uint8_t SETTINGS_TOTAL = 18;       // indices 0..17
static constexpr uint8_t TAG_LIST_TOTAL = 12;       // indices 0..11
static constexpr uint8_t MAIN_MENU_TOTAL = 4;       // indices 0..3
static constexpr uint8_t CONFIRM_TOTAL = 5;         // indices 0..4
static constexpr uint8_t ABOUT_SELECTABLE_ROWS = 2; // indices 0..1

// ---------------------------------------------------------------------------
// Hardware Test submenu
// ---------------------------------------------------------------------------
// Rows: Exit, Back, Peripheral, GPS, Accel, FG Learning. The Accel row sits
// between GPS Test and FG Learning.
static constexpr uint8_t HW_TEST_PERIPHERAL = 2;
static constexpr uint8_t HW_TEST_GPS = 3;
static constexpr uint8_t HW_TEST_ACCEL = 4;
static constexpr uint8_t HW_TEST_FG_LEARNING = 5;
static constexpr uint8_t HW_TEST_TOTAL = 6;

/// Confirm-dialog source sentinel for the FG Learning arm. Kept outside the
/// real Settings index range so the shared Confirm screen can route it back to
/// the Hardware Test submenu instead of Settings.
static constexpr uint8_t SETTING_FG_LEARNING = 200;

/// Visible content items per page (excluding Exit/Back header rows).
static constexpr uint8_t PAGE_SIZE = 8;

/// Sentinel deadline: snackbar was just shown, not yet armed.
static constexpr uint32_t SNACKBAR_PENDING = UINT32_MAX;

/// Selectable metrics in browse order: None, Pm25, Co2, Temp, Humidity.
static constexpr Metric METRIC_CYCLE[] = {Metric::None, Metric::Pm25, Metric::Co2, Metric::Temp,
                                          Metric::Humidity};
static constexpr uint8_t METRIC_CYCLE_LEN = 5;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/// Copy text into a DisplayValues row slot.
static void copy_row(DisplayValues &v, uint8_t index, const char *text, bool disabled) {
  if (index >= MAX_LIST_ROWS)
    return;
  if (text != nullptr) {
    (void)snprintf(v.rows[index].text, sizeof(v.rows[index].text), "%s", text);
  } else {
    v.rows[index].text[0] = '\0';
  }
  v.rows[index].disabled = disabled;
}

/// Circular modulo that handles negative values.
static int wrap(int value, int count) { return ((value % count) + count) % count; }

/// Compute page-based scroll start for a given logical index.
/// Indices 0-1 (Exit/Back) always map to scroll 0.
static uint8_t page_scroll(uint8_t index) {
  if (index <= 1)
    return 0;
  return (uint8_t)(((index - 2) / PAGE_SIZE) * PAGE_SIZE);
}

/// Convert a logical list index to its display row, given the scroll offset.
/// Indices 0-1 map directly; content indices are offset by scroll.
static uint8_t display_row(uint8_t index, uint8_t scroll) {
  if (index <= 1)
    return index;
  return (uint8_t)(2 + (index - 2 - scroll));
}

// ---------------------------------------------------------------------------
// UIManager — construction
// ---------------------------------------------------------------------------

UIManager::UIManager(const Config &config) : _config(config) {
  (void)snprintf(_about_firmware, sizeof(_about_firmware), "Firmware %s",
                 _config.firmware_version ? _config.firmware_version : "?");
  (void)snprintf(_about_serial, sizeof(_about_serial), "Serial %s",
                 _config.serial_number ? _config.serial_number : "?");

  // Captive-portal SSID; matches ProvisioningManager's "airgradient-<serial>"
  // convention.  Reused by the instruction line and the Wi-Fi QR encoder.
  if (_config.serial_number != nullptr && _config.serial_number[0] != '\0') {
    (void)snprintf(_ap_ssid_buf, sizeof(_ap_ssid_buf), "airgradient-%s", _config.serial_number);
  } else {
    _ap_ssid_buf[0] = '\0';
  }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

UIActionResult UIManager::handle_input(InputSource source, InputType type) {
  // TouchEnter (CH1) gestures: long-press exits the menu to Home, double-press
  // navigates back one level.  Both are no-ops outside the regular menu tree
  // (Home, setup sessions, non-interactive screens).
  if (source == InputSource::TouchEnter) {
    if (type == InputType::LongPress) {
      const bool boot_onboarding = _screen == Screen::GettingStarted && _getting_started_from_boot;
      if (is_on_menu_screen() && !boot_onboarding) {
        go_home();
      }
      return {};
    }
    if (type == InputType::DoublePress) {
      navigate_back();
      return {};
    }
  }

  // Everything below is per-screen short-press navigation.
  if (type != InputType::ShortPress)
    return {};

  switch (_screen) {
  case Screen::Home:
    return dispatch_home(source, type);
  case Screen::MainMenu:
    return dispatch_menu(source, type);
  case Screen::Settings:
    return dispatch_settings(source, type);
  case Screen::SettingsChoice:
    return dispatch_settings_choice(source, type);
  case Screen::TagList:
    return dispatch_tag_list(source, type);
  case Screen::About:
    return dispatch_about(source, type);
  case Screen::Confirm:
    return dispatch_confirm(source, type);
  case Screen::Provisioning:
    return dispatch_provisioning(source, type);
  case Screen::ProvisioningConfirm:
    return dispatch_provisioning_confirm(source, type);
  case Screen::GettingStarted:
    return dispatch_getting_started(source, type);
  case Screen::HardwareTest:
    return dispatch_hardware_test(source, type);
  case Screen::PeripheralTest:
    return dispatch_peripheral_test(source, type);
  case Screen::GpsTest:
    return dispatch_gps_test(source, type);
  case Screen::AccelTest:
    return dispatch_accel_test(source, type);
  case Screen::PairWatch:
    return dispatch_pair_watch(source, type);
  case Screen::ShutdownUser:
  case Screen::ShutdownDischarge:
  case Screen::ShutdownTemperature:
  case Screen::PairingPasskey:
  case Screen::Info:
  // FG learning screens are owned by FgLearningRunner, not UIManager.
  case Screen::FgLearnCharging:
  case Screen::FgLearnResting:
  case Screen::FgLearnUnplug:
  case Screen::DischargeComplete:
  case Screen::FgLearnVerifying:
  case Screen::FgLearnComplete:
  case Screen::FgLearnFailed:
    // Info has no interactive elements.  Shutdown* / PairingPasskey have
    // no row-cursor either.  Drop all input.
    return {};
  }
  return {};
}

DisplayValues UIManager::build_values(const BuildContext &ctx) const {
  // Cache tracking state for handle_input decisions.
  _tracking_active = ctx.tracking_active;

  DisplayValues v{};

  // --- Sensor readings (channel A) ---
  v.co2_ppm = ctx.sensor_data.co2.co2;
  v.pm25_ugm3 = ctx.sensor_data.pm_a.pm_25;
  v.temperature_c = ctx.sensor_data.temp_hum_a.temperature;
  v.humidity_pct = ctx.sensor_data.temp_hum_a.humidity;
  v.tvoc_index = ctx.sensor_data.tvoc_nox.tvoc_index;
  v.nox_index = ctx.sensor_data.tvoc_nox.nox_index;
  v.pressure_hpa = ctx.sensor_data.pressure.pressure;
  v.altitude_m = ctx.sensor_data.pressure.altitude;

  // --- Battery ---
  v.battery_pct = ctx.battery_pct;
  v.is_battery_charging = ctx.is_battery_charging;
  v.is_plugged_in = ctx.is_plugged_in;

  // --- Status flags ---
  v.locked = ctx.locked;
  v.ble_enabled = ctx.ble_enabled;
  v.ble_connected = ctx.ble_connected;
  v.wifi_enabled = ctx.wifi_enabled;
  v.wifi_connected = ctx.wifi_connected;
  v.gps_enabled = ctx.gps_enabled;
  v.gps_fix = ctx.gps_fix;
  v.tracking_active = ctx.tracking_active;
  v.display_off = ctx.display_off;
  v.use_fahrenheit = ctx.use_fahrenheit;
  v.pm_use_usaqi = ctx.pm_use_usaqi;

  // --- Screen navigation ---
  v.screen = _screen;
  v.active_metric = _active_metric;

  // --- Populate rows based on current screen ---
  switch (_screen) {
  case Screen::Home:
    populate_chart(v, ctx.cache, ctx.cache_count);
    break;
  case Screen::MainMenu:
    populate_menu_rows(v);
    populate_chart(v, ctx.cache, ctx.cache_count);
    break;
  case Screen::Settings:
    populate_settings_rows(v);
    break;
  case Screen::SettingsChoice:
    populate_settings_choice_rows(v);
    break;
  case Screen::TagList:
    populate_tag_list_rows(v);
    break;
  case Screen::About:
    populate_about_rows(v);
    break;
  case Screen::Confirm:
    populate_confirm_rows(v);
    break;
  case Screen::ShutdownUser:
  case Screen::ShutdownDischarge:
  case Screen::ShutdownTemperature:
    break;
  case Screen::PairingPasskey:
    v.ble_passkey = _ble_passkey;
    break;
  case Screen::PairWatch:
    v.watch_pairing_view = _watch_pairing_view;
    v.watch_pairing_number = _watch_pairing_number;
    v.watch_pairing_action_index = _watch_pairing_action_index;
    break;
  case Screen::Provisioning:
    populate_provisioning_rows(v);
    break;
  case Screen::ProvisioningConfirm:
    populate_provisioning_confirm_rows(v);
    break;
  case Screen::Info:
    // Info renders v.info_text directly; no row population needed.
    break;
  case Screen::GettingStarted:
    populate_getting_started_rows(v);
    break;
  case Screen::HardwareTest:
    populate_hardware_test_rows(v);
    break;
  case Screen::PeripheralTest:
    populate_peripheral_test_rows(v);
    break;
  case Screen::GpsTest:
    populate_gps_test_rows(v, ctx);
    break;
  case Screen::AccelTest:
    populate_accel_test_rows(v, ctx);
    break;
  // FG learning screens are built by FgLearningRunner, not UIManager.
  case Screen::FgLearnCharging:
  case Screen::FgLearnResting:
  case Screen::FgLearnUnplug:
  case Screen::DischargeComplete:
  case Screen::FgLearnVerifying:
  case Screen::FgLearnComplete:
  case Screen::FgLearnFailed:
    break;
  }

  // Provisioning-screen status surfaces here so the renderer can show
  // it on Screen::Provisioning. Home conveys network state purely via
  // the status-bar Wi-Fi icon (spec: no persistent Home status line).
  v.provisioning_transport = static_cast<uint8_t>(_provisioning_transport);
  v.provisioning_status = provisioning_status_text();
  v.provisioning_connected_ip = _provisioning_connected_ip;
  v.provisioning_confirm_kind = _provisioning_confirm_kind;
  v.provisioning_confirm_index = _provisioning_confirm_index;

  // Captive-portal SSID/password for Provisioning instruction lines.
  // SSID is null when no serial was configured -> renderer placeholder.
  v.provisioning_ap_ssid = (_ap_ssid_buf[0] != '\0') ? _ap_ssid_buf : nullptr;
  v.provisioning_ap_password = _config.ap_password;

  // Only publish the QR pointer on the Provisioning screen to avoid
  // pinning stale state in frames built for other screens.
  // Only publish the QR on screens that render it; avoids pinning stale state.
  if (_screen == Screen::Provisioning || _screen == Screen::GettingStarted) {
    v.qr = &_qr;
  }

  // --- Info screen text ---
  v.info_text = (_screen == Screen::Info && _info_text[0] != '\0') ? _info_text : nullptr;

  // --- Snackbar ---
  // Snackbars are suppressed on every session screen so a stale "Mode
  // changed" / "Locked" / "Wi-Fi connected" cannot leak onto the bring-up
  // narration or the Provisioning page.  The orchestrator clears the
  // snackbar buffer on session entry; this is a belt-and-braces guard.
  const bool session_screen =
      (_screen == Screen::Info || _screen == Screen::Provisioning ||
       _screen == Screen::ProvisioningConfirm || _screen == Screen::GettingStarted);
  v.snackbar_text = (!session_screen && snackbar_active()) ? _snackbar_text : nullptr;

  return v;
}

void UIManager::set_screen(Screen screen) { _screen = screen; }

Screen UIManager::current_screen() const { return _screen; }

bool UIManager::is_on_menu_screen() const {
  switch (_screen) {
  case Screen::MainMenu:
  case Screen::Settings:
  case Screen::SettingsChoice:
  case Screen::TagList:
  case Screen::Confirm:
  case Screen::About:
  case Screen::HardwareTest:
  case Screen::PeripheralTest:
  // Live test screens; the orchestrator drives their refresh, so suppress
  // background re-renders (and enable long-press-to-Home / double-press-back).
  case Screen::GpsTest:
  case Screen::AccelTest:
  // Suppress background re-renders over the static QR page.
  case Screen::GettingStarted:
    return true;
  default:
    return false;
  }
}

bool UIManager::is_focus_screen() const {
  return _screen == Screen::PairingPasskey || _screen == Screen::PairWatch;
}

bool UIManager::is_hardware_test_screen() const {
  return _screen == Screen::HardwareTest || _screen == Screen::PeripheralTest ||
         _screen == Screen::GpsTest || _screen == Screen::AccelTest;
}

void UIManager::show_snackbar(const char *text) {
  if (text == nullptr) {
    _snackbar_text[0] = '\0';
    _snackbar_deadline_ms = 0;
    return;
  }
  (void)snprintf(_snackbar_text, sizeof(_snackbar_text), "%s", text);
  _snackbar_deadline_ms = SNACKBAR_PENDING;
}

void UIManager::clear_expired_snackbar(uint32_t now_ms) {
  if (_snackbar_text[0] == '\0')
    return;

  // Arm the deadline on first clear call after show_snackbar.
  if (_snackbar_deadline_ms == SNACKBAR_PENDING) {
    _snackbar_deadline_ms = now_ms + SNACKBAR_DURATION_MS;
    return;
  }

  // Check expiry (wraparound-safe).
  if (_snackbar_deadline_ms != 0 && (int32_t)(_snackbar_deadline_ms - now_ms) <= 0) {
    _snackbar_text[0] = '\0';
    _snackbar_deadline_ms = 0;
  }
}

void UIManager::sync_settings(const GoSettings &s) {
  _setting_units = s.use_fahrenheit ? 1 : 0;
  _setting_pm_display = s.pm_use_usaqi ? 1 : 0;

  // Map interval seconds to option index.
  // Options: "3s"=0, "10s"=1, "30s"=2, "60s"=3, "5m"=4, "15m"=5, "1h"=6
  // Display interval also has "Display Off"=7 (seconds == 0).
  // PM/other sensor intervals use "Off"=7 (seconds == 0).
  static constexpr int INTERVAL_SECONDS[] = {3, 10, 30, 60, 300, 900, 3600};
  static constexpr uint8_t INTERVAL_COUNT = 7;

  auto seconds_to_index = [](int seconds, bool has_off) -> uint8_t {
    if (seconds <= 0)
      return has_off ? 7 : 0;
    for (uint8_t i = 0; i < INTERVAL_COUNT; ++i) {
      if (seconds <= INTERVAL_SECONDS[i])
        return i;
    }
    return 6; // >= 1h → clamp to "1h"
  };

  _setting_measure_interval = seconds_to_index(s.measure_interval_seconds, false);

  // GPS mode
  switch (s.gps_mode) {
  case GpsMode::AlwaysOff:
    _setting_gps_mode = 0;
    break;
  case GpsMode::OnWhenTracking:
    _setting_gps_mode = 1;
    break;
  case GpsMode::AlwaysOn:
    _setting_gps_mode = 2;
    break;
  }

  // Operating mode
  switch (s.operating_mode) {
  case OperatingMode::Stationary:
    _setting_mode = 0;
    break;
  case OperatingMode::Portable:
    _setting_mode = 1;
    break;
  case OperatingMode::Offline:
    _setting_mode = 2;
    break;
  }

  // Auto-lock: 0=Off, 10=1, 30=2, 60=3
  if (s.auto_lock_seconds <= 0)
    _setting_auto_lock = 0;
  else if (s.auto_lock_seconds <= 10)
    _setting_auto_lock = 1;
  else if (s.auto_lock_seconds <= 30)
    _setting_auto_lock = 2;
  else
    _setting_auto_lock = 3;

  // LED settings — enum values map directly to option indices
  _setting_display_led = static_cast<uint8_t>(s.front_led_brightness);
  _setting_aqi_led = static_cast<uint8_t>(s.back_led_brightness);
  _setting_touch_led = static_cast<uint8_t>(s.touch_led_intensity);
  _setting_buzzer_volume = s.buzzer_enabled ? 1 : 0;
}

void UIManager::apply_to_settings(GoSettings &settings) const {
  // Boolean flags
  settings.use_fahrenheit = (_setting_units == 1);
  settings.pm_use_usaqi = (_setting_pm_display == 1);

  // Map interval option index back to seconds.
  // Indices 0-6 map to {3, 10, 30, 60, 300, 900, 3600}; index 7 = 0 (Off).
  static constexpr int INTERVAL_SECONDS[] = {3, 10, 30, 60, 300, 900, 3600};
  static constexpr uint8_t INTERVAL_COUNT = 7;

  auto index_to_seconds = [](uint8_t index) -> int {
    if (index < INTERVAL_COUNT)
      return INTERVAL_SECONDS[index];
    return 0; // index 7 = Off / Display Off
  };

  settings.measure_interval_seconds = index_to_seconds(_setting_measure_interval);

  // GPS mode
  switch (_setting_gps_mode) {
  case 0:
    settings.gps_mode = GpsMode::AlwaysOff;
    break;
  case 1:
    settings.gps_mode = GpsMode::OnWhenTracking;
    break;
  case 2:
    settings.gps_mode = GpsMode::AlwaysOn;
    break;
  default:
    settings.gps_mode = GpsMode::OnWhenTracking;
    break;
  }

  // Operating mode
  switch (_setting_mode) {
  case 0:
    settings.operating_mode = OperatingMode::Stationary;
    break;
  case 1:
    settings.operating_mode = OperatingMode::Portable;
    break;
  case 2:
    settings.operating_mode = OperatingMode::Offline;
    break;
  default:
    settings.operating_mode = OperatingMode::Offline;
    break;
  }

  // Auto-lock: index 0=Off(0s), 1=10s, 2=30s, 3=60s
  static constexpr int AUTO_LOCK_SECONDS[] = {0, 10, 30, 60};
  settings.auto_lock_seconds = (_setting_auto_lock < 4) ? AUTO_LOCK_SECONDS[_setting_auto_lock] : 0;

  // LED settings — option indices map directly to enum values
  settings.front_led_brightness = static_cast<LedBrightness>(_setting_display_led);
  settings.back_led_brightness = static_cast<LedBrightness>(_setting_aqi_led);
  settings.touch_led_intensity = static_cast<TouchLedIntensity>(_setting_touch_led);
  settings.buzzer_enabled = (_setting_buzzer_volume == 1);
}

void UIManager::reset_to_home() {
  _screen = Screen::Home;
  _active_metric = Metric::None;
}

void UIManager::show_pairing_passkey(uint32_t passkey) {
  _ble_passkey = passkey;
  _screen = Screen::PairingPasskey;
}

void UIManager::dismiss_pairing_passkey() {
  _ble_passkey = 0;
  if (_screen == Screen::PairingPasskey) {
    _screen = Screen::Home;
    _active_metric = Metric::None;
  }
}

void UIManager::show_watch_pairing_waiting() {
  _watch_pairing_view = WatchPairingView::Waiting;
  _watch_pairing_number = 0;
  _watch_pairing_action_index = 0;
  _screen = Screen::PairWatch;
}

void UIManager::show_watch_numeric_comparison(uint32_t number) {
  _watch_pairing_view = WatchPairingView::NumericComparison;
  _watch_pairing_number = number;
  _watch_pairing_action_index = 1;
  _screen = Screen::PairWatch;
}

void UIManager::show_watch_pairing_confirming() {
  _watch_pairing_view = WatchPairingView::Confirming;
  _screen = Screen::PairWatch;
}

void UIManager::show_watch_pairing_result(bool success) {
  _watch_pairing_view = success ? WatchPairingView::Success : WatchPairingView::Failure;
  _watch_pairing_number = 0;
  _screen = Screen::PairWatch;
}

void UIManager::dismiss_watch_pairing() {
  _watch_pairing_number = 0;
  _watch_pairing_action_index = 0;
  if (_screen == Screen::PairWatch) {
    _screen = Screen::Settings;
    _settings_index = SETTING_PAIR_WATCH;
    _settings_scroll_start = page_scroll(_settings_index);
  }
}

// ---------------------------------------------------------------------------
// Stationary networking surface
// ---------------------------------------------------------------------------

void UIManager::set_provisioning_transport(ProvisioningTransport t) {
  _provisioning_transport = t;
  // Cursor always lands on the switch-transport row (the inactive option).
  _provisioning_row_index = 0;
  // QR payload depends on the active transport.
  _encode_provisioning_qr();
}

ProvisioningTransport UIManager::provisioning_transport() const { return _provisioning_transport; }

void UIManager::set_provisioning_ui_state(ProvisioningUiState s) { _provisioning_ui_state = s; }

void UIManager::show_info(const char *text) {
  if (text == nullptr) {
    _info_text[0] = '\0';
  } else {
    (void)snprintf(_info_text, sizeof(_info_text), "%s", text);
  }
  _screen = Screen::Info;
}

const char *wifi_failure_text(WifiDisconnectReason reason) {
  switch (reason) {
  case WifiDisconnectReason::auth_failed:
    return "Wrong password";
  case WifiDisconnectReason::no_ap_found:
    return "Network not found";
  case WifiDisconnectReason::assoc_failed:
    return "Connection refused";
  case WifiDisconnectReason::dhcp_failed:
    return "No IP address";
  case WifiDisconnectReason::connection_lost:
    return "No response";
  default:
    return "Connection failed";
  }
}

void UIManager::show_getting_started(bool from_boot) {
  _getting_started_from_boot = from_boot;
  // Encode setup QR on entry; failure leaves it empty (renderer skips it).
  (void)AirgradientProvisioning::encode_url_qr(SETUP_GUIDE_URL, &_qr);
  _screen = Screen::GettingStarted;
}

void UIManager::open_provisioning(ProvisioningTransport active) {
  // Idempotent reset — see header for the rationale.  Every entry starts
  // with a clean per-session UI sub-state regardless of how the prior
  // session was torn down.
  _provisioning_transport = active;
  _provisioning_row_index = 0;
  _provisioning_ui_state = ProvisioningUiState::WaitingForCredentials;
  _provisioning_confirm_kind = 0;
  _provisioning_confirm_index = 0;
  _provisioning_connected_ip = 0;
  _screen = Screen::Provisioning;
  _encode_provisioning_qr();
}

void UIManager::open_provisioning_confirm(uint8_t kind) {
  _provisioning_confirm_kind = kind;
  _provisioning_confirm_index = 0; // default highlight on "No"
  _screen = Screen::ProvisioningConfirm;
}

void UIManager::set_provisioning_connected(uint32_t ip) {
  _provisioning_connected_ip = ip;
  if (ip != 0) {
    _provisioning_ui_state = ProvisioningUiState::Connected;
  }
}

const char *UIManager::provisioning_status_text() const {
  // Connected state: formatted "Connected! a.b.c.d" rebuilt on demand so
  // the buffer stays valid across the next DisplayValues snapshot.
  if (_provisioning_ui_state == ProvisioningUiState::Connected || _provisioning_connected_ip != 0) {
    char ip_str[16];
    format_ipv4_be(_provisioning_connected_ip, ip_str);
    (void)snprintf(_provisioning_connected_text, sizeof(_provisioning_connected_text),
                   "Connected! %s", ip_str);
    return _provisioning_connected_text;
  }

  const bool ble = _provisioning_transport == ProvisioningTransport::BleOnly;
  switch (_provisioning_ui_state) {
  case ProvisioningUiState::WaitingForCredentials:
    return ble ? "Waiting for app..." : "Waiting for setup...";
  case ProvisioningUiState::SwitchingTransport:
    // Active transport is the source; the text names the target.
    return ble ? "Switching to Wi-Fi..." : "Switching to BLE...";
  case ProvisioningUiState::Connecting:
    return "Connecting...";
  case ProvisioningUiState::ConnectFailed:
    return "Connect failed - try again";
  case ProvisioningUiState::Connected:
    // Handled above when _provisioning_connected_ip is set.  Falling
    // through here means the state was set without a populated IP — show
    // the generic word and let the orchestrator fix up the IP next frame.
    return "Connected!";
  case ProvisioningUiState::Idle:
    return nullptr;
  }
  return nullptr;
}

void UIManager::_encode_provisioning_qr() {
  // BleOnly -> companion-app URL; WifiOnly -> WIFI: join descriptor.
  // On failure the QR stays zeroed and the renderer no-ops.
  if (_provisioning_transport == ProvisioningTransport::BleOnly) {
    (void)AirgradientProvisioning::encode_go_to_app_qr(&_qr);
    return;
  }
  const char *password = (_config.ap_password != nullptr) ? _config.ap_password : "";
  (void)AirgradientProvisioning::encode_wifi_qr(_ap_ssid_buf, password,
                                                AirgradientProvisioning::WifiAuth::Wpa, &_qr);
}

// ---------------------------------------------------------------------------
// Internal queries
// ---------------------------------------------------------------------------

bool UIManager::snackbar_active() const {
  return _snackbar_text[0] != '\0' && _snackbar_deadline_ms != 0;
}

// ---------------------------------------------------------------------------
// Navigation helpers
// ---------------------------------------------------------------------------

void UIManager::go_home() { _screen = Screen::Home; }

void UIManager::navigate_back() {
  // Single source of truth for parent navigation, shared by the on-screen
  // "Back" rows and the double-press gesture.  Mirrors each screen's parent,
  // restoring the parent cursor where the "Back" rows did.
  switch (_screen) {
  case Screen::MainMenu:
    go_home();
    break;
  case Screen::Settings:
    _screen = Screen::MainMenu;
    _menu_index = 2; // cursor on "Settings"
    break;
  case Screen::SettingsChoice:
    _screen = Screen::Settings;
    break;
  case Screen::About:
    _screen = Screen::MainMenu;
    _menu_index = 3; // cursor on "About Device"
    break;
  case Screen::Confirm:
    if (_confirm_source_setting == SETTING_FG_LEARNING) {
      // FG Learning confirm lives under the Hardware Test submenu.
      open_hardware_test();
      _hardware_test_index = HW_TEST_FG_LEARNING;
    } else {
      _screen = Screen::Settings;
      _settings_index = _confirm_source_setting;
      _settings_scroll_start = page_scroll(_settings_index);
    }
    break;
  case Screen::HardwareTest:
    _screen = Screen::Settings;
    _settings_index = SETTING_HARDWARE_TEST;
    _settings_scroll_start = page_scroll(_settings_index);
    break;
  case Screen::PeripheralTest:
    // Leave the flow → back to the Hardware Test submenu on the Peripheral row.
    open_hardware_test();
    _hardware_test_index = HW_TEST_PERIPHERAL;
    break;
  case Screen::GpsTest:
    // Leave the live screen → Hardware Test submenu on the GPS row.
    open_hardware_test();
    _hardware_test_index = HW_TEST_GPS;
    break;
  case Screen::AccelTest:
    // Leave the live screen → Hardware Test submenu on the Accel row.
    open_hardware_test();
    _hardware_test_index = HW_TEST_ACCEL;
    break;
  case Screen::TagList:
    _screen = Screen::MainMenu;
    _menu_index = 2;
    break;
  case Screen::GettingStarted:
    if (!_getting_started_from_boot) {
      _screen = Screen::Settings;
      _settings_index = SETTING_SETUP_GUIDE;
      _settings_scroll_start = page_scroll(_settings_index);
    }
    break;
  default:
    // Home, Provisioning/ProvisioningConfirm (setup session), and
    // non-interactive screens have no back target.
    break;
  }
}

void UIManager::open_main_menu() {
  _active_metric = Metric::None; // clear selection behind the overlay
  _screen = Screen::MainMenu;
  _menu_index = 0;
}

void UIManager::open_settings() {
  _screen = Screen::Settings;
  _settings_index = 1;
  _settings_scroll_start = 0;
}

void UIManager::open_settings_choice(uint8_t setting_id) {
  _editing_setting_id = setting_id;
  _screen = Screen::SettingsChoice;
  // Pre-select the currently active option.
  _settings_choice_index = (uint8_t)(2 + setting_current_option(setting_id));
  sync_choice_scroll();
}

void UIManager::open_about() {
  _screen = Screen::About;
  _about_index = 1;
}

void UIManager::open_tag_list() {
  _screen = Screen::TagList;
  _tag_list_index = 1;
  _tag_scroll_start = 0;
}

void UIManager::open_confirm(uint8_t source_setting) {
  _confirm_source_setting = source_setting;
  _screen = Screen::Confirm;
  _confirm_index = 1;
}

void UIManager::open_hardware_test() {
  _screen = Screen::HardwareTest;
  _hardware_test_index = 1; // land on Back
}

void UIManager::open_peripheral_test() {
  _screen = Screen::PeripheralTest;
  _peripheral_index = 0; // default cursor on Pass
  // Sensible default until the orchestrator pushes the first step view.
  _peripheral_view = PeripheralTestView{};
  _peripheral_view.kind = PeripheralTestView::Kind::Actuator;
  _peripheral_view.prompt = "Front LED on?";
}

void UIManager::set_peripheral_test_view(const PeripheralTestView &view) {
  _peripheral_view = view;
  // Re-arm the Pass/Fail cursor whenever a fresh actuator step appears.
  if (view.kind == PeripheralTestView::Kind::Actuator) {
    _peripheral_index = 0;
  }
}

void UIManager::open_gps_test() { _screen = Screen::GpsTest; }

void UIManager::open_accel_test() { _screen = Screen::AccelTest; }

// ---------------------------------------------------------------------------
// Movement helpers
// ---------------------------------------------------------------------------

void UIManager::move_menu(int delta) {
  // Circular navigation — all 4 rows are always enabled.
  _menu_index = (uint8_t)wrap((int)_menu_index + delta, MAIN_MENU_TOTAL);
}

void UIManager::move_settings(int delta) {
  // Circular navigation, page-based scroll.
  _settings_index = (uint8_t)wrap((int)_settings_index + delta, SETTINGS_TOTAL);
  _settings_scroll_start = page_scroll(_settings_index);
}

void UIManager::move_settings_choice(int delta) {
  // Circular wrapping (including Exit/Back).
  int total = 2 + (int)setting_option_count(_editing_setting_id);
  _settings_choice_index = (uint8_t)wrap((int)_settings_choice_index + delta, total);
  sync_choice_scroll();
}

void UIManager::move_tag_list(int delta) {
  // Circular navigation, page-based scroll.
  _tag_list_index = (uint8_t)wrap((int)_tag_list_index + delta, TAG_LIST_TOTAL);
  _tag_scroll_start = page_scroll(_tag_list_index);
}

void UIManager::move_about(int delta) {
  // Circular between Exit (0) and Back (1).
  _about_index = (uint8_t)wrap((int)_about_index + delta, ABOUT_SELECTABLE_ROWS);
}

void UIManager::move_confirm(int delta) {
  // Circular across all 5 rows (index 2 is non-selectable but navigable).
  _confirm_index = (uint8_t)wrap((int)_confirm_index + delta, CONFIRM_TOTAL);
}

void UIManager::move_hardware_test(int delta) {
  // Circular navigation across all submenu rows.
  _hardware_test_index = (uint8_t)wrap((int)_hardware_test_index + delta, HW_TEST_TOTAL);
}

void UIManager::browse_metric(int delta) {
  // No display_off guard here — browse_metric is only reachable from
  // dispatch_home() → handle_input(), which the orchestrator only calls
  // when unlocked.  When unlocked the dashboard is always visible
  // (display_off is suppressed by the orchestrator), so browsing is valid.
  //
  // Cycle through 5 selectable entries: None, Pm25, Co2, Temp, Humidity.
  // TVOC and NOx remain in the enum but are not selectable.
  int idx = 0;
  for (int i = 0; i < METRIC_CYCLE_LEN; ++i) {
    if (METRIC_CYCLE[i] == _active_metric) {
      idx = i;
      break;
    }
  }
  idx = wrap(idx + delta, METRIC_CYCLE_LEN);
  _active_metric = METRIC_CYCLE[idx];
}

// ---------------------------------------------------------------------------
// Settings choice helpers
// ---------------------------------------------------------------------------

uint8_t UIManager::setting_option_count(uint8_t setting_id) const {
  switch (setting_id) {
  case SETTING_UNITS:
    return UNITS_COUNT;
  case SETTING_PM_DISPLAY:
    return PM_DISPLAY_COUNT;
  case SETTING_MEASURE_INTERVAL:
    return MEASURE_INTERVAL_COUNT;
  case SETTING_GPS_MODE:
    return GPS_MODE_COUNT;
  case SETTING_MODE:
    return MODE_COUNT;
  case SETTING_AUTO_LOCK:
    return AUTO_LOCK_COUNT;
  case SETTING_DISPLAY_LED:
  case SETTING_AQI_LED:
    return LED_BRIGHTNESS_COUNT;
  case SETTING_TOUCH_LED:
    return TOUCH_LED_COUNT;
  case SETTING_BUZZER:
    return BUZZER_COUNT;
  case SETTING_PLAY_MELODY:
    return MELODY_COUNT;
  default:
    return 0;
  }
}

uint8_t UIManager::setting_current_option(uint8_t setting_id) const {
  switch (setting_id) {
  case SETTING_UNITS:
    return _setting_units;
  case SETTING_PM_DISPLAY:
    return _setting_pm_display;
  case SETTING_MEASURE_INTERVAL:
    return _setting_measure_interval;
  case SETTING_GPS_MODE:
    return _setting_gps_mode;
  case SETTING_MODE:
    return _setting_mode;
  case SETTING_AUTO_LOCK:
    return _setting_auto_lock;
  case SETTING_DISPLAY_LED:
    return _setting_display_led;
  case SETTING_AQI_LED:
    return _setting_aqi_led;
  case SETTING_TOUCH_LED:
    return _setting_touch_led;
  case SETTING_BUZZER:
    return _setting_buzzer_volume;
  case SETTING_PLAY_MELODY:
    return _setting_melody;
  default:
    return 0;
  }
}

void UIManager::apply_setting_choice(uint8_t option_index) {
  // The UI Manager updates its internal state and returns
  // UIAction::SettingsChanged. The orchestrator is responsible for
  // reading back the new values and persisting to NVS.

  switch (_editing_setting_id) {
  case SETTING_UNITS:
    _setting_units = option_index;
    break;
  case SETTING_PM_DISPLAY:
    _setting_pm_display = option_index;
    break;
  case SETTING_MEASURE_INTERVAL:
    _setting_measure_interval = option_index;
    break;
  case SETTING_GPS_MODE:
    _setting_gps_mode = option_index;
    break;
  case SETTING_MODE:
    _setting_mode = option_index;
    break;
  case SETTING_AUTO_LOCK:
    _setting_auto_lock = option_index;
    break;
  case SETTING_DISPLAY_LED:
    _setting_display_led = option_index;
    break;
  case SETTING_AQI_LED:
    _setting_aqi_led = option_index;
    break;
  case SETTING_TOUCH_LED:
    _setting_touch_led = option_index;
    break;
  case SETTING_BUZZER:
    _setting_buzzer_volume = option_index;
    break;
  case SETTING_PLAY_MELODY:
    _setting_melody = option_index;
    break;
  default:
    break;
  }

  // Return to Settings screen.
  _screen = Screen::Settings;
  _settings_choice_index = 1;
  _settings_choice_scroll_start = 0;
  _editing_setting_id = 0;
}

void UIManager::sync_choice_scroll() {
  uint8_t option_count = setting_option_count(_editing_setting_id);

  // No scrolling needed for header rows or short lists.
  if (_settings_choice_index <= 1 || option_count <= PAGE_SIZE) {
    _settings_choice_scroll_start = 0;
    return;
  }

  // Sliding-window scroll: keep the selected option visible.
  uint8_t option_idx = (uint8_t)(_settings_choice_index - 2);
  if (option_idx < _settings_choice_scroll_start) {
    _settings_choice_scroll_start = option_idx;
  } else if (option_idx >= (uint8_t)(_settings_choice_scroll_start + PAGE_SIZE)) {
    _settings_choice_scroll_start = (uint8_t)(option_idx - (PAGE_SIZE - 1));
  }

  uint8_t max_scroll = (option_count > PAGE_SIZE) ? (uint8_t)(option_count - PAGE_SIZE) : 0;
  if (_settings_choice_scroll_start > max_scroll) {
    _settings_choice_scroll_start = max_scroll;
  }
}

// ---------------------------------------------------------------------------
// Input dispatch — per screen
// ---------------------------------------------------------------------------

UIActionResult UIManager::dispatch_home(InputSource source, InputType type) {
  (void)type;
  switch (source) {
  case InputSource::TouchUp:
    browse_metric(-1);
    break;
  case InputSource::TouchDown:
    browse_metric(1);
    break;
  case InputSource::TouchEnter:
    open_main_menu();
    break;
  default:
    break;
  }
  return {};
}

UIActionResult UIManager::dispatch_menu(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_menu(-1);
    break;
  case InputSource::TouchDown:
    move_menu(1);
    break;
  case InputSource::TouchEnter:
    switch (_menu_index) {
    case 0: // Exit Menu
      go_home();
      break;
    case 1: // Start / Stop Tracking
      if (_tracking_active) {
        go_home();
        result.action = UIAction::StopTracking;
      } else {
        go_home();
        result.action = UIAction::StartTracking;
      }
      break;
    case 2: // Settings
      open_settings();
      break;
    case 3: // About Device
      open_about();
      break;
    default:
      break;
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_settings(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_settings(-1);
    break;
  case InputSource::TouchDown:
    move_settings(1);
    break;
  case InputSource::TouchEnter:
    if (_settings_index == 0) {
      // Exit → Home
      go_home();
    } else if (_settings_index == 1) {
      // Back → MainMenu (cursor on "Settings")
      navigate_back();
    } else {
      if (_settings_index == SETTING_SETUP_GUIDE) {
        // Setup Guide → Getting Started (Back returns here)
        show_getting_started(/*from_boot=*/false);
      } else if (_settings_index == SETTING_HARDWARE_TEST) {
        // Hardware Test → submenu (Back returns here)
        open_hardware_test();
      } else if (_settings_index == SETTING_PAIR_WATCH) {
        show_watch_pairing_waiting();
        result.action = UIAction::PairWatchRequested;
      } else if (_settings_index == SETTING_CO2_CALIBRATION ||
                 _settings_index == SETTING_CLEAR_DATA) {
        // Open confirm dialog for action items
        open_confirm(_settings_index);
      } else if (_settings_index == SETTING_PLAY_MELODY) {
        // Open choice screen for Play Melody
        open_settings_choice(_settings_index);
      } else if (_settings_index >= SETTING_UNITS && _settings_index <= SETTING_BUZZER) {
        // Open choice screen for this setting
        open_settings_choice(_settings_index);
      }
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_settings_choice(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_settings_choice(-1);
    break;
  case InputSource::TouchDown:
    move_settings_choice(1);
    break;
  case InputSource::TouchEnter:
    if (_settings_choice_index == 0) {
      // Exit → Home
      go_home();
    } else if (_settings_choice_index == 1) {
      // Back → Settings
      navigate_back();
    } else {
      // Apply chosen option
      uint8_t option_index = (uint8_t)(_settings_choice_index - 2);

      if (_editing_setting_id == SETTING_PLAY_MELODY) {
        // Play Melody is a transient action, not a persistent setting.
        // option_index 0 = Chime, 1 = Tetris -> MelodySelect 1, 2
        apply_setting_choice(option_index);
        result.action = UIAction::PlayMelody;
        result.melody = static_cast<MelodySelect>(option_index + 1);
      } else if (_editing_setting_id == SETTING_MODE) {
        // Mode change has its own UIAction with the new mode.
        apply_setting_choice(option_index);
        // Exit to Home so the mode status icon updates in context.
        // Stationary entry overrides this with its Info screen.
        go_home();
        result.action = UIAction::ChangeMode;
        switch (option_index) {
        case 0:
          result.new_mode = OperatingMode::Stationary;
          break;
        case 1:
          result.new_mode = OperatingMode::Portable;
          break;
        case 2:
        default:
          result.new_mode = OperatingMode::Offline;
          break;
        }
      } else {
        apply_setting_choice(option_index);
        result.action = UIAction::SettingsChanged;
      }
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_about(InputSource source, InputType type) {
  (void)type;

  switch (source) {
  case InputSource::TouchUp:
    move_about(-1);
    break;
  case InputSource::TouchDown:
    move_about(1);
    break;
  case InputSource::TouchEnter:
    if (_about_index == 0) {
      // Exit → Home
      go_home();
    } else if (_about_index == 1) {
      // Back → MainMenu (cursor on "About Device")
      navigate_back();
    }
    break;
  default:
    break;
  }
  return {};
}

UIActionResult UIManager::dispatch_confirm(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_confirm(-1);
    break;
  case InputSource::TouchDown:
    move_confirm(1);
    break;
  case InputSource::TouchEnter:
    switch (_confirm_index) {
    case 0: // Exit → Home
      go_home();
      break;
    case 1: // Back → Settings (cursor on source setting)
      navigate_back();
      break;
    case 3: // No → Settings (cursor on source setting)
      navigate_back();
      break;
    case 4: // Yes → perform action, go home
      go_home();
      if (_confirm_source_setting == SETTING_CLEAR_DATA) {
        result.action = UIAction::ClearData;
      } else if (_confirm_source_setting == SETTING_CO2_CALIBRATION) {
        result.action = UIAction::CalibrateCo2;
      } else if (_confirm_source_setting == SETTING_FG_LEARNING) {
        // Orchestrator writes factory state and reboots into FG learning.
        result.action = UIAction::ArmFgLearning;
      }
      break;
    case 2: // Question label — non-selectable, do nothing
    default:
      break;
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_tag_list(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_tag_list(-1);
    break;
  case InputSource::TouchDown:
    move_tag_list(1);
    break;
  case InputSource::TouchEnter:
    if (_tag_list_index == 0) {
      // Exit → Home
      go_home();
    } else if (_tag_list_index == 1) {
      // Back → MainMenu
      navigate_back();
    } else {
      // Select tag
      uint8_t tag_idx = (uint8_t)(_tag_list_index - 2);
      if (tag_idx < TAG_COUNT) {
        go_home();
        result.action = UIAction::SaveTag;
        result.tag_index = tag_idx;
        result.tag_label = TAG_LABELS[tag_idx];
      }
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_provisioning(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
  case InputSource::TouchDown:
    move_provisioning(1); // 2 rows — direction is irrelevant
    break;
  case InputSource::TouchEnter:
    // Row 0 = switch transport, row 1 = cancel setup.  Open the
    // confirmation overlay; the orchestrator routes Yes via the new
    // UIAction values returned from dispatch_provisioning_confirm.
    open_provisioning_confirm(_provisioning_row_index);
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_provisioning_confirm(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
  case InputSource::TouchDown:
    move_provisioning_confirm(1); // 2-button toggle
    break;
  case InputSource::TouchEnter:
    if (_provisioning_confirm_index == 0) {
      // No → back to Provisioning page, no action.
      _screen = Screen::Provisioning;
    } else {
      // Yes → emit the kind-specific action.  The orchestrator routes
      // ConfirmSwitchProvisioningTransport (kind=0) back into the page
      // and routes ConfirmCancelProvisioning (kind=1) to change_mode.
      if (_provisioning_confirm_kind == 0) {
        result.action = UIAction::ConfirmSwitchProvisioningTransport;
        _screen = Screen::Provisioning;
      } else {
        result.action = UIAction::ConfirmCancelProvisioning;
        // Stay on the confirm screen visually — the orchestrator will
        // tear the session down and the leave-render will repaint Home /
        // Portable on its own.
      }
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_getting_started(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  // Single action row — only TouchEnter acts.
  if (source == InputSource::TouchEnter) {
    if (_getting_started_from_boot) {
      // Orchestrator marks the flag and leaves to Home; keep _screen here.
      result.action = UIAction::AckOnboarding;
    } else {
      // Back → Settings, cursor on Setup Guide (flag unchanged).
      navigate_back();
    }
  }
  return result;
}

UIActionResult UIManager::dispatch_hardware_test(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (source) {
  case InputSource::TouchUp:
    move_hardware_test(-1);
    break;
  case InputSource::TouchDown:
    move_hardware_test(1);
    break;
  case InputSource::TouchEnter:
    if (_hardware_test_index == 0) {
      // Exit → Home
      go_home();
    } else if (_hardware_test_index == 1) {
      // Back → Settings (cursor on "Hardware Test")
      navigate_back();
    } else if (_hardware_test_index == HW_TEST_PERIPHERAL) {
      // Peripheral Test → start the guided flow (orchestrator drives hardware).
      open_peripheral_test();
      result.action = UIAction::RunPeripheralTest;
    } else if (_hardware_test_index == HW_TEST_GPS) {
      // GPS Test → open the live screen (orchestrator starts receiver + TTFF).
      open_gps_test();
      result.action = UIAction::OpenGpsTest;
    } else if (_hardware_test_index == HW_TEST_ACCEL) {
      // Accel Test → open the live screen (orchestrator polls + classifies).
      open_accel_test();
      result.action = UIAction::OpenAccelTest;
    } else if (_hardware_test_index == HW_TEST_FG_LEARNING) {
      // FG Learning → strong confirm dialog (Back/No return here).
      open_confirm(SETTING_FG_LEARNING);
    }
    break;
  default:
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_peripheral_test(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  switch (_peripheral_view.kind) {
  case PeripheralTestView::Kind::Actuator:
    switch (source) {
    case InputSource::TouchUp:
    case InputSource::TouchDown:
      _peripheral_index = (uint8_t)(_peripheral_index == 0 ? 1 : 0); // toggle Pass/Fail
      break;
    case InputSource::TouchEnter:
      result.action =
          (_peripheral_index == 0) ? UIAction::PeripheralStepPass : UIAction::PeripheralStepFail;
      break;
    default:
      break;
    }
    break;
  case PeripheralTestView::Kind::Testing:
    // Automatic phase — ignore input.
    break;
  case PeripheralTestView::Kind::Summary:
    if (source == InputSource::TouchEnter) {
      navigate_back(); // back to Hardware Test submenu
      result.action = UIAction::PeripheralTestExit;
    }
    break;
  }
  return result;
}

UIActionResult UIManager::dispatch_gps_test(InputSource source, InputType type) {
  (void)type;
  // Live screen: nothing to browse, so any touch exits. The orchestrator
  // detects the screen leaving GpsTest and restores GPS state (no exit action).
  if (source == InputSource::TouchUp || source == InputSource::TouchDown ||
      source == InputSource::TouchEnter) {
    navigate_back(); // back to Hardware Test submenu on the GPS row
  }
  return {};
}

UIActionResult UIManager::dispatch_accel_test(InputSource source, InputType type) {
  (void)type;
  // Live screen: nothing to browse, so any touch exits. The orchestrator
  // detects the screen leaving AccelTest and restores hardware (no exit action).
  if (source == InputSource::TouchUp || source == InputSource::TouchDown ||
      source == InputSource::TouchEnter) {
    navigate_back(); // back to Hardware Test submenu on the Accel row
  }
  return {};
}

UIActionResult UIManager::dispatch_pair_watch(InputSource source, InputType type) {
  (void)type;
  UIActionResult result{};

  if (_watch_pairing_view == WatchPairingView::Waiting) {
    if (source == InputSource::TouchEnter) {
      result.action = UIAction::PairWatchCancelled;
    }
    return result;
  }

  if (_watch_pairing_view == WatchPairingView::NumericComparison) {
    if (source == InputSource::TouchUp) {
      move_watch_pairing_action(-1);
    } else if (source == InputSource::TouchDown) {
      move_watch_pairing_action(1);
    } else if (source == InputSource::TouchEnter) {
      result.action = _watch_pairing_action_index == 0 ? UIAction::PairWatchCancelled
                                                       : UIAction::PairWatchConfirmed;
    }
  }
  return result;
}

void UIManager::move_provisioning(int delta) {
  // Two rows: 0 = switch transport, 1 = cancel setup.
  _provisioning_row_index =
      static_cast<uint8_t>(wrap(static_cast<int>(_provisioning_row_index) + delta, 2));
}

void UIManager::move_provisioning_confirm(int delta) {
  // Two buttons: 0 = No (default), 1 = Yes.
  _provisioning_confirm_index =
      static_cast<uint8_t>(wrap(static_cast<int>(_provisioning_confirm_index) + delta, 2));
}

void UIManager::move_watch_pairing_action(int delta) {
  _watch_pairing_action_index =
      static_cast<uint8_t>(wrap(static_cast<int>(_watch_pairing_action_index) + delta, 2));
}

// ---------------------------------------------------------------------------
// Row population
// ---------------------------------------------------------------------------

void UIManager::populate_menu_rows(DisplayValues &v) const {
  v.row_count = MAIN_MENU_TOTAL;
  copy_row(v, 0, "Exit Menu", false);
  copy_row(v, 1, _tracking_active ? "Stop Tracking" : "Start Tracking", false);
  copy_row(v, 2, "Settings", false);
  copy_row(v, 3, "About Device", false);
  v.selected_row = _menu_index;
}

void UIManager::populate_settings_rows(DisplayValues &v) const {
  // Fixed header rows
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);
  v.show_separator_after_back = true;

  // Compute page-based scroll
  uint8_t scroll = page_scroll(_settings_index);

  // Content items: indices 2..10 (9 items total)
  const uint8_t content_count = SETTINGS_TOTAL - 2;
  uint8_t visible = 0;

  for (uint8_t i = 0; i < PAGE_SIZE && (scroll + i) < content_count; ++i) {
    const uint8_t item_index = static_cast<uint8_t>(SETTING_SETUP_GUIDE + scroll + i);
    char label[48];

    switch (item_index) {
    case SETTING_SETUP_GUIDE:
      (void)snprintf(label, sizeof(label), "Setup Guide");
      break;
    case SETTING_UNITS:
      (void)snprintf(label, sizeof(label), "Units: %s", UNITS_OPTIONS[_setting_units]);
      break;
    case SETTING_PM_DISPLAY:
      (void)snprintf(label, sizeof(label), "PM Display: %s",
                     PM_DISPLAY_OPTIONS[_setting_pm_display]);
      break;
    case SETTING_MEASURE_INTERVAL:
      (void)snprintf(label, sizeof(label), "Measure Int.: %s",
                     MEASURE_INTERVAL_OPTIONS[_setting_measure_interval]);
      break;
    case SETTING_GPS_MODE:
      (void)snprintf(label, sizeof(label), "GPS Mode: %s", GPS_MODE_OPTIONS[_setting_gps_mode]);
      break;
    case SETTING_MODE:
      (void)snprintf(label, sizeof(label), "Mode: %s", MODE_OPTIONS[_setting_mode]);
      break;
    case SETTING_AUTO_LOCK:
      (void)snprintf(label, sizeof(label), "Auto Lock: %s", AUTO_LOCK_OPTIONS[_setting_auto_lock]);
      break;
    case SETTING_DISPLAY_LED:
      (void)snprintf(label, sizeof(label), "Display LED: %s",
                     LED_BRIGHTNESS_OPTIONS[_setting_display_led]);
      break;
    case SETTING_AQI_LED:
      (void)snprintf(label, sizeof(label), "AQI LED: %s", LED_BRIGHTNESS_OPTIONS[_setting_aqi_led]);
      break;
    case SETTING_TOUCH_LED:
      (void)snprintf(label, sizeof(label), "Touch LED: %s", TOUCH_LED_OPTIONS[_setting_touch_led]);
      break;
    case SETTING_BUZZER:
      (void)snprintf(label, sizeof(label), "Buzzer: %s", BUZZER_OPTIONS[_setting_buzzer_volume]);
      break;
    case SETTING_PLAY_MELODY:
      (void)snprintf(label, sizeof(label), "Play Melody");
      break;
    case SETTING_CO2_CALIBRATION:
      (void)snprintf(label, sizeof(label), "CO2: Calibrate");
      break;
    case SETTING_CLEAR_DATA:
      (void)snprintf(label, sizeof(label), "Data: Clear Data");
      break;
    case SETTING_HARDWARE_TEST:
      (void)snprintf(label, sizeof(label), "Hardware Test");
      break;
    case SETTING_PAIR_WATCH:
      (void)snprintf(label, sizeof(label), "Pair Watch");
      break;
    default:
      label[0] = '\0';
      break;
    }

    copy_row(v, (uint8_t)(2 + visible), label, false);
    ++visible;
  }

  v.row_count = (uint8_t)(2 + visible);
  v.selected_row = display_row(_settings_index, scroll);
}

void UIManager::populate_settings_choice_rows(DisplayValues &v) const {
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);
  v.show_separator_after_back = true;

  uint8_t option_count = setting_option_count(_editing_setting_id);
  const char *const *options = nullptr;

  switch (_editing_setting_id) {
  case SETTING_UNITS:
    options = UNITS_OPTIONS;
    break;
  case SETTING_PM_DISPLAY:
    options = PM_DISPLAY_OPTIONS;
    break;
  case SETTING_MEASURE_INTERVAL:
    options = MEASURE_INTERVAL_OPTIONS;
    break;
  case SETTING_GPS_MODE:
    options = GPS_MODE_OPTIONS;
    break;
  case SETTING_MODE:
    options = MODE_OPTIONS;
    break;
  case SETTING_AUTO_LOCK:
    options = AUTO_LOCK_OPTIONS;
    break;
  case SETTING_DISPLAY_LED:
  case SETTING_AQI_LED:
    options = LED_BRIGHTNESS_OPTIONS;
    break;
  case SETTING_TOUCH_LED:
    options = TOUCH_LED_OPTIONS;
    break;
  case SETTING_BUZZER:
    options = BUZZER_OPTIONS;
    break;
  case SETTING_PLAY_MELODY:
    options = MELODY_OPTIONS;
    break;
  default:
    break;
  }

  uint8_t visible = 0;
  if (options != nullptr) {
    for (uint8_t i = 0; i < PAGE_SIZE && (_settings_choice_scroll_start + i) < option_count; ++i) {
      copy_row(v, (uint8_t)(2 + visible), options[_settings_choice_scroll_start + i], false);
      ++visible;
    }
  }

  v.row_count = (uint8_t)(2 + visible);
  v.selected_row = display_row(_settings_choice_index, _settings_choice_scroll_start);
}

void UIManager::populate_about_rows(DisplayValues &v) const {
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);
  v.row_count = 2;
  v.selected_row = _about_index;
  v.show_separator_after_back = true;

  v.about_title = "AirGradient Go";
  v.about_firmware = _about_firmware;
  v.about_serial = _about_serial;
  v.about_hardware = "Open Source Hardware";
}

void UIManager::populate_confirm_rows(DisplayValues &v) const {
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);

  const char *question = "Confirm?";
  if (_confirm_source_setting == SETTING_CLEAR_DATA) {
    question = "Clear Data?";
  } else if (_confirm_source_setting == SETTING_CO2_CALIBRATION) {
    question = "Calibrate CO2?";
  } else if (_confirm_source_setting == SETTING_FG_LEARNING) {
    question = "Start FG Learning?";
  }
  copy_row(v, 2, question, true); // non-selectable

  copy_row(v, 3, "No", false);
  copy_row(v, 4, "Yes", false);
  v.row_count = 5;
  v.selected_row = _confirm_index;
  v.show_separator_after_back = true;
}

void UIManager::populate_tag_list_rows(DisplayValues &v) const {
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);
  v.show_separator_after_back = true;

  // Compute page-based scroll
  uint8_t scroll = page_scroll(_tag_list_index);

  uint8_t visible = 0;
  for (uint8_t i = 0; i < PAGE_SIZE && (scroll + i) < TAG_COUNT; ++i) {
    copy_row(v, (uint8_t)(2 + visible), TAG_LABELS[scroll + i], false);
    ++visible;
  }

  v.row_count = (uint8_t)(2 + visible);
  v.selected_row = display_row(_tag_list_index, scroll);
}

void UIManager::populate_provisioning_rows(DisplayValues &v) const {
  // Two action rows.  Labels depend on the active transport — row 0 is
  // always the switch-transport button, row 1 is always cancel-setup.
  const bool ble_active = _provisioning_transport == ProvisioningTransport::BleOnly;
  copy_row(v, 0, ble_active ? "Use portal" : "Use app", false);
  copy_row(v, 1, "Cancel setup", false);
  v.row_count = 2;
  v.selected_row = _provisioning_row_index;
}

void UIManager::populate_provisioning_confirm_rows(DisplayValues &v) const {
  // The renderer (DisplayService::_draw_provisioning_confirm) reads the
  // question from v.rows[0].text and draws the No/Yes buttons itself.
  // Question text depends on kind + active transport (see spec).
  const char *question = "Confirm?";
  if (_provisioning_confirm_kind == 0) {
    // Switch transport — name the target transport, not the source.
    question = (_provisioning_transport == ProvisioningTransport::BleOnly)
                   ? "Switch to Wi-Fi setup?"
                   : "Switch to app setup?";
  } else {
    question = "Cancel setup?";
  }
  copy_row(v, 0, question, true); // non-selectable; renderer uses text only
  v.row_count = 1;
  v.selected_row = _provisioning_confirm_index; // mirror for any future reuse
}

void UIManager::populate_getting_started_rows(DisplayValues &v) const {
  // Single action row; label depends on entry source.
  copy_row(v, 0, _getting_started_from_boot ? "Start using" : "Back", false);
  v.row_count = 1;
  v.selected_row = 0;
}

void UIManager::populate_hardware_test_rows(DisplayValues &v) const {
  copy_row(v, 0, "Exit", false);
  copy_row(v, 1, "Back", false);
  v.show_separator_after_back = true;
  copy_row(v, HW_TEST_PERIPHERAL, "Peripheral Test", false);
  copy_row(v, HW_TEST_GPS, "GPS Test", false);
  copy_row(v, HW_TEST_ACCEL, "Accel Test", false);
  copy_row(v, HW_TEST_FG_LEARNING, "FG Learning", false);
  v.row_count = HW_TEST_TOTAL;
  v.selected_row = _hardware_test_index;
}

void UIManager::populate_peripheral_test_rows(DisplayValues &v) const {
  switch (_peripheral_view.kind) {
  case PeripheralTestView::Kind::Actuator: {
    const char *prompt = _peripheral_view.prompt ? _peripheral_view.prompt : "Working?";
    copy_row(v, 0, prompt, true); // non-selectable header
    copy_row(v, 1, "Pass", false);
    copy_row(v, 2, "Fail", false);
    v.row_count = 3;
    v.selected_row = (uint8_t)(1 + (_peripheral_index != 0 ? 1 : 0));
    break;
  }
  case PeripheralTestView::Kind::Testing:
    copy_row(v, 0, "Testing sensors...", true);
    copy_row(v, 1, "Please wait", true);
    v.row_count = 2;
    v.selected_row = 0; // disabled rows never highlight
    break;
  case PeripheralTestView::Kind::Summary: {
    const auto &pv = _peripheral_view;
    char label[48];
    (void)snprintf(label, sizeof(label), "%s - tap to exit", pv.overall ? "PASS" : "FAIL");
    copy_row(v, 0, label, true);

    auto row = [&](uint8_t i, const char *name, bool pass) {
      char buf[48];
      (void)snprintf(buf, sizeof(buf), "%s: %s", name, pass ? "PASS" : "FAIL");
      copy_row(v, i, buf, false);
    };
    row(1, "Front LED", pv.front_led);
    row(2, "Back LED", pv.back_led);
    row(3, "Touch LED", pv.touch_led);
    row(4, "Buzzer", pv.buzzer);
    row(5, "Temp/Hum", pv.temp_hum);
    row(6, "CO2", pv.co2);
    row(7, "PM", pv.pm);
    row(8, "TVOC/NOx", pv.tvoc_nox);
    row(9, "Pressure", pv.pressure);
    v.row_count = 10;
    v.selected_row = 0; // header disabled; any tap exits
    break;
  }
  }
}

void UIManager::populate_gps_test_rows(DisplayValues &v, const BuildContext &ctx) const {
  copy_row(v, 0, "GPS Test - tap to exit", true); // non-selectable header

  char buf[48];
  const GpsData *gps = ctx.gps_data;

  // TTFF mm:ss — running until the first fix latches, then frozen. A trailing
  // "..." marks the still-counting state.
  const unsigned mm = static_cast<unsigned>(ctx.gps_ttff_secs / 60);
  const unsigned ss = static_cast<unsigned>(ctx.gps_ttff_secs % 60);
  (void)snprintf(buf, sizeof(buf), "TTFF: %02u:%02u%s", mm, ss, ctx.gps_ttff_fixed ? "" : " ...");
  copy_row(v, 1, buf, false);

  const char *fix_str = "NoFix";
  if (gps != nullptr) {
    switch (gps->fix.fix_type) {
    case GpsFixType::Fix2D:
      fix_str = "2D";
      break;
    case GpsFixType::Fix3D:
      fix_str = "3D";
      break;
    default:
      fix_str = "NoFix";
      break;
    }
  }
  (void)snprintf(buf, sizeof(buf), "Fix: %s", fix_str);
  copy_row(v, 2, buf, false);

  if (gps != nullptr && is_satellite_count_valid(gps->fix.satellite_count)) {
    (void)snprintf(buf, sizeof(buf), "Sats: %d", gps->fix.satellite_count);
  } else {
    (void)snprintf(buf, sizeof(buf), "Sats: --");
  }
  copy_row(v, 3, buf, false);

  if (gps != nullptr && gps->fix.hdop > 0.0f) {
    (void)snprintf(buf, sizeof(buf), "HDOP: %.1f", static_cast<double>(gps->fix.hdop));
  } else {
    (void)snprintf(buf, sizeof(buf), "HDOP: --");
  }
  copy_row(v, 4, buf, false);

  if (gps != nullptr && is_latitude_valid(gps->position.latitude)) {
    (void)snprintf(buf, sizeof(buf), "Lat: %.5f", gps->position.latitude);
  } else {
    (void)snprintf(buf, sizeof(buf), "Lat: --");
  }
  copy_row(v, 5, buf, false);

  if (gps != nullptr && is_longitude_valid(gps->position.longitude)) {
    (void)snprintf(buf, sizeof(buf), "Lon: %.5f", gps->position.longitude);
  } else {
    (void)snprintf(buf, sizeof(buf), "Lon: --");
  }
  copy_row(v, 6, buf, false);

  if (gps != nullptr && is_gps_timestamp_valid(gps->timestamp)) {
    (void)snprintf(buf, sizeof(buf), "UTC: %02d:%02d:%02d", gps->timestamp.hour,
                   gps->timestamp.minute, gps->timestamp.second);
  } else {
    (void)snprintf(buf, sizeof(buf), "UTC: No time");
  }
  copy_row(v, 7, buf, false);

  v.row_count = 8;
  v.selected_row = 0; // header disabled; any tap exits
}

void UIManager::populate_accel_test_rows(DisplayValues &v, const BuildContext &ctx) const {
  copy_row(v, 0, "Accel Test - tap to exit", true); // non-selectable header

  char buf[48];

  (void)snprintf(buf, sizeof(buf), "WHO_AM_I: 0x%02X (%s)", ctx.accel_who_am_i,
                 ctx.accel_id_ok ? "OK" : "BAD");
  copy_row(v, 1, buf, false);

  if (ctx.accel_read_ok) {
    (void)snprintf(buf, sizeof(buf), "X: %d mg", ctx.accel_x_mg);
    copy_row(v, 2, buf, false);
    (void)snprintf(buf, sizeof(buf), "Y: %d mg", ctx.accel_y_mg);
    copy_row(v, 3, buf, false);
    (void)snprintf(buf, sizeof(buf), "Z: %d mg", ctx.accel_z_mg);
    copy_row(v, 4, buf, false);
    (void)snprintf(buf, sizeof(buf), "|a|: %u mg", ctx.accel_magnitude_mg);
    copy_row(v, 5, buf, false);
  } else {
    copy_row(v, 2, "X: --", false);
    copy_row(v, 3, "Y: --", false);
    copy_row(v, 4, "Z: --", false);
    copy_row(v, 5, "|a|: --", false);
  }

  (void)snprintf(buf, sizeof(buf), "Result: %s", ctx.accel_pass ? "PASS" : "FAIL");
  copy_row(v, 6, buf, false);

  v.row_count = 7;
  v.selected_row = 0; // header disabled; any tap exits
}

// ---------------------------------------------------------------------------
// Chart extraction
// ---------------------------------------------------------------------------

void UIManager::populate_chart(DisplayValues &v, const MeasuresAGo *cache,
                               uint8_t cache_count) const {
  if (_active_metric == Metric::None || cache == nullptr || cache_count == 0) {
    v.chart_samples = nullptr;
    v.chart_count = 0;
    return;
  }

  uint8_t valid_count = 0;
  float min_val = 0.0f;
  float max_val = 0.0f;
  bool first = true;

  for (uint8_t i = 0; i < cache_count && i < UI_CHART_BUF_SIZE; ++i) {
    float sample = 0.0f;
    bool valid = false;

    switch (_active_metric) {
    case Metric::Pm25:
      if (cache[i].pm_a.pm_25 != MeasuresInvalid::PM) {
        sample = cache[i].pm_a.pm_25;
        valid = true;
      }
      break;
    case Metric::Co2:
      if (cache[i].co2.co2 != MeasuresInvalid::CO2) {
        sample = static_cast<float>(cache[i].co2.co2);
        valid = true;
      }
      break;
    case Metric::Temp:
      if (cache[i].temp_hum_a.temperature != MeasuresInvalid::TEMPERATURE) {
        sample = cache[i].temp_hum_a.temperature;
        valid = true;
      }
      break;
    case Metric::Humidity:
      if (cache[i].temp_hum_a.humidity != MeasuresInvalid::HUMIDITY) {
        sample = cache[i].temp_hum_a.humidity;
        valid = true;
      }
      break;
    case Metric::Tvoc:
      if (cache[i].tvoc_nox.tvoc_index != MeasuresInvalid::TVOC) {
        sample = static_cast<float>(cache[i].tvoc_nox.tvoc_index);
        valid = true;
      }
      break;
    case Metric::Nox:
      if (cache[i].tvoc_nox.nox_index != MeasuresInvalid::NOX) {
        sample = static_cast<float>(cache[i].tvoc_nox.nox_index);
        valid = true;
      }
      break;
    case Metric::None:
    default:
      break;
    }

    if (valid) {
      _chart_buf[valid_count] = sample;
      if (first) {
        min_val = sample;
        max_val = sample;
        first = false;
      } else {
        if (sample < min_val)
          min_val = sample;
        if (sample > max_val)
          max_val = sample;
      }
      ++valid_count;
    }
  }

  v.chart_samples = (valid_count > 0) ? _chart_buf : nullptr;
  v.chart_count = valid_count;
  v.chart_min = (valid_count > 0) ? min_val : 0.0f;
  v.chart_max = (valid_count > 0) ? max_val : 0.0f;
}
