#include "serial_command/serial_command.h"

#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "go_events.h"

namespace {

constexpr size_t SERIAL_COMMAND_READ_BUFFER_BYTES = 64;
constexpr size_t SERIAL_COMMAND_MAX_TOKENS = 5;

constexpr char HELP_RESPONSE[] =
    "\n#AG OK COMMANDS HELP GET_SERIAL SET_SLR <PM|TEMP|HUM> <scale> <intercept> "
    "GET_SLR <PM|TEMP|HUM> FACTORY_RESET\n";

static_assert(sizeof(HELP_RESPONSE) <= SERIAL_COMMAND_MAX_RESPONSE_BYTES);

struct Token {
  const char *data;
  size_t size;
};

bool token_equals(const Token &token, const char *value) {
  const size_t value_size = std::strlen(value);
  return token.size == value_size && std::memcmp(token.data, value, value_size) == 0;
}

size_t tokenize(const char *line, size_t line_size, Token *tokens, size_t max_tokens) {
  size_t token_count = 0;
  size_t position = 0;

  while (position < line_size) {
    while (position < line_size && (line[position] == ' ' || line[position] == '\t')) {
      ++position;
    }
    if (position == line_size) {
      break;
    }

    const size_t token_start = position;
    while (position < line_size && line[position] != ' ' && line[position] != '\t') {
      ++position;
    }
    if (token_count < max_tokens) {
      tokens[token_count++] = {line + token_start, position - token_start};
    } else {
      return max_tokens + 1;
    }
  }

  return token_count;
}

bool parse_float(const Token &token, float &value) {
  if (token.size == 0 || token.size > SERIAL_COMMAND_MAX_LINE_BYTES) {
    return false;
  }

  char number[SERIAL_COMMAND_MAX_LINE_BYTES + 1];
  std::memcpy(number, token.data, token.size);
  number[token.size] = '\0';

  char *end = nullptr;
  value = std::strtof(number, &end);
  return end == number + token.size && std::isfinite(value);
}

const char *target_for_result(SerialCommandResultKind kind) {
  switch (kind) {
  case SerialCommandResultKind::SlrPm:
    return "PM";
  case SerialCommandResultKind::SlrTemperature:
    return "TEMP";
  case SerialCommandResultKind::SlrHumidity:
    return "HUM";
  default:
    return nullptr;
  }
}

} // namespace

SerialCommandService::SerialCommandService(RtosQueueHandle event_queue,
                                           SerialCommandChannel &channel)
    : _event_queue(event_queue), _channel(channel) {}

bool SerialCommandService::start() {
  if (_started) {
    const bool was_receiving = _receiving.exchange(true);
    if (!was_receiving) {
      RTOS::task_notify_give(_task_handle);
    }
    return true;
  }
  if (!_channel.initialize()) {
    return false;
  }

  _result_queue = RTOS::queue_create(1, sizeof(SerialCommandResult));
  if (_result_queue == nullptr) {
    return false;
  }

  _receiving.store(true);
#ifndef TEST_HOST
  if (!RTOS::task_create(_task_entry, "serial_cmd", SERIAL_COMMAND_TASK_STACK_BYTES, this,
                         SERIAL_COMMAND_TASK_PRIORITY, &_task_handle)) {
    _receiving.store(false);
    RTOS::queue_delete(_result_queue);
    _result_queue = nullptr;
    return false;
  }
#endif
  _started = true;
  return true;
}

void SerialCommandService::stop_receiving() { _receiving.store(false); }

void SerialCommandService::complete(const SerialCommandResult &result) {
  if (_result_queue == nullptr) {
    return;
  }
  const bool delivered = RTOS::queue_send(_result_queue, &result, UINT32_MAX);
  if (!delivered) {
    return;
  }
}

void SerialCommandService::_task_entry(void *param) {
  static_cast<SerialCommandService *>(param)->_command_task();
}

