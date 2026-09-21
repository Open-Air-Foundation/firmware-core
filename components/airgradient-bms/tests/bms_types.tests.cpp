/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <string>
#include <utility>

#include "types/bms_types.h"

using Catch::Matchers::WithinAbs;

// ---------------------------------------------------------------------------
// BmsInvalid sentinel constants
// ---------------------------------------------------------------------------

TEST_CASE("BmsInvalid sentinel values", "[BmsTypes]") {
  REQUIRE(BmsInvalid::VOLT < 0.0f);
  REQUIRE(BmsInvalid::PERCENT < 0.0f);
  REQUIRE(BmsInvalid::CURRENT_MA == -32768);
  REQUIRE(BmsInvalid::VOLTAGE_MV == 65535);
  REQUIRE(BmsInvalid::TEMPERATURE_C == -32768);
}

// ---------------------------------------------------------------------------
// BmsTelemetry validation
// ---------------------------------------------------------------------------

TEST_CASE("BmsTelemetry default-constructed is invalid", "[BmsTypes]") {
  BmsTelemetry t{};
  REQUIRE_FALSE(t.is_battery_voltage_valid());
  REQUIRE_FALSE(t.is_charging_voltage_valid());
  REQUIRE_FALSE(t.is_valid());
}

TEST_CASE("BmsTelemetry with valid voltages", "[BmsTypes]") {
  BmsTelemetry t{};
  t.battery_voltage = 3.7f;
  t.charging_voltage = 5.0f;

  REQUIRE(t.is_battery_voltage_valid());
  REQUIRE(t.is_charging_voltage_valid());
  REQUIRE(t.is_valid());
}

TEST_CASE("BmsTelemetry with zero voltage is valid", "[BmsTypes]") {
  BmsTelemetry t{};
  t.battery_voltage = 0.0f;
  t.charging_voltage = 0.0f;

  REQUIRE(t.is_battery_voltage_valid());
  REQUIRE(t.is_charging_voltage_valid());
  REQUIRE(t.is_valid());
}

TEST_CASE("BmsTelemetry partial validity", "[BmsTypes]") {
  SECTION("Battery valid, charging invalid") {
    BmsTelemetry t{};
    t.battery_voltage = 3.7f;
    // charging_voltage left at default sentinel

    REQUIRE(t.is_battery_voltage_valid());
    REQUIRE_FALSE(t.is_charging_voltage_valid());
    REQUIRE_FALSE(t.is_valid());
  }

  SECTION("Battery invalid, charging valid") {
    BmsTelemetry t{};
    // battery_voltage left at default sentinel
    t.charging_voltage = 5.0f;

    REQUIRE_FALSE(t.is_battery_voltage_valid());
    REQUIRE(t.is_charging_voltage_valid());
    REQUIRE_FALSE(t.is_valid());
  }
}

// ---------------------------------------------------------------------------
// BmsStatus validation
// ---------------------------------------------------------------------------

TEST_CASE("BmsStatus default-constructed is invalid", "[BmsTypes]") {
  BmsStatus s{};
  REQUIRE(s.charging_state == BmsChargingState::Unknown);
  REQUIRE_FALSE(s.is_charging_state_valid());
  REQUIRE_FALSE(s.is_valid());
}

TEST_CASE("BmsStatus with known charging state is valid", "[BmsTypes]") {
  BmsStatus s{};
  s.charging_state = BmsChargingState::FastCharge;

  REQUIRE(s.is_charging_state_valid());
  REQUIRE(s.is_valid());
}

TEST_CASE("BmsStatus all charging states", "[BmsTypes]") {
  // Unknown should be invalid; all others should be valid
  REQUIRE_FALSE(BmsStatus{BmsChargingState::Unknown}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::NotCharging}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::TrickleCharge}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::PreCharge}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::FastCharge}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::TaperCharge}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::TopOffTimerActiveCharging}.is_charging_state_valid());
  REQUIRE(BmsStatus{BmsChargingState::ChargeTerminationDone}.is_charging_state_valid());
}

// ---------------------------------------------------------------------------
// BmsChargingState enum values
// ---------------------------------------------------------------------------

