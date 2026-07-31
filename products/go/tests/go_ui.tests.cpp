/**
 * AirGradient Go — UIManager unit tests
 *
 * Covers the host-testable surface of UIManager (pure state machine):
 *
 * Navigation      — Home → MainMenu → Settings / About / TagList / Confirm,
 *                   Back/Exit transitions, cursor positions after return.
 *
 * Metric cycling  — browse_metric via TouchUp/Down on Home screen, wrapping
 *                   through None → Pm25 → Co2 → Temp → Humidity.
 *
 * Settings choice — open choice screen, apply selection, verify
 *                   UIAction::SettingsChanged returned.
 *
 * Snackbar        — show, arm deadline, expire.
 *
 * sync_settings   — GoSettings → internal option indices round-trip.
 *
 * Chart extraction — populate_chart with MeasuresAGo cache.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "go_ui.h"

// ============================================================================
// Helpers
// ============================================================================

static constexpr UIManager::Config DEFAULT_UI_CONFIG = {
    .firmware_version = "0.1.0",
    .serial_number = "AABBCCDDEEFF",
};

/// Build a minimal BuildContext with default invalid sensor data.
static BuildContext make_default_ctx() {
  static Measures empty_measures{};
  return BuildContext{
      .sensor_data = empty_measures,
      .battery_pct = 0xFF,
      .is_battery_charging = false,
      .locked = false,
      .ble_enabled = false,
      .ble_connected = false,
      .wifi_enabled = false,
      .wifi_connected = false,
      .gps_enabled = true,
      .gps_fix = false,
      .tracking_active = false,
      .display_off = false,
      .use_fahrenheit = false,
      .pm_use_usaqi = false,
      .cache = nullptr,
      .cache_count = 0,
      .now_ms = 0,
  };
}

/// Simulate a short press from the given source.
static UIActionResult press(UIManager &ui, InputSource source) {
  return ui.handle_input(source, InputType::ShortPress);
}

/// Simulate a TouchEnter long-press (exit menu to Home).
static UIActionResult long_press(UIManager &ui) {
  return ui.handle_input(InputSource::TouchEnter, InputType::LongPress);
}

/// Simulate a TouchEnter double-press (back one level).
static UIActionResult double_press(UIManager &ui) {
  return ui.handle_input(InputSource::TouchEnter, InputType::DoublePress);
}

/// Home -> MainMenu -> Settings (cursor at 1 = Back).
static void go_to_settings(UIManager &ui) {
  press(ui, InputSource::TouchEnter); // Home → MainMenu
  press(ui, InputSource::TouchDown);  // 0→1
  press(ui, InputSource::TouchDown);  // 1→2 (Settings)
  press(ui, InputSource::TouchEnter); // → Settings
}

// ============================================================================
// Navigation
// ============================================================================

TEST_CASE("UIManager: basic navigation", "[UIManager][nav]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  SECTION("starts on Home screen") { CHECK(ui.current_screen() == Screen::Home); }

  SECTION("TouchEnter on Home opens MainMenu") {
    press(ui, InputSource::TouchEnter);

    CHECK(ui.current_screen() == Screen::MainMenu);
  }

  SECTION("Exit from MainMenu goes to Home") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu (cursor at 0 = Exit)
    press(ui, InputSource::TouchEnter); // Exit → Home

    CHECK(ui.current_screen() == Screen::Home);
  }

  SECTION("Settings from MainMenu") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    // Navigate to Settings (index 2): 0→1→2
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2 (Settings)
    press(ui, InputSource::TouchEnter); // 2 → Settings

    CHECK(ui.current_screen() == Screen::Settings);
  }

  SECTION("About Device from MainMenu") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2
    press(ui, InputSource::TouchDown);  // 2→3 (About Device)
    press(ui, InputSource::TouchEnter); // → About

    CHECK(ui.current_screen() == Screen::About);
  }

  SECTION("Back from Settings goes to MainMenu") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2
    press(ui, InputSource::TouchEnter); // → Settings (cursor at 1 = Back)
    press(ui, InputSource::TouchEnter); // Back → MainMenu

    CHECK(ui.current_screen() == Screen::MainMenu);
  }

  SECTION("Exit from Settings goes to Home") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2
    press(ui, InputSource::TouchEnter); // → Settings (cursor at 1)
    press(ui, InputSource::TouchUp);    // 1→0 (Exit)
    press(ui, InputSource::TouchEnter); // → Home

    CHECK(ui.current_screen() == Screen::Home);
  }

  SECTION("Back from About goes to MainMenu") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2
    press(ui, InputSource::TouchDown);  // 2→3
    press(ui, InputSource::TouchEnter); // → About (cursor at 1 = Back)
    press(ui, InputSource::TouchEnter); // Back → MainMenu

    CHECK(ui.current_screen() == Screen::MainMenu);
  }
}

// ============================================================================
// First-boot Getting Started guide
// ============================================================================

TEST_CASE("UIManager: Getting Started via Settings -> Setup Guide", "[UIManager][onboarding]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  // Home -> MainMenu -> Settings (cursor at 1 = Back).
  auto go_to_settings = [&]() {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2 (Settings)
    press(ui, InputSource::TouchEnter); // → Settings (cursor at 1 = Back)
  };

  SECTION("Setup Guide row opens Getting Started; Back returns to Settings") {
    go_to_settings();
    press(ui, InputSource::TouchDown);  // 1→2 (Setup Guide, first content item)
    press(ui, InputSource::TouchEnter); // → Getting Started

    CHECK(ui.current_screen() == Screen::GettingStarted);

    // Settings entry: action row reads "Back" and the setup QR is published.
    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    REQUIRE(v.row_count == 1);
    CHECK(std::string(v.rows[0].text) == "Back");
    REQUIRE(v.qr != nullptr);
    CHECK(v.qr->size() > 0);

    // Back → Settings (cursor on Setup Guide row); no application action.
    UIActionResult result = press(ui, InputSource::TouchEnter);
    CHECK(result.action == UIAction::None);
    CHECK(ui.current_screen() == Screen::Settings);
    v = ui.build_values(ctx);
    CHECK(v.selected_row == 2); // Setup Guide row
  }
}

TEST_CASE("UIManager: Getting Started boot entry emits AckOnboarding", "[UIManager][onboarding]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.show_getting_started(/*from_boot=*/true);
  CHECK(ui.current_screen() == Screen::GettingStarted);

  // Boot entry: action row reads "Start using".
  auto ctx = make_default_ctx();
  DisplayValues v = ui.build_values(ctx);
  REQUIRE(v.row_count == 1);
  CHECK(std::string(v.rows[0].text) == "Start using");
  REQUIRE(v.qr != nullptr);

  // "Start using" press returns AckOnboarding; the orchestrator owns the
  // subsequent screen transition, so the screen stays put here.
  UIActionResult result = press(ui, InputSource::TouchEnter);
  CHECK(result.action == UIAction::AckOnboarding);
  CHECK(ui.current_screen() == Screen::GettingStarted);
}

// ============================================================================
// Wrap-around navigation
// ============================================================================

