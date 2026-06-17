/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "fg_learning_controller.h"

#include <catch2/catch_test_macros.hpp>

namespace {

VerifyInputs good_verify() {
  VerifyInputs in;
  in.reads_ok = true;
  in.itpor = false;
  in.qmax_up = true;
  in.design_capacity_mah = 2000;
  in.qmax_mah = 1950;
  for (size_t i = 0; i < FG_LEARNING_RA_TABLE_SIZE; ++i) {
    in.ra[i] = static_cast<int16_t>(50 + i);
  }
  return in;
}

} // namespace

// ============================================================================
// Per-stage action mapping
// ============================================================================

TEST_CASE("Charge action: charge on at 1500 mA, full load, charging screen", "[fglearn][action]") {
  FgLearningController c;
  c.start();
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.cycle() == 1);

  PowerSnapshot s;
  FgLearningAction a = c.tick(s, 0);
  REQUIRE(a.active);
  REQUIRE(a.set_charge_enabled);
  REQUIRE(a.charge_current_ma == FgLearningController::CHARGE_CURRENT_MA);
  REQUIRE_FALSE(a.low_power);
  REQUIRE(a.manual_cue == ManualCue::None);
  REQUIRE(a.screen == Screen::FgLearnCharging);
}

TEST_CASE("Rest action: charge off, low power, resting screen", "[fglearn][action]") {
  FgLearningController c;
  c.load(FgLearningStage::Rest, 1, 0);
  PowerSnapshot s;
  FgLearningAction a = c.tick(s, 0);
  REQUIRE(a.active);
  REQUIRE_FALSE(a.set_charge_enabled);
  REQUIRE(a.low_power);
  REQUIRE(a.manual_cue == ManualCue::None);
  REQUIRE(a.screen == Screen::FgLearnResting);
}

TEST_CASE("Discharge action: full load, unplug cue", "[fglearn][action]") {
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 1, 0);
  PowerSnapshot s;
  FgLearningAction a = c.tick(s, 0);
  REQUIRE(a.active);
  REQUIRE_FALSE(a.set_charge_enabled);
  REQUIRE_FALSE(a.low_power);
  REQUIRE(a.manual_cue == ManualCue::Unplug);
  REQUIRE(a.screen == Screen::FgLearnUnplug);
}

TEST_CASE("Verify action: low power, run_verify set", "[fglearn][action]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, 2, 0);
  PowerSnapshot s;
  FgLearningAction a = c.tick(s, 0);
  REQUIRE(a.active);
  REQUIRE(a.low_power);
  REQUIRE(a.run_verify);
  REQUIRE(a.screen == Screen::FgLearnVerifying);
}

TEST_CASE("Complete/Failed/Idle actions are inactive with the right cue", "[fglearn][action]") {
  FgLearningController c;
  PowerSnapshot s;

  c.load(FgLearningStage::Complete, 2, 0);
  FgLearningAction a = c.tick(s, 0);
  REQUIRE_FALSE(a.active);
  REQUIRE(a.manual_cue == ManualCue::Complete);
  REQUIRE(a.screen == Screen::FgLearnComplete);

  c.load(FgLearningStage::Failed, 2, 3);
  a = c.tick(s, 0);
  REQUIRE_FALSE(a.active);
  REQUIRE(a.manual_cue == ManualCue::Failed);
  REQUIRE(a.screen == Screen::FgLearnFailed);

  c.reset();
  a = c.tick(s, 0);
  REQUIRE_FALSE(a.active);
  REQUIRE(a.manual_cue == ManualCue::None);
}

// ============================================================================
// FSM transitions
// ============================================================================

TEST_CASE("Charge -> Rest on FC flag", "[fglearn][fsm]") {
  FgLearningController c;
  c.start();
  PowerSnapshot s;
  c.tick(s, 0); // arm timing

  s.fg_flag_fc = true;
  FgLearningAction a = c.tick(s, 1000);
  REQUIRE(c.stage() == FgLearningStage::Rest);
  REQUIRE(a.persist_stage);
  REQUIRE(a.screen == Screen::FgLearnResting);
}

TEST_CASE("Charge -> Rest on BMS termination only after charging observed", "[fglearn][fsm]") {
  FgLearningController c;
  c.start();
  PowerSnapshot s;

  // Termination seen but charging never observed: must NOT advance.
  s.fg_flag_fc = false;
  s.charging_status = BmsChargingState::ChargeTerminationDone;
  c.tick(s, 0);
  c.tick(s, 1000);
  REQUIRE(c.stage() == FgLearningStage::Charge);

  // Now observe charging, then termination: advance.
  s.charging_status = BmsChargingState::FastCharge;
  c.tick(s, 2000);
  s.charging_status = BmsChargingState::ChargeTerminationDone;
  c.tick(s, 3000);
  REQUIRE(c.stage() == FgLearningStage::Rest);
}

