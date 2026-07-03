/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "drivers/bq25629/bq25629_bms.h"

#include <cinttypes>
#include <cmath>

#include "esp_log.h"
#include "rtos.h"

static constexpr const char *TAG = "BQ25629Bms";

/// Valid range for battery NTC temperature.  Anything outside this band
/// (including the vendor's -999.0f sentinel, NaN, or obviously-bogus
/// values) collapses to BmsInvalid::TEMPERATURE_C.
static constexpr float BATTERY_TEMP_VALID_MIN_C = -40.0f;
static constexpr float BATTERY_TEMP_VALID_MAX_C = 100.0f;

static BmsPowerSource map_vbus_status(drivers::VBusStatus vs);

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

BQ25629Bms::BQ25629Bms(i2c_master_bus_handle_t i2c_bus, const drivers::BQ25629_Config &config,
                       uint8_t address)
    : _charger(i2c_bus, address), _config(config) {}

// ---------------------------------------------------------------------------
// BmsDevice -- init
// ---------------------------------------------------------------------------

bool BQ25629Bms::init() {
  esp_err_t err = _charger.init(_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "BQ25629 init failed: %s", esp_err_to_name(err));
    return false;
  }

  // Disable the chip-level watchdog; callers can re-configure via
  // PowerService::set_watchdog_timeout_ms.
  err = _charger.set_watchdog_timeout(drivers::WatchdogTimeout::Disable);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_watchdog_timeout failed: %s", esp_err_to_name(err));
    return false;
  }

  if (!_apply_pmid_config()) {
    return false;
  }

  // Reset the watchdog timer after the full post-init sequence.
  err = _charger.reset_watchdog();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "reset_watchdog failed: %s", esp_err_to_name(err));
    return false;
  }

  ESP_LOGI(TAG, "BQ25629Bms initialized (PMID armed; chip handles buck↔boost autonomously)");
  return true;
}

// ---------------------------------------------------------------------------
// PMID configuration sequence (shared by init and resync)
// ---------------------------------------------------------------------------

bool BQ25629Bms::_apply_pmid_config() {
  // Arm PMID; the chip then handles buck↔boost autonomously based on
  // VBUS-detect (VBUS present → buck, EN_OTG masked; VBUS absent → boost
  // at VOTG).
  // Sequence: HIZ off → TS on → VOTG=5100 → BYPASS off → EN_OTG=1
  //           → settle → readback verify.
  static constexpr uint32_t STEP_DELAY_MS = 10;
  static constexpr uint32_t OTG_SETTLE_MS = 300;

  esp_err_t err = _charger.disable_hiz_mode();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "disable_hiz_mode failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  // Registers survive a BATFET power cycle (BAT never drops below UVLO), so
  // clear any stale PMID discharge sink a previous power_cycle() left armed.
  err = _charger.enable_pmid_discharge(false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "enable_pmid_discharge(false) failed: %s (continuing)", esp_err_to_name(err));
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  err = _charger.set_ts_ignore(false);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_ts_ignore failed: %s (continuing)", esp_err_to_name(err));
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  err = _charger.set_votg_voltage(5100);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set_votg_voltage failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  err = _charger.enable_bypass_otg(false);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enable_bypass_otg(false) failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(STEP_DELAY_MS);

  // Pre-OTG ADC snapshot for boost-engage diagnostics.
  drivers::BQ25629_ADC_Data pre_adc{};
  if (_charger.read_adc(pre_adc) == ESP_OK) {
    ESP_LOGI(TAG, "pre-OTG ADC: vbat=%umV vsys=%umV vpmid=%umV vbus=%umV ibat=%dmA",
             pre_adc.vbat_mv, pre_adc.vsys_mv, pre_adc.vpmid_mv, pre_adc.vbus_mv, pre_adc.ibat_ma);
  }

  err = _charger.enable_otg(true);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enable_otg(true) failed: %s", esp_err_to_name(err));
    return false;
  }
  RTOS::delay_ms(OTG_SETTLE_MS);

  bool en_otg = false;
  drivers::BQ25629_ADC_Data post_adc{};
  const bool have_otg = _charger.get_otg_enabled(en_otg) == ESP_OK;
  const bool have_post_adc = _charger.read_adc(post_adc) == ESP_OK;
  ESP_LOGI(TAG, "post-OTG verify: EN_OTG=%d vpmid=%umV vbat=%umV", en_otg,
           have_post_adc ? post_adc.vpmid_mv : 0, have_post_adc ? post_adc.vbat_mv : 0);
  if (have_otg && !en_otg) {
    ESP_LOGE(TAG, "PMID config: chip refused EN_OTG");
    return false;
  }
  _pmid_enabled = true;
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- telemetry
// ---------------------------------------------------------------------------

