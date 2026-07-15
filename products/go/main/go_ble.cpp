/**
 * AirGradient Go — BLE Service implementation
 *
 * GATT setup, CBOR encoding/decoding, NimBLE callbacks, and binary
 * history streaming.  Only init() is guarded with #ifndef TEST_HOST
 * (it instantiates the concrete NimbleBleServer). All other methods
 * use the abstract BleServer* interface and compile under host tests.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_ble.h"

#include "ag_log.h"
#include "go_ble_protocol.h"
#include "go_events.h"
#include "go_storage.h"

#ifndef TEST_HOST
#include "sdkconfig.h"
#endif
#include "rtos.h"

#include <cbor.h>

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <utility>
#include <vector>

static constexpr const char *TAG = "BLE";

// Forward declaration — defined in the CBOR decode helpers section.
static const char *ble_command_to_str(BleCommand cmd);

// ---------------------------------------------------------------------------
// GATT UUIDs
// ---------------------------------------------------------------------------

static constexpr const char *SERVICE_UUID = "d1c0c0a0-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *MEASURES_CHAR_UUID = "d1c0c0a1-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *STATUS_CHAR_UUID = "d1c0c0a2-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *CONFIG_CHAR_UUID = "d1c0c0a3-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *HISTORY_CHAR_UUID = "d1c0c0a4-6b48-4b2a-9b1d-59f9f2b0a1e1";
static constexpr const char *CAL_CHAR_UUID = "d1c0c0a5-6b48-4b2a-9b1d-59f9f2b0a1e1";

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

// Advertised name ("AirGradient Go <last4hex>") is computed by the shared
// compute_ble_adv_name() helper (go_ble.h) into BleService::_adv_name.

/// Minimum negotiated MTU below which notifications are suppressed.
static constexpr size_t MIN_USEFUL_MTU = 128;

/// CBOR encoding buffer size — Status, Measures, History, and Config deltas.
static constexpr size_t CBOR_BUF_SIZE = 256;

/// Full Config snapshot (READ) buffer — sized to the 512-byte ATT attribute
/// ceiling that Read-Long can serve, giving the snapshot room to grow.
static constexpr size_t CONFIG_SNAPSHOT_BUF_SIZE = 512;

/// Status delta buffer — fits both shapes: {tracking, session} and the larger
/// {charging, bat_pct, bat_v} (~39 B).
static constexpr size_t STATUS_DELTA_BUF_SIZE = 64;

// NOTIFY single-PDU budget note: every Config/Status notification is bounded at
// the source (single-field config delta, fixed command strings, 2-key status
// delta) so it fits a conservative ~180-byte budget (the 185-byte minimum
// negotiated MTU yields an MTU-3 = 182-byte PDU). This is a correctness
// invariant, enforced by host tests (TEST_NOTIFY_BUDGET in go_ble.tests.cpp),
// not a runtime gate: the device does not track the negotiated MTU, so it never
// drops a payload that a larger MTU could carry. If a payload ever exceeds the
// real negotiated MTU, NimBLE truncates the single PDU; the client detects the
// CBOR decode failure and re-READs the authoritative full snapshot.

/// RoutePointWire: packed binary format, 55 bytes per point.
static constexpr size_t ROUTE_POINT_WIRE_SIZE = 56;

/// History notification tag bytes.
static constexpr uint8_t HISTORY_TAG_CBOR = 0x00;
static constexpr uint8_t HISTORY_TAG_BINARY = 0x01;

/// Binary data notification header: tag (1) + point_index (2) = 3 bytes.
static constexpr size_t BINARY_HEADER_SIZE = 3;

/// Maximum ATT payload for a single notification (assumes 247-byte MTU).
/// Conservative estimate; actual limit depends on negotiated MTU.
static constexpr size_t MAX_NOTIFY_PAYLOAD = 244;

/// Maximum route points per binary notification.
static constexpr size_t POINTS_PER_NOTIFICATION =
    (MAX_NOTIFY_PAYLOAD - BINARY_HEADER_SIZE) / ROUTE_POINT_WIRE_SIZE;

/// Read batch size for route points from storage.
static constexpr uint16_t ROUTE_READ_BATCH = 4;

/// Backpressure retry delay when notify() returns false.
static constexpr uint32_t NOTIFY_RETRY_DELAY_MS = 1;

/// Sessions per page in paginated list response.
static constexpr uint16_t SESSIONS_PER_PAGE = 6;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

uint16_t BleService::measures_properties() {
  return AgBleProperty::READ | AgBleProperty::NOTIFY | AgBleProperty::READ_AUTHEN;
}

uint16_t BleService::status_properties() {
  // READ = steady-state polling; NOTIFY = urgent tracking transitions
  // (delivery best-effort; Read is authoritative on connect).
  return AgBleProperty::READ | AgBleProperty::NOTIFY | AgBleProperty::READ_AUTHEN;
}

uint16_t BleService::config_properties() {
  return AgBleProperty::READ | AgBleProperty::WRITE | AgBleProperty::NOTIFY |
         AgBleProperty::READ_AUTHEN | AgBleProperty::WRITE_AUTHEN;
}

uint16_t BleService::history_properties() {
  return AgBleProperty::WRITE | AgBleProperty::NOTIFY | AgBleProperty::WRITE_AUTHEN;
}

uint16_t BleService::cal_properties() {
  // Deliberately NOT authenticated: the calibration collector (and the
  // screenless reference board, which cannot display a passkey) must be able
  // to subscribe without pairing. Read-only raw telemetry, nothing sensitive.
  return AgBleProperty::READ | AgBleProperty::NOTIFY;
}

BleService::BleService(RtosQueueHandle event_queue, StorageService &storage,
                       AgBleServer &ble_server)
    : _event_queue(event_queue), _storage(storage), _server(&ble_server) {}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

#ifndef TEST_HOST

bool BleService::init(const char *serial) {
  if (!init_stack_and_register(serial)) {
    return false;
  }
  return start_advertising();
}

bool BleService::init_stack_and_register(const char *serial) {
  if (_initialized) {
    AG_LOGW(TAG, "already initialized");
    return true;
  }

  // Computed once; stored for start_advertising().
  compute_ble_adv_name(serial, _adv_name, sizeof(_adv_name));

  // BLE server is borrowed from the board; the orchestrator guarantees
  // mutual exclusion across operating modes.
  if (!_server->init(_adv_name)) {
    AG_LOGE(TAG, "BLE stack init failed");
    return false;
  }

  // --- Security: Passkey Entry (Display Only) ---
  if (!_server->set_security(AgBleIoCapability::DISPLAY_ONLY, AgBleAuth::BOND | AgBleAuth::MITM)) {
    AG_LOGE(TAG, "set_security failed");
    _server->deinit();
    return false;
  }

  // --- GATT Service ---
  auto *svc = _server->add_service(SERVICE_UUID);
  if (svc == nullptr) {
    AG_LOGE(TAG, "add_service failed");
    _server->deinit();
    return false;
  }

  // --- Characteristics ---

  // Measures: Read + Notify, authenticated to force pairing before subscribe/read.
  _measures_char = svc->add_characteristic(MEASURES_CHAR_UUID, measures_properties());
  if (_measures_char == nullptr) {
    AG_LOGE(TAG, "add Measures characteristic failed");
    _server->deinit();
    return false;
  }

  // Status: Read + Notify, authenticated
  _status_char = svc->add_characteristic(STATUS_CHAR_UUID, status_properties());
  if (_status_char == nullptr) {
    AG_LOGE(TAG, "add Status characteristic failed");
    _server->deinit();
    return false;
  }

  // Config: Read + Write + Notify, authenticated
  _config_char = svc->add_characteristic(CONFIG_CHAR_UUID, config_properties());
  if (_config_char == nullptr) {
    AG_LOGE(TAG, "add Config characteristic failed");
    _server->deinit();
    return false;
  }

  // History: Write + Notify, authenticated
  _history_char = svc->add_characteristic(HISTORY_CHAR_UUID, history_properties());
  if (_history_char == nullptr) {
    AG_LOGE(TAG, "add History characteristic failed");
    _server->deinit();
    return false;
  }

  // Cal: Read + Notify, unauthenticated (see cal_properties())
  _cal_char = svc->add_characteristic(CAL_CHAR_UUID, cal_properties());
  if (_cal_char == nullptr) {
    AG_LOGE(TAG, "add Cal characteristic failed");
    _server->deinit();
    return false;
  }

  // --- Write callbacks ---
  _config_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { on_config_write(data, len); });
  _history_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { on_history_write(data, len); });

  // --- Start GATT service ---
  if (!svc->start()) {
    AG_LOGE(TAG, "service start failed");
    _server->deinit();
    return false;
  }

  // --- Callbacks ---
  _server->set_connect_callback([this](uint16_t conn_handle) { on_connect(conn_handle); });
  _server->set_disconnect_callback(
      [this](uint16_t conn_handle, int reason) { on_disconnect(conn_handle, reason); });
  _server->set_passkey_display_callback([this](uint32_t passkey) { on_passkey_request(passkey); });
  _server->set_auth_complete_callback([this](uint16_t /*conn_handle*/, bool success) {
    // Drives onboarding via the event. The icon reads live link state in
    // is_authenticated(), so no flag is cached here.
    AG_LOGI(TAG, "auth %s", success ? "OK" : "FAILED");
    Event evt{};
    evt.type = EventType::BleAuthComplete;
    evt.ble_auth_ok = success;
    RTOS::queue_send(_event_queue, &evt);
  });

  AG_LOGI(TAG, "stack init + GATT registered (advertising deferred)");
  return true;
}

