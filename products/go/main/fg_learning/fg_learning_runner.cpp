/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "fg_learning/fg_learning_runner.h"

#include "board_config.h"
#include "buzzer/go_buzzer.h"
#include "config_store.h"
#include "go_board.h"
#include "go_display.h"
#include "go_melody.h"
#include "go_settings.h"
#include "go_storage.h"
#include "led/go_led.h"

#include <esp_system.h> // esp_reset_reason()

#include "ag_log.h"
#include "common.h" // reboot()
#include "rtos.h"

static constexpr const char *TAG = "FgLearnRunner";

namespace {

// Poll cadence — capped well under the 60 s external-WDT window. The EDV
// debounce is sample-count based, so wall-clock debounce = 3 x this.
constexpr uint32_t FACTORY_LEARNING_POLL_MS = 5000;

// External HW watchdog feed interval (< 60 s window).
constexpr uint32_t FG_LEARNING_EXT_WDT_FEED_MS = 30000;

// Power-button debounce. A short press past this clears the run (or exits a
// terminal screen). Sampled at ABORT_POLL_MS, so the effective hold is ~one slice.
constexpr uint32_t POWER_PRESS_DEBOUNCE_MS = 50;

// Abort-button sampling interval during the idle wait.
constexpr uint32_t ABORT_POLL_MS = 100;

// Bounded retries for the pre-ship CycleDone commit.
constexpr int EDV_COMMIT_RETRY_MAX = 3;

// Discharge CPU-active load: per ABORT_POLL_MS slice, busy-spin this long then
// yield the remainder. This deliberate battery-side draw holds the gauge above
// its ~120 mA Dsg current threshold so Ra learns; the yield remainder lets the
// idle task run so the Task WDT stays fed. ~90% duty — bench-tunable.
constexpr uint32_t FG_LEARNING_CPU_ACTIVE_MS = 90;
static_assert(FG_LEARNING_CPU_ACTIVE_MS < ABORT_POLL_MS,
              "CPU active span must leave a yield remainder each slice");

// Settle time after powering EN_PM before starting the SPS30 (sensor boot).
constexpr uint32_t PM_SETTLE_MS = 500;

// Battery-side discharge magnitude (mA) at/above which the gauge qualifies the
// DISCHARGE state for Ra learning (the ~120 mA Dsg Current Threshold). Used
// only to journal the first qualifying poll — not a control decision.
constexpr int DSG_QUALIFY_MA = 120;

// Unplug-prompt LED breathing period (ms). Only active while plugged (bus healthy).
constexpr uint32_t UNPLUG_BREATHE_MS = 2000;

bool is_terminal(FgLearningStage s) {
  return s == FgLearningStage::Complete || s == FgLearningStage::Failed;
}

// Deliberate CPU-active burst — the volatile accumulator stops the optimiser
// eliding the work.
void busy_spin_ms(uint32_t ms) {
  const uint32_t end = RTOS::get_time_ms() + ms;
  volatile uint32_t acc = 0;
  while (RTOS::get_time_ms() < end) {
    acc = acc + 1;
  }
  (void)acc;
}

} // namespace

FgLearningRunner::FgLearningRunner(const Deps &deps)
    : _controller(deps.power.fg_learning_rest_min_ms()), _deps(deps) {}

