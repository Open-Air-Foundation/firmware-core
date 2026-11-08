/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include <deque>
#include <string>

#if __has_include("airgradient_gpio.h")
#include "airgradient_gpio.h"
#else
#include "../../airgradient-gpio/hal/airgradient_gpio.h"
#endif

#if __has_include("rtos.h")
#include "rtos.h"
#else
#include "../../airgradient-common/include/rtos.h"
#endif

#include "../drivers/simcom_a7672x/simcom_a7672x.h"

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);
};

class ScopedRTOSInstance {
public:
  explicit ScopedRTOSInstance(RTOS *rtos) { RTOS::set_instance(rtos); }

  ~ScopedRTOSInstance() { RTOS::set_instance(nullptr); }
};

class MockGpioHal {
public:
  MAKE_MOCK4(configure, bool(int, gpio::Mode, gpio::PullMode, gpio::InterruptType));
  MAKE_MOCK1(get_level, int(int));
  MAKE_MOCK2(set_level, bool(int, int));
  MAKE_MOCK3(add_interrupt_handler, bool(int, gpio::InterruptHandler, void *));
  MAKE_MOCK1(remove_interrupt_handler, bool(int));
  MAKE_MOCK1(enable_interrupt, bool(int));
  MAKE_MOCK1(disable_interrupt, bool(int));

  gpio::Hal as_hal();
};

static bool mock_configure(void *context, int pin, gpio::Mode mode, gpio::PullMode pull,
                           gpio::InterruptType interrupt, int) {
  return static_cast<MockGpioHal *>(context)->configure(pin, mode, pull, interrupt);
}
static int mock_get_level(void *context, int pin) {
  return static_cast<MockGpioHal *>(context)->get_level(pin);
}
static bool mock_set_level(void *context, int pin, int level) {
  return static_cast<MockGpioHal *>(context)->set_level(pin, level);
}
static bool mock_add_interrupt_handler(void *context, int pin, gpio::InterruptHandler handler,
                                       void *arg) {
  return static_cast<MockGpioHal *>(context)->add_interrupt_handler(pin, handler, arg);
}
static bool mock_remove_interrupt_handler(void *context, int pin) {
  return static_cast<MockGpioHal *>(context)->remove_interrupt_handler(pin);
}
static bool mock_enable_interrupt(void *context, int pin) {
  return static_cast<MockGpioHal *>(context)->enable_interrupt(pin);
}
static bool mock_disable_interrupt(void *context, int pin) {
  return static_cast<MockGpioHal *>(context)->disable_interrupt(pin);
}

gpio::Hal MockGpioHal::as_hal() {
  static const gpio::Operations operations{
      mock_configure,
      mock_get_level,
      mock_set_level,
      mock_add_interrupt_handler,
      mock_remove_interrupt_handler,
      mock_enable_interrupt,
      mock_disable_interrupt,
  };
  return gpio::Hal{operations, this};
}

class MockAirgradientSerial : public AirgradientSerial {
public:
  MAKE_MOCK1(begin, bool(int), override);
  MAKE_MOCK0(end, void(), override);
  MAKE_MOCK0(available, int(), override);
  MAKE_MOCK1(print, void(const char *), override);
  MAKE_MOCK2(write, int(const uint8_t *, int), override);
  MAKE_MOCK0(read, int(), override);
  MAKE_MOCK2(read, int(uint8_t *, int), override);

  void queue_rx(const char *data) {
    while (data != nullptr && *data != '\0') {
      _rx.push_back(static_cast<uint8_t>(*data));
      data++;
    }
  }

  int queued_rx_size() const { return static_cast<int>(_rx.size()); }

  int pop_rx() {
    if (_rx.empty()) {
      return -1;
    }

    const uint8_t value = _rx.front();
    _rx.pop_front();
    return value;
  }

  void append_tx(const char *data) {
    if (data != nullptr) {
      _tx += data;
    }
  }

  const std::string &tx() const { return _tx; }

private:
  std::deque<uint8_t> _rx;
  std::string _tx;
};

class StubSerial : public AirgradientSerial {};

class TestableSimcomA7672x : public SimcomA7672x {
public:
  using SimcomA7672x::SimcomA7672x;

  CellularStatus test_parse_operator_list(const char *response, OperatorList *out_operators) {
    return _parse_operator_list(response, out_operators);
  }
};

