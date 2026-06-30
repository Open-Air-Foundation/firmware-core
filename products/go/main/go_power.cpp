/**
 * AirGradient Go — Power Management Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

// ---------------------------------------------------------------------------
// Platform compatibility — RTC_DATA_ATTR
//
// On ESP-IDF, RTC_DATA_ATTR places a variable in RTC slow memory so it
// survives deep sleep.  In host test builds the attribute does not exist;
// define it away so the translation unit compiles as a regular static.
// ---------------------------------------------------------------------------

#if !defined(TEST_HOST) && defined(__has_include)
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#endif

#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif

// ---------------------------------------------------------------------------
// Platform compatibility — ESP-IDF sleep APIs
// ---------------------------------------------------------------------------

#ifndef TEST_HOST
#include "driver/gpio.h"
#include "esp_sleep.h"
#endif

#include "go_power.h"

#include "hal/fuel_gauge_device.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "ag_log.h"
#include "common.h"
#include "rtos.h"

static constexpr const char *TAG = "PowerService";

// ---------------------------------------------------------------------------
// FG flag decode helper
// ---------------------------------------------------------------------------

/// Format active BQ27427 Flags() bits as a compact string.
/// Output: "0x01A9[FC|CHG|OCVTAKEN|DSG]" — raw hex followed by active-bit
/// names in brackets.  When no bits are active the bracket part is "[-]".
/// @param flags  Raw 16-bit Flags() register value.
/// @param buf    Destination buffer (caller-owned).
/// @param len    Size of @p buf in bytes.
/// @return       @p buf (for direct use in printf-style calls).
static char *fmt_fg_flags(uint16_t flags, char *buf, size_t len) {
  // Raw hex prefix.
  int pos = snprintf(buf, len, "0x%04X[", flags);
  if (pos < 0 || static_cast<size_t>(pos) >= len) {
    return buf;
  }

  struct BitName {
    uint16_t mask;
    const char *name;
  };
  static constexpr BitName FLAG_BITS[] = {
      {FgFlags::FC, "FC"},       {FgFlags::CHG, "CHG"},     {FgFlags::OCVTAKEN, "OCVTAKEN"},
      {FgFlags::ITPOR, "ITPOR"}, {FgFlags::CFGUP, "CFGUP"}, {FgFlags::BAT_DET, "BAT_DET"},
      {FgFlags::DSG, "DSG"},
  };

  bool first = true;
  for (const auto &b : FLAG_BITS) {
    if (flags & b.mask) {
      pos += snprintf(buf + pos, len - static_cast<size_t>(pos), "%s%s", first ? "" : "|", b.name);
      if (static_cast<size_t>(pos) >= len) {
        return buf;
      }
      first = false;
    }
  }

  if (first) {
    pos += snprintf(buf + pos, len - static_cast<size_t>(pos), "-");
  }

  snprintf(buf + pos, len - static_cast<size_t>(pos), "]");
  return buf;
}

// ---------------------------------------------------------------------------
// RTC state storage
//
// These two variables live in RTC slow memory (survives deep sleep).
// Under TEST_HOST the RTC_DATA_ATTR is defined away, making them ordinary
// file-scope statics with the same semantics for unit testing.
// ---------------------------------------------------------------------------

RTC_DATA_ATTR static RtcAppState s_rtc_state;
RTC_DATA_ATTR static bool s_rtc_state_valid = false;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

PowerService::PowerService(BmsDevice &bms, const gpio::Hal &gpio, const Config &config)
    : _bms(bms), _gpio(gpio), _config(config) {}

void PowerService::set_fuel_gauge(FuelGaugeDevice *fg) { _fg = fg; }

// ---------------------------------------------------------------------------
// BMS operations
// ---------------------------------------------------------------------------

PowerSnapshot PowerService::poll_bms(bool pm_invalid_hint) {
  PowerSnapshot status{};

  bool telemetry_ok = false;
  BmsTelemetry telemetry{};
  if (_bms.read_telemetry(telemetry)) {
    telemetry_ok = true;
    if (telemetry.is_battery_voltage_valid()) {
      status.battery_voltage = telemetry.battery_voltage;
    }
    if (telemetry.is_charging_voltage_valid()) {
      status.charging_voltage = telemetry.charging_voltage;
    }
    status.telemetry = telemetry;
  } else {
    AG_LOGW(TAG, "poll_bms: read_telemetry() failed");
  }

  // --- FG snapshot (V1 path) ---
  // Reads are independent; partial failures leave individual fields at
  // their invalid sentinels.
  bool fg_soc_ok = false;
  uint8_t fg_soc = BmsInvalid::SOC_PERCENT;
  if (_fg != nullptr && _fg->ready()) {
    fg_soc_ok = _fg->read_soc_percent(fg_soc);
    if (fg_soc_ok) {
      status.fg_soc_percent = fg_soc;
    }

    uint16_t fg_mv = BmsInvalid::VOLTAGE_MV;
    if (_fg->read_voltage_mv(fg_mv)) {
      status.fg_voltage_mv = fg_mv;
    }

    int16_t fg_ma = BmsInvalid::CURRENT_MA;
    if (_fg->read_average_current_ma(fg_ma)) {
      status.fg_current_ma = fg_ma;
    }

    int16_t fg_pw = BmsInvalid::POWER_MW;
    if (_fg->read_average_power_mw(fg_pw)) {
      status.fg_power_mw = fg_pw;
    }

    uint16_t fg_rem = BmsInvalid::CAPACITY_MAH;
    if (_fg->read_remaining_capacity_mah(fg_rem)) {
      status.fg_remaining_capacity_mah = fg_rem;
    }

    uint16_t fg_fcc = BmsInvalid::CAPACITY_MAH;
    if (_fg->read_full_charge_capacity_mah(fg_fcc)) {
      status.fg_full_charge_capacity_mah = fg_fcc;
    }

    float fg_tc = BmsInvalid::FG_TEMP_C;
    if (_fg->read_internal_temperature_c(fg_tc)) {
      status.fg_internal_temperature_c = fg_tc;
    }

    uint16_t fg_fl = 0;
    if (_fg->read_flags(fg_fl)) {
      status.fg_flags = fg_fl;
    }
  }

  // --- SOC source preference: FG first; BQ25629 voltage estimate fallback ---
  if (fg_soc_ok) {
    status.battery_percentage = static_cast<float>(fg_soc);
    status.battery_percent_source = BatteryPercentSource::FuelGauge;
  } else {
    float pct = -1.0f;
    if (_bms.get_battery_percentage(&pct)) {
      status.battery_percentage = pct;
      status.battery_percent_source = BatteryPercentSource::BatteryCharger;
    }
  }
  status.critical =
      (status.battery_percentage >= 0.0f && status.battery_percentage < BATTERY_CRITICAL_PERCENT);

  bool status_ok = false;
  BmsStatus bms_status{};
  if (_bms.read_status(bms_status)) {
    status_ok = true;
    status.charging_status = bms_status.charging_state;
    status.charger_status = bms_status;
  }

  _log_poll_snapshot(status);

  // PMID boost recovery (safety net): re-kick if vpmid collapsed on battery.
  if (status_ok && telemetry_ok) {
    rekick_pmid_if_collapsed(telemetry, bms_status.power_source);
  }

  // -------------------------------------------------------------------------
  // EDV (over-discharge) trip — gated on explicit "on-battery" status
  // -------------------------------------------------------------------------
  {
    const bool on_battery = status_ok && (bms_status.power_source == BmsPowerSource::None ||
                                          bms_status.power_source == BmsPowerSource::OtgMode);

    if (on_battery && telemetry_ok && telemetry.is_battery_voltage_valid() &&
        telemetry.battery_voltage < EDV_SHIP_THRESHOLD_V) {
      ++_edv_low_count;
    } else {
      _edv_low_count = 0;
    }

    if (_edv_low_count >= EDV_SHIP_DEBOUNCE_SAMPLES) {
      AG_LOGW(TAG, "EDV trip: cell %.2fV < %.1fV for %d polls -> requesting ship mode",
              telemetry.battery_voltage, EDV_SHIP_THRESHOLD_V, _edv_low_count);
      status.ship_mode_request = ShipModeRequest::OverDischarge;
    }
  }

  // -------------------------------------------------------------------------
  // OT (over-temperature) trip — two-tier policy
  // -------------------------------------------------------------------------
  if (telemetry_ok && telemetry.is_battery_temperature_valid()) {
    const int16_t bat_temp = telemetry.battery_temperature_c;

    // Tier 2: request ship mode at SHIP_THRESHOLD.  Charging is disabled
    // immediately; the orchestrator shows a warning then calls shutdown().
    if (bat_temp >= OT_SHIP_THRESHOLD_C) {
      AG_LOGW(TAG, "OT trip: cell hot %d°C >= %d°C -> requesting ship mode", bat_temp,
              OT_SHIP_THRESHOLD_C);
      if (!_thermal_charge_disabled) {
        if (_bms.set_charge_enable(false)) {
          _thermal_charge_disabled = true;
        }
      }
      status.ship_mode_request = ShipModeRequest::OverTemperature;
    }
    // Tier 1: charge cutoff at HOT_CUTOFF (edge-triggered going up).
    else if (bat_temp >= OT_CHARGE_HOT_CUTOFF_C && !_thermal_charge_disabled) {
      AG_LOGW(TAG, "OT warn: cell warm %d°C >= %d°C -> disable charging", bat_temp,
              OT_CHARGE_HOT_CUTOFF_C);
      if (_bms.set_charge_enable(false)) {
        _thermal_charge_disabled = true;
      }
    }
    // Tier 1: charge resume at HOT_RESUME (edge-triggered going down).
    // Only issue the I2C write when full-charge pause is also inactive;
    // otherwise the thermal flag clears but charging stays off.
    else if (bat_temp <= OT_CHARGE_HOT_RESUME_C && _thermal_charge_disabled) {
      AG_LOGI(TAG, "OT clear: cell cooled %d°C <= %d°C -> re-enable charging", bat_temp,
              OT_CHARGE_HOT_RESUME_C);
      _thermal_charge_disabled = false;
      if (!_full_charge_paused) {
        _bms.set_charge_enable(true);
      }
    }
  }

  // -------------------------------------------------------------------------
  // Full-charge pause — disable charging when battery is full + plugged
  // -------------------------------------------------------------------------
  const bool plugged = status_ok && bms_power_source_has_external_input(bms_status.power_source);

  // FC flag (FG path) or ChargeTerminationDone + 100 % (Prototype fallback).
  const bool fg_full = (_fg != nullptr && _fg->ready() && (status.fg_flags & FgFlags::FC));
  const bool bms_full =
      (_fg == nullptr && status.charging_status == BmsChargingState::ChargeTerminationDone &&
       status.battery_percentage >= 100.0f);
  const bool full = fg_full || bms_full;

  if (plugged && full && !_full_charge_paused) {
    _full_charge_paused = true;
    if (!_thermal_charge_disabled) {
      _bms.set_charge_enable(false);
    }
    AG_LOGI(TAG, "full-charge pause: battery full while plugged, charging disabled");
  } else if (_full_charge_paused && status.battery_percentage >= 0.0f &&
             status.battery_percentage <= static_cast<float>(FULL_CHARGE_RESUME_SOC)) {
    _full_charge_paused = false;
    if (!_thermal_charge_disabled) {
      _bms.set_charge_enable(true);
    }
    AG_LOGI(TAG, "full-charge resume: SOC dropped to %.0f%%", status.battery_percentage);
  }

  status.full_charge_paused = _full_charge_paused;

  return status;
}

PowerSnapshot PowerService::poll_bms_fg_learning(bool pm_invalid_hint) {
  // Normal poll first, then layer the learning-only fields on top so the extra
  // CONTROL_STATUS read stays off the normal field hot path.
  PowerSnapshot status = poll_bms(pm_invalid_hint);

  // Flags()-derived bits are free — read_flags() already ran in poll_bms().
  uint8_t lf = 0;
  if (status.fg_flags & FgFlags::FC) {
    lf |= FG_LEARN_FC;
  }
  if (status.fg_flags & FgFlags::CHG) {
    lf |= FG_LEARN_CHG;
  }
  if (status.fg_flags & FgFlags::DSG) {
    lf |= FG_LEARN_DSG;
  }
  if (status.fg_flags & FgFlags::ITPOR) {
    lf |= FG_LEARN_ITPOR;
  }
  if (status.fg_flags & FgFlags::OCVTAKEN) {
    lf |= FG_LEARN_OCV_TAKEN;
  }

  // CONTROL_STATUS (QMAX_UP / RES_UP) is the one extra read — learning only.
  if (_fg != nullptr && _fg->ready()) {
    uint16_t cs = 0;
    if (_fg->read_control_status(cs)) {
      if (cs & FgControlStatus::QMAX_UP) {
        lf |= FG_LEARN_QMAX_UP;
      }
      if (cs & FgControlStatus::RES_UP) {
        lf |= FG_LEARN_RES_UP;
      }
    }
  }
  status.fg_learning_flags = lf;

  status.external_input_present =
      bms_power_source_has_external_input(status.charger_status.power_source);
  // Derived mirror for the pure FSM: Discharge -> CycleDone trips on this.
  status.edv_cutoff_reached = (status.ship_mode_request == ShipModeRequest::OverDischarge);

  return status;
}

FgLearningVerifyReadout PowerService::read_fg_learning_verify() {
  FgLearningVerifyReadout out{};
  if (_fg == nullptr || !_fg->ready()) {
    return out;
  }

  bool ok = true;
  uint16_t flags = 0;
  ok = _fg->read_flags(flags) && ok;
  out.itpor = (flags & FgFlags::ITPOR) != 0;

  uint16_t cs = 0;
  ok = _fg->read_control_status(cs) && ok;
  out.qmax_up = (cs & FgControlStatus::QMAX_UP) != 0;

  // Qmax Cell 0 is fixed-point, not mAh: Qmax(mAh) = raw * DC / 2^14
  // (TRM 7.4.2.3.1). Scale here so the verify band check sees true mAh.
  uint16_t qmax_raw = 0;
  ok = _fg->read_qmax_cell0(qmax_raw) && ok;
  ok = _fg->read_design_capacity_mah(out.design_capacity_mah) && ok;
  out.qmax_mah =
      static_cast<uint16_t>((static_cast<uint32_t>(qmax_raw) * out.design_capacity_mah) / 16384u);
  ok = _fg->read_ra_table(out.ra, FG_RA_TABLE_SIZE) && ok;

  out.ok = ok;
  return out;
}

bool PowerService::set_charge_current_ma(uint16_t current_ma) {
  return _bms.set_charge_current_ma(current_ma);
}

void PowerService::set_manual_charge_disabled(bool disabled) { _bms.set_charge_enable(!disabled); }

bool PowerService::set_chemistry_4v2() {
  return _fg != nullptr && _fg->ready() && _fg->select_chemistry_4v2();
}

bool PowerService::set_update_status_learning(bool enable) {
  return _fg != nullptr && _fg->ready() && _fg->set_update_status_learning(enable);
}

void PowerService::_log_poll_snapshot(const PowerSnapshot &snap) {
  AG_LOGI(TAG,
          "poll_bms: perc=%.1f%% src=%s vbat=%.1fV vbus=%.1fV critical=%d | "
          "charge=%s pwr=%s full_chg_paused=%d | "
          "treg=%d vsys=%d iindpm=%d vindpm=%d safety_tmr=%d wd=%d",
          snap.battery_percentage, bms_battery_percent_source_str(snap.battery_percent_source),
          snap.battery_voltage, snap.charging_voltage, snap.critical,
          bms_charging_state_str(snap.charger_status.charging_state),
          bms_power_source_str(snap.charger_status.power_source), snap.full_charge_paused,
          snap.charger_status.thermal_regulation, snap.charger_status.vsys_regulation,
          snap.charger_status.input_current_regulation,
          snap.charger_status.input_voltage_regulation, snap.charger_status.safety_timer_expired,
          snap.charger_status.watchdog_expired);

  const auto &t = snap.telemetry;
  AG_LOGI(TAG,
          "poll_bms: ibus=%dmA ibat=%dmA vsys=%umV vpmid=%umV ts=%.1f%% "
          "tdie=%d°C tbat=%d°C",
          t.input_current_ma, t.battery_current_ma, t.system_voltage_mv, t.pmid_voltage_mv,
          t.ts_percent, t.die_temperature_c, t.battery_temperature_c);

  // --- FG telemetry (V1 path) ---
  if (_fg != nullptr && _fg->ready()) {
    char flag_buf[64];
    fmt_fg_flags(snap.fg_flags, flag_buf, sizeof(flag_buf));

    AG_LOGI(TAG,
            "poll_bms: FG soc=%u%% v=%umV i=%dmA p=%dmW rem=%umAh fcc=%umAh "
            "t=%.1fC %s therm_chg_off=%d",
            snap.fg_soc_percent, snap.fg_voltage_mv, snap.fg_current_ma, snap.fg_power_mw,
            snap.fg_remaining_capacity_mah, snap.fg_full_charge_capacity_mah,
            snap.fg_internal_temperature_c, flag_buf, _thermal_charge_disabled);

    if (snap.fg_flags & FgFlags::ITPOR) {
      AG_LOGW(TAG, "poll_bms: FG ITPOR set — gauge reset detected, learned data may be lost");
    }
  }
}

bool PowerService::poll_charging_status(BmsChargingState &state) {
  if (!_bms.get_charging_state(state)) {
    AG_LOGW(TAG, "poll_charging_status: get_charging_state() failed");
    return false;
  }
  return true;
}

bool PowerService::poll_status(BmsStatus &status) {
  status = BmsStatus{};
  if (!_bms.read_status(status)) {
    AG_LOGW(TAG, "poll_status: read_status() failed");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// PMID boost recovery
// ---------------------------------------------------------------------------

bool PowerService::rekick_pmid_if_collapsed(const BmsTelemetry &t, BmsPowerSource src) {
  const bool on_battery = (src == BmsPowerSource::None || src == BmsPowerSource::OtgMode);
  const bool pmid_valid = (t.pmid_voltage_mv != BmsInvalid::VOLTAGE_MV);
  if (!(on_battery && pmid_valid && t.pmid_voltage_mv < PMID_HEALTHY_MIN_MV)) {
    return false;
  }
  AG_LOGW(TAG, "PMID collapsed (vpmid=%u mV) -> re-kick boost", t.pmid_voltage_mv);
  _bms.set_pmid_enabled(false);
  RTOS::delay_ms(PMID_REKICK_OFF_MS);
  _bms.set_pmid_enabled(true);
  return true;
}

bool PowerService::ensure_pmid_healthy() {
  BmsStatus s{};
  BmsTelemetry t{};
  if (!_bms.read_status(s) || !_bms.read_telemetry(t)) {
    return false;
  }
  return rekick_pmid_if_collapsed(t, s.power_source);
}

bool PowerService::reset_watchdog() {
  const bool ok = _bms.update_watchdog();
  if (!ok) {
    AG_LOGW(TAG, "reset_watchdog: update_watchdog() failed");
  }
  return ok;
}

bool PowerService::set_watchdog_timeout_ms(uint32_t timeout_ms) {
  return _bms.set_watchdog_timeout_ms(timeout_ms);
}

void PowerService::shutdown() {
#ifndef TEST_HOST
  AG_LOGI(TAG, "shutdown: entering BMS ship mode (QoN)");
  if (!_bms.enter_ship_mode()) {
    AG_LOGE(TAG, "shutdown: enter_ship_mode failed — falling back to deep sleep");
  }
  // enter_ship_mode() cuts system power and should not return.
  // If it does (failed or unexpected return), fall back to deep sleep with
  // GPIO wake sources so the user can press a button to try powering on again.
  // No timer is configured — this is a shutdown, not a scheduled wake cycle.
  uint64_t wake_mask = 0;
  if (_config.pin_wake_button_power >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_power;
  }
  if (_config.pin_wake_button_boot >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_boot;
  }
  if (wake_mask != 0) {
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  }
  esp_deep_sleep_start();
#endif
}

// ---------------------------------------------------------------------------
// External watchdog
// ---------------------------------------------------------------------------

void PowerService::init_ext_watchdog() {
  if (_config.pin_ext_wdt < 0) {
    return;
  }
  if (!ext_watchdog_init(_gpio, _config.pin_ext_wdt)) {
    AG_LOGE(TAG, "init_ext_watchdog: GPIO %d config failed", _config.pin_ext_wdt);
  }
}

void PowerService::reset_ext_watchdog() {
  if (_config.pin_ext_wdt < 0) {
    return;
  }
  ext_watchdog_reset(_gpio, _config.pin_ext_wdt);
}

// ---------------------------------------------------------------------------
// RTC state persistence
// ---------------------------------------------------------------------------

void PowerService::save_state(const RtcAppState &state) {
  memcpy(&s_rtc_state, &state, sizeof(RtcAppState));
  s_rtc_state_valid = true;
}

RtcAppState PowerService::load_state() const {
  if (!s_rtc_state_valid) {
    // Return defaults: Offline mode, Idle behavior, Locked, GPS on.
    return RtcAppState{};
  }
  RtcAppState out{};
  memcpy(&out, &s_rtc_state, sizeof(RtcAppState));
  return out;
}

// ---------------------------------------------------------------------------
// decide_sleep — pure logic (no platform dependencies)
// ---------------------------------------------------------------------------

PowerService::SleepDecision PowerService::decide_sleep(const GoSettings &settings,
                                                       LockState lock_state, OperatingMode mode,
                                                       uint32_t awake_ms) const {
  // Only Offline mode enters sleep; Portable and Stationary stay awake.
  if (mode != OperatingMode::Offline) {
    return {SleepType::None, 0};
  }

  if (lock_state == LockState::Unlocked) {
    return {SleepType::None, 0};
  }

  uint32_t interval_ms = static_cast<uint32_t>(settings.measure_interval_seconds) * 1000;

  // Subtract time already spent awake so total cycle matches the interval
  uint32_t sleep_ms = (awake_ms < interval_ms) ? (interval_ms - awake_ms) : 0;

  if (sleep_ms >= static_cast<uint32_t>(_config.deep_sleep_threshold_ms)) {
    return {SleepType::Deep, sleep_ms};
  }
  // Interval too short: deep sleep overhead (~3–4 s reboot) exceeds the
  // sleep duration.  Stay awake and let the main loop run normally.
  return {SleepType::None, 0};
}

// ---------------------------------------------------------------------------
// PM sensor hold — pure logic (no platform dependencies)
// ---------------------------------------------------------------------------

bool PowerService::should_hold_pm_sensor(uint32_t sleep_duration_ms) const {
  return _config.pin_pm_power >= 0 && sleep_duration_ms < _config.sensor_hold_max_sleep_ms;
}

bool PowerService::should_sleep_pm_sensor(uint32_t measure_interval_ms) const {
  return _config.pin_pm_power >= 0 && measure_interval_ms >= _config.pm_sleep_threshold_ms;
}

void PowerService::set_pm_power(bool on) {
  // EN_PM load switch only — cuts SPS30 fan current between measurements.
  // PMID (EN_OTG) is armed once in BQ25629Bms::init() and never toggled
  // here; per-cycle boost cold-starts can trip 1S cell-protection OCP.
  if (_config.pin_pm_power < 0) {
    return;
  }
  const uint8_t on_level = _config.pm_power_on_level;
  const uint8_t off_level = on_level ? 0 : 1;
  _gpio.set_level(_config.pin_pm_power, on ? on_level : off_level);
  AG_LOGI(TAG, "set_pm_power: %s", on ? "ON" : "OFF");
}

// ---------------------------------------------------------------------------
// Sleep entry — platform-specific, guarded by #ifndef TEST_HOST
// ---------------------------------------------------------------------------

void PowerService::enter_sleep(uint32_t sleep_duration_ms) {
#ifndef TEST_HOST
  // Hold PM sensor power GPIO during short sleeps so the sensor stays warm
  // and the next fast-path boot can skip the 10 s warmup.
  if (should_hold_pm_sensor(sleep_duration_ms)) {
    auto pin = static_cast<gpio_num_t>(_config.pin_pm_power);
    gpio_hold_en(pin);
    AG_LOGI(TAG, "enter_sleep: holding PM power GPIO %d for warm wake", _config.pin_pm_power);
  }

  AG_LOGI(TAG, "enter_sleep: entering deep sleep for %" PRIu32 " ms", sleep_duration_ms);
  configure_wake_sources(sleep_duration_ms);
  esp_deep_sleep_start();
  // Does not return — CPU reboots on wake.
#endif
}

// ---------------------------------------------------------------------------
// Wake source configuration — platform-specific, guarded by #ifndef TEST_HOST
// ---------------------------------------------------------------------------

void PowerService::configure_wake_sources(uint32_t timer_ms) {
#ifndef TEST_HOST
  // Timer wake: convert milliseconds to microseconds for the ESP-IDF API.
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(timer_ms) * 1000ULL);

  // GPIO wake: both buttons use EXT1 which supports multiple GPIOs in a
  // single bitmask.  The target (ESP32-C5) does not have EXT0; EXT1 is the
  // correct deep-sleep GPIO wake source.  Buttons are active-low, so wake
  // fires when ANY selected GPIO is pulled low.
  uint64_t wake_mask = 0;
  if (_config.pin_wake_button_power >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_power;
  }
  if (_config.pin_wake_button_boot >= 0) {
    wake_mask |= 1ULL << _config.pin_wake_button_boot;
  }
  if (wake_mask != 0) {
    esp_sleep_enable_ext1_wakeup(wake_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  }
#endif
}

// ---------------------------------------------------------------------------
// Boot path — static helpers
// ---------------------------------------------------------------------------

// static
WakeCause PowerService::get_wake_cause() {
#ifndef TEST_HOST
  const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  switch (cause) {
  case ESP_SLEEP_WAKEUP_TIMER:
    return WakeCause::Timer;
  case ESP_SLEEP_WAKEUP_EXT0:
  case ESP_SLEEP_WAKEUP_EXT1:
  case ESP_SLEEP_WAKEUP_GPIO:
    return WakeCause::Button;
  default:
    // ESP_SLEEP_WAKEUP_UNDEFINED = first power-on (not a wake from sleep).
    return WakeCause::PowerOn;
  }
#else
  return WakeCause::PowerOn;
#endif
}

// static
bool PowerService::is_fast_path_wake(WakeCause cause, const RtcAppState &state) {
  return cause == WakeCause::Timer && state.lock_state == LockState::Locked;
}

// static
void PowerService::release_sleep_gpio_holds(int pin_pm_power) {
#ifndef TEST_HOST
  if (pin_pm_power >= 0) {
    gpio_hold_dis(static_cast<gpio_num_t>(pin_pm_power));
  }
#else
  (void)pin_pm_power;
#endif
}

// ---------------------------------------------------------------------------
// Free function — early boot path
// ---------------------------------------------------------------------------

RtcAppState load_rtc_app_state() {
  if (!s_rtc_state_valid) {
    return RtcAppState{};
  }
  RtcAppState out{};
  memcpy(&out, &s_rtc_state, sizeof(RtcAppState));
  return out;
}