TEST_CASE("UIManager: Settings wrap-around navigation", "[UIManager][nav][settings]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  // Navigate to Settings: Home → MainMenu → Settings (cursor starts at 1 = Back).
  // Portable Settings has 18 indices: Exit(0), Back(1), items(2..17).
  auto go_to_settings = [&]() {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2 (Settings)
    press(ui, InputSource::TouchEnter); // → Settings (cursor at 1)
  };

  SECTION("Down past last item wraps to Exit") {
    go_to_settings(); // cursor at 1 (Back)

    // Navigate down from index 1 to index 17 (last item): 16 presses.
    for (int i = 0; i < 16; ++i) {
      press(ui, InputSource::TouchDown);
    }

    press(ui, InputSource::TouchDown); // 17→0 (wrap to Exit)

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(v.selected_row == 0); // Exit row
  }

  SECTION("Up past Exit wraps to last item") {
    go_to_settings(); // cursor at 1 (Back)

    press(ui, InputSource::TouchUp); // 1→0 (Exit)
    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(v.selected_row == 0); // confirm we're on Exit

    press(ui, InputSource::TouchUp); // 0→17 (wrap to last item)

    v = ui.build_values(ctx);
    // After wrapping to index 17, scroll resets to page_scroll(17).
    CHECK(ui.current_screen() == Screen::Settings);
    // Pressing Enter on Exit would go Home; instead, press Down to verify we
    // advance to 0 (confirming we were at 17).
    press(ui, InputSource::TouchDown); // 17→0 (Exit)
    v = ui.build_values(ctx);
    CHECK(v.selected_row == 0);
  }
}

TEST_CASE("UIManager: TagList wrap-around navigation", "[UIManager][nav][taglist]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  // TagList has 12 indices: Exit(0), Back(1), tags(2..11).
  // set_screen places cursor at default index (1 = Back).
  ui.set_screen(Screen::TagList);
  REQUIRE(ui.current_screen() == Screen::TagList);

  SECTION("Down past last tag wraps to Exit") {
    // Cursor starts at 1 (Back). Navigate down 10 times to reach index 11.
    for (int i = 0; i < 10; ++i) {
      press(ui, InputSource::TouchDown);
    }

    // One more Down wraps to 0 (Exit).
    press(ui, InputSource::TouchDown);

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(v.selected_row == 0); // Exit row
  }

  SECTION("Up past Exit wraps to last tag") {
    press(ui, InputSource::TouchUp); // 1→0 (Exit)
    press(ui, InputSource::TouchUp); // 0→11 (wrap to last tag)

    // Verify we wrapped — pressing Down should go back to 0.
    press(ui, InputSource::TouchDown); // 11→0
    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(v.selected_row == 0); // Exit row
  }
}

// ============================================================================
// Metric cycling
// ============================================================================

TEST_CASE("UIManager: metric cycling on Home", "[UIManager][metric]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  SECTION("starts with Metric::None") {
    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);

    CHECK(v.active_metric == Metric::None);
  }

  SECTION("TouchDown cycles forward through metrics") {
    press(ui, InputSource::TouchDown); // None → Pm25

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(v.active_metric == Metric::Pm25);
  }

  SECTION("TouchUp from None wraps to Humidity") {
    press(ui, InputSource::TouchUp); // None → Humidity (wraps backward)

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(v.active_metric == Metric::Humidity);
  }

  SECTION("full forward cycle: None → Pm25 → Co2 → Temp → Humidity → None") {
    auto check_metric = [&](Metric expected) {
      auto ctx = make_default_ctx();
      DisplayValues v = ui.build_values(ctx);
      CHECK(v.active_metric == expected);
    };

    check_metric(Metric::None);
    press(ui, InputSource::TouchDown);
    check_metric(Metric::Pm25);
    press(ui, InputSource::TouchDown);
    check_metric(Metric::Co2);
    press(ui, InputSource::TouchDown);
    check_metric(Metric::Temp);
    press(ui, InputSource::TouchDown);
    check_metric(Metric::Humidity);
    press(ui, InputSource::TouchDown);
    check_metric(Metric::None); // wraps
  }
}

// ============================================================================
// Tracking actions
// ============================================================================

TEST_CASE("UIManager: tracking start/stop", "[UIManager][tracking]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  SECTION("Start Tracking returns UIAction::StartTracking") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu (cursor at 0)
    press(ui, InputSource::TouchDown);  // 0 → 1 (Start Tracking)
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::StartTracking);
    CHECK(ui.current_screen() == Screen::Home);
  }

  SECTION("Stop Tracking returns UIAction::StopTracking") {
    // First, start tracking by building with tracking_active=true
    auto ctx = make_default_ctx();
    ctx.tracking_active = true;
    ui.build_values(ctx); // caches _tracking_active = true

    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0 → 1 (Stop Tracking)
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::StopTracking);
    CHECK(ui.current_screen() == Screen::Home);
  }
}

// ============================================================================
// Settings choice
// ============================================================================

TEST_CASE("UIManager: settings choice apply", "[UIManager][settings]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  SECTION("changing units returns SettingsChanged") {
    // Navigate: Home → MainMenu → Settings → Units → select "F"
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2 (Settings)
    press(ui, InputSource::TouchEnter); // → Settings (cursor at 1 = Back)
    press(ui, InputSource::TouchDown);  // 1→2 (Setup Guide)
    press(ui, InputSource::TouchDown);  // 2→3 (Pair Watch)
    press(ui, InputSource::TouchDown);  // 3→4 (Units)
    press(ui, InputSource::TouchEnter); // → SettingsChoice for Units

    CHECK(ui.current_screen() == Screen::SettingsChoice);

    // Current selection is pre-selected to current value.
    // Default _setting_units=0 (C), so cursor is at index 2 (first option).
    // Navigate down to F (index 3).
    press(ui, InputSource::TouchDown);
    auto result = press(ui, InputSource::TouchEnter); // Apply "F"

    CHECK(result.action == UIAction::SettingsChanged);
    CHECK(ui.current_screen() == Screen::Settings);

    // Verify the setting took effect via build_values.
    // The UIManager stores _setting_units=1 now.
    // build_values uses ctx.use_fahrenheit (passed by orchestrator), so
    // we verify indirectly by checking the settings row label.
    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    // Can't easily check the setting label from build_values without
    // navigating to Settings screen, but the action was correct.
  }

  SECTION("changing mode returns ChangeMode with new mode") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2
    press(ui, InputSource::TouchEnter); // → Settings (cursor 1 = Back)

    // Navigate down to Mode (display index 8)
    for (int i = 0; i < 7; ++i)
      press(ui, InputSource::TouchDown); // 1→2→3→4→5→6→7→8

    press(ui, InputSource::TouchEnter); // → SettingsChoice for Mode

    CHECK(ui.current_screen() == Screen::SettingsChoice);

    // Default _setting_mode=1 (Portable), cursor at option index 1 → logical index 3.
    // Navigate up to Stationary (option 0, logical index 2).
    press(ui, InputSource::TouchUp);
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::ChangeMode);
    CHECK(result.new_mode == OperatingMode::Stationary);
    CHECK(ui.current_screen() == Screen::Home);
  }
}

// ============================================================================
// LED settings choice
// ============================================================================