TEST_CASE("simcom_a7672x init performs bring-up sequence without power control",
          "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);

  serial.queue_rx("\r\nOK\r\n\r\nOK\r\n\r\nOK\r\nManufacturer: INCORPORATED\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  REQUIRE(modem.init() == CellularStatus::Ok);
  REQUIRE(modem.power_on() == CellularStatus::Unsupported);
  REQUIRE(serial.tx() == "AT\r\nATE0\r\nAT+CGEREP=0\r\nATI\r\n");
}

TEST_CASE("simcom_a7672x reinitialize repeats bring-up sequence",
          "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);

  serial.queue_rx("\r\nOK\r\n\r\nOK\r\n\r\nOK\r\nManufacturer: INCORPORATED\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  REQUIRE(modem.reinitialize() == CellularStatus::Ok);
  REQUIRE(serial.tx() == "AT\r\nATE0\r\nAT+CGEREP=0\r\nATI\r\n");
}

TEST_CASE("simcom_a7672x init configures and powers module when power control "
          "is provided",
          "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockGpioHal gpio_mock;
  gpio::Hal gpio_hal = gpio_mock.as_hal();
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial, gpio_hal, 4);
  trompeloeil::sequence seq;

  serial.queue_rx("\r\nOK\r\n\r\nOK\r\n\r\nOK\r\nManufacturer: INCORPORATED\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  REQUIRE_CALL(gpio_mock, configure(4, gpio::Mode::Output, gpio::PullMode::Floating,
                                    gpio::InterruptType::Disabled))
      .RETURN(true);
  REQUIRE_CALL(gpio_mock, set_level(4, 0)).IN_SEQUENCE(seq).RETURN(true);
  REQUIRE_CALL(rtos, delay_ms_impl(SimcomA7672x::POWER_ON_LOW_DELAY_MS)).IN_SEQUENCE(seq);
  REQUIRE_CALL(gpio_mock, set_level(4, 1)).IN_SEQUENCE(seq).RETURN(true);
  REQUIRE_CALL(rtos, delay_ms_impl(SimcomA7672x::POWER_ON_HIGH_DELAY_MS)).IN_SEQUENCE(seq);
  REQUIRE_CALL(gpio_mock, set_level(4, 0)).IN_SEQUENCE(seq).RETURN(true);
  REQUIRE_CALL(rtos, delay_ms_impl(SimcomA7672x::POWER_ON_HIGH_DELAY_MS)).IN_SEQUENCE(seq);

  REQUIRE(modem.init() == CellularStatus::Ok);
  REQUIRE(serial.tx() == "AT\r\nATE0\r\nAT+CGEREP=0\r\nATI\r\n");
}

TEST_CASE("simcom_a7672x force power off pulses the power pin",
          "[airgradient-cellular][simcom-a7672x]") {
  StubSerial serial;
  MockGpioHal gpio_mock;
  gpio::Hal gpio_hal = gpio_mock.as_hal();
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial, gpio_hal, 5);
  trompeloeil::sequence seq;

  REQUIRE_CALL(gpio_mock, set_level(5, 1)).IN_SEQUENCE(seq).RETURN(true);
  REQUIRE_CALL(rtos, delay_ms_impl(SimcomA7672x::FORCE_POWER_OFF_HIGH_DELAY_MS)).IN_SEQUENCE(seq);
  REQUIRE_CALL(gpio_mock, set_level(5, 0)).IN_SEQUENCE(seq).RETURN(true);

  REQUIRE(modem.power_off(true) == CellularStatus::Ok);
  REQUIRE(modem.power_off(false) == CellularStatus::Unsupported);
}

TEST_CASE("simcom_a7672x reset power cycles when power control is available",
          "[airgradient-cellular][simcom-a7672x]") {
  StubSerial serial;
  MockGpioHal gpio_mock;
  gpio::Hal gpio_hal = gpio_mock.as_hal();
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial, gpio_hal, 7);
  trompeloeil::sequence seq;

  REQUIRE_CALL(gpio_mock, set_level(7, 1)).IN_SEQUENCE(seq).RETURN(true);
  REQUIRE_CALL(rtos, delay_ms_impl(SimcomA7672x::FORCE_POWER_OFF_HIGH_DELAY_MS)).IN_SEQUENCE(seq);
  REQUIRE_CALL(gpio_mock, set_level(7, 0)).IN_SEQUENCE(seq).RETURN(true);
  REQUIRE_CALL(rtos, delay_ms_impl(SimcomA7672x::RESET_RECOVERY_DELAY_MS)).IN_SEQUENCE(seq);
  REQUIRE_CALL(gpio_mock, set_level(7, 0)).IN_SEQUENCE(seq).RETURN(true);
  REQUIRE_CALL(rtos, delay_ms_impl(SimcomA7672x::POWER_ON_LOW_DELAY_MS)).IN_SEQUENCE(seq);
  REQUIRE_CALL(gpio_mock, set_level(7, 1)).IN_SEQUENCE(seq).RETURN(true);
  REQUIRE_CALL(rtos, delay_ms_impl(SimcomA7672x::POWER_ON_HIGH_DELAY_MS)).IN_SEQUENCE(seq);
  REQUIRE_CALL(gpio_mock, set_level(7, 0)).IN_SEQUENCE(seq).RETURN(true);
  REQUIRE_CALL(rtos, delay_ms_impl(SimcomA7672x::POWER_ON_HIGH_DELAY_MS)).IN_SEQUENCE(seq);

  REQUIRE(modem.reset() == CellularStatus::Ok);
  REQUIRE(modem.sleep() == CellularStatus::Unsupported);
}

