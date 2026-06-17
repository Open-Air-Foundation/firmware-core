#ifndef GO_DISPLAY_H
#define GO_DISPLAY_H

#include <cstdint>

#include "measures_types.h"
#include "rtos.h"
#include "services/provisioning_qr.h"

// ---------------------------------------------------------------------------
// Host-compatible types (no ESP-IDF dependency)
// ---------------------------------------------------------------------------

enum class Screen : uint8_t {
  Home,
  MainMenu,
  Settings,
  SettingsChoice,
  TagList,
  About,
  Confirm,
  ShutdownUser,        ///< Goodbye screen for user-initiated power-off
  ShutdownDischarge,   ///< Safety-trip shutdown — battery critically low (OverDischarge)
  ShutdownTemperature, ///< Safety-trip shutdown — battery overheated (OverTemperature)
  PairingPasskey,      ///< Shows 6-digit BLE pairing passkey
  Provisioning,        ///< Stationary Wi-Fi provisioning page (QR + status + actions)
  ProvisioningConfirm, ///< Yes/No confirmation overlay for Provisioning actions
  Info,                ///< Generic single-text presentation surface (bring-up narration, etc.)
  GettingStarted,      ///< One-time first-boot guide (setup QR + single action row)

  // --- Fuel-gauge learning phase screens (text-only banners) ---
  FgLearnCharging,   ///< "Battery learning" / "Charging…"
  FgLearnResting,    ///< "Battery learning" / "Resting…"
  FgLearnUnplug,     ///< "Battery learning" / "Unplug charger"
  DischargeComplete, ///< "Battery learning" / "Discharge complete"
  FgLearnVerifying,  ///< "Battery learning" / "Verifying…"
  FgLearnComplete,   ///< "Battery learning" / "Complete"
  FgLearnFailed,     ///< "Battery learning" / "Failed"
};

enum class Metric : uint8_t {
  None,
  Pm25,
  Co2,
  Temp,
  Humidity,
  Tvoc,
  Nox,
};

inline constexpr uint8_t MAX_LIST_ROWS = 10;

struct ListRow {
  char text[48];
  bool disabled = false;
};

struct DisplayValues {
  // --- Sensor readings (channel A) ---
  int co2_ppm = MeasuresInvalid::CO2;
  float pm25_ugm3 = MeasuresInvalid::PM;
  float temperature_c = MeasuresInvalid::TEMPERATURE;
  float humidity_pct = MeasuresInvalid::HUMIDITY;
  int tvoc_index = MeasuresInvalid::TVOC;
  int nox_index = MeasuresInvalid::NOX;
  float pressure_hpa = MeasuresInvalid::PRESSURE;
  float altitude_m = MeasuresInvalid::ALTITUDE;

  // --- Battery ---
  uint8_t battery_pct = 0xFF; // 0xFF = no data
  bool is_battery_charging = false;
  bool is_plugged_in = false;

  // --- Status flags ---
  bool locked = true;
  bool ble_enabled = false;
  bool ble_connected = false;
  bool wifi_enabled = false;   // show the Wi-Fi icon (Stationary mode)
  bool wifi_connected = false; // connected vs disconnected glyph
  bool gps_enabled = true;
  bool gps_fix = false;
  bool tracking_active = false;
  bool display_off = false;
  bool use_fahrenheit = false;
  bool pm_use_usaqi = false;

  // --- Screen navigation ---
  Screen screen = Screen::Home;
  Metric active_metric = Metric::None;

  // --- List/menu content ---
  ListRow rows[MAX_LIST_ROWS] = {};
  uint8_t row_count = 0;
  uint8_t selected_row = 0;
  bool show_separator_after_back = false;

  // --- About screen ---
  const char *about_title = nullptr;
  const char *about_firmware = nullptr;
  const char *about_serial = nullptr;
  const char *about_hardware = nullptr;

  // --- Chart data ---
  const float *chart_samples = nullptr;
  uint8_t chart_count = 0;
  float chart_min = 0.0f;
  float chart_max = 0.0f;

  // --- Snackbar ---
  const char *snackbar_text = nullptr;

  // --- BLE pairing ---
  uint32_t ble_passkey = 0; ///< 6-digit passkey for PairingPasskey screen

  // --- Stationary networking (Provisioning screen only — Home conveys
  // network state purely through the status-bar Wi-Fi icon per spec) ---
  const char *provisioning_status = nullptr; ///< Transport-specific instructions
  uint8_t provisioning_transport = 0;        ///< ProvisioningTransport value