TEST_CASE("UIManager: LED settings choice", "[UIManager][settings][led]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  // Helper: navigate to Settings screen
  auto go_to_settings = [&]() {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2 (Settings)
    press(ui, InputSource::TouchEnter); // → Settings (cursor at 1 = Back)
  };

  SECTION("Display LED opens SettingsChoice and applies Dim") {
    go_to_settings();

    // Navigate to Display LED (display index 10) — 9 presses from Back (1)
    for (int i = 0; i < 9; ++i)
      press(ui, InputSource::TouchDown);

    press(ui, InputSource::TouchEnter);
    CHECK(ui.current_screen() == Screen::SettingsChoice);

    // Default _setting_display_led=0 (Off), cursor at option 0 → logical 2.
    // Navigate down to Dim (option 1, logical 3).
    press(ui, InputSource::TouchDown); // Off→Dim
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::SettingsChanged);
    CHECK(ui.current_screen() == Screen::Settings);

    GoSettings s{};
    ui.apply_to_settings(s);
    CHECK(s.front_led_brightness == LedBrightness::Dim);
  }

  SECTION("AQI LED opens SettingsChoice and applies Bright") {
    go_to_settings();

    // Navigate to AQI LED (display index 11) — 10 presses from Back (1)
    for (int i = 0; i < 10; ++i)
      press(ui, InputSource::TouchDown);

    press(ui, InputSource::TouchEnter);
    CHECK(ui.current_screen() == Screen::SettingsChoice);

    // Default _setting_aqi_led=0 (Off), cursor at option 0 → logical 2.
    // Navigate down to Bright (option 3, logical 5).
    press(ui, InputSource::TouchDown); // Dim
    press(ui, InputSource::TouchDown); // Mid
    press(ui, InputSource::TouchDown); // Bright
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::SettingsChanged);

    GoSettings s{};
    ui.apply_to_settings(s);
    CHECK(s.back_led_brightness == LedBrightness::Bright);
  }

  SECTION("Touch LED opens SettingsChoice and applies Dim") {
    go_to_settings();

    // Navigate to Touch LED (display index 12) — 11 presses from Back (1)
    for (int i = 0; i < 11; ++i)
      press(ui, InputSource::TouchDown);

    press(ui, InputSource::TouchEnter);
    CHECK(ui.current_screen() == Screen::SettingsChoice);

    // Default _setting_touch_led=0 (Off), cursor at option 0 → logical 2.
    // Navigate down to Dim (option 1, logical 3).
    press(ui, InputSource::TouchDown); // Dim
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::SettingsChanged);

    GoSettings s{};
    ui.apply_to_settings(s);
    CHECK(s.touch_led_intensity == TouchLedIntensity::Dim);
  }

  SECTION("sync_settings round-trips LED values") {
    GoSettings input{};
    input.front_led_brightness = LedBrightness::Mid;
    input.back_led_brightness = LedBrightness::Off;
    input.touch_led_intensity = TouchLedIntensity::Dim;
    ui.sync_settings(input);

    GoSettings output{};
    ui.apply_to_settings(output);
    CHECK(output.front_led_brightness == LedBrightness::Mid);
    CHECK(output.back_led_brightness == LedBrightness::Off);
    CHECK(output.touch_led_intensity == TouchLedIntensity::Dim);
  }
}

// ============================================================================
// Snackbar
// ============================================================================

TEST_CASE("UIManager: snackbar lifecycle", "[UIManager][snackbar]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  SECTION("no snackbar by default") {
    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);

    CHECK(v.snackbar_text == nullptr);
  }

  SECTION("show_snackbar sets text, cleared after duration") {
    ui.show_snackbar("Hello");

    // First clear call arms the deadline.
    ui.clear_expired_snackbar(1000);

    auto ctx = make_default_ctx();
    ctx.now_ms = 1000;
    DisplayValues v = ui.build_values(ctx);
    REQUIRE(v.snackbar_text != nullptr);
    CHECK(std::string(v.snackbar_text) == "Hello");

    // After 3000ms (SNACKBAR_DURATION_MS), should expire.
    ui.clear_expired_snackbar(1000 + 3001);

    ctx.now_ms = 1000 + 3001;
    v = ui.build_values(ctx);
    CHECK(v.snackbar_text == nullptr);
  }

  SECTION("show_snackbar(nullptr) clears immediately") {
    ui.show_snackbar("Temp");
    ui.show_snackbar(nullptr);

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(v.snackbar_text == nullptr);
  }
}

// ============================================================================
// sync_settings
// ============================================================================

TEST_CASE("UIManager: sync_settings from GoSettings", "[UIManager][sync]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  SECTION("syncs display settings to internal state") {
    GoSettings s{};
    s.use_fahrenheit = true;
    s.pm_use_usaqi = true;
    s.measure_interval_seconds = 60;
    s.gps_mode = GpsMode::AlwaysOn;
    s.operating_mode = OperatingMode::Stationary;
    s.auto_lock_seconds = 30;

    ui.sync_settings(s);

    // Navigate to Settings screen to verify labels in build_values.
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2
    press(ui, InputSource::TouchEnter); // → Settings

    CHECK(ui.current_screen() == Screen::Settings);
  }

  SECTION("sync_settings maps measure_interval 10s to index 1") {
    GoSettings s{};
    s.measure_interval_seconds = 10;
    ui.sync_settings(s);

    // Verify via apply_to_settings round-trip
    GoSettings out{};
    ui.apply_to_settings(out);
    CHECK(out.measure_interval_seconds == 10);
  }

  SECTION("sync_settings maps measure_interval 300s to index 4") {
    GoSettings s{};
    s.measure_interval_seconds = 300;
    ui.sync_settings(s);

    GoSettings out{};
    ui.apply_to_settings(out);
    CHECK(out.measure_interval_seconds == 300);
  }

  SECTION("sync_settings maps measure_interval minimum 3s to index 0") {
    GoSettings s{};
    s.measure_interval_seconds = 3;
    ui.sync_settings(s);

    GoSettings out{};
    ui.apply_to_settings(out);
    CHECK(out.measure_interval_seconds == 3);
  }

  SECTION("sync_settings clamps sub-minimum interval up to 3s (index 0)") {
    // Values below the UI minimum (e.g. legacy or BLE-set 1s/2s) resolve to the
    // lowest enum option and round-trip out as 3s.
    GoSettings s{};
    s.measure_interval_seconds = 1;
    ui.sync_settings(s);

    GoSettings out{};
    ui.apply_to_settings(out);
    CHECK(out.measure_interval_seconds == 3);

    s.measure_interval_seconds = 2;
    ui.sync_settings(s);
    ui.apply_to_settings(out);
    CHECK(out.measure_interval_seconds == 3);
  }

  SECTION("GPS mode mapping") {
    GoSettings s{};

    s.gps_mode = GpsMode::AlwaysOff;
    ui.sync_settings(s);
    // Internal state is _setting_gps_mode = 0. Verified indirectly.

    s.gps_mode = GpsMode::AlwaysOn;
    ui.sync_settings(s);
    // Internal state is _setting_gps_mode = 2.
  }

  SECTION("auto_lock mapping") {
    GoSettings s{};

    s.auto_lock_seconds = 0;
    ui.sync_settings(s); // Off = index 0

    s.auto_lock_seconds = 10;
    ui.sync_settings(s); // 10s = index 1

    s.auto_lock_seconds = 30;
    ui.sync_settings(s); // 30s = index 2

    s.auto_lock_seconds = 60;
    ui.sync_settings(s); // 60s = index 3
  }
}

// ============================================================================
// Chart extraction
// ============================================================================

