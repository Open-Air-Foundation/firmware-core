/**
 * AirGradient Go — Fuel-Gauge Learning Journal (persistent diagnostics)
 *
 * Append-only text log on the SPI-NAND FATFS mount. The learning run spans
 * ~1 day and many reboots / ship-cycles, so live serial is unreliable: every
 * event is appended to a file that survives power-off, and the whole file is
 * replayed to serial on each boot and at the terminal result. Connect serial
 * at any point — even at the Failed screen — and the full history prints.
 *
 * Writes are event-driven only (boots, stage transitions, learning-flag edges,
 * verify, ship, result) — never per-poll — so NAND wear stays trivial. Each
 * append is fsync'd so a sudden reset loses at most the in-flight record.
 * Target-only — never host-compiled.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstddef>
#include <cstdint>

#include "fg_learning/fg_learning_controller.h" // FgLearnFailReason
#include "go_settings.h"                        // FgLearningStage

class FgLearningJournal {
public:
  FgLearningJournal() = default;

  /// Build the log path and enable file ops only when the NAND mount is ready.
  /// When disabled, every method still mirrors to serial but writes no file.
  void begin(const char *mount_path, bool storage_ready);

  /// Replay the whole journal file to serial (boot + terminal).
  void dump_to_serial();

  /// Truncate the file — fresh run / abort.
  void reset();

  // --- Tier 1: the "why did it fail / is it rebooting" events ---

  void boot(int reset_reason, FgLearningStage stage, uint8_t cycle, uint8_t itpor_losses,
            bool itpor_now, uint8_t soc_pct, uint16_t vbat_mv);
  void verify(bool reads_ok, bool itpor, bool qmax_up, uint16_t qmax_mah, uint16_t design_cap_mah,
              const int16_t *ra, size_t ra_len, bool pass, FgLearnFailReason reason);
  void result(FgLearningStage stage, FgLearnFailReason reason);

  // --- Tier 2: progress / qualification trail ---

  void stage_enter(FgLearningStage stage, uint8_t cycle, uint8_t soc_pct, uint16_t vbat_mv);
  void flag_event(const char *name, uint8_t soc_pct, uint16_t vbat_mv, int16_t current_ma);
  void edv_ship(uint8_t cycle, uint8_t soc_pct, uint16_t vbat_mv);
  void target_ship(uint8_t cycle, uint8_t soc_pct, uint16_t vbat_mv);

private:
  void write_line(const char *body); ///< prefix seq+uptime, mirror to serial, append+fsync

  bool _enabled = false;
  char _path[80] = {0};
  uint32_t _seq = 0;
};
