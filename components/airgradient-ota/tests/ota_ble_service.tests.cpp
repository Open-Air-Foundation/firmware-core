/**
 * AirGradient — OtaBleService host tests (v2 push model)
 *
 * Drives the protocol/state core under TEST_HOST against a mock AgBleServer
 * and a fake OtaImageWriter: CBOR Control decode + bounds, the wire-constant
 * mapping, the Idle -> Starting -> Downloading -> Applying state machine, the
 * begin/write/finish sequencing, byte-count/framing rules, NOTIFY-only Status
 * emission, the rejection rules, the progress callback, and the run() /
 * is_active lifecycle.
 *
 * Data is flashed directly in the real _on_data_write() callback (via
 * write_data). The begin/finish/abort steps that run() drives on the product
 * task are pumped explicitly (begin_step/finish_step/terminate); the blocking
 * run() loop and the live stall watchdog are no-ops under TEST_HOST and are
 * verified by HIL.
 */

#include "services/ota_ble_protocol.h"
#include "services/ota_ble_service.h"

#include "fake_ota_image_writer.h"
#include "mock_ble.h"
#include "ota_ble_service_test_access.h"

#include <cbor.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

// Placeholder UUIDs — must match ota_ble_service.cpp.
constexpr const char *OTA_SERVICE_UUID = "ab9a0001-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_CONTROL_CHAR_UUID = "ab9a0002-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_DATA_CHAR_UUID = "ab9a0003-1e3c-4f5a-9b6d-0a1b2c3d4e5f";
constexpr const char *OTA_STATUS_CHAR_UUID = "ab9a0004-1e3c-4f5a-9b6d-0a1b2c3d4e5f";

// -- CBOR control encoders --------------------------------------------------

std::vector<uint8_t> encode_start(uint64_t total, const char *fw) {
  uint8_t buf[128];
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, fw != nullptr ? 3 : 2);
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_OP);
  cbor_encode_text_stringz(&map, OTA_BLE_OP_START);
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_TOTAL);
  cbor_encode_uint(&map, total);
  if (fw != nullptr) {
    cbor_encode_text_stringz(&map, OTA_BLE_KEY_FW);
    cbor_encode_text_stringz(&map, fw);
  }
  cbor_encoder_close_container(&enc, &map);
  size_t len = cbor_encoder_get_buffer_size(&enc, buf);
  return std::vector<uint8_t>(buf, buf + len);
}

std::vector<uint8_t> encode_op(const char *op) {
  uint8_t buf[32];
  CborEncoder enc;
  cbor_encoder_init(&enc, buf, sizeof(buf), 0);
  CborEncoder map;
  cbor_encoder_create_map(&enc, &map, 1);
  cbor_encode_text_stringz(&map, OTA_BLE_KEY_OP);
  cbor_encode_text_stringz(&map, op);
  cbor_encoder_close_container(&enc, &map);
  size_t len = cbor_encoder_get_buffer_size(&enc, buf);
  return std::vector<uint8_t>(buf, buf + len);
}

// Decode a Status NOTIFY payload {state, result, bytes} into a pair of wire
// bytes (state, result); the bytes field is read separately via decode_bytes.
std::pair<uint8_t, uint8_t> decode_status(const std::vector<uint8_t> &payload) {
  CborParser parser;
  CborValue map;
  REQUIRE(cbor_parser_init(payload.data(), payload.size(), 0, &parser, &map) == CborNoError);
  REQUIRE(cbor_value_is_map(&map));

  CborValue v;
  uint64_t state = 0;
  uint64_t result = 0;
  REQUIRE(cbor_value_map_find_value(&map, OTA_BLE_KEY_STATE, &v) == CborNoError);
  REQUIRE(cbor_value_is_unsigned_integer(&v));
  cbor_value_get_uint64(&v, &state);
  REQUIRE(cbor_value_map_find_value(&map, OTA_BLE_KEY_RESULT, &v) == CborNoError);
  REQUIRE(cbor_value_is_unsigned_integer(&v));
  cbor_value_get_uint64(&v, &result);
  return {static_cast<uint8_t>(state), static_cast<uint8_t>(result)};
}

