#include "go_display.h"

#ifndef TEST_HOST

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "go_text_wrap.h"

#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

extern "C" {
#include "u8x8.h"
}

#include "rtos.h"

static constexpr const char *TAG = "go_display";

// ===========================================================================
// RTC display snapshot — persists across deep sleep
// ===========================================================================
//
// Survives deep sleep in RTC slow memory.  Written by prepare_for_sleep()
// and read by run_button_wake_path() on the next button wake.
//
// s_rtc_display_snapshot_valid is false on first power-on (zero-initialized
// RTC memory), so an uninitialized snapshot is never used.

RTC_DATA_ATTR static RtcDisplaySnapshot s_rtc_display_snapshot;
RTC_DATA_ATTR static bool s_rtc_display_snapshot_valid = false;

void save_rtc_display_snapshot(const DisplayValues &values) {
  s_rtc_display_snapshot.co2_ppm = values.co2_ppm;
  s_rtc_display_snapshot.pm25_ugm3 = values.pm25_ugm3;
  s_rtc_display_snapshot.temperature_c = values.temperature_c;
  s_rtc_display_snapshot.humidity_pct = values.humidity_pct;
  s_rtc_display_snapshot.tvoc_index = values.tvoc_index;
  s_rtc_display_snapshot.nox_index = values.nox_index;
  s_rtc_display_snapshot.pressure_hpa = values.pressure_hpa;
  s_rtc_display_snapshot.altitude_m = values.altitude_m;
  s_rtc_display_snapshot.battery_pct = values.battery_pct;
  s_rtc_display_snapshot.is_battery_charging = values.is_battery_charging;
  s_rtc_display_snapshot.gps_enabled = values.gps_enabled;
  s_rtc_display_snapshot.gps_fix = values.gps_fix;
  s_rtc_display_snapshot.tracking_active = values.tracking_active;
  s_rtc_display_snapshot.ble_enabled = values.ble_enabled;
  s_rtc_display_snapshot.use_fahrenheit = values.use_fahrenheit;
  s_rtc_display_snapshot.use_feet = values.use_feet;
  s_rtc_display_snapshot.pm_use_usaqi = values.pm_use_usaqi;
  s_rtc_display_snapshot_valid = true;
}

bool load_rtc_display_snapshot(RtcDisplaySnapshot *snapshot_out) {
  if (!s_rtc_display_snapshot_valid) {
    return false;
  }
  *snapshot_out = s_rtc_display_snapshot;
  return true;
}

// ===========================================================================
// Anonymous namespace: display driver + rendering helpers
// ===========================================================================

namespace {

// ---------------------------------------------------------------------------
// Display driver — SSD1680 SPI protocol (file-local)
// ---------------------------------------------------------------------------

constexpr int WIDTH_PX = 128;
constexpr int HEIGHT_PX = 250;
constexpr int FRAME_BYTES = (WIDTH_PX * HEIGHT_PX) / 8; // 4000
constexpr size_t TX_BOUNCE_BYTES = 4096;

// Error-propagation macro for driver functions returning esp_err_t.
// Intentionally unscoped so it works in non-void-returning functions.
#define DISP_RETURN_ON_ERR(expr)                                                                   \
  do {                                                                                             \
    const esp_err_t _e = (expr);                                                                   \
    if (_e != ESP_OK)                                                                              \
      return _e;                                                                                   \
  } while (0)

struct DriverState {
  bool inited = false;
  bool bus_acquired = false;

  spi_host_device_t spi_host = SPI2_HOST;
  int spi_clock_hz = 4000000;

  gpio_num_t pin_cs = GPIO_NUM_NC;
  gpio_num_t pin_dc = GPIO_NUM_NC;
  gpio_num_t pin_rst = GPIO_NUM_NC;
  gpio_num_t pin_busy = GPIO_NUM_NC;

  spi_device_handle_t spi = nullptr;
  uint8_t *tx_bounce = nullptr;
};

DriverState g_driver;

// --- Low-level GPIO helpers ---

inline void set_cs(int level) { gpio_set_level(g_driver.pin_cs, level); }
inline void set_dc(int level) { gpio_set_level(g_driver.pin_dc, level); }
inline void set_rst(int level) { gpio_set_level(g_driver.pin_rst, level); }

void delay_ms(uint32_t ms) {
  if (ms == 0)
    return;
  RTOS::delay_ms(ms);
}

// Poll cadence must be >= 1 tick; a sub-tick delay degrades to vTaskDelay(0)
// (bare yield), starving the lower-priority LED/buzzer tasks during refresh.
static constexpr uint32_t BUSY_POLL_MS = 10;

// BUSY=1 means controller processing; BUSY=0 means ready.
void wait_busy_low() {
  while (gpio_get_level(g_driver.pin_busy) != 0) {
    RTOS::delay_ms(BUSY_POLL_MS);
  }
}

// --- SPI transfer primitives ---

esp_err_t spi_write_byte(uint8_t value) {
  spi_transaction_t t = {};
  t.flags = SPI_TRANS_USE_TXDATA;
  t.length = 8;
  t.tx_data[0] = value;
  return spi_device_polling_transmit(g_driver.spi, &t);
}

esp_err_t spi_write_bulk(const uint8_t *data, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    const size_t chunk = (len - offset > TX_BOUNCE_BYTES) ? TX_BOUNCE_BYTES : (len - offset);
    memcpy(g_driver.tx_bounce, data + offset, chunk);
    spi_transaction_t t = {};
    t.length = chunk * 8;
    t.tx_buffer = g_driver.tx_bounce;
    DISP_RETURN_ON_ERR(spi_device_polling_transmit(g_driver.spi, &t));
    offset += chunk;
  }
  return ESP_OK;
}

// --- SSD1680 command/data protocol ---

esp_err_t write_cmd(uint8_t command) {
  set_cs(0);
  set_dc(0);
  const esp_err_t err = spi_write_byte(command);
  set_cs(1);
  return err;
}

esp_err_t write_data(uint8_t data) {
  set_cs(0);
  set_dc(1);
  const esp_err_t err = spi_write_byte(data);
  set_cs(1);
  return err;
}

esp_err_t write_data_bytes(const uint8_t *data, size_t len) {
  set_cs(0);
  set_dc(1);
  const esp_err_t err = spi_write_bulk(data, len);
  set_cs(1);
  return err;
}

// --- Driver public API ---

esp_err_t driver_init(const DisplayService::Config &cfg) {
  if (g_driver.inited)
    return ESP_OK;

  g_driver.spi_host = cfg.spi_host;
  g_driver.spi_clock_hz = cfg.clock_hz;
  g_driver.pin_cs = static_cast<gpio_num_t>(cfg.pin_cs);
  g_driver.pin_dc = static_cast<gpio_num_t>(cfg.pin_dc);
  g_driver.pin_rst = static_cast<gpio_num_t>(cfg.pin_rst);
  g_driver.pin_busy = static_cast<gpio_num_t>(cfg.pin_busy);

  // Configure output GPIOs (CS, DC, RST)
  gpio_config_t out_cfg = {};
  out_cfg.pin_bit_mask = (1ULL << cfg.pin_cs) | (1ULL << cfg.pin_dc) | (1ULL << cfg.pin_rst);
  out_cfg.mode = GPIO_MODE_OUTPUT;
  out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  out_cfg.intr_type = GPIO_INTR_DISABLE;
  DISP_RETURN_ON_ERR(gpio_config(&out_cfg));

  // Configure BUSY input
  gpio_config_t in_cfg = {};
  in_cfg.pin_bit_mask = 1ULL << cfg.pin_busy;
  in_cfg.mode = GPIO_MODE_INPUT;
  in_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  in_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  in_cfg.intr_type = GPIO_INTR_DISABLE;
  DISP_RETURN_ON_ERR(gpio_config(&in_cfg));

  set_cs(1);
  set_dc(1);
  set_rst(1);

  // Attach SPI device (CS managed manually, not by ESP-IDF driver)
  spi_device_interface_config_t devcfg = {};
  devcfg.mode = 0;
  devcfg.clock_speed_hz = cfg.clock_hz;
  devcfg.spics_io_num = -1;
  devcfg.queue_size = 1;
  devcfg.flags = SPI_DEVICE_NO_DUMMY;
  DISP_RETURN_ON_ERR(spi_bus_add_device(cfg.spi_host, &devcfg, &g_driver.spi));

  // DMA-capable bounce buffer for chunked SPI writes
  g_driver.tx_bounce = static_cast<uint8_t *>(heap_caps_malloc(TX_BOUNCE_BYTES, MALLOC_CAP_DMA));
  if (g_driver.tx_bounce == nullptr)
    return ESP_ERR_NO_MEM;

  delay_ms(100);
  g_driver.inited = true;
  ESP_LOGI(TAG, "EPD driver init done");
  return ESP_OK;
}

esp_err_t driver_bus_acquire() {
  if (!g_driver.inited || g_driver.spi == nullptr)
    return ESP_ERR_INVALID_STATE;
  if (g_driver.bus_acquired)
    return ESP_ERR_INVALID_STATE;

  // ESP-IDF spi_device_acquire_bus() only supports portMAX_DELAY (infinite
  // wait).  Finite timeouts return ESP_ERR_INVALID_ARG.
  DISP_RETURN_ON_ERR(spi_device_acquire_bus(g_driver.spi, portMAX_DELAY));
  g_driver.bus_acquired = true;
  return ESP_OK;
}

void driver_bus_release() {
  if (!g_driver.bus_acquired)
    return;
  if (g_driver.spi != nullptr) {
    spi_device_release_bus(g_driver.spi);
  }
  g_driver.bus_acquired = false;
}

// Full SSD1680 initialization sequence
esp_err_t driver_hw_init_full() {
  // Hardware reset
  set_rst(0);
  delay_ms(10);
  set_rst(1);
  delay_ms(10);
  wait_busy_low();

  // Software reset
  DISP_RETURN_ON_ERR(write_cmd(0x12));
  wait_busy_low();

  // Driver output control: gate lines = HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x01));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Data entry mode: X increment, Y decrement
  DISP_RETURN_ON_ERR(write_cmd(0x11));
  DISP_RETURN_ON_ERR(write_data(0x01));

  // RAM-X window: 0 to 15 (0..127 pixels)
  DISP_RETURN_ON_ERR(write_cmd(0x44));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(WIDTH_PX / 8 - 1)));

  // RAM-Y window: 249 to 0
  DISP_RETURN_ON_ERR(write_cmd(0x45));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Border waveform (full mode)
  DISP_RETURN_ON_ERR(write_cmd(0x3C));
  DISP_RETURN_ON_ERR(write_data(0x05));

  // Display update control
  DISP_RETURN_ON_ERR(write_cmd(0x21));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // Built-in temperature sensor
  DISP_RETURN_ON_ERR(write_cmd(0x18));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // RAM-X counter to 0
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // RAM-Y counter to HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  wait_busy_low();

  return ESP_OK;
}

// Write frame to both SSD1680 RAM planes + trigger full update.
esp_err_t driver_set_basemap(const uint8_t *data) {
  DISP_RETURN_ON_ERR(write_cmd(0x24));
  DISP_RETURN_ON_ERR(write_data_bytes(data, FRAME_BYTES));
  DISP_RETURN_ON_ERR(write_cmd(0x26));
  DISP_RETURN_ON_ERR(write_data_bytes(data, FRAME_BYTES));

  // Trigger full update
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xF7));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();
  return ESP_OK;
}

// Prepare controller for partial update mode.
esp_err_t driver_part_begin() {
  // Hardware reset
  set_rst(0);
  delay_ms(10);
  set_rst(1);
  delay_ms(10);
  wait_busy_low();

  // Re-establish Y mapping after reset
  DISP_RETURN_ON_ERR(write_cmd(0x01));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Border waveform (partial mode — differs from full init)
  DISP_RETURN_ON_ERR(write_cmd(0x3C));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // Data entry mode (same as full init)
  DISP_RETURN_ON_ERR(write_cmd(0x11));
  DISP_RETURN_ON_ERR(write_data(0x01));

  return ESP_OK;
}