bool BQ25629Bms::read_telemetry(BmsTelemetry &out) {
  out = BmsTelemetry{}; // Reset all fields to invalid sentinels.

  drivers::BQ25629_ADC_Data adc{};
  esp_err_t err = _charger.read_adc(adc);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "read_adc failed: %s", esp_err_to_name(err));
    return false;
  }

  // Voltages — convert mV to V for the legacy float fields.
  out.battery_voltage = static_cast<float>(adc.vbat_mv) / 1000.0f;
  out.charging_voltage = static_cast<float>(adc.vbus_mv) / 1000.0f;

  // Currents
  out.input_current_ma = adc.ibus_ma;
  out.battery_current_ma = adc.ibat_ma;

  // Additional voltages (mV, native ADC units)
  out.system_voltage_mv = adc.vsys_mv;
  out.pmid_voltage_mv = adc.vpmid_mv;

  // Temperature
  out.ts_percent = adc.ts_percent;
  out.die_temperature_c = adc.tdie_c;

  // Battery NTC temperature — populate from the Steinhart-Hart conversion.
  // Leave at BmsInvalid::TEMPERATURE_C when the reading is out of range.
  drivers::BQ25629_NTC_Data ntc{};
  out.battery_temperature_c = BmsInvalid::TEMPERATURE_C;
  if (_charger.read_ntc_temperature(ntc) == ESP_OK) {
    const float t = ntc.temperature_c;
    if (!std::isnan(t) && t > BATTERY_TEMP_VALID_MIN_C && t < BATTERY_TEMP_VALID_MAX_C) {
      out.battery_temperature_c = static_cast<int16_t>(std::lroundf(t));
    }
  }

  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- status
// ---------------------------------------------------------------------------

/// Map vendor ChargeStatus to the shared BmsChargingState enum.
///
/// The BQ25629 uses a 2-bit charge status field that cannot distinguish
/// trickle, pre-charge, and fast-charge individually.  The combined value
/// TRICKLE_PRECHARGE_FASTCHARGE is mapped to FastCharge because CC-mode
/// fast-charging is by far the most common state represented by this code.
static BmsChargingState map_charge_status(drivers::ChargeStatus cs) {
  switch (cs) {
  case drivers::ChargeStatus::NOT_CHARGING:
    return BmsChargingState::NotCharging;
  case drivers::ChargeStatus::TRICKLE_PRECHARGE_FASTCHARGE:
    return BmsChargingState::FastCharge;
  case drivers::ChargeStatus::TAPER_CHARGE:
    return BmsChargingState::TaperCharge;
  case drivers::ChargeStatus::TOPOFF_TIMER_ACTIVE:
    return BmsChargingState::TopOffTimerActiveCharging;
  default:
    return BmsChargingState::Unknown;
  }
}

/// Map vendor VBusStatus to the shared BmsPowerSource enum.
static BmsPowerSource map_vbus_status(drivers::VBusStatus vs) {
  switch (vs) {
  case drivers::VBusStatus::NO_ADAPTER:
    return BmsPowerSource::None;
  case drivers::VBusStatus::USB_SDP:
    return BmsPowerSource::UsbSdp;
  case drivers::VBusStatus::USB_CDP:
    return BmsPowerSource::UsbCdp;
  case drivers::VBusStatus::USB_DCP:
    return BmsPowerSource::UsbDcp;
  case drivers::VBusStatus::UNKNOWN_ADAPTER:
    return BmsPowerSource::UnknownAdapter;
  case drivers::VBusStatus::NON_STANDARD:
    return BmsPowerSource::NonStandard;
  case drivers::VBusStatus::OTG_MODE:
    return BmsPowerSource::OtgMode;
  default:
    return BmsPowerSource::Unknown;
  }
}

bool BQ25629Bms::read_status(BmsStatus &out) {
  drivers::BQ25629_Status raw{};
  esp_err_t err = _charger.read_status(raw);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "read_status failed: %s", esp_err_to_name(err));
    out.charging_state = BmsChargingState::Unknown;
    out.power_source = BmsPowerSource::Unknown;
    return false;
  }

  out.charging_state = map_charge_status(raw.charge_status);
  out.power_source = map_vbus_status(raw.vbus_status);
  out.thermal_regulation = raw.treg_stat;
  out.vsys_regulation = raw.vsys_stat;
  out.input_current_regulation = raw.iindpm_stat;
  out.input_voltage_regulation = raw.vindpm_stat;
  out.safety_timer_expired = raw.safety_tmr_stat;
  out.watchdog_expired = raw.wd_stat;
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- charging state (lightweight, single register)
// ---------------------------------------------------------------------------

bool BQ25629Bms::get_charging_state(BmsChargingState &state) {
  drivers::ChargeStatus raw{};
  esp_err_t err = _charger.get_charge_status(raw);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "get_charge_status failed: %s", esp_err_to_name(err));
    state = BmsChargingState::Unknown;
    return false;
  }
  state = map_charge_status(raw);
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- battery percentage
// ---------------------------------------------------------------------------

