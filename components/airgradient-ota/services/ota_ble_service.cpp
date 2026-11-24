/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "services/ota_ble_service.h"

#include <cstring>

#include <cbor.h>

#include "ag_log.h"
#include "decimal64.h"
#include "hal/ble_server.h"
#include "services/ota_ble_protocol.h"

// Pull in Kconfig values when building with ESP-IDF; fall back to the spec
// defaults for native host-test builds where no sdkconfig.h exists.
#if defined(__has_include) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifndef CONFIG_AG_OTA_BLE_DATA_MAX_BYTES
#define CONFIG_AG_OTA_BLE_DATA_MAX_BYTES 512
#endif
#ifndef CONFIG_AG_OTA_BLE_CONTROL_MAX_BYTES
#define CONFIG_AG_OTA_BLE_CONTROL_MAX_BYTES 64
#endif
#ifndef CONFIG_AG_OTA_BLE_FW_MAX_LEN
#define CONFIG_AG_OTA_BLE_FW_MAX_LEN 32
#endif
#ifndef CONFIG_AG_OTA_BLE_STALL_TIMEOUT_MS
#define CONFIG_AG_OTA_BLE_STALL_TIMEOUT_MS 10000
#endif
#ifndef CONFIG_AG_OTA_BLE_PROGRESS_INTERVAL_MS
#define CONFIG_AG_OTA_BLE_PROGRESS_INTERVAL_MS 5000
#endif

namespace {

constexpr const char *TAG = "OtaBle";

// OTA GATT UUIDs (service + Control/Data/Status)
constexpr const char *OTA_SERVICE_UUID = "ab9a0001-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_CONTROL_CHAR_UUID = "ab9a0002-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_DATA_CHAR_UUID = "ab9a0003-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_STATUS_CHAR_UUID = "ab9a0004-1e3c-4f5a-9b6d-0a1b2c3d4e5f";

// Control op string buffer — longest accepted op ("start") + NUL.
constexpr size_t OP_BUF_LEN = 8;

// Status NOTIFY CBOR buffer — {state:u8, result:u8, bytes:u32} with headroom.
constexpr size_t STATUS_CBOR_BUF_SIZE = 48;

// Connection-interval windows (ms) the service brackets a transfer with. Fast
// while flashing for throughput; relaxed restored on a non-success terminal.
// These are hints — the central may accept, clamp, or reject them, and the
// call is a no-op under TEST_HOST / on servers without conn-param support.
constexpr uint16_t OTA_CONN_FAST_MIN_MS = 15;
constexpr uint16_t OTA_CONN_FAST_MAX_MS = 30;
constexpr uint16_t OTA_CONN_RELAXED_MIN_MS = 30;
constexpr uint16_t OTA_CONN_RELAXED_MAX_MS = 50;
constexpr uint16_t OTA_CONN_LATENCY = 0;
constexpr uint16_t OTA_CONN_TIMEOUT_MS = 2000;

} // namespace

OtaBleService::OtaBleService(AgBleServer &server, OtaImageWriter &writer)
    : _server(server), _writer(writer), _on_progress(nullptr) {}