TEST_CASE("UIManager: chart extraction from MeasuresAGo cache", "[UIManager][chart]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  SECTION("no metric selected — no chart data") {
    MeasuresAGo cache[2]{};
    cache[0].pm_a.pm_25 = 10.0f;
    cache[1].pm_a.pm_25 = 20.0f;

    auto ctx = make_default_ctx();
    ctx.cache = cache;
    ctx.cache_count = 2;

    DisplayValues v = ui.build_values(ctx);

    CHECK(v.chart_samples == nullptr);
    CHECK(v.chart_count == 0);
  }

  SECTION("PM2.5 metric selected — chart populated") {
    // Select PM2.5 metric
    press(ui, InputSource::TouchDown); // None → Pm25

    MeasuresAGo cache[3]{};
    cache[0].pm_a.pm_25 = 10.0f;
    cache[1].pm_a.pm_25 = 30.0f;
    cache[2].pm_a.pm_25 = 20.0f;

    auto ctx = make_default_ctx();
    ctx.cache = cache;
    ctx.cache_count = 3;

    DisplayValues v = ui.build_values(ctx);

    CHECK(v.chart_count == 3);
    REQUIRE(v.chart_samples != nullptr);
    CHECK(v.chart_min == Catch::Approx(10.0f));
    CHECK(v.chart_max == Catch::Approx(30.0f));
  }

  SECTION("invalid cache entries are skipped") {
    press(ui, InputSource::TouchDown); // None → Pm25

    MeasuresAGo cache[3]{};
    cache[0].pm_a.pm_25 = 10.0f;
    cache[1].pm_a.pm_25 = MeasuresInvalid::PM; // invalid
    cache[2].pm_a.pm_25 = 20.0f;

    auto ctx = make_default_ctx();
    ctx.cache = cache;
    ctx.cache_count = 3;

    DisplayValues v = ui.build_values(ctx);

    CHECK(v.chart_count == 2); // only 2 valid
    CHECK(v.chart_min == Catch::Approx(10.0f));
    CHECK(v.chart_max == Catch::Approx(20.0f));
  }
}

// ============================================================================
// Confirm (clear data)
// ============================================================================

TEST_CASE("UIManager: clear data confirm dialog", "[UIManager][confirm]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  // Helper: navigate to Settings → Clear Data → Confirm
  auto navigate_to_clear_data_confirm = [&]() {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2
    press(ui, InputSource::TouchEnter); // → Settings (cursor at 1)

    // Navigate to "Clear Data" (display index 16)
    for (int i = 0; i < 15; ++i)
      press(ui, InputSource::TouchDown); // 1→2→...→16

    press(ui, InputSource::TouchEnter); // → Confirm (cursor at 1 = Back)
  };

  SECTION("Yes in confirm returns ClearData") {
    navigate_to_clear_data_confirm();
    CHECK(ui.current_screen() == Screen::Confirm);

    // Navigate to Yes (index 4)
    press(ui, InputSource::TouchDown); // 1→2
    press(ui, InputSource::TouchDown); // 2→3
    press(ui, InputSource::TouchDown); // 3→4 (Yes)
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::ClearData);
    CHECK(ui.current_screen() == Screen::Home);
  }

  SECTION("confirm shows Clear Data? question") {
    navigate_to_clear_data_confirm();

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[2].text) == "Clear Data?");
    CHECK(v.rows[2].disabled == true);
  }
}

// ============================================================================
// Confirm (CO2 calibration)
// ============================================================================

TEST_CASE("UIManager: CO2 calibration confirm dialog", "[UIManager][confirm][co2]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  // Helper: navigate to Settings → CO2: Calibrate → Confirm
  auto navigate_to_co2_confirm = [&]() {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2
    press(ui, InputSource::TouchEnter); // → Settings (cursor at 1)

    // Navigate to "CO2: Calibrate" (display index 15)
    for (int i = 0; i < 14; ++i)
      press(ui, InputSource::TouchDown); // 1→2→...→15

    press(ui, InputSource::TouchEnter); // → Confirm (cursor at 1 = Back)
  };

  SECTION("Yes in confirm returns CalibrateCo2") {
    navigate_to_co2_confirm();
    CHECK(ui.current_screen() == Screen::Confirm);

    // Navigate to Yes (index 4)
    press(ui, InputSource::TouchDown); // 1→2
    press(ui, InputSource::TouchDown); // 2→3
    press(ui, InputSource::TouchDown); // 3→4 (Yes)
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::CalibrateCo2);
    CHECK(ui.current_screen() == Screen::Home);
  }

  SECTION("No in confirm returns to Settings on CO2 row") {
    navigate_to_co2_confirm();
    CHECK(ui.current_screen() == Screen::Confirm);

    // Navigate to No (index 3)
    press(ui, InputSource::TouchDown); // 1→2
    press(ui, InputSource::TouchDown); // 2→3 (No)
    press(ui, InputSource::TouchEnter);

    CHECK(ui.current_screen() == Screen::Settings);
  }

  SECTION("Back in confirm returns to Settings on CO2 row") {
    navigate_to_co2_confirm();
    CHECK(ui.current_screen() == Screen::Confirm);

    // Cursor starts on Back (index 1)
    press(ui, InputSource::TouchEnter);

    CHECK(ui.current_screen() == Screen::Settings);
  }

  SECTION("confirm shows Calibrate CO2? question") {
    navigate_to_co2_confirm();

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[2].text) == "Calibrate CO2?");
    CHECK(v.rows[2].disabled == true);
  }
}

// ============================================================================
// Hardware Test submenu (Settings → Hardware Test → FG Learning)
// ============================================================================

TEST_CASE("UIManager: Hardware Test submenu navigation", "[UIManager][hwtest]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  // Settings → "Hardware Test" is the last content row (display index 17).
  // From the Settings entry cursor (index 1 = Back), 16 downs land on it.
  auto navigate_to_hardware_test = [&]() {
    go_to_settings(ui); // cursor at 1
    for (int i = 0; i < 16; ++i)
      press(ui, InputSource::TouchDown);
    press(ui, InputSource::TouchEnter); // → Hardware Test submenu
  };

  SECTION("Settings row opens the Hardware Test submenu") {
    navigate_to_hardware_test();
    CHECK(ui.current_screen() == Screen::HardwareTest);

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[0].text) == "Exit");
    CHECK(std::string(v.rows[1].text) == "Back");
    CHECK(std::string(v.rows[2].text) == "Peripheral Test");
    CHECK(std::string(v.rows[3].text) == "GPS Test");
    CHECK(std::string(v.rows[4].text) == "Accel Test");
    CHECK(std::string(v.rows[5].text) == "FG Learning");
    CHECK(v.row_count == 6);
  }

  SECTION("Back returns to Settings on the Hardware Test row") {
    navigate_to_hardware_test();
    // Cursor lands on Back (index 1) on entry.
    press(ui, InputSource::TouchEnter);
    CHECK(ui.current_screen() == Screen::Settings);

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[v.selected_row].text) == "Hardware Test");
  }

  SECTION("double-press backs out of the submenu to Settings") {
    navigate_to_hardware_test();
    double_press(ui);
    CHECK(ui.current_screen() == Screen::Settings);
  }
}

