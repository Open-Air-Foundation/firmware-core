/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "fg_learning_controller.h"

const char *fg_learn_fail_reason_str(FgLearnFailReason r) {
  switch (r) {
  case FgLearnFailReason::None:
    return "None";
  case FgLearnFailReason::ChargeTimeout:
    return "ChargeTimeout";
  case FgLearnFailReason::PorLossCap:
    return "PorLossCap";
  case FgLearnFailReason::VerifyReadFail:
    return "VerifyReadFail";
  case FgLearnFailReason::VerifyItpor:
    return "VerifyItpor";
  case FgLearnFailReason::VerifyQmaxNotUpdated:
    return "QmaxNotUpdated";
  case FgLearnFailReason::VerifyDesignCapZero:
    return "DesignCapZero";
  case FgLearnFailReason::VerifyQmaxOutOfBand:
    return "QmaxOutOfBand";
  case FgLearnFailReason::VerifyRaInvalid:
    return "RaInvalid";
  }
  return "?";
}

void FgLearningController::load(FgLearningStage stage, uint8_t cycle, uint8_t itpor_losses) {
  _stage = stage;
  _cycle = cycle;
  _itpor_losses = itpor_losses;
  _fail_reason = FgLearnFailReason::None;
  _stage_time_set = false;
  _charge_observed = false;
}

void FgLearningController::start() {
  _stage = FgLearningStage::Charge;
  _cycle = 1;
  _itpor_losses = 0;
  _fail_reason = FgLearnFailReason::None;
  _stage_time_set = false;
  _charge_observed = false;
}

void FgLearningController::reset() {
  _stage = FgLearningStage::Idle;
  _cycle = 0;
  _itpor_losses = 0;
  _fail_reason = FgLearnFailReason::None;
  _stage_time_set = false;
  _charge_observed = false;
}

void FgLearningController::enter_stage(FgLearningStage stage, uint32_t now_ms) {
  _stage = stage;
  _stage_entered_ms = now_ms;
  _stage_time_set = true;
  if (stage == FgLearningStage::Charge) {
    _charge_observed = false;
  }
}

void FgLearningController::por_loss_restart() {
  _itpor_losses++;
  if (_itpor_losses >= ITPOR_LOSS_CAP) {
    _stage = FgLearningStage::Failed; // cap reached: cell/EDV margin needs attention
    _fail_reason = FgLearnFailReason::PorLossCap;
  } else {
    _stage = FgLearningStage::Charge; // restart the current cycle (cycle unchanged)
    _charge_observed = false;
  }
  _stage_time_set = false;
}

bool FgLearningController::resume_on_boot(const PowerSnapshot &snap) {
  // ITPOR alone is the loss signal during a run; QMAX_UP is legitimately 0 for
  // all of cycle 1, so it must not gate resume.
  const bool por = (snap.fg_learning_flags & FG_LEARN_ITPOR) != 0;
  const bool on_charger = snap.external_input_present;

  switch (_stage) {
  case FgLearningStage::Idle:
    return false;

  case FgLearningStage::Complete:
    if (por) {
      _stage = FgLearningStage::Failed; // learning lost; re-arm needed
      _stage_time_set = false;
      return true;
    }
    return false;

  case FgLearningStage::CycleDone:
    if (por) {
      por_loss_restart();
    } else if (_cycle < CYCLE_TARGET) {
      _cycle++;
      _stage = FgLearningStage::Charge;
      _charge_observed = false;
      _stage_time_set = false;
    } else {
      _stage = FgLearningStage::Verify;
      _stage_time_set = false;
    }
    return true;

  case FgLearningStage::Charge:
  case FgLearningStage::Rest:
  case FgLearningStage::Discharge:
    if (por) {
      por_loss_restart();
    } else if (on_charger) {
      _stage = FgLearningStage::Charge; // restart this cycle's charge
      _charge_observed = false;
      _stage_time_set = false;
    } else {
      _stage = FgLearningStage::Discharge; // spurious reset on battery: keep draining
      _stage_time_set = false;
    }
    return true;

  case FgLearningStage::Verify:
    _stage_time_set = false; // re-run verify
    return true;

  case FgLearningStage::Failed:
    return true; // sticky; await re-arm
  }
  return false;
}

