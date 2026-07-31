/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BLE_TYPES_H
#define BLE_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>

// BLE characteristic property flags. Combine with | to set multiple properties.
// The lower byte covers standard GATT properties; the upper byte covers
// security requirements for individual characteristics.
namespace AgBleProperty {
inline constexpr uint16_t READ = 0x0001;
inline constexpr uint16_t WRITE = 0x0002;
inline constexpr uint16_t WRITE_NR = 0x0004; // write with no response
inline constexpr uint16_t NOTIFY = 0x0008;
inline constexpr uint16_t INDICATE = 0x0010;
inline constexpr uint16_t READ_ENC = 0x0020;     // read requires encryption
inline constexpr uint16_t READ_AUTHEN = 0x0040;  // read requires authentication
inline constexpr uint16_t WRITE_ENC = 0x0080;    // write requires encryption
inline constexpr uint16_t WRITE_AUTHEN = 0x0100; // write requires authentication
} // namespace AgBleProperty

// BLE IO capability for pairing. Determines how the user can interact
// with the device during the pairing process.
enum class AgBleIoCapability : uint8_t {
  DISPLAY_ONLY,       // device can display a passkey
  DISPLAY_YES_NO,     // device can display; user can confirm yes/no
  KEYBOARD_ONLY,      // user can enter a passkey
  NO_INPUT_NO_OUTPUT, // no user interaction (Just Works)
  KEYBOARD_DISPLAY,   // device can display; user can also enter a passkey
};

// BLE security authentication flags. Combine with | to set multiple flags.
namespace AgBleAuth {
inline constexpr uint8_t BOND = 0x01; // persist pairing keys across power cycles
inline constexpr uint8_t MITM = 0x02; // man-in-the-middle protection
inline constexpr uint8_t SC = 0x04;   // LE Secure Connections
} // namespace AgBleAuth

// Invoked when a client connects. conn_handle identifies the connection.
using AgBleConnectCallback = std::function<void(uint16_t conn_handle)>;

// Invoked when a client disconnects. reason is a BLE stack error code.
using AgBleDisconnectCallback = std::function<void(uint16_t conn_handle, int reason)>;

// Invoked when a client writes to a characteristic.
// data and len describe the written payload. The buffer is only valid for the
// duration of the callback; callers must copy if they need to retain the data.
using AgBleWriteCallback = std::function<void(const uint8_t *data, size_t len)>;

// Invoked when the BLE stack needs the device to display a passkey during
// pairing. passkey is a 6-digit numeric code (000000–999999) that must be
// shown to the user so they can enter it on the peer device.
using AgBlePasskeyDisplayCallback = std::function<void(uint32_t passkey)>;

// Invoked when LE Secure Connections Numeric Comparison requires an explicit
// local accept or reject decision. The connection handle identifies the
// pending pairing procedure; the driver must not accept by default.
using AgBleNumericComparisonCallback = std::function<void(uint16_t conn_handle, uint32_t number)>;

// Invoked when the pairing/authentication procedure completes.
// success is true when the link is encrypted (and authenticated if MITM was
// requested).
using AgBleAuthCompleteCallback = std::function<void(uint16_t conn_handle, bool success)>;

#endif // BLE_TYPES_H
