#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "board_config.h"
#include "native_gpio.h"
#include "nvs_config_store.h"
#include "reference_settings.h"
#include "rtos.h"

// ============================================================
// Test selection
// Uncomment exactly one of the macros below to choose which
// test runs in app_main.  Alternatively, pass the macro via
// the build system (e.g. cmake -DEXTRA_CXXFLAGS=-DRUN_TEST_GPIO).
// Defaults to RUN_TEST_SENSORS when nothing is defined.
// ============================================================
// #define RUN_TEST_SENSORS
#define RUN_TEST_ACCEL
// #define RUN_TEST_AIRGRADIENT_CLIENT
// #define RUN_TEST_BLE
// #define RUN_TEST_CONFIG
// #define RUN_TEST_GPIO
// #define RUN_TEST_HTTP_SERVER
// #define RUN_TEST_LOCAL_SERVER
// #define RUN_TEST_NAND_STORAGE
// #define RUN_TEST_OTA
// #define RUN_TEST_PAYLOAD_CACHE
// #define RUN_TEST_PROVISIONING
// #define RUN_TEST_SERIAL
// #define RUN_TEST_TOUCH
// #define RUN_TEST_WIFI

#if !defined(RUN_TEST_SENSORS) && !defined(RUN_TEST_BLE) && !defined(RUN_TEST_CONFIG) &&           \
    !defined(RUN_TEST_GPIO) && !defined(RUN_TEST_AIRGRADIENT_CLIENT) &&                            \
    !defined(RUN_TEST_NAND_STORAGE) && !defined(RUN_TEST_OTA) &&                                   \
    !defined(RUN_TEST_PAYLOAD_CACHE) && !defined(RUN_TEST_PROVISIONING) &&                         \
    !defined(RUN_TEST_SERIAL) && !defined(RUN_TEST_TOUCH) && !defined(RUN_TEST_WIFI) &&            \
    !defined(RUN_TEST_HTTP_SERVER) && !defined(RUN_TEST_LOCAL_SERVER) && !defined(RUN_TEST_ACCEL)
#define RUN_TEST_SENSORS
#endif

// --- Conditional test headers --------------------------------
#ifdef RUN_TEST_ACCEL
#include "test_accel.h"
#endif

#ifdef RUN_TEST_SENSORS
#include "test_sensors.h"
#endif

#ifdef RUN_TEST_BLE
#include "test_ble.h"
#endif

#ifdef RUN_TEST_CONFIG
#include "test_config.h"
#endif

#ifdef RUN_TEST_GPIO
#include "test_gpio.h"
#endif

#ifdef RUN_TEST_HTTP_SERVER
#include "test_http_server.h"
#endif

#ifdef RUN_TEST_LOCAL_SERVER
#include "test_local_server.h"
#endif

#ifdef RUN_TEST_NAND_STORAGE
#include "test_nand_storage.h"
#endif

#ifdef RUN_TEST_OTA
#include "test_ota.h"
#endif

#ifdef RUN_TEST_PAYLOAD_CACHE
#include "test_payload_cache.h"
#endif

#ifdef RUN_TEST_PROVISIONING
#include "test_provisioning.h"
#endif

#ifdef RUN_TEST_SERIAL
#include "test_serial.h"
#endif

#ifdef RUN_TEST_TOUCH
#include "test_touch.h"
#endif

#ifdef RUN_TEST_AIRGRADIENT_CLIENT
#include "test_airgradient_client.h"
#endif

#ifdef RUN_TEST_WIFI
#include "test_wifi.h"
#endif

// GPIO power-rail init is only done when the selected test requires it.
// Tests that drive sensor enable lines or exercise GPIO directly need this.
// NAND_STORAGE, PAYLOAD_CACHE, CONFIG, BLE, and TOUCH do not.
#if defined(RUN_TEST_SENSORS) || defined(RUN_TEST_GPIO) || defined(RUN_TEST_SERIAL)
#define NEEDS_GPIO 1
#endif

