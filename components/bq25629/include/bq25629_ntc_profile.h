/**
 * @file bq25629_ntc_profile.h
 * @brief Typed JEITA/NTC temperature-profile configuration for the BQ25629
 *
 * Host-testable: no ESP-IDF dependencies. The charger's NTC_Control registers
 * (REG0x1A/0x1B/0x1C, datasheet SLUSEG4C tables 8-23..8-25) pack threshold
 * and action fields into raw bytes; this header names every option and
 * encodes a profile back into those bytes at compile time.
 *
 * The chip compares the TS pin against fixed percentages of its bias rail.
 * Datasheet temperature labels for those percentages assume TI's reference
 * network (RT1 = 5.24 kΩ, RT2 = 30.31 kΩ, 103AT NTC, B = 3435/3950 K class).
 * This board populates RT1 = 4.12 kΩ / RT2 = 17.33 kΩ and an NTC measured at
 * B ≈ 3511 K (calibrated 2026-08-28 against the on-board DPS368), so each
 * option below lists the board-real trip temperature next to the datasheet
 * label. Comparator tolerance is ±0.5 % of bias ≈ ±1 °C on this network;
 * every threshold releases (hysteresis) about 2 label-°C inside its trip
 * point.
 */

#pragma once

#include <cstdint>

namespace drivers {
namespace ntc_profile {

/// REG0x1A bit 7 — TS_IGNORE
enum class TsFeedback : uint8_t {
  kUsed = 0,    ///< Charger enforces the TS profile (chip default)
  kIgnored = 1, ///< TS always reported good; charge/OTG never TS-limited
};

/// REG0x1A bits 6:5 — TS_TH_OTG_HOT: suspend OTG boost above this temperature
enum class OtgHotSuspend : uint8_t {
  kAt55C = 0,    ///< 37.7 % of bias — board-real +59.9 °C
  kAt60C = 1,    ///< 34.4 % of bias — board-real +65.2 °C (chip default)
  kAt65C = 2,    ///< 31.3 % of bias — board-real +70.5 °C
  kDisabled = 3, ///< No hot cutoff for OTG
};

/// REG0x1A bit 4 — TS_TH_OTG_COLD: suspend OTG boost below this temperature
enum class OtgColdSuspend : uint8_t {
  kAtMinus20C = 0, ///< 80.0 % of bias — board-real −43.5 °C
  kAtMinus10C = 1, ///< 77.1 % of bias — board-real −17.1 °C (chip default)
};

/// Charge-current action applied inside a JEITA zone (TS_ISET_* fields)
enum class ChargeAction : uint8_t {
  kSuspend = 0,
  kIchg20Percent = 1,
  kIchg40Percent = 2,
  kUnchanged = 3,
};

/// Charge-voltage action applied inside a JEITA zone (TS_VSET_* fields)
enum class VregAction : uint8_t {
  kVregMinus300mV = 0,
  kVregMinus200mV = 1,
  kVregMinus100mV = 2,
  kUnchanged = 3,
};

/// REG0x1C bit 6 — TS_VSET_SYM: mirror warm-side VREG derating onto the
/// cold-side zones (PRECOOL uses TS_VSET_PREWARM, COOL uses TS_VSET_WARM)
enum class CoolVregPolicy : uint8_t {
  kUnchanged = 0,      ///< Cold-side zones keep full VREG (chip default)
  kMirrorWarmSide = 1,
};

/// REG0x1B bits 7:5 — TS_TH1_TH2_TH3, cold-side zone boundaries.
/// TH1 = COLD suspend, TH2 = COOL/PRECOOL boundary, TH3 = PRECOOL/NORMAL.
/// Names carry the datasheet labels; comments give board-real °C.
enum class ColdThresholds : uint8_t {
  kTh1_0_Th2_5_Th3_15 = 0,   ///< real −2.2 / +4.0 / +16.3 °C
  kTh1_0_Th2_10_Th3_15 = 1,  ///< real −2.2 / +10.4 / +16.3 °C (chip default)
  kTh1_0_Th2_15_Th3_20 = 2,  ///< real −2.2 / +16.3 / +22.0 °C
  kTh1_0_Th2_20_Th3_20 = 3,  ///< real −2.2 / +22.0 / +22.0 °C
  kTh1_m5_Th2_5_Th3_15 = 4,  ///< real −9.8 / +4.0 / +16.3 °C
  kTh1_m5_Th2_10_Th3_15 = 5, ///< real −9.8 / +10.4 / +16.3 °C
  kTh1_m5_Th2_10_Th3_20 = 6, ///< real −9.8 / +10.4 / +22.0 °C
  kTh1_0_Th2_10_Th3_20 = 7,  ///< real −2.2 / +10.4 / +22.0 °C
};

/// REG0x1B bits 4:2 — TS_TH4_TH5_TH6, warm-side zone boundaries.
/// TH4 = NORMAL/PREWARM boundary, TH5 = PREWARM/WARM, TH6 = HOT suspend.
/// The register table (8-24) prints code 001 as TH5 = 40 °C, but the
/// electrical characteristics (§7.5) place code 001 in the 45 °C comparator
/// group (44.8 % of bias); the EC table is the authoritative one and matches
/// the suspend temperature measured on this board.
enum class WarmThresholds : uint8_t {
  kTh4_35_Th5_40_Th6_60 = 0, ///< real 38.5 / 43.8 / 65.2 °C
  kTh4_35_Th5_45_Th6_60 = 1, ///< real 38.5 / 49.1 / 65.2 °C (chip default)
  kTh4_35_Th5_50_Th6_60 = 2, ///< real 38.5 / 54.5 / 65.2 °C
  kTh4_40_Th5_55_Th6_60 = 3, ///< real 43.8 / 59.9 / 65.2 °C
  kTh4_35_Th5_40_Th6_50 = 4, ///< real 38.5 / 43.8 / 54.5 °C
  kTh4_35_Th5_45_Th6_50 = 5, ///< real 38.5 / 49.1 / 54.5 °C
  kTh4_40_Th5_45_Th6_60 = 6, ///< real 43.8 / 49.1 / 65.2 °C
  kTh4_40_Th5_50_Th6_60 = 7, ///< real 43.8 / 54.5 / 65.2 °C
};

/// Raw NTC_Control register bytes produced by NtcProfile::encode().
struct NtcProfileBytes {
  uint8_t control0; ///< REG0x1A NTC_Control_0
  uint8_t control1; ///< REG0x1B NTC_Control_1
  uint8_t control2; ///< REG0x1C NTC_Control_2
};

struct NtcProfile {
  TsFeedback ts_feedback;
  OtgHotSuspend otg_hot;
  OtgColdSuspend otg_cold;
  ChargeAction warm_action;    ///< TS_ISET_WARM — WARM zone (TH5..TH6)
  ChargeAction cool_action;    ///< TS_ISET_COOL — COOL zone (TH1..TH2)
  ColdThresholds cold_thresholds;
  WarmThresholds warm_thresholds;
  VregAction warm_vreg;        ///< TS_VSET_WARM — WARM zone
  CoolVregPolicy cool_vreg;    ///< TS_VSET_SYM
  VregAction prewarm_vreg;     ///< TS_VSET_PREWARM — PREWARM zone (TH4..TH5)
  ChargeAction prewarm_action; ///< TS_ISET_PREWARM — PREWARM zone
  ChargeAction precool_action; ///< TS_ISET_PRECOOL — PRECOOL zone (TH2..TH3)