TEST_CASE("Charge -> Failed on charge timeout", "[fglearn][fsm]") {
  FgLearningController c;
  c.start();
  PowerSnapshot s;
  c.tick(s, 0); // arm at t=0
  c.tick(s, FgLearningController::CHARGE_TIMEOUT_MS);
  REQUIRE(c.stage() == FgLearningStage::Failed);
}

TEST_CASE("Rest requires both ocv_taken and the 500 s gate", "[fglearn][fsm]") {
  PowerSnapshot s;

  SECTION("ocv taken but time too short -> stays Rest") {
    FgLearningController c;
    c.load(FgLearningStage::Rest, 1, 0);
    c.tick(s, 0); // arm at 0
    s.fg_ocv_taken = true;
    c.tick(s, 1000);
    REQUIRE(c.stage() == FgLearningStage::Rest);
  }

  SECTION("time elapsed but ocv not taken -> stays Rest") {
    FgLearningController c;
    c.load(FgLearningStage::Rest, 1, 0);
    c.tick(s, 0);
    s.fg_ocv_taken = false;
    c.tick(s, FgLearningController::REST_TIMEOUT_MS + 1);
    REQUIRE(c.stage() == FgLearningStage::Rest);
  }

  SECTION("both satisfied -> Discharge") {
    FgLearningController c;
    c.load(FgLearningStage::Rest, 1, 0);
    c.tick(s, 0);
    s.fg_ocv_taken = true;
    c.tick(s, FgLearningController::REST_TIMEOUT_MS + 1);
    REQUIRE(c.stage() == FgLearningStage::Discharge);
  }
}

TEST_CASE("Discharge -> CycleDone on edv_cutoff_reached", "[fglearn][fsm]") {
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 1, 0);
  PowerSnapshot s;
  c.tick(s, 0);
  s.edv_cutoff_reached = true;
  FgLearningAction a = c.tick(s, 100);
  REQUIRE(c.stage() == FgLearningStage::CycleDone);
  REQUIRE(a.persist_stage);
  REQUIRE(a.screen == Screen::DischargeComplete);
}

// ============================================================================
// Boot-resume matrix
// ============================================================================

TEST_CASE("resume: Idle stays Idle, no change", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Idle, 0, 0);
  PowerSnapshot s;
  REQUIRE_FALSE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Idle);
}

TEST_CASE("resume: Complete no POR stays Complete", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Complete, 2, 0);
  PowerSnapshot s;
  REQUIRE_FALSE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Complete);
}

TEST_CASE("resume: Complete with POR -> Failed", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Complete, 2, 0);
  PowerSnapshot s;
  s.fg_itpor = true;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Failed);
}

TEST_CASE("resume: CycleDone no POR, cycle < cap -> next Charge cycle", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::CycleDone, 1, 0);
  PowerSnapshot s;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.cycle() == 2);
}

TEST_CASE("resume: CycleDone no POR, cycle >= cap -> Verify", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::CycleDone, FgLearningController::CYCLE_TARGET, 0);
  PowerSnapshot s;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Verify);
}

TEST_CASE("resume: CycleDone with POR -> cap-guarded restart", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::CycleDone, 2, 0);
  PowerSnapshot s;
  s.fg_itpor = true;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.itpor_losses() == 1);
}

TEST_CASE("resume: mid-run no POR on charger -> restart Charge", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Rest, 1, 0);
  PowerSnapshot s;
  s.external_input_present = true;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Charge);
}

TEST_CASE("resume: mid-run no POR on battery -> resume Discharge", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Rest, 1, 0);
  PowerSnapshot s;
  s.external_input_present = false;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Discharge);
}

TEST_CASE("resume: mid-run with POR -> cap-guarded restart", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 1, 0);
  PowerSnapshot s;
  s.fg_itpor = true;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.itpor_losses() == 1);
}

TEST_CASE("resume: POR-loss reaching the cap -> Failed", "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Discharge, 1, FgLearningController::ITPOR_LOSS_CAP - 1);
  PowerSnapshot s;
  s.fg_itpor = true;
  REQUIRE(c.resume_on_boot(s));
  REQUIRE(c.itpor_losses() == FgLearningController::ITPOR_LOSS_CAP);
  REQUIRE(c.stage() == FgLearningStage::Failed);
}

