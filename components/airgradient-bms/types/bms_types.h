/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BMS_TYPES_H
#define BMS_TYPES_H

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Invalid sentinels
// ---------------------------------------------------------------------------

namespace BmsInvalid {
static constexpr float VOLT = -1.0f;
static constexpr float PERCENT = -1.0f;
static constexpr int16_t CURRENT_MA = -32768; // INT16_MIN
static constexpr uint16_t VOLTAGE_MV = 65535; // UINT16_MAX
static constexpr int16_t TEMPERATURE_C = -32768;

// Fuel-gauge-specific sentinels (used by PowerSnapshot FG fields).
static constexpr uint8_t SOC_PERCENT = 255;     // SOC is 0..100
static constexpr int16_t POWER_MW = -32768;     // INT16_MIN; signed power
static constexpr uint16_t CAPACITY_MAH = 65535; // UINT16_MAX
static constexpr float FG_TEMP_C = -273.16f;    // below absolute zero
} // namespace BmsInvalid

// ---------------------------------------------------------------------------
// Validation range
// ---------------------------------------------------------------------------

namespace BmsRange {
static constexpr float MIN_VALID_VOLT = 0.0f;
} // namespace BmsRange

// ---------------------------------------------------------------------------
// BmsChargingState
// ---------------------------------------------------------------------------

/// Shared public charging-state enum.  Replaces the driver-local
/// BQ25XX::ChargingStatus so that higher-level code does not need to
/// depend on a specific charger driver header.
enum class BmsChargingState : uint8_t {
  Unknown,
  NotCharging,
  TrickleCharge,
  PreCharge,
  FastCharge,
  TaperCharge,
  TopOffTimerActiveCharging,
  ChargeTerminationDone,
};