// Write pixel data to a rectangular region for partial update.
// Coordinates are logical screen coordinates (Y=0 at top).
esp_err_t driver_part_write_region(unsigned int x_start_px, unsigned int y_start_px,
                                   const uint8_t *data, unsigned int height_px,
                                   unsigned int width_px) {
  const unsigned int x_start = x_start_px / 8;
  const unsigned int x_end = x_start + width_px / 8 - 1;

  // Map logical top-origin Y to controller Y (inverted).
  // Controller Y=249 corresponds to screen top (Y=0).
  const unsigned int y_start = (HEIGHT_PX - 1) - y_start_px;
  const unsigned int y_end = (HEIGHT_PX - 1) - (y_start_px + height_px - 1);

  // Set RAM window
  DISP_RETURN_ON_ERR(write_cmd(0x44));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(x_start)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(x_end)));

  DISP_RETURN_ON_ERR(write_cmd(0x45));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start / 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_end % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_end / 256)));

  // Set RAM cursor
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(x_start)));
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start / 256)));

  // Write pixel data (new RAM plane only; partial uses diff against basemap)
  DISP_RETURN_ON_ERR(write_cmd(0x24));
  return write_data_bytes(data, static_cast<size_t>(height_px) * width_px / 8);
}

// Trigger partial display update.
esp_err_t driver_part_commit() {
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xFF));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();
  return ESP_OK;
}

// ---------------------------------------------------------------------------
// Fast refresh (0xC7 waveform) — full-screen, no flash
// ---------------------------------------------------------------------------
// Uses a temperature-override trick: force the SSD1680 to load the OTP LUT
// for 100 °C, which selects a shorter, more aggressive waveform. The 0xC7
// trigger then drives all pixels unconditionally (non-differential).

// Initialize controller for fast refresh mode.
// Must be called before driver_fast_write() + driver_fast_commit().
esp_err_t driver_hw_init_fast() {
  // Hardware reset
  set_rst(0);
  delay_ms(10);
  set_rst(1);
  delay_ms(10);

  // Software reset (clears all registers to defaults)
  DISP_RETURN_ON_ERR(write_cmd(0x12));
  wait_busy_low();

  // Select built-in temperature sensor
  DISP_RETURN_ON_ERR(write_cmd(0x18));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // Load actual temperature value into register
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xB1));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();

  // Override temperature register to 100 °C → selects faster OTP LUT
  DISP_RETURN_ON_ERR(write_cmd(0x1A));
  DISP_RETURN_ON_ERR(write_data(0x64)); // 100 °C
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Reload LUT using the overridden temperature
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0x91));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();

  // Re-establish display geometry (SW reset cleared these).
  // Must match driver_hw_init_full() for our frame buffer layout.

  // Driver output control: gate lines = HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x01));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Data entry mode: X increment, Y decrement
  DISP_RETURN_ON_ERR(write_cmd(0x11));
  DISP_RETURN_ON_ERR(write_data(0x01));

  // RAM-X window: 0 to 15 (0..127 pixels)
  DISP_RETURN_ON_ERR(write_cmd(0x44));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(WIDTH_PX / 8 - 1)));

  // RAM-Y window: 249 to 0
  DISP_RETURN_ON_ERR(write_cmd(0x45));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // RAM-X counter to 0
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // RAM-Y counter to HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));

  return ESP_OK;
}

// Write full frame to both RAM planes for fast refresh.
// Writes 0x24 (new) and 0x26 (old/basemap) with identical data so that
// subsequent partial refreshes (which diff 0x24 vs 0x26) remain coherent.
esp_err_t driver_fast_write(const uint8_t *data) {
  // Write image to new RAM plane (0x24)
  DISP_RETURN_ON_ERR(write_cmd(0x24));
  DISP_RETURN_ON_ERR(write_data_bytes(data, FRAME_BYTES));

  // Reset RAM cursor for second plane write
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));

  // Write same image to old RAM plane (0x26) — basemap coherence
  DISP_RETURN_ON_ERR(write_cmd(0x26));
  return write_data_bytes(data, FRAME_BYTES);
}

// Trigger fast display update (0xC7 waveform — non-differential, no flash).
esp_err_t driver_fast_commit() {
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xC7));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();
  return ESP_OK;
}

// Put SSD1680 into deep sleep mode 1.
esp_err_t driver_deep_sleep() {
  DISP_RETURN_ON_ERR(write_cmd(0x10));
  DISP_RETURN_ON_ERR(write_data(0x01));
  delay_ms(100);
  return ESP_OK;
}

#undef DISP_RETURN_ON_ERR

// ---------------------------------------------------------------------------
// Layout constants — see products/go/docs/display_service.md
// ---------------------------------------------------------------------------

constexpr int SCREEN_W = 128;
constexpr int CONTENT_W = 122; // preserved for non-home screens (snackbar, list, etc.)
constexpr int BODY_Y = 18;
constexpr int BODY_H = 232;

// Status bar
constexpr int STATUS_DIVIDER_Y = 17;
constexpr int ICON_GAP = 2;
constexpr int LOCK_X = 6;
constexpr int BATTERY_X = 116;
constexpr int TRACKING_X = 109;
constexpr int STATUS_BASELINE_Y = 12;
constexpr int ELLIPSE_CY = 8;
constexpr int LINK_CY = 5;

// Home screen: hero blocks
constexpr int PM_BLOCK_Y = 18;
constexpr int PM_BLOCK_H = 72;
constexpr int PM_LABEL_BASELINE_Y = 44;
constexpr int PM_VALUE_BASELINE_Y = 84;
constexpr int CO2_BLOCK_Y = 90;
constexpr int CO2_BLOCK_H = 72;
constexpr int CO2_LABEL_NAME_BASELINE_Y = 112;
constexpr int CO2_LABEL_UNIT_BASELINE_Y = 113;
constexpr int CO2_VALUE_BASELINE_Y = 153;

// Home screen: grid
constexpr int GRID_DIVIDER_0_Y = 162; // between CO2 and row 1
constexpr int GRID_DIVIDER_1_Y = 192; // between row 1 and row 2
constexpr int GRID_DIVIDER_2_Y = 222; // between row 2 and row 3
constexpr int GRID_DIVIDER_X = 64;

// Display-off
constexpr int DISPLAY_OFF_LOGO_Y = 113;
constexpr int DISPLAY_OFF_LOGO_H = 24;

// System screens — shared divider chrome (geometry only;
// each screen positions its own divider Y).
constexpr int SYSTEM_DIVIDER_X_MARGIN = 18;
constexpr int SYSTEM_DIVIDER_THICKNESS = 3;

// Shutdown — brand header + reason-specific icon/text
constexpr int SHUTDOWN_BRAND_BASELINE_Y = 34;
constexpr int SHUTDOWN_DIVIDER_Y = 49;
constexpr int SHUTDOWN_ICON_CENTER_Y = 94;
constexpr int SHUTDOWN_TITLE_L1_BASELINE_Y = 151;
constexpr int SHUTDOWN_TITLE_L2_BASELINE_Y = 169;
constexpr int SHUTDOWN_ACTION_BASELINE_Y = 198;
constexpr int SHUTDOWN_DETAIL_BASELINE_Y = 214;

// Pairing passkey — title-as-header + grouped passkey + hint.
constexpr int PAIRING_TITLE_L1_BASELINE_Y = 58;
constexpr int PAIRING_TITLE_L2_BASELINE_Y = 76;
constexpr int PAIRING_DIVIDER_Y = 86;
constexpr int PAIRING_PASSKEY_BASELINE_Y = 145;
constexpr int PAIRING_HINT_BASELINE_Y = 180;

// Chart — shifted 1px down for 2px-thick 3rd grid divider when active
constexpr int PLOT_X = 4;
constexpr int PLOT_Y = 225;
constexpr int PLOT_W = 121; // x=4..124 → 121 pixels
constexpr int PLOT_H = 25;  // y=225..248 → 25 pixels

// Snackbar (unchanged)
constexpr int SNACKBAR_Y = 232;
constexpr int SNACKBAR_H = 18;
constexpr int SNACKBAR_TEXT_BASELINE_Y = 244;

// Menu overlays
constexpr int MAIN_MENU_BG_Y = 162;
constexpr int MAIN_MENU_BG_H = 88;
constexpr int FULL_SCREEN_BG_Y = 18;
constexpr int FULL_SCREEN_BG_H = 232;

// Grid cell label/value positions
constexpr int GRID_LABEL_X[6] = {7, 68, 7, 68, 7, 68};
constexpr int GRID_LABEL_Y[6] = {175, 175, 205, 205, 235, 235};
constexpr int GRID_VALUE_X[6] = {7, 68, 7, 68, 7, 68};
constexpr int GRID_VALUE_Y[6] = {187, 187, 217, 217, 247, 247};

// Selection rectangles — shifted 1px down for 2px-thick 1st grid divider
constexpr int SEL_TEMP_X = 0;
constexpr int SEL_TEMP_Y = 164;
constexpr int SEL_TEMP_W = 64;
constexpr int SEL_TEMP_H = 28;
constexpr int SEL_HUM_X = 64;
constexpr int SEL_HUM_Y = 164;
constexpr int SEL_HUM_W = 64;
constexpr int SEL_HUM_H = 28;

// ---------------------------------------------------------------------------
// u8g2 virtual display callback — provides geometry info only
// ---------------------------------------------------------------------------

const u8x8_display_info_t U8X8_DISPLAY_INFO_EPD_128X250 = {
    /* chip_enable_level = */ 0,
    /* chip_disable_level = */ 1,
    /* post_chip_enable_wait_ns = */ 0,
    /* pre_chip_disable_wait_ns = */ 0,
    /* reset_pulse_width_ms = */ 0,
    /* post_reset_wait_ms = */ 0,
    /* sda_setup_time_ns = */ 0,
    /* sck_pulse_width_ns = */ 0,
    /* sck_clock_hz = */ 10000000UL,
    /* spi_mode = */ 0,
    /* i2c_bus_clock_100kHz = */ 0,
    /* data_setup_time_ns = */ 0,
    /* write_pulse_width_ns = */ 0,
    /* tile_width = */ 16,
    /* tile_hight = */ 32,
    /* default_x_offset = */ 0,
    /* flipmode_x_offset = */ 0,
    /* pixel_width = */ 128,
    /* pixel_height = */ 250,
};

uint8_t u8x8_d_epd_128x250_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  (void)arg_int;
  (void)arg_ptr;
  switch (msg) {
  case U8X8_MSG_DISPLAY_SETUP_MEMORY:
    u8x8_d_helper_display_setup_memory(u8x8, &U8X8_DISPLAY_INFO_EPD_128X250);
    return 1;
  default:
    return 1;
  }
}

// ---------------------------------------------------------------------------
// Helper predicates
// ---------------------------------------------------------------------------

bool is_home_like(Screen screen) { return screen == Screen::Home || screen == Screen::MainMenu; }

bool is_list_screen(Screen screen) {
  return screen == Screen::Settings || screen == Screen::SettingsChoice ||
         screen == Screen::TagList || screen == Screen::Confirm || screen == Screen::About ||
         screen == Screen::HardwareTest || screen == Screen::PeripheralTest ||
         screen == Screen::GpsTest || screen == Screen::AccelTest;
}

// Any reason-specific shutdown screen.
bool is_shutdown_screen(Screen screen) {
  return screen == Screen::ShutdownUser || screen == Screen::ShutdownDischarge ||
         screen == Screen::ShutdownTemperature || screen == Screen::ShutdownTemperatureLow;
}