// Read the `bytes` progress field from a Status NOTIFY payload.
uint64_t decode_bytes(const std::vector<uint8_t> &payload) {
  CborParser parser;
  CborValue map;
  REQUIRE(cbor_parser_init(payload.data(), payload.size(), 0, &parser, &map) == CborNoError);
  REQUIRE(cbor_value_is_map(&map));

  CborValue v;
  uint64_t bytes = 0;
  REQUIRE(cbor_value_map_find_value(&map, OTA_BLE_KEY_BYTES, &v) == CborNoError);
  REQUIRE(cbor_value_is_unsigned_integer(&v));
  cbor_value_get_uint64(&v, &bytes);
  return bytes;
}

// Test harness bundling the mock server, fake writer, service, and the
// captured characteristics.
struct Harness {
  MockBleServer server;
  FakeOtaImageWriter writer;
  OtaBleService svc{server, writer};
  OtaBleServiceTestAccess access{svc};
  MockBleCharacteristic *control = nullptr;
  MockBleCharacteristic *data = nullptr;
  MockBleCharacteristic *status = nullptr;

  Harness() {
    REQUIRE(svc.setup());
    control = server.find_char(OTA_SERVICE_UUID, OTA_CONTROL_CHAR_UUID);
    data = server.find_char(OTA_SERVICE_UUID, OTA_DATA_CHAR_UUID);
    status = server.find_char(OTA_SERVICE_UUID, OTA_STATUS_CHAR_UUID);
    REQUIRE(control != nullptr);
    REQUIRE(data != nullptr);
    REQUIRE(status != nullptr);
  }

  void write_control(const std::vector<uint8_t> &p) { control->simulate_write(p.data(), p.size()); }
  void write_data(const std::vector<uint8_t> &p) { data->simulate_write(p.data(), p.size()); }
  void write_data(const uint8_t *p, size_t n) { data->simulate_write(p, n); }

  // Forwarders to the friend test-access (private steps + internal state).
  bool begin_step() { return access.begin_step(); }
  void finish_step() { access.finish_step(); }
  void terminate(OtaStatus status) { access.terminate(status); }
  void emit_progress(OtaState state) { access.emit_progress(state); }
  uint8_t internal_state() const { return access.internal_state(); }
  OtaStatus pending_terminal_status() const { return access.pending_terminal_status(); }
  bool finish_pending() const { return access.finish_pending(); }
  bool abort_pending() const { return access.abort_pending(); }

  std::pair<uint8_t, uint8_t> last_status() const {
    return decode_status(status->all_notified.back());
  }
};

// Internal-state wire values from OtaBleService::State.
constexpr uint8_t ST_IDLE = 0;
constexpr uint8_t ST_STARTING = 1;
constexpr uint8_t ST_DOWNLOADING = 2;
constexpr uint8_t ST_APPLYING = 3;

// Status NOTIFY wire pairs.
constexpr std::pair<uint8_t, uint8_t> DOWNLOADING_OK{0x01, 0x00};
constexpr std::pair<uint8_t, uint8_t> APPLYING_OK{0x02, 0x00};
constexpr std::pair<uint8_t, uint8_t> DONE_OK{0x03, 0x00};
constexpr std::pair<uint8_t, uint8_t> FAILED_FLASH{0x04, 0x01};
constexpr std::pair<uint8_t, uint8_t> FAILED_INVALID_IMAGE{0x04, 0x02};
constexpr std::pair<uint8_t, uint8_t> FAILED_TRANSPORT{0x04, 0x03};
constexpr std::pair<uint8_t, uint8_t> FAILED_ABORTED{0x04, 0x04};
constexpr std::pair<uint8_t, uint8_t> FAILED_INVALID_ARG{0x04, 0x05};

} // namespace

// ===========================================================================
// setup / GATT registration
// ===========================================================================

