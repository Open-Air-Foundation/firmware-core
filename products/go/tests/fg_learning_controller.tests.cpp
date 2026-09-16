/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "fg_learning/fg_learning_controller.h"

#include <catch2/catch_test_macros.hpp>

namespace {

constexpr uint16_t DESIGN_CAP = 2000;

// A snapshot that, on its own, advances nothing.
PowerSnapshot quiet_snapshot() {
  PowerSnapshot s;
  s.charging_status = BmsChargingState::NotCharging;
  return s;
}

// A fully-passing verify input.
VerifyInputs passing_verify() {
  VerifyInputs in;
  in.reads_ok = true;
  in.itpor = false;
  in.qmax_up = true;
  in.qmax_mah = DESIGN_CAP;
  in.design_capacity_mah = DESIGN_CAP;
  for (auto &v : in.ra) {
    v = 100;
  }
  return in;
}

} // namespace

// ============================================================================
// Lifecycle: start / reset / load
// ============================================================================

TEST_CASE("start arms a fresh run at Charge cycle 1", "[fg]") {
  FgLearningController c;
  c.start();
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.cycle() == 1);
  REQUIRE(c.itpor_losses() == 0);
}

TEST_CASE("reset clears to Idle", "[fg]") {
  FgLearningController c;
  c.start();
  c.reset();
  REQUIRE(c.stage() == FgLearningStage::Idle);
  REQUIRE(c.cycle() == 0);
}

TEST_CASE("load restores persisted state", "[fg]") {
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 2, 1);
  REQUIRE(c.stage() == FgLearningStage::Discharge);
  REQUIRE(c.cycle() == 2);
  REQUIRE(c.itpor_losses() == 1);
}

// ============================================================================
// Idle tick is inactive
// ============================================================================

TEST_CASE("Idle tick reports inactive", "[fg]") {
  FgLearningController c;
  FgLearningAction a = c.tick(quiet_snapshot(), 0);
  REQUIRE_FALSE(a.active);
  REQUIRE_FALSE(a.persist_stage);
}

// ============================================================================
// Charge stage
// ============================================================================

TEST_CASE("Charge emits charge-enable at 1500 mA", "[fg]") {
  FgLearningController c;
  c.start();
  FgLearningAction a = c.tick(quiet_snapshot(), 0);
  REQUIRE(a.active);
  REQUIRE(a.set_charge_enabled);
  REQUIRE(a.charge_current_ma == FgLearningController::CHARGE_CURRENT_MA);
  REQUIRE(a.screen == Screen::FgLearnCharging);
  REQUIRE(a.manual_cue == ManualCue::None);
}

TEST_CASE("Charge advances to Rest on FC flag", "[fg]") {
  FgLearningController c;
  c.start();
  PowerSnapshot s = quiet_snapshot();
  s.fg_learning_flags |= FG_LEARN_FC;
  FgLearningAction a = c.tick(s, 0);
  REQUIRE(c.stage() == FgLearningStage::Rest);
  REQUIRE(a.persist_stage);
  REQUIRE(a.screen == Screen::FgLearnResting);
}

TEST_CASE("Charge advances on BMS termination only after charging observed", "[fg]") {
  FgLearningController c;
  c.start();

  // Termination seen with no prior charging must NOT advance.
  PowerSnapshot term = quiet_snapshot();
  term.charging_status = BmsChargingState::ChargeTerminationDone;
  c.tick(term, 0);
  REQUIRE(c.stage() == FgLearningStage::Charge);

  // Observe active charging, then termination advances.
  PowerSnapshot charging = quiet_snapshot();
  charging.charging_status = BmsChargingState::FastCharge;
  c.tick(charging, 100);
  REQUIRE(c.stage() == FgLearningStage::Charge);

  c.tick(term, 200);
  REQUIRE(c.stage() == FgLearningStage::Rest);
}

TEST_CASE("Charge times out to Failed", "[fg]") {
  FgLearningController c;
  c.start();
  c.tick(quiet_snapshot(), 0); // stamp entry time
  FgLearningAction a = c.tick(quiet_snapshot(), FgLearningController::CHARGE_TIMEOUT_MS);
  REQUIRE(c.stage() == FgLearningStage::Failed);
  REQUIRE(a.manual_cue == ManualCue::Failed);
  REQUIRE_FALSE(a.active);
}

// ============================================================================
// Rest stage
// ============================================================================

