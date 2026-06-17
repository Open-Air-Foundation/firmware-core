/**
 * AirGradient Go — Fuel-Gauge Learning Controller (pure FSM)
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "fg_learning_controller.h"

#include "ag_log.h"

static constexpr const char *TAG = "FgLearning";

namespace {

// Static hardware intent for a stage (no transition logic here).
FgLearningAction action_for_stage(FgLearningStage stage) {
  FgLearningAction a;
  switch (stage) {
  case FgLearningStage::Charge:
    a.active = true;
    a.set_charge_enabled = true;
    a.charge_current_ma = FgLearningController::CHARGE_CURRENT_MA;
    a.screen = Screen::FgLearnCharging;
    break;
  case FgLearningStage::Rest:
    a.active = true;
    a.low_power = true; // quiet load keeps the OCV window below Quit Current
    a.screen = Screen::FgLearnResting;
    break;
  case FgLearningStage::Discharge:
    a.active = true;
    a.manual_cue = ManualCue::Unplug;
    a.screen = Screen::FgLearnUnplug;
    break;
  case FgLearningStage::CycleDone:
    a.active = true;
    a.screen = Screen::DischargeComplete;
    break;
  case FgLearningStage::Verify:
    a.active = true;
    a.low_power = true;
    a.run_verify = true;
    a.screen = Screen::FgLearnVerifying;
    break;
  case FgLearningStage::Complete:
    a.manual_cue = ManualCue::Complete;
    a.screen = Screen::FgLearnComplete;
    break;
  case FgLearningStage::Failed:
    a.manual_cue = ManualCue::Failed;
    a.screen = Screen::FgLearnFailed;
    break;
  case FgLearningStage::Idle:
    break; // active=false, Screen::Home (defaults)
  }
  return a;
}

} // namespace

void FgLearningController::load(FgLearningStage stage, uint8_t cycle, uint8_t itpor_losses) {
  _stage = stage;
  _cycle = cycle;
  _itpor_losses = itpor_losses;
  _charging_observed = false;
  _timing_armed = false;
}

void FgLearningController::start() {
  _stage = FgLearningStage::Charge;
  _cycle = 1;
  _itpor_losses = 0;
  _charging_observed = false;
  _timing_armed = false;
  AG_LOGI(TAG, "learning armed: cycle 1/%u", CYCLE_TARGET);
}

void FgLearningController::reset() {
  _stage = FgLearningStage::Idle;
  _cycle = 0;
  _itpor_losses = 0;
  _charging_observed = false;
  _timing_armed = false;
}

void FgLearningController::_enter(FgLearningStage stage, uint32_t now_ms) {
  _stage = stage;
  _stage_entered_ms = now_ms;
  _timing_armed = true;
  if (stage == FgLearningStage::Charge) {
    _charging_observed = false;
  }
}

void FgLearningController::_por_loss_restart() {
  _itpor_losses = static_cast<uint8_t>(_itpor_losses + 1);
  if (_itpor_losses >= ITPOR_LOSS_CAP) {
    AG_LOGE(TAG, "POR-loss cap reached (%u) — learning failed; check EDV margin/cell",
            _itpor_losses);
    _stage = FgLearningStage::Failed;
  } else {
    AG_LOGW(TAG, "POR loss during run (%u/%u) — restarting cycle charge", _itpor_losses,
            ITPOR_LOSS_CAP);
    _stage = FgLearningStage::Charge;
    _charging_observed = false;
  }
}

bool FgLearningController::resume_on_boot(const PowerSnapshot &snap) {
  const FgLearningStage prev_stage = _stage;
  const uint8_t prev_cycle = _cycle;
  const uint8_t prev_losses = _itpor_losses;

  // The ONLY loss signal during a run is fg_itpor (a POR wiped RAM).  qmax_up
  // is legitimately 0 through all of cycle 1, so it must not gate resume.
  const bool por_loss = snap.fg_itpor;
  const bool on_charger = snap.external_input_present;

  switch (_stage) {
  case FgLearningStage::Idle:
    break; // normal operation
  case FgLearningStage::Complete:
    if (por_loss) {
      AG_LOGE(TAG, "POR wiped a completed learning result — re-arm needed");
      _stage = FgLearningStage::Failed;
    }
    break;
  case FgLearningStage::CycleDone:
    if (por_loss) {
      _por_loss_restart();
    } else if (_cycle < CYCLE_TARGET) {
      _stage = FgLearningStage::Charge;
      _cycle = static_cast<uint8_t>(_cycle + 1);
      _charging_observed = false;
    } else {
      _stage = FgLearningStage::Verify;
    }
    break;
  case FgLearningStage::Charge:
  case FgLearningStage::Rest:
  case FgLearningStage::Discharge:
    if (por_loss) {
      _por_loss_restart();
    } else if (on_charger) {
      _stage = FgLearningStage::Charge; // re-plugged: restart this cycle's charge
      _charging_observed = false;
    } else {
      _stage = FgLearningStage::Discharge; // on battery: spurious reset, keep draining
    }
    break;
  case FgLearningStage::Verify:
    break; // re-run verify
  case FgLearningStage::Failed:
    break; // await re-arm
  }

  _timing_armed = false; // re-arm phase timing at the current clock
  return _stage != prev_stage || _cycle != prev_cycle || _itpor_losses != prev_losses;
}

FgLearningAction FgLearningController::tick(const PowerSnapshot &snap, uint32_t now_ms) {
  if (!_timing_armed) {
    _stage_entered_ms = now_ms;
    _timing_armed = true;
  }

  const FgLearningStage prev = _stage;

  switch (_stage) {
  case FgLearningStage::Charge: {
    if (is_bms_charging(snap.charging_status)) {
      _charging_observed = true;
    }
    const bool bms_terminated =
        _charging_observed && snap.charging_status == BmsChargingState::ChargeTerminationDone;
    if (snap.fg_flag_fc || bms_terminated) {
      if (!snap.fg_flag_fc) {
        AG_LOGW(TAG, "FC never latched; advancing on BMS charge-termination (OCV1 at relaxed top)");
      }
      _enter(FgLearningStage::Rest, now_ms);
    } else if (now_ms - _stage_entered_ms >= CHARGE_TIMEOUT_MS) {
      AG_LOGE(TAG, "charge timeout — learning failed");
      _enter(FgLearningStage::Failed, now_ms);
    }
    break;
  }
  case FgLearningStage::Rest:
    if (snap.fg_ocv_taken && (now_ms - _stage_entered_ms >= REST_TIMEOUT_MS)) {
      _enter(FgLearningStage::Discharge, now_ms);
    }
    break;
  case FgLearningStage::Discharge:
    if (snap.edv_cutoff_reached) {
      _enter(FgLearningStage::CycleDone, now_ms);
    }
    break;
  case FgLearningStage::CycleDone:
    // Holds until the orchestrator's EDV hook persists+ships and the device
    // powers off; resume_on_boot() advances on the next boot.
    break;
  case FgLearningStage::Verify:
    // on_verify_result() drives the exit; tick only requests the read.
    break;
  case FgLearningStage::Idle:
  case FgLearningStage::Complete:
  case FgLearningStage::Failed:
    break;
  }

  FgLearningAction action = action_for_stage(_stage);
  if (_stage != prev) {
    action.persist_stage = true;
  }
  return action;
}

bool FgLearningController::on_verify_result(const VerifyInputs &in) {
  if (verify_pass(in)) {
    _stage = FgLearningStage::Complete;
    _timing_armed = false;
    AG_LOGI(TAG, "verify pass: Qmax=%u mAh (DC=%u)", in.qmax_mah, in.design_capacity_mah);
    return true;
  }
  if (_cycle >= CYCLE_TARGET) {
    AG_LOGE(TAG, "verify failed at cycle cap — learning failed");
    _stage = FgLearningStage::Failed;
  } else {
    // Retained for fidelity with the reference flow; not reachable on the
    // normal reboot path (Verify is only entered once cycle >= CYCLE_TARGET).
    _stage = FgLearningStage::Charge;
    _cycle = static_cast<uint8_t>(_cycle + 1);
    _charging_observed = false;
  }
  _timing_armed = false;
  return false;
}

bool FgLearningController::verify_pass(const VerifyInputs &in) {
  if (!in.reads_ok) {
    return false;
  }
  if (in.itpor) {
    return false; // a POR wiped learning
  }
  if (!in.qmax_up) {
    return false; // Qmax never updated
  }
  if (in.design_capacity_mah == 0) {
    return false; // cannot band-check without a design capacity
  }

  // Qmax within [0.7 x DC, 1.4 x DC].
  const uint32_t dc = in.design_capacity_mah;
  const uint32_t low = dc * QMAX_MIN_TENTHS / 10u;
  const uint32_t high = dc * QMAX_MAX_TENTHS / 10u;
  if (in.qmax_mah < low || in.qmax_mah > high) {
    return false;
  }

  // Criterion 4: Ra grid healthy.  The full "moved off ROM defaults + smooth
  // adjacent transitions" tolerance is bench-pending (spec Open Question: Ra
  // default band).  For now require every entry strictly positive and the grid
  // not flat (proxy for having moved off a uniform ROM default).
  // TODO(bench): tighten against a captured learned grid.
  int16_t min_ra = in.ra[0];
  int16_t max_ra = in.ra[0];
  for (size_t i = 0; i < FG_LEARNING_RA_TABLE_SIZE; ++i) {
    if (in.ra[i] <= 0) {
      return false;
    }
    if (in.ra[i] < min_ra) {
      min_ra = in.ra[i];
    }
    if (in.ra[i] > max_ra) {
      max_ra = in.ra[i];
    }
  }
  if (min_ra == max_ra) {
    return false; // flat grid → not learned
  }

  return true;
}