TEST_CASE("setup registers OTA service with Control/Data/Status characteristics") {
  Harness h;
  MockBleGattService *svc = h.server.find_service(OTA_SERVICE_UUID);
  REQUIRE(svc != nullptr);
  REQUIRE(svc->started);

  // Control: WRITE | WRITE_AUTHEN. Data: WRITE_NR | WRITE_AUTHEN.
  // Status: NOTIFY | READ_AUTHEN (no READ).
  REQUIRE(svc->props_of(OTA_CONTROL_CHAR_UUID) ==
          (AgBleProperty::WRITE | AgBleProperty::WRITE_AUTHEN));
  REQUIRE(svc->props_of(OTA_DATA_CHAR_UUID) ==
          (AgBleProperty::WRITE_NR | AgBleProperty::WRITE_AUTHEN));
  REQUIRE(svc->props_of(OTA_STATUS_CHAR_UUID) ==
          (AgBleProperty::NOTIFY | AgBleProperty::READ_AUTHEN));
  REQUIRE((svc->props_of(OTA_STATUS_CHAR_UUID) & AgBleProperty::READ) == 0);

  REQUIRE(h.control->has_write_callback());
  REQUIRE(h.data->has_write_callback());
}

TEST_CASE("setup returns false when the service cannot be added") {
  MockBleServer server;
  server.add_service_returns = false;
  FakeOtaImageWriter writer;
  OtaBleService svc{server, writer};
  REQUIRE_FALSE(svc.setup());
}

// ===========================================================================
// Wire constants
// ===========================================================================

TEST_CASE("to_wire pins the frozen state/result bytes") {
  using namespace ota_ble_protocol;
  REQUIRE(to_wire(OtaState::Downloading) == 0x01);
  REQUIRE(to_wire(OtaState::Applying) == 0x02);
  REQUIRE(to_wire(OtaState::Done) == 0x03);
  REQUIRE(to_wire(OtaState::Failed) == 0x04);

  REQUIRE(to_wire(OtaStatus::Ok) == 0x00);
  REQUIRE(to_wire(OtaStatus::FlashError) == 0x01);
  REQUIRE(to_wire(OtaStatus::InvalidImage) == 0x02);
  REQUIRE(to_wire(OtaStatus::TransportError) == 0x03);
  REQUIRE(to_wire(OtaStatus::Aborted) == 0x04);
  REQUIRE(to_wire(OtaStatus::InvalidArgument) == 0x05);
}

// ===========================================================================
// Happy path / sequencing
// ===========================================================================

TEST_CASE("valid START latches Starting until begin_step runs") {
  Harness h;
  h.write_control(encode_start(1024, "3.2.0"));

  REQUIRE(h.internal_state() == ST_STARTING);
  REQUIRE(h.svc.is_active());           // set at START, before begin()
  REQUIRE_FALSE(h.writer.begin_called); // begin runs in run(), not here
  REQUIRE(h.status->notify_count == 0);

  REQUIRE(h.begin_step());
  REQUIRE(h.writer.begin_called);
  REQUIRE(h.writer.begin_total == 1024);
  REQUIRE(h.internal_state() == ST_DOWNLOADING);

  // The ready signal: NOTIFY Downloading{Ok} with bytes=0 so far.
  REQUIRE(h.status->notify_count == 1);
  REQUIRE(h.last_status() == DOWNLOADING_OK);
  REQUIRE(decode_bytes(h.status->all_notified.back()) == 0);
}

TEST_CASE("full transfer: START -> chunks -> END -> Done") {
  Harness h;
  h.write_control(encode_start(6, "fw"));
  REQUIRE(h.begin_step());

  // Data is flashed directly in the write callback (no separate drain step).
  h.write_data(std::vector<uint8_t>{1, 2, 3});
  h.write_data(std::vector<uint8_t>{4, 5, 6});

  REQUIRE(h.writer.write_calls == 2);
  REQUIRE(h.writer.bytes_written() == 6);

  // No periodic progress NOTIFY here: the 5 s tick lives in the run() loop,
  // which host tests do not run. Only the ready NOTIFY has fired so far.
  REQUIRE(h.status->notify_count == 1);

  h.write_control(encode_op(OTA_BLE_OP_END));
  REQUIRE(h.finish_pending()); // END latched FINISH for run()
  h.finish_step();

  REQUIRE(h.writer.finish_called);
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.svc.is_active());

  // Applying then Done NOTIFY, both carrying the full byte count.
  REQUIRE(h.status->notify_count == 3);
  REQUIRE(decode_status(h.status->all_notified[1]) == APPLYING_OK);
  REQUIRE(decode_status(h.status->all_notified[2]) == DONE_OK);
  REQUIRE(decode_bytes(h.status->all_notified[1]) == 6);
  REQUIRE(decode_bytes(h.status->all_notified[2]) == 6);
}