TEST_CASE("BmsChargingState enum distinct values", "[BmsTypes]") {
  // Verify all enum values are distinct
  REQUIRE(static_cast<uint8_t>(BmsChargingState::Unknown) !=
          static_cast<uint8_t>(BmsChargingState::NotCharging));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::NotCharging) !=
          static_cast<uint8_t>(BmsChargingState::TrickleCharge));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::TrickleCharge) !=
          static_cast<uint8_t>(BmsChargingState::PreCharge));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::PreCharge) !=
          static_cast<uint8_t>(BmsChargingState::FastCharge));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::FastCharge) !=
          static_cast<uint8_t>(BmsChargingState::TaperCharge));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::TaperCharge) !=
          static_cast<uint8_t>(BmsChargingState::TopOffTimerActiveCharging));
  REQUIRE(static_cast<uint8_t>(BmsChargingState::TopOffTimerActiveCharging) !=
          static_cast<uint8_t>(BmsChargingState::ChargeTerminationDone));
}

// ---------------------------------------------------------------------------
// BmsPowerSource enum values
// ---------------------------------------------------------------------------

TEST_CASE("BmsPowerSource enum distinct values", "[BmsTypes]") {
  const uint8_t values[] = {
      static_cast<uint8_t>(BmsPowerSource::Unknown),
      static_cast<uint8_t>(BmsPowerSource::None),
      static_cast<uint8_t>(BmsPowerSource::UsbSdp),
      static_cast<uint8_t>(BmsPowerSource::UsbCdp),
      static_cast<uint8_t>(BmsPowerSource::UsbDcp),
      static_cast<uint8_t>(BmsPowerSource::UnknownAdapter),
      static_cast<uint8_t>(BmsPowerSource::NonStandard),
      static_cast<uint8_t>(BmsPowerSource::OtgMode),
  };
  constexpr size_t N = sizeof(values) / sizeof(values[0]);
  for (size_t i = 0; i < N; ++i) {
    for (size_t j = i + 1; j < N; ++j) {
      REQUIRE(values[i] != values[j]);
    }
  }
}

// ---------------------------------------------------------------------------
// BmsStatus — power source validation
// ---------------------------------------------------------------------------

TEST_CASE("BmsStatus default has Unknown power source", "[BmsTypes]") {
  BmsStatus s{};
  REQUIRE(s.power_source == BmsPowerSource::Unknown);
  REQUIRE_FALSE(s.is_power_source_valid());
}

TEST_CASE("BmsStatus all power sources", "[BmsTypes]") {
  // Unknown should be invalid; all others should be valid
  auto make = [](BmsPowerSource ps) {
    BmsStatus s{};
    s.power_source = ps;
    return s;
  };
  REQUIRE_FALSE(make(BmsPowerSource::Unknown).is_power_source_valid());
  REQUIRE(make(BmsPowerSource::None).is_power_source_valid());
  REQUIRE(make(BmsPowerSource::UsbSdp).is_power_source_valid());
  REQUIRE(make(BmsPowerSource::UsbCdp).is_power_source_valid());
  REQUIRE(make(BmsPowerSource::UsbDcp).is_power_source_valid());
  REQUIRE(make(BmsPowerSource::UnknownAdapter).is_power_source_valid());
  REQUIRE(make(BmsPowerSource::NonStandard).is_power_source_valid());
  REQUIRE(make(BmsPowerSource::OtgMode).is_power_source_valid());
}

// ---------------------------------------------------------------------------
// BmsStatus — boolean flags default to false
// ---------------------------------------------------------------------------

TEST_CASE("BmsStatus boolean flags default to false", "[BmsTypes]") {
  BmsStatus s{};
  REQUIRE_FALSE(s.thermal_regulation);
  REQUIRE_FALSE(s.vsys_regulation);
  REQUIRE_FALSE(s.input_current_regulation);
  REQUIRE_FALSE(s.input_voltage_regulation);
  REQUIRE_FALSE(s.safety_timer_expired);
  REQUIRE_FALSE(s.watchdog_expired);
}

TEST_CASE("BmsStatus boolean flags can be set", "[BmsTypes]") {
  BmsStatus s{};
  s.thermal_regulation = true;
  s.vsys_regulation = true;
  s.input_current_regulation = true;
  s.input_voltage_regulation = true;
  s.safety_timer_expired = true;
  s.watchdog_expired = true;

  REQUIRE(s.thermal_regulation);
  REQUIRE(s.vsys_regulation);
  REQUIRE(s.input_current_regulation);
  REQUIRE(s.input_voltage_regulation);
  REQUIRE(s.safety_timer_expired);
  REQUIRE(s.watchdog_expired);
}

// ---------------------------------------------------------------------------
// BmsTelemetry — new ADC fields default to sentinels
// ---------------------------------------------------------------------------

