/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BLE_SERVER_H
#define BLE_SERVER_H

#include "ble_types.h"

#include <cstddef>
#include <cstdint>

// Abstract BLE characteristic. Returned by AgBleGattService::add_characteristic().
// Lifetime: valid until AgBleServer::deinit() is called.
//
// ISR-safe: no
// Thread-safe: set_value() and notify() may be called from any application
//   task (not only the BLE host task) — the driver takes the host lock for
//   these. A single characteristic must still not be driven concurrently from
//   two tasks at once.
// Blocking: no (except initial stack operations during init)
// Allocates: no (operates on pre-allocated internal buffers)
class AgBleCharacteristic {
public:
  virtual ~AgBleCharacteristic() = default;

  // Updates the characteristic value. Returns false if the characteristic is
  // invalid or the data could not be stored.
  virtual bool set_value(const uint8_t *data, size_t len) = 0;

  // Sends a notification to all subscribed clients using the stored value.
  // Returns false if there are no active subscribers or if the characteristic
  // does not have the NOTIFY property.
  virtual bool notify() = 0;

  // Sends a notification carrying the supplied buffer, without modifying the
  // stored (READ) value. Use this to decouple the NOTIFY payload from the
  // READ snapshot. Returns false if there are no active subscribers or if the
  // characteristic does not have the NOTIFY property.
  virtual bool notify(const uint8_t *data, size_t len) = 0;

  // Registers a callback invoked when a client writes to this characteristic.
  // The callback is stored by value; it must remain valid for the lifetime
  // of this characteristic.
  virtual void set_write_callback(AgBleWriteCallback callback) = 0;
};

// Abstract BLE service. Returned by AgBleServer::add_service().
// Lifetime: valid until AgBleServer::deinit() is called.
//
// ISR-safe: no
// Thread-safe: no
class AgBleGattService {
public:
  virtual ~AgBleGattService() = default;

  // Creates and returns a characteristic for this service. Returns nullptr
  // on failure. The returned pointer is non-owning; lifetime is tied to
  // AgBleServer. properties is a bitfield of AgBleProperty flags.
  // Must be called before AgBleGattService::start().
  virtual AgBleCharacteristic *add_characteristic(const char *uuid, uint16_t properties) = 0;

  // Registers the service and its characteristics with the GATT database.
  // Must be called after all characteristics have been added and before
  // AgBleServer::start_advertising(). Returns false on registration failure.
  virtual bool start() = 0;
};

// Abstract BLE peripheral server. Manages the GATT server lifecycle,
// service/characteristic registration, advertising, and security.
//
// Typical call sequence:
//   init() -> [set_security()] -> add_service() -> [add_characteristic()] ->
//   start() -> [set_passkey_display_callback()] -> [set_auth_complete_callback()]
//   -> [set_connect_callback()] -> [set_disconnect_callback()] ->
//   [set_advertising_name()] -> [add_advertised_service_uuid()] ->
//   [set_manufacturer_data()] -> start_advertising()
//
// ISR-safe: no
// Thread-safe: no
// Allocates: yes (init() allocates the BLE stack heap)
class AgBleServer {
public:
  virtual ~AgBleServer() = default;

  // Initialises the BLE stack with the given device name. Must be called
  // before any other method. Blocking during stack initialisation.
  // Returns false if the stack could not be initialised.
  virtual bool init(const char *device_name) = 0;

  // Stops advertising, tears down the GATT server, and releases the BLE
  // stack heap. Active connections are disconnected as part of teardown;
  // application callbacks are not invoked during or after this call.
  // Safe to call multiple times.
  virtual void deinit() = 0;

  // Configures BLE security parameters for future SMP procedures. io_cap
  // selects the pairing IO model; auth_flags is a bitfield of AgBleAuth flags
  // (BOND, MITM, SC). May be called after init(), including while advertising
  // and clients are connected; it must not rebuild GATT or disconnect peers.
  // Returns false if the server is not initialised.
  virtual bool set_security(AgBleIoCapability io_cap, uint8_t auth_flags) = 0;

  // Deletes all stored bond information. Useful for factory reset or
  // development. Returns false on failure.
  virtual bool delete_all_bonds() = 0;