// A "navigable" screen is one the user reaches through normal menu interaction.
// Transitions between navigable screens use body-only partial for snappy UX.
// Screens NOT listed here (PairingPasskey, Shutdown*) trigger Fast on transition.
bool is_navigable(Screen screen) { return is_home_like(screen) || is_list_screen(screen); }

// A "menu-navigation" screen is any navigable screen except Home.
// Transitions where either side is menu-navigation always use Partial.
bool is_menu_navigation_screen(Screen screen) {
  return is_navigable(screen) && screen != Screen::Home;
}

bool metric_has_chart(Metric metric) { return metric != Metric::None; }

bool is_float_non_negative(float value) { return value >= 0.0f; }

bool is_temperature_valid(float celsius) { return celsius > MeasuresInvalid::TEMPERATURE; }

bool is_altitude_valid(float altitude_m) { return altitude_m > MeasuresInvalid::ALTITUDE; }

float temp_for_display(float celsius, bool use_fahrenheit) {
  if (!is_temperature_valid(celsius))
    return celsius;
  if (!use_fahrenheit)
    return celsius;
  return celsius * 9.0f / 5.0f + 32.0f;
}

// ---------------------------------------------------------------------------
// Value formatting (inline during rendering)
// ---------------------------------------------------------------------------

int pm25_to_usaqi(float pm25) {
  struct Breakpoint {
    float c_low;
    float c_high;
    int i_low;
    int i_high;
  };
  static const Breakpoint BP[] = {
      {0.0f, 12.0f, 0, 50},       {12.1f, 35.4f, 51, 100},    {35.5f, 55.4f, 101, 150},
      {55.5f, 150.4f, 151, 200},  {150.5f, 250.4f, 201, 300}, {250.5f, 350.4f, 301, 400},
      {350.5f, 500.4f, 401, 500},
  };
  if (!(pm25 >= 0.0f))
    return -1;
  for (const auto &bp : BP) {
    if (pm25 <= bp.c_high) {
      const float ratio = (pm25 - bp.c_low) / (bp.c_high - bp.c_low);
      return bp.i_low + static_cast<int>(lroundf(ratio * static_cast<float>(bp.i_high - bp.i_low)));
    }
  }
  return 500;
}

void format_one_decimal(char *out, size_t out_size, float value) {
  const int scaled = static_cast<int>(lroundf(value * 10.0f));
  const bool neg = scaled < 0;
  const unsigned int abs_scaled = static_cast<unsigned int>(neg ? -scaled : scaled);
  snprintf(out, out_size, "%s%u.%01u", neg ? "-" : "", abs_scaled / 10U, abs_scaled % 10U);
}

