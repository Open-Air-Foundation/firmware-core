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
#include <esp_system.h>
#include <nvs_flash.h>

#include "ag_i2c.h"
#include "ag_log.h"
#include "airgradient_uart.h"
#include "backends/rtc_payload_cache_storage.h"
#include "cap1203.h"
#include "common.h"
#include "drivers/bq25629/bq25629_bms.h"
#include "drivers/bq27427/bq27427.h"
#include "drivers/bq27742/bq27742.h"
#include "drivers/dps368/dps368.h"
#include "drivers/s12/s12.h"
#include "drivers/scd4x/scd4x.h"
#include "drivers/sgp41/sgp41.h"
#include "drivers/sht40/sht40.h"
#include "drivers/sps30/sps30.h"
#include "drivers/stcc4/stcc4.h"
#include "accel/lis2dh12.h"
#include "expander_gpio.h"
#include "gps/gps_driver.h"
#include "native_gpio.h"
#include "tca6408a.h"
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

// v2.0 ships a different cell: Cowon INR18490NP, 2600 mAh min at 0.2C,
// 3.7 V nominal.  Terminate Voltage stays at 3.0 V on purpose — the cell allows
// 2.5 V, but 3.0 V keeps the gauge out of the knee where SOC gets noisy and
// leaves headroom above the protector's 2.7 V undervoltage trip.
static constexpr FgCellConfig AGO_CELL_CONFIG_V2 = {
    .design_capacity_mah = 2600,
    .design_energy_mwh = 9620,
    .terminate_voltage_mv = 3000,
    .sleep_current_ma = 50,
};

// The learned state copied off the one unit that runs a fuel-gauge learning
// cycle.  While qmax_mah is 0 this is a characterisation build: a fresh gauge
// only gets its Qmax seeded and is then left to learn, and a gauge that has
// already learned prints its image at boot as a C initialiser, ready to paste
// in here.  Filling it in turns every later unit into a production unit that
// installs the image and never runs a cycle of its own.
static constexpr FgGoldenImage AGO_FG_GOLDEN_V2 = {};

// Either no image (a characterisation build) or one that would survive the
// runtime guard.  A half-pasted image would otherwise compile and only fail
// once it reached a production unit.
static_assert(AGO_FG_GOLDEN_V2.qmax_mah == 0 || fg_golden_image_valid(AGO_FG_GOLDEN_V2),
              "AGO_FG_GOLDEN_V2 is filled in but incomplete — see fg_learning.md#golden-image");

// Print a captured image the way it has to be typed back in.  Each profile is
// 15 numbers, so the three fields go on their own lines.
static void format_ra_profile(const FgRaProfile &p, char *out, size_t len) {
  int n = snprintf(out, len, "{0x%04X, {", p.flag);
  for (size_t i = 0; i < FG_RA_TABLE_SIZE; ++i) {
    if (n < 0 || static_cast<size_t>(n) >= len) {
      return;
    }
    n += snprintf(out + n, len - static_cast<size_t>(n), "%s%d", i == 0 ? "" : ", ", p.ra[i]);
  }
  if (n > 0 && static_cast<size_t>(n) < len) {
    snprintf(out + n, len - static_cast<size_t>(n), "}}");
  }
}

// Gauge-side reference for detecting the end of a charge.  Flags()[FC] is only
// raised once Voltage() reaches Charging Voltage minus Taper Voltage (TRM
// SLUUAX0C §2.6.4).  TI's 4350 mV default against the 100 mV Taper Voltage puts
// that at 4250 mV, which a 4.20 V cell never reaches: FC could never set, the
// full-charge pause never engaged, and the learning run had no way out of its
// Charge stage.  Matched to the charger's own regulation voltage above.
static constexpr uint16_t AGO_FG_CHARGING_VOLTAGE_MV_V2 = 4200;

// Firmware-layer protection thresholds for the same cell.  TI's factory
// defaults suit a generic cell that tolerates 4.35 V and 55 °C charging; the
// Cowon INR18490NP is a 4.20 ± 0.05 V part rated 0–45 °C charge, −20–60 °C
// discharge, 2.50 V end-of-discharge (datasheet 4.7), so OV and OT Chg come
// down to sit inside those limits.
//
// UV sits between two things.  Above it, PowerService ships the device at
// 2.85 V, confirmed on two readings and acted on by the first poll of the boot
// it hands to; below it, the hardware UVP is 2.340 V once OVP code 000 is
// programmed.  2.7 V leaves 150 mV under the firmware's own trip, which is the
// few seconds of discharge between that decision and the screen being drawn —
// the reading that triggers it is already a loaded one, so there is no sag step
// to absorb on top.  It also keeps 200 mV over the cell's rated 2.50 V
// end-of-discharge, the widest margin of the pairings tried, which is why it
// stays here rather than following the trip down.
//
// Recovery is deliberately far above the trip.  The firmware layer re-closes
// the DSG FET on Voltage() alone (TRM §5.3.1.2), so a narrow band lets an
// unloaded cell spring back, re-close, sag, and trip again.  3.1 V is past
// where an empty cell rests, which turns the trip into a latch until charged.
static constexpr FgProtectionConfig AGO_PROTECTION_CONFIG = {
    .ov_prot_threshold_mv = 4250, // TI default 4390
    .ov_prot_recovery_mv = 4150,  // TI default 4290
    .uv_prot_threshold_mv = 2700, // TI default 2800
    .uv_prot_recovery_mv = 3100,  // TI default 2900 — 500 mV, anti-chatter
    .ot_chg_dc = 450,             // TI default 550
    .ot_chg_recovery_dc = 400,    // TI default 500
    .ot_dsg_dc = 600,             // TI default, already matches the cell
    .ot_dsg_recovery_dc = 550,    // TI default
};

// Hardware protector, written once per unit with an adapter attached.  Pack
// Configuration D gains CIFET and CSFET so the gauge opens the CHG FET itself
// outside the JEITA window (T1 = 0 °C, T4/T5 = 50/60 °C factory) instead of
// only reporting it — the charger never reads ChargingCurrent().  OVP code 000
// is the lowest hardware trip available, 4.275 V, and pairs the fixed UVP to
// 2.340 V; the factory 111 (4.450 V) never fires for a 4.20 V cell.  Prot OC
// Config stays at factory until R8 and the system peak current are measured.
static constexpr uint8_t AGO_PROTECTOR_PACK_CONFIG_D = 0xB3; // TI default 0x83
static constexpr uint8_t AGO_PROTECTOR_OV_CONFIG = 0x00;     // TI default 0x07 (4.450 V)

// FG DM corruption sanity ranges.  A reading outside any of these
// ranges is treated as evidence of a corrupted persistent block
// (most commonly a prior aborted CFGUPDATE).
static constexpr uint16_t FG_DC_SANITY_MIN_MAH = 500;
static constexpr uint16_t FG_DC_SANITY_MAX_MAH = 8000;
static constexpr uint16_t FG_FCC_SANITY_MAX_MAH = 8500;

// ===========================================================================
// Private helper: CO2 sensor detection
// ===========================================================================