void FgLearningRunner::run() {
  // Display first: init() sets up the u8g2 renderer + SPI driver and does the
  // first full refresh; update_sync() later REQUIRES this prior init().
  DisplayValues boot = build_dashboard_values(PowerSnapshot{});
  boot.fg_dashboard.valid = false; // no gauge data yet at bring-up
  _deps.display.init(boot, /*defer_refresh=*/false);

  // LED + buzzer need BOTH init() and start(): start() creates the worker task;
  // queued commands are dropped while not started.
  _deps.led.init();
  _deps.led.start();
  _deps.led.back_set_brightness(LedBrightness::Bright); // default is Off → cues invisible
  _deps.buzzer.init();
  _deps.buzzer.start();
  _deps.buzzer.set_enabled(true);

  // Persistent journal on NAND (mounted by board.storage()). Replay the prior
  // history to serial first so connecting mid-run shows everything that already
  // happened, then resume_on_boot() appends this boot's BOOT record.
  _journal.begin(_deps.storage.mount_path(), _deps.storage.is_mounted());
  _journal.dump_to_serial();

  resume_on_boot();

  for (;;) {
    const uint32_t now = RTOS::get_time_ms();
    feed_ext_watchdog(now); // MUST run < every 60 s
    PowerSnapshot snap = _deps.power.poll_bms_fg_learning();

    journal_flag_edges(snap); // ITPOR / QMAX_UP / RES_UP / discharge-qualified, once each

    handle_edv_ship(snap); // ships (does not return) on EDV during Discharge

    const FgLearningAction action = _controller.tick(snap, now);
    apply_action(action, snap.external_input_present);
    // Unplug flips PMID (input -> OTG boost), browning the SPS30 out of
    // measuring even though EN_PM holds. Re-enable PM on the unplug edge so the
    // retry below re-inits the fan. _prev_* still hold last poll's values here.
    const bool charger_unplugged =
        _prev_inputs_valid && _prev_external_input_present && !snap.external_input_present;
    if (_discharge_load_on && charger_unplugged) {
      _deps.board.stop_pm_fan(); // clear inited flag -> next start re-inits
      _pm_fan_running = false;
    }
    // Keep retrying until the PM fan is confirmed measuring (valid read).
    if (_discharge_load_on && !_pm_fan_running) {
      _pm_fan_running = _deps.board.start_pm_fan();
    }
    if (action.run_verify) {
      run_verify();
    }

    const FgLearningStage stage = _controller.stage();
    const bool stage_changed = !_prev_stage_valid || stage != _prev_stage;
    if (stage_changed) {
      _stage_entered_ms = now; // reset the per-stage elapsed clock
      _journal.stage_enter(stage, _controller.cycle(), snap.fg_soc_percent, snap.fg_voltage_mv);
    }
    _prev_stage = stage;
    _prev_stage_valid = true;

    // The bottom rest (OCV2) has to happen powered off: a cell at the discharge
    // floor cannot feed the system for the hours the gauge needs, and only the
    // rev 1 EDV path used to reach ship mode.  Re-plug reboots into the resume
    // matrix, which carries the run on from the persisted CycleDone.
    if (stage == FgLearningStage::CycleDone) {
      _journal.target_ship(_controller.cycle(), snap.fg_soc_percent, snap.fg_voltage_mv);
      ship_after_cycle(snap); // does not return on target
    }

    if (is_terminal(stage)) {
      handback_terminal(); // does not return
    }

    // Repaint on a stage change, a charging-state / plug change, or the
    // heartbeat — so plug/unplug shows within a poll instead of a heartbeat.
    const bool inputs_changed = !_prev_inputs_valid ||
                                snap.charging_status != _prev_charging_status ||
                                snap.external_input_present != _prev_external_input_present;
    if (stage_changed || inputs_changed ||
        (now - _last_paint_ms) >= FG_LEARNING_DISPLAY_REFRESH_MS) {
      refresh_dashboard(snap);
    }
    _prev_charging_status = snap.charging_status;
    _prev_external_input_present = snap.external_input_present;
    _prev_inputs_valid = true;

    // Responsive idle wait: samples the abort button every ABORT_POLL_MS so a
    // short power press is caught (the poll cadence alone is far too coarse).
    // While discharging it also spends most of each slice on the CPU-active
    // load (steady battery-side draw to qualify DISCHARGE).
    idle_poll(FACTORY_LEARNING_POLL_MS, /*watch_abort=*/true);
  }
}

void FgLearningRunner::resume_on_boot() {
  FactorySettings fs{};
  load_factory_settings(_deps.config_store, fs);
  _controller.load(fs.fg_learning_stage, fs.fg_learning_cycle, fs.fg_learning_itpor_losses);
  _controller.restore_fail_reason(static_cast<FgLearnFailReason>(fs.fg_learning_fail_reason));

  // Idempotent chemistry prerequisite (only writes when the Chem ID is wrong).
  _deps.power.set_chemistry_4v2();

  // Journal this boot — the ESP reset reason is otherwise NEVER logged on the
  // learning path (go_app enters it before the normal reset-reason log).
  PowerSnapshot snap = _deps.power.poll_bms_fg_learning();
  const bool itpor_now = (snap.fg_learning_flags & FG_LEARN_ITPOR) != 0;
  _journal.boot(static_cast<int>(esp_reset_reason()), _controller.stage(), _controller.cycle(),
                _controller.itpor_losses(), itpor_now, snap.fg_soc_percent, snap.fg_voltage_mv);

  if (is_terminal(_controller.stage())) {
    handback_terminal(); // sticky terminal hold (Complete/Failed); does not return
  }

  // Cycle-1 entry: lift the gauge change limits so Qmax/Ra move freely.
  if (_controller.cycle() == 1) {
    _deps.power.set_update_status_learning(true);
  }

  _controller.resume_on_boot(snap);
}