void format_pm_value(char *out, size_t out_size, float value, bool use_usaqi) {
  if (!is_float_non_negative(value)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (use_usaqi) {
    const int aqi = pm25_to_usaqi(value);
    if (aqi > 500) {
      snprintf(out, out_size, "500+");
      return;
    }
    snprintf(out, out_size, "%d", aqi);
    return;
  }
  if (value > 999.0f) {
    snprintf(out, out_size, "999+");
    return;
  }
  if (value >= 100.0f) {
    snprintf(out, out_size, "%.0f", value);
    return;
  }
  format_one_decimal(out, out_size, value);
}

void format_co2_value(char *out, size_t out_size, int value) {
  if (value == MeasuresInvalid::CO2) {
    snprintf(out, out_size, "-");
    return;
  }
  if (value > 9999) {
    snprintf(out, out_size, "9999+");
    return;
  }
  snprintf(out, out_size, "%d", value);
}

void format_temperature_value(char *out, size_t out_size, float celsius, bool use_fahrenheit) {
  if (!is_temperature_valid(celsius)) {
    snprintf(out, out_size, "-");
    return;
  }
  const float shown = temp_for_display(celsius, use_fahrenheit);
  snprintf(out, out_size, "%.1f %c", shown, use_fahrenheit ? 'F' : 'C');
}

void format_humidity_value(char *out, size_t out_size, float humidity) {
  if (!is_float_non_negative(humidity)) {
    snprintf(out, out_size, "-");
    return;
  }
  snprintf(out, out_size, "%.0f %%", humidity);
}

// Integer index display for TVOC/NOx grid cells.
void format_int_index_value(char *out, size_t out_size, int value) {
  if (value < 0) {
    snprintf(out, out_size, "-");
    return;
  }
  snprintf(out, out_size, "%d", value);
}

// Float index display for chart min/max stats.
void format_float_index_value(char *out, size_t out_size, float value) {
  if (!is_float_non_negative(value)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (fabsf(value - roundf(value)) < 0.05f) {
    snprintf(out, out_size, "%.0f", value);
    return;
  }
  format_one_decimal(out, out_size, value);
}

void format_pressure_value(char *out, size_t out_size, float pressure_hpa) {
  if (!is_float_non_negative(pressure_hpa)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (pressure_hpa > 9999.0f) {
    snprintf(out, out_size, "9999+ hPa");
    return;
  }
  snprintf(out, out_size, "%.0f hPa", pressure_hpa);
}

void format_altitude_value(char *out, size_t out_size, float altitude_m, bool use_feet) {
  constexpr float METERS_TO_FEET = 3.28084f;
  constexpr float MAX_DISPLAY_ALTITUDE_METERS = 9999.0f;

  if (!is_altitude_valid(altitude_m)) {
    snprintf(out, out_size, "-");
    return;
  }

  if (use_feet) {
    const long altitude_ft = lroundf(altitude_m * METERS_TO_FEET);
    snprintf(out, out_size, "%ld ft", altitude_ft);
    return;
  }

  if (altitude_m > MAX_DISPLAY_ALTITUDE_METERS) {
    snprintf(out, out_size, "9999+ m");
    return;
  }

  const long rounded_altitude_m = lroundf(altitude_m);
  snprintf(out, out_size, "%ld m", rounded_altitude_m);
}

void format_chart_stat(char *out, size_t out_size, Metric metric, float value, bool use_fahrenheit,
                       bool pm_use_usaqi) {
  switch (metric) {
  case Metric::Pm25:
    format_pm_value(out, out_size, value, pm_use_usaqi);
    break;
  case Metric::Co2:
    format_co2_value(out, out_size, static_cast<int>(lroundf(value)));
    break;
  case Metric::Temp:
    format_temperature_value(out, out_size, value, use_fahrenheit);
    break;
  case Metric::Humidity:
    format_humidity_value(out, out_size, value);
    break;
  case Metric::Tvoc:
  case Metric::Nox:
    format_float_index_value(out, out_size, value);
    break;
  case Metric::None:
  default:
    snprintf(out, out_size, "-");
    break;
  }
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

void draw_text(u8g2_t *u, int x, int baseline_y, const char *text) {
  if (text == nullptr)
    return;
  u8g2_DrawStr(u, static_cast<u8g2_uint_t>(x), static_cast<u8g2_uint_t>(baseline_y), text);
}

void draw_centered_text(u8g2_t *u, int center_x, int baseline_y, const char *text) {
  if (text == nullptr)
    return;
  const int w = static_cast<int>(u8g2_GetStrWidth(u, text));
  draw_text(u, center_x - w / 2, baseline_y, text);
}

void draw_logo(u8g2_t *u, int y, int h) {
  u8g2_SetFont(u, u8g2_font_6x10_tr);
  draw_centered_text(u, CONTENT_W / 2, y + h / 2 + 4, "AirGradient");
}

// Wi-Fi glyphs from u8g2_font_siji_t_6x10
constexpr uint16_t WIFI_GLYPH_CONNECTED = 57419;    // 0xE04B
constexpr uint16_t WIFI_GLYPH_DISCONNECTED = 57879; // 0xE217

// Battery / power glyphs from u8g2_font_siji_t_6x10
constexpr uint16_t PLUG_GLYPH = 57410;
constexpr uint16_t BATTERY_GLYPH_CHARGING = 57914;
constexpr uint16_t BATTERY_GLYPH_LVL_0_10 = 57932;
constexpr uint16_t BATTERY_GLYPH_LVL_10_30 = 57933;
constexpr uint16_t BATTERY_GLYPH_LVL_30_50 = 57935;
constexpr uint16_t BATTERY_GLYPH_LVL_50_70 = 57937;
constexpr uint16_t BATTERY_GLYPH_LVL_70_95 = 57939;
constexpr uint16_t BATTERY_GLYPH_LVL_95_100 = 57940;

uint16_t battery_glyph(bool is_charging, uint8_t pct) {
  if (is_charging)
    return BATTERY_GLYPH_CHARGING;
  const uint8_t v = (pct > 100U) ? 100U : pct;
  if (v <= 10U)
    return BATTERY_GLYPH_LVL_0_10;
  if (v <= 30U)
    return BATTERY_GLYPH_LVL_10_30;
  if (v <= 50U)
    return BATTERY_GLYPH_LVL_30_50;
  if (v <= 70U)
    return BATTERY_GLYPH_LVL_50_70;
  if (v <= 95U)
    return BATTERY_GLYPH_LVL_70_95;
  return BATTERY_GLYPH_LVL_95_100;
}

// Draw a single grid cell (label in helvR08, value in helvB08).
// For temperature, caller should use DrawUTF8 directly for the degree symbol.
void draw_cell(u8g2_t *u, int index, const char *label, const char *value, bool use_utf8) {
  u8g2_SetFont(u, u8g2_font_helvR08_tr);
  draw_text(u, GRID_LABEL_X[index], GRID_LABEL_Y[index], label);
  u8g2_SetFont(u, u8g2_font_helvB08_tf);
  if (use_utf8) {
    u8g2_DrawUTF8(u, static_cast<u8g2_uint_t>(GRID_VALUE_X[index]),
                  static_cast<u8g2_uint_t>(GRID_VALUE_Y[index]), value);
  } else {
    draw_text(u, GRID_VALUE_X[index], GRID_VALUE_Y[index], value);
  }
}

// ---------------------------------------------------------------------------
// Shutdown helpers — pure u8g2-primitive icons (no XBM asset).
// ---------------------------------------------------------------------------

void draw_shutdown_line_2px(u8g2_t *u, int x0, int y0, int x1, int y1) {
  u8g2_DrawLine(u, x0, y0, x1, y1);
  u8g2_DrawLine(u, x0 + 1, y0, x1 + 1, y1);
}

void draw_shutdown_power_icon(u8g2_t *u, int center_x, int center_y) {
  constexpr int RADIUS = 23;
  constexpr int STROKE_W = 3;
  // Filled disc minus a smaller filled disc avoids diagonal gaps from
  // stacking concentric u8g2 circles at adjacent radii.
  u8g2_DrawDisc(u, center_x, center_y, RADIUS, U8G2_DRAW_ALL);
  u8g2_SetDrawColor(u, 1);
  u8g2_DrawDisc(u, center_x, center_y, RADIUS - STROKE_W, U8G2_DRAW_ALL);
  u8g2_SetDrawColor(u, 0);
  u8g2_DrawBox(u, center_x - 1, center_y - RADIUS - 4, STROKE_W, RADIUS + 5);
}

void draw_shutdown_battery_low_icon(u8g2_t *u, int center_x, int center_y) {
  constexpr int BODY_W = 52;
  constexpr int BODY_H = 26;
  constexpr int STROKE_W = 2;
  const int x = center_x - BODY_W / 2;
  const int y = center_y - BODY_H / 2;

  u8g2_DrawBox(u, x, y, BODY_W, STROKE_W);
  u8g2_DrawBox(u, x, y + BODY_H - STROKE_W, BODY_W, STROKE_W);
  u8g2_DrawBox(u, x, y, STROKE_W, BODY_H);
  u8g2_DrawBox(u, x + BODY_W - STROKE_W, y, STROKE_W, BODY_H);
  u8g2_DrawBox(u, x + BODY_W, y + 7, 4, 12);
  u8g2_DrawBox(u, center_x - 1, center_y - 9, 3, 12);
  u8g2_DrawBox(u, center_x - 1, center_y + 7, 3, 3);
}

void draw_shutdown_battery_temperature_icon(u8g2_t *u, int center_x, int center_y) {
  constexpr int BULB_R = 8;
  constexpr int TUBE_H = 34;
  constexpr int TUBE_W = 8;
  constexpr int ICON_H = 54;
  constexpr int STROKE_W = 2;

  const int icon_y = center_y - ICON_H / 2;
  const int tube_x = center_x - TUBE_W / 2;
  const int tube_top_y = icon_y + 3;
  const int bulb_y = tube_top_y + TUBE_H;

  u8g2_DrawBox(u, tube_x, tube_top_y, TUBE_W, STROKE_W);
  u8g2_DrawBox(u, tube_x, tube_top_y, STROKE_W, TUBE_H);
  u8g2_DrawBox(u, tube_x + TUBE_W - STROKE_W, tube_top_y, STROKE_W, TUBE_H);
  u8g2_DrawFilledEllipse(u, tube_x + TUBE_W / 2, bulb_y, BULB_R, BULB_R, U8G2_DRAW_ALL);
  u8g2_DrawBox(u, tube_x + 2, tube_top_y + 17, 4, 17);
}

void draw_shutdown_battery_hot_icon(u8g2_t *u, int center_x, int center_y) {
  constexpr int ICON_W = 56;
  constexpr int ICON_H = 54;
  const int icon_x = center_x - ICON_W / 2;
  const int icon_y = center_y - ICON_H / 2;

  draw_shutdown_battery_temperature_icon(u, icon_x + 14, center_y);
  draw_shutdown_line_2px(u, icon_x + 41, icon_y + 5, icon_x + 33, icon_y + 15);
  draw_shutdown_line_2px(u, icon_x + 33, icon_y + 15, icon_x + 43, icon_y + 24);
  draw_shutdown_line_2px(u, icon_x + 47, icon_y + 5, icon_x + 39, icon_y + 15);
  draw_shutdown_line_2px(u, icon_x + 39, icon_y + 15, icon_x + 49, icon_y + 24);
}

void draw_shutdown_battery_cold_icon(u8g2_t *u, int center_x, int center_y) {
  constexpr int ICON_W = 56;
  constexpr int ICON_H = 54;
  const int icon_x = center_x - ICON_W / 2;
  const int icon_y = center_y - ICON_H / 2;
  const int snowflake_x = icon_x + 41;
  const int snowflake_y = icon_y + 16;

  draw_shutdown_battery_temperature_icon(u, icon_x + 14, center_y);
  draw_shutdown_line_2px(u, snowflake_x - 10, snowflake_y, snowflake_x + 10, snowflake_y);
  draw_shutdown_line_2px(u, snowflake_x, snowflake_y - 10, snowflake_x, snowflake_y + 10);
  draw_shutdown_line_2px(u, snowflake_x - 7, snowflake_y - 7, snowflake_x + 7, snowflake_y + 7);
  draw_shutdown_line_2px(u, snowflake_x + 7, snowflake_y - 7, snowflake_x - 7, snowflake_y + 7);
}

// When title_l2 is empty, lift action/detail by the L1->L2 line-height
// so the title->action gap stays constant.
void draw_shutdown_text(u8g2_t *u, const char *title_l1, const char *title_l2, const char *action,
                        const char *detail) {
  const bool has_l2 = (title_l2 != nullptr && title_l2[0] != '\0');
  constexpr int LIFT_PX = SHUTDOWN_TITLE_L2_BASELINE_Y - SHUTDOWN_TITLE_L1_BASELINE_Y;
  const int action_y = has_l2 ? SHUTDOWN_ACTION_BASELINE_Y : (SHUTDOWN_ACTION_BASELINE_Y - LIFT_PX);
  const int detail_y = has_l2 ? SHUTDOWN_DETAIL_BASELINE_Y : (SHUTDOWN_DETAIL_BASELINE_Y - LIFT_PX);

  u8g2_SetFont(u, u8g2_font_helvB14_tf);
  draw_centered_text(u, SCREEN_W / 2, SHUTDOWN_TITLE_L1_BASELINE_Y, title_l1);
  if (has_l2) {
    draw_centered_text(u, SCREEN_W / 2, SHUTDOWN_TITLE_L2_BASELINE_Y, title_l2);
  }
  u8g2_SetFont(u, u8g2_font_helvB08_tf);
  draw_centered_text(u, (SCREEN_W / 2) + 1, action_y, action);
  if (detail != nullptr && detail[0] != '\0') {
    u8g2_SetFont(u, u8g2_font_helvB08_tf);
    draw_centered_text(u, SCREEN_W / 2, detail_y, detail);
  }
}

// ---------------------------------------------------------------------------
// Full-canvas session-screen helpers
// ---------------------------------------------------------------------------

inline bool is_session_screen(Screen s) {
  return s == Screen::Info || s == Screen::Provisioning || s == Screen::ProvisioningConfirm ||
         s == Screen::GettingStarted;
}

// Non-capturing StrWidthFn that forwards to u8g2_GetStrWidth.  The text
// passed by compute_wrapped_lines() is not NUL-terminated, so copy into a
// small stack scratch buffer first.
int u8g2_str_width_fn(const char *text, size_t len, void *ctx) {
  auto *u = static_cast<u8g2_t *>(ctx);
  char buf[64];
  size_t copy_len = (len < sizeof(buf)) ? len : sizeof(buf) - 1;
  if (text != nullptr && copy_len > 0) {
    memcpy(buf, text, copy_len);
  }
  buf[copy_len] = '\0';
  return static_cast<int>(u8g2_GetStrWidth(u, buf));
}

// Product-local QR rendering parameters; matrix data is shared via
// AirgradientProvisioning::QrCode.
constexpr int QR_MODULE_PX = 2;
constexpr int QR_QUIET_MODULES = 4;

// Draw `qr` centered at `center_x` with its quiet-zone top edge at
// `y_top`.  `module_px` sets the per-module pixel size (Getting Started
// uses a larger value for a chunkier, easier-to-scan code).  No-op on
// null or empty matrix.
void draw_provisioning_qr(u8g2_t *u, const AirgradientProvisioning::QrCode *qr, int center_x,
                          int y_top, int module_px = QR_MODULE_PX) {
  if (qr == nullptr) {
    return;
  }
  const int modules = qr->size();
  if (modules <= 0) {
    return;
  }
  const int total_px = (modules + QR_QUIET_MODULES * 2) * module_px;
  const int qr_x = center_x - total_px / 2 + QR_QUIET_MODULES * module_px;
  const int qr_y = y_top + QR_QUIET_MODULES * module_px;
  for (int row = 0; row < modules; ++row) {
    for (int col = 0; col < modules; ++col) {
      if (qr->module_on(col, row)) {
        u8g2_DrawBox(u, qr_x + col * module_px, qr_y + row * module_px, module_px, module_px);
      }
    }
  }
}

void draw_provisioning_separator(u8g2_t *u, int y) { u8g2_DrawHLine(u, 10, y, SCREEN_W - 20); }

void draw_provisioning_action_row(u8g2_t *u, const char *text, int y, bool selected) {
  constexpr int ROW_H = 18;
  constexpr int ROW_X = 4;
  constexpr int ROW_W = SCREEN_W - 8;
  if (selected) {
    u8g2_DrawBox(u, ROW_X, y, ROW_W, ROW_H);
    u8g2_SetDrawColor(u, 1);
  }
  u8g2_SetFont(u, u8g2_font_6x10_tr);
  draw_centered_text(u, SCREEN_W / 2, y + 12, text);
  if (selected) {
    u8g2_SetDrawColor(u, 0);
  }
}

// Draw list/menu rows with selection highlight.
void draw_list_rows(u8g2_t *u, const DisplayValues &v, bool full_screen) {
  const int row_base_y = full_screen ? 20 : 166;
  constexpr int ROW_RECT_H = 20;
  constexpr int ROW_STEP = 22;
  const int text_baseline = full_screen ? 33 : 179;
  // Extra offset for content rows (index >= 2) to clear the separator line.
  const int content_pad = (full_screen && v.show_separator_after_back) ? 2 : 0;

  for (uint8_t i = 0; i < v.row_count && i < MAX_LIST_ROWS; ++i) {
    const int pad = (i >= 2) ? content_pad : 0;
    const int row_y = row_base_y + ROW_STEP * static_cast<int>(i) + pad;
    const bool selected = (i == v.selected_row) && !v.rows[i].disabled;
    if (selected) {
      u8g2_DrawBox(u, 0, row_y, SCREEN_W, ROW_RECT_H);
      u8g2_SetDrawColor(u, 1);
    }
    u8g2_SetFont(u, u8g2_font_6x10_tr);
    draw_text(u, 10, text_baseline + ROW_STEP * static_cast<int>(i) + pad, v.rows[i].text);
    if (selected) {
      u8g2_SetDrawColor(u, 0);
    }
  }

  if (v.show_separator_after_back) {
    u8g2_DrawHLine(u, 0, 63, SCREEN_W);
  }
}

} // anonymous namespace

// ===========================================================================
// DisplayService — public methods
// ===========================================================================

DisplayService::DisplayService(const Config &config)
    : _config(config), _u8g2{}, _render_buf{}, _prev_values{}, _diff_count(0),
      _pending_mode(RefreshMode::Full), _task_handle(nullptr), _running(false),
      _worker_busy(false) {}

bool DisplayService::init(const DisplayValues &initial, bool defer_refresh) {
  esp_err_t err = driver_init(_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "driver init failed: %s", esp_err_to_name(err));
    return false;
  }

  // u8g2: software renderer into RAM only — no hardware callbacks
  u8g2_SetupDisplay(&_u8g2, u8x8_d_epd_128x250_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
  u8g2_SetupBuffer(&_u8g2, _render_buf, BUF_TILE_HEIGHT, u8g2_ll_hvline_horizontal_right_lsb,
                   U8G2_MIRROR);
  u8g2_SetFontMode(&_u8g2, 1); // Transparent font mode

  _prev_values = initial;
  _render_frame(initial);

  if (!defer_refresh) {
    // Synchronous initial full refresh (worker not yet started).
    // Default behavior: backward compatible with run_full_boot() and run_fast_path().
    err = driver_bus_acquire();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "bus acquire failed for init: %s", esp_err_to_name(err));
      return false;
    }
    err = driver_hw_init_full();
    if (err == ESP_OK) {
      err = driver_set_basemap(_render_buf);
    }
    driver_bus_release();

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "initial refresh failed: %s", esp_err_to_name(err));
      return false;
    }
    _diff_count = 0;
  } else {
    // Deferred refresh: render is already in _render_buf. Reserve it for the
    // worker, then signal the initial full refresh. Returns in ~10 ms.
    // The worker acquires the SPI bus for ~3 s; any other SPI device (NAND)
    // that tries to transmit will block until the worker releases the bus.
    _pending_mode = RefreshMode::Full;
    _worker_busy.store(true); // cleared by worker when refresh completes
    // The _prev_values header now reflects the snapshot-based initial frame,
    // which may differ from the live runtime state the orchestrator produces.
    // Skip the header-change penalty on the first update()
    _defer_header_check = true;
  }

  // Start async worker task.
  _running.store(true);
  const bool created = RTOS::task_create(
      _worker_entry, "disp_worker", static_cast<uint32_t>(_config.task_stack_size), this,
      static_cast<uint32_t>(_config.task_priority), &_task_handle);
  if (!created) {
    ESP_LOGE(TAG, "failed to create worker task");
    _running.store(false);
    _worker_busy.store(false);
    _task_handle = nullptr;
    return false;
  }

  if (defer_refresh) {
    // Kick the worker to process the initial full refresh immediately.
    RTOS::task_notify_give(_task_handle);
  }

  return true;
}