bool BleService::start_advertising() {
  // --- Advertising ---
  if (!_server->set_advertising_name(_adv_name)) {
    AG_LOGE(TAG, "set_advertising_name failed");
    _server->deinit();
    return false;
  }

  if (!_server->add_advertised_service_uuid(SERVICE_UUID)) {
    AG_LOGE(TAG, "add_advertised_service_uuid failed");
    _server->deinit();
    return false;
  }

  if (!_server->start_advertising()) {
    AG_LOGE(TAG, "start_advertising failed");
    _server->deinit();
    return false;
  }

  _initialized = true;
  AG_LOGI(TAG, "initialized, advertising as '%s'", _adv_name);
  return true;
}

#endif // TEST_HOST

void BleService::deinit() {
  if (!_initialized) {
    return;
  }

  _connected.store(false);
  _export_active = false;
  _export_session_id = 0;

  // _server stays bound — it is borrowed from the board for the service
  // lifetime.  Clearing characteristic pointers prevents stale pointer
  // dereferences if the next owner (e.g. provisioning) rebuilds the GATT
  // table differently.
  _server->deinit();
  _initialized = false;
  _measures_char = nullptr;
  _status_char = nullptr;
  _config_char = nullptr;
  _history_char = nullptr;
  _cal_char = nullptr;

  AG_LOGI(TAG, "deinitialized");
}

void BleService::set_disconnect_observer(AgBleDisconnectCallback cb) {
  _disconnect_observer = std::move(cb);
}

bool BleService::delete_all_bonds() {
  // Borrowed server is always bound after ctor; proxy directly.
  const bool ok = _server->delete_all_bonds();
  if (!ok) {
    AG_LOGW(TAG, "delete_all_bonds failed");
  }

  return ok;
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------

bool BleService::is_initialized() const { return _initialized; }

bool BleService::is_connected() const { return _connected.load(); }

bool BleService::is_authenticated() const {
  return _connected.load() && _server->is_peer_authenticated();
}

// ---------------------------------------------------------------------------
// NimBLE callbacks (run in NimBLE task context)
// ---------------------------------------------------------------------------

void BleService::on_connect(uint16_t conn_handle) {
  AG_LOGI(TAG, "client connected: handle=%u", conn_handle);
  _connected.store(true);

  // Stop advertising — single connection device.  The borrowed server is
  // always bound; the NimBLE callback would not have fired otherwise.
  _server->stop_advertising();

  Event evt{};
  evt.type = EventType::BleConnected;
  RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_disconnect(uint16_t conn_handle, int reason) {
  AG_LOGI(TAG, "client disconnected: handle=%u reason=%d", conn_handle, reason);

  // Fan out to the observer FIRST so an in-flight OTA aborts synchronously,
  // before advertising restarts. BleService keeps sole ownership of the slot.
  if (_disconnect_observer) {
    _disconnect_observer(conn_handle, reason);
  }

  _connected.store(false);

  // Clean up active history export
  _export_active = false;
  _export_session_id = 0;

  // Restart advertising — borrowed server is always bound.
  _server->start_advertising();

  Event evt{};
  evt.type = EventType::BleDisconnected;
  RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_config_write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0 || len > WRITE_BUF_SIZE) {
    AG_LOGW(TAG, "config write: invalid length %u", static_cast<unsigned>(len));
    return;
  }

  _config_write_mutex.lock();
  memcpy(_config_write_buf, data, len);
  _config_write_len = len;
  _config_write_pending = true;
  _config_write_mutex.unlock();

  Event evt{};
  evt.type = EventType::BleConfigWrite;
  RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_history_write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0 || len > WRITE_BUF_SIZE) {
    AG_LOGW(TAG, "history write: invalid length %u", static_cast<unsigned>(len));
    return;
  }

  _history_write_mutex.lock();
  memcpy(_history_write_buf, data, len);
  _history_write_len = len;
  _history_write_pending = true;
  _history_write_mutex.unlock();

  Event evt{};
  evt.type = EventType::BleHistoryWrite;
  RTOS::queue_send(_event_queue, &evt);
}

void BleService::on_passkey_request(uint32_t passkey) {
  AG_LOGI(TAG, "passkey display: %06" PRIu32, passkey);

  Event evt{};
  evt.type = EventType::BlePairingRequest;
  evt.ble_passkey = passkey;
  RTOS::queue_send(_event_queue, &evt);
}

// ---------------------------------------------------------------------------
// Pending write retrieval (called by orchestrator task)
// ---------------------------------------------------------------------------

size_t BleService::take_pending_config_write(uint8_t *buf, size_t buf_size) {
  _config_write_mutex.lock();
  if (!_config_write_pending) {
    _config_write_mutex.unlock();
    return 0;
  }
  size_t len = std::min(_config_write_len, buf_size);
  memcpy(buf, _config_write_buf, len);
  _config_write_pending = false;
  _config_write_len = 0;
  _config_write_mutex.unlock();
  return len;
}

size_t BleService::take_pending_history_write(uint8_t *buf, size_t buf_size) {
  _history_write_mutex.lock();
  if (!_history_write_pending) {
    _history_write_mutex.unlock();
    return 0;
  }
  size_t len = std::min(_history_write_len, buf_size);
  memcpy(buf, _history_write_buf, len);
  _history_write_pending = false;
  _history_write_len = 0;
  _history_write_mutex.unlock();
  return len;
}

// ---------------------------------------------------------------------------
// Data output: Measures
// ---------------------------------------------------------------------------

void BleService::notify_measures(const MeasuresAGo &measures, const GpsData &gps,
                                 time_t timestamp) {
  if (_measures_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_measures(buf, sizeof(buf), measures, gps, timestamp);
  if (len == 0) {
    AG_LOGW(TAG, "measures encode failed");
    return;
  }

  _measures_char->set_value(buf, len);

  if (_connected.load()) {
    _measures_char->notify();
  }
}

// ---------------------------------------------------------------------------
// Data output: Cal telemetry
// ---------------------------------------------------------------------------

void BleService::notify_cal(const BleCalTelemetry &cal, time_t timestamp) {
  if (_cal_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_cal(buf, sizeof(buf), cal, timestamp);
  if (len == 0) {
    AG_LOGW(TAG, "cal encode failed");
    return;
  }

  _cal_char->set_value(buf, len);

  if (_connected.load()) {
    _cal_char->notify();
  }
}

// ---------------------------------------------------------------------------
// Data output: Status
// ---------------------------------------------------------------------------

void BleService::update_status(const PowerSnapshot &power, const GpsData &gps, bool tracking_active,
                               uint32_t session_id) {
  if (_status_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_status(buf, sizeof(buf), power, gps, tracking_active, session_id);
  if (len == 0) {
    AG_LOGW(TAG, "status encode failed");
    return;
  }

  _status_char->set_value(buf, len);
}

void BleService::notify_tracking_status(const PowerSnapshot &power, const GpsData &gps,
                                        bool tracking_active, uint32_t session_id) {
  // Refresh the full 9-key snapshot through the sole writer (READ stays full).
  update_status(power, gps, tracking_active, session_id);

  if (!_connected.load() || _status_char == nullptr) {
    return;
  }

  // NOTIFY carries only the transition delta ({tracking, session}).
  uint8_t delta[STATUS_DELTA_BUF_SIZE];
  size_t len = encode_status_transition(delta, sizeof(delta), tracking_active, session_id);
  if (len == 0) {
    return; // encoder overflow guard (logged in encode_status_transition)
  }
  _status_char->notify(delta, len);
}

void BleService::notify_charging_status(const PowerSnapshot &power, const GpsData &gps,
                                        bool tracking_active, uint32_t session_id) {
  // Refresh the full 9-key snapshot through the sole writer (READ stays full).
  update_status(power, gps, tracking_active, session_id);

  if (!_connected.load() || _status_char == nullptr) {
    return;
  }

  // Power delta only; keys disjoint from the tracking delta, so no "type".
  uint8_t delta[STATUS_DELTA_BUF_SIZE];
  size_t len = encode_status_charging(delta, sizeof(delta), power);
  if (len == 0) {
    return; // encoder overflow guard
  }
  _status_char->notify(delta, len);
}

void BleService::notify_disconnect(BleDiscReason reason) {
  if (!_connected.load() || _status_char == nullptr) {
    return;
  }

  // NOTIFY-only: the `disc` key never enters the READ snapshot, so this never
  // touches the stored value. Best-effort; the caller settles before teardown.
  uint8_t delta[STATUS_DELTA_BUF_SIZE];
  size_t len = encode_status_disc(delta, sizeof(delta), reason);
  if (len == 0) {
    return; // encoder overflow guard
  }
  _status_char->notify(delta, len);
}

// ---------------------------------------------------------------------------
// Data output: Config
// ---------------------------------------------------------------------------

void BleService::update_config(const GoSettings &settings) {
  if (_config_char == nullptr) {
    return;
  }

  // Full snapshot — buffer sized to the 512-byte ATT ceiling for Read-Long.
  uint8_t buf[CONFIG_SNAPSHOT_BUF_SIZE];
  size_t len = encode_config(buf, sizeof(buf), settings);
  if (len == 0) {
    AG_LOGW(TAG, "config encode failed");
    return;
  }

  _config_char->set_value(buf, len);
}

void BleService::notify_config(const GoSettings &prev, const GoSettings &cur) {
  // Sole writer of the stored snapshot (READ) — refresh before any delta goes
  // out, closing the READ-vs-notify race without call-site ordering.
  update_config(cur);

  if (!_connected.load() || _config_char == nullptr) {
    return;
  }

  // NOTIFY carries only the delta; the stored READ value stays the full snapshot.
  uint8_t buf[CBOR_BUF_SIZE];
  size_t len = encode_config_delta(buf, sizeof(buf), prev, cur);
  if (len == 0) {
    return; // encoder overflow guard (logged in encode_config_delta)
  }
  _config_char->notify(buf, len);
}

void BleService::notify_command_result(BleCommand cmd, bool success, const char *error) {
  if (!_connected.load() || _config_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  size_t map_len = 3; // type, cmd, ok
  if (!success && error != nullptr) {
    map_len = 4; // + err
  }
  cbor_encoder_create_map(&encoder, &map, map_len);

  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_CMD_RESULT);

  cbor_encode_text_stringz(&map, BLE_KEY_CMD);
  cbor_encode_text_stringz(&map, ble_command_to_str(cmd));

  cbor_encode_text_stringz(&map, BLE_KEY_OK);
  cbor_encode_boolean(&map, success);

  if (!success && error != nullptr) {
    cbor_encode_text_stringz(&map, BLE_KEY_ERR);
    cbor_encode_text_stringz(&map, error);
  }

  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "cmd_result encode overflow");
    return;
  }
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  // notify(data, len) leaves the Config stored value as the full snapshot.
  _config_char->notify(buf, len);
}

void BleService::notify_command_progress(BleCommand cmd) {
  if (!_connected.load() || _config_char == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);

  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_CMD_PROGRESS);

  cbor_encode_text_stringz(&map, BLE_KEY_CMD);
  cbor_encode_text_stringz(&map, ble_command_to_str(cmd));

  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "cmd_progress encode overflow");
    return;
  }
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  // notify(data, len) leaves the Config stored value as the full snapshot.
  _config_char->notify(buf, len);
}