TEST_CASE("Status is NOTIFY-only — never sets a stored READ value") {
  Harness h;
  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.begin_step());
  h.write_data(std::vector<uint8_t>{7, 8, 9});
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();

  REQUIRE(h.status->set_value_count == 0);
  REQUIRE(h.status->notify_count == 3);
}

// ===========================================================================
// Connection-interval bracketing (service-owned)
// ===========================================================================

namespace {

// Expected conn-param windows owned by ota_ble_service.cpp.
constexpr uint16_t CONN_FAST_MIN_MS = 15;
constexpr uint16_t CONN_FAST_MAX_MS = 30;
constexpr uint16_t CONN_RELAXED_MIN_MS = 30;
constexpr uint16_t CONN_RELAXED_MAX_MS = 50;

bool is_fast(const MockBleServer::ConnParamRequest &r) {
  return r.min_interval_ms == CONN_FAST_MIN_MS && r.max_interval_ms == CONN_FAST_MAX_MS;
}

bool is_relaxed(const MockBleServer::ConnParamRequest &r) {
  return r.min_interval_ms == CONN_RELAXED_MIN_MS && r.max_interval_ms == CONN_RELAXED_MAX_MS;
}

} // namespace

TEST_CASE("begin_step requests the fast conn-param window") {
  Harness h;
  h.write_control(encode_start(6, "fw"));
  REQUIRE(h.server.conn_param_requests.empty()); // not before begin

  REQUIRE(h.begin_step());
  REQUIRE(h.server.conn_param_requests.size() == 1);
  REQUIRE(is_fast(h.server.conn_param_requests.front()));
}

TEST_CASE("successful transfer keeps the fast window (no restore before reboot)") {
  Harness h;
  h.write_control(encode_start(6, "fw"));
  REQUIRE(h.begin_step());
  h.write_data(std::vector<uint8_t>{1, 2, 3});
  h.write_data(std::vector<uint8_t>{4, 5, 6});
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();

  // Only the fast request; Done does not restore (product reboots).
  REQUIRE(h.server.conn_param_requests.size() == 1);
  REQUIRE(is_fast(h.server.conn_param_requests.front()));
}

TEST_CASE("aborted transfer restores the relaxed window") {
  Harness h;
  h.write_control(encode_start(6, "fw"));
  REQUIRE(h.begin_step());
  h.terminate(OtaStatus::Aborted);

  REQUIRE(h.server.conn_param_requests.size() == 2);
  REQUIRE(is_fast(h.server.conn_param_requests.front()));
  REQUIRE(is_relaxed(h.server.conn_param_requests.back()));
}

TEST_CASE("truncated END restores the relaxed window") {
  Harness h;
  h.write_control(encode_start(6, "fw"));
  REQUIRE(h.begin_step());
  h.write_data(std::vector<uint8_t>{1, 2, 3}); // short of declared 6
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();

  REQUIRE(h.last_status() == FAILED_TRANSPORT);
  REQUIRE(h.server.conn_param_requests.size() == 2);
  REQUIRE(is_relaxed(h.server.conn_param_requests.back()));
}

// ===========================================================================
// Control decode + bounds
// ===========================================================================

TEST_CASE("malformed control CBOR while Idle is rejected InvalidArgument") {
  Harness h;
  h.write_control(std::vector<uint8_t>{0xFF, 0xFF, 0xFF});

  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.svc.is_active());
  REQUIRE_FALSE(h.writer.begin_called);
  REQUIRE(h.status->notify_count == 1);
  REQUIRE(h.last_status() == FAILED_INVALID_ARG);
}

TEST_CASE("oversized control write is rejected InvalidArgument") {
  Harness h;
  h.write_control(std::vector<uint8_t>(128, 0x20)); // > CONTROL_MAX_BYTES (64)
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(h.last_status() == FAILED_INVALID_ARG);
}

