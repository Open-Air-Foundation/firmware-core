#pragma once

#include "airgradient_gpio.h"
#include "go_board_capabilities.h"
#include "go_types.h"
#include "types/bms_types.h"

#include <cstdint>
#include <string>

/// Blank, unknown, or unreadable eFuse IDs retain legacy V1 behavior.
inline BoardVariant board_variant_from_efuse(uint8_t id, bool read_ok) {
  return read_ok && id == static_cast<uint8_t>(BoardVariant::V2) ? BoardVariant::V2
                                                                 : BoardVariant::V1;
}

// ---------------------------------------------------------------------------
// PM bus isolation — active-low EN_PM on V1.
// Host-safe constants shared by board initialization and PowerService.
// ---------------------------------------------------------------------------

inline constexpr uint8_t PM_POWER_ON_LEVEL = 0;
inline constexpr uint8_t PM_POWER_OFF_LEVEL = 1;

// ---------------------------------------------------------------------------
// Fuel-gauge bring-up decision helper (header-only, host-test-safe)
// ---------------------------------------------------------------------------

struct FgRecoveryDecision {
  bool needs_factory_reset; ///< True when DC or FCC is out of range
  bool needs_config_write;  ///< True when current FgCellConfig differs from target
};

/// Decide whether the BQ27427 needs a factory reset and / or a fresh
/// cell-config write based on what it currently has in persistent memory.
///
/// Each input value is paired with a _ok validity flag so the helper can
/// distinguish "I read this and it's bad" from "I could not read this at
/// all".  Transient I2C errors must NOT trigger a destructive reset /
/// config-write — only confidently-observed corruption does.
inline FgRecoveryDecision evaluate_fg_state(uint16_t current_design_capacity_mah, bool dc_ok,
                                            uint16_t current_full_charge_capacity_mah, bool fcc_ok,
                                            const FgCellConfig &current_cell_config, bool cfg_ok,
                                            const FgCellConfig &target_cell_config,
                                            uint16_t dc_sanity_min_mah, uint16_t dc_sanity_max_mah,
                                            uint16_t fcc_sanity_max_mah) {
  FgRecoveryDecision out{false, false};
  const bool dc_corrupt = dc_ok && (current_design_capacity_mah < dc_sanity_min_mah ||
                                    current_design_capacity_mah > dc_sanity_max_mah);
  const bool fcc_corrupt = fcc_ok && (current_full_charge_capacity_mah > fcc_sanity_max_mah);
  out.needs_factory_reset = dc_corrupt || fcc_corrupt;
  out.needs_config_write = cfg_ok && (current_cell_config != target_cell_config);
  return out;
}

// Forward declarations — avoid pulling full headers into the interface.
class AccelSensor;
class AgBleServer;
class AgClient;
class BmsDevice;
class BuzzerService;
class CapTouchSensor;
class ConfigStore;
class DisplayService;
class GpsDriver;
class HttpServer;
class LedService;
class PowerService;
class SensorManager;
class StorageService;
class WifiHal;
class WifiManager;
struct GoSettings;
struct GoInputConfig;

/// Abstract factory / BSP interface for the AGo board.
///
/// Owns the lifecycle of every hardware-touching object.  GoApp calls
/// init methods in boot-path-specific order, then accesses services via
/// lazy accessors.
///
/// On target, GoHardwareBoard implements this with real ESP-IDF drivers.
/// In tests, MockBoard implements this with stubs and mocks.
struct GoBoard {
  virtual ~GoBoard() = default;

  // -----------------------------------------------------------------
  // Init methods (fine-grained, idempotent)
  //
  // Each initialises one subsystem. Successful initialization is idempotent;
  // init_bms() remains retryable after failure. Boot paths call these in the
  // order their hardware sequencing requires.
  // -----------------------------------------------------------------

  virtual void init_nvs() = 0;   ///< NVS flash
  virtual void init_buses() = 0; ///< GPIO power enables + I2C bus + settling
  virtual void init_spi() = 0;   ///< SPI bus

  /// Initialize the optional fuel gauge (requires buses). Independent from
  /// charger initialization and idempotent after the first attempt.
  virtual void init_fuel_gauge() = 0;

  /// Initialize the BMS driver (requires buses). Returns true when the
  /// charger is available. A failed call may be retried.
  virtual bool init_bms() = 0;