void SerialCommandService::_command_task() {
  while (true) {
    if (!_receiving.load()) {
      if (_awaiting_result) {
        SerialCommandResult result{};
        if (RTOS::queue_receive(_result_queue, &result, UINT32_MAX)) {
          _complete_result(result);
        }
        continue;
      }
      (void)RTOS::task_notify_take(UINT32_MAX);
      continue;
    }
    _poll_once();
  }
}

void SerialCommandService::_poll_once() {
  SerialCommandResult result{};
  if (_awaiting_result && RTOS::queue_receive(_result_queue, &result, 0)) {
    _complete_result(result);
  }
  char buffer[SERIAL_COMMAND_READ_BUFFER_BYTES];
  const int read_size = _channel.read_bytes(buffer, sizeof(buffer), SERIAL_COMMAND_RX_WAIT_MS);
  if (read_size <= 0) {
    return;
  }
  if (!_receiving.load()) {
    return;
  }

  const size_t received_size = static_cast<size_t>(read_size);
  const size_t process_size = received_size < sizeof(buffer) ? received_size : sizeof(buffer);
  for (size_t i = 0; i < process_size; ++i) {
    _process_byte(buffer[i]);
  }
}

void SerialCommandService::_process_byte(char byte) {
  if (_discarding_line) {
    if (byte == '\n') {
      _discarding_line = false;
    }
    return;
  }

  if (byte == '\n') {
    size_t line_size = _line_size;
    if (line_size > 0 && _line[line_size - 1] == '\r') {
      --line_size;
    }
    _handle_line(_line, line_size);
    _line_size = 0;
    return;
  }

  if (_line_size >= SERIAL_COMMAND_MAX_LINE_BYTES) {
    _line_size = 0;
    _discarding_line = true;
    return;
  }
  _line[_line_size++] = byte;
}

void SerialCommandService::_handle_line(const char *line, size_t line_size) {
  constexpr char ENVELOPE[] = "#AG ";
  if (line_size < sizeof(ENVELOPE) - 1 || std::memcmp(line, ENVELOPE, sizeof(ENVELOPE) - 1) != 0) {
    return;
  }

  Token tokens[SERIAL_COMMAND_MAX_TOKENS]{};
  const size_t token_count =
      tokenize(line + sizeof(ENVELOPE) - 1, line_size - (sizeof(ENVELOPE) - 1), tokens,
               SERIAL_COMMAND_MAX_TOKENS);
  if (token_count == 0) {
    _write_error("EMPTY_COMMAND");
    return;
  }

  if (token_equals(tokens[0], "HELP")) {
    if (token_count != 1) {
      _write_error("INVALID_ARGUMENT");
      return;
    }
    if (_awaiting_result) {
      _write_error("BUSY");
      return;
    }
    if (!_channel.write_response(HELP_RESPONSE, sizeof(HELP_RESPONSE) - 1)) {
      return;
    }
    return;
  }

  if (token_equals(tokens[0], "GET_SERIAL")) {
    if (token_count != 1) {
      _write_error("INVALID_ARGUMENT");
      return;
    }
    _submit_request({SerialCommandKind::GetSerial});
    return;
  }

  if (token_equals(tokens[0], "FACTORY_RESET")) {
    if (token_count != 1) {
      _write_error("INVALID_ARGUMENT");
      return;
    }
    _submit_request({SerialCommandKind::FactoryReset});
    return;
  }

  if (token_equals(tokens[0], "GET_SLR")) {
    if (token_count != 2) {
      _write_error("INVALID_ARGUMENT");
      return;
    }
    if (token_equals(tokens[1], "PM")) {
      _submit_request({SerialCommandKind::GetPmSlr});
    } else if (token_equals(tokens[1], "TEMP")) {
      _submit_request({SerialCommandKind::GetTemperatureSlr});
    } else if (token_equals(tokens[1], "HUM")) {
      _submit_request({SerialCommandKind::GetHumiditySlr});
    } else {
      _write_error("INVALID_ARGUMENT");
    }
    return;
  }

  if (token_equals(tokens[0], "SET_SLR")) {
    if (token_count != 4) {
      _write_error("INVALID_ARGUMENT");
      return;
    }

    float scaling_factor = 0.0f;
    float intercept = 0.0f;
    if (!parse_float(tokens[2], scaling_factor) || !parse_float(tokens[3], intercept)) {
      _write_error("INVALID_ARGUMENT");
      return;
    }

    SerialCommandRequest request{};
    if (token_equals(tokens[1], "PM")) {
      request.kind = SerialCommandKind::SetPmSlr;
      request.pm25_correction.algorithm = Pm25CorrectionAlgorithm::CustomViaPm25Raw;
      request.pm25_correction.scaling_factor = scaling_factor;
      request.pm25_correction.intercept = intercept;
    } else if (token_equals(tokens[1], "TEMP")) {
      request.kind = SerialCommandKind::SetTemperatureSlr;
      request.linear_correction.algorithm = LinearCorrectionAlgorithm::Custom;
      request.linear_correction.scaling_factor = scaling_factor;
      request.linear_correction.intercept = intercept;
    } else if (token_equals(tokens[1], "HUM")) {
      request.kind = SerialCommandKind::SetHumiditySlr;
      request.linear_correction.algorithm = LinearCorrectionAlgorithm::Custom;
      request.linear_correction.scaling_factor = scaling_factor;
      request.linear_correction.intercept = intercept;
    } else {
      _write_error("INVALID_ARGUMENT");
      return;
    }
    _submit_request(request);
    return;
  }

  _write_error("INVALID_COMMAND");
}