FgLearningAction FgLearningController::build_action() const {
  FgLearningAction a;
  switch (_stage) {
  case FgLearningStage::Charge:
    a.active = true;
    a.set_charge_enabled = true;
    a.charge_current_ma = CHARGE_CURRENT_MA;
    a.low_power = false;
    a.screen = Screen::FgLearnCharging;
    break;
  case FgLearningStage::Rest:
    a.active = true;
    a.low_power = true;
    a.screen = Screen::FgLearnResting;
    break;
  case FgLearningStage::Discharge:
    a.active = true;
    a.low_power = false;
    a.manual_cue = ManualCue::Unplug;
    a.screen = Screen::FgLearnUnplug;
    break;
  case FgLearningStage::CycleDone:
    a.active = true;
    a.low_power = true;
    a.screen = Screen::DischargeComplete;
    break;
  case FgLearningStage::Verify:
    a.active = true;
    a.low_power = true;
    a.run_verify = true;
    a.screen = Screen::FgLearnVerifying;
    break;
  case FgLearningStage::Complete:
    a.active = false;
    a.manual_cue = ManualCue::Complete;
    a.screen = Screen::FgLearnComplete;
    break;
  case FgLearningStage::Failed:
    a.active = false;
    a.manual_cue = ManualCue::Failed;
    a.screen = Screen::FgLearnFailed;
    break;
  case FgLearningStage::Idle:
    a.active = false;
    a.screen = Screen::Home;
    break;
  }
  return a;
}

FgLearningAction FgLearningController::tick(const PowerSnapshot &snap, uint32_t now_ms) {
  if (!_stage_time_set) {
    _stage_entered_ms = now_ms;
    _stage_time_set = true;
  }

  const FgLearningStage prev = _stage;

  switch (_stage) {
  case FgLearningStage::Charge:
    if (is_bms_charging(snap.charging_status)) {
      _charge_observed = true;
    }
    // Advance on FC flag, or on BMS charge-termination once charging was
    // actually observed (guards the Taper-Voltage mismatch where FC never sets).
    if ((snap.fg_learning_flags & FG_LEARN_FC) ||
        (snap.charging_status == BmsChargingState::ChargeTerminationDone && _charge_observed)) {
      enter_stage(FgLearningStage::Rest, now_ms);
    } else if (now_ms - _stage_entered_ms >= CHARGE_TIMEOUT_MS) {
      enter_stage(FgLearningStage::Failed, now_ms);
      _fail_reason = FgLearnFailReason::ChargeTimeout;
    }
    break;

  case FgLearningStage::Rest:
    if ((snap.fg_learning_flags & FG_LEARN_OCV_TAKEN) &&
        (now_ms - _stage_entered_ms >= _rest_min_ms)) {
      enter_stage(FgLearningStage::Discharge, now_ms);
    }
    break;

  case FgLearningStage::Discharge:
    if (snap.discharge_target_reached) {
      enter_stage(FgLearningStage::CycleDone, now_ms);
    }
    break;

  case FgLearningStage::Idle:
  case FgLearningStage::CycleDone:
  case FgLearningStage::Verify:
  case FgLearningStage::Complete:
  case FgLearningStage::Failed:
    break;
  }

  FgLearningAction a = build_action();
  a.persist_stage = (_stage != prev);
  return a;
}

bool FgLearningController::on_verify_result(const VerifyInputs &in) {
  const FgLearnFailReason reason = verify_fail_reason(in);
  const bool pass = (reason == FgLearnFailReason::None);
  if (pass) {
    _stage = FgLearningStage::Complete;
  } else if (_cycle >= CYCLE_TARGET) {
    _stage = FgLearningStage::Failed;
    _fail_reason = reason;
  } else {
    // Guard branch — not reachable through the normal reboot path (Verify is
    // entered only at the cycle cap), but kept for fidelity.
    _cycle++;
    _stage = FgLearningStage::Charge;
    _charge_observed = false;
  }
  _stage_time_set = false;
  return pass;
}

bool FgLearningController::verify_pass(const VerifyInputs &in) {
  return verify_fail_reason(in) == FgLearnFailReason::None;
}

FgLearnFailReason FgLearningController::verify_fail_reason(const VerifyInputs &in) {
  if (!in.reads_ok) {
    return FgLearnFailReason::VerifyReadFail;
  }
  if (in.itpor) { // a POR since learning
    return FgLearnFailReason::VerifyItpor;
  }
  if (!in.qmax_up) {
    return FgLearnFailReason::VerifyQmaxNotUpdated;
  }
  if (in.design_capacity_mah == 0) {
    return FgLearnFailReason::VerifyDesignCapZero;
  }
  const float dc = static_cast<float>(in.design_capacity_mah);
  if (static_cast<float>(in.qmax_mah) < QMAX_MIN_FRACTION * dc ||
      static_cast<float>(in.qmax_mah) > QMAX_MAX_FRACTION * dc) {
    return FgLearnFailReason::VerifyQmaxOutOfBand;
  }
  // Ra grid healthy: every value must be positive. The "moved off ROM
  // defaults across the whole range" tolerance is bench-pending (needs a real
  // learned grid) and is layered on later without changing this gate.
  for (size_t i = 0; i < FG_LEARNING_RA_TABLE_SIZE; i++) {
    if (in.ra[i] <= 0) {
      return FgLearnFailReason::VerifyRaInvalid;
    }
  }
  return FgLearnFailReason::None;
}