// I2C is only initialised when the selected test requires it.
#if defined(RUN_TEST_SENSORS) || defined(RUN_TEST_SERIAL) || defined(RUN_TEST_TOUCH) ||            \
    defined(RUN_TEST_ACCEL)
#define NEEDS_I2C 1
#endif

static constexpr const char *TAG = "main";

#ifdef NEEDS_GPIO
static bool init_gpio(const gpio::Hal &hal);
#endif

static bool init_nvs();

extern "C" void app_main(void) {
#ifdef RUN_TEST_SENSORS
  ESP_LOGI(TAG, "=== Test: SENSORS ===");
#elif defined(RUN_TEST_ACCEL)
  ESP_LOGI(TAG, "=== Test: ACCEL ===");
#elif defined(RUN_TEST_BLE)
  ESP_LOGI(TAG, "=== Test: BLE ===");
#elif defined(RUN_TEST_CONFIG)
  ESP_LOGI(TAG, "=== Test: CONFIG ===");
#elif defined(RUN_TEST_GPIO)
  ESP_LOGI(TAG, "=== Test: GPIO ===");
#elif defined(RUN_TEST_HTTP_SERVER)
  ESP_LOGI(TAG, "=== Test: HTTP_SERVER ===");
#elif defined(RUN_TEST_LOCAL_SERVER)
  ESP_LOGI(TAG, "=== Test: LOCAL_SERVER ===");
#elif defined(RUN_TEST_NAND_STORAGE)
  ESP_LOGI(TAG, "=== Test: NAND_STORAGE ===");
#elif defined(RUN_TEST_OTA)
  ESP_LOGI(TAG, "=== Test: OTA ===");
#elif defined(RUN_TEST_PAYLOAD_CACHE)
  ESP_LOGI(TAG, "=== Test: PAYLOAD_CACHE ===");
#elif defined(RUN_TEST_PROVISIONING)
  ESP_LOGI(TAG, "=== Test: PROVISIONING ===");
#elif defined(RUN_TEST_SERIAL)
  ESP_LOGI(TAG, "=== Test: SERIAL ===");
#elif defined(RUN_TEST_TOUCH)
  ESP_LOGI(TAG, "=== Test: TOUCH ===");
#elif defined(RUN_TEST_AIRGRADIENT_CLIENT)
  ESP_LOGI(TAG, "=== Test: AIRGRADIENT_CLIENT ===");
#elif defined(RUN_TEST_WIFI)
  ESP_LOGI(TAG, "=== Test: WIFI ===");
#endif

#ifdef NEEDS_GPIO
  if (!init_gpio(gpio::native::hal)) {
    ESP_LOGE(TAG, "GPIO init failed");
    return;
  }
#endif

  if (!init_nvs()) {
    ESP_LOGE(TAG, "NVS init failed");
    return;
  }

  RTOS::delay_ms(100);

#ifdef NEEDS_I2C
  i2c_master_bus_config_t i2c_bus_config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = PIN_I2C_SDA,
      .scl_io_num = PIN_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags =
          {
              .enable_internal_pullup = true,
              .allow_pd = false,
          },
  };

  i2c_master_bus_handle_t i2c_bus = nullptr;
  if (i2c_new_master_bus(&i2c_bus_config, &i2c_bus) != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus init failed");
    return;
  }
  ESP_LOGI(TAG, "I2C bus ready");
  RTOS::delay_ms(100);
#endif

  // ---- Run the selected test --------------------------------
#ifdef RUN_TEST_SENSORS
  run_test_sensors(i2c_bus);

#elif defined(RUN_TEST_ACCEL)
  run_test_accel(i2c_bus, gpio::native::hal);

#elif defined(RUN_TEST_BLE)
#ifdef CONFIG_BT_NIMBLE_ENABLED
  NvsConfigStore config_store("reference");
  ReferenceSettings settings = load_reference_settings(config_store);
  run_test_ble(settings.device_name.c_str());