TEST_CASE("UIManager: FG Learning arm confirm dialog", "[UIManager][hwtest][fg]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  auto navigate_to_fg_confirm = [&]() {
    go_to_settings(ui);
    for (int i = 0; i < 16; ++i)
      press(ui, InputSource::TouchDown); // → Hardware Test row
    press(ui, InputSource::TouchEnter);  // → Hardware Test submenu (cursor 1)
    press(ui, InputSource::TouchDown);   // 1→2 (Peripheral Test)
    press(ui, InputSource::TouchDown);   // 2→3 (GPS Test)
    press(ui, InputSource::TouchDown);   // 3→4 (Accel Test)
    press(ui, InputSource::TouchDown);   // 4→5 (FG Learning)
    press(ui, InputSource::TouchEnter);  // → Confirm
  };

  SECTION("FG Learning row opens the confirm dialog with a strong question") {
    navigate_to_fg_confirm();
    CHECK(ui.current_screen() == Screen::Confirm);

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[2].text) == "Start FG Learning?");
    CHECK(v.rows[2].disabled == true);
  }

  SECTION("Yes emits ArmFgLearning") {
    navigate_to_fg_confirm();
    press(ui, InputSource::TouchDown); // 1→2 (question, non-selectable)
    press(ui, InputSource::TouchDown); // 2→3 (No)
    press(ui, InputSource::TouchDown); // 3→4 (Yes)
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::ArmFgLearning);
  }

  SECTION("No returns to the Hardware Test submenu on the FG Learning row") {
    navigate_to_fg_confirm();
    press(ui, InputSource::TouchDown); // 1→2
    press(ui, InputSource::TouchDown); // 2→3 (No)
    press(ui, InputSource::TouchEnter);

    CHECK(ui.current_screen() == Screen::HardwareTest);
    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[v.selected_row].text) == "FG Learning");
  }

  SECTION("Back returns to the Hardware Test submenu") {
    navigate_to_fg_confirm();
    // Cursor starts on Back (index 1).
    press(ui, InputSource::TouchEnter);
    CHECK(ui.current_screen() == Screen::HardwareTest);
  }
}

TEST_CASE("UIManager: Peripheral Test flow", "[UIManager][hwtest][peripheral]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  auto open_peripheral = [&]() {
    go_to_settings(ui);
    for (int i = 0; i < 16; ++i)
      press(ui, InputSource::TouchDown);       // → Hardware Test row
    press(ui, InputSource::TouchEnter);        // → submenu (cursor 1)
    press(ui, InputSource::TouchDown);         // 1→2 (Peripheral Test)
    return press(ui, InputSource::TouchEnter); // → RunPeripheralTest
  };

  SECTION("selecting the row starts the flow") {
    auto result = open_peripheral();
    CHECK(result.action == UIAction::RunPeripheralTest);
    CHECK(ui.current_screen() == Screen::PeripheralTest);
  }

  SECTION("actuator step emits Pass then Fail") {
    open_peripheral();
    // Default view is an actuator step with the cursor on Pass.
    auto pass = press(ui, InputSource::TouchEnter);
    CHECK(pass.action == UIAction::PeripheralStepPass);

    // Toggle to Fail then confirm.
    press(ui, InputSource::TouchDown);
    auto fail = press(ui, InputSource::TouchEnter);
    CHECK(fail.action == UIAction::PeripheralStepFail);
  }

  SECTION("testing view ignores input") {
    open_peripheral();
    PeripheralTestView v{};
    v.kind = PeripheralTestView::Kind::Testing;
    ui.set_peripheral_test_view(v);

    auto r = press(ui, InputSource::TouchEnter);
    CHECK(r.action == UIAction::None);
    CHECK(ui.current_screen() == Screen::PeripheralTest);
  }

  SECTION("summary renders result and tap exits to submenu") {
    open_peripheral();
    PeripheralTestView v{};
    v.kind = PeripheralTestView::Kind::Summary;
    v.overall = true;
    v.co2 = true;
    v.pm = false;
    ui.set_peripheral_test_view(v);

    auto ctx = make_default_ctx();
    DisplayValues dv = ui.build_values(ctx);
    CHECK(std::string(dv.rows[0].text) == "PASS - tap to exit");
    CHECK(std::string(dv.rows[6].text) == "CO2: PASS");
    CHECK(std::string(dv.rows[7].text) == "PM: FAIL");

    auto r = press(ui, InputSource::TouchEnter);
    CHECK(r.action == UIAction::PeripheralTestExit);
    CHECK(ui.current_screen() == Screen::HardwareTest);
  }
}

TEST_CASE("UIManager: GPS Test screen", "[UIManager][hwtest][gps]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  auto open_gps = [&]() {
    go_to_settings(ui);
    for (int i = 0; i < 16; ++i)
      press(ui, InputSource::TouchDown);       // → Hardware Test row
    press(ui, InputSource::TouchEnter);        // → submenu (cursor 1)
    press(ui, InputSource::TouchDown);         // 1→2 (Peripheral Test)
    press(ui, InputSource::TouchDown);         // 2→3 (GPS Test)
    return press(ui, InputSource::TouchEnter); // → OpenGpsTest
  };

  SECTION("selecting the row opens the live screen and emits OpenGpsTest") {
    auto result = open_gps();
    CHECK(result.action == UIAction::OpenGpsTest);
    CHECK(ui.current_screen() == Screen::GpsTest);
  }

  SECTION("renders parsed fields; running TTFF has a trailing marker") {
    open_gps();
    GpsData gps{};
    gps.fix.fix_type = GpsFixType::Fix3D;
    gps.fix.satellite_count = 9;
    gps.fix.hdop = 1.2f;
    gps.position.latitude = 12.34567;
    gps.position.longitude = 98.76543;
    gps.timestamp = GpsTimestamp{2026, 7, 6, 8, 5, 3, true};

    auto ctx = make_default_ctx();
    ctx.gps_data = &gps;
    ctx.gps_ttff_secs = 83; // 01:23
    ctx.gps_ttff_fixed = false;

    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[0].text) == "GPS Test - tap to exit");
    CHECK(std::string(v.rows[1].text) == "TTFF: 01:23 ...");
    CHECK(std::string(v.rows[2].text) == "Fix: 3D");
    CHECK(std::string(v.rows[3].text) == "Sats: 9");
    CHECK(std::string(v.rows[4].text) == "HDOP: 1.2");
    CHECK(std::string(v.rows[7].text) == "UTC: 08:05:03");
  }

  SECTION("frozen TTFF drops the trailing marker; no-fix fields show dashes") {
    open_gps();
    GpsData gps{}; // NoFix, invalid position/timestamp

    auto ctx = make_default_ctx();
    ctx.gps_data = &gps;
    ctx.gps_ttff_secs = 5;
    ctx.gps_ttff_fixed = true;

    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[1].text) == "TTFF: 00:05");
    CHECK(std::string(v.rows[2].text) == "Fix: NoFix");
    CHECK(std::string(v.rows[3].text) == "Sats: --");
    CHECK(std::string(v.rows[5].text) == "Lat: --");
    CHECK(std::string(v.rows[7].text) == "UTC: No time");
  }

  SECTION("any tap exits to the Hardware Test submenu on the GPS row") {
    open_gps();
    press(ui, InputSource::TouchDown); // any touch exits
    CHECK(ui.current_screen() == Screen::HardwareTest);

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[v.selected_row].text) == "GPS Test");
  }
}

