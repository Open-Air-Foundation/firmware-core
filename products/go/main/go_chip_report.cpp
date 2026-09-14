/**
 * AirGradient Go — Boot chip report implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_chip_report.h"

#include <cstdio>
#include <cstring>

void ChipReport::label(Chip chip, const char *name, const char *bus) {
  ChipEntry &e = _entries[static_cast<size_t>(chip)];
  e.name = name != nullptr ? name : "";
  e.bus = bus != nullptr ? bus : "";
}

void ChipReport::set(Chip chip, ChipState state, const char *note) {
  ChipEntry &e = _entries[static_cast<size_t>(chip)];
  e.state = state;
  if (note != nullptr) {
    snprintf(e.note, sizeof(e.note), "%s", note);
  } else {
    e.note[0] = '\0';
  }
}

size_t ChipReport::count(ChipState state) const {
  size_t n = 0;
  for (const ChipEntry &e : _entries) {
    if (e.state == state) {
      ++n;
    }
  }
  return n;
}

const char *ChipReport::state_str(ChipState state) {
  switch (state) {
  case ChipState::Pass:
    return "PASS";
  case ChipState::Fail:
    return "FAIL";
  case ChipState::Absent:
    return "n/a";
  case ChipState::Untested:
    break;
  }
  return "----";
}

size_t ChipReport::format_line(Chip chip, char *out, size_t len) const {
  if (out == nullptr || len == 0) {
    return 0;
  }
  const ChipEntry &e = entry(chip);
  const int n = snprintf(out, len, "%-10s %-11s %-4s%s%s", e.name, e.bus, state_str(e.state),
                         e.note[0] != '\0' ? "  " : "", e.note);
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n) < len ? static_cast<size_t>(n) : len - 1;
}

size_t ChipReport::format_summary(char *out, size_t len) const {
  if (out == nullptr || len == 0) {
    return 0;
  }
  const int n = snprintf(out, len, "pass %u  fail %u  untested %u  absent %u",
                         static_cast<unsigned>(count(ChipState::Pass)),
                         static_cast<unsigned>(count(ChipState::Fail)),
                         static_cast<unsigned>(count(ChipState::Untested)),
                         static_cast<unsigned>(count(ChipState::Absent)));
  if (n < 0) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n) < len ? static_cast<size_t>(n) : len - 1;
}