  /// Connected-state IP for the Provisioning success message.  Network
  /// byte order (low byte = first octet) to match WifiGotIpCallback /
  /// WifiStaticIpConfig / format_ipv4_be.  Non-zero overrides the
  /// status-line text with "Connected! a.b.c.d".
  uint32_t provisioning_connected_ip = 0;

  /// 0 = switch transport, 1 = cancel setup (drives ProvisioningConfirm question).
  uint8_t provisioning_confirm_kind = 0;

  /// 0 = No (default), 1 = Yes (drives ProvisioningConfirm button highlight).
  uint8_t provisioning_confirm_index = 0;

  /// WifiOnly captive-portal AP SSID for Provisioning instruction L1.
  /// Borrowed pointer (UIManager owns).  Null -> placeholder.
  const char *provisioning_ap_ssid = nullptr;

  /// WifiOnly captive-portal AP password for Provisioning instruction L2.
  /// Borrowed pointer.  Null -> placeholder.
  const char *provisioning_ap_password = nullptr;

  /// QR for the Provisioning / Getting Started pages (mutually exclusive).
  /// Borrowed; UIManager re-encodes on entry. Null/empty skips the QR area.
  const AirgradientProvisioning::QrCode *qr = nullptr;

  // --- Info screen (generic single-text page) ---
  /// Active source string for Screen::Info.  Plain ASCII.  Newlines are
  /// honored as hard breaks; longer runs auto-wrap.  Pointer must remain
  /// valid through the next DisplayValues snapshot.  Null/empty renders a
  /// blank canvas.
  const char *info_text = nullptr;
};

// ---------------------------------------------------------------------------
// RTC display snapshot — saved before deep sleep, loaded on button wake.
// Scalar data only; no pointers.  Estimated size: ~50 bytes.
// ---------------------------------------------------------------------------

struct RtcDisplaySnapshot {
  // Sensor values
  int co2_ppm;
  float pm25_ugm3;
  float temperature_c;
  float humidity_pct;
  int tvoc_index;
  int nox_index;
  float pressure_hpa;
  float altitude_m;

  // Battery
  uint8_t battery_pct;
  bool is_battery_charging;

  // Status flags
  bool gps_enabled;
  bool gps_fix;
  bool tracking_active;
  bool ble_enabled;

  // Rendering settings
  bool use_fahrenheit;
  bool pm_use_usaqi;
};

// ---------------------------------------------------------------------------
// DisplayService (hardware-dependent, excluded from host builds)
// ---------------------------------------------------------------------------

#ifndef TEST_HOST

#include <driver/spi_master.h>

extern "C" {
#include "u8g2.h"
}

/// Display refresh tier — controls how the e-paper controller updates.
enum class RefreshMode : uint8_t {
  Full,    ///< Full GC waveform (flash). Resets basemap in both RAM planes.
  Fast,    ///< Full-screen differential (no flash). Writes full frame to RAM 0x24.
  Partial, ///< Body-only differential (no flash). Writes body region to RAM 0x24.
};

class DisplayService {
public:
  struct Config {
    // SPI
    spi_host_device_t spi_host;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int pin_busy;
    int clock_hz = 4000000;

    // Worker task
    uint16_t task_stack_size = 4096;
    uint8_t task_priority = 4;

    // Refresh limits
    uint8_t max_partial_ops = 20;
  };

  explicit DisplayService(const Config &config);

  /// Initialize display hardware and start worker task.
  ///
  /// When defer_refresh is false (default): performs a synchronous full
  /// refresh, then starts the async worker task.  Unchanged behavior for
  /// run_full_boot() and run_fast_path().
  ///
  /// When defer_refresh is true: renders into the software buffer, starts
  /// the worker task, and posts the initial full refresh as the worker's
  /// first job.  Returns in ~10 ms without waiting for the e-paper refresh.
  /// The worker holds the SPI bus for the duration (~3 s), naturally
  /// serializing any other SPI device (NAND) without explicit coordination.
  bool init(const DisplayValues &initial, bool defer_refresh = false);

  /// Submit a new frame for display.
  /// Renders into framebuffer (fast), then signals worker task.
  /// wait=false and worker busy: returns false (skipped).
  /// wait=true: blocks until worker finishes previous refresh.
  bool update(const DisplayValues &values, bool wait = false);

  /// Synchronous one-shot update for fast-path boot.
  /// Renders and drives SPI inline (blocking). Does not use worker task.
  void update_sync(const DisplayValues &values);