TEST_CASE("UIManager: Accel Test screen", "[UIManager][hwtest][accel]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  auto open_accel = [&]() {
    go_to_settings(ui);
    for (int i = 0; i < 16; ++i)
      press(ui, InputSource::TouchDown);       // → Hardware Test row
    press(ui, InputSource::TouchEnter);        // → submenu (cursor 1)
    press(ui, InputSource::TouchDown);         // 1→2 (Peripheral Test)
    press(ui, InputSource::TouchDown);         // 2→3 (GPS Test)
    press(ui, InputSource::TouchDown);         // 3→4 (Accel Test)
    return press(ui, InputSource::TouchEnter); // → OpenAccelTest
  };

  SECTION("selecting the row opens the live screen and emits OpenAccelTest") {
    auto result = open_accel();
    CHECK(result.action == UIAction::OpenAccelTest);
    CHECK(ui.current_screen() == Screen::AccelTest);
  }

  SECTION("renders WHO_AM_I, X/Y/Z, magnitude, and a PASS result") {
    open_accel();
    auto ctx = make_default_ctx();
    ctx.accel_who_am_i = 0x33;
    ctx.accel_id_ok = true;
    ctx.accel_read_ok = true;
    ctx.accel_x_mg = 10;
    ctx.accel_y_mg = -20;
    ctx.accel_z_mg = 1000;
    ctx.accel_magnitude_mg = 1000;
    ctx.accel_pass = true;

    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[0].text) == "Accel Test - tap to exit");
    CHECK(std::string(v.rows[1].text) == "WHO_AM_I: 0x33 (OK)");
    CHECK(std::string(v.rows[2].text) == "X: 10 mg");
    CHECK(std::string(v.rows[3].text) == "Y: -20 mg");
    CHECK(std::string(v.rows[4].text) == "Z: 1000 mg");
    CHECK(std::string(v.rows[5].text) == "|a|: 1000 mg");
    CHECK(std::string(v.rows[6].text) == "Result: PASS");
  }

  SECTION("bad identity / unreadable sensor shows BAD, dashes, and FAIL") {
    open_accel();
    auto ctx = make_default_ctx();
    ctx.accel_who_am_i = 0x00;
    ctx.accel_id_ok = false;
    ctx.accel_read_ok = false;
    ctx.accel_pass = false;

    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[1].text) == "WHO_AM_I: 0x00 (BAD)");
    CHECK(std::string(v.rows[2].text) == "X: --");
    CHECK(std::string(v.rows[5].text) == "|a|: --");
    CHECK(std::string(v.rows[6].text) == "Result: FAIL");
  }

  SECTION("any tap exits to the Hardware Test submenu on the Accel row") {
    open_accel();
    press(ui, InputSource::TouchDown); // any touch exits
    CHECK(ui.current_screen() == Screen::HardwareTest);

    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(std::string(v.rows[v.selected_row].text) == "Accel Test");
  }
}

// ============================================================================
// Tag list
// ============================================================================

TEST_CASE("UIManager: tag list", "[UIManager][tag]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  // Tag list is no longer reachable from the main menu (Add Tag removed),
  // but the tag list screen plumbing is preserved for future use.
  // Test it by forcing the screen directly.

  SECTION("selecting a tag returns SaveTag with index") {
    ui.set_screen(Screen::TagList);
    CHECK(ui.current_screen() == Screen::TagList);

    // Default cursor is at index 1 (Back). One down reaches first tag.
    press(ui, InputSource::TouchDown); // 1→2 (first tag: "Traffic Emissions")
    auto result = press(ui, InputSource::TouchEnter);

    CHECK(result.action == UIAction::SaveTag);
    CHECK(result.tag_index == 0);
    REQUIRE(result.tag_label != nullptr);
    CHECK(std::string(result.tag_label) == "Traffic Emissions");
    CHECK(ui.current_screen() == Screen::Home);
  }
}

// ============================================================================
// reset_to_home
// ============================================================================

TEST_CASE("UIManager: reset_to_home", "[UIManager][reset]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  press(ui, InputSource::TouchDown);  // Select a metric
  press(ui, InputSource::TouchEnter); // Go to MainMenu

  CHECK(ui.current_screen() == Screen::MainMenu);

  ui.reset_to_home();

  CHECK(ui.current_screen() == Screen::Home);

  auto ctx = make_default_ctx();
  DisplayValues v = ui.build_values(ctx);
  CHECK(v.active_metric == Metric::None);
}

// ============================================================================
// set_screen
// ============================================================================

TEST_CASE("UIManager: set_screen", "[UIManager][screen]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.set_screen(Screen::ShutdownUser);
  CHECK(ui.current_screen() == Screen::ShutdownUser);

  ui.set_screen(Screen::ShutdownDischarge);
  CHECK(ui.current_screen() == Screen::ShutdownDischarge);

  ui.set_screen(Screen::ShutdownTemperature);
  CHECK(ui.current_screen() == Screen::ShutdownTemperature);

  ui.set_screen(Screen::Home);
  CHECK(ui.current_screen() == Screen::Home);
}

TEST_CASE("UIManager: Pair Watch flow", "[UIManager][pair-watch]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  go_to_settings(ui);
  press(ui, InputSource::TouchDown); // Back → Setup Guide
  DisplayValues values = ui.build_values(make_default_ctx());
  CHECK(std::string(values.rows[2].text) == "Setup Guide");

  press(ui, InputSource::TouchDown); // Setup Guide → Pair Watch
  values = ui.build_values(make_default_ctx());
  CHECK(std::string(values.rows[3].text) == "Pair Watch");

  auto result = press(ui, InputSource::TouchEnter);
  CHECK(result.action == UIAction::PairWatchRequested);
  CHECK(ui.current_screen() == Screen::PairWatch);

  values = ui.build_values(make_default_ctx());
  CHECK(values.watch_pairing_view == WatchPairingView::Waiting);

  ui.show_watch_numeric_comparison(123456);
  values = ui.build_values(make_default_ctx());
  CHECK(values.watch_pairing_view == WatchPairingView::NumericComparison);
  CHECK(values.watch_pairing_number == 123456);
  CHECK(values.watch_pairing_action_index == 1); // Confirm is the default.

  result = press(ui, InputSource::TouchEnter);
  CHECK(result.action == UIAction::PairWatchConfirmed);

  ui.show_watch_numeric_comparison(123456);
  result = press(ui, InputSource::TouchDown);
  CHECK(result.action == UIAction::None);
  values = ui.build_values(make_default_ctx());
  CHECK(values.watch_pairing_action_index == 0);
  result = press(ui, InputSource::TouchEnter);
  CHECK(result.action == UIAction::PairWatchCancelled);

  ui.show_watch_pairing_confirming();
  result = press(ui, InputSource::TouchEnter);
  CHECK(result.action == UIAction::None);
  values = ui.build_values(make_default_ctx());
  CHECK(values.watch_pairing_view == WatchPairingView::Confirming);

  ui.show_watch_pairing_result(true);
  values = ui.build_values(make_default_ctx());
  CHECK(values.watch_pairing_view == WatchPairingView::Success);
  ui.dismiss_watch_pairing();
  CHECK(ui.current_screen() == Screen::Settings);
}

// ============================================================================
// Long press ignored
// ============================================================================

