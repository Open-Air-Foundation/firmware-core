/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SD_CARD_SELFTEST_H
#define SD_CARD_SELFTEST_H

#include "sd_card_storage.h"

#include <cstdint>

/// Result of one write / read-back / delete pass over a mounted card.
///
/// Each step is reported separately because they fail for different reasons: a
/// card can mount and refuse writes when its controller has gone read-only at
/// end of life, and it can accept a write and hand back different bytes when
/// the bus is marginal. A single pass/fail would hide both.
struct SdSelfTest {
  bool wrote = false;
  bool read_back = false; ///< every byte came back as written
  bool deleted = false;   ///< the file was gone afterwards
  uint32_t bytes = 0;
  uint32_t write_ms = 0;
  uint32_t read_ms = 0;
  uint32_t write_kbps = 0;
  uint32_t read_kbps = 0;
  uint32_t first_bad_offset = 0; ///< only meaningful when read_back is false
  char detail[64] = {};          ///< the first step that failed, and why
};

/// Writes a pattern file, reads it back, compares it, and deletes it.
/// Requires a mounted card and leaves nothing behind on success.
bool sd_card_selftest(SdCardStorage &card, SdSelfTest &out, uint32_t bytes = 32u * 1024u);

#endif // SD_CARD_SELFTEST_H
