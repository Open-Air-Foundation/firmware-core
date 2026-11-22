# Provisioning BLE Transport

GATT services, pairing configuration, and status codes for provisioning clients
and firmware integrators. See [lifecycle](lifecycle.md) for server ownership and
attached-session rules, and the [component README](../README.md) for setup.

## BLE Transport

The BLE transport creates two GATT services on the borrowed
`AgBleServer`:

**AirGradient Provisioning Service** (`acbcfea8-e541-4c40-9bfd-17820f16c95c`):

| Characteristic | UUID | Properties |
|---|---|---|
| Credentials/Status | `703fa252-3d2a-4da9-a05c-83b0d9cacb8e` | READ, READ_ENC, WRITE, WRITE_ENC, NOTIFY |
| Wi-Fi Scan | `467a080f-e50f-42c9-b9b2-a2ab14d82725` | WRITE, WRITE_ENC, NOTIFY |

**Device Information Service** (`180A`):

| Characteristic | UUID | Properties |
|---|---|---|
| Model Number | `2A24` | READ, READ_ENC |
| Serial Number | `2A25` | READ, READ_ENC |
| Firmware Revision | `2A26` | READ, READ_ENC |
| Manufacturer Name | `2A29` | READ, READ_ENC |

BLE security is configurable through `ProvisioningBleConfig`:

- `io_capability` (default `AgBleIoCapability::NO_INPUT_NO_OUTPUT`)
- `auth_flags` (default `AgBleAuth::BOND | AgBleAuth::SC`)

The defaults preserve the reference product's Just Works + bonded
Secure Connections behaviour. Products that need a different security
profile override `auth_flags` and `io_capability` on the config struct
before calling `ProvisioningManager::start()`. Product-specific pairing choices
belong to the product documentation; attached sessions inherit the security of their existing BLE server.

Standalone advertising stops on client connect and restarts on disconnect.
Attached sessions leave advertising and connection callbacks to the owner.

Scan results are sent as paginated JSON notifications; see
[Timing and Teardown](#timing-and-teardown) for pacing.

## BLE Status Codes

The provisioning manager automatically sends these on Wi-Fi state
changes:

| Code | Name | Meaning |
|---|---|---|
| `0` | `WIFI_CONNECTED` | Wi-Fi connected |
| `10` | `WIFI_CONNECT_FAILED` | Failed to connect |
| `14` | `CREDENTIALS_NOT_SAVED` | Connected, but the credential could not be persisted (will not survive a reboot) |

Products send application-level codes via `send_ble_status()`:

| Code | Name | Meaning |
|---|---|---|
| `1` | `CONNECTING_TO_SERVER` | Connecting to AirGradient server |
| `2` | `SERVER_REACHABLE` | AirGradient server is reachable |
| `3` | `MONITOR_CONFIGURED` | Monitor is configured on dashboard |
| `11` | `SERVER_UNREACHABLE` | AirGradient server is unreachable |
| `12` | `GET_CONFIG_FAILED` | Failed to get monitor config |
| `13` | `NOT_REGISTERED` | Monitor not registered on dashboard |

All codes are defined as named constants in `ProvisioningBleStatus`.

## Implementation and Verification

The [BLE transport](../internal/ble_transport.cpp) owns UUIDs, characteristic
setup, and scan pagination. The [shared JSON parser](../internal/provisioning_json.cpp)
handles credential validation. [Component tests](../README.md#tests) exercise
transport callbacks, JSON parsing, and notifications.

## Timing and Teardown

- BLE scan pagination uses `ProvisioningTimer` (wraps `esp_timer` on
  firmware, no-op with `fire_for_test()` under `TEST_HOST`). Pages are
  sent at ~100 ms intervals to avoid overwhelming the BLE stack.
- The provisioning manager clears BLE transport callbacks before
  calling `teardown()` to prevent the disconnect triggered by
  `AgBleServer::deinit()` from calling back into the manager while the
  mutex is held.