  // Creates and returns a service. Returns nullptr on failure. The returned
  // pointer is non-owning; lifetime is tied to this AgBleServer.
  // Must be called after init() and before start_advertising().
  virtual AgBleGattService *add_service(const char *uuid) = 0;

  // Sets the advertised local name. Must be called after init() and before
  // start_advertising(). Pass nullptr to omit the local name field.
  virtual bool set_advertising_name(const char *name) = 0;

  // Adds a service UUID to the advertised payload. May be called multiple
  // times to advertise multiple services. Must be called after init() and
  // before start_advertising().
  virtual bool add_advertised_service_uuid(const char *uuid) = 0;

  // Sets the manufacturer-specific data in the advertising payload.
  // The data buffer must include the 2-byte company ID prefix followed by
  // the payload bytes. Must be called after init() and before
  // start_advertising(). Returns false if the data could not be set.
  virtual bool set_manufacturer_data(const uint8_t *data, size_t len) = 0;

  // Starts the GATT server and begins advertising. Should be called after all
  // services and characteristics have been added and started.
  virtual bool start_advertising() = 0;

  // Requests a connection-parameter update on the active connection(s). This is
  // a hint to the central, which may accept, clamp, or reject it; the interval
  // is re-negotiable mid-connection so the switch is glitch-free. Intervals and
  // the supervision timeout are expressed in milliseconds (the implementation
  // converts to the BLE 1.25 ms / 10 ms units). Returns false if there is no
  // active connection or under TEST_HOST. Default no-op so servers that do not
  // support it (or test doubles) need not implement it.
  virtual bool request_conn_params(uint16_t min_interval_ms, uint16_t max_interval_ms,
                                   uint16_t latency, uint16_t supervision_timeout_ms) {
    (void)min_interval_ms;
    (void)max_interval_ms;
    (void)latency;
    (void)supervision_timeout_ms;
    return false;
  }

  // Stops advertising without disconnecting active clients.
  virtual bool stop_advertising() = 0;

  // Registers a callback invoked when a client connects. Stored by value.
  // Must be set before start_advertising() to guarantee delivery of all
  // connection events.
  virtual void set_connect_callback(AgBleConnectCallback callback) = 0;

  // Registers a callback invoked when a client disconnects. Stored by value.
  // Must be set before start_advertising() to guarantee delivery of all
  // disconnection events.
  virtual void set_disconnect_callback(AgBleDisconnectCallback callback) = 0;

  // Registers a callback invoked when the stack needs the device to display
  // a passkey during pairing. The driver generates a random 6-digit passkey
  // and passes it to the callback. Stored by value. Must be set before
  // start_advertising() to guarantee delivery.
  virtual void set_passkey_display_callback(AgBlePasskeyDisplayCallback callback) = 0;

  // Registers the explicit Numeric Comparison request callback. The caller
  // must subsequently call confirm_numeric_comparison() with the same handle.
  // Until then, the driver must leave the SMP request unanswered.
  virtual void set_numeric_comparison_callback(AgBleNumericComparisonCallback callback) {
    (void)callback;
  }

  // Accepts or rejects a pending Numeric Comparison request for conn_handle.
  // Returns false when there is no matching pending request or injection fails.
  virtual bool confirm_numeric_comparison(uint16_t conn_handle, bool accept) {
    (void)conn_handle;
    (void)accept;
    return false;
  }

  // Registers a callback invoked when pairing/authentication completes.
  // Stored by value. Must be set before start_advertising() to guarantee
  // delivery.
  virtual void set_auth_complete_callback(AgBleAuthCompleteCallback callback) = 0;

  // Returns the current number of connected clients. Default 0 for test
  // doubles that do not model connection state.
  virtual uint8_t connected_client_count() const { return 0; }

  // True when any active link is authenticated (MITM-paired). Reflects the
  // stack's live security state rather than a one-shot pairing event, so it
  // cannot get stuck after a bonded reconnect. Default false for test doubles.
  // Thread-safe: may be called from any task; the driver takes the host lock.
  virtual bool is_peer_authenticated() const { return false; }
};

#endif // BLE_SERVER_H