TEST_CASE("START with total=0 is rejected InvalidArgument") {
  Harness h;
  h.write_control(encode_start(0, "fw"));
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.writer.begin_called);
  REQUIRE(h.last_status() == FAILED_INVALID_ARG);
}

TEST_CASE("START with an over-long fw string is rejected InvalidArgument") {
  Harness h;
  std::string long_fw(64, 'x'); // > FW_MAX_LEN (32)
  h.write_control(encode_start(1024, long_fw.c_str()));
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(h.last_status() == FAILED_INVALID_ARG);
}

TEST_CASE("START without fw is accepted (fw is informational)") {
  Harness h;
  h.write_control(encode_start(512, nullptr));
  REQUIRE(h.internal_state() == ST_STARTING);
  REQUIRE(h.svc.is_active());
}

// ===========================================================================
// Byte-count / framing rules
// ===========================================================================

TEST_CASE("oversized Data write records a TransportError abort") {
  Harness h;
  h.write_control(encode_start(4096, nullptr));
  REQUIRE(h.begin_step());

  h.write_data(std::vector<uint8_t>(1024, 0xAB)); // > DATA_MAX_BYTES (512)
  REQUIRE(h.abort_pending());
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);
  REQUIRE(h.writer.write_calls == 0); // never written

  h.terminate(h.pending_terminal_status());
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE_FALSE(h.writer.finish_called);
  REQUIRE(h.last_status() == FAILED_TRANSPORT);
  REQUIRE(h.internal_state() == ST_IDLE);
}

TEST_CASE("empty Data while Downloading records a TransportError abort") {
  Harness h;
  h.write_control(encode_start(64, nullptr));
  REQUIRE(h.begin_step());
  h.write_data(nullptr, 0);
  REQUIRE(h.abort_pending());
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);
}

TEST_CASE("Data overflow past the declared total records a TransportError abort") {
  Harness h;
  h.write_control(encode_start(4, nullptr)); // total = 4
  REQUIRE(h.begin_step());
  h.write_data(std::vector<uint8_t>{1, 2, 3, 4, 5}); // 5 > 4
  REQUIRE(h.abort_pending());
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);
  REQUIRE(h.writer.write_calls == 0);
}

TEST_CASE("early END (truncated) aborts with TransportError and never finishes") {
  Harness h;
  h.write_control(encode_start(10, nullptr));
  REQUIRE(h.begin_step());
  h.write_data(std::vector<uint8_t>{1, 2, 3}); // only 3 of 10

  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step(); // bytes_accepted (3) != total (10)

  REQUIRE_FALSE(h.writer.finish_called);
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE(h.last_status() == FAILED_TRANSPORT);
  REQUIRE(h.internal_state() == ST_IDLE);
}

TEST_CASE("Data while Starting (before the ready NOTIFY) is a protocol violation") {
  Harness h;
  h.write_control(encode_start(64, nullptr));
  REQUIRE(h.internal_state() == ST_STARTING);

  h.write_data(std::vector<uint8_t>{1, 2, 3});
  REQUIRE(h.abort_pending());
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);
  REQUIRE(h.writer.write_calls == 0);
}

// ===========================================================================
// Chunk-write error
// ===========================================================================

TEST_CASE("a writer.write failure latches FlashError; terminal emits one Failed{FlashError}") {
  Harness h;
  h.write_control(encode_start(8, nullptr));
  REQUIRE(h.begin_step());

  h.writer.write_status = OtaStatus::FlashError;
  h.write_data(std::vector<uint8_t>{1, 2, 3, 4});
  REQUIRE(h.writer.write_calls == 1);
  REQUIRE(h.abort_pending());
  REQUIRE(h.pending_terminal_status() == OtaStatus::FlashError);

  h.terminate(h.pending_terminal_status());
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE_FALSE(h.writer.finish_called);
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.svc.is_active());

  // Exactly one Failed NOTIFY (ready + Failed = 2 total).
  REQUIRE(h.status->notify_count == 2);
  REQUIRE(h.last_status() == FAILED_FLASH);
}

// ===========================================================================
// begin / finish flash failures
// ===========================================================================