// ---------------------------------------------------------------------------
// History: session list
// ---------------------------------------------------------------------------

void BleService::handle_history_list() {
  if (_history_char == nullptr) {
    return;
  }

  // Read session list from storage (heap-allocated, no hard cap)
  uint16_t session_count = _storage.session_count();
  std::vector<uint32_t> session_ids(session_count);
  if (session_count > 0) {
    session_count = _storage.list_sessions(session_ids.data(), session_count);
  }

  // Paginate: send one CBOR notification per page
  uint16_t total_pages =
      session_count > 0 ? (session_count + SESSIONS_PER_PAGE - 1) / SESSIONS_PER_PAGE : 1;

  for (uint16_t page = 0; page < total_pages && _connected.load(); page++) {
    uint16_t start = page * SESSIONS_PER_PAGE;
    uint16_t page_count = std::min(static_cast<uint16_t>(SESSIONS_PER_PAGE),
                                   static_cast<uint16_t>(session_count - start));

    // Encode: {"type":"sessions","sessions":[...],"pg":N,"tpg":N,"cnt":N}
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 5);

    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_SESSIONS);

    cbor_encode_text_stringz(&map, BLE_KEY_SESSIONS);
    CborEncoder arr;
    cbor_encoder_create_array(&map, &arr, page_count);

    for (uint16_t i = 0; i < page_count; i++) {
      uint16_t idx = start + i;
      CborEncoder sess_map;
      cbor_encoder_create_map(&arr, &sess_map, 3);

      cbor_encode_text_stringz(&sess_map, BLE_KEY_ID);
      cbor_encode_uint(&sess_map, session_ids[idx]);

      cbor_encode_text_stringz(&sess_map, BLE_KEY_PTS);
      uint32_t pts = _storage.get_session_point_count(session_ids[idx]);
      cbor_encode_uint(&sess_map, pts);

      cbor_encode_text_stringz(&sess_map, BLE_KEY_TS);
      time_t start_ts = _storage.get_session_start_time(session_ids[idx]);
      cbor_encode_uint(&sess_map, static_cast<uint64_t>(start_ts));

      cbor_encoder_close_container(&arr, &sess_map);
    }

    cbor_encoder_close_container(&map, &arr);

    cbor_encode_text_stringz(&map, BLE_KEY_PG);
    cbor_encode_uint(&map, static_cast<uint64_t>(page + 1));

    cbor_encode_text_stringz(&map, BLE_KEY_TPG);
    cbor_encode_uint(&map, static_cast<uint64_t>(total_pages));

    cbor_encode_text_stringz(&map, BLE_KEY_CNT);
    cbor_encode_uint(&map, static_cast<uint64_t>(session_count));

    cbor_encoder_close_container(&encoder, &map);

    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
  }
}

// ---------------------------------------------------------------------------
// History: start download
// ---------------------------------------------------------------------------

void BleService::handle_history_start(uint32_t session_id) {
  if (_history_char == nullptr) {
    return;
  }

  // End any active download first
  if (_export_active) {
    _export_active = false;
  }

  // Check if session exists
  uint32_t total_points = _storage.get_session_point_count(session_id);
  if (total_points == 0) {
    // Send error: session not found
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 2);
    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ERROR);
    cbor_encode_text_stringz(&map, BLE_KEY_ERR);
    cbor_encode_text_stringz(&map, BLE_VAL_ERR_SESSION_NOT_FOUND);
    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
    return;
  }

  _export_session_id = session_id;
  _export_active = true;

  // Send "started" CBOR response
  {
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 4);

    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_STARTED);

    cbor_encode_text_stringz(&map, BLE_KEY_SESSION);
    cbor_encode_uint(&map, session_id);

    cbor_encode_text_stringz(&map, BLE_KEY_TOTAL);
    cbor_encode_uint(&map, total_points);

    cbor_encode_text_stringz(&map, BLE_KEY_PT_SIZE);
    cbor_encode_uint(&map, ROUTE_POINT_WIRE_SIZE);

    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
  }

  // Stream all points as binary notifications
  uint32_t sent = 0;
  RoutePoint batch[ROUTE_READ_BATCH];

  for (uint32_t offset = 0; offset < total_points && _connected.load(); /* incremented below */) {
    uint16_t to_read = static_cast<uint16_t>(
        std::min(static_cast<uint32_t>(ROUTE_READ_BATCH), total_points - offset));
    uint16_t actually_read = _storage.read_route_points(session_id, offset, batch, to_read);

    if (actually_read == 0) {
      // Flash read error — abort stream
      AG_LOGE(TAG, "flash read error at offset %" PRIu32, offset);
      uint8_t buf[CBOR_BUF_SIZE];
      CborEncoder encoder;
      cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
      CborEncoder map;
      cbor_encoder_create_map(&encoder, &map, 2);
      cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
      cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ERROR);
      cbor_encode_text_stringz(&map, BLE_KEY_ERR);
      cbor_encode_text_stringz(&map, BLE_VAL_ERR_FLASH_ERROR);
      cbor_encoder_close_container(&encoder, &map);
      size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
      send_history_cbor(buf, len);
      _export_active = false;
      return;
    }

    // Convert to wire format and send
    uint8_t wire_buf[ROUTE_READ_BATCH * ROUTE_POINT_WIRE_SIZE];
    for (uint16_t i = 0; i < actually_read; i++) {
      route_point_to_wire(batch[i], &wire_buf[i * ROUTE_POINT_WIRE_SIZE]);
    }

    send_history_binary(static_cast<uint16_t>(offset), wire_buf,
                        actually_read * ROUTE_POINT_WIRE_SIZE);

    offset += actually_read;
    sent += actually_read;
  }

  // Send "done" CBOR response
  {
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 2);
    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_DONE);
    cbor_encode_text_stringz(&map, BLE_KEY_SENT);
    cbor_encode_uint(&map, sent);
    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
  }
}

// ---------------------------------------------------------------------------
// History: fill missing points
// ---------------------------------------------------------------------------