bool DisplayService::update(const DisplayValues &values, bool wait) {
  if (_task_handle == nullptr || !_running.load())
    return false;

  if (!_claim_framebuffer(wait)) {
    return false;
  }

  const bool same_list_screen =
      is_list_screen(_prev_values.screen) && _prev_values.screen == values.screen;
  const bool header_changed = _is_header_changed(values, _prev_values) && !_defer_header_check;
  _defer_header_check = false;
  const bool both_navigable = is_navigable(_prev_values.screen) && is_navigable(values.screen);
  const bool can_partial = (both_navigable && !header_changed) || same_list_screen;

  // Crossing the full-canvas session boundary forces a full refresh so no
  // ghosting from the prior layout remains. All in-session transitions use
  // partial refresh over y=0..249 so disjoint layouts clear cleanly without
  // the Full waveform's ~3 s flash.
  const bool prev_in_session = is_session_screen(_prev_values.screen);
  const bool next_in_session = is_session_screen(values.screen);
  const bool crossing_session_boundary = prev_in_session != next_in_session;

  _render_frame(values);

  const bool entering_system_screen =
      is_shutdown_screen(values.screen) || values.screen == Screen::PairingPasskey;
  const bool menu_navigation =
      !entering_system_screen &&
      (is_menu_navigation_screen(_prev_values.screen) || is_menu_navigation_screen(values.screen));

  if (crossing_session_boundary) {
    // Full refresh on the session boundary.  Reset the partial-op counter
    // so the next session's partials start with a fresh budget.
    _pending_mode = RefreshMode::Full;
    _diff_count = 0;
    _menu_exited = false;
  } else if (prev_in_session && next_in_session) {
    // Intra-session transitions (Info text update, Provisioning status
    // change, Provisioning <-> ProvisioningConfirm, No <-> Yes) are always
    // full-canvas partial refreshes.  The partial-op counter is NOT
    // consulted inside the session.
    _pending_mode = RefreshMode::Partial;
    _menu_exited = false;
  } else if (menu_navigation) {
    _pending_mode = RefreshMode::Partial;
    _menu_exited = true;
  } else if (_diff_count >= _config.max_partial_ops) {
    _pending_mode = RefreshMode::Full;
  } else if (_menu_exited) {
    _pending_mode = RefreshMode::Fast;
  } else if (can_partial) {
    _pending_mode = RefreshMode::Partial;
  } else {
    _pending_mode = RefreshMode::Fast;
  }

  // Clear flag after any full-screen refresh (Full or Fast rewrites everything).
  if (_pending_mode != RefreshMode::Partial) {
    _menu_exited = false;
  }

  _prev_values = values;

  RTOS::task_notify_give(_task_handle);
  return true;
}

void DisplayService::update_sync(const DisplayValues &values) {
  (void)_claim_framebuffer(true);
  _render_frame(values);

  esp_err_t err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for sync update: %s", esp_err_to_name(err));
    _release_framebuffer();
    return;
  }
  err = driver_hw_init_full();
  if (err == ESP_OK) {
    err = driver_set_basemap(_render_buf);
  }
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sync update failed: %s", esp_err_to_name(err));
  }
  _diff_count = 0;
  _prev_values = values;
  _release_framebuffer();
}

void DisplayService::flush() {
  while (_worker_busy.load()) {
    RTOS::delay_ms(WORKER_POLL_MS);
  }
}

void DisplayService::clear() {
  (void)_claim_framebuffer(true);
  memset(_render_buf, 0xFF, sizeof(_render_buf));

  esp_err_t err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for clear: %s", esp_err_to_name(err));
    _release_framebuffer();
    return;
  }
  err = driver_hw_init_full();
  if (err == ESP_OK) {
    err = driver_set_basemap(_render_buf);
  }
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "clear failed: %s", esp_err_to_name(err));
  }
  _diff_count = 0;
  _release_framebuffer();
}

void DisplayService::deep_sleep() {
  esp_err_t err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for deep sleep: %s", esp_err_to_name(err));
    return;
  }
  err = driver_deep_sleep();
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "deep sleep failed: %s", esp_err_to_name(err));
  }
}

void DisplayService::stop() {
  if (_task_handle == nullptr)
    return;

  // Complete the pending frame before asking the idle worker to exit.
  flush();
  _running.store(false);
  RTOS::task_notify_give(_task_handle); // Wake worker so it can exit
  RTOS::delay_ms(50);                   // Let worker task fully exit
  _task_handle = nullptr;
}

bool DisplayService::_claim_framebuffer(bool wait) {
  while (true) {
    bool expected = false;
    if (_worker_busy.compare_exchange_strong(expected, true)) {
      return true;
    }
    if (!wait) {
      return false;
    }
    RTOS::delay_ms(WORKER_POLL_MS);
  }
}

void DisplayService::_release_framebuffer() { _worker_busy.store(false); }

// ===========================================================================
// DisplayService — rendering pipeline
// ===========================================================================

void DisplayService::_render_frame(const DisplayValues &v) {
  memset(_render_buf, 0xFF, sizeof(_render_buf));
  u8g2_SetDrawColor(&_u8g2, 0); // Black on white

  if (is_shutdown_screen(v.screen)) {
    _draw_shutdown(v.screen);
    return;
  }
  if (v.screen == Screen::PairingPasskey) {
    _draw_pairing_passkey(v);
    return;
  }

  // Factory learning dashboard owns the full canvas — no status bar.
  if (v.show_fg_dashboard) {
    _draw_fg_learning_dashboard(v);
    return;
  }

  // Session screens own the full canvas — no status bar, no snackbar.
  if (v.screen == Screen::Info) {
    _draw_info(v);
    return;
  }
  if (v.screen == Screen::Provisioning) {
    _draw_provisioning(v);
    return;
  }
  if (v.screen == Screen::ProvisioningConfirm) {
    _draw_provisioning_confirm(v);
    return;
  }
  if (v.screen == Screen::GettingStarted) {
    _draw_getting_started(v);
    return;
  }

  _draw_status_bar(v);

  switch (v.screen) {
  case Screen::Home:
    _draw_home(v);
    break;
  case Screen::MainMenu:
    _draw_home(v);
    _draw_menu_overlay(v);
    break;
  case Screen::Settings:
  case Screen::SettingsChoice:
  case Screen::TagList:
  case Screen::Confirm:
  case Screen::About:
  case Screen::HardwareTest:
  case Screen::PeripheralTest:
  case Screen::GpsTest:
  case Screen::AccelTest:
    _draw_full_screen_list(v);
    break;
  case Screen::ShutdownUser:
  case Screen::ShutdownDischarge:
  case Screen::ShutdownTemperature:
  case Screen::ShutdownTemperatureLow:
  case Screen::PairingPasskey:
  case Screen::Info:
  case Screen::Provisioning:
  case Screen::ProvisioningConfirm:
  case Screen::GettingStarted:
    // Already handled above before the status-bar draw.
    break;
  // FG learning dashboard renderer is wired in a later phase (FgLearningRunner
  // populates DisplayValues directly). No-op until then.
  case Screen::FgLearnCharging:
  case Screen::FgLearnResting:
  case Screen::FgLearnUnplug:
  case Screen::DischargeComplete:
  case Screen::FgLearnVerifying:
  case Screen::FgLearnComplete:
  case Screen::FgLearnFailed:
    break;
  }

  _draw_snackbar(v);
}

bool DisplayService::_is_header_changed(const DisplayValues &a, const DisplayValues &b) const {
  return a.battery_pct != b.battery_pct || a.is_battery_charging != b.is_battery_charging ||
         a.is_plugged_in != b.is_plugged_in || a.locked != b.locked ||
         a.ble_enabled != b.ble_enabled || a.ble_connected != b.ble_connected ||
         a.wifi_enabled != b.wifi_enabled || a.wifi_connected != b.wifi_connected ||
         a.gps_enabled != b.gps_enabled || a.gps_fix != b.gps_fix ||
         a.tracking_active != b.tracking_active;
}

// ===========================================================================
// DisplayService — screen draw methods
// ===========================================================================

void DisplayService::_draw_status_bar(const DisplayValues &v) {
  int cursor = LOCK_X;

  // 1. Lock or Unlock icon (left-pinned, always drawn)
  if (v.locked) {
    u8g2_SetFont(&_u8g2, u8g2_font_open_iconic_all_1x_t);
    u8g2_DrawGlyph(&_u8g2, static_cast<u8g2_uint_t>(cursor), STATUS_BASELINE_Y, 0xCA);
  } else {
    u8g2_SetFont(&_u8g2, u8g2_font_open_iconic_thing_1x_t);
    u8g2_DrawGlyph(&_u8g2, static_cast<u8g2_uint_t>(cursor), STATUS_BASELINE_Y, 0x44);
  }
  cursor += 10 + ICON_GAP;

  // 2. Dynamic flow icons: WiFi (connected vs disconnected), Link/Unlink, GPS
  if (v.wifi_enabled) {
    u8g2_SetFont(&_u8g2, u8g2_font_siji_t_6x10);
    u8g2_DrawGlyph(&_u8g2, static_cast<u8g2_uint_t>(cursor), STATUS_BASELINE_Y,
                   v.wifi_connected ? WIFI_GLYPH_CONNECTED : WIFI_GLYPH_DISCONNECTED);
    cursor += 10 + ICON_GAP;
  }

  if (v.ble_enabled) {
    const int x = cursor;
    if (v.ble_connected) {
      // Link: two frames + connecting bar
      u8g2_DrawFrame(&_u8g2, x, LINK_CY, 5, 5);
      u8g2_DrawFrame(&_u8g2, x + 6, LINK_CY, 5, 5);
      u8g2_DrawLine(&_u8g2, x + 3, LINK_CY + 2, x + 7, LINK_CY + 2);
    } else {
      // Unlink: two frames + broken fragments
      u8g2_DrawFrame(&_u8g2, x, LINK_CY, 5, 5);
      u8g2_DrawFrame(&_u8g2, x + 6, LINK_CY, 5, 5);
      u8g2_DrawLine(&_u8g2, x + 4, LINK_CY + 4, x + 3, LINK_CY + 4);
      u8g2_DrawLine(&_u8g2, x + 6, LINK_CY + 4, x + 7, LINK_CY + 4);
      u8g2_DrawLine(&_u8g2, x + 4, LINK_CY - 2, x + 3, LINK_CY - 3);
      u8g2_DrawLine(&_u8g2, x + 6, LINK_CY - 2, x + 7, LINK_CY - 3);
      u8g2_DrawLine(&_u8g2, x + 4, LINK_CY + 6, x + 3, LINK_CY + 7);
      u8g2_DrawLine(&_u8g2, x + 6, LINK_CY + 6, x + 7, LINK_CY + 7);
    }
    cursor += 12 + ICON_GAP;
  }

  if (v.gps_fix) {
    u8g2_SetFont(&_u8g2, u8g2_font_siji_t_6x10);
    u8g2_DrawGlyph(&_u8g2, static_cast<u8g2_uint_t>(cursor), STATUS_BASELINE_Y, 0xE0A1);
    cursor += 10 + ICON_GAP;
  }

  // 3. Tracking dot (right-pinned; shifts left when plug icon is present)
  if (v.tracking_active) {
    const bool plug_visible = v.is_plugged_in && !v.is_battery_charging;
    const int track_x = plug_visible ? (BATTERY_X - 10 - ICON_GAP - ICON_GAP - 2) : TRACKING_X;
    u8g2_DrawFilledEllipse(&_u8g2, track_x, ELLIPSE_CY, 2, 2, U8G2_DRAW_ALL);
  }

  // 4. Battery + optional plug icon (right-pinned, fixed position)
  if (v.battery_pct != 0xFFu || v.is_battery_charging) {
    u8g2_SetFont(&_u8g2, u8g2_font_siji_t_6x10);

    // Show plug icon left of battery when plugged in but not actively
    // charging (e.g. full-charge pause, charge termination done).
    if (v.is_plugged_in && !v.is_battery_charging) {
      u8g2_DrawGlyph(&_u8g2, BATTERY_X - 10 - ICON_GAP, STATUS_BASELINE_Y + 1, PLUG_GLYPH);
    }

    u8g2_DrawGlyph(&_u8g2, BATTERY_X, STATUS_BASELINE_Y,
                   battery_glyph(v.is_battery_charging, v.battery_pct));
  }

  // Header divider
  u8g2_DrawHLine(&_u8g2, 0, STATUS_DIVIDER_Y, SCREEN_W);
}