TEST_CASE("simcom_a7672x drains ATI output for module info",
          "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  char module_info[32] = {};
  size_t module_info_length = 0;

  serial.queue_rx("\r\nManufacturer: INCORPORATED\r\nModel: "
                  "A7600C\r\nRevision: A7600C_V1.0\r\nIMEI: "
                  "351602000330570\r\n+GCAP: +CGSM,+FCLASS,+DS\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  const CellularStatus status =
      modem.get_module_info({module_info, sizeof(module_info)}, &module_info_length);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(module_info_length == 0);
  REQUIRE(std::string(module_info) == "");
  REQUIRE(serial.tx() == "ATI\r\n");
}

TEST_CASE("simcom_a7672x retrieves SIM ICCID", "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  char iccid[32] = {};
  size_t iccid_length = 0;

  serial.queue_rx("\r\n+ICCID: 8986001234567890123\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  const CellularStatus status = modem.get_sim_iccid({iccid, sizeof(iccid)}, &iccid_length);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(iccid_length == 19);
  REQUIRE(std::string(iccid) == "8986001234567890123");
  REQUIRE(serial.tx() == "AT+CICCID\r\n");
}

TEST_CASE("simcom_a7672x detects SIM ready state", "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);

  serial.queue_rx("\r\n+CPIN: READY\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  REQUIRE(modem.check_sim_ready() == CellularStatus::Ok);
  REQUIRE(serial.tx() == "AT+CPIN?\r\n");
}

TEST_CASE("simcom_a7672x returns failed when SIM is not ready",
          "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);

  serial.queue_rx("\r\n+CPIN: SIM PIN\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  REQUIRE(modem.check_sim_ready() == CellularStatus::Failed);
}

TEST_CASE("simcom_a7672x parses signal quality and RSSI", "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  CellularSignalQuality signal_quality = {};

  serial.queue_rx("\r\n+CSQ: 15,0\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  const CellularStatus status = modem.get_signal_quality(&signal_quality);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(signal_quality.csq == 15);
  REQUIRE(signal_quality.rssi_dbm == -83);
  REQUIRE(serial.tx() == "AT+CSQ\r\n");
}

TEST_CASE("simcom_a7672x retrieves PDP IP address", "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  char ip_address[32] = {};
  size_t ip_address_length = 0;

  serial.queue_rx("\r\n+CGPADDR: 1,10.160.32.44\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  const CellularStatus status =
      modem.get_ip_address({ip_address, sizeof(ip_address)}, &ip_address_length);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(ip_address_length == 12);
  REQUIRE(std::string(ip_address) == "10.160.32.44");
  REQUIRE(serial.tx() == "AT+CGPADDR=1\r\n");
}

TEST_CASE("simcom_a7672x queries auto registration state",
          "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  CellularNetworkRegistration registration = {};

  serial.queue_rx("\r\n+CREG: 0,1\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  const CellularStatus status =
      modem.get_network_registration(CellularTechnology::Auto, &registration);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(registration.state == CellularRegistrationState::RegisteredHome);
  REQUIRE(registration.is_packet_service_ready);
  REQUIRE(serial.tx() == "AT+CREG?\r\n");
}

TEST_CASE("simcom_a7672x queries 2G registration state", "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  CellularNetworkRegistration registration = {};

  serial.queue_rx("\r\n+CGREG: 1,2\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  const CellularStatus status =
      modem.get_network_registration(CellularTechnology::TwoG, &registration);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(registration.state == CellularRegistrationState::Searching);
  REQUIRE_FALSE(registration.is_packet_service_ready);
  REQUIRE(serial.tx() == "AT+CGREG?\r\n");
}

TEST_CASE("simcom_a7672x queries LTE registration state", "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  CellularNetworkRegistration registration = {};

  serial.queue_rx("\r\n+CEREG: 0,3\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).RETURN(0);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_));
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  const CellularStatus status =
      modem.get_network_registration(CellularTechnology::Lte, &registration);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(registration.state == CellularRegistrationState::Denied);
  REQUIRE_FALSE(registration.is_packet_service_ready);
  REQUIRE(serial.tx() == "AT+CEREG?\r\n");
}

TEST_CASE("simcom_a7672x start network registration runs core readiness flow",
          "[airgradient-cellular][simcom-a7672x]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\nOK\r\n"
                  "\r\n+CPIN: READY\r\n\r\nOK\r\n"
                  "\r\nOK\r\n"
                  "\r\nOK\r\n"
                  "\r\nOK\r\n"
                  "\r\n+COPS: (2,\"CHN-UNICOM\",\"UNICOM\",\"46001\",7)\r\n\r\nOK\r\n"
                  "\r\nOK\r\n"
                  "\r\n+CEREG: 0,1\r\n\r\nOK\r\n"
                  "\r\n+CSQ: 15,0\r\n\r\nOK\r\n"
                  "\r\n+CNSMOD: 0,7\r\n\r\nOK\r\n"
                  "\r\nOK\r\n"
                  "\r\n+CGATT: 0\r\n\r\nOK\r\n"
                  "\r\nOK\r\n"
                  "\r\n+CSQ: 15,0\r\n\r\nOK\r\n"
                  "\r\n+CGPADDR: 1,10.160.32.44\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  CellularRegistrationRequest request = {};
  request.technology = CellularTechnology::Lte;
  request.apn = "internet";

  const CellularStatus status = modem.start_network_registration(request);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(serial.tx() == "AT\r\n"
                         "AT+CPIN?\r\n"
                         "AT+CEREG=0\r\n"
                         "AT+CNMP=38\r\n"
                         "AT+CGDCONT=1,\"IP\",\"internet\"\r\n"
                         "AT+COPS=?\r\n"
                         "AT+COPS=1,2,\"46001\",7\r\n"
                         "AT+CEREG?\r\n"
                         "AT+CSQ\r\n"
                         "AT+CNSMOD?\r\n"
                         "AT+CGACT=1,1\r\n"
                         "AT+CGATT?\r\n"
                         "AT+CGATT=1\r\n"
                         "AT+CSQ\r\n"
                         "AT+CGPADDR=1\r\n");
}

TEST_CASE("simcom_a7672x start network registration validates APN",
          "[airgradient-cellular][simcom-a7672x]") {
  StubSerial serial;
  SimcomA7672x modem(serial);
  CellularRegistrationRequest request = {};
  request.technology = CellularTechnology::Auto;
  request.apn = nullptr;

  REQUIRE(modem.start_network_registration(request) == CellularStatus::InvalidArgument);
}

TEST_CASE("simcom_a7672x parses operator list with multiple operators",
          "[airgradient-cellular][simcom-a7672x][operator-scan]") {
  StubSerial serial;
  TestableSimcomA7672x modem(serial);
  OperatorList operators = {};

  // Real response format from AT+COPS=?
  const char *response = "(2,\"CHN-UNICOM\",\"UNICOM\",\"46001\",7),"
                         "(1,\"CHN-UNICOM\",\"UNICOM\",\"46001\",2),"
                         "(1,\"CHN-UNICOM\",\"UNICOM\",\"46001\",0),,"
                         "(0,1,2,3,4),(0,1,2)";

  const CellularStatus status = modem.test_parse_operator_list(response, &operators);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(operators.count == 3);

  // First operator: status 2 (current), LTE
  REQUIRE(operators.operators[0].operator_id == 46001);
  REQUIRE(operators.operators[0].access_tech == OperatorAccessTech::EUtranLte);

  // Second operator: status 1 (available), UTRAN
  REQUIRE(operators.operators[1].operator_id == 46001);
  REQUIRE(operators.operators[1].access_tech == OperatorAccessTech::Utran);

  // Third operator: status 1 (available), GSM
  REQUIRE(operators.operators[2].operator_id == 46001);
  REQUIRE(operators.operators[2].access_tech == OperatorAccessTech::Gsm);
}

TEST_CASE("simcom_a7672x parses operator list filtering by status",
          "[airgradient-cellular][simcom-a7672x][operator-scan]") {
  StubSerial serial;
  TestableSimcomA7672x modem(serial);
  OperatorList operators = {};

  // Status 3 = forbidden, should be filtered out
  const char *response = "(1,\"Available\",\"AVL\",\"310260\",7),"
                         "(3,\"Forbidden\",\"FBD\",\"310410\",7),"
                         "(2,\"Current\",\"CUR\",\"310120\",0),"
                         "(0,\"Unknown\",\"UNK\",\"310999\",7)";

  const CellularStatus status = modem.test_parse_operator_list(response, &operators);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(operators.count == 2);

  // Only status 1 and 2 should be included
  REQUIRE(operators.operators[0].operator_id == 310260);
  REQUIRE(operators.operators[1].operator_id == 310120);
}

TEST_CASE("simcom_a7672x parses operator list with empty response",
          "[airgradient-cellular][simcom-a7672x][operator-scan]") {
  StubSerial serial;
  TestableSimcomA7672x modem(serial);
  OperatorList operators = {};

  const CellularStatus status = modem.test_parse_operator_list("", &operators);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(operators.count == 0);
}

TEST_CASE("simcom_a7672x parses operator list respecting MAX_OPERATORS limit",
          "[airgradient-cellular][simcom-a7672x][operator-scan]") {
  StubSerial serial;
  TestableSimcomA7672x modem(serial);
  OperatorList operators = {};

  // Build a response with more than MAX_OPERATORS (16)
  std::string response;
  for (int i = 0; i < 20; i++) {
    if (i > 0) {
      response += ",";
    }
    response += "(1,\"Op" + std::to_string(i) + "\",\"O" + std::to_string(i) + "\",\"" +
                std::to_string(310000 + i) + "\",7)";
  }

  const CellularStatus status = modem.test_parse_operator_list(response.c_str(), &operators);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(operators.count == OperatorList::MAX_OPERATORS);
  REQUIRE(operators.operators[0].operator_id == 310000);
  REQUIRE(operators.operators[15].operator_id == 310015);
}

TEST_CASE("simcom_a7672x parses operator list with null arguments",
          "[airgradient-cellular][simcom-a7672x][operator-scan]") {
  StubSerial serial;
  TestableSimcomA7672x modem(serial);
  OperatorList operators = {};

  REQUIRE(modem.test_parse_operator_list(nullptr, &operators) == CellularStatus::InvalidArgument);
  REQUIRE(modem.test_parse_operator_list("(1,\"Op\",\"O\",\"310000\",7)", nullptr) ==
          CellularStatus::InvalidArgument);
}

TEST_CASE("simcom_a7672x resolves DNS successfully", "[airgradient-cellular][simcom-a7672x][dns]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\n+CDNSGIP: 1,\"example.com\",\"93.184.216.34\"\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  char ip_buffer[32] = {};
  size_t ip_length = 0;

  const CellularStatus status =
      modem.resolve_dns("example.com", {ip_buffer, sizeof(ip_buffer)}, &ip_length);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(std::string(ip_buffer) == "93.184.216.34");
  REQUIRE(ip_length == 13);
  REQUIRE(serial.tx() == "AT+CDNSGIP=\"example.com\"\r\n");
}

TEST_CASE("simcom_a7672x DNS resolution validates null hostname",
          "[airgradient-cellular][simcom-a7672x][dns]") {
  StubSerial serial;
  SimcomA7672x modem(serial);
  char ip_buffer[32] = {};

  REQUIRE(modem.resolve_dns(nullptr, {ip_buffer, sizeof(ip_buffer)}) ==
          CellularStatus::InvalidArgument);
}

TEST_CASE("simcom_a7672x DNS resolution validates empty hostname",
          "[airgradient-cellular][simcom-a7672x][dns]") {
  StubSerial serial;
  SimcomA7672x modem(serial);
  char ip_buffer[32] = {};

  REQUIRE(modem.resolve_dns("", {ip_buffer, sizeof(ip_buffer)}) == CellularStatus::InvalidArgument);
}

TEST_CASE("simcom_a7672x DNS resolution validates null buffer",
          "[airgradient-cellular][simcom-a7672x][dns]") {
  StubSerial serial;
  SimcomA7672x modem(serial);

  REQUIRE(modem.resolve_dns("example.com", {nullptr, 32}) == CellularStatus::InvalidArgument);
}

TEST_CASE("simcom_a7672x DNS resolution validates zero buffer size",
          "[airgradient-cellular][simcom-a7672x][dns]") {
  StubSerial serial;
  SimcomA7672x modem(serial);
  char ip_buffer[32] = {};

  REQUIRE(modem.resolve_dns("example.com", {ip_buffer, 0}) == CellularStatus::InvalidArgument);
}

TEST_CASE("simcom_a7672x DNS resolution handles buffer too small",
          "[airgradient-cellular][simcom-a7672x][dns]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\n+CDNSGIP: 1,\"example.com\",\"93.184.216.34\"\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  char ip_buffer[10] = {}; // Too small for "93.184.216.34"

  const CellularStatus status = modem.resolve_dns("example.com", {ip_buffer, sizeof(ip_buffer)});

  REQUIRE(status == CellularStatus::BufferTooSmall);
}

TEST_CASE("simcom_a7672x DNS resolution handles malformed response",
          "[airgradient-cellular][simcom-a7672x][dns]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  // Missing quotes in response
  serial.queue_rx("\r\n+CDNSGIP: 1,example.com,93.184.216.34\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  char ip_buffer[32] = {};

  const CellularStatus status = modem.resolve_dns("example.com", {ip_buffer, sizeof(ip_buffer)});

  REQUIRE(status == CellularStatus::Failed);
}

TEST_CASE("simcom_a7672x DNS resolution handles empty IP address",
          "[airgradient-cellular][simcom-a7672x][dns]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  // Empty IP address in response
  serial.queue_rx("\r\n+CDNSGIP: 1,\"example.com\",\"\"\r\n\r\nOK\r\n");
  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  char ip_buffer[32] = {};

  const CellularStatus status = modem.resolve_dns("example.com", {ip_buffer, sizeof(ip_buffer)});

  REQUIRE(status == CellularStatus::BufferTooSmall);
}

// HTTP GET Tests

TEST_CASE("simcom_a7672x HTTP GET successful with body",
          "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  // Mock response sequence: HTTPINIT → HTTPPARA URL → HTTPACTION → HTTPREAD → HTTPTERM
  serial.queue_rx("\r\nOK\r\n");                              // HTTPINIT
  serial.queue_rx("\r\nOK\r\n");                              // HTTPPARA URL
  serial.queue_rx("\r\nOK\r\n");                              // HTTPACTION command
  serial.queue_rx("\r\n+HTTPACTION: 0,200,11\r\n\r\nOK\r\n"); // HTTPACTION URC
  serial.queue_rx("\r\n+HTTPREAD: 11\r\n");                   // HTTPREAD response
  serial.queue_rx("Hello World");                             // Body data
  serial.queue_rx("\r\n+HTTPREAD: 0\r\n\r\nOK\r\n");          // End marker
  serial.queue_rx("\r\nOK\r\n");                              // HTTPTERM

  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  uint8_t response_buffer[64] = {};
  CellularHttpGetRequest request = {};
  request.url = "http://example.com/test";
  request.response_body = {response_buffer, sizeof(response_buffer)};

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_get(request, &response);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(response.status_code == 200);
  REQUIRE(response.body_size == 11);
  REQUIRE(response.body_bytes_written == 11);
  REQUIRE(response.is_body_truncated == false);
  REQUIRE(std::string(reinterpret_cast<char *>(response_buffer)) == "Hello World");

  const std::string tx = serial.tx();
  REQUIRE(tx.find("AT+HTTPINIT\r\n") != std::string::npos);
  REQUIRE(tx.find("AT+HTTPPARA=\"URL\",\"http://example.com/test\"\r\n") != std::string::npos);
  REQUIRE(tx.find("AT+HTTPACTION=0\r\n") != std::string::npos);
  REQUIRE(tx.find("AT+HTTPREAD=0,11\r\n") != std::string::npos);
  REQUIRE(tx.find("AT+HTTPTERM\r\n") != std::string::npos);
}

TEST_CASE("simcom_a7672x HTTP GET with default timeouts",
          "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\nOK\r\n"); // HTTPINIT
  serial.queue_rx("\r\nOK\r\n"); // HTTPPARA URL
  serial.queue_rx("\r\nOK\r\n"); // HTTPACTION
  serial.queue_rx("\r\n+HTTPACTION: 0,200,0\r\n\r\nOK\r\n");
  serial.queue_rx("\r\nOK\r\n"); // HTTPTERM

  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  CellularHttpGetRequest request = {};
  request.url = "http://example.com";
  // timeouts default to -1 (use driver defaults)

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_get(request, &response);

  REQUIRE(status == CellularStatus::Ok);

  const std::string tx = serial.tx();
  // Should NOT send HTTPPARA CONNECTTO or RECVTO commands
  REQUIRE(tx.find("CONNECTTO") == std::string::npos);
  REQUIRE(tx.find("RECVTO") == std::string::npos);
}

TEST_CASE("simcom_a7672x HTTP GET with custom timeouts",
          "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\nOK\r\n"); // HTTPINIT
  serial.queue_rx("\r\nOK\r\n"); // HTTPPARA CONNECTTO
  serial.queue_rx("\r\nOK\r\n"); // HTTPPARA RECVTO
  serial.queue_rx("\r\nOK\r\n"); // HTTPPARA URL
  serial.queue_rx("\r\nOK\r\n"); // HTTPACTION
  serial.queue_rx("\r\n+HTTPACTION: 0,200,0\r\n\r\nOK\r\n");
  serial.queue_rx("\r\nOK\r\n"); // HTTPTERM

  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  CellularHttpGetRequest request = {};
  request.url = "http://example.com";
  request.timeouts.connection_timeout_seconds = 30;
  request.timeouts.response_timeout_seconds = 10;

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_get(request, &response);

  REQUIRE(status == CellularStatus::Ok);

  const std::string tx = serial.tx();
  REQUIRE(tx.find("AT+HTTPPARA=\"CONNECTTO\",30\r\n") != std::string::npos);
  REQUIRE(tx.find("AT+HTTPPARA=\"RECVTO\",10\r\n") != std::string::npos);
}

TEST_CASE("simcom_a7672x HTTP GET with truncated body",
          "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\nOK\r\n");                               // HTTPINIT
  serial.queue_rx("\r\nOK\r\n");                               // HTTPPARA URL
  serial.queue_rx("\r\nOK\r\n");                               // HTTPACTION
  serial.queue_rx("\r\n+HTTPACTION: 0,200,100\r\n\r\nOK\r\n"); // Body size 100
  // First chunk (200 bytes requested, but only 100 available)
  serial.queue_rx("\r\n+HTTPREAD: 100\r\n");
  serial.queue_rx(std::string(100, 'X').c_str()); // 100 bytes of data
  serial.queue_rx("\r\n+HTTPREAD: 0\r\n\r\nOK\r\n");
  serial.queue_rx("\r\nOK\r\n"); // HTTPTERM

  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  uint8_t response_buffer[50] = {}; // Buffer smaller than body
  CellularHttpGetRequest request = {};
  request.url = "http://example.com";
  request.response_body = {response_buffer, sizeof(response_buffer)};

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_get(request, &response);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(response.status_code == 200);
  REQUIRE(response.body_size == 100);
  REQUIRE(response.body_bytes_written == 50); // Only wrote what fits
  REQUIRE(response.is_body_truncated == true);
}

TEST_CASE("simcom_a7672x HTTP GET with module error retry",
          "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\nOK\r\n"); // HTTPINIT
  serial.queue_rx("\r\nOK\r\n"); // HTTPPARA URL
  // First attempt - module error 701
  serial.queue_rx("\r\nOK\r\n");                             // HTTPACTION
  serial.queue_rx("\r\n+HTTPACTION: 0,701,0\r\n\r\nOK\r\n"); // Module error
  // Second attempt - success
  serial.queue_rx("\r\nOK\r\n"); // HTTPACTION
  serial.queue_rx("\r\n+HTTPACTION: 0,200,0\r\n\r\nOK\r\n");
  serial.queue_rx("\r\nOK\r\n"); // HTTPTERM

  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  CellularHttpGetRequest request = {};
  request.url = "http://example.com";

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_get(request, &response);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(response.status_code == 200);
}

TEST_CASE("simcom_a7672x HTTP GET with null URL", "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  SimcomA7672x modem(serial);

  CellularHttpGetRequest request = {};
  request.url = nullptr;

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_get(request, &response);

  REQUIRE(status == CellularStatus::InvalidArgument);
}

TEST_CASE("simcom_a7672x HTTP GET with null response pointer",
          "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  SimcomA7672x modem(serial);

  CellularHttpGetRequest request = {};
  request.url = "http://example.com";

  const CellularStatus status = modem.http_get(request, nullptr);

  REQUIRE(status == CellularStatus::InvalidArgument);
}

// HTTP POST Tests

TEST_CASE("simcom_a7672x HTTP POST successful with body and content-type",
          "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\nOK\r\n");       // HTTPINIT
  serial.queue_rx("\r\nOK\r\n");       // HTTPPARA CONTENT
  serial.queue_rx("\r\nOK\r\n");       // HTTPPARA URL
  serial.queue_rx("\r\nDOWNLOAD\r\n"); // HTTPDATA prompt
  serial.queue_rx("\r\nOK\r\n");       // After sending body
  serial.queue_rx("\r\nOK\r\n");       // HTTPACTION
  serial.queue_rx("\r\n+HTTPACTION: 1,201,0\r\n\r\nOK\r\n");
  serial.queue_rx("\r\nOK\r\n"); // HTTPTERM

  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  CellularHttpPostRequest request = {};
  request.url = "http://example.com/api";
  request.body = "{\"test\":true}";
  request.content_type = "application/json";

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_post(request, &response);

  REQUIRE(status == CellularStatus::Ok);
  REQUIRE(response.status_code == 201);

  const std::string tx = serial.tx();
  REQUIRE(tx.find("AT+HTTPPARA=\"CONTENT\",\"application/json\"\r\n") != std::string::npos);
  REQUIRE(tx.find("AT+HTTPDATA=13,10\r\n") != std::string::npos);
  REQUIRE(tx.find("{\"test\":true}") != std::string::npos);
  REQUIRE(tx.find("AT+HTTPACTION=1\r\n") != std::string::npos);
}

TEST_CASE("simcom_a7672x HTTP POST with empty body",
          "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\nOK\r\n");       // HTTPINIT
  serial.queue_rx("\r\nOK\r\n");       // HTTPPARA URL
  serial.queue_rx("\r\nDOWNLOAD\r\n"); // HTTPDATA
  serial.queue_rx("\r\nOK\r\n");       // After body
  serial.queue_rx("\r\nOK\r\n");       // HTTPACTION
  serial.queue_rx("\r\n+HTTPACTION: 1,200,0\r\n\r\nOK\r\n");
  serial.queue_rx("\r\nOK\r\n"); // HTTPTERM

  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  CellularHttpPostRequest request = {};
  request.url = "http://example.com";
  request.body = ""; // Empty body

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_post(request, &response);

  REQUIRE(status == CellularStatus::Ok);

  const std::string tx = serial.tx();
  REQUIRE(tx.find("AT+HTTPDATA=0,10\r\n") != std::string::npos);
}

TEST_CASE("simcom_a7672x HTTP POST without content-type",
          "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  MockRTOS rtos;
  ScopedRTOSInstance scoped_rtos(&rtos);
  SimcomA7672x modem(serial);
  uint64_t now_ms = 0;

  serial.queue_rx("\r\nOK\r\n"); // HTTPINIT
  serial.queue_rx("\r\nOK\r\n"); // HTTPPARA URL
  serial.queue_rx("\r\nDOWNLOAD\r\n");
  serial.queue_rx("\r\nOK\r\n");
  serial.queue_rx("\r\nOK\r\n"); // HTTPACTION
  serial.queue_rx("\r\n+HTTPACTION: 1,200,0\r\n\r\nOK\r\n");
  serial.queue_rx("\r\nOK\r\n"); // HTTPTERM

  ALLOW_CALL(rtos, get_time_ms_impl()).LR_RETURN(now_ms);
  ALLOW_CALL(rtos, delay_ms_impl(trompeloeil::_)).LR_SIDE_EFFECT(now_ms += _1);
  ALLOW_CALL(serial, print(trompeloeil::_)).LR_SIDE_EFFECT(serial.append_tx(_1));
  ALLOW_CALL(serial, available()).LR_RETURN(serial.queued_rx_size());
  ALLOW_CALL(serial, read()).LR_RETURN(serial.pop_rx());

  CellularHttpPostRequest request = {};
  request.url = "http://example.com";
  request.body = "test";
  request.content_type = nullptr; // No content-type

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_post(request, &response);

  REQUIRE(status == CellularStatus::Ok);

  const std::string tx = serial.tx();
  // Should NOT send HTTPPARA CONTENT
  REQUIRE(tx.find("CONTENT") == std::string::npos);
}

TEST_CASE("simcom_a7672x HTTP POST with null body", "[airgradient-cellular][simcom-a7672x][http]") {
  MockAirgradientSerial serial;
  SimcomA7672x modem(serial);

  CellularHttpPostRequest request = {};
  request.url = "http://example.com";
  request.body = nullptr;

  CellularHttpResponse response = {};
  const CellularStatus status = modem.http_post(request, &response);

  REQUIRE(status == CellularStatus::InvalidArgument);
}