TEST_CASE("BmsTelemetry new fields default to sentinels", "[BmsTypes]") {
  BmsTelemetry t{};
  REQUIRE(t.input_current_ma == BmsInvalid::CURRENT_MA);
  REQUIRE(t.battery_current_ma == BmsInvalid::CURRENT_MA);
  REQUIRE(t.system_voltage_mv == BmsInvalid::VOLTAGE_MV);
  REQUIRE(t.pmid_voltage_mv == BmsInvalid::VOLTAGE_MV);
  REQUIRE(t.ts_percent == Catch::Approx(BmsInvalid::PERCENT));
  REQUIRE(t.die_temperature_c == BmsInvalid::TEMPERATURE_C);
  REQUIRE(t.battery_temperature_c == BmsInvalid::TEMPERATURE_C);
}

TEST_CASE("BmsTelemetry battery_temperature_c validation", "[BmsTypes]") {
  BmsTelemetry t{};

  SECTION("default is invalid") { REQUIRE_FALSE(t.is_battery_temperature_valid()); }

  SECTION("valid temperature") {
    t.battery_temperature_c = 25;
    REQUIRE(t.is_battery_temperature_valid());
  }

  SECTION("zero is valid") {
    t.battery_temperature_c = 0;
    REQUIRE(t.is_battery_temperature_valid());
  }

  SECTION("negative valid temperature") {
    t.battery_temperature_c = -20;
    REQUIRE(t.is_battery_temperature_valid());
  }
}

// ---------------------------------------------------------------------------
// Inline to-string helpers
// ---------------------------------------------------------------------------

TEST_CASE("bms_charging_state_str returns non-null for all values", "[BmsTypes]") {
  // Every known enum value should return a specific string (not "?")
  REQUIRE(std::string(bms_charging_state_str(BmsChargingState::Unknown)) == "Unknown");
  REQUIRE(std::string(bms_charging_state_str(BmsChargingState::NotCharging)) == "NotCharging");
  REQUIRE(std::string(bms_charging_state_str(BmsChargingState::TrickleCharge)) == "TrickleCharge");
  REQUIRE(std::string(bms_charging_state_str(BmsChargingState::PreCharge)) == "PreCharge");
  REQUIRE(std::string(bms_charging_state_str(BmsChargingState::FastCharge)) == "FastCharge");
  REQUIRE(std::string(bms_charging_state_str(BmsChargingState::TaperCharge)) == "TaperCharge");
  REQUIRE(std::string(bms_charging_state_str(BmsChargingState::TopOffTimerActiveCharging)) ==
          "TopOff");
  REQUIRE(std::string(bms_charging_state_str(BmsChargingState::ChargeTerminationDone)) == "Done");
}

TEST_CASE("bms_power_source_str returns non-null for all values", "[BmsTypes]") {
  REQUIRE(std::string(bms_power_source_str(BmsPowerSource::Unknown)) == "Unknown");
  REQUIRE(std::string(bms_power_source_str(BmsPowerSource::None)) == "None");
  REQUIRE(std::string(bms_power_source_str(BmsPowerSource::UsbSdp)) == "USB_SDP");
  REQUIRE(std::string(bms_power_source_str(BmsPowerSource::UsbCdp)) == "USB_CDP");
  REQUIRE(std::string(bms_power_source_str(BmsPowerSource::UsbDcp)) == "USB_DCP");
  REQUIRE(std::string(bms_power_source_str(BmsPowerSource::UnknownAdapter)) == "UnknownAdapter");
  REQUIRE(std::string(bms_power_source_str(BmsPowerSource::NonStandard)) == "NonStandard");
  REQUIRE(std::string(bms_power_source_str(BmsPowerSource::OtgMode)) == "OTG");
}

// ---------------------------------------------------------------------------
// Fuel-gauge protection configuration
// ---------------------------------------------------------------------------

TEST_CASE("FgProtectionConfig equality compares every field", "[BmsTypes]") {
  const FgProtectionConfig a{4250, 4150, 2700, 2900, 450, 400, 600, 550};
  FgProtectionConfig b = a;
  REQUIRE(a == b);
  REQUIRE_FALSE(a != b);

  SECTION("threshold differs") {
    b.uv_prot_threshold_mv = 2800;
    REQUIRE(a != b);
  }
  SECTION("recovery differs") {
    b.ot_chg_recovery_dc = 500;
    REQUIRE(a != b);
  }
  SECTION("last field differs") {
    b.ot_dsg_recovery_dc = 549;
    REQUIRE(a != b);
  }
}