OtaBleService::~OtaBleService() { teardown(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool OtaBleService::setup() {
  AgBleGattService *svc = _server.add_service(OTA_SERVICE_UUID);
  if (svc == nullptr) {
    AG_LOGE(TAG, "add OTA service failed");
    return false;
  }

  // Control is Write-With-Response (acknowledges receipt only).
  constexpr uint16_t control_props = AgBleProperty::WRITE | AgBleProperty::WRITE_AUTHEN;
  _control_char = svc->add_characteristic(OTA_CONTROL_CHAR_UUID, control_props);
  if (_control_char == nullptr) {
    AG_LOGE(TAG, "add Control characteristic failed");
    return false;
  }

  // Data is Write-Without-Response: no per-chunk ATT round-trip.
  constexpr uint16_t data_props = AgBleProperty::WRITE_NR | AgBleProperty::WRITE_AUTHEN;
  _data_char = svc->add_characteristic(OTA_DATA_CHAR_UUID, data_props);
  if (_data_char == nullptr) {
    AG_LOGE(TAG, "add Data characteristic failed");
    return false;
  }

  // Status is NOTIFY-only with no readable value. READ_AUTHEN (without READ)
  // gates the CCCD subscription on an authenticated link.
  constexpr uint16_t status_props = AgBleProperty::NOTIFY | AgBleProperty::READ_AUTHEN;
  _status_char = svc->add_characteristic(OTA_STATUS_CHAR_UUID, status_props);
  if (_status_char == nullptr) {
    AG_LOGE(TAG, "add Status characteristic failed");
    return false;
  }

  _control_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { _on_control_write(data, len); });
  _data_char->set_write_callback(
      [this](const uint8_t *data, size_t len) { _on_data_write(data, len); });

  if (!_signal.is_created()) {
    _signal.create();
  }

  if (!svc->start()) {
    AG_LOGE(TAG, "OTA service start failed");
    return false;
  }

  AG_LOGI(TAG, "OTA GATT registered (Control/Data/Status)");
  return true;
}

void OtaBleService::teardown() {
  // Clear write callbacks first so a late write cannot re-enter us.
  if (_control_char != nullptr) {
    _control_char->set_write_callback(nullptr);
  }
  if (_data_char != nullptr) {
    _data_char->set_write_callback(nullptr);
  }

  // Best effort: wake any in-flight run() so it aborts and returns.
  if (is_active()) {
    AG_LOGI(TAG, "teardown: aborting in-flight transfer");
    _latch_abort(OtaStatus::Aborted);
  }

  _control_char = nullptr;
  _data_char = nullptr;
  _status_char = nullptr;
}

void OtaBleService::handle_disconnect() {
  if (!is_active()) {
    return;
  }
  // Link is already gone; the terminal NOTIFY run() emits will simply no-op.
  AG_LOGI(TAG, "disconnect: aborting in-flight transfer");
  _latch_abort(OtaStatus::TransportError);
}

bool OtaBleService::is_active() const { return _admission.load() == Admission::Active; }

bool OtaBleService::block_start() {
  Admission expected = Admission::Idle;
  return _admission.compare_exchange_strong(expected, Admission::Blocked) ||
         expected == Admission::Blocked;
}

void OtaBleService::allow_start() {
  Admission expected = Admission::Blocked;
  _admission.compare_exchange_strong(expected, Admission::Idle);
}

// ---------------------------------------------------------------------------
// Control characteristic (NimBLE host task)
// ---------------------------------------------------------------------------

void OtaBleService::_on_control_write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0) {
    return;
  }
  if (len > CONFIG_AG_OTA_BLE_CONTROL_MAX_BYTES) {
    AG_LOGW(TAG, "oversized control write: %u bytes", static_cast<unsigned>(len));
    if (_state == State::Idle) {
      _reject_start(OtaStatus::InvalidArgument);
    }
    return;
  }

  CborParser parser;
  CborValue map;
  if (cbor_parser_init(data, len, 0, &parser, &map) != CborNoError || !cbor_value_is_map(&map)) {
    AG_LOGW(TAG, "malformed control CBOR");
    if (_state == State::Idle) {
      _reject_start(OtaStatus::InvalidArgument);
    }
    return;
  }

  CborValue op_val;
  if (cbor_value_map_find_value(&map, OTA_BLE_KEY_OP, &op_val) != CborNoError ||
      !cbor_value_is_text_string(&op_val)) {
    AG_LOGW(TAG, "control missing 'op'");
    if (_state == State::Idle) {
      _reject_start(OtaStatus::InvalidArgument);
    }
    return;
  }

  char op[OP_BUF_LEN];
  size_t op_len = sizeof(op);
  if (cbor_value_copy_text_string(&op_val, op, &op_len, nullptr) != CborNoError) {
    AG_LOGW(TAG, "control 'op' too long / unreadable");
    if (_state == State::Idle) {
      _reject_start(OtaStatus::InvalidArgument);
    }
    return;
  }

  if (std::strcmp(op, OTA_BLE_OP_START) == 0) {
    _handle_start(&map);
  } else if (std::strcmp(op, OTA_BLE_OP_END) == 0) {
    _handle_end();
  } else if (std::strcmp(op, OTA_BLE_OP_ABORT) == 0) {
    _handle_abort();
  } else {
    AG_LOGW(TAG, "unknown control op '%s'", op);
  }
}