bool BQ25629Bms::get_battery_percentage(float *output) {
  if (output == nullptr) {
    return false;
  }

  uint8_t percent = 0;
  esp_err_t err = _charger.estimate_battery_percent(percent);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "estimate_battery_percent failed: %s", esp_err_to_name(err));
    return false;
  }

  *output = static_cast<float>(percent);
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- watchdog
// ---------------------------------------------------------------------------

bool BQ25629Bms::update_watchdog() {
  esp_err_t err = _charger.reset_watchdog();
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "reset_watchdog failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- ship mode
// ---------------------------------------------------------------------------

bool BQ25629Bms::feature_ship_available() const { return true; }

bool BQ25629Bms::enter_ship_mode() {
  esp_err_t err = _charger.enter_ship_mode();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "enter_ship_mode failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// BmsDevice -- power-path control
// ---------------------------------------------------------------------------

bool BQ25629Bms::set_pmid_enabled(bool enabled) {
  // init() arms EN_OTG once; the chip handles buck↔boost on its own.
  // This entry point exists for explicit recovery / shutdown control,
  // not per-measurement PM cycling.
  esp_err_t err = _charger.enable_otg(enabled);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_pmid_enabled(%s) failed: %s", enabled ? "true" : "false",
             esp_err_to_name(err));
    return false;
  }
  _pmid_enabled = enabled;
  return true;
}

bool BQ25629Bms::resync_pmid() { return _apply_pmid_config(); }

bool BQ25629Bms::hard_resync_pmid() {
  // Escalation for a boost the chip refuses to restart via EN_OTG toggles:
  // REG_RST clears latched power-path/input-FSM state (register-domain
  // only; BATFET stays on), then the full configuration and PMID arm
  // sequence is re-applied from scratch.
  esp_err_t err = _charger.soft_reset();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "hard_resync_pmid: soft_reset failed: %s", esp_err_to_name(err));
    return false;
  }

  // Disable the chip watchdog FIRST: REG_RST re-arms it at its 50 s POR
  // default, and a mid-sequence I2C failure below must not leave a live
  // watchdog that keeps wiping the registers back to defaults every 50 s.
  err = _charger.set_watchdog_timeout(drivers::WatchdogTimeout::Disable);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "hard_resync_pmid: set_watchdog_timeout failed: %s", esp_err_to_name(err));
    return false;
  }

  err = _charger.apply_config(_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "hard_resync_pmid: apply_config failed: %s", esp_err_to_name(err));
    return false;
  }

  return _apply_pmid_config();
}

bool BQ25629Bms::power_cycle() {
  // Full BATFET excursion: off for tBATFET_RST (~430 ms), then the chip
  // re-engages it on its own — the software equivalent of ship mode + power
  // button, which is the only intervention observed to clear stuck
  // power-path state that survives REG_RST.  SYS (and this MCU) lose power
  // ~25 ms after the write; on success this function never returns.
  (void)_charger.enable_otg(false);
  // Actively drain PMID so every rail reaches cold-boot levels well inside
  // the 430 ms window (IPMID_LOAD ~30 mA sink; VBUS follows via its bleed).
  (void)_charger.enable_pmid_discharge(true);

  esp_err_t err = _charger.system_power_reset();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "power_cycle: system_power_reset failed: %s", esp_err_to_name(err));
    (void)_charger.enable_pmid_discharge(false);
    return false;
  }
  return true;
}

bool BQ25629Bms::dump_power_registers(const char *context) {
  return _charger.log_otg_registers(context) == ESP_OK;
}

bool BQ25629Bms::set_charge_enable(bool enabled) {
  esp_err_t err = _charger.enable_charging(enabled);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_charge_enable(%s) failed: %s", enabled ? "true" : "false",
             esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ25629Bms::set_charge_current_ma(uint16_t current_ma) {
  esp_err_t err = _charger.set_charge_current(current_ma);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_charge_current_ma(%u) failed: %s", current_ma, esp_err_to_name(err));
    return false;
  }
  return true;
}

bool BQ25629Bms::set_watchdog_timeout_ms(uint32_t timeout_ms) {
  // Map milliseconds to the closest supported WatchdogTimeout enum value
  // at or above the requested period.
  drivers::WatchdogTimeout wdt;
  if (timeout_ms == 0) {
    wdt = drivers::WatchdogTimeout::Disable;
  } else if (timeout_ms <= 50000) {
    wdt = drivers::WatchdogTimeout::Sec50;
  } else if (timeout_ms <= 100000) {
    wdt = drivers::WatchdogTimeout::Sec100;
  } else {
    wdt = drivers::WatchdogTimeout::Sec200;
  }

  esp_err_t err = _charger.set_watchdog_timeout(wdt);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "set_watchdog_timeout_ms(%" PRIu32 ") failed: %s", timeout_ms,
             esp_err_to_name(err));
    return false;
  }
  return true;
}