  /// Wait until the most-recently-queued frame has finished painting.
  /// Returns immediately when the worker is idle.  Cheap polling loop
  /// using RTOS::delay_ms(1), mirroring the existing clear()/stop()
  /// busy-wait pattern.
  ///
  /// Must NOT be called from the display worker task itself
  /// (self-deadlocks because _worker_busy clears only when the worker
  /// returns to its loop).  Safe from any other task, including the
  /// orchestrator task — the only caller in this product.
  void flush();

  /// Clear display to white (full refresh). Blocking.
  void clear();

  /// Put EPD controller into deep sleep mode.
  void deep_sleep();

  /// Stop worker task. Call before entering ESP deep sleep.
  void stop();

private:
  static constexpr int BUF_ROW_BYTES = 16;
  static constexpr int BUF_TILE_HEIGHT = 32;
  static constexpr int BUF_SIZE = BUF_ROW_BYTES * BUF_TILE_HEIGHT * 8; // 4096
  static constexpr int BODY_Y = 18;
  static constexpr int BODY_H = 232;
  // Sized to the full canvas (128x250) so session screens can run a
  // whole-screen partial refresh and avoid title-region ghosting.  Non-
  // session screens still copy only the body slice (BODY_H rows).
  static constexpr int FULL_H = 250;
  static constexpr int REGION_SIZE = BUF_ROW_BYTES * FULL_H; // 4000

  Config _config;

  // u8g2 instance and render buffer
  u8g2_t _u8g2;
  uint8_t _render_buf[BUF_SIZE];

  // SPI transmit buffer (owned by worker after signal)
  uint8_t _spi_buf[BUF_SIZE];
  uint8_t _region_buf[REGION_SIZE];

  // Refresh state
  DisplayValues _prev_values;
  uint8_t _diff_count = 0;
  RefreshMode _pending_mode = RefreshMode::Full;
  bool _defer_header_check = false;
  bool _menu_exited = false;

  // Worker task
  RtosTaskHandle _task_handle = nullptr;
  volatile bool _running = false;
  volatile bool _worker_busy = false;

  // Render methods
  void _render_frame(const DisplayValues &v);
  bool _is_header_changed(const DisplayValues &a, const DisplayValues &b) const;

  void _draw_status_bar(const DisplayValues &v);
  void _draw_home(const DisplayValues &v);
  void _draw_menu_overlay(const DisplayValues &v);
  void _draw_full_screen_list(const DisplayValues &v);
  void _draw_snackbar(const DisplayValues &v);
  void _draw_shutdown(Screen s);
  void _draw_pairing_passkey(const DisplayValues &v);
  void _draw_chart(const DisplayValues &v);
  void _draw_info(const DisplayValues &v);
  void _draw_fg_learning_banner(Screen screen);
  void _draw_provisioning(const DisplayValues &v);
  void _draw_provisioning_confirm(const DisplayValues &v);
  void _draw_getting_started(const DisplayValues &v);

  // Worker
  static void _worker_entry(void *arg);
  void _worker_loop();
};

// ---------------------------------------------------------------------------
// Free functions — RTC display snapshot persistence (non-TEST_HOST only)
// ---------------------------------------------------------------------------

/// Save display values to RTC memory as a snapshot for the next button wake.
/// Called from prepare_for_sleep() after the final display update.
void save_rtc_display_snapshot(const DisplayValues &values);

/// Load the RTC display snapshot saved before the last deep sleep.
/// Returns true and fills *snapshot_out when a valid snapshot exists.
/// Returns false when no valid snapshot is present (first power-on).
bool load_rtc_display_snapshot(RtcDisplaySnapshot *snapshot_out);

#else // TEST_HOST

// ---------------------------------------------------------------------------
// Host-test stub — no-op implementations so the Orchestrator compiles
// without conditional compilation in its own source files.
// ---------------------------------------------------------------------------

class DisplayService {
public:
  struct Config {};

  explicit DisplayService(const Config &) {}

  bool init(const DisplayValues &, bool = false) { return true; }

  bool update(const DisplayValues &, bool = false) {
    ++spy_update_count;
    return true;
  }

  void update_sync(const DisplayValues &) {}
  void flush() { ++spy_flush_count; }
  void clear() {}
  void deep_sleep() { spy_deep_sleep_called = true; }
  void stop() {}

  // Test spies — reset via test_spy::reset() in stubs.
  inline static bool spy_deep_sleep_called = false;
  inline static uint32_t spy_update_count = 0;
  inline static uint32_t spy_flush_count = 0;
};

// Stub implementations for host builds.
inline void save_rtc_display_snapshot(const DisplayValues &) {}
inline bool load_rtc_display_snapshot(RtcDisplaySnapshot *) { return false; }

#endif // !TEST_HOST

#endif // GO_DISPLAY_H
