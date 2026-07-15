/**
 * AirGradient Go — BLE Service
 *
 * BLE peripheral service exposing sensor measurements, device status,
 * configuration, and stored route data to a connected phone app over a
 * single custom GATT service.  Active only in Portable operating mode.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#pragma once

// ---------------------------------------------------------------------------
// Event types required in go_events.h for compilation:
//
//   EventType::BleConnected        — no payload
//   EventType::BleDisconnected     — no payload
//   EventType::BleConfigWrite      — no payload (data in BleService pending buffer)
//   EventType::BleHistoryWrite     — no payload (data in BleService pending buffer)
//   EventType::BlePairingRequest   — payload: uint32_t ble_passkey
//
// Add to the Event union:
//   uint32_t ble_passkey;  // BlePairingRequest
// ---------------------------------------------------------------------------

#include "go_power.h"
#include "go_settings.h"
#include "go_types.h"
#include "hal/ble_server.h"
#include "measures_types.h"
#include "rtos.h"
#include "gps/gps_types.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

class StorageService; // forward declaration

// ---------------------------------------------------------------------------
// BLE advertised name
// ---------------------------------------------------------------------------

// "AirGradient Go <last4hex>" derived from the device serial (falls back to
// "0000"). Shared by BleService (Portable) and WifiService provisioning
// (Stationary) so both flows advertise the same name.
inline constexpr const char *BLE_ADV_NAME_PREFIX = "AirGradient Go ";
inline constexpr size_t BLE_ADV_NAME_SUFFIX_LEN = 4;
inline constexpr size_t BLE_ADV_NAME_BUF_SIZE = 24; // prefix + suffix + NUL + margin

inline void compute_ble_adv_name(const char *serial, char *out, size_t buf_size) {
  const char *suffix = "0000";
  const size_t serial_len = serial != nullptr ? std::strlen(serial) : 0;
  if (serial_len >= BLE_ADV_NAME_SUFFIX_LEN) {
    suffix = serial + (serial_len - BLE_ADV_NAME_SUFFIX_LEN);
  }
  std::snprintf(out, buf_size, "%s%s", BLE_ADV_NAME_PREFIX, suffix);
}

// ---------------------------------------------------------------------------
// CBOR decode result types (used by orchestrator after take_pending_*())
// ---------------------------------------------------------------------------

/// Operation type for a Config characteristic write.
enum class BleConfigOp : uint8_t {
  Set,     ///< Config update — changed fields merged into GoSettings
  Command, ///< Command execution — cmd in BleConfigDecodeResult::cmd
  Invalid, ///< Decode failed or unknown op
};

/// BLE command identifiers (decoded from the "cmd" CBOR string).
enum class BleCommand : uint8_t {
  Co2Calibration, ///< "co2_cal"  — trigger CO2 background calibration
  ClearData,      ///< "clear_data" — erase all stored route data
  FactoryReset,   ///< "factory_rst" — reset settings to defaults
  StartTracking,  ///< "start_tracking" — begin GPS + sensor route logging
  StopTracking,   ///< "stop_tracking"  — end route logging
  SetAiding,      ///< "set_aiding" — inject A-GNSS aiding data
  Set,            ///< Synthetic — used for config-set error notifications
  Unknown,        ///< Unrecognised command string
};

/// Reason the device is about to drop the BLE link, carried by the NOTIFY-only
/// Status `disc` delta so the client can treat the imminent disconnect as
/// expected rather than an error.
enum class BleDiscReason : uint8_t {
  Overheat,     ///< Over-temperature safety shutdown
  LowBatt,      ///< Over-discharge safety shutdown
  User,         ///< User-initiated (long-press) shutdown
  OpStationary, ///< Operating mode changing to Stationary
  OpOffline,    ///< Operating mode changing to Offline
};

/// Result of decoding a Config characteristic write.
struct BleConfigDecodeResult {
  BleConfigOp op = BleConfigOp::Invalid;
  BleCommand cmd = BleCommand::Unknown;   ///< Valid when op == Command
  bool has_unknown_keys = false;          ///< True if any unrecognized config key was present
  size_t recognized_config_key_count = 0; ///< Count of recognized config-key occurrences in a "set"
  GpsAidingData aiding;                   ///< Valid when cmd == SetAiding
};

/// Operation type for a History characteristic write.
enum class BleHistoryOp : uint8_t {
  List,    ///< Request session list
  Start,   ///< Start download — session_id is set
  Fill,    ///< Retransmit points — point_indices[] and point_count are set
  End,     ///< End download
  Delete,  ///< Delete a single session — session_id is set
  Invalid, ///< Decode failed or unknown op
};

/// Raw thermal-calibration telemetry for the Cal characteristic.  One sample
/// per measurement cycle, assembled by the orchestrator from the freshest
/// data it already holds (no extra sensor reads).  Invalid-sentinel fields
/// are omitted from the encoded CBOR map.
struct BleCalTelemetry {
  TempHumData sht;              ///< SHT40 temp/hum (external stem)
  TempHumData dps;              ///< DPS368 temp (main PCB; humidity invalid)
  float pressure_hpa = MeasuresInvalid::PRESSURE;
  float t_fg_c = BmsInvalid::FG_TEMP_C;                 ///< fuel-gauge internal temp
  int16_t t_die_c = BmsInvalid::TEMPERATURE_C;          ///< BQ25629 die temp
  int16_t t_bat_c = BmsInvalid::TEMPERATURE_C;          ///< battery NTC temp
  int16_t ibat_fg_ma = BmsInvalid::CURRENT_MA;          ///< FG battery current (+charge)
  int16_t ibat_bms_ma = BmsInvalid::CURRENT_MA;         ///< charger battery current
  int16_t ibus_ma = BmsInvalid::CURRENT_MA;             ///< input/VBUS current
  float vbus = BmsInvalid::VOLT;
  float vbat = BmsInvalid::VOLT;
  BmsChargingState charging = BmsChargingState::Unknown;
  bool gps_active = false;
  uint32_t uptime_s = 0;
};

/// Result of decoding a History characteristic write.
struct BleHistoryDecodeResult {
  BleHistoryOp op = BleHistoryOp::Invalid;
  uint32_t session_id = 0; ///< Valid when op == Start or Delete

  static constexpr size_t MAX_FILL_POINTS = 50;
  uint32_t point_indices[MAX_FILL_POINTS] = {};
  size_t point_count = 0; ///< Valid when op == Fill
};

class BleService {
public:
  // --- Construction ---

  /// event_queue: shared orchestrator event queue (for posting BLE events)
  /// storage:     storage service reference (for history export reads)
  /// ble_server:  borrowed BLE server shared with Stationary provisioning.
  ///              The orchestrator enforces mutual exclusion: only one
  ///              owner (BleService or ProvisioningManager) drives the
  ///              server at a time across mode transitions.
  BleService(RtosQueueHandle event_queue, StorageService &storage, AgBleServer &ble_server);

  // --- Lifecycle (called by orchestrator) ---

  /// Convenience wrapper: init_stack_and_register() + start_advertising().
  /// serial: 12-char lowercase hex device serial; the BLE name uses its last
  ///         four chars (e.g. "AirGradient Go ef0e"). Returns false on failure.
  bool init(const char *serial);

  /// Phase 1: init the stack, set security, register the AGo data service +
  /// chars and connection callbacks; store the advertised name. Does NOT
  /// advertise, so extra GATT services (prov + DIS) can be slotted in before
  /// start_advertising(). Returns false on failure.
  bool init_stack_and_register(const char *serial);

  /// Phase 3: set the advertised name + service UUID and start advertising.
  /// Marks the service initialized. Returns false if advertising fails.
  bool start_advertising();

  /// Stop advertising, disconnect clients, tear down BLE stack.
  /// Safe to call when not initialized (no-op).
  void deinit();

  /// Register an optional disconnect observer fanned out from
  /// on_disconnect() (NimBLE host-task context) BEFORE the existing
  /// handling (advertising restart / BleDisconnected post).  Used by the
  /// orchestrator to forward the disconnect to OtaService so an in-flight
  /// BLE OTA transfer aborts synchronously.  Pass nullptr to clear.
  /// BleService stays the sole owner of the server's single disconnect slot.
  void set_disconnect_observer(AgBleDisconnectCallback cb);

  // --- Data output (called by orchestrator in orchestrator task context) ---

  /// Encode measures + GPS as CBOR and update the characteristic value.
  /// Always sets the value for READ access. Additionally sends a notification
  /// when a client is connected. Call on every SensorDataReady regardless of
  /// connection state so the characteristic always holds the latest data.
  void notify_measures(const MeasuresAGo &measures, const GpsData &gps, time_t timestamp);

  /// Encode a Cal telemetry sample as CBOR and update the characteristic
  /// value. Always sets the value for READ access; additionally notifies when
  /// a client is connected. Call on every SensorDataReady — the Cal
  /// characteristic is the raw-data stream for thermal-calibration runs.
  void notify_cal(const BleCalTelemetry &cal, time_t timestamp);

  /// Set the Status characteristic value (no notify). Use for steady-state
  /// refreshes (BMS poll, GPS fix, history delete) — clients see it on
  /// the next Read.
  void update_status(const PowerSnapshot &power, const GpsData &gps, bool tracking_active,
                     uint32_t session_id);

  /// Refresh the full Status snapshot AND push a `{tracking, session}` delta
  /// NOTIFY. Use on urgent tracking transitions (start success, start failure,
  /// manual stop) so the client need not poll.
  void notify_tracking_status(const PowerSnapshot &power, const GpsData &gps, bool tracking_active,
                              uint32_t session_id);

  /// Refresh the full Status snapshot AND push a `{charging, bat_pct, bat_v}`
  /// delta NOTIFY. Use on charging transitions (plug in, unplug, charge
  /// complete) so clients reflect the change without polling. Delta keys are
  /// disjoint from notify_tracking_status()'s, so clients merge either shape by
  /// key.
  void notify_charging_status(const PowerSnapshot &power, const GpsData &gps, bool tracking_active,
                              uint32_t session_id);

  /// Push a NOTIFY-only `{disc}` Status delta announcing that the device is
  /// about to drop the BLE link, and why (shutdown or operating-mode change).
  /// Best-effort and fire-and-forget — the caller must leave a brief settle
  /// window before tearing down the link / cutting power. The key is never part
  /// of the Status READ snapshot.
  void notify_disconnect(BleDiscReason reason);

  /// Update the readable Config characteristic value with current settings.
  /// Sole writer of the stored Config snapshot (READ). Called after settings
  /// change from any source (steady-state value-only refresh).
  void update_config(const GoSettings &settings);

  /// Refresh the stored Config snapshot (via update_config(cur)) and send a
  /// delta NOTIFY carrying only the fields that differ between @p prev and
  /// @p cur, plus a `"type":"config"` discriminator. The stored READ value is
  /// always the full snapshot; the notification never touches it.
  void notify_config(const GoSettings &prev, const GoSettings &cur);

  /// Send a command result notification on the Config characteristic.
  void notify_command_result(BleCommand cmd, bool success, const char *error = nullptr);

  /// Send a command progress notification on the Config characteristic.
  /// Indicates that a long-running command has been accepted and is in progress.
  /// Sent before the actual work begins; the final result arrives via
  /// notify_command_result() when the command completes.
  void notify_command_progress(BleCommand cmd);

  /// Delete all persisted BLE bond information.
  /// Returns true when BLE is not initialized or the driver reports success.
  bool delete_all_bonds();

  // --- Pending write data (called by orchestrator after BLE events) ---

  /// Retrieve the raw CBOR bytes from the last Config write.
  /// Returns the number of bytes copied to buf. Returns 0 if no pending data.
  /// Clears the pending flag after retrieval.
  size_t take_pending_config_write(uint8_t *buf, size_t buf_size);

  /// Retrieve the raw CBOR bytes from the last History write.
  /// Returns the number of bytes copied to buf. Returns 0 if no pending data.
  /// Clears the pending flag after retrieval.
  size_t take_pending_history_write(uint8_t *buf, size_t buf_size);

  // --- History download (called by orchestrator after decoding history command) ---

  /// Send session list as CBOR notification.
  void handle_history_list();

  /// Stream all points for a session as binary notifications. Blocks until
  /// complete. Sends CBOR "started" before and "done" after the stream.
  void handle_history_start(uint32_t session_id);

  /// Retransmit specific points by index. Sends binary data notifications
  /// for each requested point, followed by a CBOR "done".
  void handle_history_fill(const uint32_t *point_indices, size_t count);

  /// Close the current download session.
  void handle_history_end();

  /// Delete a single route session from NAND storage.
  /// Ends any active export for this session before deleting.
  /// Sends "deleted" on success, or "error" with appropriate error string.
  /// The caller (orchestrator) must check for active tracking session
  /// conflict before calling this method.
  void handle_history_delete(uint32_t session_id);

  /// Send a history error notification on the History characteristic.
  /// Convenience for the orchestrator to report errors it detects
  /// before delegating to handle_history_* methods.
  void notify_history_error(const char *err);

  // --- State queries ---

  bool is_initialized() const;
  /// True while a GAP link exists (not necessarily usable).
  bool is_connected() const;
  /// True while the active link is authenticated (MITM-paired) — the
  /// usable-link signal that drives the BLE icon. Reads the stack's live
  /// security state so it cannot get stuck after a bonded reconnect.
  bool is_authenticated() const;

  // --- CBOR decode helpers (called by orchestrator after take_pending_*()) ---

  /// Decode a raw Config characteristic write.
  /// If the operation is "set", changed fields are merged directly into @p settings.
  /// If the operation is "cmd", the command name is stored in the result.
  /// @param buf        Raw CBOR bytes from take_pending_config_write().
  /// @param len        Length of buf.
  /// @param settings   [in/out] Current settings — modified in-place for "set" ops.
  /// @return           Decode result with operation type.
  static BleConfigDecodeResult decode_config_write(const uint8_t *buf, size_t len,
                                                   GoSettings &settings);

  /// Decode a raw History characteristic write command.
  /// @param buf  Raw CBOR bytes from take_pending_history_write().
  /// @param len  Length of buf.
  /// @return     Decode result with operation type, session_id, or point indices.
  static BleHistoryDecodeResult decode_history_write(const uint8_t *buf, size_t len);

#ifdef TEST_HOST
  friend class BleServiceTestAccess;
#endif

private:
  RtosQueueHandle _event_queue;
  StorageService &_storage;

  // Borrowed: lifetime managed by the board.  Always non-null after ctor.
  // Tests may overwrite this pointer through BleServiceTestAccess.
  AgBleServer *_server = nullptr;
  // True between a successful init() and deinit().  Replaces the previous
  // `_server != nullptr` initialisation gate, which is no longer valid now
  // that the BLE server is borrowed for the lifetime of the service.
  bool _initialized = false;
  AgBleCharacteristic *_measures_char = nullptr;
  AgBleCharacteristic *_status_char = nullptr;
  AgBleCharacteristic *_config_char = nullptr;
  AgBleCharacteristic *_history_char = nullptr;
  AgBleCharacteristic *_cal_char = nullptr;

  // Advertised name; set in phase 1, used in phase 3 (start_advertising()).
  char _adv_name[BLE_ADV_NAME_BUF_SIZE] = {};

  std::atomic<bool> _connected{false};

  // Optional disconnect fan-out (OTA abort).  Invoked first in
  // on_disconnect(), synchronously on the NimBLE host task.  Nullable.
  AgBleDisconnectCallback _disconnect_observer;

  // --- Pending write buffers (written by NimBLE callbacks, read by orchestrator) ---

  static constexpr size_t WRITE_BUF_SIZE = 256;

  RtosMutex _config_write_mutex;
  uint8_t _config_write_buf[WRITE_BUF_SIZE] = {};
  size_t _config_write_len = 0;
  bool _config_write_pending = false;

  RtosMutex _history_write_mutex;
  uint8_t _history_write_buf[WRITE_BUF_SIZE] = {};
  size_t _history_write_len = 0;
  bool _history_write_pending = false;

  // --- History export state ---

  uint32_t _export_session_id = 0;
  bool _export_active = false;

  // --- Internal helpers ---

  void on_connect(uint16_t conn_handle);
  void on_disconnect(uint16_t conn_handle, int reason);
  void on_config_write(const uint8_t *data, size_t len);
  void on_history_write(const uint8_t *data, size_t len);
  void on_passkey_request(uint32_t passkey);

  /// CBOR encoding helpers (stack-allocated buffers, no heap)
  size_t encode_measures(uint8_t *buf, size_t buf_size, const MeasuresAGo &m, const GpsData &gps,
                         time_t ts);
  size_t encode_status(uint8_t *buf, size_t buf_size, const PowerSnapshot &power,
                       const GpsData &gps, bool tracking, uint32_t session_id);
  /// Encode a Cal telemetry sample (READ + NOTIFY form, omit-invalid).
  /// Returns 0 on encoder overflow.
  size_t encode_cal(uint8_t *buf, size_t buf_size, const BleCalTelemetry &cal, time_t ts);
  /// Encode the Status transition delta (`{tracking, session}`) for NOTIFY.
  /// Returns 0 on encoder overflow.
  size_t encode_status_transition(uint8_t *buf, size_t buf_size, bool tracking,
                                  uint32_t session_id);
  /// Encode the Status charging delta (`{charging, bat_pct, bat_v}`) for NOTIFY.
  /// Returns 0 on encoder overflow.
  size_t encode_status_charging(uint8_t *buf, size_t buf_size, const PowerSnapshot &power);
  /// Encode the Status disconnect-notice delta (`{disc}`) for NOTIFY.
  /// Returns 0 on encoder overflow.
  size_t encode_status_disc(uint8_t *buf, size_t buf_size, BleDiscReason reason);
  /// Encode the full Config snapshot (READ form): every field, no `"type"`.
  /// Returns 0 on encoder overflow.
  size_t encode_config(uint8_t *buf, size_t buf_size, const GoSettings &settings);
  /// Encode a Config delta (NOTIFY form): `"type":"config"` plus only the
  /// fields that differ between @p prev and @p cur. Returns 0 on overflow.
  size_t encode_config_delta(uint8_t *buf, size_t buf_size, const GoSettings &prev,
                             const GoSettings &cur);

  /// Send a CBOR control response on the History characteristic (tag 0x00).
  bool send_history_cbor(const uint8_t *cbor_data, size_t cbor_len);

  /// Send a binary data chunk on the History characteristic (tag 0x01).
  /// Retries with backpressure if the TX buffer is full.
  bool send_history_binary(uint16_t first_point_index, const uint8_t *data, size_t data_len);

  /// Convert a RoutePoint to the 55-byte RoutePointWire binary format.
  static void route_point_to_wire(const struct RoutePoint &point, uint8_t *out);

  /// Map BmsChargingState to CBOR text value.
  static const char *charging_state_to_str(BmsChargingState state);

  /// Map BleDiscReason to CBOR text value.
  static const char *disc_reason_to_str(BleDiscReason reason);

  /// Map GpsMode to CBOR text value.
  static const char *gps_mode_to_str(GpsMode mode);

  /// Map OperatingMode to CBOR text value.
  static const char *operating_mode_to_str(OperatingMode mode);

  /// Characteristic properties for the Measurements characteristic.
  static uint16_t measures_properties();

  /// Characteristic properties for the Status characteristic.
  static uint16_t status_properties();

  /// Characteristic properties for the Config characteristic.
  static uint16_t config_properties();

  /// Characteristic properties for the History characteristic.
  static uint16_t history_properties();

  /// Characteristic properties for the Cal characteristic.
  static uint16_t cal_properties();
};

// --- Integration Notes ---
//
// go_events.h:
//   Add EventType entries: BleConnected, BleDisconnected, BleConfigWrite,
//   BleHistoryWrite, BlePairingRequest, BleAuthComplete.
//   Add Event union members: uint32_t ble_passkey; bool ble_auth_ok;
//
// go_orchestrator.h/.cpp:
//   Add BleService member to Orchestrator::Services.
//   Add event dispatch handlers for all BLE event types.
//   Wire init()/deinit() into mode transitions (Portable enter/leave).
//   Forward sensor data via notify_measures() on SensorDataReady when BLE connected.
//
// go_storage.h/.cpp:
//   Add read methods: list_sessions(), get_session_point_count(),
//   read_route_points(), get_session_start_time().
//
// go_display.cpp / go_ui.cpp:
//   Add passkey display overlay for BlePairingRequest events.
//   BLE connected icon is already scaffolded.
//
// nand_storage.h:
//   Add total_capacity_kb() and used_kb() methods for Status characteristic
//   flash usage reporting.
//
// sdkconfig:
//   Full sdkconfig may need regeneration after adding NimBLE defaults.
//
// esp-nimble-cpp submodule:
//   Must be initialized: git submodule update --init