void BleService::handle_history_fill(const uint32_t *point_indices, size_t count) {
  if (_history_char == nullptr) {
    return;
  }

  if (!_export_active) {
    // Send error: no active download
    uint8_t buf[CBOR_BUF_SIZE];
    CborEncoder encoder;
    cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
    CborEncoder map;
    cbor_encoder_create_map(&encoder, &map, 2);
    cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
    cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ERROR);
    cbor_encode_text_stringz(&map, BLE_KEY_ERR);
    cbor_encode_text_stringz(&map, BLE_VAL_ERR_NO_ACTIVE_DOWNLOAD);
    cbor_encoder_close_container(&encoder, &map);
    size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
    send_history_cbor(buf, len);
    return;
  }

  uint32_t sent = 0;

  for (size_t i = 0; i < count && _connected.load(); i++) {
    RoutePoint point;
    uint16_t actually_read =
        _storage.read_route_points(_export_session_id, point_indices[i], &point, 1);

    if (actually_read == 0) {
      AG_LOGW(TAG, "fill: could not read point %" PRIu32, point_indices[i]);
      continue;
    }

    uint8_t wire_buf[ROUTE_POINT_WIRE_SIZE];
    route_point_to_wire(point, wire_buf);
    send_history_binary(static_cast<uint16_t>(point_indices[i]), wire_buf, ROUTE_POINT_WIRE_SIZE);
    sent++;
  }

  // Send "done" CBOR response
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);
  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_DONE);
  cbor_encode_text_stringz(&map, BLE_KEY_SENT);
  cbor_encode_uint(&map, sent);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

// ---------------------------------------------------------------------------
// History: end download
// ---------------------------------------------------------------------------

void BleService::handle_history_end() {
  _export_active = false;
  _export_session_id = 0;

  if (_history_char == nullptr) {
    return;
  }

  // Send "ended" CBOR response
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 1);
  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ENDED);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

void BleService::handle_history_delete(uint32_t session_id) {
  if (_history_char == nullptr) {
    return;
  }

  // Check if session exists
  uint32_t point_count = _storage.get_session_point_count(session_id);
  if (point_count == 0) {
    notify_history_error(BLE_VAL_ERR_SESSION_NOT_FOUND);
    return;
  }

  // End any active export for this session
  if (_export_active && _export_session_id == session_id) {
    _export_active = false;
    _export_session_id = 0;
  }

  // Delete the route file
  if (!_storage.delete_route(session_id)) {
    notify_history_error(BLE_VAL_ERR_DELETE_FAILED);
    return;
  }

  // Send "deleted" CBOR response: {"type": "deleted", "session": <id>}
  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);
  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_DELETED);
  cbor_encode_text_stringz(&map, BLE_KEY_SESSION);
  cbor_encode_uint(&map, session_id);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

void BleService::notify_history_error(const char *err) {
  if (_history_char == nullptr || err == nullptr) {
    return;
  }

  uint8_t buf[CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);
  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_ERROR);
  cbor_encode_text_stringz(&map, BLE_KEY_ERR);
  cbor_encode_text_stringz(&map, err);
  cbor_encoder_close_container(&encoder, &map);
  size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  send_history_cbor(buf, len);
}

// ---------------------------------------------------------------------------
// History: notification helpers
// ---------------------------------------------------------------------------

bool BleService::send_history_cbor(const uint8_t *cbor_data, size_t cbor_len) {
  if (_history_char == nullptr || cbor_len == 0) {
    return false;
  }

  // Prepend tag byte 0x00
  uint8_t tagged[CBOR_BUF_SIZE + 1];
  tagged[0] = HISTORY_TAG_CBOR;
  size_t total = 1 + cbor_len;
  if (total > sizeof(tagged)) {
    AG_LOGW(TAG, "history CBOR too large: %u bytes", static_cast<unsigned>(total));
    return false;
  }
  memcpy(&tagged[1], cbor_data, cbor_len);

  _history_char->set_value(tagged, total);

  // Retry with backpressure
  while (!_history_char->notify()) {
    if (!_connected.load()) {
      return false;
    }
    RTOS::delay_ms(NOTIFY_RETRY_DELAY_MS);
  }
  return true;
}

bool BleService::send_history_binary(uint16_t first_point_index, const uint8_t *data,
                                     size_t data_len) {
  if (_history_char == nullptr || data_len == 0) {
    return false;
  }

  // Build notification: [tag] [uint16_le index] [data...]
  uint8_t packet[MAX_NOTIFY_PAYLOAD];
  size_t total = BINARY_HEADER_SIZE + data_len;
  if (total > sizeof(packet)) {
    AG_LOGW(TAG, "history binary too large: %u bytes", static_cast<unsigned>(total));
    return false;
  }

  packet[0] = HISTORY_TAG_BINARY;
  // Little-endian uint16
  packet[1] = static_cast<uint8_t>(first_point_index & 0xFF);
  packet[2] = static_cast<uint8_t>((first_point_index >> 8) & 0xFF);
  memcpy(&packet[BINARY_HEADER_SIZE], data, data_len);

  _history_char->set_value(packet, total);

  // Retry with backpressure
  while (!_history_char->notify()) {
    if (!_connected.load()) {
      return false;
    }
    RTOS::delay_ms(NOTIFY_RETRY_DELAY_MS);
  }
  return true;
}

// ---------------------------------------------------------------------------
// RoutePointWire conversion
// ---------------------------------------------------------------------------

void BleService::route_point_to_wire(const RoutePoint &point, uint8_t *out) {
  // 55 bytes, packed little-endian.
  // Use memcpy for type-punning safety (platform is little-endian on ESP32).

  size_t offset = 0;

  auto write_u32 = [&](uint32_t v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };
  auto write_f64 = [&](double v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };
  auto write_f32 = [&](float v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };
  auto write_u8 = [&](uint8_t v) {
    out[offset] = v;
    offset += 1;
  };
  auto write_i16 = [&](int16_t v) {
    memcpy(out + offset, &v, sizeof(v));
    offset += sizeof(v);
  };

  // timestamp (4)
  write_u32(static_cast<uint32_t>(point.timestamp));

  // GPS position
  if (is_latitude_valid(point.gps.position.latitude)) {
    write_f64(point.gps.position.latitude);
  } else {
    write_f64(GPS_LATITUDE_INVALID);
  }

  if (is_longitude_valid(point.gps.position.longitude)) {
    write_f64(point.gps.position.longitude);
  } else {
    write_f64(GPS_LONGITUDE_INVALID);
  }

  // altitude (4)
  if (is_altitude_valid(point.gps.altitude_m)) {
    write_f32(point.gps.altitude_m);
  } else {
    write_f32(GPS_ALTITUDE_INVALID);
  }

  // gps_fix (1)
  write_u8(static_cast<uint8_t>(point.gps.fix.fix_type));

  // Sensor data — use sentinel values for invalid fields
  const auto &th = point.sensors.temp_hum_a;
  write_f32(th.is_temp_valid() ? th.temperature : MeasuresInvalid::TEMPERATURE);
  write_f32(th.is_hum_valid() ? th.humidity : MeasuresInvalid::HUMIDITY);

  const auto &pm = point.sensors.pm_a;
  write_f32(pm.is_pm_01_valid() ? pm.pm_01 : MeasuresInvalid::PM);
  write_f32(pm.is_pm_25_valid() ? pm.pm_25 : MeasuresInvalid::PM);
  write_f32(pm.is_pm_10_valid() ? pm.pm_10 : MeasuresInvalid::PM);

  const auto &co2 = point.sensors.co2;
  write_i16(co2.is_valid() ? static_cast<int16_t>(co2.co2) : static_cast<int16_t>(-1));

  const auto &tvoc_nox = point.sensors.tvoc_nox;
  write_i16(tvoc_nox.is_tvoc_index_valid() ? static_cast<int16_t>(tvoc_nox.tvoc_index)
                                           : static_cast<int16_t>(-1));
  write_i16(tvoc_nox.is_nox_index_valid() ? static_cast<int16_t>(tvoc_nox.nox_index)
                                          : static_cast<int16_t>(-1));

  // pressure (4)
  const auto &pres = point.sensors.pressure;
  write_f32(pres.is_pressure_valid() ? pres.pressure : MeasuresInvalid::PRESSURE);

  // battery_percentage (1) — 0–100 valid, 255 = invalid
  write_u8(point.battery_percentage >= 0.0f ? static_cast<uint8_t>(point.battery_percentage)
                                            : static_cast<uint8_t>(255));
}

// ---------------------------------------------------------------------------
// CBOR encoding: Measures
// ---------------------------------------------------------------------------