void OtaBleService::_handle_start(const void *map_ptr) {
  if (_state != State::Idle) {
    AG_LOGW(TAG, "START while active — rejected (no second transfer)");
    return;
  }

  const CborValue *map = static_cast<const CborValue *>(map_ptr);

  // total (required, non-zero, fits u32).
  CborValue total_val;
  if (cbor_value_map_find_value(map, OTA_BLE_KEY_TOTAL, &total_val) != CborNoError ||
      !cbor_value_is_unsigned_integer(&total_val)) {
    AG_LOGW(TAG, "START missing/invalid 'total'");
    _reject_start(OtaStatus::InvalidArgument);
    return;
  }
  uint64_t total = 0;
  cbor_value_get_uint64(&total_val, &total);
  if (total == 0 || total > UINT32_MAX) {
    char total_text[DECIMAL64_BUFFER_SIZE];
    AG_LOGW(TAG, "START rejected: total=%s", format_uint64_decimal(total, total_text));
    _reject_start(OtaStatus::InvalidArgument);
    return;
  }

  // fw (informational, optional). Truncation against FW_MAX_LEN is a reject.
  CborValue fw_val;
  char fw[CONFIG_AG_OTA_BLE_FW_MAX_LEN + 1] = {0};
  if (cbor_value_map_find_value(map, OTA_BLE_KEY_FW, &fw_val) == CborNoError &&
      cbor_value_is_text_string(&fw_val)) {
    size_t fw_len = sizeof(fw);
    if (cbor_value_copy_text_string(&fw_val, fw, &fw_len, nullptr) != CborNoError) {
      AG_LOGW(TAG, "START 'fw' exceeds %d bytes", CONFIG_AG_OTA_BLE_FW_MAX_LEN);
      _reject_start(OtaStatus::InvalidArgument);
      return;
    }
  }
  AG_LOGI(TAG, "START: total=%u fw='%s'", static_cast<unsigned>(total), fw);

  Admission expected = Admission::Idle;
  if (!_admission.compare_exchange_strong(expected, Admission::Active)) {
    _reject_start(OtaStatus::Aborted);
    return;
  }
  _total = static_cast<size_t>(total);
  _bytes_accepted.store(0);
  _pending.store(Cmd::None);
  _state = State::Starting;
  // is_active before begin() so the product can inhibit sleep across the erase.
  _signal.give(); // wake a run() parked waiting for START
}

void OtaBleService::_handle_end() {
  if (_state == State::Downloading && _pending.load() == Cmd::None) {
    _pending.store(Cmd::Finish);
    _signal.give();
  }
  // END while Idle/Starting/Applying or with a terminal pending: ignored.
}

void OtaBleService::_handle_abort() {
  if (_state != State::Idle) {
    AG_LOGI(TAG, "ABORT received from phone");
    _latch_abort(OtaStatus::Aborted);
  }
  // ABORT while Idle: ignored.
}

void OtaBleService::_reject_start(OtaStatus status) {
  // Pre-transfer rejection, emitted on the host task (no run() is live yet).
  _notify_status(OtaState::Failed, status);
}

void OtaBleService::_latch_abort(OtaStatus status) {
  // First latch wins; a later cause does not override the recorded reason.
  Cmd expected = Cmd::None;
  if (!_pending.compare_exchange_strong(expected, Cmd::Abort)) {
    return;
  }
  _terminal_status = status;
  _signal.give();
}

// ---------------------------------------------------------------------------
// Data characteristic (NimBLE host task) — flashes directly from the callback
// ---------------------------------------------------------------------------