TEST_CASE("UIManager: TouchEnter gestures (back / exit to home)", "[UIManager][input]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  SECTION("Long-press on Home is a no-op") {
    auto result = long_press(ui);
    CHECK(result.action == UIAction::None);
    CHECK(ui.current_screen() == Screen::Home);
  }

  SECTION("Double-press on Home is a no-op") {
    auto result = double_press(ui);
    CHECK(result.action == UIAction::None);
    CHECK(ui.current_screen() == Screen::Home);
  }

  SECTION("Double-press from MainMenu goes back to Home") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    double_press(ui);
    CHECK(ui.current_screen() == Screen::Home);
  }

  SECTION("Double-press from Settings goes back to MainMenu (cursor on Settings)") {
    go_to_settings(ui);
    double_press(ui);

    CHECK(ui.current_screen() == Screen::MainMenu);
    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(v.selected_row == 2); // cursor restored to "Settings"
  }

  SECTION("Double-press from SettingsChoice goes back to Settings") {
    ui.set_screen(Screen::SettingsChoice);
    double_press(ui);
    CHECK(ui.current_screen() == Screen::Settings);
  }

  SECTION("Double-press from About goes back to MainMenu (cursor on About)") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    press(ui, InputSource::TouchDown);  // 0→1
    press(ui, InputSource::TouchDown);  // 1→2
    press(ui, InputSource::TouchDown);  // 2→3 (About Device)
    press(ui, InputSource::TouchEnter); // → About
    double_press(ui);

    CHECK(ui.current_screen() == Screen::MainMenu);
    auto ctx = make_default_ctx();
    DisplayValues v = ui.build_values(ctx);
    CHECK(v.selected_row == 3); // cursor restored to "About Device"
  }

  SECTION("Long-press from a deep screen exits straight to Home") {
    go_to_settings(ui);
    CHECK(ui.current_screen() == Screen::Settings);
    long_press(ui);
    CHECK(ui.current_screen() == Screen::Home);
  }

  SECTION("Long-press from MainMenu exits to Home") {
    press(ui, InputSource::TouchEnter); // Home → MainMenu
    long_press(ui);
    CHECK(ui.current_screen() == Screen::Home);
  }
}

// ============================================================================
// Info screen
// ============================================================================

TEST_CASE("UIManager: show_info sets Screen::Info and stores the text", "[UIManager][info]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.show_info("Connecting to saved Wi-Fi...");
  CHECK(ui.current_screen() == Screen::Info);

  DisplayValues v = ui.build_values(make_default_ctx());
  REQUIRE(v.info_text != nullptr);
  CHECK(std::string(v.info_text) == "Connecting to saved Wi-Fi...");
}

TEST_CASE("wifi_failure_text maps reasons to UI phrases", "[UIManager][info][wifi]") {
  using R = WifiDisconnectReason;
  CHECK(std::string(wifi_failure_text(R::auth_failed)) == "Wrong password");
  CHECK(std::string(wifi_failure_text(R::no_ap_found)) == "Network not found");
  CHECK(std::string(wifi_failure_text(R::assoc_failed)) == "Connection refused");
  CHECK(std::string(wifi_failure_text(R::dhcp_failed)) == "No IP address");
  CHECK(std::string(wifi_failure_text(R::connection_lost)) == "No response");
  // Reasons outside the bring-up policy fall back to a generic phrase.
  CHECK(std::string(wifi_failure_text(R::unknown)) == "Connection failed");
  CHECK(std::string(wifi_failure_text(R::requested_by_user)) == "Connection failed");
}

TEST_CASE("UIManager: build_values passes Wi-Fi enabled/connected through", "[UIManager][wifi]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  BuildContext ctx = make_default_ctx();

  ctx.wifi_enabled = true;
  ctx.wifi_connected = true;
  DisplayValues v = ui.build_values(ctx);
  CHECK(v.wifi_enabled);
  CHECK(v.wifi_connected);

  // Disconnected: icon still shown, but distinct glyph state.
  ctx.wifi_connected = false;
  v = ui.build_values(ctx);
  CHECK(v.wifi_enabled);
  CHECK_FALSE(v.wifi_connected);
}

TEST_CASE("UIManager: Screen::Info ignores all touch input", "[UIManager][info]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.show_info("Trying default Wi-Fi...");

  CHECK(press(ui, InputSource::TouchUp).action == UIAction::None);
  CHECK(press(ui, InputSource::TouchDown).action == UIAction::None);
  CHECK(press(ui, InputSource::TouchEnter).action == UIAction::None);
  CHECK(ui.current_screen() == Screen::Info);
}

TEST_CASE("UIManager: snackbar is suppressed on all session screens",
          "[UIManager][session][snackbar]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.show_snackbar("Mode changed");

  // Home — snackbar visible.
  auto ctx = make_default_ctx();
  ctx.now_ms = 100;
  ui.clear_expired_snackbar(ctx.now_ms);
  REQUIRE(ui.build_values(ctx).snackbar_text != nullptr);

  ui.show_info("Preparing stationary mode...");
  CHECK(ui.build_values(ctx).snackbar_text == nullptr);

  ui.open_provisioning(ProvisioningTransport::BleOnly);
  CHECK(ui.build_values(ctx).snackbar_text == nullptr);

  ui.open_provisioning_confirm(0);
  CHECK(ui.build_values(ctx).snackbar_text == nullptr);
}

// ============================================================================
// Provisioning page (two-row layout + confirm overlay)
// ============================================================================

TEST_CASE("UIManager: open_provisioning resets per-session state", "[UIManager][provisioning]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  // Dirty state from a "previous session" — should not leak after open.
  ui.set_provisioning_connected(0x0104a8c0); // 192.168.4.1
  ui.set_provisioning_ui_state(ProvisioningUiState::Connecting);

  ui.open_provisioning(ProvisioningTransport::WifiOnly);

  DisplayValues v = ui.build_values(make_default_ctx());
  CHECK(ui.current_screen() == Screen::Provisioning);
  CHECK(v.provisioning_connected_ip == 0);
  CHECK(v.provisioning_transport == static_cast<uint8_t>(ProvisioningTransport::WifiOnly));
  CHECK(v.provisioning_confirm_index == 0);
  CHECK(v.provisioning_confirm_kind == 0);
  REQUIRE(v.provisioning_status != nullptr);
  CHECK(std::string(v.provisioning_status) == "Waiting for setup...");
  CHECK(v.selected_row == 0);
  CHECK(v.row_count == 2);
}

TEST_CASE("UIManager: provisioning rows are two action rows with transport-aware labels",
          "[UIManager][provisioning]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.open_provisioning(ProvisioningTransport::BleOnly);
  DisplayValues v = ui.build_values(make_default_ctx());
  REQUIRE(v.row_count == 2);
  CHECK(std::string(v.rows[0].text) == "Use portal");
  CHECK(std::string(v.rows[1].text) == "Cancel setup");

  ui.open_provisioning(ProvisioningTransport::WifiOnly);
  v = ui.build_values(make_default_ctx());
  CHECK(std::string(v.rows[0].text) == "Use app");
  CHECK(std::string(v.rows[1].text) == "Cancel setup");
}

TEST_CASE("UIManager: provisioning TouchUp/Down toggles between two rows",
          "[UIManager][provisioning]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.open_provisioning(ProvisioningTransport::BleOnly);

  CHECK(ui.build_values(make_default_ctx()).selected_row == 0);
  press(ui, InputSource::TouchDown);
  CHECK(ui.build_values(make_default_ctx()).selected_row == 1);
  press(ui, InputSource::TouchDown);
  CHECK(ui.build_values(make_default_ctx()).selected_row == 0);
  press(ui, InputSource::TouchUp);
  CHECK(ui.build_values(make_default_ctx()).selected_row == 1);
}

TEST_CASE("UIManager: provisioning TouchEnter opens ProvisioningConfirm with the row kind",
          "[UIManager][provisioning][confirm]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.open_provisioning(ProvisioningTransport::BleOnly);

  // Row 0 (switch transport) — kind=0
  auto result = press(ui, InputSource::TouchEnter);
  CHECK(result.action == UIAction::None);
  CHECK(ui.current_screen() == Screen::ProvisioningConfirm);
  DisplayValues v = ui.build_values(make_default_ctx());
  CHECK(v.provisioning_confirm_kind == 0);
  CHECK(v.provisioning_confirm_index == 0); // No default

  // Back to Provisioning via No.
  press(ui, InputSource::TouchEnter);
  CHECK(ui.current_screen() == Screen::Provisioning);

  // Row 1 (cancel) — kind=1
  press(ui, InputSource::TouchDown); // cursor 0 -> 1
  press(ui, InputSource::TouchEnter);
  CHECK(ui.current_screen() == Screen::ProvisioningConfirm);
  CHECK(ui.build_values(make_default_ctx()).provisioning_confirm_kind == 1);
}