TEST_CASE("writer.begin failure terminates with FlashError") {
  Harness h;
  h.writer.begin_status = OtaStatus::FlashError;
  h.write_control(encode_start(64, nullptr));
  REQUIRE_FALSE(h.begin_step());

  REQUIRE_FALSE(h.svc.is_active());
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(h.last_status() == FAILED_FLASH);
}

TEST_CASE("finish validation failure maps to InvalidImage") {
  Harness h;
  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.begin_step());
  h.write_data(std::vector<uint8_t>{1, 2, 3});

  h.writer.finish_status = OtaStatus::InvalidImage;
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();

  REQUIRE(h.writer.finish_called);
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE(h.last_status() == FAILED_INVALID_IMAGE);
}

// ===========================================================================
// Failure mapping: ABORT / disconnect / teardown
// ===========================================================================

TEST_CASE("Control ABORT maps to Aborted with a NOTIFY") {
  Harness h;
  h.write_control(encode_start(64, nullptr));
  REQUIRE(h.begin_step());

  h.write_control(encode_op(OTA_BLE_OP_ABORT));
  REQUIRE(h.abort_pending());
  REQUIRE(h.pending_terminal_status() == OtaStatus::Aborted);

  h.terminate(h.pending_terminal_status());
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE(h.last_status() == FAILED_ABORTED);
}

TEST_CASE("disconnect maps to TransportError; terminal NOTIFY is attempted") {
  Harness h;
  h.write_control(encode_start(64, nullptr));
  REQUIRE(h.begin_step());
  const int notifies_before = h.status->notify_count;

  h.svc.handle_disconnect();
  REQUIRE(h.abort_pending());
  REQUIRE(h.pending_terminal_status() == OtaStatus::TransportError);

  // On real hardware the NOTIFY no-ops on the dropped link; the mock still
  // records the attempt with the mapped status.
  h.terminate(h.pending_terminal_status());
  REQUIRE(h.writer.abort_calls == 1);
  REQUIRE(h.status->notify_count == notifies_before + 1);
  REQUIRE(h.last_status() == FAILED_TRANSPORT);
}

TEST_CASE("disconnect while idle is a no-op") {
  Harness h;
  h.svc.handle_disconnect();
  REQUIRE_FALSE(h.svc.is_active());
  REQUIRE_FALSE(h.abort_pending());
}

TEST_CASE("teardown mid-transfer latches Aborted") {
  Harness h;
  h.write_control(encode_start(64, nullptr));
  REQUIRE(h.begin_step());

  h.svc.teardown();
  REQUIRE(h.abort_pending());
  REQUIRE(h.pending_terminal_status() == OtaStatus::Aborted);
}

// ===========================================================================
// Rejection rules
// ===========================================================================

TEST_CASE("Data before START is ignored") {
  Harness h;
  h.write_data(std::vector<uint8_t>{1, 2, 3});
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(h.writer.write_calls == 0);
  REQUIRE_FALSE(h.abort_pending());
}

TEST_CASE("a second START while active is rejected (no second transfer)") {
  Harness h;
  h.write_control(encode_start(64, "a"));
  REQUIRE(h.begin_step());

  h.write_control(encode_start(128, "b"));
  REQUIRE(h.internal_state() == ST_DOWNLOADING);
  REQUIRE(h.writer.begin_total == 64);  // unchanged
  REQUIRE(h.status->notify_count == 1); // no new NOTIFY
}

TEST_CASE("END while Idle is ignored") {
  Harness h;
  h.write_control(encode_op(OTA_BLE_OP_END));
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.writer.finish_called);
  REQUIRE_FALSE(h.finish_pending());
}

TEST_CASE("ABORT while Idle is ignored") {
  Harness h;
  h.write_control(encode_op(OTA_BLE_OP_ABORT));
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.abort_pending());
}

// ===========================================================================
// run / is_active lifecycle
// ===========================================================================

TEST_CASE("run returns Ok immediately while idle") {
  Harness h;
  // No START latched: the idle gate returns without driving a transfer.
  REQUIRE(h.svc.run() == OtaStatus::Ok);
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE_FALSE(h.svc.is_active());
  REQUIRE_FALSE(h.writer.begin_called);
}

TEST_CASE("START latches Starting (is_active) for run to drive") {
  Harness h;
  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.internal_state() == ST_STARTING);
  REQUIRE(h.svc.is_active());
}

