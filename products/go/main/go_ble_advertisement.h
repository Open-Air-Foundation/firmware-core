/**
 * AirGradient Go — Offline BLE measurement payload
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

#include "measures_types.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace GoBleAdvertisement {

inline constexpr const char *SERVICE_UUID = "992f1fe6-4519-4d4f-8ca7-546d36126ee8";
inline constexpr uint8_t VERSION = 1;
inline constexpr size_t PAYLOAD_SIZE = 13;

inline constexpr size_t VERSION_OFFSET = 0;
inline constexpr size_t TEMPERATURE_OFFSET = 1;
inline constexpr size_t HUMIDITY_OFFSET = 3;
inline constexpr size_t PM25_OFFSET = 4;
inline constexpr size_t CO2_OFFSET = 6;
inline constexpr size_t PRESSURE_OFFSET = 8;
inline constexpr size_t ALTITUDE_OFFSET = 10;
inline constexpr size_t BATTERY_OFFSET = 12;

inline constexpr uint8_t INVALID_U8 = 0xFF;
inline constexpr uint16_t INVALID_U16 = 0xFFFF;
inline constexpr uint16_t INVALID_I16 = 0x8000;

using Payload = std::array<uint8_t, PAYLOAD_SIZE>;

// Service Data body only; excludes the AD header and UUID. Multibyte fields
// are little-endian. Units: 0.01 C, 1 %RH, 0.1 ug/m3, 1 ppm, 0.1 hPa, 1 m,
// and 1 % battery. Altitude comes from pressure.altitude, not GPS.
// Round to nearest, ties away from zero; invalid or unencodable fields use
// their wire sentinel. The caller supplies battery in power.battery_percentage.
Payload encode(const MeasuresAGo &measures);

// Version 2 ships as Manufacturer Specific Data beside a Complete List of
// 128-bit Service UUIDs (SERVICE_UUID), so iOS can match it with a
// service-filtered scan while the phone is locked. The UUID list leaves room
// for only 9 payload bytes, so fields are bit-packed, least significant bit
// first across the bytes:
//   bits  0..2   version (2)
//   bits  3..13  temperature, (C + 40) * 10        0..2046, invalid 2047
//   bits 14..20  humidity, 1 %RH                   0..126,  invalid 127
//   bits 21..34  PM2.5, 0.1 ug/m3                  0..16382, invalid 16383
//   bits 35..48  CO2, 1 ppm                        0..16382, invalid 16383
//   bits 49..61  pressure, (hPa - 300) * 10        0..8190, invalid 8191
//   bits 62..68  battery, 1 %                      0..126,  invalid 127
//   bits 69..71  reserved, 0
// Barometric altitude is not sent; receivers derive it from pressure.
inline constexpr uint8_t VERSION_V2 = 2;
inline constexpr size_t PAYLOAD_V2_SIZE = 9;
// Bluetooth SIG's company ID reserved for testing. Replace with an assigned ID
// before release.
inline constexpr uint16_t COMPANY_ID = 0xFFFF;
inline constexpr size_t COMPANY_ID_SIZE = 2;
inline constexpr size_t MANUFACTURER_DATA_V2_SIZE = COMPANY_ID_SIZE + PAYLOAD_V2_SIZE;

using PayloadV2 = std::array<uint8_t, PAYLOAD_V2_SIZE>;
using ManufacturerDataV2 = std::array<uint8_t, MANUFACTURER_DATA_V2_SIZE>;

PayloadV2 encode_v2(const MeasuresAGo &measures);
// Company ID (little-endian) followed by the v2 payload.
ManufacturerDataV2 manufacturer_data_v2(const PayloadV2 &payload);

// Why an Offline fast-path wake did or did not broadcast. The console stops
// printing soon after GPS init on these wakes, so the report is kept in RTC
// memory and printed early on the next boot instead.
enum class WakeOutcome : uint8_t {
  Advertised,
  SkippedButton,
  SkippedSleepTooShort,
  SkippedShipMode,
  SkippedPromotion, // storage failure promoted before the display refresh
  SetupFailed,
  StopFailed,
};

enum class SetupStep : uint8_t { None, Init, Payload, Interval, Start };

struct WakeReport {
  uint32_t sequence = 0; // counts stored reports since the last power-on reset
  WakeOutcome outcome = WakeOutcome::SkippedPromotion;
  SetupStep failed_step = SetupStep::None;
  uint32_t advertising_started_ms = 0; // since boot; 0 when not advertised
  uint32_t advertising_duration_ms = 0;
  uint32_t awake_ms = 0;    // when the sleep decision was made
  uint8_t payload_size = 0; // bytes of payload that were broadcast
  Payload payload{};
};

// Stores the report for the next boot and assigns its sequence number.
void store_wake_report(WakeReport report);
// Returns the stored report once, then clears it.
bool take_wake_report(WakeReport &out);
// Writes a single-line description, e.g. "seq=3 outcome=advertised ...".
void format_wake_report(const WakeReport &report, char *buffer, size_t size);

} // namespace GoBleAdvertisement