TEST_CASE("Rest needs both OCV taken and the rest timeout", "[fg]") {
  FgLearningController c;
  c.load(FgLearningStage::Rest, 1, 0);
  c.tick(quiet_snapshot(), 0); // stamp entry

  // Timeout elapsed but OCV not taken: stays.
  c.tick(quiet_snapshot(), FgLearningController::REST_TIMEOUT_MS);
  REQUIRE(c.stage() == FgLearningStage::Rest);

  // OCV taken but timeout not elapsed: stays.
  PowerSnapshot ocv = quiet_snapshot();
  ocv.fg_learning_flags |= FG_LEARN_OCV_TAKEN;
  c.tick(ocv, FgLearningController::REST_TIMEOUT_MS - 1);
  REQUIRE(c.stage() == FgLearningStage::Rest);

  // Both satisfied: advances to Discharge.
  c.tick(ocv, FgLearningController::REST_TIMEOUT_MS);
  REQUIRE(c.stage() == FgLearningStage::Discharge);
}

TEST_CASE("Rest action is low-power with no charge", "[fg]") {
  FgLearningController c;
  c.load(FgLearningStage::Rest, 1, 0);
  FgLearningAction a = c.tick(quiet_snapshot(), 0);
  REQUIRE(a.active);
  REQUIRE(a.low_power);
  REQUIRE_FALSE(a.set_charge_enabled);
  REQUIRE(a.screen == Screen::FgLearnResting);
}

// ============================================================================
// Discharge stage
// ============================================================================

TEST_CASE("Discharge raises the Unplug cue and full load", "[fg]") {
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 1, 0);
  FgLearningAction a = c.tick(quiet_snapshot(), 0);
  REQUIRE(a.active);
  REQUIRE_FALSE(a.low_power);
  REQUIRE(a.manual_cue == ManualCue::Unplug);
  REQUIRE(a.screen == Screen::FgLearnUnplug);
}

TEST_CASE("Discharge advances to CycleDone when the discharge target is reached", "[fg]") {
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 1, 0);
  PowerSnapshot s = quiet_snapshot();
  s.discharge_target_reached = true;
  FgLearningAction a = c.tick(s, 0);
  REQUIRE(c.stage() == FgLearningStage::CycleDone);
  REQUIRE(a.persist_stage);
  REQUIRE(a.screen == Screen::DischargeComplete);
}

TEST_CASE("Discharge holds while only the v1 EDV mirror is set", "[fg]") {
  // The FSM reads discharge_target_reached, which PowerService derives per
  // variant; edv_cutoff_reached alone is the v1 ship-mode trigger and must not
  // move the stage on its own.
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 1, 0);
  PowerSnapshot s = quiet_snapshot();
  s.edv_cutoff_reached = true;
  c.tick(s, 0);
  REQUIRE(c.stage() == FgLearningStage::Discharge);
}

// ============================================================================
// Verify stage
// ============================================================================

TEST_CASE("Verify requests a verify read", "[fg]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, 2, 0);
  FgLearningAction a = c.tick(quiet_snapshot(), 0);
  REQUIRE(a.active);
  REQUIRE(a.run_verify);
  REQUIRE(a.screen == Screen::FgLearnVerifying);
}

// ============================================================================
// Boot resume matrix
// ============================================================================

TEST_CASE("resume: Idle stays inactive", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Idle, 0, 0);
  REQUIRE_FALSE(c.resume_on_boot(quiet_snapshot()));
  REQUIRE(c.stage() == FgLearningStage::Idle);
}

TEST_CASE("resume: Complete without POR stays inactive", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Complete, 2, 0);
  REQUIRE_FALSE(c.resume_on_boot(quiet_snapshot()));
  REQUIRE(c.stage() == FgLearningStage::Complete);
}

TEST_CASE("resume: Complete with POR fails (learning lost)", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Complete, 2, 0);
  PowerSnapshot s = quiet_snapshot();
  s.fg_learning_flags |= FG_LEARN_ITPOR;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Failed);
}

TEST_CASE("resume: CycleDone no POR below cap advances cycle and charges", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::CycleDone, 1, 0);
  REQUIRE(c.resume_on_boot(quiet_snapshot()));
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.cycle() == 2);
}

TEST_CASE("resume: CycleDone no POR at cap goes to Verify", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::CycleDone, FgLearningController::CYCLE_TARGET, 0);
  REQUIRE(c.resume_on_boot(quiet_snapshot()));
  REQUIRE(c.stage() == FgLearningStage::Verify);
}

TEST_CASE("resume: mid-run no POR on charger restarts Charge", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Rest, 1, 0);
  PowerSnapshot s = quiet_snapshot();
  s.external_input_present = true;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.cycle() == 1);
}

TEST_CASE("resume: mid-run no POR on battery resumes Discharge", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Rest, 1, 0);
  PowerSnapshot s = quiet_snapshot();
  s.external_input_present = false;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Discharge);
}