size_t BleService::encode_measures(uint8_t *buf, size_t buf_size, const MeasuresAGo &m,
                                   const GpsData &gps, time_t ts) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // Count fields to determine map size (ts is always present)
  size_t field_count = 1; // "ts"

  const auto &th = m.temp_hum_a;
  if (th.is_temp_valid())
    field_count++;
  if (th.is_hum_valid())
    field_count++;

  const auto &pm = m.pm_a;
  if (pm.is_pm_01_valid())
    field_count++;
  if (pm.is_pm_25_valid())
    field_count++;
  if (pm.is_pm_10_valid())
    field_count++;

  if (m.co2.is_valid())
    field_count++;
  if (m.tvoc_nox.is_tvoc_index_valid())
    field_count++;
  if (m.tvoc_nox.is_nox_index_valid())
    field_count++;

  if (m.pressure.is_pressure_valid())
    field_count++;

  // GPS fields are only included when tracking (indicated by fix type check).
  // The caller (orchestrator) calls notify_measures() on every SensorDataReady;
  // we check fix availability as a defensive guard.
  // GPS fields: lat, lon, alt, fix, sat — always included as a group when tracking.
  bool include_gps = is_fix_valid(gps.fix);
  if (include_gps) {
    // lat, lon conditionally based on validity
    if (is_latitude_valid(gps.position.latitude))
      field_count++;
    if (is_longitude_valid(gps.position.longitude))
      field_count++;
    if (is_altitude_valid(gps.altitude_m))
      field_count++;
    field_count++; // "fix" (always when GPS included)
    field_count++; // "sat" (always when GPS included)
  }

  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, field_count);

  // Sensor fields — omit invalid
  if (th.is_temp_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_TEMP);
    cbor_encode_float(&map, th.temperature);
  }
  if (th.is_hum_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_HUM);
    cbor_encode_float(&map, th.humidity);
  }

  if (pm.is_pm_01_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_PM1);
    cbor_encode_float(&map, pm.pm_01);
  }
  if (pm.is_pm_25_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_PM25);
    cbor_encode_float(&map, pm.pm_25);
  }
  if (pm.is_pm_10_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_PM10);
    cbor_encode_float(&map, pm.pm_10);
  }

  if (m.co2.is_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_CO2);
    cbor_encode_uint(&map, static_cast<uint64_t>(m.co2.co2));
  }

  if (m.tvoc_nox.is_tvoc_index_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_TVOC);
    cbor_encode_uint(&map, static_cast<uint64_t>(m.tvoc_nox.tvoc_index));
  }
  if (m.tvoc_nox.is_nox_index_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_NOX);
    cbor_encode_uint(&map, static_cast<uint64_t>(m.tvoc_nox.nox_index));
  }

  if (m.pressure.is_pressure_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_PRES);
    cbor_encode_float(&map, m.pressure.pressure);
  }

  // GPS fields (only when tracking / fix valid)
  if (include_gps) {
    if (is_latitude_valid(gps.position.latitude)) {
      cbor_encode_text_stringz(&map, BLE_KEY_LAT);
      cbor_encode_double(&map, gps.position.latitude);
    }
    if (is_longitude_valid(gps.position.longitude)) {
      cbor_encode_text_stringz(&map, BLE_KEY_LON);
      cbor_encode_double(&map, gps.position.longitude);
    }
    if (is_altitude_valid(gps.altitude_m)) {
      cbor_encode_text_stringz(&map, BLE_KEY_ALT);
      cbor_encode_float(&map, gps.altitude_m);
    }
    cbor_encode_text_stringz(&map, BLE_KEY_FIX);
    cbor_encode_uint(&map, static_cast<uint64_t>(gps.fix.fix_type));

    cbor_encode_text_stringz(&map, BLE_KEY_SAT);
    cbor_encode_uint(&map, static_cast<uint64_t>(is_satellite_count_valid(gps.fix.satellite_count)
                                                     ? gps.fix.satellite_count
                                                     : 0));
  }

  // Timestamp is always present
  cbor_encode_text_stringz(&map, BLE_KEY_TS);
  cbor_encode_uint(&map, static_cast<uint64_t>(ts));

  cbor_encoder_close_container(&encoder, &map);

  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Cal telemetry
// ---------------------------------------------------------------------------

size_t BleService::encode_cal(uint8_t *buf, size_t buf_size, const BleCalTelemetry &cal,
                              time_t ts) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  const bool pres_valid = cal.pressure_hpa >= MeasuresRange::MIN_VALID_PRESSURE &&
                          cal.pressure_hpa <= MeasuresRange::MAX_VALID_PRESSURE;
  const bool tfg_valid = cal.t_fg_c != BmsInvalid::FG_TEMP_C;
  const bool tdie_valid = cal.t_die_c != BmsInvalid::TEMPERATURE_C;
  const bool tbat_valid = cal.t_bat_c != BmsInvalid::TEMPERATURE_C;
  const bool ibat_valid = cal.ibat_fg_ma != BmsInvalid::CURRENT_MA;
  const bool ichg_valid = cal.ibat_bms_ma != BmsInvalid::CURRENT_MA;
  const bool ibus_valid = cal.ibus_ma != BmsInvalid::CURRENT_MA;
  const bool vbus_valid = cal.vbus >= BmsRange::MIN_VALID_VOLT;
  const bool vbat_valid = cal.vbat >= BmsRange::MIN_VALID_VOLT;

  // "chg", "gps", "up", "ts" are always present
  size_t field_count = 4;
  if (cal.sht.is_temp_valid())
    field_count++;
  if (cal.sht.is_hum_valid())
    field_count++;
  if (cal.dps.is_temp_valid())
    field_count++;
  if (pres_valid)
    field_count++;
  if (tfg_valid)
    field_count++;
  if (tdie_valid)
    field_count++;
  if (tbat_valid)
    field_count++;
  if (ibat_valid)
    field_count++;
  if (ichg_valid)
    field_count++;
  if (ibus_valid)
    field_count++;
  if (vbus_valid)
    field_count++;
  if (vbat_valid)
    field_count++;

  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, field_count);

  if (cal.sht.is_temp_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_TEMP);
    cbor_encode_float(&map, cal.sht.temperature);
  }
  if (cal.sht.is_hum_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_HUM);
    cbor_encode_float(&map, cal.sht.humidity);
  }
  if (cal.dps.is_temp_valid()) {
    cbor_encode_text_stringz(&map, BLE_KEY_CAL_TDPS);
    cbor_encode_float(&map, cal.dps.temperature);
  }
  if (pres_valid) {
    cbor_encode_text_stringz(&map, BLE_KEY_PRES);
    cbor_encode_float(&map, cal.pressure_hpa);
  }
  if (tfg_valid) {
    cbor_encode_text_stringz(&map, BLE_KEY_CAL_TFG);
    cbor_encode_float(&map, cal.t_fg_c);
  }
  if (tdie_valid) {
    cbor_encode_text_stringz(&map, BLE_KEY_CAL_TDIE);
    cbor_encode_int(&map, cal.t_die_c);
  }
  if (tbat_valid) {
    cbor_encode_text_stringz(&map, BLE_KEY_CAL_TBAT);
    cbor_encode_int(&map, cal.t_bat_c);
  }
  if (ibat_valid) {
    cbor_encode_text_stringz(&map, BLE_KEY_CAL_IBAT);
    cbor_encode_int(&map, cal.ibat_fg_ma);
  }
  if (ichg_valid) {
    cbor_encode_text_stringz(&map, BLE_KEY_CAL_ICHG);
    cbor_encode_int(&map, cal.ibat_bms_ma);
  }
  if (ibus_valid) {
    cbor_encode_text_stringz(&map, BLE_KEY_CAL_IBUS);
    cbor_encode_int(&map, cal.ibus_ma);
  }
  if (vbus_valid) {
    cbor_encode_text_stringz(&map, BLE_KEY_CAL_VBUS);
    cbor_encode_float(&map, cal.vbus);
  }
  if (vbat_valid) {
    cbor_encode_text_stringz(&map, BLE_KEY_CAL_VBAT);
    cbor_encode_float(&map, cal.vbat);
  }

  cbor_encode_text_stringz(&map, BLE_KEY_CAL_CHG);
  cbor_encode_text_stringz(&map, charging_state_to_str(cal.charging));

  cbor_encode_text_stringz(&map, BLE_KEY_CAL_GPS);
  cbor_encode_uint(&map, cal.gps_active ? 1 : 0);

  cbor_encode_text_stringz(&map, BLE_KEY_CAL_UP);
  cbor_encode_uint(&map, cal.uptime_s);

  cbor_encode_text_stringz(&map, BLE_KEY_TS);
  cbor_encode_uint(&map, static_cast<uint64_t>(ts));

  cbor_encoder_close_container(&encoder, &map);

  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// CBOR encoding: Status
// ---------------------------------------------------------------------------

size_t BleService::encode_status(uint8_t *buf, size_t buf_size, const PowerSnapshot &power,
                                 const GpsData &gps, bool tracking, uint32_t session_id) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // All 9 keys are always present. Firmware version is intentionally not
  // exposed here; it lives only in DIS Firmware Revision (0x2A26).
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 9);

  cbor_encode_text_stringz(&map, BLE_KEY_GPS_FIX);
  cbor_encode_uint(&map, static_cast<uint64_t>(gps.fix.fix_type));

  cbor_encode_text_stringz(&map, BLE_KEY_GPS_SAT);
  cbor_encode_uint(&map, static_cast<uint64_t>(is_satellite_count_valid(gps.fix.satellite_count)
                                                   ? gps.fix.satellite_count
                                                   : 0));

  cbor_encode_text_stringz(&map, BLE_KEY_BAT_PCT);
  cbor_encode_uint(&map, static_cast<uint64_t>(power.battery_percentage >= 0.0f
                                                   ? static_cast<uint32_t>(power.battery_percentage)
                                                   : 0));

  cbor_encode_text_stringz(&map, BLE_KEY_BAT_V);
  cbor_encode_float(&map, power.battery_voltage >= 0.0f ? power.battery_voltage : 0.0f);

  cbor_encode_text_stringz(&map, BLE_KEY_CHARGING);
  cbor_encode_text_stringz(&map, charging_state_to_str(power.charging_status));

  cbor_encode_text_stringz(&map, BLE_KEY_TRACKING);
  cbor_encode_boolean(&map, tracking);

  cbor_encode_text_stringz(&map, BLE_KEY_SESSION);
  cbor_encode_uint(&map, session_id);

  cbor_encode_text_stringz(&map, BLE_KEY_FLASH_KB);
  cbor_encode_uint(&map, _storage.total_capacity_kb());

  cbor_encode_text_stringz(&map, BLE_KEY_USED_KB);
  cbor_encode_uint(&map, _storage.used_kb());

  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "status encode overflow");
    return 0;
  }
  return cbor_encoder_get_buffer_size(&encoder, buf);
}

