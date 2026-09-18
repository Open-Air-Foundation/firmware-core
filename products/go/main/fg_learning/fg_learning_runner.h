/**
 * AirGradient Go — Fuel-Gauge Learning Runner (hardware-owning)
 *
 * Dedicated factory boot path that drives the whole multi-hour, multi-cycle
 * learning sequence in isolation: it owns the pure FgLearningController, brings
 * up only the hardware the run needs (display, LED, buzzer), and never
 * constructs the Orchestrator or any normal producer. Target-only — never
 * host-compiled.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstdint>

#include "fg_learning/fg_learning_controller.h"
#include "fg_learning/fg_learning_journal.h"
#include "go_power.h"

class GoBoard;
class DisplayService;
class LedService;
class BuzzerService;
class ConfigStore;
class StorageService;

class FgLearningRunner {
public:
  struct Deps {
    PowerService &power;       ///< poll, charge/load ctl, verify read, shutdown, ext WDT
    DisplayService &display;   ///< dashboard
    LedService &led;           ///< manual-cue LED
    BuzzerService &buzzer;     ///< unplug melody
    ConfigStore &config_store; ///< FactorySettings load / save / clear
    GoBoard &board;            ///< gpio_hal (abort button); reboot is a free fn
    StorageService &storage;   ///< NAND mount for the persistent learning journal
  };

  explicit FgLearningRunner(const Deps &deps);

  /// Bring-up + resume + run loop. Never returns.
  [[noreturn]] void run();

private:
  /// LED phase derived from (stage, plug state); drives edge-triggered back LED.
  enum class LedPhase : uint8_t { Off, Rest, UnplugPrompt, Discharging, Complete, Failed };

  void resume_on_boot(); ///< load -> controller, prereqs, one poll
  void apply_action(const FgLearningAction &a,
                    bool ext_input);                   ///< charge / load / cue / screen / persist
  void run_verify();                                   ///< read FG, on_verify_result, persist
  bool handle_edv_ship(const PowerSnapshot &snap);     ///< v1 EDV pre-emption of the Discharge stage
  void ship_after_cycle(const PowerSnapshot &snap);    ///< persist CycleDone -> ship (single owner)
  void feed_ext_watchdog(uint32_t now);                ///< pulse external HW WDT (< 60 s window)
  void refresh_dashboard(const PowerSnapshot &snap);   ///< build DisplayValues, full refresh
  bool poll_abort_button();                            ///< power short press -> clear + reboot
  void idle_poll(uint32_t total_ms, bool watch_abort); ///< abort sampling + Discharge CPU duty
  [[noreturn]] void handback_terminal();               ///< cleanup + result paint/LED, hold

  void set_discharge_load(bool on);                   ///< PM rail + CPU duty flag
  void journal_flag_edges(const PowerSnapshot &snap); ///< journal learning-flag milestones once
  DisplayValues build_dashboard_values(const PowerSnapshot &snap) const;
  void terminal_cleanup(); ///< idempotent restore for both terminal stages

  FgLearningController _controller; ///< pure FSM, owned
  FgLearningJournal _journal;       ///< persistent diagnostics (NAND + serial replay)
  Deps _deps;

  // Learning-flag edge latches — each milestone is journalled once per boot.
  bool _seen_itpor = false;
  bool _seen_qmax_up = false;
  bool _seen_res_up = false;
  bool _seen_dsg_qualified = false;

  uint32_t _last_paint_ms = 0;
  uint32_t _last_wdt_ms = 0;
  uint32_t _stage_entered_ms = 0;  ///< wall-clock when the current stage was entered (this boot)
  uint32_t _power_btn_down_ms = 0; ///< 0 = not pressed; else press-start time
  bool _abort_btn_ready = false;   ///< power button GPIO configured
  bool _discharge_load_on = false;
  bool _pm_fan_running = false;             ///< PM fan confirmed measuring (valid read)
  Screen _screen = Screen::FgLearnCharging; ///< latest phase screen for the dashboard
  FgLearningStage _prev_stage = FgLearningStage::Idle;
  bool _prev_stage_valid = false;

  // LED phase edge gate; _led_applied forces the first write after boot.
  LedPhase _last_led_phase = LedPhase::Off;
  bool _led_applied = false;

  // Repaint promptly on a charging-state / plug change (not just the heartbeat).
  BmsChargingState _prev_charging_status = BmsChargingState::Unknown;
  bool _prev_external_input_present = false;
  bool _prev_inputs_valid = false;
};