void OtaBleService::_on_data_write(const uint8_t *data, size_t len) {
  if (_state == State::Starting) {
    // Phone did not wait for the ready NOTIFY — protocol violation.
    AG_LOGW(TAG, "Data while Starting — protocol violation");
    _latch_abort(OtaStatus::TransportError);
    return;
  }
  if (_state != State::Downloading || _pending.load() != Cmd::None) {
    // Before START, after a terminal latch, or outside Downloading: ignored.
    return;
  }

  if (data == nullptr || len == 0) {
    AG_LOGW(TAG, "empty Data write");
    _latch_abort(OtaStatus::TransportError);
    return;
  }
  if (len > CONFIG_AG_OTA_BLE_DATA_MAX_BYTES) {
    AG_LOGW(TAG, "Data write %u > max %d", static_cast<unsigned>(len),
            CONFIG_AG_OTA_BLE_DATA_MAX_BYTES);
    _latch_abort(OtaStatus::TransportError);
    return;
  }
  if (_bytes_accepted.load() + len > _total) {
    AG_LOGW(TAG, "Data overflow past declared size");
    _latch_abort(OtaStatus::TransportError);
    return;
  }

  const OtaStatus st = _writer.write(data, len);
  if (st != OtaStatus::Ok) {
    AG_LOGE(TAG, "writer.write failed");
    _latch_abort(OtaStatus::FlashError);
    return;
  }
  _bytes_accepted.fetch_add(len);
}

// ---------------------------------------------------------------------------
// Product task: run() + directly-invokable steps (host-testable core)
// ---------------------------------------------------------------------------

void OtaBleService::set_on_progress(OtaProgressCallback cb) { _on_progress = cb; }

OtaStatus OtaBleService::run(uint32_t timeout_ms) {
  // Idle gate: optionally park up to timeout_ms waiting for a START, then bail
  // out cheaply if none latched.
  if (_state != State::Starting && timeout_ms != 0) {
    _signal.take(timeout_ms);
  }
  if (_state != State::Starting) {
    return OtaStatus::Ok; // nothing to drive
  }

  // Start edge on the product task, before the stack-hungry begin().
  _emit_progress(OtaState::Starting);

  if (!_begin_step()) {
    return _result; // begin failure already emitted Failed{FlashError}
  }

  // Downloading: Data callbacks flash chunks. Block on the control signal in
  // PROGRESS_INTERVAL_MS ticks, servicing END/ABORT/disconnect; on each tick
  // log progress and accumulate idle time, aborting once it reaches the stall
  // window (Data writes never signal, so ticks pace the log during streaming).
  size_t last_bytes = _bytes_accepted.load();
  uint32_t idle_ms = 0;
  for (;;) {
    const bool signalled = _signal.take(CONFIG_AG_OTA_BLE_PROGRESS_INTERVAL_MS);

    const Cmd cmd = _pending.load();
    if (cmd == Cmd::Finish) {
      _finish_step();
      return _result;
    }
    if (cmd == Cmd::Abort) {
      _terminate(_terminal_status);
      return _result;
    }

    if (!signalled) {
      const size_t now = _bytes_accepted.load();
      if (now != last_bytes) {
        last_bytes = now;
        idle_ms = 0;
      } else {
        idle_ms += CONFIG_AG_OTA_BLE_PROGRESS_INTERVAL_MS;
        if (idle_ms >= CONFIG_AG_OTA_BLE_STALL_TIMEOUT_MS) {
          AG_LOGW(TAG, "aborting: no data received (stall)");
          _terminate(OtaStatus::TransportError);
          return _result;
        }
      }
      const unsigned pct = _total > 0 ? static_cast<unsigned>(now * 100 / _total) : 0;
      AG_LOGI(TAG, "progress: %u%% (%u/%u bytes)", pct, static_cast<unsigned>(now),
              static_cast<unsigned>(_total));
      // Push the device's real byte count so the phone shows true progress.
      _notify_status(OtaState::Downloading, OtaStatus::Ok);
      _emit_progress(OtaState::Downloading);
    }
  }
}

bool OtaBleService::_begin_step() {
  // Bracket the transfer with the fast conn-param window before the erase so
  // the whole flash flows on the throughput-optimised interval.
  _request_fast_conn_params();

  const OtaStatus st = _writer.begin(_total);
  if (st != OtaStatus::Ok) {
    AG_LOGE(TAG, "writer.begin failed");
    _emit_terminal(OtaState::Failed, OtaStatus::FlashError);
    return false;
  }
  _state = State::Downloading;
  AG_LOGI(TAG, "downloading: partition ready, expecting %u bytes", static_cast<unsigned>(_total));
  // The "ready" signal: the phone waits for this NOTIFY before sending Data.
  _notify_status(OtaState::Downloading, OtaStatus::Ok);
  _emit_progress(OtaState::Downloading);
  return true;
}