static CO2Sensor *init_co2_sensor(i2c_master_bus_handle_t i2c_bus, bool sensors_warm,
                                  const char **selected_name) {
  *selected_name = "none";

  // 1. SenseAir S12 (no integrated T/RH)
  auto *s12 = new S12(i2c_bus, I2C_ADDR_S12);
  if (s12->init()) {
    AG_LOGI(TAG, "CO2 sensor: S12 selected");
    *selected_name = "S12 0x68";
    return s12;
  }
  AG_LOGW(TAG, "CO2 sensor: S12 not detected");
  delete s12;

  // 2. Sensirion SCD4x (with integrated T/RH)
  auto *scd4x = new SCD4x(i2c_bus, I2C_ADDR_SCD4X);
  if (scd4x->init(sensors_warm)) {
    AG_LOGI(TAG, "CO2 sensor: SCD4x selected");
    *selected_name = "SCD4x 0x62";
    return scd4x;
  }
  AG_LOGW(TAG, "CO2 sensor: SCD4x not detected");
  delete scd4x;

  // 3. Sensirion STCC4 (with integrated T/RH)
  auto *stcc4 = new STCC4(i2c_bus, I2C_ADDR_STCC4);
  if (stcc4->init()) {
    AG_LOGI(TAG, "CO2 sensor: STCC4 selected");
    *selected_name = "STCC4 0x64";
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

  if (!_i2c_ready) {
    RTOS::delay_ms(100);
    _init_i2c_and_variant();
    RTOS::delay_ms(100);
  }

  // Drive PM power to the variant-appropriate "ON" level.
  //   Prototype: IO26 already at 1 (safe-default above), no write needed.
  //   v1:        IO26 level 0 (active-low PM ON).
  //   v2:        EN_PM1 lives on the expander; IO26 is unconnected.
  switch (_variant) {
  case BoardVariant::V2:
    if (_expander != nullptr && _expander->ready()) {
      gpio::expander::hal.set_level(PIN_V2_PM_POWER, pm_power_on_level(_variant));
    }
    break;
  case BoardVariant::V1:
    hal.set_level(PIN_PM_POWER, pm_power_on_level(_variant));
    break;
  case BoardVariant::Prototype:
    break;
  }

  RTOS::delay_ms(100);
  _buses_ready = true;
}

void GoHardwareBoard::_init_i2c_and_variant() {
  if (_i2c_ready) {
    return;
  }

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

  // Board variant detection.  The TCA6408A expander exists only on v2.0; the
  // fuel gauge address 0x55 is shared by BQ27427 (v1.0) and BQ27742 (v2.0).
  constexpr int PROBE_TIMEOUT_MS = 100;
  const bool expander_present = i2c_device_present(_i2c_bus, I2C_ADDR_TCA6408A, PROBE_TIMEOUT_MS);
  const bool fg_present = i2c_device_present(_i2c_bus, I2C_ADDR_BQ27742, PROBE_TIMEOUT_MS);
  _variant = detect_board_variant(expander_present, fg_present);
  AG_LOGI(TAG, "board variant: %s (TCA6408A @ 0x20 %s, FG @ 0x55 %s)", board_variant_str(_variant),
          expander_present ? "ACK" : "NACK", fg_present ? "ACK" : "NACK");
  _label_chips();
  // The census matters most when detection went wrong, so it runs on every
  // variant (about 100 ms of probing).
  _log_i2c_census();

  if (_variant == BoardVariant::V2) {
    _init_expander();
  }
  _i2c_ready = true;
}

bool GoHardwareBoard::_init_expander() {
  if (_expander == nullptr) {
    _expander = new TCA6408A(_i2c_bus, {.address = I2C_ADDR_TCA6408A});
  }
  if (!_expander->init()) {
    AG_LOGE(TAG, "TCA6408A init failed — v2 control lines unavailable");
    _chips.set(Chip::Expander, false, "no ACK / readback");
    return false;
  }
  // Warm boots find the registers as we left them; a cold boot finds POR
  // values.  apply() writes outputs first, so no line glitches either way.
  if (!_expander->apply(V2_EXPANDER_OUTPUT_IDLE, V2_EXPANDER_CONFIG)) {
    AG_LOGE(TAG, "TCA6408A apply failed");
    _chips.set(Chip::Expander, false, "register write");
    return false;
  }
  gpio::expander::attach(_expander);
  AG_LOGI(TAG, "TCA6408A ready (output=0x%02X config=0x%02X)", _expander->cached_output(),
          _expander->cached_config());
  _chips.set(Chip::Expander, true);
  return true;
}

// Names and bus labels depend on the variant; verdicts arrive as each init
// runs.  Parts that are not fitted on this variant are marked Absent up
// front so the table never shows them as forgotten.
void GoHardwareBoard::_label_chips() {
  const bool proto = _variant == BoardVariant::Prototype;
  const bool v2 = _variant == BoardVariant::V2;

  _chips.label(Chip::Expander, "TCA6408A", "I2C 0x20");
  _chips.label(Chip::FuelGauge, v2 ? "BQ27742" : "BQ27427", "I2C 0x55");
  _chips.label(Chip::Charger, "BQ25628", "I2C 0x6A");
  _chips.label(Chip::TempHum, "SHT4x", "I2C 0x44");
  _chips.label(Chip::Voc, "SGP41", "I2C 0x59");
  _chips.label(Chip::Co2, "CO2", "I2C");
  _chips.label(Chip::Pressure, v2 ? "SPL07-003" : "DPS368", "I2C 0x77");
  _chips.label(Chip::Accel, "LIS2DH12", "I2C 0x18");
  _chips.label(Chip::Pm, "SPS30", "I2C 0x69");
  _chips.label(Chip::LedDriver, "LP5036", "I2C 0x33");
  _chips.label(Chip::Touch, "CAP1203", "I2C 0x28");
  _chips.label(Chip::Nand, "W25N512", v2 ? "SPI exp.P2" : "SPI IO4");
  _chips.label(Chip::Display, "SSD1680", "SPI IO0");
  _chips.label(Chip::Gps, "TAU1113", "UART1");

  if (!v2) {
    _chips.set(Chip::Expander, ChipState::Absent);
  }
  if (proto) {
    _chips.set(Chip::FuelGauge, ChipState::Absent);
    _chips.set(Chip::TempHum, ChipState::Absent);
    _chips.set(Chip::LedDriver, ChipState::Absent);
  }
}

void GoHardwareBoard::report_chip(Chip chip, bool ok) {
  // An inert service (e.g. LedService without a driver on Prototype) reports
  // success; keep the Absent verdict for parts this variant does not carry.
  if (_chips.entry(chip).state == ChipState::Absent) {
    return;
  }
  _chips.set(chip, ok);
}

// The accelerometer is only created on demand (Hardware Test), so give it a
// WHO_AM_I probe here if nothing has touched it yet.
void GoHardwareBoard::_probe_accel_for_report() {
  if (_chips.entry(Chip::Accel).state != ChipState::Untested || !_buses_ready) {
    return;
  }
  LIS2DH12::Config cfg;
  cfg.address = I2C_ADDR_LIS2DH12;
  LIS2DH12 accel(_i2c_bus, cfg);
  const bool ok = accel.init();
  _chips.set(Chip::Accel, ok, ok ? "WHO_AM_I 0x33" : "no WHO_AM_I");
}

void GoHardwareBoard::log_chip_report() {
  _probe_accel_for_report();

  char line[96];
  AG_LOGI(TAG, "chip report (%s):", board_variant_str(_variant));
  for (size_t i = 0; i < ChipReport::COUNT; ++i) {
    _chips.format_line(static_cast<Chip>(i), line, sizeof(line));
    AG_LOGI(TAG, "  %s", line);
  }
  _chips.format_summary(line, sizeof(line));
  AG_LOGI(TAG, "  %s", line);
}

// Bring-up aid: one line listing every ACKing 7-bit address, so the serial
// log doubles as the I2C census (plan P3-03).
void GoHardwareBoard::_log_i2c_census() {
  constexpr int CENSUS_TIMEOUT_MS = 20;
  char line[160];
  size_t used = 0;
  for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
    if (!i2c_device_present(_i2c_bus, addr, CENSUS_TIMEOUT_MS)) {
      continue;
    }
    const int n = snprintf(line + used, sizeof(line) - used, " 0x%02X", addr);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(line) - used) {
      break;
    }
    used += static_cast<size_t>(n);
  }
  AG_LOGI(TAG, "I2C census:%s", used ? line : " (none)");
}