TEST_CASE("resume: mid-cycle-1 reboot without POR does not count a loss", "[fg][resume]") {
  // ITPOR==0, QMAX_UP==0 (Qmax not yet learned) must resume, not lose.
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 1, 0);
  PowerSnapshot s = quiet_snapshot();
  // QMAX_UP not set (Qmax not yet learned), no ITPOR.
  s.external_input_present = false;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.itpor_losses() == 0);
  REQUIRE(c.stage() == FgLearningStage::Discharge);
}

TEST_CASE("resume: POR loss is cap-guarded then fails", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 1, 0);
  PowerSnapshot s = quiet_snapshot();
  s.fg_learning_flags |= FG_LEARN_ITPOR;

  // First two POR losses restart at Charge.
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.itpor_losses() == 1);

  c.load(FgLearningStage::Discharge, 1, 1);
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.itpor_losses() == 2);

  // Third hits the cap -> Failed.
  c.load(FgLearningStage::Discharge, 1, 2);
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Failed);
  REQUIRE(c.itpor_losses() == 3);
}

TEST_CASE("resume: Verify re-runs verify", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, 2, 0);
  REQUIRE(c.resume_on_boot(quiet_snapshot()));
  REQUIRE(c.stage() == FgLearningStage::Verify);
}

TEST_CASE("resume: Failed stays Failed", "[fg][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Failed, 2, 0);
  REQUIRE(c.resume_on_boot(quiet_snapshot()));
  REQUIRE(c.stage() == FgLearningStage::Failed);
}

// ============================================================================
// Verify criteria
// ============================================================================

TEST_CASE("verify_pass accepts a healthy readout", "[fg][verify]") {
  REQUIRE(FgLearningController::verify_pass(passing_verify()));
}

TEST_CASE("verify_pass rejects failed reads", "[fg][verify]") {
  VerifyInputs in = passing_verify();
  in.reads_ok = false;
  REQUIRE_FALSE(FgLearningController::verify_pass(in));
}

TEST_CASE("verify_pass rejects POR", "[fg][verify]") {
  VerifyInputs in = passing_verify();
  in.itpor = true;
  REQUIRE_FALSE(FgLearningController::verify_pass(in));
}

TEST_CASE("verify_pass rejects qmax not updated", "[fg][verify]") {
  VerifyInputs in = passing_verify();
  in.qmax_up = false;
  REQUIRE_FALSE(FgLearningController::verify_pass(in));
}

TEST_CASE("verify_pass rejects Qmax out of band", "[fg][verify]") {
  VerifyInputs low = passing_verify();
  low.qmax_mah = static_cast<uint16_t>(DESIGN_CAP * 0.5f); // below 0.7x
  REQUIRE_FALSE(FgLearningController::verify_pass(low));

  VerifyInputs high = passing_verify();
  high.qmax_mah = static_cast<uint16_t>(DESIGN_CAP * 1.5f); // above 1.4x
  REQUIRE_FALSE(FgLearningController::verify_pass(high));
}

TEST_CASE("verify_pass rejects a non-positive Ra value", "[fg][verify]") {
  VerifyInputs in = passing_verify();
  in.ra[7] = 0;
  REQUIRE_FALSE(FgLearningController::verify_pass(in));

  in = passing_verify();
  in.ra[0] = -1;
  REQUIRE_FALSE(FgLearningController::verify_pass(in));
}

// ============================================================================
// on_verify_result transitions
// ============================================================================

TEST_CASE("on_verify_result pass -> Complete", "[fg][verify]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, FgLearningController::CYCLE_TARGET, 0);
  REQUIRE(c.on_verify_result(passing_verify()));
  REQUIRE(c.stage() == FgLearningStage::Complete);
}

TEST_CASE("on_verify_result fail at cap -> Failed", "[fg][verify]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, FgLearningController::CYCLE_TARGET, 0);
  VerifyInputs bad = passing_verify();
  bad.qmax_up = false;
  REQUIRE_FALSE(c.on_verify_result(bad));
  REQUIRE(c.stage() == FgLearningStage::Failed);
}

TEST_CASE("on_verify_result fail below cap -> another Charge cycle (guard)", "[fg][verify]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, 1, 0);
  VerifyInputs bad = passing_verify();
  bad.qmax_up = false;
  REQUIRE_FALSE(c.on_verify_result(bad));
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.cycle() == 2);
}

// ============================================================================
// Fail-reason attribution
// ============================================================================