TEST_CASE("resume: mid-cycle-1 reboot (itpor=0, qmax_up=0) resumes, no loss counted",
          "[fglearn][resume]") {
  FgLearningController c;
  c.load(FgLearningStage::Charge, 1, 0);
  PowerSnapshot s;
  s.fg_itpor = false;
  s.fg_qmax_up = false; // Qmax legitimately not learned yet in cycle 1
  s.external_input_present = true;
  c.resume_on_boot(s);
  REQUIRE(c.itpor_losses() == 0);
  REQUIRE(c.stage() == FgLearningStage::Charge);
}

TEST_CASE("resume: Verify re-runs verify; Failed stays Failed", "[fglearn][resume]") {
  PowerSnapshot s;

  FgLearningController v;
  v.load(FgLearningStage::Verify, 2, 0);
  REQUIRE_FALSE(v.resume_on_boot(s));
  REQUIRE(v.stage() == FgLearningStage::Verify);

  FgLearningController f;
  f.load(FgLearningStage::Failed, 2, 3);
  REQUIRE_FALSE(f.resume_on_boot(s));
  REQUIRE(f.stage() == FgLearningStage::Failed);
}

// ============================================================================
// Verify criteria
// ============================================================================

TEST_CASE("verify_pass accepts a healthy read-back", "[fglearn][verify]") {
  REQUIRE(FgLearningController::verify_pass(good_verify()));
}

TEST_CASE("verify_pass rejects each failure mode", "[fglearn][verify]") {
  SECTION("reads not ok") {
    VerifyInputs in = good_verify();
    in.reads_ok = false;
    REQUIRE_FALSE(FgLearningController::verify_pass(in));
  }
  SECTION("itpor set") {
    VerifyInputs in = good_verify();
    in.itpor = true;
    REQUIRE_FALSE(FgLearningController::verify_pass(in));
  }
  SECTION("qmax_up clear") {
    VerifyInputs in = good_verify();
    in.qmax_up = false;
    REQUIRE_FALSE(FgLearningController::verify_pass(in));
  }
  SECTION("design capacity zero") {
    VerifyInputs in = good_verify();
    in.design_capacity_mah = 0;
    REQUIRE_FALSE(FgLearningController::verify_pass(in));
  }
  SECTION("Qmax below band") {
    VerifyInputs in = good_verify();
    in.qmax_mah = 1000; // < 0.7 * 2000
    REQUIRE_FALSE(FgLearningController::verify_pass(in));
  }
  SECTION("Qmax above band") {
    VerifyInputs in = good_verify();
    in.qmax_mah = 3000; // > 1.4 * 2000
    REQUIRE_FALSE(FgLearningController::verify_pass(in));
  }
  SECTION("Ra non-positive entry") {
    VerifyInputs in = good_verify();
    in.ra[5] = 0;
    REQUIRE_FALSE(FgLearningController::verify_pass(in));
  }
  SECTION("Ra grid flat (still at ROM default)") {
    VerifyInputs in = good_verify();
    for (size_t i = 0; i < FG_LEARNING_RA_TABLE_SIZE; ++i) {
      in.ra[i] = 50;
    }
    REQUIRE_FALSE(FgLearningController::verify_pass(in));
  }
}

// ============================================================================
// on_verify_result transitions
// ============================================================================

TEST_CASE("on_verify_result: pass -> Complete", "[fglearn][verify]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, FgLearningController::CYCLE_TARGET, 0);
  REQUIRE(c.on_verify_result(good_verify()));
  REQUIRE(c.stage() == FgLearningStage::Complete);
}

TEST_CASE("on_verify_result: fail at cycle cap -> Failed", "[fglearn][verify]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, FgLearningController::CYCLE_TARGET, 0);
  VerifyInputs in = good_verify();
  in.qmax_up = false;
  REQUIRE_FALSE(c.on_verify_result(in));
  REQUIRE(c.stage() == FgLearningStage::Failed);
}

TEST_CASE("on_verify_result: fail below cap -> another Charge cycle (guard branch)",
          "[fglearn][verify]") {
  FgLearningController c;
  c.load(FgLearningStage::Verify, 1, 0); // below CYCLE_TARGET (not reachable normally)
  VerifyInputs in = good_verify();
  in.qmax_up = false;
  REQUIRE_FALSE(c.on_verify_result(in));
  REQUIRE(c.stage() == FgLearningStage::Charge);
  REQUIRE(c.cycle() == 2);
}