  /// Initialise the Wi-Fi subsystem (NVS, netif, event loop, esp_wifi_init,
  /// event handlers, single-shot timers).  Idempotent.  **Not** called by
  /// init_core() — mode-conditional callers (Stationary entry) invoke it
  /// explicitly so Portable-only boots never pay the cost.
  virtual void init_wifi_subsystem() = 0;

  // -----------------------------------------------------------------
  // Convenience gate — initializes NVS, I2C, and SPI (skips what's done).
  // Fuel-gauge and BMS initialization remain explicit and independent.
  // -----------------------------------------------------------------

  virtual void init_core() = 0;

  // -----------------------------------------------------------------
  // Lazy service accessors
  //
  // Create-on-first-call.  Objects are owned by the board and live for
  // the duration of the process (never freed — the app never returns).
  // Callers must ensure prerequisite init methods have been called.
  // -----------------------------------------------------------------

  virtual ConfigStore &config_store() = 0;
  virtual GoSettings load_settings() = 0;
  /// Return the initialized charger, or nullptr when BMS initialization failed.
  virtual BmsDevice *bms() = 0;
  virtual SensorManager &sensors(bool warm = false) = 0;
  virtual StorageService &storage() = 0;
  virtual DisplayService &display() = 0;
  virtual LedService &led_service() = 0;
  virtual BuzzerService &buzzer_service() = 0;
  virtual PowerService &power() = 0;

  // -----------------------------------------------------------------
  // Lazy radio accessors
  //
  // These return references to shared radio infrastructure used by the
  // Stationary networking stack and provisioning.  Construction is lazy
  // and side-effect-free; the actual ESP-IDF Wi-Fi init is gated by
  // init_wifi_subsystem().  Portable mode borrows only ble_server().
  // -----------------------------------------------------------------

  virtual WifiHal &wifi_hal() = 0;
  virtual WifiManager &wifi_manager() = 0;
  virtual HttpServer &http_server() = 0;
  virtual AgBleServer &ble_server() = 0;

  /// Lazy AgClient accessor.  First call runs begin(serial, Wifi);
  /// sub-millisecond, no sockets.  Safe on every boot path.
  virtual AgClient &ag_client() = 0;

  // -----------------------------------------------------------------
  // Per-call factories (caller owns the returned object)
  // -----------------------------------------------------------------

  virtual GpsDriver *new_gps_driver() = 0;
  virtual CapTouchSensor *new_touch_sensor() = 0;
  virtual GoInputConfig input_config() = 0;

  /// Create the accelerometer on the shared I2C bus and init() it. Returns
  /// nullptr if the part is absent / init fails (WHO_AM_I mismatch); callers
  /// must null-check. WHO_AM_I detects absence.
  virtual AccelSensor *new_accel_sensor() = 0;
  /// Native shake interrupt pin; INVALID_PIN keeps only accelerometer diagnostics.
  virtual int accel_interrupt_pin() = 0;

  // -----------------------------------------------------------------
  // Platform info
  // -----------------------------------------------------------------

  /// Read board identity on first access, before any peripheral initialization.
  virtual BoardVariant variant() = 0;
  BoardCapabilities capabilities() { return board_capabilities(variant()); }
  virtual std::string serial_number() = 0;
  virtual const char *firmware_version() = 0;
  virtual const gpio::Hal &gpio_hal() = 0;

  // -----------------------------------------------------------------
  // Hardware operations
  // -----------------------------------------------------------------

  virtual void release_gpio_holds() = 0;
  virtual void ulp_stop() = 0;
  virtual void ulp_start() = 0;
  virtual void restart() = 0;

  // Factory learning: drive the SPS30 fan as a deliberate discharge load.
  // EN_PM (PowerService::set_pm_power) only powers the sensor; the fan spins
  // only while it is measuring. Default no-ops so test boards need no stub.

  /// Start SPS30 measurement (fan on). EN_PM must already be powered.
  virtual bool start_pm_fan() { return false; }

  /// Stop SPS30 measurement. Cutting EN_PM also stops the fan.
  virtual void stop_pm_fan() {}

  // -----------------------------------------------------------------
  // Button ISR management
  //
  // The fast path uses a GPIO ISR to detect button presses during
  // blocking operations.  The board owns ISR setup/teardown;
  // GoApp reads the volatile flag.
  // -----------------------------------------------------------------

  /// Install a falling-edge ISR on the given pin.  When triggered,
  /// the ISR sets *flag to true.
  virtual void install_button_isr(int pin, volatile bool *flag) = 0;

  /// Remove the ISR and disable the interrupt on the given pin.
  virtual void remove_button_isr(int pin) = 0;
};
