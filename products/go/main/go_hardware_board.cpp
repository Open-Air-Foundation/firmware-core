/**
 * AirGradient Go — GoHardwareBoard Implementation
 *
 * Real ESP-IDF hardware implementation of the GoBoard interface.
 * Contains all hardware init calls, driver creation, and bus management
 * moved from main.cpp.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_hardware_board.h"

#include <cassert>
#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_app_desc.h>
#include <nvs_flash.h>

#include "ag_i2c.h"
#include "ag_log.h"
#include "airgradient_uart.h"
#include "backends/rtc_payload_cache_storage.h"
#include "cap1203.h"
#include "common.h"
#include "drivers/bq25629/bq25629_bms.h"
#include "drivers/bq27427/bq27427.h"
#include "drivers/dps368/dps368.h"
#include "drivers/s12/s12.h"
#include "drivers/scd4x/scd4x.h"
#include "drivers/sgp41/sgp41.h"
#include "drivers/sht40/sht40.h"
#include "drivers/sps30/sps30.h"
#include "drivers/stcc4/stcc4.h"
#include "gps/gps_driver.h"
#include "native_gpio.h"
#include "nvs_config_store.h"
#include "rtos.h"
#include "services/payload_cache.h"
#include "services/sensor_manager.h"
#include "spi_nand_storage.h"

#include "board_config.h"
#include "drivers/esp_wifi_hal.h"
#include "drivers/idf_http_server.h"
#include "go_buzzer.h"
#include "go_buzzer_driver.h"
#include "go_display.h"
#include "go_led.h"
#include "go_led_driver.h"
#include "go_power.h"
#include "go_storage.h"
#include "go_ulp.h"
#include "nimble_ble_server.h"
#include "services/ag_client.h"
#include "services/wifi_manager.h"

static constexpr const char *TAG = "board";

// ---------------------------------------------------------------------------
// Fuel-gauge cell configuration — validated on hardware against AGo's
// single-cell 2000 mAh Li-ion pack.  Revisit if cell sourcing changes.
// ---------------------------------------------------------------------------

static constexpr FgCellConfig AGO_CELL_CONFIG = {
    .design_capacity_mah = 2000,
    .design_energy_mwh = 7400,
    .terminate_voltage_mv = 3000,
    .sleep_current_ma = 50,
};

// FG DM corruption sanity ranges.  A reading outside any of these
// ranges is treated as evidence of a corrupted persistent block
// (most commonly a prior aborted CFGUPDATE).
static constexpr uint16_t FG_DC_SANITY_MIN_MAH = 500;
static constexpr uint16_t FG_DC_SANITY_MAX_MAH = 8000;
static constexpr uint16_t FG_FCC_SANITY_MAX_MAH = 8500;

// ===========================================================================
// Private helper: CO2 sensor detection
// ===========================================================================

static CO2Sensor *init_co2_sensor(i2c_master_bus_handle_t i2c_bus) {
  // 1. SenseAir S12 (no integrated T/RH)
  auto *s12 = new S12(i2c_bus, I2C_ADDR_S12);
  if (s12->init()) {
    AG_LOGI(TAG, "CO2 sensor: S12 selected");
    return s12;
  }
  AG_LOGW(TAG, "CO2 sensor: S12 not detected");
  delete s12;

  // 2. Sensirion SCD4x (with integrated T/RH)
  auto *scd4x = new SCD4x(i2c_bus, I2C_ADDR_SCD4X);
  if (scd4x->init()) {
    AG_LOGI(TAG, "CO2 sensor: SCD4x selected");
    return scd4x;
  }
  AG_LOGW(TAG, "CO2 sensor: SCD4x not detected");
  delete scd4x;

  // 3. Sensirion STCC4 (with integrated T/RH)
  auto *stcc4 = new STCC4(i2c_bus, I2C_ADDR_STCC4);
  if (stcc4->init()) {
    AG_LOGI(TAG, "CO2 sensor: STCC4 selected");
    return stcc4;
  }
  AG_LOGW(TAG, "CO2 sensor: STCC4 not detected");
  delete stcc4;

  AG_LOGE(TAG, "CO2 sensor init failed (all candidates)");
  return nullptr;
}

// ===========================================================================
// Init methods (idempotent)
// ===========================================================================

void GoHardwareBoard::init_nvs() {
  if (_nvs_ready)
    return;
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  _nvs_ready = true;
}

void GoHardwareBoard::init_buses() {
  if (_buses_ready)
    return;

  // GPIO power enables — write a safe-default level before variant detection.
  // Level 1 is safe on both variants:
  //   Prototype (active-high): level 1 = PM ON (matches existing behavior)
  //   v1       (active-low):   level 1 = PM OFF (safe before detection)
  auto &hal = gpio::native::hal;
  hal.configure(PIN_PM_POWER, gpio::Mode::Output, gpio::PullMode::Floating,
                gpio::InterruptType::Disabled);
  gpio_set_drive_capability(PIN_PM_POWER, GPIO_DRIVE_CAP_3);
  hal.set_level(PIN_PM_POWER, 1);

  RTOS::delay_ms(100);

  // I2C bus
  i2c_master_bus_config_t config = {
      .i2c_port = I2C_MASTER_PORT,
      .sda_io_num = PIN_I2C_SDA,
      .scl_io_num = PIN_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = I2C_GLITCH_IGNORE_CNT,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags =
          {
              .enable_internal_pullup = I2C_INTERNAL_PULLUPS,
              .allow_pd = false,
          },
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &_i2c_bus));
  AG_LOGI(TAG, "I2C bus ready");

  RTOS::delay_ms(100);

  // Board variant detection — probe BQ27427 fuel gauge at 0x55.
  // ACK = v1 board; NACK / transport error = prototype (fail-safe).
  constexpr uint8_t BQ27427_PROBE_ADDR = 0x55;
  constexpr int BQ27427_PROBE_TIMEOUT_MS = 100;

  const bool fg_present =
      i2c_device_present(_i2c_bus, BQ27427_PROBE_ADDR, BQ27427_PROBE_TIMEOUT_MS);
  _variant = fg_present ? BoardVariant::V1 : BoardVariant::Prototype;
  AG_LOGI(TAG, "board variant: %s (BQ27427 @ 0x55 %s)", board_variant_str(_variant),
          fg_present ? "ACK" : "NACK");

  // Drive PM_POWER to the variant-appropriate "ON" level.
  // Prototype: already at 1 (safe-default above), no write needed.
  // v1: write level 0 (active-low PM ON).
  if (_variant == BoardVariant::V1) {
    hal.set_level(PIN_PM_POWER, pm_power_on_level(_variant));
  }

  RTOS::delay_ms(100);
  _buses_ready = true;
}

void GoHardwareBoard::init_spi() {
  if (_spi_ready)
    return;

  spi_bus_config_t bus = {};
  bus.mosi_io_num = PIN_SPI_MOSI;
  bus.miso_io_num = PIN_SPI_MISO;
  bus.sclk_io_num = PIN_SPI_SCLK;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = 4096;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST, &bus, SPI_DMA_CH_AUTO));

  AG_LOGI(TAG, "SPI bus ready");
  _spi_ready = true;
}

void GoHardwareBoard::init_bms() {
  if (_bms_ready)
    return;

  constexpr drivers::BQ25629_Config config = {
      .charge_voltage_mv = 4200,
      .charge_current_ma = 500,
      .input_current_limit_ma = 1500,
      .input_voltage_limit_mv = 4600,
      .min_system_voltage_mv = 3520,
      .precharge_current_ma = 30,
      .term_current_ma = 20,
      .enable_charging = true,
      .enable_adc = true,
  };
  _bms_driver = new BQ25629Bms(_i2c_bus, config, I2C_ADDR_BMS);
  if (!_bms_driver->init()) {
    AG_LOGE(TAG, "BMS init failed");
  }

  // --- V1 fuel-gauge bring-up ---
  if (_variant == BoardVariant::V1) {
    _fuel_gauge = new BQ27427(_i2c_bus);
    if (!_fuel_gauge->init()) {
      AG_LOGE(TAG, "BQ27427 init failed — FG offline");
      // Continue: _fuel_gauge stays non-null but ready() == false.
    } else {
      // Pass 1: read state with validity flags.
      uint16_t dc = 0;
      uint16_t fcc = 0;
      FgCellConfig current{};
      const bool dc_ok = _fuel_gauge->read_design_capacity_mah(dc);
      const bool fcc_ok = _fuel_gauge->read_full_charge_capacity_mah(fcc);
      const bool cfg_ok = _fuel_gauge->read_cell_config(current);

      FgRecoveryDecision decision =
          evaluate_fg_state(dc, dc_ok, fcc, fcc_ok, current, cfg_ok, AGO_CELL_CONFIG,
                            FG_DC_SANITY_MIN_MAH, FG_DC_SANITY_MAX_MAH, FG_FCC_SANITY_MAX_MAH);

      // Tracks whether `current` reflects a successful CellConfig read.
      bool cfg_current_ok = cfg_ok;

      if (decision.needs_factory_reset) {
        AG_LOGW(TAG, "BQ27427 corrupted state (dc=%u fcc=%u dc_ok=%d fcc_ok=%d) — resetting", dc,
                fcc, dc_ok, fcc_ok);
        if (!_fuel_gauge->reset_to_factory_defaults()) {
          AG_LOGE(TAG, "BQ27427 reset_to_factory_defaults() failed — "
                       "FG may be in inconsistent state; skipping config write");
          decision = {false, false};
        } else {
          // Pass 2: post-reset, re-read to drive needs_config_write.
          const bool dc2_ok = _fuel_gauge->read_design_capacity_mah(dc);
          const bool fcc2_ok = _fuel_gauge->read_full_charge_capacity_mah(fcc);
          const bool cfg2_ok = _fuel_gauge->read_cell_config(current);
          cfg_current_ok = cfg2_ok;
          decision =
              evaluate_fg_state(dc, dc2_ok, fcc, fcc2_ok, current, cfg2_ok, AGO_CELL_CONFIG,
                                FG_DC_SANITY_MIN_MAH, FG_DC_SANITY_MAX_MAH, FG_FCC_SANITY_MAX_MAH);
        }
      }

      if (decision.needs_config_write) {
        AG_LOGI(TAG, "BQ27427 applying cell config");
        if (!_fuel_gauge->write_cell_config(AGO_CELL_CONFIG)) {
          AG_LOGW(TAG, "BQ27427 write_cell_config() failed — cell parameters "
                       "not updated; runtime polling continues with whatever "
                       "the chip currently has");
        }
      } else if (cfg_current_ok) {
        AG_LOGI(TAG, "BQ27427 cell config already correct — preserved");
      } else {
        AG_LOGW(TAG, "BQ27427 cell config unreadable — left as-is");
      }

      // One-shot diagnostic snapshot.
      uint8_t soc = 0;
      uint16_t mv = 0;
      int16_t ma = 0;
      float tc = 0.0f;
      _fuel_gauge->read_soc_percent(soc);
      _fuel_gauge->read_voltage_mv(mv);
      _fuel_gauge->read_average_current_ma(ma);
      _fuel_gauge->read_internal_temperature_c(tc);
      AG_LOGI(TAG, "BQ27427 boot: soc=%u%% v=%umV i=%dmA t=%.1fC", soc, mv, ma, tc);
    }
  }

  _bms_ready = true;
}

void GoHardwareBoard::init_wifi_subsystem() {
  if (_wifi_inited)
    return;

  // Construct the HAL on demand if no accessor call beat us to it.
  // EspWifiHal::init() drives nvs_flash_init, esp_netif_init, the system
  // event loop, esp_wifi_init, default storage mode, event handlers, and
  // the single-shot timers used by Wi-Fi.  Idempotent inside the HAL as
  // well; the board-layer flag avoids re-entering the HAL call entirely
  // after the first success.
  WifiHal &hal = wifi_hal();
  const WifiStatus status = hal.init();
  if (status != WifiStatus::Ok) {
    AG_LOGE(TAG, "wifi subsystem init failed (status=%d)", static_cast<int>(status));
    return;
  }
  _wifi_inited = true;
}

void GoHardwareBoard::init_core() {
  init_nvs();
  init_buses();
  init_spi();
  init_bms();
}

// ===========================================================================
// Lazy service accessors
// ===========================================================================

ConfigStore &GoHardwareBoard::config_store() {
  assert(_nvs_ready && "config_store() requires init_nvs()");
  if (!_config_store) {
    _config_store = new NvsConfigStore("go");
  }
  return *_config_store;
}

GoSettings GoHardwareBoard::load_settings() {
  if (!_settings_loaded) {
    _settings = load_go_settings(config_store());
    print_settings(_settings);
    _settings_loaded = true;
  }
  return _settings;
}

BmsDevice &GoHardwareBoard::bms() {
  assert(_bms_ready && "bms() requires init_bms()");
  return *_bms_driver;
}

// ---------------------------------------------------------------------------
// Cold-boot PMID gate
// ---------------------------------------------------------------------------

/// Max time (ms) to wait for PMID to reach PMID_HEALTHY_MIN_MV at boot.
static constexpr uint32_t PMID_WAIT_TIMEOUT_MS = 500;

/// Poll interval (ms) while waiting for PMID at boot.
static constexpr uint32_t PMID_WAIT_POLL_MS = 50;

void GoHardwareBoard::_ensure_pmid_ready() {
  assert(_bms_driver && "init_bms() must precede _ensure_pmid_ready()");

  bool rekicked = false;
  uint32_t elapsed_ms = 0;
  while (elapsed_ms < PMID_WAIT_TIMEOUT_MS) {
    BmsTelemetry t{};
    if (_bms_driver->read_telemetry(t) && t.pmid_voltage_mv != BmsInvalid::VOLTAGE_MV &&
        t.pmid_voltage_mv >= PowerService::PMID_HEALTHY_MIN_MV) {
      AG_LOGI(TAG, "PMID ready: vpmid=%u mV (waited %lu ms)", t.pmid_voltage_mv,
              static_cast<unsigned long>(elapsed_ms));
      return;
    }

    // One re-kick attempt after the first poll sees a low rail.
    if (!rekicked) {
      AG_LOGW(TAG, "PMID low at boot (vpmid=%u mV) -> re-kick boost", t.pmid_voltage_mv);
      _bms_driver->set_pmid_enabled(false);
      RTOS::delay_ms(PowerService::PMID_REKICK_OFF_MS);
      _bms_driver->set_pmid_enabled(true);
      rekicked = true;
    }

    RTOS::delay_ms(PMID_WAIT_POLL_MS);
    elapsed_ms += PMID_WAIT_POLL_MS;
  }

  AG_LOGW(TAG, "PMID wait timed out after %lu ms — proceeding to SPS30 init",
          static_cast<unsigned long>(elapsed_ms));
}

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------

SensorManager &GoHardwareBoard::sensors(bool warm) {
  assert(_buses_ready && "sensors() requires init_buses()");
  assert(_bms_ready && "sensors() requires init_bms()");
  assert(_power_ready && "sensors() requires power()");
  if (!_sensor_manager) {
    auto *sgp41 = new SGP41(_i2c_bus, I2C_ADDR_SGP41);
    auto *sps30 = new SPS30(_i2c_bus);
    auto *dps368 = new DPS368(_i2c_bus, I2C_ADDR_DPS368);

    auto *s = new Sensors{};

    // Init DPS368 first: continuous mode starts immediately, giving the
    // pressure sensor time to produce its first measurement (~120 ms)
    if (dps368->init()) {
      s->pressure = dps368;
    } else {
      AG_LOGE(TAG, "DPS368 init failed");
    }

    s->co2 = init_co2_sensor(_i2c_bus);

    if (_variant == BoardVariant::V1) {
      auto *sht40 = new SHT40(_i2c_bus, I2C_ADDR_SHT40);
      if (sht40->init()) {
        s->temp_hum = sht40;
      } else {
        AG_LOGE(TAG, "SHT40 init failed");
      }
    }

    if (sgp41->init()) {
      s->tvoc_nox = sgp41;
    } else {
      AG_LOGE(TAG, "SGP41 init failed");
    }

    // Wait for PMID to reach healthy voltage before probing SPS30.
    _ensure_pmid_ready();

    if (sps30->init(warm)) {
      s->pms_a = sps30;
    } else {
      AG_LOGE(TAG, "SPS30 init failed");
    }

    s->temp_hum_a_fallback.priority[0] = TempHumSource::DEDICATED;
    s->temp_hum_a_fallback.priority[1] = TempHumSource::CO2;
    s->temp_hum_a_fallback.priority[2] = TempHumSource::PRESSURE;
    s->temp_hum_a_fallback.count = 3;

    _sensor_manager = new SensorManager(*s);
  }
  return *_sensor_manager;
}

StorageService &GoHardwareBoard::storage() {
  assert(_spi_ready && "storage() requires init_spi()");
  if (!_storage) {
    auto *rtc_storage = new RtcPayloadCacheStorage();
    auto *cache = new PayloadCache(*rtc_storage, PAYLOAD_CACHE_MAX_SIZE);

    SpiNandStorage::Config nand_config{};
    nand_config.spi_host = SPI_HOST;
    nand_config.cs_pin = PIN_NAND_CS;
    auto *nand = new SpiNandStorage(nand_config);

    _storage = new StorageService(*cache, *nand);
    _storage->restore_cache();
    if (!_storage->init()) {
      AG_LOGE(TAG, "NAND storage init failed");
    }
  }
  return *_storage;
}

DisplayService &GoHardwareBoard::display() {
  assert(_spi_ready && "display() requires init_spi()");
  if (!_display) {
    _display = new DisplayService({
        .spi_host = SPI_HOST,
        .pin_cs = PIN_DISPLAY_CS,
        .pin_dc = PIN_DISPLAY_DC,
        .pin_rst = PIN_DISPLAY_RST,
        .pin_busy = PIN_DISPLAY_BUSY,
    });
  }
  return *_display;
}

LedService &GoHardwareBoard::led_service() {
  assert(_buses_ready && "led_service() requires init_buses()");
  if (!_led_service) {
    LedService::Config cfg{};
    if (_variant == BoardVariant::V1) {
      _lp5036 = new LP5036(_i2c_bus, {});
      cfg.driver = _lp5036;
    }
    // Prototype: cfg.driver stays nullptr → inert mode
    _led_service = new LedService(cfg);
  }
  return *_led_service;
}

BuzzerService &GoHardwareBoard::buzzer_service() {
  assert(_buses_ready && "buzzer_service() requires init_buses()");
  if (!_buzzer_service) {
    BuzzerService::Config cfg{};
    if (_variant == BoardVariant::V1) {
      _ledc_buzzer = new LedcBuzzer({
          .pin = static_cast<int>(PIN_BUZZER),
          .default_freq_hz = BUZZER_FREQ_HZ,
      });
      cfg.driver = _ledc_buzzer;
    }
    // Prototype: cfg.driver stays nullptr -> inert mode
    _buzzer_service = new BuzzerService(cfg);
  }
  return *_buzzer_service;
}

// ===========================================================================
// Lazy radio accessors
//
// Construction is side-effect-free:
//   * EspWifiHal()        — pure C++ init; no driver calls until init().
//   * WifiManager(hal)    — only registers std::function callbacks on the HAL
//                           (esp_wifi_hal.cpp:425-442); never touches the
//                           driver.  Safe against an uninitialised HAL.
//   * IdfHttpServer()     — stores nothing until start().
//   * NimbleBleServer()   — defaulted; NimBLEDevice singleton untouched until
//                           init(name).
// ===========================================================================

WifiHal &GoHardwareBoard::wifi_hal() {
  if (!_wifi_hal) {
    _wifi_hal = new EspWifiHal();
  }
  return *_wifi_hal;
}

WifiManager &GoHardwareBoard::wifi_manager() {
  if (!_wifi_manager) {
    // Saved networks need NVS; reached only after init_nvs() on Stationary.
    assert(_nvs_ready && "wifi_manager() requires init_nvs()");
    if (!_wifi_creds_store) {
      _wifi_creds_store = new NvsConfigStore(WIFI_CREDS_NVS_NAMESPACE);
    }
    _wifi_manager = new WifiManager(wifi_hal(), *_wifi_creds_store);
  }
  return *_wifi_manager;
}

HttpServer &GoHardwareBoard::http_server() {
  if (!_http_server) {
    _http_server = new IdfHttpServer();
  }
  return *_http_server;
}

AgBleServer &GoHardwareBoard::ble_server() {
  if (!_ble_server) {
    _ble_server = new NimbleBleServer();
  }
  return *_ble_server;
}

AgClient &GoHardwareBoard::ag_client() {
  if (!_ag_client) {
    _ag_client = new AgClient();
    const std::string serial = build_serial_number();
    if (!_ag_client->begin(serial.c_str(), NetworkType::Wifi)) {
      AG_LOGE(TAG, "ag_client: begin() failed (serial=%s)", serial.c_str());
    }
  }
  return *_ag_client;
}

PowerService &GoHardwareBoard::power() {
  assert(_bms_ready && "power() requires init_bms()");
  if (!_power) {
    _power = new PowerService(*_bms_driver, gpio::native::hal,
                              {
                                  .pin_wake_button_power = PIN_BUTTON_POWER,
                                  .pin_wake_button_boot = -1,
                                  .pin_ext_wdt = PIN_EXT_WDT,
                                  .deep_sleep_threshold_ms = 5000,
                                  .pin_pm_power = PIN_PM_POWER,
                                  .pm_power_on_level = pm_power_on_level(_variant),
                                  .sensor_hold_max_sleep_ms = 20000,
                              });
    _power->init_ext_watchdog();
    _power->reset_ext_watchdog();
    if (_fuel_gauge != nullptr && _fuel_gauge->ready()) {
      _power->set_fuel_gauge(_fuel_gauge);
    }
    _power_ready = true;
  }
  return *_power;
}

// ===========================================================================
// Per-call factories
// ===========================================================================

GpsDriver *GoHardwareBoard::new_gps_driver() {
  auto *serial = new AirgradientUART(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
  return new GpsDriver(*serial);
}

CapTouchSensor *GoHardwareBoard::new_touch_sensor() {
  assert(_buses_ready && "new_touch_sensor() requires init_buses()");
  CAP1203::Config cfg;
  cfg.delta_sense = TOUCH_DELTA_SENSE;
  cfg.repeat_rate_channels = TouchChannel::CH2 | TouchChannel::CH3;
  auto *touch = new CAP1203(_i2c_bus, I2C_ADDR_CAP1203, cfg);
  if (!touch->init()) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }
  return touch;
}

// ===========================================================================
// Platform info
// ===========================================================================

BoardVariant GoHardwareBoard::variant() const {
  assert(_buses_ready && "variant() requires init_buses()");
  return _variant;
}

std::string GoHardwareBoard::serial_number() { return build_serial_number(); }

const char *GoHardwareBoard::firmware_version() { return esp_app_get_description()->version; }

const gpio::Hal &GoHardwareBoard::gpio_hal() { return gpio::native::hal; }

// ===========================================================================
// Hardware operations
// ===========================================================================

void GoHardwareBoard::release_gpio_holds() { PowerService::release_sleep_gpio_holds(PIN_PM_POWER); }

void GoHardwareBoard::ulp_stop() { ulp_wdt_stop(); }

void GoHardwareBoard::ulp_start() { ulp_wdt_start(); }

void GoHardwareBoard::install_button_isr(int pin, volatile bool *flag) {
  gpio_install_isr_service(0); // idempotent
  gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_NEGEDGE);
  gpio_isr_handler_add(
      static_cast<gpio_num_t>(pin), [](void *arg) { *static_cast<volatile bool *>(arg) = true; },
      const_cast<bool *>(flag));
}

void GoHardwareBoard::remove_button_isr(int pin) {
  gpio_isr_handler_remove(static_cast<gpio_num_t>(pin));
  gpio_set_intr_type(static_cast<gpio_num_t>(pin), GPIO_INTR_DISABLE);
}

// Returns true only once a valid measurement is read back — proof the fan is
// actually running (data-ready requires the sensor to be measuring). The caller
// retries until then. init() is idempotent here via _pm_fan_inited.
bool GoHardwareBoard::start_pm_fan() {
  if (_pm_fan == nullptr) {
    _pm_fan = new SPS30(_i2c_bus);
  }
  if (!_pm_fan_inited) {
    if (!_pm_fan->init(/*skip_reset=*/false)) {
      AG_LOGW(TAG, "start_pm_fan: SPS30 init failed (will retry)");
      return false;
    }
    _pm_fan_inited = true;
  }
  PMData pm{};
  if (_pm_fan->read(pm)) {
    AG_LOGI(TAG, "start_pm_fan: fan confirmed (PM2.5=%.1f)", static_cast<double>(pm.pm_25));
    return true;
  }
  AG_LOGW(TAG, "start_pm_fan: no valid PM read yet (will retry)");
  return false;
}

// Fan stops when EN_PM is cut (PowerService::set_pm_power(false)); losing power
// means the next start must re-init.
void GoHardwareBoard::stop_pm_fan() { _pm_fan_inited = false; }