TEST_CASE("fg_protector_checksum is the plain sum the gauge checks", "[BmsTypes]") {
  REQUIRE(fg_protector_checksum(0x0A, 0x07) == 0x0011); // TI factory default
  REQUIRE(fg_protector_checksum(0x0A, 0x00) == 0x000A); // OVP code 000 on factory OC
  REQUIRE(fg_protector_checksum(0xFF, 0xFF) == 0x01FE); // no 8-bit wrap
}

TEST_CASE("fg_qmax_q14_to_mah converts the BQ27427 fixed-point Qmax", "[BmsTypes]") {
  // Captured from a real device: 17211 * 2000 / 16384 = 2100 mAh.
  REQUIRE(fg_qmax_q14_to_mah(17211, 2000) == 2100);
  REQUIRE(fg_qmax_q14_to_mah(16384, 2000) == 2000); // exactly design capacity
  REQUIRE(fg_qmax_q14_to_mah(0, 2000) == 0);
  // Widest in-range inputs: the 32-bit intermediate holds, and the result still
  // fits the uint16_t return (max Design Capacity is 14500 mAh, TRM Table 5-5).
  REQUIRE(fg_qmax_q14_to_mah(65535, 14500) == 57999);
}

TEST_CASE("FgLearningProgress defaults to nothing learned", "[BmsTypes]") {
  FgLearningProgress p{};
  REQUIRE_FALSE(p.qmax_updated);
  REQUIRE_FALSE(p.ra_updated);
  REQUIRE_FALSE(p.ocv_taken);
}

namespace {

/// An image as it comes off a unit that finished a learning cycle: R_a0 is the
/// profile in use, R_a0x holds the alternate.
FgGoldenImage learned_image() {
  FgGoldenImage img{};
  img.qmax_mah = 2543;
  img.update_status = FG_GOLDEN_UPDATE_STATUS;
  img.ra0.flag = 0x0055; // enabled, resistances updated
  img.ra0x.flag = 0xFF00;
  for (size_t i = 0; i < FG_RA_TABLE_SIZE; ++i) {
    img.ra0.ra[i] = static_cast<int16_t>(300 + i);
    img.ra0x.ra[i] = static_cast<int16_t>(290 + i);
  }
  return img;
}

} // namespace

TEST_CASE("an empty golden image marks a characterisation build", "[BmsTypes][golden]") {
  REQUIRE_FALSE(fg_golden_image_valid(FgGoldenImage{}));
}

TEST_CASE("fg_golden_image_valid accepts a finished capture", "[BmsTypes][golden]") {
  REQUIRE(fg_golden_image_valid(learned_image()));

  SECTION("either profile may be the enabled one") {
    FgGoldenImage img = learned_image();
    std::swap(img.ra0, img.ra0x);
    REQUIRE(fg_golden_image_valid(img));
  }
}

TEST_CASE("fg_golden_image_valid rejects an unusable capture", "[BmsTypes][golden]") {
  SECTION("no Qmax") {
    FgGoldenImage img = learned_image();
    img.qmax_mah = 0;
    REQUIRE_FALSE(fg_golden_image_valid(img));
  }
  SECTION("Qmax past the part's ceiling") {
    FgGoldenImage img = learned_image();
    img.qmax_mah = FG_QMAX_CELL0_MAX_MAH + 1;
    REQUIRE_FALSE(fg_golden_image_valid(img));
  }
  SECTION("Update Status still says Impedance Track is running") {
    FgGoldenImage img = learned_image();
    img.update_status = 0x06; // what the gauge that learned it reads
    REQUIRE_FALSE(fg_golden_image_valid(img));
  }
  SECTION("neither profile is enabled") {
    FgGoldenImage img = learned_image();
    img.ra0.flag = 0xFFFF;
    REQUIRE_FALSE(fg_golden_image_valid(img));
  }
  SECTION("both profiles claim to be enabled") {
    FgGoldenImage img = learned_image();
    img.ra0x.flag = 0x0055;
    REQUIRE_FALSE(fg_golden_image_valid(img));
  }
  SECTION("the enabled profile holds a dead resistance") {
    FgGoldenImage img = learned_image();
    img.ra0.ra[7] = 0;
    REQUIRE_FALSE(fg_golden_image_valid(img));
  }
  SECTION("a dead resistance in the idle profile is not a reason to refuse") {
    FgGoldenImage img = learned_image();
    img.ra0x.ra[7] = 0;
    REQUIRE(fg_golden_image_valid(img));
  }
}