void FgLearningRunner::apply_action(const FgLearningAction &a, bool ext_input) {
  _screen = a.screen;
  const FgLearningStage stage = _controller.stage();
  const bool transitioned = a.persist_stage;

  // Charge control.
  if (a.set_charge_enabled) {
    _deps.power.set_manual_charge_disabled(false);
    _deps.power.set_charge_current_ma(a.charge_current_ma);
  } else {
    _deps.power.set_manual_charge_disabled(true);
  }

  // Controlled load stack: PM + CPU duty on only during Discharge.
  set_discharge_load(stage == FgLearningStage::Discharge);

  // Back LED: derived from (stage, plug state), written once per phase change.
  // Breathing is only used while plugged (healthy bus); Discharging is solid.
  LedPhase phase = LedPhase::Off;
  switch (stage) {
  case FgLearningStage::Rest:
    phase = LedPhase::Rest;
    break;
  case FgLearningStage::Discharge:
    phase = ext_input ? LedPhase::UnplugPrompt : LedPhase::Discharging;
    break;
  case FgLearningStage::Complete:
    phase = LedPhase::Complete;
    break;
  case FgLearningStage::Failed:
    phase = LedPhase::Failed;
    break;
  default:
    break;
  }
  if (!_led_applied || phase != _last_led_phase) {
    switch (phase) {
    case LedPhase::Rest:
      _deps.led.back_solid(Rgb{255, 140, 0}); // amber
      break;
    case LedPhase::UnplugPrompt:
      _deps.led.back_breathe(Rgb{160, 0, 255}, UNPLUG_BREATHE_MS); // purple breathe
      break;
    case LedPhase::Discharging:
      _deps.led.back_solid(Rgb{0, 0, 255}); // blue
      break;
    case LedPhase::Complete:
      _deps.led.back_solid(Rgb{0, 255, 0}); // green
      break;
    case LedPhase::Failed:
      _deps.led.back_solid(Rgb{255, 0, 0}); // red
      break;
    case LedPhase::Off:
      _deps.led.back_off();
      break;
    }
    _last_led_phase = phase;
    _led_applied = true;
  }

  // Unplug cue: fire the alert melody once on entry to Discharge.
  if (transitioned && a.manual_cue == ManualCue::Unplug) {
    _deps.buzzer.play(PATTERN_UNPLUG, PATTERN_UNPLUG_COUNT);
  }

  // Persist the new stage on any transition.
  if (transitioned) {
    save_fg_learning_state(_deps.config_store, stage, _controller.cycle(),
                           _controller.itpor_losses());
  }
}

void FgLearningRunner::run_verify() {
  const FgLearningVerifyReadout r = _deps.power.read_fg_learning_verify();

  VerifyInputs in;
  in.reads_ok = r.ok;
  in.itpor = r.itpor;
  in.qmax_up = r.qmax_up;
  in.qmax_mah = r.qmax_mah;
  in.design_capacity_mah = r.design_capacity_mah;
  static_assert(FG_RA_TABLE_SIZE == FG_LEARNING_RA_TABLE_SIZE, "Ra table size mismatch");
  for (size_t i = 0; i < FG_LEARNING_RA_TABLE_SIZE; ++i) {
    in.ra[i] = r.ra[i];
  }

  const FgLearnFailReason reason = FgLearningController::verify_fail_reason(in);
  const bool pass = _controller.on_verify_result(in);
  _journal.verify(in.reads_ok, in.itpor, in.qmax_up, in.qmax_mah, in.design_capacity_mah, in.ra,
                  FG_LEARNING_RA_TABLE_SIZE, pass, reason);
  save_fg_learning_state(_deps.config_store, _controller.stage(), _controller.cycle(),
                         _controller.itpor_losses());
  save_fg_learning_fail_reason(_deps.config_store, static_cast<uint8_t>(_controller.fail_reason()));
}

bool FgLearningRunner::handle_edv_ship(const PowerSnapshot &snap) {
  if (!snap.edv_cutoff_reached || _controller.stage() != FgLearningStage::Discharge) {
    return false;
  }
  _journal.edv_ship(_controller.cycle(), snap.fg_soc_percent, snap.fg_voltage_mv);
  ship_after_cycle(snap);
  return true;
}

