/**
 * AirGradient Go — Boot chip report
 *
 * Collects one pass/fail verdict per chip during hardware init and renders
 * them as a table for the serial log, so a bring-up run shows at a glance
 * which parts answered.  Pure data + formatting: no ESP-IDF, host-testable.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include <cstddef>
#include <cstdint>

enum class Chip : uint8_t {
  Expander,  ///< TCA6408A (v2 only)
  FuelGauge, ///< BQ27427 (v1) / BQ27742 (v2)
  Charger,   ///< BQ25628
  TempHum,   ///< SHT4x
  Voc,       ///< SGP41
  Co2,       ///< S12 / SCD4x / STCC4
  Pressure,  ///< DPS368 / SPL07-003
  Accel,     ///< LIS2DH12
  Pm,        ///< SPS30 (behind TMUX121 on v1/v2)
  LedDriver, ///< LP5036
  Touch,     ///< CAP1203
  Nand,      ///< W25N512 (SPI)
  Display,   ///< SSD1680 e-paper (SPI)
  Gps,       ///< TAU1113 (UART) — driver created; data is a runtime check
  COUNT,
};

enum class ChipState : uint8_t {
  Untested, ///< init never reached this chip on this boot path
  Pass,     ///< driver init succeeded (identity verified where the driver does so)
  Fail,     ///< driver init failed
  Absent,   ///< not fitted on this board variant
};

struct ChipEntry {
  const char *name = "";
  const char *bus = "";
  ChipState state = ChipState::Untested;
  char note[24] = {};
};

class ChipReport {
public:
  static constexpr size_t COUNT = static_cast<size_t>(Chip::COUNT);

  /// Give a chip its display name and bus label (e.g. "BQ27742", "I2C 0x55").
  void label(Chip chip, const char *name, const char *bus);

  /// Record the verdict.  A later call overrides an earlier one, so a retry
  /// that succeeds turns a Fail into a Pass.
  void set(Chip chip, ChipState state, const char *note = nullptr);
  void set(Chip chip, bool ok, const char *note = nullptr) {
    set(chip, ok ? ChipState::Pass : ChipState::Fail, note);
  }

  const ChipEntry &entry(Chip chip) const { return _entries[static_cast<size_t>(chip)]; }
  size_t count(ChipState state) const;

  /// Render one aligned line: "NAME       BUS         PASS  note".
  /// Returns the number of characters written (excluding the terminator).
  size_t format_line(Chip chip, char *out, size_t len) const;

  /// Render the trailer: "pass 9  fail 1  untested 2  absent 2".
  size_t format_summary(char *out, size_t len) const;

  static const char *state_str(ChipState state);

private:
  ChipEntry _entries[COUNT];
};
