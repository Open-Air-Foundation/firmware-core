#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "go_events.h"
#include "serial_command/serial_command.h"

class SerialCommandServiceTestAccess {
public:
  static void poll(SerialCommandService &service) { service._poll_once(); }
  static bool is_receiving(const SerialCommandService &service) {
    return service._receiving.load();
  }
};

class TestRTOS final : public RTOS {
public:
  void delay_ms_impl(uint32_t) override {}
  uint64_t get_time_ms_impl() override { return 0; }
};

class FakeSerialCommandChannel final : public SerialCommandChannel {
public:
  bool initialize() override {
    initialized = true;
    return initialize_result;
  }

  int read_bytes(char *buffer, size_t buffer_size, uint32_t) override {
    const size_t bytes_to_read = std::min(buffer_size, input.size());
    std::copy_n(input.begin(), bytes_to_read, buffer);
    input.erase(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(bytes_to_read));
    return static_cast<int>(bytes_to_read);
  }

  bool write_response(const char *response, size_t response_size) override {
    responses.emplace_back(response, response_size);
    return write_result;
  }

  void append_input(const std::string &value) {
    input.insert(input.end(), value.begin(), value.end());
  }

  bool initialize_result = true;
  bool write_result = true;
  bool initialized = false;
  std::vector<char> input;
  std::vector<std::string> responses;
};

struct SerialCommandFixture {
  TestRTOS rtos;
  RtosQueueHandle event_queue = nullptr;
  FakeSerialCommandChannel channel;
  std::unique_ptr<SerialCommandService> service;

  explicit SerialCommandFixture(uint32_t event_queue_depth = EVENT_QUEUE_DEPTH) {
    RTOS::set_instance(&rtos);
    event_queue = RTOS::queue_create(event_queue_depth, sizeof(Event));
    service = std::make_unique<SerialCommandService>(event_queue, channel);
    REQUIRE(service->start());
  }

  ~SerialCommandFixture() {
    RTOS::queue_delete(event_queue);
    RTOS::set_instance(nullptr);
  }

  void poll() { SerialCommandServiceTestAccess::poll(*service); }

  void drain_input() {
    while (!channel.input.empty()) {
      poll();
    }
  }

  Event receive_event() {
    Event event{};
    REQUIRE(RTOS::queue_receive(event_queue, &event, 0));
    return event;
  }

  void complete_operation_failed() {
    service->complete({SerialCommandResultKind::OperationFailed});
    poll();
  }
};

TEST_CASE("serial command parses envelope and recovers overlong lines", "[serial_command]") {
  SerialCommandFixture fixture;

  fixture.channel.append_input("ignored\n#AG \r\n#AG HELP\r\n");
  fixture.poll();

  REQUIRE(fixture.channel.responses.size() == 2);
  CHECK(fixture.channel.responses[0] == "\n#AG ERROR EMPTY_COMMAND\n");
  CHECK(fixture.channel.responses[1] ==
        "\n#AG OK COMMANDS HELP GET_SERIAL SET_SLR <PM|TEMP|HUM> <scale> <intercept> "
        "GET_SLR <PM|TEMP|HUM> FACTORY_RESET\n");

  fixture.channel.append_input(std::string(SERIAL_COMMAND_MAX_LINE_BYTES + 1, 'x') +
                               "\n#AG GET_SERIAL\n");
  fixture.drain_input();

  Event event = fixture.receive_event();
  CHECK(event.type == EventType::SerialCommandRequest);
  CHECK(event.serial_command_request.kind == SerialCommandKind::GetSerial);
}

TEST_CASE("serial command rejects malformed grammar and maps SLR requests", "[serial_command]") {
  SerialCommandFixture fixture;

  fixture.channel.append_input("#AG UNKNOWN\n#AG GET_SERIAL extra\n#AG SET_SLR TEMP nan 1\n"
                               "#AG SET_SLR OTHER 1 2\n#AG SET_SLR TEMP 1.1 -0.3\n");
  fixture.drain_input();

  REQUIRE(fixture.channel.responses.size() == 4);
  CHECK(fixture.channel.responses[0] == "\n#AG ERROR INVALID_COMMAND\n");
  CHECK(fixture.channel.responses[1] == "\n#AG ERROR INVALID_ARGUMENT\n");
  CHECK(fixture.channel.responses[2] == "\n#AG ERROR INVALID_ARGUMENT\n");
  CHECK(fixture.channel.responses[3] == "\n#AG ERROR INVALID_ARGUMENT\n");

  Event event = fixture.receive_event();
  CHECK(event.serial_command_request.kind == SerialCommandKind::SetTemperatureSlr);
  CHECK(event.serial_command_request.linear_correction.algorithm ==
        LinearCorrectionAlgorithm::Custom);
  CHECK(event.serial_command_request.linear_correction.scaling_factor == 1.1f);
  CHECK(event.serial_command_request.linear_correction.intercept == -0.3f);
}

