/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_BLE_DEVICE_INFORMATION_H
#define AG_BLE_DEVICE_INFORMATION_H

#include <cstdint>

class AgBleServer;

struct DeviceInformation {
  const char *model = nullptr;
  const char *serial = nullptr;
  const char *firmware_version = nullptr;
  const char *manufacturer = nullptr;
};

// Register the standard Device Information Service on an initialized server,
// before advertising. Null fields become empty values; read_properties applies
// to all four characteristics. Values are copied during registration.
// Returns false on failure; stack cleanup remains the caller's responsibility.
bool register_device_information(AgBleServer &server, const DeviceInformation &info,
                                 uint16_t read_properties);

#endif // AG_BLE_DEVICE_INFORMATION_H