void FgLearningRunner::ship_after_cycle(const PowerSnapshot &snap) {
  // Battery safety beats resumability: stop draining first so the cell
  // recovers before the rest.
  set_discharge_load(false);

  // Persist CycleDone, then ship regardless of the commit result — never linger
  // discharging at the floor.
  for (int i = 0; i < EDV_COMMIT_RETRY_MAX; ++i) {
    if (save_fg_learning_state(_deps.config_store, FgLearningStage::CycleDone, _controller.cycle(),
                               _controller.itpor_losses())) {
      break;
    }
  }

  _screen = Screen::DischargeComplete;
  refresh_dashboard(snap); // last frame shown before ship-off
  _deps.power.shutdown();  // ship mode — does not return
}

void FgLearningRunner::feed_ext_watchdog(uint32_t now) {
  if (_last_wdt_ms != 0 && (now - _last_wdt_ms) < FG_LEARNING_EXT_WDT_FEED_MS) {
    return;
  }
  _deps.power.reset_ext_watchdog();
  _last_wdt_ms = now;
}

void FgLearningRunner::set_discharge_load(bool on) {
  if (on == _discharge_load_on) {
    return;
  }
  _discharge_load_on = on;
  if (on) {
    // Power the PM rail; the fan is started+confirmed by the run() loop's
    // per-tick retry (start_pm_fan) until a valid PM read.
    _deps.power.set_pm_power(true);
    RTOS::delay_ms(PM_SETTLE_MS);
  } else {
    _deps.board.stop_pm_fan();
    _deps.power.set_pm_power(false);
    _pm_fan_running = false;
  }
  // CPU duty is applied per slice in idle_poll() while _discharge_load_on.
}

DisplayValues FgLearningRunner::build_dashboard_values(const PowerSnapshot &snap) const {
  DisplayValues v{};
  v.show_fg_dashboard = true;
  v.screen = _screen;

  FgLearningDashboardData &d = v.fg_dashboard;
  d.valid = snap.fg_soc_percent != BmsInvalid::SOC_PERCENT;
  d.cycle = _controller.cycle();
  d.cycle_target = FgLearningController::CYCLE_TARGET;
  d.soc_pct = snap.fg_soc_percent;
  d.voltage_mv = snap.fg_voltage_mv;
  d.current_ma = snap.fg_current_ma;
  d.remaining_mah = snap.fg_remaining_capacity_mah;
  d.full_charge_mah = snap.fg_full_charge_capacity_mah;
  d.design_capacity_mah = snap.fg_design_capacity_mah;
  d.temperature_c = snap.fg_internal_temperature_c;
  d.flag_fc = (snap.fg_learning_flags & FG_LEARN_FC) != 0;
  d.flag_chg = (snap.fg_learning_flags & FG_LEARN_CHG) != 0;
  d.flag_dsg = (snap.fg_learning_flags & FG_LEARN_DSG) != 0;
  d.qmax_up = (snap.fg_learning_flags & FG_LEARN_QMAX_UP) != 0;
  d.res_up = (snap.fg_learning_flags & FG_LEARN_RES_UP) != 0;
  d.ocv_taken = (snap.fg_learning_flags & FG_LEARN_OCV_TAKEN) != 0;
  d.charge_current_ma = (_controller.stage() == FgLearningStage::Charge)
                            ? FgLearningController::CHARGE_CURRENT_MA
                            : 0;
  d.bms_charging_state = static_cast<uint8_t>(snap.charging_status);
  d.stage_elapsed_ms = RTOS::get_time_ms() - _stage_entered_ms;
  d.external_input_present = snap.external_input_present;
  // Fail reason (static string) for the Failed screen; nullptr when not failed.
  d.fail_reason = (_controller.fail_reason() == FgLearnFailReason::None)
                      ? nullptr
                      : fg_learn_fail_reason_str(_controller.fail_reason());
  return v;
}

void FgLearningRunner::journal_flag_edges(const PowerSnapshot &snap) {
  const uint8_t soc = snap.fg_soc_percent;
  const uint16_t vbat = snap.fg_voltage_mv;
  const int16_t cur = snap.fg_current_ma;
  if (!_seen_itpor && (snap.fg_learning_flags & FG_LEARN_ITPOR)) {
    _seen_itpor = true;
    _journal.flag_event("ITPOR", soc, vbat, cur);
  }
  if (!_seen_qmax_up && (snap.fg_learning_flags & FG_LEARN_QMAX_UP)) {
    _seen_qmax_up = true;
    _journal.flag_event("QMAX_UP", soc, vbat, cur);
  }
  if (!_seen_res_up && (snap.fg_learning_flags & FG_LEARN_RES_UP)) {
    _seen_res_up = true;
    _journal.flag_event("RES_UP", soc, vbat, cur);
  }
  // First poll where the battery-side discharge magnitude clears the gauge's
  // ~120 mA Dsg threshold — proof the discharge load is qualifying for Ra.
  if (!_seen_dsg_qualified && _discharge_load_on && cur != BmsInvalid::CURRENT_MA &&
      cur <= -DSG_QUALIFY_MA) {
    _seen_dsg_qualified = true;
    _journal.flag_event("DSG_QUALIFIED", soc, vbat, cur);
  }
}