TEST_CASE("verify_fail_reason maps each criterion", "[fg][verify][reason]") {
  REQUIRE(FgLearningController::verify_fail_reason(passing_verify()) == FgLearnFailReason::None);

  VerifyInputs reads = passing_verify();
  reads.reads_ok = false;
  REQUIRE(FgLearningController::verify_fail_reason(reads) == FgLearnFailReason::VerifyReadFail);

  VerifyInputs por = passing_verify();
  por.itpor = true;
  REQUIRE(FgLearningController::verify_fail_reason(por) == FgLearnFailReason::VerifyItpor);

  VerifyInputs qmax = passing_verify();
  qmax.qmax_up = false;
  REQUIRE(FgLearningController::verify_fail_reason(qmax) ==
          FgLearnFailReason::VerifyQmaxNotUpdated);

  VerifyInputs dc = passing_verify();
  dc.design_capacity_mah = 0;
  REQUIRE(FgLearningController::verify_fail_reason(dc) == FgLearnFailReason::VerifyDesignCapZero);

  VerifyInputs band = passing_verify();
  band.qmax_mah = static_cast<uint16_t>(DESIGN_CAP * 0.5f);
  REQUIRE(FgLearningController::verify_fail_reason(band) == FgLearnFailReason::VerifyQmaxOutOfBand);

  VerifyInputs ra = passing_verify();
  ra.ra[3] = 0;
  REQUIRE(FgLearningController::verify_fail_reason(ra) == FgLearnFailReason::VerifyRaInvalid);
}

TEST_CASE("fail_reason records the verify criterion at cap", "[fg][verify][reason]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, FgLearningController::CYCLE_TARGET, 0);
  VerifyInputs bad = passing_verify();
  bad.qmax_up = false;
  REQUIRE_FALSE(c.on_verify_result(bad));
  REQUIRE(c.stage() == FgLearningStage::Failed);
  REQUIRE(c.fail_reason() == FgLearnFailReason::VerifyQmaxNotUpdated);
}

TEST_CASE("fail_reason records charge timeout", "[fg][reason]") {
  FgLearningController c;
  c.start();
  c.tick(quiet_snapshot(), 0); // stamp entry
  FgLearningAction a = c.tick(quiet_snapshot(), FgLearningController::CHARGE_TIMEOUT_MS);
  REQUIRE(c.stage() == FgLearningStage::Failed);
  REQUIRE(c.fail_reason() == FgLearnFailReason::ChargeTimeout);
  (void)a;
}

TEST_CASE("fail_reason records POR-loss cap", "[fg][resume][reason]") {
  FgLearningController c;
  // Two prior losses persisted; a third POR on resume hits the cap.
  c.load(FgLearningStage::Discharge, 1, FgLearningController::ITPOR_LOSS_CAP - 1);
  PowerSnapshot s = quiet_snapshot();
  s.fg_learning_flags |= FG_LEARN_ITPOR;
  c.resume_on_boot(s);
  REQUIRE(c.stage() == FgLearningStage::Failed);
  REQUIRE(c.fail_reason() == FgLearnFailReason::PorLossCap);
}

TEST_CASE("fail_reason restore round-trips", "[fg][reason]") {
  FgLearningController c;
  c.load(FgLearningStage::Failed, 2, 0);
  REQUIRE(c.fail_reason() == FgLearnFailReason::None); // load clears
  c.restore_fail_reason(FgLearnFailReason::VerifyRaInvalid);
  REQUIRE(c.fail_reason() == FgLearnFailReason::VerifyRaInvalid);
}

// ============================================================================
// Manual-cue mapping coverage
// ============================================================================

TEST_CASE("manual cue is None in automatic stages", "[fg][cue]") {
  FgLearningController c;

  c.load(FgLearningStage::Charge, 1, 0);
  REQUIRE(c.tick(quiet_snapshot(), 0).manual_cue == ManualCue::None);

  c.load(FgLearningStage::Rest, 1, 0);
  REQUIRE(c.tick(quiet_snapshot(), 0).manual_cue == ManualCue::None);

  c.load(FgLearningStage::CycleDone, 1, 0);
  REQUIRE(c.tick(quiet_snapshot(), 0).manual_cue == ManualCue::None);

  c.load(FgLearningStage::Verify, 2, 0);
  REQUIRE(c.tick(quiet_snapshot(), 0).manual_cue == ManualCue::None);
}

TEST_CASE("manual cue maps Discharge/Failed/Complete", "[fg][cue]") {
  FgLearningController c;

  c.load(FgLearningStage::Discharge, 1, 0);
  REQUIRE(c.tick(quiet_snapshot(), 0).manual_cue == ManualCue::Unplug);

  c.load(FgLearningStage::Failed, 2, 0);
  REQUIRE(c.tick(quiet_snapshot(), 0).manual_cue == ManualCue::Failed);

  c.load(FgLearningStage::Complete, 2, 0);
  REQUIRE(c.tick(quiet_snapshot(), 0).manual_cue == ManualCue::Complete);
}
