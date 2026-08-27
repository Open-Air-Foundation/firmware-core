#ifndef SERIAL_COMMAND_H
#define SERIAL_COMMAND_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

#include "measurement_corrections.h"
#include "rtos.h"

inline constexpr size_t SERIAL_COMMAND_MAX_LINE_BYTES = 128;
inline constexpr size_t SERIAL_COMMAND_MAX_RESPONSE_BYTES = 128;
inline constexpr size_t SERIAL_COMMAND_MAX_SERIAL_BYTES = 13;
inline constexpr uint32_t SERIAL_COMMAND_USB_TX_BUFFER_BYTES = 256;
inline constexpr uint32_t SERIAL_COMMAND_USB_RX_BUFFER_BYTES = 256;
inline constexpr uint32_t SERIAL_COMMAND_RX_WAIT_MS = 50;
inline constexpr uint32_t SERIAL_COMMAND_TASK_STACK_BYTES = 3072;
inline constexpr uint32_t SERIAL_COMMAND_TASK_PRIORITY = 3;

enum class SerialCommandKind : uint8_t {
  GetSerial,
  SetPmSlr,
  SetTemperatureSlr,
  SetHumiditySlr,
  GetPmSlr,
  GetTemperatureSlr,
  GetHumiditySlr,
  FactoryReset,
};

struct SerialCommandRequest {
  SerialCommandKind kind = SerialCommandKind::GetSerial;
  union {
    Pm25Correction pm25_correction;
    LinearCorrection linear_correction;
  };
};

enum class SerialCommandResultKind : uint8_t {
  Serial,
  SlrPm,
  SlrTemperature,
  SlrHumidity,
  Reset,
  SlrNotSet,
  InvalidArgument,
  OperationFailed,
};

struct SerialCommandResult {
  SerialCommandResultKind kind = SerialCommandResultKind::OperationFailed;
  union {
    char serial[SERIAL_COMMAND_MAX_SERIAL_BYTES];
    Pm25Correction pm25_correction;
    LinearCorrection linear_correction;
  };
};

static_assert(std::is_trivially_copyable<SerialCommandRequest>::value);
static_assert(std::is_trivially_copyable<SerialCommandResult>::value);

class SerialCommandChannel {
public:
  virtual ~SerialCommandChannel() = default;

  virtual bool initialize() = 0;
  virtual int read_bytes(char *buffer, size_t buffer_size, uint32_t timeout_ms) = 0;
  virtual bool write_response(const char *response, size_t response_size) = 0;
};

class SerialCommandService {
public:
  SerialCommandService(RtosQueueHandle event_queue, SerialCommandChannel &channel);

  /// Initialize the transport and start the command task. Idempotent after success.
  bool start();

  /// Stop receiving commands without tearing down the USB transport.
  void stop_receiving();

  /// Deliver the completed result for the single accepted command.
  void complete(const SerialCommandResult &result);

private:
#ifdef TEST_HOST
  friend class SerialCommandServiceTestAccess;
#endif

  static void _task_entry(void *param);
  void _command_task();
  void _poll_once();
  void _process_byte(char byte);
  void _handle_line(const char *line, size_t line_size);
  void _submit_request(const SerialCommandRequest &request);
  void _complete_result(const SerialCommandResult &result);
  void _write_error(const char *error_code);
  void _write_response(const char *format, ...);

  RtosQueueHandle _event_queue;
  SerialCommandChannel &_channel;
  RtosQueueHandle _result_queue = nullptr;
  RtosTaskHandle _task_handle = nullptr;
  char _line[SERIAL_COMMAND_MAX_LINE_BYTES]{};
  size_t _line_size = 0;
  bool _discarding_line = false;
  bool _awaiting_result = false;
  bool _started = false;
  std::atomic<bool> _receiving{false};
};

class UsbSerialCommandChannel final : public SerialCommandChannel {
public:
  bool initialize() override;
  int read_bytes(char *buffer, size_t buffer_size, uint32_t timeout_ms) override;
  bool write_response(const char *response, size_t response_size) override;

private:
  int _tx_fd = -1;
  bool _initialized = false;
};

#endif // SERIAL_COMMAND_H