size_t BleService::encode_status_transition(uint8_t *buf, size_t buf_size, bool tracking,
                                            uint32_t session_id) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // Transition delta: only the two keys that change. No "type" — the Status
  // characteristic carries only status notifications.
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 2);

  cbor_encode_text_stringz(&map, BLE_KEY_TRACKING);
  cbor_encode_boolean(&map, tracking);

  cbor_encode_text_stringz(&map, BLE_KEY_SESSION);
  cbor_encode_uint(&map, session_id);

  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "status transition encode overflow");
    return 0;
  }
  return cbor_encoder_get_buffer_size(&encoder, buf);
}

size_t BleService::encode_status_charging(uint8_t *buf, size_t buf_size,
                                          const PowerSnapshot &power) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // 3-key power delta; same clamping as encode_status().
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 3);

  cbor_encode_text_stringz(&map, BLE_KEY_CHARGING);
  cbor_encode_text_stringz(&map, charging_state_to_str(power.charging_status));

  cbor_encode_text_stringz(&map, BLE_KEY_BAT_PCT);
  cbor_encode_uint(&map, static_cast<uint64_t>(power.battery_percentage >= 0.0f
                                                   ? static_cast<uint32_t>(power.battery_percentage)
                                                   : 0));

  cbor_encode_text_stringz(&map, BLE_KEY_BAT_V);
  cbor_encode_float(&map, power.battery_voltage >= 0.0f ? power.battery_voltage : 0.0f);

  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "status charging encode overflow");
    return 0;
  }
  return cbor_encoder_get_buffer_size(&encoder, buf);
}

size_t BleService::encode_status_disc(uint8_t *buf, size_t buf_size, BleDiscReason reason) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // Single-key disconnect notice; merged by key like the other Status deltas.
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 1);

  cbor_encode_text_stringz(&map, BLE_KEY_DISC);
  cbor_encode_text_stringz(&map, disc_reason_to_str(reason));

  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "status disc encode overflow");
    return 0;
  }
  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// Config field registry
// ---------------------------------------------------------------------------
//
// A single table drives both the full snapshot (READ) and the delta (NOTIFY).
// Adding a config field is one row plus its two small helpers below. The
// enum-to-wire string mappers are file-scope here so the free encode helpers
// can reach them; BleService::gps_mode_to_str()/operating_mode_to_str() (kept
// for tests) delegate to these.

static const char *gps_mode_to_wire(GpsMode mode) {
  switch (mode) {
  case GpsMode::AlwaysOff:
    return BLE_VAL_GPS_OFF;
  case GpsMode::OnWhenTracking:
    return BLE_VAL_GPS_TRACKING;
  case GpsMode::AlwaysOn:
    return BLE_VAL_GPS_ALWAYS;
  default:
    return BLE_VAL_GPS_TRACKING;
  }
}

static const char *operating_mode_to_wire(OperatingMode mode) {
  switch (mode) {
  case OperatingMode::Portable:
    return BLE_VAL_MODE_PORTABLE;
  case OperatingMode::Stationary:
    return BLE_VAL_MODE_STATIONARY;
  case OperatingMode::Offline:
    return BLE_VAL_MODE_OFFLINE;
  default:
    return BLE_VAL_MODE_OFFLINE;
  }
}

struct ConfigField {
  const char *key;
  void (*encode_value)(CborEncoder &map, const GoSettings &s);
  bool (*differs)(const GoSettings &a, const GoSettings &b);
};

// Per-field encode helpers — write only the value (the caller writes the key).
static void enc_meas_int(CborEncoder &m, const GoSettings &s) {
  cbor_encode_uint(&m, static_cast<uint64_t>(s.measure_interval_seconds));
}
static void enc_temp_f(CborEncoder &m, const GoSettings &s) {
  cbor_encode_boolean(&m, s.use_fahrenheit);
}
static void enc_pm_aqi(CborEncoder &m, const GoSettings &s) {
  cbor_encode_boolean(&m, s.pm_use_usaqi);
}
static void enc_gps_int(CborEncoder &m, const GoSettings &s) {
  cbor_encode_uint(&m, static_cast<uint64_t>(s.gps_interval_seconds));
}
static void enc_gps_mode(CborEncoder &m, const GoSettings &s) {
  cbor_encode_text_stringz(&m, gps_mode_to_wire(s.gps_mode));
}
static void enc_inact_to(CborEncoder &m, const GoSettings &s) {
  cbor_encode_uint(&m, static_cast<uint64_t>(s.inactivity_timeout_seconds));
}
static void enc_auto_lock(CborEncoder &m, const GoSettings &s) {
  cbor_encode_uint(&m, static_cast<uint64_t>(s.auto_lock_seconds));
}
static void enc_dev_name(CborEncoder &m, const GoSettings &s) {
  cbor_encode_text_stringz(&m, s.device_name.c_str());
}
static void enc_op_mode(CborEncoder &m, const GoSettings &s) {
  cbor_encode_text_stringz(&m, operating_mode_to_wire(s.operating_mode));
}
static void enc_fled(CborEncoder &m, const GoSettings &s) {
  cbor_encode_uint(&m, static_cast<uint64_t>(s.front_led_brightness));
}
static void enc_bled(CborEncoder &m, const GoSettings &s) {
  cbor_encode_uint(&m, static_cast<uint64_t>(s.back_led_brightness));
}
static void enc_tled(CborEncoder &m, const GoSettings &s) {
  cbor_encode_uint(&m, static_cast<uint64_t>(s.touch_led_intensity));
}

// Per-field difference predicates for delta encoding.
static bool dif_meas_int(const GoSettings &a, const GoSettings &b) {
  return a.measure_interval_seconds != b.measure_interval_seconds;
}
static bool dif_temp_f(const GoSettings &a, const GoSettings &b) {
  return a.use_fahrenheit != b.use_fahrenheit;
}
static bool dif_pm_aqi(const GoSettings &a, const GoSettings &b) {
  return a.pm_use_usaqi != b.pm_use_usaqi;
}
static bool dif_gps_int(const GoSettings &a, const GoSettings &b) {
  return a.gps_interval_seconds != b.gps_interval_seconds;
}
static bool dif_gps_mode(const GoSettings &a, const GoSettings &b) {
  return a.gps_mode != b.gps_mode;
}
static bool dif_inact_to(const GoSettings &a, const GoSettings &b) {
  return a.inactivity_timeout_seconds != b.inactivity_timeout_seconds;
}
static bool dif_auto_lock(const GoSettings &a, const GoSettings &b) {
  return a.auto_lock_seconds != b.auto_lock_seconds;
}
static bool dif_dev_name(const GoSettings &a, const GoSettings &b) {
  return a.device_name != b.device_name;
}
static bool dif_op_mode(const GoSettings &a, const GoSettings &b) {
  return a.operating_mode != b.operating_mode;
}
static bool dif_fled(const GoSettings &a, const GoSettings &b) {
  return a.front_led_brightness != b.front_led_brightness;
}
static bool dif_bled(const GoSettings &a, const GoSettings &b) {
  return a.back_led_brightness != b.back_led_brightness;
}
static bool dif_tled(const GoSettings &a, const GoSettings &b) {
  return a.touch_led_intensity != b.touch_led_intensity;
}

static const ConfigField CONFIG_FIELDS[] = {
    {BLE_KEY_MEAS_INT, enc_meas_int, dif_meas_int},
    {BLE_KEY_TEMP_F, enc_temp_f, dif_temp_f},
    {BLE_KEY_PM_AQI, enc_pm_aqi, dif_pm_aqi},
    {BLE_KEY_GPS_INT, enc_gps_int, dif_gps_int},
    {BLE_KEY_GPS_MODE, enc_gps_mode, dif_gps_mode},
    {BLE_KEY_INACT_TO, enc_inact_to, dif_inact_to},
    {BLE_KEY_AUTO_LOCK, enc_auto_lock, dif_auto_lock},
    {BLE_KEY_DEV_NAME, enc_dev_name, dif_dev_name},
    {BLE_KEY_OP_MODE, enc_op_mode, dif_op_mode},
    {BLE_KEY_FRONT_LED, enc_fled, dif_fled},
    {BLE_KEY_BACK_LED, enc_bled, dif_bled},
    {BLE_KEY_TOUCH_LED, enc_tled, dif_tled},
};
static constexpr size_t CONFIG_FIELD_COUNT = sizeof(CONFIG_FIELDS) / sizeof(CONFIG_FIELDS[0]);

