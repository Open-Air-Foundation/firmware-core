#pragma once

#include "go_board.h"
#include "go_settings.h"

#include <driver/i2c_master.h>

class AgClient;
class BQ25629Bms;
class BQ27427;
class SPS30;
class EspWifiHal;
class IdfHttpServer;
class LedcBuzzer;
class LP5036;
class NimbleBleServer;
class NvsConfigStore;
class WifiManager;

/// Real hardware implementation of GoBoard for the AGo board.
///
/// Wraps all ESP-IDF init calls, driver creation, and bus management.
/// Objects are heap-allocated and never freed (process lifetime).
class GoHardwareBoard : public GoBoard {
public:
  // --- Init methods ---
  void init_nvs() override;
  void init_buses() override;
  void init_spi() override;
  void init_bms() override;
  void init_wifi_subsystem() override;
  void init_core() override;

  // --- Lazy service accessors ---
  ConfigStore &config_store() override;
  GoSettings load_settings() override;
  BmsDevice &bms() override;
  SensorManager &sensors(bool warm) override;
  StorageService &storage() override;
  DisplayService &display() override;
  LedService &led_service() override;
  BuzzerService &buzzer_service() override;
  PowerService &power() override;

  // --- Lazy radio accessors ---
  WifiHal &wifi_hal() override;
  WifiManager &wifi_manager() override;
  HttpServer &http_server() override;
  AgBleServer &ble_server() override;
  AgClient &ag_client() override;

  // --- Per-call factories ---
  GpsDriver *new_gps_driver() override;
  CapTouchSensor *new_touch_sensor() override;

  // --- Platform ---
  BoardVariant variant() const override;
  std::string serial_number() override;
  const char *firmware_version() override;
  const gpio::Hal &gpio_hal() override;
  void release_gpio_holds() override;
  void ulp_stop() override;
  void ulp_start() override;
  void install_button_isr(int pin, volatile bool *flag) override;
  void remove_button_isr(int pin) override;
  bool start_pm_fan() override;
  void stop_pm_fan() override;

private:
  // Board variant (detected in init_buses, fail-safe default: Prototype)
  BoardVariant _variant = BoardVariant::Prototype;

  // Init tracking (idempotency)
  bool _nvs_ready = false;
  bool _buses_ready = false;
  bool _spi_ready = false;
  bool _bms_ready = false;
  bool _power_ready = false;
  bool _wifi_inited = false;

  // Cold-boot PMID gate — bounded wait + re-kick for SPS30 probe
  void _ensure_pmid_ready();

  // Bus handles
  i2c_master_bus_handle_t _i2c_bus = nullptr;

  // Owned objects (heap-allocated, never freed)
  NvsConfigStore *_config_store = nullptr;
  GoSettings _settings{};
  bool _settings_loaded = false;
  BQ25629Bms *_bms_driver = nullptr;
  BQ27427 *_fuel_gauge = nullptr;
  SPS30 *_pm_fan = nullptr; // dedicated PM-fan load for factory learning discharge
  bool _pm_fan_inited = false;
  SensorManager *_sensor_manager = nullptr;
  StorageService *_storage = nullptr;
  DisplayService *_display = nullptr;
  LP5036 *_lp5036 = nullptr;
  LedService *_led_service = nullptr;
  LedcBuzzer *_ledc_buzzer = nullptr;
  BuzzerService *_buzzer_service = nullptr;
  PowerService *_power = nullptr;

  // Radio infrastructure (lazy, never freed)
  EspWifiHal *_wifi_hal = nullptr;
  // Saved Wi-Fi networks; own NVS namespace, separate from "go" settings.
  NvsConfigStore *_wifi_creds_store = nullptr;
  WifiManager *_wifi_manager = nullptr;
  IdfHttpServer *_http_server = nullptr;
  NimbleBleServer *_ble_server = nullptr;
  AgClient *_ag_client = nullptr;
};