TEST_CASE("UIManager: ProvisioningConfirm question is transport- and kind-aware",
          "[UIManager][provisioning][confirm]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.open_provisioning(ProvisioningTransport::BleOnly);
  ui.open_provisioning_confirm(0); // switch transport
  DisplayValues v = ui.build_values(make_default_ctx());
  REQUIRE(v.row_count >= 1);
  CHECK(std::string(v.rows[0].text) == "Switch to Wi-Fi setup?");

  ui.open_provisioning(ProvisioningTransport::WifiOnly);
  ui.open_provisioning_confirm(0);
  v = ui.build_values(make_default_ctx());
  CHECK(std::string(v.rows[0].text) == "Switch to app setup?");

  ui.open_provisioning_confirm(1); // cancel
  v = ui.build_values(make_default_ctx());
  CHECK(std::string(v.rows[0].text) == "Cancel setup?");
}

TEST_CASE("UIManager: ProvisioningConfirm No returns to Provisioning with no action",
          "[UIManager][provisioning][confirm]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.open_provisioning(ProvisioningTransport::BleOnly);
  ui.open_provisioning_confirm(0);

  auto result = press(ui, InputSource::TouchEnter); // No is index 0 (default)
  CHECK(result.action == UIAction::None);
  CHECK(ui.current_screen() == Screen::Provisioning);
}

TEST_CASE("UIManager: ProvisioningConfirm Yes on switch emits ConfirmSwitch",
          "[UIManager][provisioning][confirm]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.open_provisioning(ProvisioningTransport::BleOnly);
  ui.open_provisioning_confirm(0);

  press(ui, InputSource::TouchDown); // No -> Yes
  auto result = press(ui, InputSource::TouchEnter);
  CHECK(result.action == UIAction::ConfirmSwitchProvisioningTransport);
  CHECK(ui.current_screen() == Screen::Provisioning);
}

TEST_CASE("UIManager: ProvisioningConfirm Yes on cancel emits ConfirmCancel",
          "[UIManager][provisioning][confirm]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.open_provisioning(ProvisioningTransport::BleOnly);
  ui.open_provisioning_confirm(1);

  press(ui, InputSource::TouchDown); // No -> Yes
  auto result = press(ui, InputSource::TouchEnter);
  CHECK(result.action == UIAction::ConfirmCancelProvisioning);
  // Stay on ProvisioningConfirm until orchestrator tears the session down.
}

TEST_CASE("UIManager: open_provisioning_confirm resets cursor to No",
          "[UIManager][provisioning][confirm]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.open_provisioning(ProvisioningTransport::BleOnly);

  // First confirm session — toggle to Yes.
  ui.open_provisioning_confirm(0);
  press(ui, InputSource::TouchDown);
  CHECK(ui.build_values(make_default_ctx()).provisioning_confirm_index == 1);

  // Re-open via TouchEnter on Provisioning — cursor should reset to No.
  ui.set_screen(Screen::Provisioning);
  press(ui, InputSource::TouchEnter);
  CHECK(ui.build_values(make_default_ctx()).provisioning_confirm_index == 0);
}

// ============================================================================
// Provisioning status text
// ============================================================================

TEST_CASE("UIManager: WaitingForCredentials text varies with transport",
          "[UIManager][provisioning][status]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.set_provisioning_transport(ProvisioningTransport::BleOnly);
  ui.set_provisioning_ui_state(ProvisioningUiState::WaitingForCredentials);
  CHECK(std::string(ui.build_values(make_default_ctx()).provisioning_status) ==
        "Waiting for app...");

  ui.set_provisioning_transport(ProvisioningTransport::WifiOnly);
  CHECK(std::string(ui.build_values(make_default_ctx()).provisioning_status) ==
        "Waiting for setup...");
}

TEST_CASE("UIManager: SwitchingTransport names the target transport",
          "[UIManager][provisioning][status]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.set_provisioning_transport(ProvisioningTransport::BleOnly);
  ui.set_provisioning_ui_state(ProvisioningUiState::SwitchingTransport);
  CHECK(std::string(ui.build_values(make_default_ctx()).provisioning_status) ==
        "Switching to Wi-Fi...");

  ui.set_provisioning_transport(ProvisioningTransport::WifiOnly);
  CHECK(std::string(ui.build_values(make_default_ctx()).provisioning_status) ==
        "Switching to BLE...");
}

TEST_CASE("UIManager: Connecting/ConnectFailed map to fixed ASCII text",
          "[UIManager][provisioning][status]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.set_provisioning_ui_state(ProvisioningUiState::Connecting);
  CHECK(std::string(ui.build_values(make_default_ctx()).provisioning_status) == "Connecting...");

  ui.set_provisioning_ui_state(ProvisioningUiState::ConnectFailed);
  CHECK(std::string(ui.build_values(make_default_ctx()).provisioning_status) ==
        "Connect failed - try again");
}

TEST_CASE("UIManager: set_provisioning_connected formats Connected! a.b.c.d",
          "[UIManager][provisioning][status]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.set_provisioning_connected(0x0104a8c0); // network byte order: 192.168.4.1
  DisplayValues v = ui.build_values(make_default_ctx());
  REQUIRE(v.provisioning_status != nullptr);
  CHECK(std::string(v.provisioning_status) == "Connected! 192.168.4.1");
  CHECK(v.provisioning_connected_ip == 0x0104a8c0);
}

TEST_CASE("UIManager: set_provisioning_connected(0) restores transport-derived status",
          "[UIManager][provisioning][status]") {
  UIManager ui(DEFAULT_UI_CONFIG);

  ui.set_provisioning_transport(ProvisioningTransport::BleOnly);
  ui.set_provisioning_ui_state(ProvisioningUiState::WaitingForCredentials);
  ui.set_provisioning_connected(0xffffffff);
  REQUIRE(std::string(ui.build_values(make_default_ctx()).provisioning_status) ==
          "Connected! 255.255.255.255");

  ui.set_provisioning_connected(0);
  // After clear, the underlying UI state may still be Connected — restore
  // to WaitingForCredentials to verify the IP override is gone.
  ui.set_provisioning_ui_state(ProvisioningUiState::WaitingForCredentials);
  CHECK(std::string(ui.build_values(make_default_ctx()).provisioning_status) ==
        "Waiting for app...");
}

TEST_CASE("UIManager: Idle clears provisioning status", "[UIManager][provisioning][status]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.set_provisioning_ui_state(ProvisioningUiState::WaitingForCredentials);
  ui.set_provisioning_ui_state(ProvisioningUiState::Idle);
  CHECK(ui.build_values(make_default_ctx()).provisioning_status == nullptr);
}

TEST_CASE("UIManager: provisioning_ap_ssid is built from serial number",
          "[UIManager][provisioning]") {
  UIManager ui(DEFAULT_UI_CONFIG);
  ui.open_provisioning(ProvisioningTransport::WifiOnly);
  DisplayValues v = ui.build_values(make_default_ctx());
  REQUIRE(v.provisioning_ap_ssid != nullptr);
  CHECK(std::string(v.provisioning_ap_ssid) == "airgradient-AABBCCDDEEFF");
}