TEST_CASE("serial command maps every correction target and factory reset", "[serial_command]") {
  SerialCommandFixture fixture;

  fixture.channel.append_input("#AG SET_SLR PM 1.2 0.4\n");
  fixture.drain_input();
  Event event = fixture.receive_event();
  CHECK(event.serial_command_request.kind == SerialCommandKind::SetPmSlr);
  CHECK(event.serial_command_request.pm25_correction.algorithm ==
        Pm25CorrectionAlgorithm::CustomViaPm25Raw);
  CHECK(event.serial_command_request.pm25_correction.scaling_factor == 1.2f);
  CHECK(event.serial_command_request.pm25_correction.intercept == 0.4f);
  fixture.complete_operation_failed();

  fixture.channel.append_input("#AG SET_SLR HUM 0.9 2\n");
  fixture.drain_input();
  event = fixture.receive_event();
  CHECK(event.serial_command_request.kind == SerialCommandKind::SetHumiditySlr);
  CHECK(event.serial_command_request.linear_correction.algorithm ==
        LinearCorrectionAlgorithm::Custom);
  fixture.complete_operation_failed();

  fixture.channel.append_input("#AG GET_SLR PM\n");
  fixture.drain_input();
  event = fixture.receive_event();
  CHECK(event.serial_command_request.kind == SerialCommandKind::GetPmSlr);
  fixture.complete_operation_failed();

  fixture.channel.append_input("#AG GET_SLR TEMP\n");
  fixture.drain_input();
  event = fixture.receive_event();
  CHECK(event.serial_command_request.kind == SerialCommandKind::GetTemperatureSlr);
  fixture.complete_operation_failed();

  fixture.channel.append_input("#AG FACTORY_RESET\n");
  fixture.drain_input();
  event = fixture.receive_event();
  CHECK(event.serial_command_request.kind == SerialCommandKind::FactoryReset);
}

TEST_CASE("serial command enforces one command in flight and formats results", "[serial_command]") {
  SerialCommandFixture fixture;

  fixture.channel.append_input("#AG GET_SERIAL\n#AG HELP\n");
  fixture.poll();
  CHECK(fixture.channel.responses == std::vector<std::string>{"\n#AG ERROR BUSY\n"});

  Event event = fixture.receive_event();
  CHECK(event.serial_command_request.kind == SerialCommandKind::GetSerial);

  SerialCommandResult serial_result{};
  serial_result.kind = SerialCommandResultKind::Serial;
  std::strncpy(serial_result.serial, "AABBCCDDEEFF", sizeof(serial_result.serial) - 1);
  fixture.service->complete(serial_result);
  fixture.poll();

  REQUIRE(fixture.channel.responses.size() == 2);
  CHECK(fixture.channel.responses[1] == "\n#AG OK SERIAL AABBCCDDEEFF\n");

  fixture.channel.append_input("#AG GET_SLR HUM\n");
  fixture.poll();
  event = fixture.receive_event();
  CHECK(event.serial_command_request.kind == SerialCommandKind::GetHumiditySlr);

  SerialCommandResult slr_result{};
  slr_result.kind = SerialCommandResultKind::SlrHumidity;
  slr_result.linear_correction.scaling_factor = 1.1f;
  slr_result.linear_correction.intercept = -0.3f;
  fixture.service->complete(slr_result);
  fixture.poll();

  CHECK(fixture.channel.responses[2] == "\n#AG OK SLR HUM 1.100000 -0.300000\n");
}

TEST_CASE("serial command reports failed event admission", "[serial_command]") {
  SerialCommandFixture fixture(1);
  Event occupied{};
  occupied.type = EventType::InactivityTimeout;
  REQUIRE(RTOS::queue_send(fixture.event_queue, &occupied, 0));

  fixture.channel.append_input("#AG FACTORY_RESET\n");
  fixture.poll();

  CHECK(fixture.channel.responses == std::vector<std::string>{"\n#AG ERROR OPERATION_FAILED\n"});
}

TEST_CASE("serial command stops receiving until restarted", "[serial_command]") {
  SerialCommandFixture fixture;
  REQUIRE(SerialCommandServiceTestAccess::is_receiving(*fixture.service));

  fixture.service->stop_receiving();
  fixture.channel.append_input("#AG GET_SERIAL\n");
  fixture.poll();

  CHECK_FALSE(SerialCommandServiceTestAccess::is_receiving(*fixture.service));
  CHECK(fixture.channel.input.empty());
  Event event{};
  CHECK_FALSE(RTOS::queue_receive(fixture.event_queue, &event, 0));
  CHECK(fixture.channel.responses.empty());

  REQUIRE(fixture.service->start());
  fixture.channel.append_input("#AG GET_SERIAL\n");
  fixture.poll();

  CHECK(SerialCommandServiceTestAccess::is_receiving(*fixture.service));
  CHECK(fixture.channel.input.empty());
  CHECK(fixture.receive_event().serial_command_request.kind == SerialCommandKind::GetSerial);
}