// ---------------------------------------------------------------------------
// CBOR encoding: Config
// ---------------------------------------------------------------------------

size_t BleService::encode_config(uint8_t *buf, size_t buf_size, const GoSettings &settings) {
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // Full snapshot (READ): every field, no "type" discriminator.
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, CONFIG_FIELD_COUNT);
  for (const auto &f : CONFIG_FIELDS) {
    cbor_encode_text_stringz(&map, f.key);
    f.encode_value(map, settings);
  }
  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "config snapshot encode overflow");
    return 0;
  }
  return cbor_encoder_get_buffer_size(&encoder, buf);
}

size_t BleService::encode_config_delta(uint8_t *buf, size_t buf_size, const GoSettings &prev,
                                       const GoSettings &cur) {
  size_t changed = 0;
  for (const auto &f : CONFIG_FIELDS) {
    if (f.differs(prev, cur)) {
      changed++;
    }
  }

  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, buf_size, 0);

  // Delta (NOTIFY): "type":"config" plus only the changed fields.
  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, changed + 1); // +1 for "type"

  cbor_encode_text_stringz(&map, BLE_KEY_TYPE);
  cbor_encode_text_stringz(&map, BLE_VAL_TYPE_CONFIG);

  for (const auto &f : CONFIG_FIELDS) {
    if (f.differs(prev, cur)) {
      cbor_encode_text_stringz(&map, f.key);
      f.encode_value(map, cur);
    }
  }
  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "config delta encode overflow");
    return 0;
  }
  return cbor_encoder_get_buffer_size(&encoder, buf);
}

// ---------------------------------------------------------------------------
// String mapping helpers
// ---------------------------------------------------------------------------

const char *BleService::charging_state_to_str(BmsChargingState state) {
  switch (state) {
  case BmsChargingState::NotCharging:
    return BLE_VAL_CHARGE_NONE;
  case BmsChargingState::TrickleCharge:
    return BLE_VAL_CHARGE_TRICKLE;
  case BmsChargingState::PreCharge:
    return BLE_VAL_CHARGE_PRE;
  case BmsChargingState::FastCharge:
    return BLE_VAL_CHARGE_FAST;
  case BmsChargingState::TaperCharge:
    return BLE_VAL_CHARGE_TAPER;
  case BmsChargingState::TopOffTimerActiveCharging:
    return BLE_VAL_CHARGE_TOPOFF;
  case BmsChargingState::ChargeTerminationDone:
    return BLE_VAL_CHARGE_DONE;
  case BmsChargingState::Unknown:
  default:
    return BLE_VAL_CHARGE_UNKNOWN;
  }
}

const char *BleService::disc_reason_to_str(BleDiscReason reason) {
  switch (reason) {
  case BleDiscReason::Overheat:
    return BLE_VAL_DISC_OVERHEAT;
  case BleDiscReason::LowBatt:
    return BLE_VAL_DISC_LOW_BATT;
  case BleDiscReason::User:
    return BLE_VAL_DISC_USER;
  case BleDiscReason::OpStationary:
    return BLE_VAL_DISC_OP_STATIONARY;
  case BleDiscReason::OpOffline:
    return BLE_VAL_DISC_OP_OFFLINE;
  }
  return BLE_VAL_DISC_USER;
}

const char *BleService::gps_mode_to_str(GpsMode mode) { return gps_mode_to_wire(mode); }

const char *BleService::operating_mode_to_str(OperatingMode mode) {
  return operating_mode_to_wire(mode);
}

// ---------------------------------------------------------------------------
// CBOR decode helpers (called by orchestrator)
// ---------------------------------------------------------------------------

/// Reverse mapping: text string -> GpsMode.
static GpsMode str_to_gps_mode(const char *s) {
  if (strcmp(s, BLE_VAL_GPS_OFF) == 0) {
    return GpsMode::AlwaysOff;
  }
  if (strcmp(s, BLE_VAL_GPS_ALWAYS) == 0) {
    return GpsMode::AlwaysOn;
  }
  return GpsMode::OnWhenTracking; // "tracking" or unrecognized
}

/// Reverse mapping: text string -> OperatingMode.
static OperatingMode str_to_operating_mode(const char *s) {
  if (strcmp(s, BLE_VAL_MODE_PORTABLE) == 0) {
    return OperatingMode::Portable;
  }
  if (strcmp(s, BLE_VAL_MODE_STATIONARY) == 0) {
    return OperatingMode::Stationary;
  }
  return OperatingMode::Offline; // "offline" or unrecognized
}

/// Reverse mapping: CBOR "cmd" string -> BleCommand enum.
static BleCommand str_to_ble_command(const char *s) {
  if (strcmp(s, BLE_VAL_CMD_CO2_CAL) == 0) {
    return BleCommand::Co2Calibration;
  }
  if (strcmp(s, BLE_VAL_CMD_CLEAR_DATA) == 0) {
    return BleCommand::ClearData;
  }
  if (strcmp(s, BLE_VAL_CMD_FACTORY_RST) == 0) {
    return BleCommand::FactoryReset;
  }
  if (strcmp(s, BLE_VAL_CMD_START_TRACKING) == 0) {
    return BleCommand::StartTracking;
  }
  if (strcmp(s, BLE_VAL_CMD_STOP_TRACKING) == 0) {
    return BleCommand::StopTracking;
  }
  if (strcmp(s, BLE_VAL_CMD_SET_AIDING) == 0) {
    return BleCommand::SetAiding;
  }
  return BleCommand::Unknown;
}

/// Forward mapping: BleCommand enum -> wire string for CBOR responses.
static const char *ble_command_to_str(BleCommand cmd) {
  switch (cmd) {
  case BleCommand::Co2Calibration:
    return BLE_VAL_CMD_CO2_CAL;
  case BleCommand::ClearData:
    return BLE_VAL_CMD_CLEAR_DATA;
  case BleCommand::FactoryReset:
    return BLE_VAL_CMD_FACTORY_RST;
  case BleCommand::StartTracking:
    return BLE_VAL_CMD_START_TRACKING;
  case BleCommand::StopTracking:
    return BLE_VAL_CMD_STOP_TRACKING;
  case BleCommand::SetAiding:
    return BLE_VAL_CMD_SET_AIDING;
  case BleCommand::Set:
    return BLE_VAL_CMD_SET;
  case BleCommand::Unknown:
    return BLE_VAL_CMD_UNKNOWN;
  }
  return BLE_VAL_CMD_UNKNOWN;
}