TEST_CASE("is_active is false before START, true mid-transfer, false at the terminal") {
  Harness h;
  REQUIRE_FALSE(h.svc.is_active());

  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.svc.is_active());
  REQUIRE(h.begin_step());
  REQUIRE(h.svc.is_active());

  h.write_data(std::vector<uint8_t>{1, 2, 3});
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();

  REQUIRE_FALSE(h.svc.is_active());
}

TEST_CASE("a fresh START after a completed transfer is accepted") {
  Harness h;
  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.begin_step());
  h.write_data(std::vector<uint8_t>{1, 2, 3});
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step();
  REQUIRE(h.internal_state() == ST_IDLE);

  // Second transfer.
  h.write_control(encode_start(2, nullptr));
  REQUIRE(h.internal_state() == ST_STARTING);
  REQUIRE(h.begin_step());
  REQUIRE(h.writer.begin_total == 2);
}

// ===========================================================================
// progress callback (set_on_progress)
// ===========================================================================

TEST_CASE("set_on_progress reports the start edge with byte/percent fields") {
  Harness h;
  std::vector<OtaProgress> events;
  h.svc.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  h.write_control(encode_start(1000, "fw"));
  h.emit_progress(OtaState::Starting); // run() fires this before begin()

  REQUIRE(events.size() == 1);
  REQUIRE(events.back().state == OtaState::Starting);
  REQUIRE(events.back().bytes_written == 0);
  REQUIRE(events.back().total_size == 1000);
  REQUIRE(events.back().percent == 0);
}

TEST_CASE("set_on_progress reports Downloading/Applying/Done across a transfer") {
  Harness h;
  std::vector<OtaProgress> events;
  h.svc.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  h.write_control(encode_start(4, "fw"));
  REQUIRE(h.begin_step()); // emits Downloading (ready), bytes=0

  h.write_data(std::vector<uint8_t>{1, 2, 3, 4}); // 100% flashed
  h.write_control(encode_op(OTA_BLE_OP_END));
  h.finish_step(); // emits Applying then Done

  REQUIRE(events.size() == 3);
  REQUIRE(events[0].state == OtaState::Downloading);
  REQUIRE(events[0].bytes_written == 0);
  REQUIRE(events[1].state == OtaState::Applying);
  REQUIRE(events[1].bytes_written == 4);
  REQUIRE(events[1].percent == 100);
  REQUIRE(events[2].state == OtaState::Done);
  REQUIRE(events[2].bytes_written == 4);
}

TEST_CASE("set_on_progress reports the terminal Failed state on abort") {
  Harness h;
  std::vector<OtaProgress> events;
  h.svc.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  h.write_control(encode_start(8, nullptr));
  REQUIRE(h.begin_step());
  h.terminate(OtaStatus::TransportError);

  REQUIRE(events.size() == 2);
  REQUIRE(events.back().state == OtaState::Failed);
}

TEST_CASE("a pre-transfer START reject does not fire the progress callback") {
  Harness h;
  std::vector<OtaProgress> events;
  h.svc.set_on_progress([&](const OtaProgress &p) { events.push_back(p); });

  h.write_control(encode_start(0, nullptr)); // total=0 -> rejected, stays Idle
  REQUIRE(h.internal_state() == ST_IDLE);
  REQUIRE(events.empty());
}

TEST_CASE("product admission rejects pending OTA before flash and permits a later retry") {
  Harness h;
  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.svc.is_active());
  CHECK(h.svc.run(0, false) == OtaStatus::InvalidArgument);
  CHECK(h.last_status() == FAILED_INVALID_ARG);
  CHECK_FALSE(h.svc.is_active());
  CHECK_FALSE(h.writer.begin_called);
  h.write_data(std::vector<uint8_t>{1, 2, 3});
  CHECK(h.writer.write_calls == 0);
  h.write_control(encode_start(3, nullptr));
  REQUIRE(h.svc.is_active());
  REQUIRE(h.begin_step());
  h.write_data(std::vector<uint8_t>{1, 2, 3});
  h.finish_step();
  CHECK(h.last_status() == DONE_OK);
}