#else
  ESP_LOGW(TAG, "BLE test skipped: CONFIG_BT_NIMBLE_ENABLED not set");
#endif

#elif defined(RUN_TEST_CONFIG)
  NvsConfigStore config_store("reference");
  run_test_config(config_store);

#elif defined(RUN_TEST_GPIO)
  run_test_gpio(gpio::native::hal);

#elif defined(RUN_TEST_HTTP_SERVER)
  run_test_http_server();

#elif defined(RUN_TEST_LOCAL_SERVER)
  run_test_local_server();

#elif defined(RUN_TEST_NAND_STORAGE)
  run_test_nand_storage();

#elif defined(RUN_TEST_OTA)
  run_test_ota();

#elif defined(RUN_TEST_PAYLOAD_CACHE)
  run_test_payload_cache();

#elif defined(RUN_TEST_PROVISIONING)
  run_test_provisioning();

#elif defined(RUN_TEST_SERIAL)
  run_test_serial(i2c_bus);

#elif defined(RUN_TEST_TOUCH)
  run_test_touch(i2c_bus);

#elif defined(RUN_TEST_AIRGRADIENT_CLIENT)
  run_test_airgradient_client();

#elif defined(RUN_TEST_WIFI)
  run_test_wifi();
#endif

  ESP_LOGI(TAG, "=== Test complete ===");
}

#ifdef NEEDS_GPIO
static bool init_gpio(const gpio::Hal &hal) {
#if defined(BOARD_MAX)
  const int output_pins[] = {
      static_cast<int>(PIN_IO_WDT),     static_cast<int>(PIN_EN_PMS1),
      static_cast<int>(PIN_EN_PMS2),    static_cast<int>(PIN_EN_CO2),
      static_cast<int>(PIN_EN_CE_CARD), static_cast<int>(PIN_EN_ALPHASENSE),
  };

  for (int pin : output_pins) {
    if (!hal.configure(pin, gpio::Mode::Output, gpio::PullMode::Floating,
                       gpio::InterruptType::Disabled)) {
      return false;
    }
  }

  gpio_set_drive_capability(PIN_IO_WDT, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability(PIN_EN_PMS1, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability(PIN_EN_PMS2, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability(PIN_EN_CO2, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability(PIN_EN_ALPHASENSE, GPIO_DRIVE_CAP_3);
  gpio_set_drive_capability(PIN_EN_CE_CARD, GPIO_DRIVE_CAP_3);

  if (!hal.set_level(static_cast<int>(PIN_IO_WDT), 0) ||
      !hal.set_level(static_cast<int>(PIN_EN_PMS1), 1) ||
      !hal.set_level(static_cast<int>(PIN_EN_PMS2), 1) ||
      !hal.set_level(static_cast<int>(PIN_EN_CE_CARD), 0) ||
      !hal.set_level(static_cast<int>(PIN_EN_CO2), 1) ||
      !hal.set_level(static_cast<int>(PIN_EN_ALPHASENSE), 1)) {
    return false;
  }

#elif defined(BOARD_GO)
  const int output_pins[] = {
      static_cast<int>(PIN_EN_PM),
  };

  for (int pin : output_pins) {
    if (!hal.configure(pin, gpio::Mode::Output, gpio::PullMode::Floating,
                       gpio::InterruptType::Disabled)) {
      return false;
    }
  }

  gpio_set_drive_capability(PIN_EN_PM, GPIO_DRIVE_CAP_3);

  if (!hal.set_level(static_cast<int>(PIN_EN_PM), 1)) {
    return false;
  }

#endif // BOARD selection

  return true;
}
#endif // NEEDS_GPIO

static bool init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    err = nvs_flash_erase();
    if (err != ESP_OK) {
      return false;
    }
    err = nvs_flash_init();
  }
  return err == ESP_OK;
}