BleConfigDecodeResult BleService::decode_config_write(const uint8_t *buf, size_t len,
                                                      GoSettings &settings) {
  BleConfigDecodeResult result{};
  if (buf == nullptr || len == 0) {
    return result;
  }

  CborParser parser;
  CborValue root;
  if (cbor_parser_init(buf, len, 0, &parser, &root) != CborNoError) {
    return result;
  }
  if (!cbor_value_is_map(&root)) {
    return result;
  }

  CborValue it;
  if (cbor_value_enter_container(&root, &it) != CborNoError) {
    return result;
  }

  char op_str[16] = {};

  // Aiding keys are command arguments, not config. Detected via an explicit
  // parser flag (value inspection is lossy) and rejected under op:"set".
  bool saw_aiding_key = false;

  // Key comparison helper
  auto key_is = [&it](const char *name) -> bool {
    bool match = false;
    cbor_value_text_string_equals(&it, name, &match);
    return match;
  };

  while (!cbor_value_at_end(&it)) {
    if (!cbor_value_is_text_string(&it)) {
      // Skip non-string key + its value
      cbor_value_advance(&it);
      if (!cbor_value_at_end(&it)) {
        cbor_value_advance(&it);
      }
      continue;
    }

    bool handled = false;

    // --- "op" field ---
    if (key_is(BLE_KEY_OP)) {
      cbor_value_advance(&it);
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(op_str) - 1;
        cbor_value_copy_text_string(&it, op_str, &slen, nullptr);
        op_str[slen] = '\0';
      }
      handled = true;
    }
    // --- "cmd" field ---
    else if (key_is(BLE_KEY_CMD)) {
      cbor_value_advance(&it);
      if (cbor_value_is_text_string(&it)) {
        char cmd_str[32] = {};
        size_t slen = sizeof(cmd_str) - 1;
        cbor_value_copy_text_string(&it, cmd_str, &slen, nullptr);
        cmd_str[slen] = '\0';
        result.cmd = str_to_ble_command(cmd_str);
      }
      handled = true;
    }
    // --- uint config fields ---
    else if (key_is(BLE_KEY_MEAS_INT)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        settings.measure_interval_seconds = static_cast<uint32_t>(v);
      }
      handled = true;
    }
    // Deprecated keys — skip value, do not modify settings
    else if (key_is(BLE_KEY_PM_INT) || key_is(BLE_KEY_OTHER_INT) || key_is(BLE_KEY_DISP_INT)) {
      cbor_value_advance(&it);
      handled = true;
    } else if (key_is(BLE_KEY_GPS_INT)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        settings.gps_interval_seconds = static_cast<uint32_t>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_INACT_TO)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        settings.inactivity_timeout_seconds = static_cast<uint32_t>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_AUTO_LOCK)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        settings.auto_lock_seconds = static_cast<uint32_t>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_FRONT_LED)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError &&
          v <= 3) {
        settings.front_led_brightness = static_cast<LedBrightness>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_BACK_LED)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError &&
          v <= 3) {
        settings.back_led_brightness = static_cast<LedBrightness>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_TOUCH_LED)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError &&
          v <= 2) {
        settings.touch_led_intensity = static_cast<TouchLedIntensity>(v);
      }
      handled = true;
    }
    // --- bool config fields ---
    else if (key_is(BLE_KEY_TEMP_F)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      bool v = false;
      if (cbor_value_is_boolean(&it) && cbor_value_get_boolean(&it, &v) == CborNoError) {
        settings.use_fahrenheit = v;
      }
      handled = true;
    } else if (key_is(BLE_KEY_PM_AQI)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      bool v = false;
      if (cbor_value_is_boolean(&it) && cbor_value_get_boolean(&it, &v) == CborNoError) {
        settings.pm_use_usaqi = v;
      }
      handled = true;
    }
    // --- text config fields ---
    else if (key_is(BLE_KEY_GPS_MODE)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      char text[16] = {};
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(text) - 1;
        cbor_value_copy_text_string(&it, text, &slen, nullptr);
        text[slen] = '\0';
        settings.gps_mode = str_to_gps_mode(text);
      }
      handled = true;
    } else if (key_is(BLE_KEY_OP_MODE)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      char text[16] = {};
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(text) - 1;
        cbor_value_copy_text_string(&it, text, &slen, nullptr);
        text[slen] = '\0';
        settings.operating_mode = str_to_operating_mode(text);
      }
      handled = true;
    } else if (key_is(BLE_KEY_DEV_NAME)) {
      cbor_value_advance(&it);
      result.recognized_config_key_count++;
      char text[65] = {};
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(text) - 1;
        cbor_value_copy_text_string(&it, text, &slen, nullptr);
        text[slen] = '\0';
        settings.device_name = text;
      }
      handled = true;
    }
    // --- Aiding command payload fields ---
    else if (key_is(BLE_KEY_LAT)) {
      cbor_value_advance(&it);
      saw_aiding_key = true;
      double v = 0;
      if (cbor_value_is_double(&it) && cbor_value_get_double(&it, &v) == CborNoError) {
        result.aiding.latitude = v;
      } else if (cbor_value_is_float(&it)) {
        float fv = 0;
        if (cbor_value_get_float(&it, &fv) == CborNoError) {
          result.aiding.latitude = static_cast<double>(fv);
        }
      }
      handled = true;
    } else if (key_is(BLE_KEY_LON)) {
      cbor_value_advance(&it);
      saw_aiding_key = true;
      double v = 0;
      if (cbor_value_is_double(&it) && cbor_value_get_double(&it, &v) == CborNoError) {
        result.aiding.longitude = v;
      } else if (cbor_value_is_float(&it)) {
        float fv = 0;
        if (cbor_value_get_float(&it, &fv) == CborNoError) {
          result.aiding.longitude = static_cast<double>(fv);
        }
      }
      handled = true;
    } else if (key_is(BLE_KEY_ALT)) {
      cbor_value_advance(&it);
      saw_aiding_key = true;
      float v = 0;
      if (cbor_value_is_double(&it)) {
        double dv = 0;
        if (cbor_value_get_double(&it, &dv) == CborNoError) {
          v = static_cast<float>(dv);
        }
        result.aiding.altitude_m = v;
      } else if (cbor_value_is_float(&it)) {
        cbor_value_get_float(&it, &v);
        result.aiding.altitude_m = v;
      }
      handled = true;
    } else if (key_is(BLE_KEY_POS_ACC)) {
      cbor_value_advance(&it);
      saw_aiding_key = true;
      float v = 0;
      if (cbor_value_is_double(&it)) {
        double dv = 0;
        if (cbor_value_get_double(&it, &dv) == CborNoError) {
          v = static_cast<float>(dv);
        }
        result.aiding.pos_acc_m = v;
      } else if (cbor_value_is_float(&it)) {
        cbor_value_get_float(&it, &v);
        result.aiding.pos_acc_m = v;
      }
      handled = true;
    } else if (key_is(BLE_KEY_EPOCH)) {
      cbor_value_advance(&it);
      saw_aiding_key = true;
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        result.aiding.epoch_s = static_cast<int64_t>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_TIME_ACC)) {
      cbor_value_advance(&it);
      saw_aiding_key = true;
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        result.aiding.time_acc_ms = static_cast<uint32_t>(v);
      }
      handled = true;
    }

    if (!handled) {
      // Unknown config key — flag it and skip the value
      result.has_unknown_keys = true;
      cbor_value_advance(&it);
    }

    // Advance past value to next key
    if (!cbor_value_at_end(&it)) {
      cbor_value_advance(&it);
    }
  }

  // Determine operation type
  if (strcmp(op_str, BLE_VAL_OP_SET) == 0) {
    result.op = BleConfigOp::Set;
  } else if (strcmp(op_str, BLE_VAL_OP_CMD) == 0) {
    result.op = BleConfigOp::Command;
  }

  // Aiding keys are valid only under op:"cmd" (set_aiding). Under op:"set"
  // they are meaningless — treat as an unknown config key. Resolved post-loop
  // because op is known only after the parse loop completes.
  if (result.op == BleConfigOp::Set && saw_aiding_key) {
    result.has_unknown_keys = true;
  }

  return result;
}

BleHistoryDecodeResult BleService::decode_history_write(const uint8_t *buf, size_t len) {
  BleHistoryDecodeResult result{};
  if (buf == nullptr || len == 0) {
    return result;
  }

  CborParser parser;
  CborValue root;
  if (cbor_parser_init(buf, len, 0, &parser, &root) != CborNoError) {
    return result;
  }
  if (!cbor_value_is_map(&root)) {
    return result;
  }

  CborValue it;
  if (cbor_value_enter_container(&root, &it) != CborNoError) {
    return result;
  }

  char op_str[16] = {};

  auto key_is = [&it](const char *name) -> bool {
    bool match = false;
    cbor_value_text_string_equals(&it, name, &match);
    return match;
  };

  while (!cbor_value_at_end(&it)) {
    if (!cbor_value_is_text_string(&it)) {
      cbor_value_advance(&it);
      if (!cbor_value_at_end(&it)) {
        cbor_value_advance(&it);
      }
      continue;
    }

    bool handled = false;

    if (key_is(BLE_KEY_OP)) {
      cbor_value_advance(&it);
      if (cbor_value_is_text_string(&it)) {
        size_t slen = sizeof(op_str) - 1;
        cbor_value_copy_text_string(&it, op_str, &slen, nullptr);
        op_str[slen] = '\0';
      }
      handled = true;
    } else if (key_is(BLE_KEY_SESSION)) {
      cbor_value_advance(&it);
      uint64_t v = 0;
      if (cbor_value_is_unsigned_integer(&it) && cbor_value_get_uint64(&it, &v) == CborNoError) {
        result.session_id = static_cast<uint32_t>(v);
      }
      handled = true;
    } else if (key_is(BLE_KEY_PTS)) {
      cbor_value_advance(&it);
      if (cbor_value_is_array(&it)) {
        CborValue arr;
        if (cbor_value_enter_container(&it, &arr) == CborNoError) {
          while (!cbor_value_at_end(&arr) &&
                 result.point_count < BleHistoryDecodeResult::MAX_FILL_POINTS) {
            uint64_t v = 0;
            if (cbor_value_is_unsigned_integer(&arr) &&
                cbor_value_get_uint64(&arr, &v) == CborNoError) {
              result.point_indices[result.point_count++] = static_cast<uint32_t>(v);
            }
            cbor_value_advance(&arr);
          }
          cbor_value_leave_container(&it, &arr);
        }
        // After leaving container, it already points past the array — skip
        // the trailing advance below.
        if (!cbor_value_at_end(&it)) {
          cbor_value_advance(&it);
        }
        continue;
      }
      handled = true;
    }

    if (!handled) {
      cbor_value_advance(&it);
    }

    if (!cbor_value_at_end(&it)) {
      cbor_value_advance(&it);
    }
  }

  // Determine operation type
  if (strcmp(op_str, BLE_VAL_OP_LIST) == 0) {
    result.op = BleHistoryOp::List;
  } else if (strcmp(op_str, BLE_VAL_OP_START) == 0) {
    result.op = BleHistoryOp::Start;
  } else if (strcmp(op_str, BLE_VAL_OP_FILL) == 0) {
    result.op = BleHistoryOp::Fill;
  } else if (strcmp(op_str, BLE_VAL_OP_END) == 0) {
    result.op = BleHistoryOp::End;
  } else if (strcmp(op_str, BLE_VAL_OP_DELETE) == 0) {
    result.op = BleHistoryOp::Delete;
  }

  return result;
}