int GoHardwareBoard::_pm_power_pin() const {
  return _variant == BoardVariant::V2 ? PIN_V2_PM_POWER : static_cast<int>(PIN_PM_POWER);
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

bool GoHardwareBoard::init_bms() {
  if (_bms_driver != nullptr) {
    return true;
  }
  _bms_init_attempted = true;

  // Termination and pre-charge follow the cell, so they differ per variant.
  //
  // v2.0 (Cowon INR18490NP, 2600 mAh) specifies end-of-charge at 0.02C = 52 mA;
  // 50 mA is the nearest 5 mA step and the lowest setting TI characterises
  // tightly (ITERM_ACC +-17% at 50 mA, against +-80% at 10 mA, SLUSEG4C
  // §6.5).  Pre-charge at 200 mA is 0.077C, inside the usual 0.1C ceiling, and
  // it matters on this board: the gauge's undervoltage trip leaves the cell
  // between 2.7 V and the 3.0 V fast-charge threshold, and pre-charge is the
  // current that climbs back out of that band.
  //
  // v1 keeps its own numbers.  Its cell is a different 2000 mAh part and its
  // datasheet is not on hand, so there is nothing to justify changing them.
  const bool cell_v2 = (_variant == BoardVariant::V2);
  const drivers::BQ25629_Config config = {
      .charge_voltage_mv = 4200,
      .charge_current_ma = 500,
      // Input current is the lower of this register and the BQ25628's ILIM
      // pin; R21 = 2.49k puts that ceiling near 1.0 A, so anything higher
      // here is fiction.  EN_EXTILIM stays enabled: IINDPM alone comes up at
      // 3.2 A after a POR, the ILIM pin does not.
      .input_current_limit_ma = 1000,
      .input_voltage_limit_mv = 4600,
      .min_system_voltage_mv = 3520,
      .precharge_current_ma = static_cast<uint16_t>(cell_v2 ? 200 : 30),
      .term_current_ma = static_cast<uint16_t>(cell_v2 ? 50 : 20),
      .enable_charging = true,
      .enable_adc = true,
  };
  _bms_driver = new BQ25629Bms(_i2c_bus, config, I2C_ADDR_BMS);
  if (!_bms_driver->init()) {
    AG_LOGE(TAG, "BMS init failed");
    delete _bms_driver;
    _bms_driver = nullptr;
    _chips.set(Chip::Charger, false);
    return false;
  }
  _chips.set(Chip::Charger, true);
  AG_LOGI(TAG, "BQ25628 charge profile: ICHG %u mA, term %u mA, precharge %u mA",
          config.charge_current_ma, config.term_current_ma, config.precharge_current_ma);

  if (_power != nullptr) {
    _power->set_bms(_bms_driver);
  }

  // Every boot path brings the gauge up before the charger, so this is the
  // first point where the adapter state the protector write needs is known.
  if (_variant == BoardVariant::V2) {
    _apply_fuel_gauge_protector();
  }
  return true;
}

void GoHardwareBoard::init_fuel_gauge() {
  if (_fuel_gauge_init_attempted) {
    return;
  }
  _fuel_gauge_init_attempted = true;

  switch (_variant) {
  case BoardVariant::V1:
    _init_fuel_gauge_v1();
    break;
  case BoardVariant::V2:
    _init_fuel_gauge_v2();
    break;
  case BoardVariant::Prototype:
    break;
  }
}

// v2.0: bq27742-G1.  Same sanity/config-write decision as v1, but never a
// factory RESET — on this part a reset opens both protection FETs for a
// moment, which drops Pack+ and reboots the system when on battery.
void GoHardwareBoard::_init_fuel_gauge_v2() {
  // 100 kHz, not the bus's 400 kHz: the gauge clock-stretches while it serves a
  // Control() result, and at 400 kHz the incremental 2-byte read of that
  // register comes back corrupted (DEVICE_TYPE read 0x??72 across boots while
  // Voltage() and even AtRate() writes were fine).  At 100 kHz it reads 0x0742.
  // Only this device is slowed; the other I2C devices keep their own speed.
  _fuel_gauge_v2 = new BQ27742(_i2c_bus, {.address = I2C_ADDR_BQ27742, .scl_speed_hz = 100000});
  if (!_fuel_gauge_v2->init()) {
    AG_LOGE(TAG, "BQ27742 init failed — FG offline");
    _chips.set(Chip::FuelGauge, false, "DEVICE_TYPE");
    return;
  }
  _chips.set(Chip::FuelGauge, true, "DEVICE_TYPE 0x0742");
  // Every data-flash step below logs and continues, because failing one leaves
  // TI's defaults, which are safer than the targets in every direction.  The
  // chip report still has to say so, or a bench run reads PASS while the cell
  // is on factory thresholds.
  bool df_ok = true;

  uint16_t dc = 0;
  uint16_t fcc = 0;
  FgCellConfig current{};
  const bool dc_ok = _fuel_gauge_v2->read_design_capacity_mah(dc);
  const bool fcc_ok = _fuel_gauge_v2->read_full_charge_capacity_mah(fcc);
  const bool cfg_ok = _fuel_gauge_v2->read_cell_config(current);

  const FgRecoveryDecision decision =
      evaluate_fg_state(dc, dc_ok, fcc, fcc_ok, current, cfg_ok, AGO_CELL_CONFIG_V2,
                        FG_DC_SANITY_MIN_MAH, FG_DC_SANITY_MAX_MAH, FG_FCC_SANITY_MAX_MAH);
  if (decision.needs_factory_reset) {
    AG_LOGW(TAG,
            "BQ27742 state out of range (dc=%u fcc=%u) — reset skipped on v2, "
            "program data flash with bqStudio",
            dc, fcc);
  }
  if (decision.needs_config_write) {
    AG_LOGI(TAG, "BQ27742 applying cell config (had DC=%u DE=%u TermV=%u SleepI=%u)",
            current.design_capacity_mah, current.design_energy_mwh, current.terminate_voltage_mv,
            current.sleep_current_ma);
    if (!_fuel_gauge_v2->write_cell_config(AGO_CELL_CONFIG_V2)) {
      AG_LOGW(TAG, "BQ27742 write_cell_config() failed — cell parameters not updated");
      df_ok = false;
    }
  } else if (cfg_ok) {
    AG_LOGI(TAG, "BQ27742 cell config already correct — preserved");
  } else {
    AG_LOGW(TAG, "BQ27742 cell config unreadable — left as-is");
  }

  uint16_t charging_voltage = 0;
  if (!_fuel_gauge_v2->read_charging_voltage_mv(charging_voltage)) {
    AG_LOGW(TAG, "BQ27742 Charging Voltage unreadable — left as-is");
  } else if (charging_voltage != AGO_FG_CHARGING_VOLTAGE_MV_V2) {
    AG_LOGI(TAG, "BQ27742 applying Charging Voltage %u → %u mV", charging_voltage,
            AGO_FG_CHARGING_VOLTAGE_MV_V2);
    if (!_fuel_gauge_v2->write_charging_voltage_mv(AGO_FG_CHARGING_VOLTAGE_MV_V2)) {
      AG_LOGW(TAG, "BQ27742 write_charging_voltage_mv() failed — FC will not set");
      df_ok = false;
    }
  } else {
    AG_LOGI(TAG, "BQ27742 Charging Voltage already correct — preserved");
  }

  FgProtectionConfig prot_current{};
  if (!_fuel_gauge_v2->read_protection_config(prot_current)) {
    AG_LOGW(TAG, "BQ27742 protection config unreadable — left as-is");
  } else {
    AG_LOGI(TAG,
            "BQ27742 protection config: OV %u/%u mV, UV %u/%u mV, OT chg %.1f/%.1f °C, "
            "dsg %.1f/%.1f °C",
            prot_current.ov_prot_threshold_mv, prot_current.ov_prot_recovery_mv,
            prot_current.uv_prot_threshold_mv, prot_current.uv_prot_recovery_mv,
            prot_current.ot_chg_dc / 10.0f, prot_current.ot_chg_recovery_dc / 10.0f,
            prot_current.ot_dsg_dc / 10.0f, prot_current.ot_dsg_recovery_dc / 10.0f);
    if (prot_current != AGO_PROTECTION_CONFIG) {
      AG_LOGI(TAG, "BQ27742 applying protection config");
      if (!_fuel_gauge_v2->write_protection_config(AGO_PROTECTION_CONFIG)) {
        AG_LOGW(TAG, "BQ27742 write_protection_config() failed — thresholds not updated");
        df_ok = false;
      }
    } else {
      AG_LOGI(TAG, "BQ27742 protection config already correct — preserved");
    }
  }

  // Impedance Track starts from Qmax Cell 0, so it has to hold the design
  // capacity before IT_ENABLE.  Afterwards the value is the gauge's own learned
  // one and must never be written back, which is what Update Status gates: it
  // is 0 only while IT has never been enabled on this part.
  uint8_t update_status = 0;
  uint16_t qmax_mah = 0;
  const bool status_ok = _fuel_gauge_v2->read_update_status(update_status) &&
                         _fuel_gauge_v2->read_qmax_cell0(qmax_mah);
  if (!status_ok) {
    AG_LOGW(TAG, "BQ27742 Qmax / Update Status unreadable — Qmax left as-is");
  }
  switch (fg_decide_install(status_ok, update_status, qmax_mah,
                            fg_golden_image_valid(AGO_FG_GOLDEN_V2),
                            AGO_CELL_CONFIG_V2.design_capacity_mah)) {
  case FgInstallAction::None:
    if (status_ok) {
      AG_LOGI(TAG, "BQ27742 learned state left alone (Update Status 0x%02X, Qmax %u mAh)",
              update_status, qmax_mah);
    }
    break;

  case FgInstallAction::DumpImage:
    AG_LOGI(TAG, "BQ27742 Impedance Track running (Update Status 0x%02X) — Qmax %u mAh is the "
                 "gauge's own",
            update_status, qmax_mah);
    _log_fg_golden_image(); // characterisation build: hand the result over
    break;

  case FgInstallAction::SeedQmax:
    AG_LOGI(TAG, "BQ27742 seeding Qmax %u → %u mAh before learning", qmax_mah,
            AGO_CELL_CONFIG_V2.design_capacity_mah);
    if (!_fuel_gauge_v2->write_qmax_cell0(AGO_CELL_CONFIG_V2.design_capacity_mah)) {
      AG_LOGW(TAG, "BQ27742 write_qmax_cell0() failed — Qmax not seeded");
      df_ok = false;
    }
    break;

  case FgInstallAction::InstallImage:
    AG_LOGI(TAG, "BQ27742 installing golden image (Qmax %u → %u mAh)", qmax_mah,
            AGO_FG_GOLDEN_V2.qmax_mah);
    if (!_fuel_gauge_v2->write_golden_image(AGO_FG_GOLDEN_V2)) {
      AG_LOGW(TAG, "BQ27742 write_golden_image() failed — retried on the next boot");
      df_ok = false;
      break;
    }
    [[fallthrough]]; // image is in; the same IT_ENABLE finishes the job

  case FgInstallAction::StartImpedanceTrack:
    // Reached on a later boot too, when the image went in but IT_ENABLE did
    // not: Update Status 0x02 is an unfinished install, not a gauge to leave
    // alone.  IT_ENABLE latches QEN, so it only runs behind a verified image.
    if (!_fuel_gauge_v2->set_update_status_learning(true)) {
      AG_LOGW(TAG, "BQ27742 IT_ENABLE failed — image installed, Impedance Track still off");
      df_ok = false;
    }
    break;
  }

  if (!df_ok) {
    _chips.set(Chip::FuelGauge, false, "data flash write failed");
  }

  uint8_t soc = 0;
  uint16_t mv = 0;
  int16_t ma = 0;
  float tc = 0.0f;
  float tpack = 0.0f;
  uint16_t flags = 0;
  uint16_t safety = 0;
  uint8_t prot = 0;
  uint8_t state = 0;
  _fuel_gauge_v2->read_soc_percent(soc);
  _fuel_gauge_v2->read_voltage_mv(mv);
  _fuel_gauge_v2->read_average_current_ma(ma);
  _fuel_gauge_v2->read_internal_temperature_c(tc);
  _fuel_gauge_v2->read_temperature_c(tpack);
  _fuel_gauge_v2->read_flags_raw(flags);
  _fuel_gauge_v2->read_safety_status(safety);
  _fuel_gauge_v2->read_protector_status(prot);
  _fuel_gauge_v2->read_protector_state(state);
  AG_LOGI(TAG, "BQ27742 boot: soc=%u%% v=%umV i=%dmA tint=%.1fC tpack=%.1fC", soc, mv, ma, tc,
          tpack);
  AG_LOGI(TAG,
          "BQ27742 boot: flags=0x%04X safety=0x%04X protector=0x%02X (CHG %s, DSG %s) state=0x%02X",
          flags, safety, prot, (prot & BQ27742::ProtectorStatus::CHG_OFF) ? "OFF" : "on",
          (prot & BQ27742::ProtectorStatus::DSG_OFF) ? "OFF" : "on", state);
}

void GoHardwareBoard::_log_fg_golden_image() {
  FgGoldenImage img{};
  if (!_fuel_gauge_v2->read_golden_image(img)) {
    AG_LOGW(TAG, "BQ27742 golden image unreadable");
    return;
  }
  // The printed Update Status is the shipping one, not the 0x06 this gauge
  // reads: bit 2 is IT_ENABLE's to set on each unit, so the line stays
  // directly pasteable.  The real value only decides the warning below.
  FgGoldenImage as_shipped = img;
  as_shipped.update_status = FG_GOLDEN_UPDATE_STATUS;

  char buf[320];
  AG_LOGI(TAG, "BQ27742 golden image: .qmax_mah = %u, .update_status = 0x%02X,",
          as_shipped.qmax_mah, as_shipped.update_status);
  format_ra_profile(img.ra0, buf, sizeof(buf));
  AG_LOGI(TAG, "BQ27742 golden image: .ra0 = %s,", buf);
  format_ra_profile(img.ra0x, buf, sizeof(buf));
  AG_LOGI(TAG, "BQ27742 golden image: .ra0x = %s,", buf);

  if (img.update_status != FG_UPDATE_STATUS_LEARNED) {
    AG_LOGW(TAG,
            "BQ27742 learning is not finished (Update Status 0x%02X, needs 0x%02X) — "
            "do not paste this in",
            img.update_status, FG_UPDATE_STATUS_LEARNED);
  } else if (!fg_golden_image_valid(as_shipped)) {
    AG_LOGW(TAG, "BQ27742 golden image is incomplete — do not paste it in");
  }
}

// Hardware protector.  Separate from _init_fuel_gauge_v2() because the write
// needs the charger: between the subclass 64 commit and the Prot Checksum the
// gauge can open both FETs, which on battery cuts the system mid-sequence.
void GoHardwareBoard::_apply_fuel_gauge_protector() {
  if (_fuel_gauge_v2 == nullptr || !_fuel_gauge_v2->ready()) {
    return;
  }

  FgProtectorConfig hw{};
  if (!_fuel_gauge_v2->read_protector_config(hw)) {
    AG_LOGW(TAG, "BQ27742 protector config unreadable — left as-is");
    return;
  }
  const uint16_t expected = fg_protector_checksum(hw.prot_oc_config, hw.prot_ov_config);
  AG_LOGI(TAG,
          "BQ27742 protector config: PackCfgD=0x%02X ProtOC=0x%02X ProtOV=0x%02X ProtChk=0x%04X%s",
          hw.pack_config_d, hw.prot_oc_config, hw.prot_ov_config, hw.prot_checksum,
          hw.prot_checksum == expected ? "" : " (MISMATCH — gauge holds both FETs open)");

  if (hw.pack_config_d == AGO_PROTECTOR_PACK_CONFIG_D &&
      hw.prot_ov_config == AGO_PROTECTOR_OV_CONFIG && hw.prot_checksum == expected) {
    AG_LOGI(TAG, "BQ27742 protector config already correct — preserved");
    return;
  }

  BmsStatus status{};
  const bool status_ok = _bms_driver != nullptr && _bms_driver->read_status(status);
  if (!status_ok || !status.is_power_source_valid()) {
    AG_LOGW(TAG, "BQ27742 protector config differs — charger status unavailable, write deferred");
    return;
  }
  if (status.power_source == BmsPowerSource::None ||
      status.power_source == BmsPowerSource::OtgMode) {
    AG_LOGW(TAG, "BQ27742 protector config differs — on battery, write deferred until USB");
    return;
  }

  AG_LOGI(TAG, "BQ27742 applying protector config");
  if (!_fuel_gauge_v2->write_protector_config(AGO_PROTECTOR_PACK_CONFIG_D,
                                              AGO_PROTECTOR_OV_CONFIG)) {
    AG_LOGE(TAG, "BQ27742 write_protector_config() failed — retried at next boot on USB");
    _chips.set(Chip::FuelGauge, false, "protector write failed");
  }

  uint16_t safety = 0;
  uint8_t prot = 0;
  uint8_t state = 0;
  _fuel_gauge_v2->read_safety_status(safety);
  _fuel_gauge_v2->read_protector_status(prot);
  _fuel_gauge_v2->read_protector_state(state);
  AG_LOGI(
      TAG,
      "BQ27742 after protector write: safety=0x%04X protector=0x%02X (CHG %s, DSG %s) state=0x%02X",
      safety, prot, (prot & BQ27742::ProtectorStatus::CHG_OFF) ? "OFF" : "on",
      (prot & BQ27742::ProtectorStatus::DSG_OFF) ? "OFF" : "on", state);
}

// v1.0: BQ27427.
void GoHardwareBoard::_init_fuel_gauge_v1() {
  {
    _fuel_gauge = new BQ27427(_i2c_bus);
    _chips.set(Chip::FuelGauge, _fuel_gauge->init(), "DEVICE_TYPE 0x0427");
    if (!_fuel_gauge->ready()) {
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

BmsDevice *GoHardwareBoard::bms() { return _bms_driver; }

// ---------------------------------------------------------------------------
// Cold-boot PMID gate
// ---------------------------------------------------------------------------

/// Max time (ms) to wait for PMID to reach PMID_HEALTHY_MIN_MV at boot.
static constexpr uint32_t PMID_WAIT_TIMEOUT_MS = 500;

/// Poll interval (ms) while waiting for PMID at boot.
static constexpr uint32_t PMID_WAIT_POLL_MS = 50;

void GoHardwareBoard::_ensure_pmid_ready() {
  if (_bms_driver == nullptr) {
    AG_LOGW(TAG, "PMID readiness check skipped: BMS unavailable");
    return;
  }

  // Nothing to wait for while the cell is under the charger's own boost
  // threshold: the rail will not come up, the re-kick draws an inrush the cell
  // cannot take, and the half second spent here is half a second not spent
  // getting to the shutdown path.
  BmsTelemetry probe{};
  if (_bms_driver->read_telemetry(probe) && probe.is_battery_voltage_valid() &&
      probe.battery_voltage < PowerService::PMID_BOOST_MIN_BATTERY_V) {
    AG_LOGW(TAG, "PMID readiness check skipped: cell at %.2fV is below the boost threshold",
            static_cast<double>(probe.battery_voltage));
    return;
  }

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
  assert(_bms_init_attempted && "sensors() requires init_bms()");
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
      const bool spl07 = dps368->variant() == DPS368::Variant::SPL07003;
      _chips.label(Chip::Pressure, spl07 ? "SPL07-003" : "DPS368", "I2C 0x77");
      _chips.set(Chip::Pressure, true, spl07 ? "ID 0x11" : "ID 0x10");
      // The SPL07-003 only exists on v2.0 boards: seeing it without the
      // expander means the TCA6408A never ACKed, and every verdict that
      // depends on the expander (PM, NAND, touch, LED, SHT4x) is void.
      if (spl07 && _variant != BoardVariant::V2) {
        AG_LOGW(TAG,
                "SPL07-003 (v2.0 part) found but board detected as %s: TCA6408A @0x20 "
                "did not ACK — check U17 ~RESET pull-up (R2), VCCP/VCCI, ADDR",
                board_variant_str(_variant));
        _chips.set(Chip::Expander, ChipState::Fail, "no ACK, check R2");
      }
    } else {
      AG_LOGE(TAG, "DPS368 init failed");
      _chips.set(Chip::Pressure, false);
    }

    const char *co2_name = nullptr;
    s->co2 = init_co2_sensor(_i2c_bus, warm, &co2_name);
    _chips.set(Chip::Co2, s->co2 != nullptr, co2_name);

    if (_variant != BoardVariant::Prototype) {
      auto *sht40 = new SHT40(_i2c_bus, I2C_ADDR_SHT40);
      if (sht40->init()) {
        s->temp_hum = sht40;
      } else {
        AG_LOGE(TAG, "SHT40 init failed");
      }
      _chips.set(Chip::TempHum, s->temp_hum != nullptr);
    }

    if (sgp41->init()) {
      s->tvoc_nox = sgp41;
    } else {
      AG_LOGE(TAG, "SGP41 init failed");
    }
    _chips.set(Chip::Voc, s->tvoc_nox != nullptr);

    // Wait for PMID to reach healthy voltage before probing SPS30.
    _ensure_pmid_ready();

    if (sps30->init(warm)) {
      s->pms_a = sps30;
    } else {
      AG_LOGE(TAG, "SPS30 init failed");
    }
    _chips.set(Chip::Pm, s->pms_a != nullptr,
               _variant == BoardVariant::Prototype ? nullptr : "via TMUX121");

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
    if (_variant == BoardVariant::V2) {
      // NAND CS sits on the I2C expander: software chip-select per transaction.
      nand_config.cs_hal = &gpio::expander::hal;
      nand_config.cs_hal_pin = PIN_V2_NAND_CS;
    } else {
      nand_config.cs_pin = PIN_NAND_CS;
    }
    auto *nand = new SpiNandStorage(nand_config);

    _storage = new StorageService(*cache, *nand);
    _storage->restore_cache();
    const bool mounted = _storage->init();
    if (!mounted) {
      AG_LOGE(TAG, "NAND storage init failed");
    }
    _chips.set(Chip::Nand, mounted, mounted ? "mounted" : "mount failed");
  }
  return *_storage;
}

DisplayService &GoHardwareBoard::display() {
  assert(_spi_ready && "display() requires init_spi()");
  if (!_display) {
    // The early-paint paths reach here before init_buses().  The D/C pin is
    // variant-dependent (expander on v2), so detect the variant first; this
    // is the no-delay subset of init_buses() and costs a few milliseconds.
    _init_i2c_and_variant();
    _display = new DisplayService({
        .spi_host = SPI_HOST,
        .pin_cs = PIN_DISPLAY_CS,
        .pin_dc =
            _variant == BoardVariant::V2 ? PIN_V2_DISPLAY_DC : static_cast<int>(PIN_DISPLAY_DC),
        .pin_rst = PIN_DISPLAY_RST,
        .pin_busy = PIN_DISPLAY_BUSY,
        .gpio = &gpio_hal(),
    });
  }
  return *_display;
}

LedService &GoHardwareBoard::led_service() {
  assert(_buses_ready && "led_service() requires init_buses()");
  if (!_led_service) {
    LedService::Config cfg{};
    if (_variant != BoardVariant::Prototype) {
      _lp5036 = new LP5036(_i2c_bus, {});
      cfg.driver = _lp5036;
      cfg.map = _variant == BoardVariant::V2 ? LedMap::v2() : LedMap::v1();
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
    if (_variant != BoardVariant::Prototype) {
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
  assert(_fuel_gauge_init_attempted && "power() requires init_fuel_gauge()");
  assert(_bms_init_attempted && "power() requires init_bms()");
  if (!_power) {
    _power = new PowerService(_bms_driver, gpio_hal(),
                              {
                                  .pin_wake_button_power = PIN_BUTTON_POWER,
                                  .pin_wake_button_boot = -1,
                                  .pin_ext_wdt = PIN_EXT_WDT,
                                  .deep_sleep_threshold_ms = 5000,
                                  .pin_pm_power = _pm_power_pin(),
                                  .pm_power_on_level = pm_power_on_level(_variant),
                                  .sensor_hold_max_sleep_ms = 20000,
                                  .fg_has_protector = (_variant == BoardVariant::V2),
                              });
    _power->init_ext_watchdog();
    _power->reset_ext_watchdog();
    if (_fuel_gauge != nullptr && _fuel_gauge->ready()) {
      _power->set_fuel_gauge(_fuel_gauge);
    } else if (_fuel_gauge_v2 != nullptr && _fuel_gauge_v2->ready()) {
      _power->set_fuel_gauge(_fuel_gauge_v2);
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
  // A UART has no identity to verify at init; NMEA arrival is the runtime
  // check (GPS Test screen / gps_service log), so the report only notes it.
  _chips.set(Chip::Gps, ChipState::Untested, "runtime: NMEA");
  return new GpsDriver(*serial);
}

CapTouchSensor *GoHardwareBoard::new_touch_sensor() {
  assert(_buses_ready && "new_touch_sensor() requires init_buses()");
  CAP1203::Config cfg;
  cfg.delta_sense = TOUCH_DELTA_SENSE;
  // The Enter pad needs clean press/release edges for its gesture FSM, so
  // it is the one channel without the repeat rate.
  cfg.repeat_rate_channels = static_cast<uint8_t>(TouchChannel::ALL & ~touch_channel_map().enter);
  auto *touch = new CAP1203(_i2c_bus, I2C_ADDR_CAP1203, cfg);
  const bool ok = touch->init();
  if (!ok) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }
  _chips.set(Chip::Touch, ok, ok ? nullptr : "check CN4 cable");
  return touch;
}

// ---------------------------------------------------------------------------
// Touch diagnostics (go/test/touch-diag branch only)
//
// A guided measurement, not a log dump.  The question this has to answer is
// what each pad peaks at, and what the other two read at that same instant,
// because the last free-running run could not tell a press on CS1 from a
// press on CS1 that dragged CS3 with it.  So the run asks for one pad at a
// time, finds the press itself, and reports the peak sample rather than
// leaving a human to pick it out of the scrollback.
//
// Reads are non-disturbing: on this part nothing clears a latch except an
// explicit write to MAIN_CONTROL.
// ---------------------------------------------------------------------------

static constexpr const char *TOUCH_DIAG_TAG = "TOUCHDIAG";

// The chip publishes a new delta once per sensing cycle.  Register 0x24 is
// left at its POR value 0x39 — AVG 8 samples, SAMP_TIME 1.28 ms, CYCLE_TIME
// 70 ms — and three channels at 8 x 1.28 ms = 31 ms fits inside that, so the
// programmed 70 ms is what holds (datasheet 5.10, p.31).  Poll at well under
// half a cycle so no update is missed; the previous 500 ms poll read one
// value in seven and could miss the peak of a two-second press entirely.
// Three FreeRTOS ticks at the project's 100 Hz, so the request is exact.
static constexpr uint32_t TOUCH_DIAG_PERIOD_MS = 30;

// Gains the run walks through, as DELTA_SENSE codes.  Only 8x, the gain rev
// 2.0 ships at: on the new enclosure the 16x-against-2x check came out at
// 6.9, 5.1 and 8.4 where 8.0 was expected, so a peak measured at one gain may
// not be carried to another and the threshold has to come from 8x itself.
// List a second gain to bring the multiplier check back.
static constexpr uint8_t TOUCH_DIAG_GAINS[] = {4}; // 8x

// Press segmentation, quoted at 16x and scaled with the gain.  The idle band
// at 16x measures about one count and the weakest pad peaks near 50, so 12
// sits an order of magnitude above the noise and well below any real press.
static constexpr int TOUCH_DIAG_ENTER_AT_16X = 12;
// At 2x the scaled entry level rounds down to 1, which the idle band alone
// would trip.
static constexpr int TOUCH_DIAG_ENTER_FLOOR = 3;

// A press has to hold above the exit level for this many polls to count, and
// fall below it for this many to end.  Three polls is one full sensing cycle,
// so a single stray sample can neither open nor close a segment.
static constexpr uint16_t TOUCH_DIAG_MIN_HOLD_SAMPLES = 3;
static constexpr uint8_t TOUCH_DIAG_RELEASE_SAMPLES = 4;

// How long to watch the pads untouched before asking for the first press.
static constexpr uint32_t TOUCH_DIAG_IDLE_MS = 2000;

// Grace before each recalibration.  Calibrating with a finger still down bakes
// the touch into the base count, and every delta for the rest of the stage is
// then measured against a pressed pad.
static constexpr uint32_t TOUCH_DIAG_HANDS_CLEAR_MS = 5000;

// Every pad is pressed this many times, round-robin.  One press per pad gave
// a single number whose pressure nobody controlled; the threshold has to clear
// the weakest of several, so the run reports the lowest peak as well as the
// highest.
static constexpr uint8_t TOUCH_DIAG_PRESSES_PER_PAD = 3;

// Another pad at this share of the pressed pad's peak is called out: at that
// level a threshold set for one pad also fires on the other.
static constexpr int TOUCH_DIAG_CROSSTALK_PCT = 40;

// The TPL5010 window is ~60 s and nothing else feeds it on this path.
static constexpr uint32_t TOUCH_DIAG_WDT_FEED_MS = 20000;

namespace {

/// What one pad did during one gain stage.
struct TouchDiagPad {
  int peak = 0;                 ///< this pad's own best delta while it was the target
  int low_peak = 0;             ///< the weakest of those per-press peaks
  int at_peak[3] = {0, 0, 0};   ///< all three channels at that same instant
  int worst_other = 0;          ///< highest any other pad reached at any point in the press
  uint8_t worst_other_pad = 0;
  int idle_lo = 0;              ///< delta range with nothing touching the pads
  int idle_hi = 0;
  uint8_t presses = 0;
  uint8_t mismatches = 0;       ///< presses where another pad beat this one
  bool saturated = false;
  bool noise = false;
};

struct TouchDiagStage {
  uint8_t sense = 0;
  TouchDiagPad pad[3];
};

/// Delta a pad has to reach to open a press segment at this gain.
int touch_diag_enter_level(uint8_t sense) {
  const int scaled = TOUCH_DIAG_ENTER_AT_16X * CAP1203::delta_sense_multiplier(sense) / 16;
  return scaled < TOUCH_DIAG_ENTER_FLOOR ? TOUCH_DIAG_ENTER_FLOOR : scaled;
}

/// Delta every pad has to fall below to close one.  Half the entry level for
/// hysteresis, so a press hovering at the threshold cannot chatter into
/// several segments — but never at or below what the pads do untouched.  The
/// gain-scaled half lands on 1 count at 2x, and a pad resting one count high
/// would then hold the segment open for good.
int touch_diag_exit_level(uint8_t sense, int idle_hi) {
  const int half = touch_diag_enter_level(sense) / 2;
  const int floor_level = idle_hi + 1;
  return half > floor_level ? half : floor_level;
}

/// Highest delta any pad reached with nothing touching it.
int touch_diag_idle_ceiling(const TouchDiagStage &stage) {
  int hi = 0;
  for (int i = 0; i < 3; ++i) {
    if (stage.pad[i].idle_hi > hi) {
      hi = stage.pad[i].idle_hi;
    }
  }
  return hi;
}

void touch_diag_feed_wdt(const gpio::Hal &hal, uint32_t &last_feed) {
  const uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
  if (last_feed == 0 || (now - last_feed) >= TOUCH_DIAG_WDT_FEED_MS) {
    ext_watchdog_reset(hal, static_cast<int>(PIN_EXT_WDT));
    last_feed = now;
  }
}

/// Recalibrate and wait for the chip to say it finished.  CALIBRATION_ACTIVATE
/// clears itself when the routine completes; until then the analog trim reads
/// 0x00, which is also the value that means the trim ran out of range, so
/// nothing may be read before this clears (datasheet 5.10.1 and 5.27).
bool touch_diag_calibrate(CAP1203 &touch, const gpio::Hal &hal, uint32_t &last_feed) {
  if (!touch.calibrate(TouchChannel::ALL)) {
    return false;
  }
  for (int i = 0; i < 100; ++i) { // 100 polls at 30 ms is 3 s
    touch_diag_feed_wdt(hal, last_feed);
    RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
    CAP1203::Diagnostics d{};
    if (touch.read_diagnostics(d) && d.cal_active == 0) {
      return true;
    }
  }
  return false;
}

/// What the pads read with nothing on them.  Everything downstream is judged
/// against this: a threshold is only safe if it clears the band by a margin.
void touch_diag_idle_band(CAP1203 &touch, const gpio::Hal &hal, TouchDiagStage &stage,
                          uint32_t &last_feed) {
  int lo[3] = {127, 127, 127};
  int hi[3] = {-128, -128, -128};
  const uint32_t started = static_cast<uint32_t>(RTOS::get_time_ms());
  uint16_t samples = 0;

  while ((static_cast<uint32_t>(RTOS::get_time_ms()) - started) < TOUCH_DIAG_IDLE_MS) {
    touch_diag_feed_wdt(hal, last_feed);
    CAP1203::Diagnostics d{};
    if (touch.read_diagnostics(d)) {
      for (int i = 0; i < 3; ++i) {
        if (d.delta[i] < lo[i]) {
          lo[i] = d.delta[i];
        }
        if (d.delta[i] > hi[i]) {
          hi[i] = d.delta[i];
        }
      }
      ++samples;
    }
    touch.clear_interrupt();
    RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
  }

  const int enter = touch_diag_enter_level(stage.sense);
  for (int i = 0; i < 3; ++i) {
    stage.pad[i].idle_lo = lo[i];
    stage.pad[i].idle_hi = hi[i];
    if (hi[i] >= enter) {
      AG_LOGW(TOUCH_DIAG_TAG,
              "CS%d already reads %+d untouched — something is resting on it, or a "
              "hand moved during the idle window; reset and try again",
              i + 1, hi[i]);
    }
  }
  AG_LOGI(TOUCH_DIAG_TAG, "idle band over %u samples: CS1 %+d..%+d  CS2 %+d..%+d  CS3 %+d..%+d",
          samples, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
}

/// Ask for each pad in turn and record the peak sample of every press.
void touch_diag_run_stage(CAP1203 &touch, const gpio::Hal &hal, TouchDiagStage &stage,
                          uint32_t &last_feed) {
  const int idle_hi = touch_diag_idle_ceiling(stage);
  const int exit_level = touch_diag_exit_level(stage.sense, idle_hi);
  int enter = touch_diag_enter_level(stage.sense);
  if (enter <= exit_level) {
    enter = exit_level + 1;
    AG_LOGW(TOUCH_DIAG_TAG,
            "the idle band reaches %+d, which leaves no room under the entry level this gain "
            "asks for — raising entry to %d, and treat every number from this stage as marginal",
            idle_hi, enter);
  }
  const uint8_t mult = CAP1203::delta_sense_multiplier(stage.sense);
  AG_LOGI(TOUCH_DIAG_TAG, "press opens at %+d, closes under %+d", enter, exit_level);

  uint8_t target = 0;
  uint8_t attempts[3] = {0, 0, 0};
  uint8_t done_mask = 0;
  uint8_t press_no = 0;

  bool in_press = false;
  uint8_t below = 0;
  uint16_t hold_samples = 0;
  int peak = 0;              // the target pad's own best delta this press
  int at_peak[3] = {0, 0, 0};
  int worst_other = 0;       // highest any other pad reached at any point
  uint8_t worst_other_pad = 0;
  uint32_t press_start = 0;
  bool press_sat = false;
  bool press_noise = false;

  AG_LOGI(TOUCH_DIAG_TAG, "%u presses per pad, round-robin — make at least one of each pad's a "
                          "light touch, since the threshold has to catch the weakest",
          TOUCH_DIAG_PRESSES_PER_PAD);
  AG_LOGI(TOUCH_DIAG_TAG, "press CS%u only (%u/%u) — one fingertip, ~2 s, then lift clear",
          target + 1, attempts[target] + 1, TOUCH_DIAG_PRESSES_PER_PAD);

  while (done_mask != 0x07) {
    touch_diag_feed_wdt(hal, last_feed);

    CAP1203::Diagnostics d{};
    if (!touch.read_diagnostics(d)) {
      AG_LOGW(TOUCH_DIAG_TAG, "read failed");
      RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
      continue;
    }
    touch.clear_interrupt();

    // Segments open and close on whichever pad is loudest, so a press on the
    // wrong pad is still seen and reported rather than silently ignored.
    int strongest = 0;
    for (int i = 0; i < 3; ++i) {
      if (d.delta[i] > strongest) {
        strongest = d.delta[i];
      }
    }

    if (!in_press) {
      if (strongest < enter) {
        RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
        continue;
      }
      in_press = true;
      below = 0;
      hold_samples = 0;
      peak = 0;
      worst_other = 0;
      worst_other_pad = target == 0 ? 1 : 0;
      press_sat = false;
      press_noise = false;
      press_start = static_cast<uint32_t>(RTOS::get_time_ms());
    }

    // The peak instant is the target pad's own best moment: this run exists to
    // learn what CS<target> reaches and what the other two read just then.
    if (d.delta[target] > peak) {
      peak = d.delta[target];
      for (int i = 0; i < 3; ++i) {
        at_peak[i] = d.delta[i];
      }
    }
    // Separately, the highest any other pad got at any point in the press.
    // That, not its value at the target's peak, is what would false-trigger.
    for (int i = 0; i < 3; ++i) {
      if (i != target && d.delta[i] > worst_other) {
        worst_other = d.delta[i];
        worst_other_pad = static_cast<uint8_t>(i);
      }
    }
    // A set noise flag forces that pad's delta to zero for the sample, so a
    // real press can read +0 mid-hold.  Flag the press rather than the sample.
    if (d.noise_flags & 0x07) {
      press_noise = true;
    }
    for (int i = 0; i < 3; ++i) {
      if (d.delta[i] == 127) {
        press_sat = true;
      }
    }

    if (strongest >= exit_level) {
      ++hold_samples;
      below = 0;
      RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
      continue;
    }
    if (++below < TOUCH_DIAG_RELEASE_SAMPLES) {
      RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
      continue;
    }

    in_press = false;
    const uint32_t held_ms = static_cast<uint32_t>(RTOS::get_time_ms()) - press_start;
    if (hold_samples < TOUCH_DIAG_MIN_HOLD_SAMPLES) {
      AG_LOGW(TOUCH_DIAG_TAG, "ignored a %u ms blip while waiting for CS%u — too short to be a "
                              "press",
              held_ms, target + 1);
      RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
      continue;
    }

    const int cross_pct = peak > 0 ? worst_other * 100 / peak : 100;
    const bool mismatch = worst_other > peak;

    TouchDiagPad &rec = stage.pad[target];
    ++rec.presses;
    if (rec.presses == 1 || peak < rec.low_peak) {
      rec.low_peak = peak;
    }
    if (mismatch) {
      ++rec.mismatches;
    }
    if (peak > rec.peak) {
      rec.peak = peak;
      for (int i = 0; i < 3; ++i) {
        rec.at_peak[i] = at_peak[i];
      }
    }
    if (worst_other > rec.worst_other) {
      rec.worst_other = worst_other;
      rec.worst_other_pad = worst_other_pad;
    }
    rec.saturated = rec.saturated || press_sat;
    rec.noise = rec.noise || press_noise;

    ++press_no;
    AG_LOGI(TOUCH_DIAG_TAG,
            "press %u @%ux  asked CS%u  peak %+d  at that instant CS1 %+d  CS2 %+d  CS3 %+d  "
            "worst other CS%u %+d (%d%%)  held %u ms (%u samples)%s%s%s",
            press_no, mult, target + 1, peak, at_peak[0], at_peak[1], at_peak[2],
            worst_other_pad + 1, worst_other, cross_pct, held_ms, hold_samples,
            mismatch ? "  MISMATCH" : "", press_sat ? "  SAT" : "", press_noise ? "  NOISE" : "");
    if (mismatch) {
      AG_LOGW(TOUCH_DIAG_TAG, "CS%u was asked for but CS%u read higher — this is the failure that "
                              "makes Up register as Enter",
              target + 1, worst_other_pad + 1);
    } else if (cross_pct >= TOUCH_DIAG_CROSSTALK_PCT) {
      AG_LOGW(TOUCH_DIAG_TAG, "CS%u followed CS%u to %d%% of its peak — no threshold separates them",
              worst_other_pad + 1, target + 1, cross_pct);
    }
    if (press_sat) {
      AG_LOGW(TOUCH_DIAG_TAG, "delta pinned at +127 — %ux is too high to measure this pad", mult);
    }

    if (++attempts[target] >= TOUCH_DIAG_PRESSES_PER_PAD) {
      done_mask = static_cast<uint8_t>(done_mask | (1u << target));
    }
    if (done_mask == 0x07) {
      break;
    }
    for (uint8_t step = 1; step <= 3; ++step) {
      const uint8_t next = static_cast<uint8_t>((target + step) % 3);
      if ((done_mask & (1u << next)) == 0) {
        target = next;
        break;
      }
    }
    AG_LOGI(TOUCH_DIAG_TAG, "press CS%u only (%u/%u) — one fingertip, ~2 s, then lift clear",
            target + 1, attempts[target] + 1, TOUCH_DIAG_PRESSES_PER_PAD);
    RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
  }
}

void touch_diag_report(const TouchDiagStage &stage) {
  const uint8_t mult = CAP1203::delta_sense_multiplier(stage.sense);
  AG_LOGI(TOUCH_DIAG_TAG, "==== %ux: each pad's peak, and what the others did during that press ====",
          mult);
  AG_LOGI(TOUCH_DIAG_TAG, "pad  peak   low   CS1   CS2   CS3   worst other   idle     n  bad");

  int worst_pct = 0;
  uint8_t worst_victim = 0;
  uint8_t worst_source = 0;
  for (int i = 0; i < 3; ++i) {
    const TouchDiagPad &p = stage.pad[i];
    if (p.peak <= 0) {
      AG_LOGW(TOUCH_DIAG_TAG, "CS%d never registered a press — nothing to tune from", i + 1);
      continue;
    }
    const int pct = p.worst_other * 100 / p.peak;
    AG_LOGI(TOUCH_DIAG_TAG,
            "CS%d  %+4d  %+4d  %+4d  %+4d  %+4d   CS%u %+4d %3d%%  %+d..%+d  %u  %u%s%s", i + 1,
            p.peak, p.low_peak, p.at_peak[0], p.at_peak[1], p.at_peak[2], p.worst_other_pad + 1,
            p.worst_other, pct, p.idle_lo, p.idle_hi, p.presses, p.mismatches,
            p.saturated ? "  SAT" : "", p.noise ? "  NOISE" : "");
    if (pct > worst_pct) {
      worst_pct = pct;
      worst_victim = p.worst_other_pad;
      worst_source = static_cast<uint8_t>(i);
    }
  }

  AG_LOGI(TOUCH_DIAG_TAG, "worst crosstalk %d%%: CS%u while CS%u was pressed — %s", worst_pct,
          worst_victim + 1, worst_source + 1,
          worst_pct >= TOUCH_DIAG_CROSSTALK_PCT
              ? "REAL. No threshold separates these pads; this is a spacing question for v2.1"
              : "the pads separate cleanly, so per-pad thresholds will work");

  // Half the weakest press is the starting point: far enough above the idle
  // band to never self-trigger, far enough below every press seen that a light
  // one still registers.  Printed per pad because the pads differ by a third.
  AG_LOGI(TOUCH_DIAG_TAG,
          "starting thresholds at %ux (half of weakest press): CS1 %d  CS2 %d  CS3 %d", mult,
          stage.pad[0].low_peak / 2, stage.pad[1].low_peak / 2, stage.pad[2].low_peak / 2);
}

/// Does the same finger really read eight times smaller at an eighth of the
/// gain?  Everything the tuning depends on assumes it does.
void touch_diag_linearity(const TouchDiagStage &hi, const TouchDiagStage &lo) {
  const uint8_t mult_hi = CAP1203::delta_sense_multiplier(hi.sense);
  const uint8_t mult_lo = CAP1203::delta_sense_multiplier(lo.sense);
  const int expect_x10 = mult_lo > 0 ? (mult_hi * 10) / mult_lo : 0;
  AG_LOGI(TOUCH_DIAG_TAG, "==== gain check: %ux against %ux, expect x%d.%d ====", mult_hi, mult_lo,
          expect_x10 / 10, expect_x10 % 10);

  bool holds = true;
  for (int i = 0; i < 3; ++i) {
    const int a = hi.pad[i].peak;
    const int b = lo.pad[i].peak;
    if (b <= 0 || a <= 0) {
      AG_LOGW(TOUCH_DIAG_TAG, "CS%d  %ux %+d  %ux %+d  — no ratio, a pad never registered", i + 1,
              mult_hi, a, mult_lo, b);
      holds = false;
      continue;
    }
    const int ratio_x10 = a * 10 / b;
    // A count either way at the low gain moves the ratio a long way, so allow
    // a quarter either side before calling the multiplier broken.
    const bool ok = ratio_x10 >= (expect_x10 * 3) / 4 && ratio_x10 <= (expect_x10 * 5) / 4;
    AG_LOGI(TOUCH_DIAG_TAG, "CS%d  %ux %+d  %ux %+d  ratio %d.%d%s", i + 1, mult_hi, a, mult_lo, b,
            ratio_x10 / 10, ratio_x10 % 10, ok ? "" : "  OFF");
    holds = holds && ok;
  }
  if (holds) {
    AG_LOGI(TOUCH_DIAG_TAG,
            "multiplier holds — a peak measured at one gain scales to any other");
  } else {
    AG_LOGW(TOUCH_DIAG_TAG, "multiplier does NOT hold — no number may be carried across a gain "
                            "change, and the tuning must be measured at the gain it will ship at");
  }
}

} // namespace

void GoHardwareBoard::run_touch_diag() {
  // Nothing else is brought up on this path, so the external watchdog has no
  // other owner: without this the board resets inside a minute.
  ext_watchdog_init(gpio_hal(), static_cast<int>(PIN_EXT_WDT));
  const gpio::Hal &hal = gpio_hal();
  uint32_t last_feed = 0;

  CAP1203::Config cfg;
  cfg.delta_sense = TOUCH_DIAG_GAINS[0];
  cfg.repeat_rate_channels = static_cast<uint8_t>(TouchChannel::ALL & ~touch_channel_map().enter);
  CAP1203 touch(_i2c_bus, I2C_ADDR_CAP1203, cfg);
  if (!touch.init()) {
    AG_LOGE(TOUCH_DIAG_TAG, "CAP1203 init failed — check CN4 cable");
  }

  AG_LOGI(TOUCH_DIAG_TAG, "guided touch measurement — nothing else on this board is running");
  AG_LOGI(TOUCH_DIAG_TAG, "board flat on the table, both hands clear until the idle band prints");

  constexpr size_t stage_count = sizeof(TOUCH_DIAG_GAINS) / sizeof(TOUCH_DIAG_GAINS[0]);
  TouchDiagStage stages[stage_count];
  size_t measured = 0;

  for (size_t s = 0; s < stage_count; ++s) {
    stages[s].sense = TOUCH_DIAG_GAINS[s];
    const uint8_t mult = CAP1203::delta_sense_multiplier(stages[s].sense);

    if (!touch.set_delta_sense(stages[s].sense)) {
      AG_LOGE(TOUCH_DIAG_TAG, "could not set gain to %ux — stopping", mult);
      break;
    }
    AG_LOGI(TOUCH_DIAG_TAG, "---- gain %ux ----", mult);
    AG_LOGW(TOUCH_DIAG_TAG, "hands off the pads — recalibrating in %u s",
            TOUCH_DIAG_HANDS_CLEAR_MS / 1000);
    for (uint32_t waited = 0; waited < TOUCH_DIAG_HANDS_CLEAR_MS; waited += TOUCH_DIAG_PERIOD_MS) {
      touch_diag_feed_wdt(hal, last_feed);
      RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
    }
    if (!touch_diag_calibrate(touch, hal, last_feed)) {
      AG_LOGW(TOUCH_DIAG_TAG, "calibration did not report finished — trim values below are suspect");
    }

    CAP1203::Diagnostics d{};
    if (touch.read_diagnostics(d)) {
      for (int i = 0; i < 3; ++i) {
        AG_LOGI(TOUCH_DIAG_TAG, "CS%d trim=%u/1023 base=%u thr=%u%s%s", i + 1, d.calibration[i],
                d.base[i], d.threshold[i],
                CAP1203::calibration_failed(d.calibration[i]) ? "  CAL-PINNED" : "",
                (d.base_out_limit & (1u << i)) ? "  BASE-OUT" : "");
      }
      if (d.general_status & 0x20) {
        AG_LOGW(TOUCH_DIAG_TAG, "ACAL_FAIL — at least one pad failed analog calibration");
      }
    }

    touch_diag_idle_band(touch, hal, stages[s], last_feed);
    touch_diag_run_stage(touch, hal, stages[s], last_feed);
    touch_diag_report(stages[s]);
    ++measured;
  }

  // Only worth comparing two stages that both actually ran; a stage that
  // never got its gain set would read as a broken multiplier.
  if constexpr (stage_count >= 2) {
    if (measured >= 2) {
      touch_diag_linearity(stages[0], stages[1]);
    }
  }
  AG_LOGI(TOUCH_DIAG_TAG, "measurement complete — reset the board to run it again");

  for (;;) {
    touch_diag_feed_wdt(hal, last_feed);
    RTOS::delay_ms(TOUCH_DIAG_PERIOD_MS);
  }
}

AccelSensor *GoHardwareBoard::new_accel_sensor() {
  assert(_buses_ready && "new_accel_sensor() requires init_buses()");
  LIS2DH12::Config cfg;
  cfg.address = I2C_ADDR_LIS2DH12;
  auto *accel = new LIS2DH12(_i2c_bus, cfg);
  // Absent / wrong device / bus error: destroy and report nullptr so callers
  // treat it as "no accelerometer" (matches the GoBoard contract).
  if (!accel->init()) {
    AG_LOGE(TAG, "LIS2DH12 accel init failed / absent");
    _chips.set(Chip::Accel, false, "no WHO_AM_I");
    delete accel;
    return nullptr;
  }
  _chips.set(Chip::Accel, true, "WHO_AM_I 0x33");
  return accel;
}

// ===========================================================================
// Platform info
// ===========================================================================

BoardVariant GoHardwareBoard::variant() const {
  assert(_buses_ready && "variant() requires init_buses()");
  return _variant;
}

int GoHardwareBoard::touch_int_pin() const {
  return _variant == BoardVariant::V2 ? PIN_V2_CAP_INT : static_cast<int>(PIN_CAP_INT);
}

TouchChannelMap GoHardwareBoard::touch_channel_map() const {
  if (_variant == BoardVariant::V2) {
    return {.enter = TOUCH_V2_CH_ENTER, .up = TOUCH_V2_CH_UP, .down = TOUCH_V2_CH_DOWN};
  }
  return {};
}

std::string GoHardwareBoard::serial_number() { return build_serial_number(); }

const char *GoHardwareBoard::firmware_version() { return esp_app_get_description()->version; }

const gpio::Hal &GoHardwareBoard::gpio_hal() {
  return _variant == BoardVariant::V2 ? gpio::expander::hal : gpio::native::hal;
}

// ===========================================================================
// Hardware operations
// ===========================================================================

void GoHardwareBoard::release_gpio_holds() {
  // v2 keeps EN_PM1 on the expander, which holds its own state through deep
  // sleep; there is no native hold to release.
  if (_variant != BoardVariant::V2) {
    PowerService::release_sleep_gpio_holds(PIN_PM_POWER);
  }
}

void GoHardwareBoard::ulp_stop() { ulp_wdt_stop(); }

void GoHardwareBoard::ulp_start() { ulp_wdt_start(); }

void GoHardwareBoard::restart() { esp_restart(); }

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
