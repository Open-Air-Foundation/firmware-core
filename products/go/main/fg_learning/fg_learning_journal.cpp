/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "fg_learning/fg_learning_journal.h"

#include <cstdio>
#include <cstring>
#include <unistd.h> // fsync, fileno

#include "ag_log.h"
#include "rtos.h"

static constexpr const char *TAG = "FgJournal";

namespace {

const char *stage_str(FgLearningStage s) {
  switch (s) {
  case FgLearningStage::Idle:
    return "Idle";
  case FgLearningStage::Charge:
    return "Charge";
  case FgLearningStage::Rest:
    return "Rest";
  case FgLearningStage::Discharge:
    return "Discharge";
  case FgLearningStage::CycleDone:
    return "CycleDone";
  case FgLearningStage::Verify:
    return "Verify";
  case FgLearningStage::Complete:
    return "Complete";
  case FgLearningStage::Failed:
    return "Failed";
  }
  return "?";
}

} // namespace

void FgLearningJournal::begin(const char *mount_path, bool storage_ready) {
  _enabled = storage_ready && mount_path != nullptr;
  if (_enabled) {
    snprintf(_path, sizeof(_path), "%s/fglearn.log", mount_path);
    AG_LOGI(TAG, "journal file: %s", _path);
  } else {
    AG_LOGW(TAG, "journal disabled (NAND not mounted) — serial mirror only");
  }
}

void FgLearningJournal::write_line(const char *body) {
  char line[256];
  const uint32_t up_s = RTOS::get_time_ms() / 1000u;
  snprintf(line, sizeof(line), "FGJ #%lu %lus %s", static_cast<unsigned long>(_seq++),
           static_cast<unsigned long>(up_s), body);

  // Always mirror to serial (useful when connected live).
  AG_LOGI(TAG, "%s", line);

  if (!_enabled) {
    return;
  }
  FILE *f = fopen(_path, "a");
  if (f == nullptr) {
    AG_LOGW(TAG, "append fopen(%s) failed", _path);
    return;
  }
  fprintf(f, "%s\n", line);
  fflush(f);
  fsync(fileno(f)); // bound loss on a sudden reset to the in-flight record
  fclose(f);
}

void FgLearningJournal::dump_to_serial() {
  if (!_enabled) {
    AG_LOGW(TAG, "dump skipped — journal disabled");
    return;
  }
  FILE *f = fopen(_path, "r");
  if (f == nullptr) {
    AG_LOGI(TAG, "no journal file yet (%s)", _path);
    return;
  }
  AG_LOGI(TAG, "===== FG LEARNING JOURNAL (%s) =====", _path);
  char buf[256];
  while (fgets(buf, sizeof(buf), f) != nullptr) {
    const size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') {
      buf[n - 1] = '\0';
    }
    AG_LOGI(TAG, "%s", buf);
  }
  fclose(f);
  AG_LOGI(TAG, "===== END JOURNAL =====");
}

void FgLearningJournal::reset() {
  _seq = 0;
  if (!_enabled) {
    return;
  }
  FILE *f = fopen(_path, "w"); // truncate
  if (f != nullptr) {
    fclose(f);
  }
  AG_LOGI(TAG, "journal reset");
}

void FgLearningJournal::boot(int reset_reason, FgLearningStage stage, uint8_t cycle,
                             uint8_t itpor_losses, bool itpor_now, uint8_t soc_pct,
                             uint16_t vbat_mv) {
  char b[160];
  snprintf(
      b, sizeof(b), "BOOT rst=%d stage=%s cycle=%u itpor_losses=%u itpor_now=%d soc=%u%% vbat=%umV",
      reset_reason, stage_str(stage), cycle, itpor_losses, itpor_now ? 1 : 0, soc_pct, vbat_mv);
  write_line(b);
}

void FgLearningJournal::verify(bool reads_ok, bool itpor, bool qmax_up, uint16_t qmax_mah,
                               uint16_t design_cap_mah, const int16_t *ra, size_t ra_len, bool pass,
                               FgLearnFailReason reason) {
  char b[256];
  int p = snprintf(b, sizeof(b),
                   "VERIFY pass=%d reason=%s reads_ok=%d itpor=%d qmax_up=%d qmax=%umAh dc=%umAh "
                   "ra=[",
                   pass ? 1 : 0, fg_learn_fail_reason_str(reason), reads_ok ? 1 : 0, itpor ? 1 : 0,
                   qmax_up ? 1 : 0, qmax_mah, design_cap_mah);
  for (size_t i = 0; i < ra_len && p > 0 && p < static_cast<int>(sizeof(b)) - 8; ++i) {
    p += snprintf(b + p, sizeof(b) - static_cast<size_t>(p), "%s%d", i == 0 ? "" : ",", ra[i]);
  }
  if (p > 0 && p < static_cast<int>(sizeof(b)) - 1) {
    snprintf(b + p, sizeof(b) - static_cast<size_t>(p), "]");
  }
  write_line(b);
}

void FgLearningJournal::result(FgLearningStage stage, FgLearnFailReason reason) {
  char b[96];
  snprintf(b, sizeof(b), "RESULT stage=%s reason=%s", stage_str(stage),
           fg_learn_fail_reason_str(reason));
  write_line(b);
}

void FgLearningJournal::stage_enter(FgLearningStage stage, uint8_t cycle, uint8_t soc_pct,
                                    uint16_t vbat_mv) {
  char b[96];
  snprintf(b, sizeof(b), "STAGE %s cycle=%u soc=%u%% vbat=%umV", stage_str(stage), cycle, soc_pct,
           vbat_mv);
  write_line(b);
}

void FgLearningJournal::flag_event(const char *name, uint8_t soc_pct, uint16_t vbat_mv,
                                   int16_t current_ma) {
  char b[96];
  snprintf(b, sizeof(b), "FLAG %s soc=%u%% vbat=%umV i=%dmA", name, soc_pct, vbat_mv, current_ma);
  write_line(b);
}

void FgLearningJournal::edv_ship(uint8_t cycle, uint8_t soc_pct, uint16_t vbat_mv) {
  char b[96];
  snprintf(b, sizeof(b), "EDV_SHIP cycle=%u soc=%u%% vbat=%umV", cycle, soc_pct, vbat_mv);
  write_line(b);
}

void FgLearningJournal::target_ship(uint8_t cycle, uint8_t soc_pct, uint16_t vbat_mv) {
  char b[96];
  snprintf(b, sizeof(b), "TARGET_SHIP cycle=%u soc=%u%% vbat=%umV", cycle, soc_pct, vbat_mv);
  write_line(b);
}