void OtaBleService::_finish_step() {
  if (_bytes_accepted.load() != _total) {
    AG_LOGE(TAG, "END truncated: %u of %u bytes", static_cast<unsigned>(_bytes_accepted.load()),
            static_cast<unsigned>(_total));
    _writer.abort();
    _emit_terminal(OtaState::Failed, OtaStatus::TransportError);
    return;
  }

  _state = State::Applying;
  AG_LOGI(TAG, "applying: %u bytes received", static_cast<unsigned>(_total));
  _notify_status(OtaState::Applying, OtaStatus::Ok);
  _emit_progress(OtaState::Applying);

  const OtaStatus st = _writer.finish();
  if (st == OtaStatus::Ok) {
    AG_LOGI(TAG, "done: image staged, reboot to run");
    _emit_terminal(OtaState::Done, OtaStatus::Ok);
  } else {
    AG_LOGE(TAG, "writer.finish failed");
    _writer.abort();
    _emit_terminal(OtaState::Failed, st);
  }
}

void OtaBleService::_terminate(OtaStatus status) {
  _writer.abort();
  _emit_terminal(OtaState::Failed, status);
}

// ---------------------------------------------------------------------------
// Status emission
// ---------------------------------------------------------------------------

void OtaBleService::_emit_terminal(OtaState state, OtaStatus status) {
  // Always attempt the terminal NOTIFY; on a dropped link it no-ops.
  _notify_status(state, status);
  _emit_progress(state);
  // Restore the relaxed window only on failure; a successful update reboots,
  // so the fast window is irrelevant and skipping the restore avoids a
  // needless conn-param renegotiation before the reset.
  if (status != OtaStatus::Ok) {
    _restore_conn_params();
  }
  _result = status;
  _pending.store(Cmd::None);
  _state = State::Idle;
  _admission.store(Admission::Idle);
}

void OtaBleService::_request_fast_conn_params() {
  _server.request_conn_params(OTA_CONN_FAST_MIN_MS, OTA_CONN_FAST_MAX_MS, OTA_CONN_LATENCY,
                              OTA_CONN_TIMEOUT_MS);
}

void OtaBleService::_restore_conn_params() {
  _server.request_conn_params(OTA_CONN_RELAXED_MIN_MS, OTA_CONN_RELAXED_MAX_MS, OTA_CONN_LATENCY,
                              OTA_CONN_TIMEOUT_MS);
}

void OtaBleService::_emit_progress(OtaState state) {
  if (_on_progress == nullptr) {
    return;
  }

  const size_t written = _bytes_accepted.load();
  uint8_t percent = 0;
  if (_total > 0) {
    size_t value = (written * 100) / _total;
    if (value > 100) {
      value = 100;
    }
    percent = static_cast<uint8_t>(value);
  }

  OtaProgress progress{state, written, _total, percent};
  _on_progress(progress);
}

void OtaBleService::_notify_status(OtaState state, OtaStatus status) {
  if (_status_char == nullptr) {
    return;
  }

  uint8_t buf[STATUS_CBOR_BUF_SIZE];
  CborEncoder encoder;
  cbor_encoder_init(&encoder, buf, sizeof(buf), 0);

  CborEncoder map;
  cbor_encoder_create_map(&encoder, &map, 3);
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_STATE);
  cbor_encode_uint(&map, ota_ble_protocol::to_wire(state));
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_RESULT);
  cbor_encode_uint(&map, ota_ble_protocol::to_wire(status));
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_BYTES);
  cbor_encode_uint(&map, _bytes_accepted.load());
  cbor_encoder_close_container(&encoder, &map);

  if (cbor_encoder_get_extra_bytes_needed(&encoder) != 0) {
    AG_LOGW(TAG, "status encode overflow");
    return;
  }
  const size_t len = cbor_encoder_get_buffer_size(&encoder, buf);
  // NOTIFY-only: never set_value(); the Status characteristic stores no value.
  _status_char->notify(buf, len);
}