void FgLearningRunner::refresh_dashboard(const PowerSnapshot &snap) {
  const DisplayValues v = build_dashboard_values(snap);
  _deps.display.update_sync(v); // full refresh; HW reset wakes the panel
  _deps.display.deep_sleep();
  _last_paint_ms = RTOS::get_time_ms();
}

bool FgLearningRunner::poll_abort_button() {
  const gpio::Hal &g = _deps.board.gpio_hal();
  if (!_abort_btn_ready) {
    g.configure(static_cast<int>(PIN_BUTTON_POWER), gpio::Mode::Input, gpio::PullMode::PullUp,
                gpio::InterruptType::Disabled);
    _abort_btn_ready = true;
  }

  const bool pressed = g.get_level(static_cast<int>(PIN_BUTTON_POWER)) == 0; // active-low
  const uint32_t now = RTOS::get_time_ms();
  if (!pressed) {
    _power_btn_down_ms = 0;
    return false;
  }
  if (_power_btn_down_ms == 0) {
    _power_btn_down_ms = now; // press start
    return false;
  }
  if ((now - _power_btn_down_ms) < POWER_PRESS_DEBOUNCE_MS) {
    return false;
  }

  // Terminal exits are expected operator finishes (info); a press mid-run is a
  // genuine abort of in-progress work (warn). Action is identical for all.
  const FgLearningStage stage = _controller.stage();
  if (stage == FgLearningStage::Complete) {
    AG_LOGI(TAG, "power press -> exit Complete, reboot to normal");
  } else if (stage == FgLearningStage::Failed) {
    AG_LOGI(TAG, "power press -> exit Failed, reboot to normal");
  } else {
    AG_LOGW(TAG, "power press -> abort run, clearing learning state");
  }
  _controller.reset();
  clear_factory_settings(_deps.config_store);
  _journal.reset(); // wipe the run's diagnostic trail on exit
  reboot();         // into normal operation — does not return on target
  return true;
}

void FgLearningRunner::idle_poll(uint32_t total_ms, bool watch_abort) {
  const uint32_t end = RTOS::get_time_ms() + total_ms;
  while (RTOS::get_time_ms() < end) {
    if (watch_abort) {
      poll_abort_button(); // short press clears + reboots (does not return)
    }
    if (_discharge_load_on) {
      // Discharge: spend most of the slice on the deliberate CPU load, then
      // yield the remainder so the idle task runs and the Task WDT stays fed.
      busy_spin_ms(FG_LEARNING_CPU_ACTIVE_MS);
      RTOS::delay_ms(ABORT_POLL_MS - FG_LEARNING_CPU_ACTIVE_MS);
    } else {
      RTOS::delay_ms(ABORT_POLL_MS);
    }
  }
}

void FgLearningRunner::terminal_cleanup() {
  _deps.power.set_manual_charge_disabled(false); // restore normal charge path
  set_discharge_load(false);                     // PM off (GPS/VOC unused)
  _deps.power.set_update_status_learning(false); // restore bounded field limits
}

void FgLearningRunner::handback_terminal() {
  const FgLearningStage stage = _controller.stage();
  terminal_cleanup();
  save_fg_learning_state(_deps.config_store, stage, _controller.cycle(),
                         _controller.itpor_losses());
  save_fg_learning_fail_reason(_deps.config_store, static_cast<uint8_t>(_controller.fail_reason()));

  // Journal the verdict, then replay the whole trail to serial so a console
  // connected only now still sees the full run history.
  _journal.result(stage, _controller.fail_reason());
  _journal.dump_to_serial();

  // Result frame + solid LED.
  _screen = (stage == FgLearningStage::Complete) ? Screen::FgLearnComplete : Screen::FgLearnFailed;
  PowerSnapshot snap = _deps.power.poll_bms_fg_learning();
  refresh_dashboard(snap);
  _deps.led.back_solid(stage == FgLearningStage::Complete ? Rgb{0, 255, 0} : Rgb{255, 0, 0});

  // Both terminals are sticky across power-cycle; a POWER press clears the run
  // and reboots to normal operation (does not return).
  for (;;) {
    feed_ext_watchdog(RTOS::get_time_ms());
    idle_poll(FACTORY_LEARNING_POLL_MS, /*watch_abort=*/true);
  }
}