  constexpr NtcProfileBytes encode() const {
    return {
        static_cast<uint8_t>((static_cast<uint8_t>(ts_feedback) << 7) |
                             (static_cast<uint8_t>(otg_hot) << 5) |
                             (static_cast<uint8_t>(otg_cold) << 4) |
                             (static_cast<uint8_t>(warm_action) << 2) |
                             static_cast<uint8_t>(cool_action)),
        static_cast<uint8_t>((static_cast<uint8_t>(cold_thresholds) << 5) |
                             (static_cast<uint8_t>(warm_thresholds) << 2) |
                             static_cast<uint8_t>(warm_vreg)),
        static_cast<uint8_t>((static_cast<uint8_t>(cool_vreg) << 6) |
                             (static_cast<uint8_t>(prewarm_vreg) << 4) |
                             (static_cast<uint8_t>(prewarm_action) << 2) |
                             static_cast<uint8_t>(precool_action)),
    };
  }
};

/// Profile programmed into the Go: every field at chip default except
/// TS_ISET_WARM, which suspends charging in the WARM zone (board-real
/// ≥ +49.1 °C) instead of continuing at full current.
constexpr NtcProfile GO_PROFILE{
    TsFeedback::kUsed,            // charger enforces this profile on its own
    OtgHotSuspend::kAt60C,        // boost cut above +65.2 °C real
    OtgColdSuspend::kAtMinus10C,  // boost cut below −17.1 °C real
    ChargeAction::kSuspend,       // WARM (+49.1..+65.2 real): stop charging —
                                  // the one deliberate change from defaults
    ChargeAction::kIchg20Percent, // COOL (−2.2..+10.4 real): charge at 20 %
    ColdThresholds::kTh1_0_Th2_10_Th3_15,  // real −2.2 / +10.4 / +16.3 °C
    WarmThresholds::kTh4_35_Th5_45_Th6_60, // real +38.5 / +49.1 / +65.2 °C
    VregAction::kVregMinus200mV,  // WARM: VREG −200 mV (moot while suspended)
    CoolVregPolicy::kUnchanged,   // cold side keeps full VREG
    VregAction::kUnchanged,       // PREWARM (+38.5..+49.1 real): full VREG
    ChargeAction::kUnchanged,     // PREWARM: full current
    ChargeAction::kUnchanged,     // PRECOOL (+10.4..+16.3 real): full current
};

// The Go has shipped with these exact bytes since bring-up; any profile edit
// must change them knowingly, not by accident.
static_assert(GO_PROFILE.encode().control0 == 0x31,
              "GO_PROFILE no longer encodes NTC_Control_0 = 0x31");
static_assert(GO_PROFILE.encode().control1 == 0x25,
              "GO_PROFILE no longer encodes NTC_Control_1 = 0x25");
static_assert(GO_PROFILE.encode().control2 == 0x3F,
              "GO_PROFILE no longer encodes NTC_Control_2 = 0x3F");

} // namespace ntc_profile
} // namespace drivers
