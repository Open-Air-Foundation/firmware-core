/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "sd_card_selftest.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static constexpr const char *TAG = "SdTest";

namespace {

// One sector's worth, reused for the whole file: a 32 KB stack buffer would
// not survive any of the tasks this runs from.
constexpr size_t CHUNK_BYTES = 512;

// Counter plus a shifting byte, so a chunk written to the wrong offset or a
// stuck bus line both show up as a mismatch rather than reading back clean.
void fill_chunk(uint8_t *chunk, uint32_t index) {
  for (size_t i = 0; i < CHUNK_BYTES; ++i) {
    chunk[i] = static_cast<uint8_t>((index * 31u + i) & 0xFF);
  }
}

uint32_t kbps(uint32_t bytes, uint32_t ms) {
  return ms == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(bytes) * 1000u) / (ms * 1024u));
}

} // namespace

bool sd_card_selftest(SdCardStorage &card, SdSelfTest &out, uint32_t bytes) {
  out = SdSelfTest{};
  if (!card.is_mounted()) {
    std::snprintf(out.detail, sizeof(out.detail), "no card mounted");
    return false;
  }

  const uint32_t chunks = bytes / CHUNK_BYTES;
  if (chunks == 0) {
    std::snprintf(out.detail, sizeof(out.detail), "size must be at least %u B", CHUNK_BYTES);
    return false;
  }
  out.bytes = chunks * CHUNK_BYTES;

  char path[80];
  std::snprintf(path, sizeof(path), "%s/agtest.bin", card.mount_path());
  uint8_t chunk[CHUNK_BYTES];

  // --- write ---
  int64_t started = esp_timer_get_time();
  FILE *f = std::fopen(path, "wb");
  if (f == nullptr) {
    std::snprintf(out.detail, sizeof(out.detail), "open for write: %s", std::strerror(errno));
    return false;
  }
  for (uint32_t i = 0; i < chunks; ++i) {
    fill_chunk(chunk, i);
    if (std::fwrite(chunk, 1, CHUNK_BYTES, f) != CHUNK_BYTES) {
      std::fclose(f);
      std::snprintf(out.detail, sizeof(out.detail), "write at %lu B: %s",
                    static_cast<unsigned long>(i * CHUNK_BYTES), std::strerror(errno));
      std::remove(path);
      return false;
    }
  }
  // fclose is where a buffered write finally reaches the card, so a failure
  // here is a failed write and not a tidy-up detail.
  if (std::fclose(f) != 0) {
    std::snprintf(out.detail, sizeof(out.detail), "close after write: %s", std::strerror(errno));
    std::remove(path);
    return false;
  }
  out.write_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);
  out.write_kbps = kbps(out.bytes, out.write_ms);
  out.wrote = true;

  // --- read back ---
  started = esp_timer_get_time();
  f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::snprintf(out.detail, sizeof(out.detail), "open for read: %s", std::strerror(errno));
    return false;
  }
  uint8_t expected[CHUNK_BYTES];
  bool matched = true;
  for (uint32_t i = 0; i < chunks && matched; ++i) {
    if (std::fread(chunk, 1, CHUNK_BYTES, f) != CHUNK_BYTES) {
      std::snprintf(out.detail, sizeof(out.detail), "short read at %lu B",
                    static_cast<unsigned long>(i * CHUNK_BYTES));
      matched = false;
      break;
    }
    fill_chunk(expected, i);
    if (std::memcmp(chunk, expected, CHUNK_BYTES) != 0) {
      size_t j = 0;
      while (j < CHUNK_BYTES && chunk[j] == expected[j]) {
        ++j;
      }
      out.first_bad_offset = static_cast<uint32_t>(i * CHUNK_BYTES + j);
      std::snprintf(out.detail, sizeof(out.detail), "byte %lu: got 0x%02X want 0x%02X",
                    static_cast<unsigned long>(out.first_bad_offset), chunk[j], expected[j]);
      matched = false;
    }
  }
  std::fclose(f);
  out.read_ms = static_cast<uint32_t>((esp_timer_get_time() - started) / 1000);
  out.read_kbps = kbps(out.bytes, out.read_ms);
  out.read_back = matched;
  if (!matched) {
    std::remove(path);
    return false;
  }

  // --- delete ---
  if (std::remove(path) != 0) {
    std::snprintf(out.detail, sizeof(out.detail), "delete: %s", std::strerror(errno));
    return false;
  }
  struct stat st;
  if (stat(path, &st) == 0) {
    std::snprintf(out.detail, sizeof(out.detail), "file still present after delete");
    return false;
  }
  out.deleted = true;

  ESP_LOGI(TAG, "%lu B: write %lu ms (%lu KB/s), read %lu ms (%lu KB/s), verified, deleted",
           static_cast<unsigned long>(out.bytes), static_cast<unsigned long>(out.write_ms),
           static_cast<unsigned long>(out.write_kbps), static_cast<unsigned long>(out.read_ms),
           static_cast<unsigned long>(out.read_kbps));
  return true;
}
