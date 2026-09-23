/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SD_LOG_CAPTURE_H
#define SD_LOG_CAPTURE_H

#include "sd_card_storage.h"

#include <cstdint>

/// Copies every ESP_LOG line into logs/boot-NNNN.log on the microSD card,
/// alongside the console, which still gets every line unchanged.
///
/// Only ESP_LOG (and AG_LOG, which is ESP_LOG on target) is captured. A plain
/// printf() goes to stdout without passing through the log system, and the
/// bootloader and panic handler print through the ROM, so none of those reach
/// the file.
namespace sd_log {

/// Returns at once. The card is mounted and the file opened on a background
/// task, because the e-paper worker can hold the SPI bus for a whole refresh
/// and the boot path should not wait on it; lines logged in the meantime are
/// held in RAM and written first.
///
/// new_file: true opens the next boot-NNNN.log; false appends to the newest
/// one, so a wake from deep sleep continues the boot it belongs to rather than
/// starting a file per wake. header: one line written ahead of the log,
/// identifying the device and the boot.
///
/// Starts once per boot; later calls return false and change nothing.
bool start(SdCardStorage &card, bool new_file, const char *header);

/// Writes out everything queued, syncs, closes the file and unmounts the card.
/// For just before power goes away: anything logged after this stays on the
/// console only. A no-op when capture never started. False if it gave up after
/// timeout_ms with the writer still busy.
bool stop(uint32_t timeout_ms = 3000);

} // namespace sd_log

#endif // SD_LOG_CAPTURE_H
