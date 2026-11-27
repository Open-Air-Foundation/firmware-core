/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "ble_device_information.h"

#include <cstring>

#include "hal/ble_server.h"

namespace {

constexpr const char *DIS_SERVICE_UUID = "180A";
constexpr const char *DIS_MODEL_UUID = "2A24";
constexpr const char *DIS_SERIAL_UUID = "2A25";
constexpr const char *DIS_FIRMWARE_UUID = "2A26";
constexpr const char *DIS_MANUFACTURER_UUID = "2A29";

} // namespace

bool register_device_information(AgBleServer &server, const DeviceInformation &info,
                                 uint16_t read_properties) {
  auto *service = server.add_service(DIS_SERVICE_UUID);
  if (service == nullptr) {
    return false;
  }

  const struct {
    const char *uuid;
    const char *value;
  } fields[] = {
      {DIS_MODEL_UUID, info.model},
      {DIS_SERIAL_UUID, info.serial},
      {DIS_FIRMWARE_UUID, info.firmware_version},
      {DIS_MANUFACTURER_UUID, info.manufacturer},
  };
  for (const auto &field : fields) {
    auto *characteristic = service->add_characteristic(field.uuid, read_properties);
    const char *value = field.value != nullptr ? field.value : "";
    if (characteristic == nullptr ||
        !characteristic->set_value(reinterpret_cast<const uint8_t *>(value), std::strlen(value))) {
      return false;
    }
  }
  return service->start();
}