/// Human-readable label for a BmsChargingState value.
inline const char *bms_charging_state_str(BmsChargingState s) {
  switch (s) {
  case BmsChargingState::Unknown:
    return "Unknown";
  case BmsChargingState::NotCharging:
    return "NotCharging";
  case BmsChargingState::TrickleCharge:
    return "TrickleCharge";
  case BmsChargingState::PreCharge:
    return "PreCharge";
  case BmsChargingState::FastCharge:
    return "FastCharge";
  case BmsChargingState::TaperCharge:
    return "TaperCharge";
  case BmsChargingState::TopOffTimerActiveCharging:
    return "TopOff";
  case BmsChargingState::ChargeTerminationDone:
    return "Done";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// BmsTelemetry
// ---------------------------------------------------------------------------

/// ADC telemetry snapshot from a BMS device.
///
/// Contains voltage, current, and temperature readings from the charger
/// IC's integrated ADC.  All fields default to invalid sentinels so that
/// implementations that only populate a subset still produce a well-defined
/// result.
struct BmsTelemetry {
  // --- Voltages (V, float) ---
  float battery_voltage = BmsInvalid::VOLT;  ///< Battery terminal voltage (V)
  float charging_voltage = BmsInvalid::VOLT; ///< Input / VBUS voltage (V)

  // --- Currents (mA, signed) ---
  int16_t input_current_ma = BmsInvalid::CURRENT_MA;   ///< Input bus current
  int16_t battery_current_ma = BmsInvalid::CURRENT_MA; ///< Battery current (+charge / -discharge)

  // --- Additional voltages (mV, unsigned) ---
  uint16_t system_voltage_mv = BmsInvalid::VOLTAGE_MV; ///< System rail voltage
  uint16_t pmid_voltage_mv = BmsInvalid::VOLTAGE_MV;   ///< PMID / power-path voltage

  // --- Temperature ---
  float ts_percent = BmsInvalid::PERCENT;                    ///< Thermistor ADC reading (%)
  int16_t die_temperature_c = BmsInvalid::TEMPERATURE_C;     ///< IC die temperature (°C)
  int16_t battery_temperature_c = BmsInvalid::TEMPERATURE_C; ///< Battery NTC temperature (°C)

  // --- Validation helpers ---
  bool is_battery_voltage_valid() const { return battery_voltage >= BmsRange::MIN_VALID_VOLT; }

  bool is_charging_voltage_valid() const { return charging_voltage >= BmsRange::MIN_VALID_VOLT; }

  bool is_battery_temperature_valid() const {
    return battery_temperature_c != BmsInvalid::TEMPERATURE_C;
  }

  bool is_valid() const { return is_battery_voltage_valid() && is_charging_voltage_valid(); }
};

// ---------------------------------------------------------------------------
// BmsPowerSource
// ---------------------------------------------------------------------------

/// Generic power source / adapter type detected by the charger IC.
/// Maps from device-specific VBUS status registers.
enum class BmsPowerSource : uint8_t {
  Unknown,        ///< Status not read or not supported
  None,           ///< No adapter connected
  UsbSdp,         ///< USB Standard Downstream Port (500 mA)
  UsbCdp,         ///< USB Charging Downstream Port (1.5 A)
  UsbDcp,         ///< USB Dedicated Charging Port (1.5 A)
  UnknownAdapter, ///< Adapter detected, type unknown (500 mA)
  NonStandard,    ///< Non-standard adapter (1 A / 2.1 A / 2.4 A)
  OtgMode,        ///< OTG boost mode active (device is source)
};

/// Human-readable label for a BmsPowerSource value.
inline const char *bms_power_source_str(BmsPowerSource s) {
  switch (s) {
  case BmsPowerSource::Unknown:
    return "Unknown";
  case BmsPowerSource::None:
    return "None";
  case BmsPowerSource::UsbSdp:
    return "USB_SDP";
  case BmsPowerSource::UsbCdp:
    return "USB_CDP";
  case BmsPowerSource::UsbDcp:
    return "USB_DCP";
  case BmsPowerSource::UnknownAdapter:
    return "UnknownAdapter";
  case BmsPowerSource::NonStandard:
    return "NonStandard";
  case BmsPowerSource::OtgMode:
    return "OTG";
  }
  return "?";
}

/// Return true when the charger reports an external input source.
///
/// OTG mode means the device itself is sourcing power on PMID, so it is not
/// treated as an external input.
inline bool bms_power_source_has_external_input(BmsPowerSource s) {
  switch (s) {
  case BmsPowerSource::UsbSdp:
  case BmsPowerSource::UsbCdp:
  case BmsPowerSource::UsbDcp:
  case BmsPowerSource::UnknownAdapter:
  case BmsPowerSource::NonStandard:
    return true;
  case BmsPowerSource::Unknown:
  case BmsPowerSource::None:
  case BmsPowerSource::OtgMode:
    return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// BmsPmidMode
// ---------------------------------------------------------------------------

/// Requested PMID rail operating mode.
enum class BmsPmidMode : uint8_t {
  Unknown,
  PassThrough,
  Boost,
};

/// Human-readable label for a BmsPmidMode value.
inline const char *bms_pmid_mode_str(BmsPmidMode mode) {
  switch (mode) {
  case BmsPmidMode::Unknown:
    return "Unknown";
  case BmsPmidMode::PassThrough:
    return "PassThrough";
  case BmsPmidMode::Boost:
    return "Boost";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// BmsStatus
// ---------------------------------------------------------------------------

/// Charger status snapshot from a BMS device.
///
/// Contains charging state, power source identification, and charger
/// regulation / fault flags.  All boolean flags default to false and
/// enum fields default to Unknown so that implementations that only
/// populate a subset still produce a well-defined result.
struct BmsStatus {
  BmsChargingState charging_state = BmsChargingState::Unknown;
  BmsPowerSource power_source = BmsPowerSource::Unknown;

  bool thermal_regulation = false;       ///< Charger in thermal regulation
  bool vsys_regulation = false;          ///< VSYS at minimum system voltage
  bool input_current_regulation = false; ///< Input current limit active
  bool input_voltage_regulation = false; ///< Input voltage limit active
  bool safety_timer_expired = false;     ///< Safety timer has expired
  bool watchdog_expired = false;         ///< Watchdog timer has expired

  bool is_charging_state_valid() const { return charging_state != BmsChargingState::Unknown; }

  bool is_power_source_valid() const { return power_source != BmsPowerSource::Unknown; }

  bool is_valid() const { return is_charging_state_valid(); }
};

// ---------------------------------------------------------------------------
// FgCellConfig — fuel gauge cell configuration block
// ---------------------------------------------------------------------------

/// Fuel-gauge cell configuration block.  Used by the BQ27427 driver
/// (read_cell_config / write_cell_config) and by the host-side
/// evaluate_fg_state helper.  Lives here (not in the driver header)
/// so host tests can use it without pulling in ESP-IDF.
struct FgCellConfig {
  uint16_t design_capacity_mah;
  uint16_t design_energy_mwh;
  uint16_t terminate_voltage_mv;
  uint16_t sleep_current_ma;
};

inline bool operator==(const FgCellConfig &a, const FgCellConfig &b) {
  return a.design_capacity_mah == b.design_capacity_mah &&
         a.design_energy_mwh == b.design_energy_mwh &&
         a.terminate_voltage_mv == b.terminate_voltage_mv &&
         a.sleep_current_ma == b.sleep_current_ma;
}

inline bool operator!=(const FgCellConfig &a, const FgCellConfig &b) { return !(a == b); }

/// Fuel-gauge firmware-layer protection thresholds (BQ27742-G1 Safety
/// subclass, TRM SLUUAX0C §5.3.1).  All eight fields share one 32-byte data
/// flash block; the U1 delay/time fields in the same block are not exposed
/// because a zero in UV Prot Delay disables undervoltage protection outright.
/// Lives here (not in the driver header) so host tests can use it without
/// pulling in ESP-IDF.
struct FgProtectionConfig {
  uint16_t ov_prot_threshold_mv;
  uint16_t ov_prot_recovery_mv;
  uint16_t uv_prot_threshold_mv;
  uint16_t uv_prot_recovery_mv;
  int16_t ot_chg_dc;          ///< 0.1 °C
  int16_t ot_chg_recovery_dc; ///< 0.1 °C
  int16_t ot_dsg_dc;          ///< 0.1 °C
  int16_t ot_dsg_recovery_dc; ///< 0.1 °C
};

inline bool operator==(const FgProtectionConfig &a, const FgProtectionConfig &b) {
  return a.ov_prot_threshold_mv == b.ov_prot_threshold_mv &&
         a.ov_prot_recovery_mv == b.ov_prot_recovery_mv &&
         a.uv_prot_threshold_mv == b.uv_prot_threshold_mv &&
         a.uv_prot_recovery_mv == b.uv_prot_recovery_mv && a.ot_chg_dc == b.ot_chg_dc &&
         a.ot_chg_recovery_dc == b.ot_chg_recovery_dc && a.ot_dsg_dc == b.ot_dsg_dc &&
         a.ot_dsg_recovery_dc == b.ot_dsg_recovery_dc;
}

inline bool operator!=(const FgProtectionConfig &a, const FgProtectionConfig &b) {
  return !(a == b);
}

/// BQ27742-G1 hardware-protector configuration: subclass 64 (Registers)
/// bytes 4–6 and the Prot Checksum in subclass 57 (Integrity Data).  The
/// gauge compares the checksum against Prot OC Config + Prot OV Config every
/// second and holds both FETs open while they disagree (TRM §5.3.8.4), so the
/// three values are only meaningful together.
struct FgProtectorConfig {
  uint8_t pack_config_d;  ///< OTFET/FCFET/CIFET/CSFET… FET enables
  uint8_t prot_oc_config; ///< OCC/OCD/SCD codes
  uint8_t prot_ov_config; ///< OVP[2:0] code; fixes the paired UVP
  uint16_t prot_checksum;
};

/// The gauge's rule for Prot Checksum: a plain 16-bit sum of the two config
/// bytes (factory 0x0A + 0x07 = 0x0011), not the one's-complement block
/// checksum used to commit data flash.
inline uint16_t fg_protector_checksum(uint8_t prot_oc_config, uint8_t prot_ov_config) {
  return static_cast<uint16_t>(prot_oc_config) + static_cast<uint16_t>(prot_ov_config);
}

// ---------------------------------------------------------------------------
// FgFlags — part-neutral gauge flag set (BQ27427 Flags() layout plus the
// BQ27742-G1 charge-block bits)
// ---------------------------------------------------------------------------

namespace FgFlags {
static constexpr uint16_t DSG = (1u << 0);      ///< Discharging
static constexpr uint16_t BAT_DET = (1u << 3);  ///< Battery detected
static constexpr uint16_t CFGUP = (1u << 4);    ///< Config update mode active
static constexpr uint16_t ITPOR = (1u << 5);    ///< Gauge POR / reset detected
static constexpr uint16_t OCVTAKEN = (1u << 7); ///< OCV measurement taken
static constexpr uint16_t CHG = (1u << 8);      ///< Charge condition
static constexpr uint16_t FC = (1u << 9);       ///< Full Charge
/// Gauge is blocking charge on temperature.  Only the BQ27742-G1 reports these;
/// the BQ27427 has no equivalent and never sets them.  Bit positions are this
/// neutral layout's own, not either gauge's.
static constexpr uint16_t CHG_SUS = (1u << 10); ///< Charge suspended (was charging)
static constexpr uint16_t CHG_INH = (1u << 11); ///< Charge inhibited (not yet charging)
} // namespace FgFlags

// ---------------------------------------------------------------------------
// FgControlStatus — BQ27427 CONTROL_STATUS bits (Control(0x0000), TRM SLUUCD5A
// Table 5-3). Bits 4/5 are SLEEP/reserved, not these — getting them wrong made
// the learning verify always fail.
// ---------------------------------------------------------------------------

/// Impedance-Track learning progress, normalised across gauge parts.
///
/// The BQ27427 reports it in CONTROL_STATUS (QMAX_UP then RES_UP); the
/// BQ27742-G1 reports it in the Update Status data-flash byte (0x04 once IT is
/// enabled, 0x05 after the charge half, 0x06 after the discharge half).  Each
/// driver maps its own register onto these two booleans so PowerService and the
/// learning FSM stay part-neutral.
struct FgLearningProgress {
  bool qmax_updated = false; ///< a Qmax value has been learned this run
  bool ra_updated = false;   ///< optimised Qmax + Ra learned (cycle complete)
};

/// Convert a Q14 Qmax Cell 0 reading to mAh.  The BQ27427 stores Qmax as a
/// fraction of Design Capacity in 14-bit fixed point (TRM SLUUCD5 §7.4.2.3.1);
/// the BQ27742-G1 stores mAh directly and needs no conversion.
inline uint16_t fg_qmax_q14_to_mah(uint16_t raw, uint16_t design_capacity_mah) {
  return static_cast<uint16_t>((static_cast<uint32_t>(raw) * design_capacity_mah) / 16384u);
}

namespace FgControlStatus {
static constexpr uint16_t QMAX_UP = (1u << 9); ///< Qmax updated (clears on POR/BAT_DET)
static constexpr uint16_t RES_UP = (1u << 8);  ///< Ra updated (sets only after QMAX_UP)
} // namespace FgControlStatus

/// Ra impedance table length (BQ27427 has 15 grid points). Single source of
/// truth shared by the driver, PowerService verify readout, and the pure
/// FgLearningController.
static constexpr size_t FG_RA_TABLE_SIZE = 15;

#endif // BMS_TYPES_H
