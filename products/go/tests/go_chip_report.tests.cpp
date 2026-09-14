/**
 * AirGradient Go — Boot chip report unit tests
 *
 * Pure verdict bookkeeping and line formatting behind the serial-log chip
 * table printed at the end of boot.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>

#include "go_chip_report.h"

TEST_CASE("chip report starts untested and counts verdicts", "[chip-report]") {
  ChipReport r;
  CHECK(r.count(ChipState::Untested) == ChipReport::COUNT);

  r.set(Chip::Charger, true);
  r.set(Chip::Pm, false, "via TMUX121");
  r.set(Chip::Expander, ChipState::Absent);

  CHECK(r.count(ChipState::Pass) == 1);
  CHECK(r.count(ChipState::Fail) == 1);
  CHECK(r.count(ChipState::Absent) == 1);
  CHECK(r.count(ChipState::Untested) == ChipReport::COUNT - 3);
  CHECK(std::string(r.entry(Chip::Pm).note) == "via TMUX121");
}

TEST_CASE("a later verdict overrides an earlier one", "[chip-report]") {
  ChipReport r;
  r.set(Chip::Charger, false);
  r.set(Chip::Charger, true); // BMS retry succeeded
  CHECK(r.entry(Chip::Charger).state == ChipState::Pass);
  CHECK(r.entry(Chip::Charger).note[0] == '\0'); // note cleared with the new verdict
}

TEST_CASE("format_line aligns name, bus and verdict", "[chip-report]") {
  ChipReport r;
  r.label(Chip::FuelGauge, "BQ27742", "I2C 0x55");
  r.set(Chip::FuelGauge, true, "DEVICE_TYPE 0x0742");
  r.label(Chip::Gps, "TAU1113", "UART1");

  char line[96];
  r.format_line(Chip::FuelGauge, line, sizeof(line));
  CHECK(std::string(line) == "BQ27742    I2C 0x55    PASS  DEVICE_TYPE 0x0742");

  r.format_line(Chip::Gps, line, sizeof(line));
  CHECK(std::string(line) == "TAU1113    UART1       ----");

  r.set(Chip::Gps, ChipState::Absent);
  r.format_line(Chip::Gps, line, sizeof(line));
  CHECK(std::string(line) == "TAU1113    UART1       n/a ");
}

TEST_CASE("format_line and format_summary respect the buffer length", "[chip-report]") {
  ChipReport r;
  r.label(Chip::Nand, "W25N512", "SPI exp.P2");
  r.set(Chip::Nand, false, "mount failed");

  char tiny[8];
  const size_t n = r.format_line(Chip::Nand, tiny, sizeof(tiny));
  CHECK(n == sizeof(tiny) - 1);
  CHECK(std::strlen(tiny) == sizeof(tiny) - 1);

  char summary[64];
  r.format_summary(summary, sizeof(summary));
  CHECK(std::string(summary) == "pass 0  fail 1  untested 13  absent 0");
}

TEST_CASE("notes longer than the field are truncated, not overflowed", "[chip-report]") {
  ChipReport r;
  r.set(Chip::Touch, false, "this note is far longer than twenty-three characters");
  CHECK(std::strlen(r.entry(Chip::Touch).note) == sizeof(ChipEntry::note) - 1);
}