void SerialCommandService::_submit_request(const SerialCommandRequest &request) {
  if (_awaiting_result) {
    _write_error("BUSY");
    return;
  }

  Event event{};
  event.type = EventType::SerialCommandRequest;
  event.serial_command_request = request;
  if (!RTOS::queue_send(_event_queue, &event, 0)) {
    _write_error("OPERATION_FAILED");
    return;
  }
  _awaiting_result = true;
}

void SerialCommandService::_complete_result(const SerialCommandResult &result) {
  _awaiting_result = false;

  switch (result.kind) {
  case SerialCommandResultKind::Serial: {
    char serial[SERIAL_COMMAND_MAX_SERIAL_BYTES];
    std::memcpy(serial, result.serial, sizeof(serial));
    serial[sizeof(serial) - 1] = '\0';
    _write_response("\n#AG OK SERIAL %s\n", serial);
    return;
  }
  case SerialCommandResultKind::SlrPm:
  case SerialCommandResultKind::SlrTemperature:
  case SerialCommandResultKind::SlrHumidity: {
    const char *target = target_for_result(result.kind);
    const float scaling_factor = result.kind == SerialCommandResultKind::SlrPm
                                     ? result.pm25_correction.scaling_factor
                                     : result.linear_correction.scaling_factor;
    const float intercept = result.kind == SerialCommandResultKind::SlrPm
                                ? result.pm25_correction.intercept
                                : result.linear_correction.intercept;
    _write_response("\n#AG OK SLR %s %.6f %.6f\n", target, static_cast<double>(scaling_factor),
                    static_cast<double>(intercept));
    return;
  }
  case SerialCommandResultKind::Reset:
    _write_response("\n#AG OK RESET\n");
    return;
  case SerialCommandResultKind::SlrNotSet:
    _write_error("SLR_NOT_SET");
    return;
  case SerialCommandResultKind::InvalidArgument:
    _write_error("INVALID_ARGUMENT");
    return;
  case SerialCommandResultKind::OperationFailed:
    _write_error("OPERATION_FAILED");
    return;
  }
}

void SerialCommandService::_write_error(const char *error_code) {
  _write_response("\n#AG ERROR %s\n", error_code);
}

void SerialCommandService::_write_response(const char *format, ...) {
  char response[SERIAL_COMMAND_MAX_RESPONSE_BYTES];
  va_list args;
  va_start(args, format);
  const int response_size = std::vsnprintf(response, sizeof(response), format, args);
  va_end(args);

  if (response_size <= 0 || static_cast<size_t>(response_size) >= sizeof(response)) {
    return;
  }
  if (!_channel.write_response(response, static_cast<size_t>(response_size))) {
    return;
  }
}