void DisplayService::_draw_home(const DisplayValues &v) {
  if (v.display_off) {
    draw_logo(&_u8g2, DISPLAY_OFF_LOGO_Y, DISPLAY_OFF_LOGO_H);
    return;
  }

  const bool chart_visible = metric_has_chart(v.active_metric);
  const bool pm_selected = (v.active_metric == Metric::Pm25);
  const bool co2_selected = (v.active_metric == Metric::Co2);
  const bool temp_selected = (v.active_metric == Metric::Temp);
  const bool hum_selected = (v.active_metric == Metric::Humidity);

  // --- Grid dividers (full 128px width) ---
  // 1st divider: always 2px thick
  u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_0_Y, SCREEN_W);
  u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_0_Y + 1, SCREEN_W);
  u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_1_Y, SCREEN_W);
  // 3rd divider: 2px thick when chart visible, 1px otherwise
  u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_2_Y, SCREEN_W);
  if (chart_visible) {
    u8g2_DrawHLine(&_u8g2, 0, GRID_DIVIDER_2_Y + 1, SCREEN_W);
  }

  // Vertical divider
  if (chart_visible) {
    // Only rows 1–2 when chart active
    u8g2_DrawVLine(&_u8g2, GRID_DIVIDER_X, GRID_DIVIDER_0_Y, GRID_DIVIDER_2_Y - GRID_DIVIDER_0_Y);
  } else {
    // All 3 rows
    u8g2_DrawVLine(&_u8g2, GRID_DIVIDER_X, GRID_DIVIDER_0_Y, 249 - GRID_DIVIDER_0_Y);
  }

  // --- Selection rectangles (filled black with white text) ---
  if (pm_selected) {
    u8g2_DrawBox(&_u8g2, 0, PM_BLOCK_Y, SCREEN_W, PM_BLOCK_H);
  }
  if (co2_selected) {
    u8g2_DrawBox(&_u8g2, 0, CO2_BLOCK_Y, SCREEN_W, CO2_BLOCK_H);
  }
  if (temp_selected) {
    u8g2_DrawBox(&_u8g2, SEL_TEMP_X, SEL_TEMP_Y, SEL_TEMP_W, SEL_TEMP_H);
  }
  if (hum_selected) {
    u8g2_DrawBox(&_u8g2, SEL_HUM_X, SEL_HUM_Y, SEL_HUM_W, SEL_HUM_H);
  }

  char buf[24];

  // --- PM2.5 hero section ---
  if (pm_selected)
    u8g2_SetDrawColor(&_u8g2, 1);

  // Dual-font label: "PM2.5" in Logisoso16 + "(ug/m3)" or "(USAQI)" in HelvR12
  {
    const char *pm_name = "PM2.5";
    const char *pm_unit = v.pm_use_usaqi ? "(USAQI)" : "(ug/m3)";

    u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
    const int name_w = static_cast<int>(u8g2_GetStrWidth(&_u8g2, pm_name));
    u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
    const int unit_w = static_cast<int>(u8g2_GetStrWidth(&_u8g2, pm_unit));
    const int total_w = name_w + unit_w;
    const int label_x = (SCREEN_W - total_w) / 2;

    u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
    draw_text(&_u8g2, label_x, PM_LABEL_BASELINE_Y, pm_name);
    u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
    draw_text(&_u8g2, label_x + name_w, PM_LABEL_BASELINE_Y, pm_unit);
  }

  // PM2.5 value — centered at x=64
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso32_tr);
  format_pm_value(buf, sizeof(buf), v.pm25_ugm3, v.pm_use_usaqi);
  draw_centered_text(&_u8g2, SCREEN_W / 2, PM_VALUE_BASELINE_Y, buf);

  if (pm_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  // --- CO2 hero section ---
  if (co2_selected)
    u8g2_SetDrawColor(&_u8g2, 1);

  // Dual-font label: "CO2" in Logisoso16 + "(ppm)" in HelvR12
  {
    const char *co2_name = "CO2";
    const char *co2_unit = "(ppm)";

    u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
    const int name_w = static_cast<int>(u8g2_GetStrWidth(&_u8g2, co2_name));
    u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
    const int unit_w = static_cast<int>(u8g2_GetStrWidth(&_u8g2, co2_unit));
    const int total_w = name_w + unit_w;
    const int label_x = (SCREEN_W - total_w) / 2;

    u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
    draw_text(&_u8g2, label_x, CO2_LABEL_NAME_BASELINE_Y, co2_name);
    u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
    draw_text(&_u8g2, label_x + name_w, CO2_LABEL_UNIT_BASELINE_Y, co2_unit);
  }

  // CO2 value — centered at x=64
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso32_tr);
  format_co2_value(buf, sizeof(buf), v.co2_ppm);
  draw_centered_text(&_u8g2, SCREEN_W / 2, CO2_VALUE_BASELINE_Y, buf);

  if (co2_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  // --- Grid cells ---
  char temp_buf[24];
  char hum_buf[24];

  // Temperature grid cell uses DrawUTF8 for the degree symbol.
  // format_temperature_value is preserved for chart stats; here we format
  // with the UTF-8 degree sign for display.
  if (!is_temperature_valid(v.temperature_c)) {
    snprintf(temp_buf, sizeof(temp_buf), "-");
  } else {
    const float shown = temp_for_display(v.temperature_c, v.use_fahrenheit);
    format_one_decimal(temp_buf, sizeof(temp_buf), shown);
    const size_t len = strlen(temp_buf);
    snprintf(temp_buf + len, sizeof(temp_buf) - len, " \xC2\xB0%c", v.use_fahrenheit ? 'F' : 'C');
  }
  format_humidity_value(hum_buf, sizeof(hum_buf), v.humidity_pct);

  // Row 1: Temp / Humidity (always visible)
  if (temp_selected)
    u8g2_SetDrawColor(&_u8g2, 1);
  draw_cell(&_u8g2, 0, "Temperature", temp_buf, true);
  if (temp_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  if (hum_selected)
    u8g2_SetDrawColor(&_u8g2, 1);
  draw_cell(&_u8g2, 1, "Humidity", hum_buf, false);
  if (hum_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  // Row 2: TVOC/NOx or Min/Max (conditional)
  if (chart_visible) {
    char min_buf[24];
    char max_buf[24];
    format_chart_stat(min_buf, sizeof(min_buf), v.active_metric, v.chart_min, v.use_fahrenheit,
                      v.pm_use_usaqi);
    format_chart_stat(max_buf, sizeof(max_buf), v.active_metric, v.chart_max, v.use_fahrenheit,
                      v.pm_use_usaqi);
    draw_cell(&_u8g2, 2, "Min", min_buf, false);
    draw_cell(&_u8g2, 3, "Max", max_buf, false);
  } else {
    char tvoc_buf[24];
    char nox_buf[24];
    format_int_index_value(tvoc_buf, sizeof(tvoc_buf), v.tvoc_index);
    format_int_index_value(nox_buf, sizeof(nox_buf), v.nox_index);
    draw_cell(&_u8g2, 2, "TVOC", tvoc_buf, false);
    draw_cell(&_u8g2, 3, "NOx", nox_buf, false);
  }

  // Row 3: Pressure/Altitude or Chart (conditional)
  if (chart_visible) {
    _draw_chart(v);
  } else {
    char pressure_buf[24];
    char altitude_buf[24];
    format_pressure_value(pressure_buf, sizeof(pressure_buf), v.pressure_hpa);
    format_altitude_value(altitude_buf, sizeof(altitude_buf), v.altitude_m, v.use_feet);
    draw_cell(&_u8g2, 4, "Pressure", pressure_buf, false);
    draw_cell(&_u8g2, 5, "Altitude", altitude_buf, false);
  }
}

void DisplayService::_draw_menu_overlay(const DisplayValues &v) {
  // Erase below the 2px-thick grid divider at MAIN_MENU_BG_Y, preserving
  // the divider pair (y, y+1) as the top border of the menu overlay.
  constexpr int erase_y = MAIN_MENU_BG_Y + 2;
  constexpr int erase_h = MAIN_MENU_BG_H - 2;
  u8g2_DrawBox(&_u8g2, 0, erase_y, SCREEN_W, erase_h);
  u8g2_SetDrawColor(&_u8g2, 1);
  u8g2_DrawBox(&_u8g2, 0, erase_y, SCREEN_W, erase_h);
  u8g2_SetDrawColor(&_u8g2, 0);
  draw_list_rows(&_u8g2, v, false);
}

void DisplayService::_draw_full_screen_list(const DisplayValues &v) {
  // White overlay area (full-screen list background)
  u8g2_DrawBox(&_u8g2, 0, FULL_SCREEN_BG_Y, CONTENT_W, FULL_SCREEN_BG_H);
  u8g2_SetDrawColor(&_u8g2, 1);
  u8g2_DrawBox(&_u8g2, 0, FULL_SCREEN_BG_Y, CONTENT_W, FULL_SCREEN_BG_H);
  u8g2_SetDrawColor(&_u8g2, 0);
  draw_list_rows(&_u8g2, v, true);

  // About screen has additional text fields below the list rows
  if (v.screen == Screen::About) {
    u8g2_SetFont(&_u8g2, u8g2_font_helvB08_tf);
    draw_text(&_u8g2, 10, 88, v.about_title);
    u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
    draw_text(&_u8g2, 10, 103, v.about_firmware);
    draw_text(&_u8g2, 10, 117, v.about_serial);
    draw_text(&_u8g2, 10, 130, v.about_hardware);
  }
}

void DisplayService::_draw_snackbar(const DisplayValues &v) {
  if (v.snackbar_text == nullptr || v.snackbar_text[0] == '\0')
    return;

  u8g2_DrawBox(&_u8g2, 0, SNACKBAR_Y, SCREEN_W, SNACKBAR_H);
  u8g2_SetDrawColor(&_u8g2, 1);
  u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, SNACKBAR_TEXT_BASELINE_Y, v.snackbar_text);
  u8g2_SetDrawColor(&_u8g2, 0);
}

void DisplayService::_draw_shutdown(Screen s) {
  // Common chrome: brand header + thick divider.
  u8g2_SetFont(&_u8g2, u8g2_font_helvB14_tf);
  draw_centered_text(&_u8g2, SCREEN_W / 2, SHUTDOWN_BRAND_BASELINE_Y, "AirGradient");
  for (int i = 0; i < SYSTEM_DIVIDER_THICKNESS; ++i) {
    u8g2_DrawHLine(&_u8g2, SYSTEM_DIVIDER_X_MARGIN, SHUTDOWN_DIVIDER_Y + i,
                   SCREEN_W - 2 * SYSTEM_DIVIDER_X_MARGIN);
  }

  // Reason-specific icon + text.
  switch (s) {
  case Screen::ShutdownDischarge:
    draw_shutdown_battery_low_icon(&_u8g2, SCREEN_W / 2, SHUTDOWN_ICON_CENTER_Y);
    draw_shutdown_text(&_u8g2, "Battery", "critically low", "Connect charger", "Charge before use");
    break;
  case Screen::ShutdownTemperature:
    draw_shutdown_battery_hot_icon(&_u8g2, SCREEN_W / 2, SHUTDOWN_ICON_CENTER_Y);
    draw_shutdown_text(&_u8g2, "Battery", "Overheated", "Move device to a", "cooler location");
    break;
  case Screen::ShutdownTemperatureLow:
    draw_shutdown_battery_cold_icon(&_u8g2, SCREEN_W / 2, SHUTDOWN_ICON_CENTER_Y);
    draw_shutdown_text(&_u8g2, "Battery", "Too Cold", "Move device to a", "warmer location");
    break;
  case Screen::ShutdownUser:
  default:
    draw_shutdown_power_icon(&_u8g2, SCREEN_W / 2, SHUTDOWN_ICON_CENTER_Y);
    draw_shutdown_text(&_u8g2, "Powered off", "", "Hold power button", "to turn on");
    break;
  }
}

void DisplayService::_draw_pairing_passkey(const DisplayValues &v) {
  u8g2_SetFont(&_u8g2, u8g2_font_helvB14_tf);
  draw_centered_text(&_u8g2, SCREEN_W / 2, PAIRING_TITLE_L1_BASELINE_Y, "Bluetooth");
  draw_centered_text(&_u8g2, SCREEN_W / 2, PAIRING_TITLE_L2_BASELINE_Y, "Pairing");

  for (int i = 0; i < SYSTEM_DIVIDER_THICKNESS; ++i) {
    u8g2_DrawHLine(&_u8g2, SYSTEM_DIVIDER_X_MARGIN, PAIRING_DIVIDER_Y + i,
                   SCREEN_W - 2 * SYSTEM_DIVIDER_X_MARGIN);
  }

  char passkey_str[8];
  snprintf(passkey_str, sizeof(passkey_str), "%06" PRIu32, v.ble_passkey);
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso32_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, PAIRING_PASSKEY_BASELINE_Y, passkey_str);

  u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, PAIRING_HINT_BASELINE_Y, "Enter on phone");
}

