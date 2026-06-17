/**
 * AirGradient Go — Fuel-Gauge Learning Controller (pure FSM)
 *
 * Owns the automated BQ27427 learning state machine.  Never touches
 * hardware: it reads a PowerSnapshot and emits an FgLearningAction for the
 * orchestrator to apply.  This keeps the decision logic host-testable
 * (the BQ27427 read path cannot host-compile).
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "go_display.h"  // Screen
#include "go_power.h"    // PowerSnapshot
#include "go_settings.h" // FgLearningStage

#include <cstddef>
#include <cstdint>

/// Ra-grid size for the FSM/verify layer.  Declared here (NOT pulled from
/// bq27427.h) so the controller stays free of the driver header and remains
/// host-testable.  A static_assert at the PowerService boundary (target build
/// only) checks this equals BQ27427::RA_TABLE_SIZE so the two cannot drift.
static constexpr size_t FG_LEARNING_RA_TABLE_SIZE = 15;

// Verify read-back (FgLearningVerifyReadout, go_power.h) must carry the same
// Ra grid size as the FSM's VerifyInputs.  Both are host-safe constants.
static_assert(FG_LEARNING_RA_TABLE_SIZE == static_cast<size_t>(FG_RA_TABLE_SIZE),
              "Ra table size mismatch between FG learning layer and PowerService readout");

/// Static-LED indicator for stages that require operator action.  The
/// orchestrator maps each to one solid back-LED colour.  Kept on the action
/// so the FSM stays pure and the mapping is host-testable.
enum class ManualCue : uint8_t { None, Unplug, Failed, Complete };

/// Desired hardware intent for one tick.  The orchestrator applies it; the
/// FSM never acts directly.
struct FgLearningAction {
  bool set_charge_enabled = false;        ///< Desired BMS charge-enable state
  uint16_t charge_current_ma = 0;         ///< ICHG to program while charging
  bool low_power = false;                 ///< Quiet load (Rest/Verify) vs full load
  ManualCue manual_cue = ManualCue::None; ///< Solid LED for operator-action stages
  Screen screen = Screen::Home;           ///< Phase screen to paint
  bool persist_stage = false;             ///< Write factory settings this tick
  bool run_verify = false;                ///< Orchestrator reads FG + calls on_verify_result()
  bool active = false;                    ///< false in {Idle, Complete, Failed}: leave normal op
};
// Note: there is NO commit_then_ship field.  The EDV persist-then-ship
// ordering is owned solely by the orchestrator's EDV hook; the FSM only
// transitions Discharge -> CycleDone on edv_cutoff_reached.

/// Learned-value read-back, built by the orchestrator from the driver and
/// passed to verify_pass() / on_verify_result().
struct VerifyInputs {
  bool reads_ok = false;
  bool itpor = false;   ///< A POR wiped learning
  bool qmax_up = false; ///< CONTROL_STATUS QMAX_UP
  uint16_t qmax_mah = 0;
  uint16_t design_capacity_mah = 0;
  int16_t ra[FG_LEARNING_RA_TABLE_SIZE] = {};
};

class FgLearningController {
public:
  static constexpr uint8_t CYCLE_TARGET = 2;          ///< Fixed cycles before verify
  static constexpr uint8_t ITPOR_LOSS_CAP = 3;        ///< POR-loss restarts -> Failed
  static constexpr uint32_t REST_TIMEOUT_MS = 500000; ///< Min rest before discharge
  static constexpr uint32_t CHARGE_TIMEOUT_MS = 8u * 60u * 60u * 1000u;
  static constexpr uint16_t CHARGE_CURRENT_MA = 1500;

  // Verify Qmax acceptance band as a fraction of design capacity, in tenths
  // (avoids float in the pure layer): [0.7 x DC, 1.4 x DC].
  static constexpr uint16_t QMAX_MIN_TENTHS = 7;
  static constexpr uint16_t QMAX_MAX_TENTHS = 14;

  /// Restore persisted run state (called after load_factory_settings()).
  void load(FgLearningStage stage, uint8_t cycle, uint8_t itpor_losses);
  /// Arm a fresh run (stage=Charge, cycle=1).
  void start();
  /// Clear to Idle.
  void reset();

  /// Re-enter the FSM from persisted state at boot, per the resume matrix.
  /// Returns true when the resume changed the run state (orchestrator
  /// persists on change).
  bool resume_on_boot(const PowerSnapshot &snap);

  /// Advance one step.  Derives the next stage from the snapshot and returns
  /// the action to apply.
  FgLearningAction tick(const PowerSnapshot &snap, uint32_t now_ms);

  /// Apply a verify read-back: pass -> Complete; fail -> Failed (at cap) or
  /// another Charge cycle (below cap).  Returns true on pass.
  bool on_verify_result(const VerifyInputs &in);

  /// Pure verify criteria (host-tested).
  static bool verify_pass(const VerifyInputs &in);

  FgLearningStage stage() const { return _stage; }
  uint8_t cycle() const { return _cycle; }
  uint8_t itpor_losses() const { return _itpor_losses; }

private:
  FgLearningStage _stage = FgLearningStage::Idle;
  uint8_t _cycle = 0;
  uint8_t _itpor_losses = 0;

  // Timing is armed lazily on the first tick after a stage entry (start/load
  // carry no now_ms), and re-armed whenever the stage changes inside tick().
  uint32_t _stage_entered_ms = 0;
  bool _timing_armed = false;

  // Charge-stage latch: a BMS charge-termination only advances to Rest once
  // charging was actually observed — guards the chemistry/Taper-Voltage case
  // where FC never latches and "Done" could otherwise be read spuriously.
  bool _charging_observed = false;

  void _enter(FgLearningStage stage, uint32_t now_ms);
  void _por_loss_restart(); // cap-guarded; escalates to Failed at the cap
};