// ---------------------------------------------------------------------------
// Session-screen render methods (Info / Provisioning / ProvisioningConfirm)
// ---------------------------------------------------------------------------

void DisplayService::_draw_info(const DisplayValues &v) {
  if (v.info_text == nullptr || v.info_text[0] == '\0') {
    return;
  }

  // Spec's draft helvB12 is not in this product's compiled u8g2 font set.
  // helvR12 is the closest available substitute — slightly smaller than
  // the bold spec font but cleaner at this body-text size, and already
  // used elsewhere on the Home page (CO2/PM unit captions).  Font choice
  // is flagged in the spec's Open Questions list for hardware tuning.
  u8g2_SetFont(&_u8g2, u8g2_font_helvB12_tf);
  // Extra breathing room between lines beyond the font's natural
  // cap-to-descender height — keeps multi-line Info text from feeling
  // cramped at the chosen body-text font size.
  constexpr int LINE_GAP_PX = 4;
  const int ascent = u8g2_GetAscent(&_u8g2);
  const int descent = u8g2_GetDescent(&_u8g2); // negative
  const int line_h = (ascent - descent) + LINE_GAP_PX;

  static constexpr size_t MAX_INFO_LINES = 8;
  WrapLine lines[MAX_INFO_LINES];
  const size_t n = compute_wrapped_lines(v.info_text, SCREEN_W, &u8g2_str_width_fn, &_u8g2, lines,
                                         MAX_INFO_LINES);
  if (n == 0) {
    return;
  }

  // Vertical center within the body region (y=18..249).  Clamp the top so
  // the first line never crosses into y < 18 (partial-refresh boundary).
  const int block_h = static_cast<int>(n) * line_h;
  int top_y = BODY_Y + (BODY_H - block_h) / 2;
  if (top_y < BODY_Y) {
    top_y = BODY_Y;
  }

  char scratch[64];
  for (size_t i = 0; i < n; ++i) {
    size_t L = lines[i].length;
    if (L >= sizeof(scratch)) {
      L = sizeof(scratch) - 1;
    }
    if (L > 0 && lines[i].begin != nullptr) {
      memcpy(scratch, lines[i].begin, L);
    }
    scratch[L] = '\0';

    const int w = static_cast<int>(u8g2_GetStrWidth(&_u8g2, scratch));
    const int x = (SCREEN_W - w) / 2;
    const int baseline_y = top_y + ascent + static_cast<int>(i) * line_h;
    draw_text(&_u8g2, x, baseline_y, scratch);
  }
}

void DisplayService::_draw_provisioning(const DisplayValues &v) {
  // ProvisioningTransport::BleOnly == 0, ProvisioningTransport::WifiOnly == 1.
  const bool ble_active = (v.provisioning_transport == 0);

  constexpr int TITLE_L1_Y = 21;
  constexpr int TITLE_L2_Y = 40;
  constexpr int QR_TOP_Y = 40;
  constexpr int QR_CAPTION_Y = 118;
  constexpr int INSTRUCTION_L1_Y = 136;
  constexpr int INSTRUCTION_L2_Y = 150;
  constexpr int STATUS_TOP_Y = 162;
  constexpr int STATUS_TEXT_Y = 183;
  constexpr int STATUS_BOTTOM_Y = 196;
  constexpr int ACTION_HELPER_Y = 210;
  constexpr int ACTION_ROW0_Y = 216;
  constexpr int ACTION_ROW1_Y = 232;

  // --- Title ---
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, TITLE_L1_Y, "Connect");
  draw_centered_text(&_u8g2, SCREEN_W / 2, TITLE_L2_Y, "to Wi-Fi");

  // --- QR code (UIManager encodes go-to-app for BleOnly, WIFI:... for WifiOnly) ---
  draw_provisioning_qr(&_u8g2, v.qr, SCREEN_W / 2, QR_TOP_Y);

  // --- QR caption ---
  u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, QR_CAPTION_Y,
                     ble_active ? "Scan to get the app" : "Scan to auto-join");

  // --- Instructions (transport-specific) ---
  if (ble_active) {
    u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
    draw_centered_text(&_u8g2, SCREEN_W / 2, INSTRUCTION_L1_Y, "Use AirGradient app");
    draw_centered_text(&_u8g2, SCREEN_W / 2, INSTRUCTION_L2_Y, "to continue");
  } else {
    u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
    const char *ssid = (v.provisioning_ap_ssid != nullptr) ? v.provisioning_ap_ssid : "airgradient";
    draw_centered_text(&_u8g2, SCREEN_W / 2, INSTRUCTION_L1_Y, ssid);
    u8g2_SetFont(&_u8g2, u8g2_font_helvB08_tf);
    char pwd_buf[40];
    const char *password =
        (v.provisioning_ap_password != nullptr) ? v.provisioning_ap_password : "cleanair";
    (void)snprintf(pwd_buf, sizeof(pwd_buf), "Password: %s", password);
    draw_centered_text(&_u8g2, SCREEN_W / 2, INSTRUCTION_L2_Y, pwd_buf);
  }

  // --- Status line (within HLine separators) ---
  // Auto-wrap the status string to at most 2 lines so long entries like
  // "Connected! 192.168.x.y" or "Connect failed - try again" don't clip
  // off the screen edges.  The status band y=163..195 (32 px) fits two
  // helvB08 lines (~10 px each) with breathing room around the separators.
  draw_provisioning_separator(&_u8g2, STATUS_TOP_Y);
  u8g2_SetFont(&_u8g2, u8g2_font_helvB08_tf);
  if (v.provisioning_status != nullptr && v.provisioning_status[0] != '\0') {
    constexpr size_t MAX_STATUS_LINES = 2;
    WrapLine status_lines[MAX_STATUS_LINES];
    const size_t n = compute_wrapped_lines(v.provisioning_status, SCREEN_W, &u8g2_str_width_fn,
                                           &_u8g2, status_lines, MAX_STATUS_LINES);
    // Single-line keeps the original baseline; two-line splits +/- 6 px
    // around the single-line baseline so the block stays centered in the
    // status band.
    constexpr int STATUS_LINE_GAP_PX = 12;
    const int two_line_top = STATUS_TEXT_Y - STATUS_LINE_GAP_PX / 2;
    char scratch[64];
    for (size_t i = 0; i < n; ++i) {
      size_t L = status_lines[i].length;
      if (L >= sizeof(scratch)) {
        L = sizeof(scratch) - 1;
      }
      if (L > 0 && status_lines[i].begin != nullptr) {
        memcpy(scratch, status_lines[i].begin, L);
      }
      scratch[L] = '\0';
      const int baseline_y =
          (n == 1) ? STATUS_TEXT_Y : two_line_top + static_cast<int>(i) * STATUS_LINE_GAP_PX;
      draw_centered_text(&_u8g2, SCREEN_W / 2, baseline_y, scratch);
    }
  }
  draw_provisioning_separator(&_u8g2, STATUS_BOTTOM_Y);

  // --- Helper text + action rows ---
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, ACTION_HELPER_Y,
                     ble_active ? "Setup without app?" : "Prefer the app?");

  // Action labels come from UIManager via v.rows[0..1].  Selection drives
  // which row gets the filled rectangle.
  const char *row0 = (v.row_count >= 1) ? v.rows[0].text : "";
  const char *row1 = (v.row_count >= 2) ? v.rows[1].text : "";
  draw_provisioning_action_row(&_u8g2, row0, ACTION_ROW0_Y, v.selected_row == 0);
  draw_provisioning_action_row(&_u8g2, row1, ACTION_ROW1_Y, v.selected_row == 1);
}

void DisplayService::_draw_provisioning_confirm(const DisplayValues &v) {
  // Question text picked by orchestrator/UIManager: surfaced through
  // v.rows[0].text (kind + active transport are folded into one string).
  const char *question = (v.row_count >= 1) ? v.rows[0].text : "Confirm?";

  // Buttons: No (index 0, default) on the left, Yes (index 1) on the right.
  constexpr int QUESTION_Y = 110;
  constexpr int BTN_Y = 140;
  constexpr int BTN_H = 24;
  constexpr int BTN_W = 50;
  constexpr int BTN_NO_X = 10;
  constexpr int BTN_YES_X = SCREEN_W - BTN_NO_X - BTN_W;
  constexpr int BTN_TEXT_BASELINE = BTN_Y + 16;

  u8g2_SetFont(&_u8g2, u8g2_font_helvB08_tf);
  draw_centered_text(&_u8g2, SCREEN_W / 2, QUESTION_Y, question);

  const bool no_selected = (v.provisioning_confirm_index == 0);

  // No button
  if (no_selected) {
    u8g2_DrawBox(&_u8g2, BTN_NO_X, BTN_Y, BTN_W, BTN_H);
    u8g2_SetDrawColor(&_u8g2, 1);
  } else {
    u8g2_DrawFrame(&_u8g2, BTN_NO_X, BTN_Y, BTN_W, BTN_H);
  }
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, BTN_NO_X + BTN_W / 2, BTN_TEXT_BASELINE, "No");
  if (no_selected) {
    u8g2_SetDrawColor(&_u8g2, 0);
  }

  // Yes button
  if (!no_selected) {
    u8g2_DrawBox(&_u8g2, BTN_YES_X, BTN_Y, BTN_W, BTN_H);
    u8g2_SetDrawColor(&_u8g2, 1);
  } else {
    u8g2_DrawFrame(&_u8g2, BTN_YES_X, BTN_Y, BTN_W, BTN_H);
  }
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, BTN_YES_X + BTN_W / 2, BTN_TEXT_BASELINE, "Yes");
  if (!no_selected) {
    u8g2_SetDrawColor(&_u8g2, 0);
  }
}

void DisplayService::_draw_getting_started(const DisplayValues &v) {
  // Simplified sibling of _draw_provisioning with its own layout: a
  // chunkier 3px QR and a single action row, no status band / helper text.
  // The whole stack (title -> QR -> caption -> instruction -> button) is
  // vertically centered in the 250px canvas. See first_boot_onboarding.md.
  constexpr int TITLE_L1_Y = 44;
  constexpr int TITLE_L2_Y = 64;
  constexpr int QR_TOP_Y = 68;
  constexpr int QR_MODULE_PX = 3;
  constexpr int QR_CAPTION_Y = 174;
  constexpr int INSTRUCTION_Y = 195;
  constexpr int ACTION_ROW_Y = 204;

  // --- Title (same font as the Provisioning page) ---
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, TITLE_L1_Y, "Getting");
  draw_centered_text(&_u8g2, SCREEN_W / 2, TITLE_L2_Y, "Started");

  // --- QR code (chunkier 3px modules for easier scanning) ---
  draw_provisioning_qr(&_u8g2, v.qr, SCREEN_W / 2, QR_TOP_Y, QR_MODULE_PX);

  // --- QR caption ---
  u8g2_SetFont(&_u8g2, u8g2_font_helvB08_tf);
  draw_centered_text(&_u8g2, SCREEN_W / 2, QR_CAPTION_Y, "Scan to set up");

  // --- Instruction (names the button action) ---
  u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
  draw_centered_text(&_u8g2, SCREEN_W / 2, INSTRUCTION_Y, "Or just use it right now");

  // --- Single action row (label from v.rows[0]) ---
  const char *row0 = (v.row_count >= 1) ? v.rows[0].text : "";
  draw_provisioning_action_row(&_u8g2, row0, ACTION_ROW_Y, /*selected=*/true);
}

namespace {

/// Phase banner text for each FgLearn* screen. Two-word states wrap to two
/// lines (l2 non-null); single-word states leave l2 null.
struct FgPhaseText {
  const char *l1;
  const char *l2;
};

FgPhaseText fg_learning_phase_lines(Screen s) {
  switch (s) {
  case Screen::FgLearnCharging:
    return {"Charging", nullptr};
  case Screen::FgLearnResting:
    return {"Resting", nullptr};
  case Screen::FgLearnUnplug:
    return {"Unplug", "charger"};
  case Screen::DischargeComplete:
    return {"Discharge", "complete"};
  case Screen::FgLearnVerifying:
    return {"Verifying", nullptr};
  case Screen::FgLearnComplete:
    return {"Complete", nullptr};
  case Screen::FgLearnFailed:
    return {"Failed", nullptr};
  default:
    return {"Battery", "learning"};
  }
}

/// Local BmsChargingState -> string map (kept in the display layer so this
/// file needs no bms_types.h include). Order MUST match the BmsChargingState
/// enum declaration in bms_types.h.
const char *fg_bms_state_str(uint8_t raw) {
  static const char *const NAMES[] = {
      "Unknown",    "NotCharging", "TrickleCharge", "PreCharge",
      "FastCharge", "TaperCharge", "TopOff",        "Done",
  };
  return (raw < (sizeof(NAMES) / sizeof(NAMES[0]))) ? NAMES[raw] : "?";
}

} // namespace

void DisplayService::_draw_fg_learning_dashboard(const DisplayValues &v) {
  const FgLearningDashboardData &d = v.fg_dashboard;

  // Phase banner (helvB14). Two-word states wrap to two lines; single-word
  // states sit centered in the reserved two-line block. Once the charger is
  // unplugged, the Discharge cue ("Unplug charger") becomes "Discharging".
  FgPhaseText phase = fg_learning_phase_lines(v.screen);
  if (v.screen == Screen::FgLearnUnplug && !d.external_input_present) {
    phase = {"Discharging", nullptr};
  }
  u8g2_SetFont(&_u8g2, u8g2_font_helvB14_tf);
  if (phase.l2 != nullptr) {
    draw_centered_text(&_u8g2, SCREEN_W / 2, 16, phase.l1);
    draw_centered_text(&_u8g2, SCREEN_W / 2, 33, phase.l2);
  } else {
    draw_centered_text(&_u8g2, SCREEN_W / 2, 24, phase.l1);
  }

  if (!d.valid) {
    u8g2_SetFont(&_u8g2, u8g2_font_helvB14_tf);
    draw_centered_text(&_u8g2, SCREEN_W / 2, 130, "FG: NO DATA");
    return;
  }

  char buf[40];

  // Cycle n/N (helvB08).
  u8g2_SetFont(&_u8g2, u8g2_font_helvB08_tf);
  snprintf(buf, sizeof(buf), "Cycle %u/%u", d.cycle, d.cycle_target);
  draw_centered_text(&_u8g2, SCREEN_W / 2, 46, buf);

  // Failed: show the cause under the banner (the BOOT-exit hint is at the bottom).
  if (v.screen == Screen::FgLearnFailed && d.fail_reason != nullptr) {
    u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
    char rbuf[40];
    snprintf(rbuf, sizeof(rbuf), "FAIL: %s", d.fail_reason);
    draw_centered_text(&_u8g2, SCREEN_W / 2, 58, rbuf);
  }

  u8g2_DrawHLine(&_u8g2, 6, 50, SCREEN_W - 12);

  // SOC (big), cell voltage, signed current.
  u8g2_SetFont(&_u8g2, u8g2_font_logisoso32_tr);
  snprintf(buf, sizeof(buf), "%u%%", d.soc_pct);
  draw_centered_text(&_u8g2, SCREEN_W / 2, 88, buf);

  u8g2_SetFont(&_u8g2, u8g2_font_logisoso16_tr);
  snprintf(buf, sizeof(buf), "%u.%03uV", d.voltage_mv / 1000, d.voltage_mv % 1000);
  draw_centered_text(&_u8g2, SCREEN_W / 2, 108, buf);

  u8g2_SetFont(&_u8g2, u8g2_font_helvR12_tr);
  snprintf(buf, sizeof(buf), "I %+d mA", d.current_ma);
  draw_centered_text(&_u8g2, SCREEN_W / 2, 126, buf);
  u8g2_DrawHLine(&_u8g2, 6, 132, SCREEN_W - 12);

  // Detail rows.
  u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
  int y = 144;
  constexpr int X = 6;
  constexpr int ROW_H = 12;

  snprintf(buf, sizeof(buf), "%u/%u mAh", d.remaining_mah, d.full_charge_mah);
  draw_text(&_u8g2, X, y, buf);
  y += ROW_H;

  // Temperature on its own row: the combined "T..C FC CHG DSG" line overran
  // the 128 px canvas and clipped DSG on the right edge.
  snprintf(buf, sizeof(buf), "Temp %.1f C", static_cast<double>(d.temperature_c));
  draw_text(&_u8g2, X, y, buf);
  y += ROW_H;

  snprintf(buf, sizeof(buf), "FC%d CHG%d DSG%d", d.flag_fc, d.flag_chg, d.flag_dsg);
  draw_text(&_u8g2, X, y, buf);
  y += ROW_H;

  snprintf(buf, sizeof(buf), "Q%d R%d OCV%d", d.qmax_up, d.res_up, d.ocv_taken);
  draw_text(&_u8g2, X, y, buf);
  y += ROW_H;

  snprintf(buf, sizeof(buf), "ICHG %u mA", d.charge_current_ma);
  draw_text(&_u8g2, X, y, buf);
  y += ROW_H;

  snprintf(buf, sizeof(buf), "BMS %s", fg_bms_state_str(d.bms_charging_state));
  draw_text(&_u8g2, X, y, buf);

  // Terminal screens (Complete/Failed): POWER-press exit hint at the bottom.
  if (v.screen == Screen::FgLearnComplete || v.screen == Screen::FgLearnFailed) {
    u8g2_SetFont(&_u8g2, u8g2_font_helvR08_tr);
    draw_centered_text(&_u8g2, SCREEN_W / 2, 216, "Press POWER to exit");
  }

  // Stage-elapsed clock (helvB12), bottom-centered, HH:MM.
  const uint32_t total_min = d.stage_elapsed_ms / 60000u;
  uint32_t hh = total_min / 60u;
  const uint32_t mm = total_min % 60u;
  if (hh > 99u) {
    hh = 99u;
  }
  u8g2_SetFont(&_u8g2, u8g2_font_helvB12_tf);
  snprintf(buf, sizeof(buf), "%02u:%02u", static_cast<unsigned>(hh), static_cast<unsigned>(mm));
  draw_centered_text(&_u8g2, SCREEN_W / 2, 230, buf);
}

void DisplayService::_draw_chart(const DisplayValues &v) {
  // Polyline only — no axes, no border lines.
  if (v.chart_samples == nullptr || v.chart_count == 0)
    return;

  const float range = v.chart_max - v.chart_min;
  const int plot_h = PLOT_H - 1; // drawable height range
  int prev_x = PLOT_X;
  int prev_y = PLOT_Y + plot_h / 2;

  for (int x = 0; x < PLOT_W; ++x) {
    const int sample_index =
        (x * static_cast<int>(v.chart_count - 1)) / ((PLOT_W > 1) ? (PLOT_W - 1) : 1);
    const float sample = v.chart_samples[sample_index];

    int y = PLOT_Y + plot_h / 2;
    if (range > 0.001f) {
      const float norm = (sample - v.chart_min) / range;
      y = PLOT_Y + plot_h - static_cast<int>(norm * static_cast<float>(plot_h));
    }
    if (y < PLOT_Y)
      y = PLOT_Y;
    if (y > PLOT_Y + plot_h)
      y = PLOT_Y + plot_h;

    const int draw_x = PLOT_X + x;
    if (x > 0) {
      u8g2_DrawLine(&_u8g2, prev_x, prev_y, draw_x, y);
    }
    prev_x = draw_x;
    prev_y = y;
  }
}

// ===========================================================================
// DisplayService — async worker task
// ===========================================================================

void DisplayService::_worker_entry(void *arg) {
  auto *self = static_cast<DisplayService *>(arg);
  self->_worker_loop();
  RTOS::task_delete(nullptr);
}

void DisplayService::_worker_loop() {
  while (true) {
    // Block until the orchestrator signals frame-ready.
    RTOS::task_notify_take(UINT32_MAX);
    if (!_running.load())
      break;

    esp_err_t err = driver_bus_acquire();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "worker: bus acquire failed: %s", esp_err_to_name(err));
      _release_framebuffer();
      continue;
    }

    switch (_pending_mode) {
    case RefreshMode::Full:
      err = driver_hw_init_full();
      if (err == ESP_OK)
        err = driver_set_basemap(_render_buf);
      _diff_count = 0;
      break;

    case RefreshMode::Fast:
      err = driver_hw_init_fast();
      if (err == ESP_OK)
        err = driver_fast_write(_render_buf);
      if (err == ESP_OK)
        err = driver_fast_commit();
      if (_diff_count < UINT8_MAX) {
        _diff_count++;
      }
      break;

    case RefreshMode::Partial:
      err = driver_part_begin();
      if (err == ESP_OK) {
        // Full-canvas session screens own the title region (y=0..17), so a
        // body-only partial would leave prior pixels there. Everything else
        // stays body-only.
        const auto region =
            go_display_geometry::partial_region(is_session_screen(_prev_values.screen));
        err = driver_part_write_region(0, region.y, _render_buf + region.byte_offset, region.height,
                                       SCREEN_W);
      }
      if (err == ESP_OK)
        err = driver_part_commit();
      if (_diff_count < UINT8_MAX) {
        _diff_count++;
      }
      break;
    }

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "worker: refresh failed: %s", esp_err_to_name(err));
    }

    driver_bus_release();
    _release_framebuffer();
  }
}

#endif // !TEST_HOST
