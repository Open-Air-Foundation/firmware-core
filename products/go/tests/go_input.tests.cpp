/**
 * AirGradient Go — InputService unit tests
 *
 * Tests the four logical processing functions in isolation:
 *   - process_touch_interrupt()  : channel mapping and noise filtering
 *   - process_button_event()     : debounce
 *   - check_pending_long_press() : long-press timing and GPIO level check
 *   - compute_queue_timeout_ms() : timeout calculation for queue receive
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include "cap_touch_sensor.h"
#include "go_input.h"
#include "rtos.h"

// ============================================================================
// Mock: CapTouchSensor
// ============================================================================

class MockCapTouchSensor : public trompeloeil::mock_interface<CapTouchSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_MOCK0(clear_interrupt);
  IMPLEMENT_CONST_MOCK0(supports_noise);
  IMPLEMENT_CONST_MOCK0(supports_calibration);
  IMPLEMENT_MOCK1(calibrate);
};

// ============================================================================
// Mock: RTOS
// ============================================================================

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);
};

// ============================================================================
// GPIO HAL stubs
//
// gpio::Hal is a plain function-pointer table, not a class.  Stubs are plain
// C++ functions; test_gpio_level[] simulates GPIO pin state (tests write
// directly: test_gpio_level[pin] = 0 or 1).
// ============================================================================

static int test_gpio_level[8] = {};

static bool gpio_configure(int, gpio::Mode, gpio::PullMode, gpio::InterruptType) { return true; }
static int gpio_get_level(int pin) { return test_gpio_level[pin]; }
static bool gpio_set_level(int, int) { return true; }
static bool gpio_add_interrupt_handler(int, gpio::InterruptHandler, void *) { return true; }
static bool gpio_remove_interrupt_handler(int) { return true; }
static bool gpio_enable_interrupt(int) { return true; }
static bool gpio_disable_interrupt(int) { return true; }

static const gpio::Hal test_gpio_hal = {
    gpio_configure,
    gpio_get_level,
    gpio_set_level,
    gpio_add_interrupt_handler,
    gpio_remove_interrupt_handler,
    gpio_enable_interrupt,
    gpio_disable_interrupt,
};

// ============================================================================
// Test helpers
// ============================================================================

static constexpr int PIN_CAP_INT = 0;
static constexpr int PIN_BTN_POWER = 1;
static constexpr int PIN_BTN_BOOT = 2;

// GPIO levels matching BUTTON_PRESSED_LEVEL in go_input.cpp (active-low).
static constexpr int BTN_PRESSED = 0;
static constexpr int BTN_RELEASED = 1;

static InputService::Config make_config() {
  InputService::Config cfg{};
  cfg.pin_cap_int = PIN_CAP_INT;
  cfg.pin_button_power = PIN_BTN_POWER;
  cfg.pin_button_boot = PIN_BTN_BOOT;
  cfg.debounce_ms = 50;
  cfg.long_press_ms = 2000;
  // Pin CH1 gesture thresholds so the tests are independent of production
  // defaults: long-press at 800 ms, double-tap window 350 ms.
  cfg.touch_long_press_ms = 800;
  cfg.touch_double_tap_window_ms = 350;
  return cfg;
}

// TestableInputService — subclass that:
//  - exposes the protected logic methods under their original names
//  - overrides post_input_event() to capture events in a vector instead of
//    sending them to a FreeRTOS queue (event_queue is passed as nullptr)
class TestableInputService : public InputService {
public:
  struct PostedEvent {
    InputSource source;
    InputType type;
  };

  std::vector<PostedEvent> events;

  TestableInputService(CapTouchSensor &touch, const InputService::Config &cfg)
      : InputService(touch, test_gpio_hal, nullptr, cfg) {}

  using InputService::check_pending_long_press;
  using InputService::check_pending_touch_gesture;
  using InputService::check_touch_health;
  using InputService::compute_queue_timeout_ms;
  using InputService::process_button_event;
  using InputService::process_touch_interrupt;

protected:
  void post_input_event(InputSource source, InputType type) override {
    events.push_back({source, type});
  }
};

// ============================================================================
// TEST CASE 1 — Touch interrupt processing
// ============================================================================

TEST_CASE("Touch interrupt processing", "[InputService][touch]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  // Touch debounce calls RTOS::get_time_ms(); return a value well past the
  // debounce window so each test section's single call is always accepted.
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(10000);

  TestableInputService svc(mock_touch, make_config());

  SECTION("CH1 press-down posts nothing immediately (gesture FSM defers it)") {
    // CH1 drives the long-press/double-press FSM: a press-down only arms the
    // FSM; the ShortPress is emitted later by check_pending_touch_gesture once
    // the double-tap window expires.  read() is called twice for CH1 (latched
    // then settled).
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .TIMES(2)
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }

  SECTION("CH2 touched → TouchUp ShortPress") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH2, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchUp);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("CH3 touched → TouchDown ShortPress") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH3, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchDown);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("Multiple channels touched → CH2/CH3 fire immediately, CH1 deferred") {
    // touched includes CH1, so read() runs twice (latched then settled).
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .TIMES(2)
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::ALL, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    // CH1 (TouchEnter) is handled by the gesture FSM and not posted here.
    REQUIRE(svc.events.size() == 2);
    CHECK(svc.events[0].source == InputSource::TouchUp);
    CHECK(svc.events[1].source == InputSource::TouchDown);
  }

  SECTION("Noisy channel filtered out — no event posted") {
    // CH1 is both touched and noisy: valid = CH1 & ~CH1 = 0
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, TouchChannel::CH1})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(false);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }

  SECTION("Two channels touched, one noisy → only clean channel fires") {
    // CH1 and CH2 touched; CH1 noisy: valid = (CH1|CH2) & ~CH1 = CH2
    const uint8_t touched = static_cast<uint8_t>(TouchChannel::CH1 | TouchChannel::CH2);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{touched, TouchChannel::CH1})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(false);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchUp);
  }

  SECTION("All channels noisy → zero events posted") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::ALL, TouchChannel::ALL})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(false);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }

  SECTION("read() fails → no events; clear_interrupt still called") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_)).RETURN(false);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }

  SECTION("Noise present + supports_calibration true → calibrate called with noise mask") {
    const uint8_t noise_mask = TouchChannel::CH2;
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{0, noise_mask})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(true);
    REQUIRE_CALL(mock_touch, calibrate(noise_mask)).RETURN(true);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty()); // touched == 0, nothing to post
  }

  SECTION("Noise present + supports_calibration false → calibrate not called") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{0, TouchChannel::CH1})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    REQUIRE_CALL(mock_touch, supports_calibration()).RETURN(false);
    // No calibrate expectation: trompeloeil fails the test if calibrate() is called.

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }
}

// ============================================================================
// TEST CASE 2 — Touch debounce (re-assertion rejection)
// ============================================================================
//
// The CAP1203 re-asserts INT while the finger is still on the pad (after each
// clear_interrupt + sensing cycle).  The time-based debounce must reject these
// re-assertions so a single physical touch produces exactly one event.  This
// applies to CH2/CH3 only; CH1 (TouchEnter) is exempt and uses the gesture FSM.

TEST_CASE("Touch debounce", "[InputService][touch][debounce]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  auto cfg = make_config(); // debounce_ms = 50 in test config
  TestableInputService svc(mock_touch, cfg);

  SECTION("Re-assertion within debounce window rejected") {
    // First touch at T=1000 → accepted.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1000);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH2, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchUp);

    // Re-assertion at T=1049 (within 50ms window) → must be rejected.
    // read() is still called (to clear INT), but no event is posted.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1049);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH2, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    CHECK(svc.events.size() == 1); // still just the original event
  }

  SECTION("New touch after debounce window accepted") {
    // First touch at T=1000 → accepted.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1000);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH2, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);

    // New touch at T=1050 (exactly at debounce boundary) → accepted.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1050);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH3, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 2);
    CHECK(svc.events[1].source == InputSource::TouchDown);
  }

  SECTION("Multiple re-assertions all rejected within window") {
    // First touch at T=1000 → accepted (well past initial _last_touch_time=0).
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1000);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH2, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);

    // Re-assertions at T=1010, T=1020, T=1030 → all within 50ms window, rejected.
    for (uint64_t t : {1010ULL, 1020ULL, 1030ULL}) {
      ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(t);
      REQUIRE_CALL(mock_touch, read(trompeloeil::_))
          .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH2, 0})
          .RETURN(true);
      REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

      svc.process_touch_interrupt();
    }

    CHECK(svc.events.size() == 1); // no duplicates
  }

  SECTION("Debounce is global across CH2/CH3") {
    // Touch CH2 at T=1000 → accepted.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1000);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH2, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);

    // Touch CH3 at T=1020 (within window, different channel) → still rejected.
    // Global debounce prevents cross-channel re-assertion from adjacent pads.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1020);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH3, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    CHECK(svc.events.size() == 1); // CH3 rejected by global debounce
  }
}

// ============================================================================
// TEST CASE 3 — Button debounce
// ============================================================================

TEST_CASE("Button debounce", "[InputService][button]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  // Reset GPIO state: buttons pressed (active-low) for press-down tests.
  test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
  test_gpio_level[PIN_BTN_BOOT] = BTN_PRESSED;

  TestableInputService svc(mock_touch, make_config());

  SECTION("First press accepted — long-press timer armed") {
    // process_button_event does not post an event immediately; it only arms
    // the long-press timer.  The event fires later in check_pending_long_press.
    svc.process_button_event(InputSource::ButtonPower, 1000);

    CHECK(svc.events.empty());
    // Verify timer is armed: at RTOS t=1800, elapsed=800ms, remaining=1200ms.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    CHECK(svc.compute_queue_timeout_ms() == 1200);
  }

  SECTION("Press within debounce window rejected — timer stays at original time") {
    svc.process_button_event(InputSource::ButtonPower, 0);  // t=0, accepted
    svc.process_button_event(InputSource::ButtonPower, 30); // t=30ms < 50ms, rejected

    // Timer must still be armed at t=0.
    // At RTOS t=1800: elapsed=1800ms, remaining=200ms.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    CHECK(svc.compute_queue_timeout_ms() == 200);
  }

  SECTION("Press after debounce window accepted — timer re-armed at new timestamp") {
    svc.process_button_event(InputSource::ButtonPower, 0);  // t=0, accepted
    svc.process_button_event(InputSource::ButtonPower, 60); // t=60ms > 50ms, accepted

    // Timer re-armed at t=60.
    // At RTOS t=1800: elapsed=1740ms, remaining=260ms.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    CHECK(svc.compute_queue_timeout_ms() == 260);
  }

  SECTION("ButtonBoot debounce is independent of ButtonPower") {
    svc.process_button_event(InputSource::ButtonPower, 0);
    svc.process_button_event(InputSource::ButtonBoot, 10); // < 50ms from Power t=0,
                                                           // but Boot has its own counter

    // Both timers should be armed independently.
    // At RTOS t=1800: Power remaining=200ms, Boot remaining=210ms → min=200ms.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    CHECK(svc.compute_queue_timeout_ms() == 200);
  }

  SECTION("Release before timeout → immediate ShortPress") {
    // Press-down: arm timer
    svc.process_button_event(InputSource::ButtonPower, 1000);
    CHECK(svc.events.empty());

    // Release: GPIO level goes high → immediate ShortPress, no waiting
    test_gpio_level[PIN_BTN_POWER] = BTN_RELEASED;
    svc.process_button_event(InputSource::ButtonPower, 1150);

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::ButtonPower);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("Release cancels pending long-press timer") {
    // Press-down
    svc.process_button_event(InputSource::ButtonPower, 500);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(600);
    CHECK(svc.compute_queue_timeout_ms() < make_config().touch_watchdog_ms); // timer pending

    // Release
    test_gpio_level[PIN_BTN_POWER] = BTN_RELEASED;
    svc.process_button_event(InputSource::ButtonPower, 600);

    // Timer should be disarmed: falls back to watchdog interval
    CHECK(svc.compute_queue_timeout_ms() == make_config().touch_watchdog_ms);
  }

  SECTION("Release bounce does not duplicate ShortPress") {
    // Press-down
    svc.process_button_event(InputSource::ButtonPower, 1000);

    // First release → ShortPress posted
    test_gpio_level[PIN_BTN_POWER] = BTN_RELEASED;
    svc.process_button_event(InputSource::ButtonPower, 1100);

    // Bounce: second release ISR fires — _pending_long_press already false
    svc.process_button_event(InputSource::ButtonPower, 1105);

    REQUIRE(svc.events.size() == 1); // only one ShortPress
  }
}

// ============================================================================
// TEST CASE 4 — Long-press detection
// ============================================================================

TEST_CASE("Long-press detection", "[InputService][button]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  TestableInputService svc(mock_touch, make_config());

  SECTION("Timer not yet expired → no event posted") {
    // Press-down: GPIO must be low when process_button_event is called.
    test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonPower, 0);

    // At t=1999: elapsed=1999ms < 2000ms threshold.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1999);
    svc.check_pending_long_press();

    CHECK(svc.events.empty());
  }

  SECTION("Timer expired, button still held → LongPress") {
    test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonPower, 0);

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2000);
    svc.check_pending_long_press();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::ButtonPower);
    CHECK(svc.events[0].type == InputType::LongPress);
  }

  SECTION("Timer expired, button already released → ShortPress (safety net)") {
    test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonPower, 0);

    // Button released before timer expired, but no release ISR fired
    // (edge case / missed interrupt). check_pending_long_press handles it.
    test_gpio_level[PIN_BTN_POWER] = BTN_RELEASED;

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2000);
    svc.check_pending_long_press();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::ButtonPower);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("After firing, pending flag cleared — second check posts nothing") {
    test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonPower, 0);

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2000);
    svc.check_pending_long_press(); // fires LongPress, clears pending flag
    svc.events.clear();
    svc.check_pending_long_press(); // timer is disarmed — must post nothing

    CHECK(svc.events.empty());
  }

  SECTION("ButtonBoot long-press is independent of ButtonPower") {
    test_gpio_level[PIN_BTN_BOOT] = BTN_PRESSED;
    svc.process_button_event(InputSource::ButtonBoot, 0);

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2500);
    svc.check_pending_long_press();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::ButtonBoot);
    CHECK(svc.events[0].type == InputType::LongPress);
  }
}

// ============================================================================
// TEST CASE 5 — Wake-press suppression
// ============================================================================
//
// When Config::suppress_button_wake = true, the first ButtonPower press-down
// is silently discarded so the wake press cannot generate a spurious ShortPress
// that would re-lock the device immediately after a button-wake boot.

TEST_CASE("Wake-press suppression", "[InputService][suppress]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1000);

  // Buttons pressed (active-low) for press-down tests.
  test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;

  SECTION("suppress=true: first ButtonPower press-down is discarded") {
    auto cfg = make_config();
    cfg.suppress_button_wake = true;
    TestableInputService svc(mock_touch, cfg);

    svc.process_button_event(InputSource::ButtonPower, 1000);

    // No event, no long-press timer armed.
    CHECK(svc.events.empty());
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1001);
    CHECK(svc.compute_queue_timeout_ms() == cfg.touch_watchdog_ms); // no timer pending
  }

  SECTION("suppress=true: suppression is one-shot; second press arms timer normally") {
    auto cfg = make_config();
    cfg.suppress_button_wake = true;
    TestableInputService svc(mock_touch, cfg);

    // First press: suppressed.
    svc.process_button_event(InputSource::ButtonPower, 0);
    CHECK(svc.events.empty());

    // Second press (past debounce): accepted, timer armed.
    svc.process_button_event(InputSource::ButtonPower, 1000);
    CHECK(svc.events.empty()); // still no event — waits for release or timeout
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1001);
    CHECK(svc.compute_queue_timeout_ms() < cfg.touch_watchdog_ms); // timer is now pending
  }

  SECTION("suppress=false (default): first ButtonPower press arms timer normally") {
    TestableInputService svc(mock_touch, make_config()); // suppress_button_wake defaults to false

    svc.process_button_event(InputSource::ButtonPower, 1000);

    CHECK(svc.events.empty()); // no immediate event — awaiting release/timeout
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1001);
    CHECK(svc.compute_queue_timeout_ms() < make_config().touch_watchdog_ms); // timer is pending
  }

  SECTION("suppress=true: touch events are unaffected") {
    auto cfg = make_config();
    cfg.suppress_button_wake = true;
    TestableInputService svc(mock_touch, cfg);

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(10000);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH3, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchDown);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }
}

// ============================================================================
// TEST CASE 6 — Queue timeout computation
// ============================================================================

TEST_CASE("Queue timeout computation", "[InputService][timeout]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  // Buttons must read as pressed to arm timers via process_button_event.
  test_gpio_level[PIN_BTN_POWER] = BTN_PRESSED;
  test_gpio_level[PIN_BTN_BOOT] = BTN_PRESSED;

  TestableInputService svc(mock_touch, make_config());

  SECTION("No pending presses → touch_watchdog_ms (periodic health check)") {
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(5000);
    CHECK(svc.compute_queue_timeout_ms() == make_config().touch_watchdog_ms);
  }

  SECTION("One pending press with time remaining → remaining ms") {
    // Armed at t=500; queried at t=1000: elapsed=500ms, remaining=1500ms.
    svc.process_button_event(InputSource::ButtonPower, 500);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1000);
    CHECK(svc.compute_queue_timeout_ms() == 1500);
  }

  SECTION("Pending press already past threshold → 0 (wake immediately)") {
    svc.process_button_event(InputSource::ButtonPower, 0);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(3000); // 3000 > 2000ms
    CHECK(svc.compute_queue_timeout_ms() == 0);
  }

  SECTION("Two pending presses → minimum remaining time") {
    // Power armed at t=500; at t=1600 elapsed=1100, remaining=900ms.
    // Boot  armed at t=1000; at t=1600 elapsed=600,  remaining=1400ms.
    // Expected: min(900, 1400) = 900ms.
    svc.process_button_event(InputSource::ButtonPower, 500);
    svc.process_button_event(InputSource::ButtonBoot, 1000);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1600);
    CHECK(svc.compute_queue_timeout_ms() == 900);
  }

  SECTION("Timeout capped at touch_watchdog_ms, not UINT32_MAX") {
    // With a long-press remaining time larger than the watchdog interval,
    // the watchdog interval should win.
    auto cfg = make_config();
    cfg.touch_watchdog_ms = 500;
    cfg.long_press_ms = 5000;
    TestableInputService svc2(mock_touch, cfg);

    // Press at t=0, query at t=1 → long-press remaining=4999ms > 500ms watchdog.
    svc2.process_button_event(InputSource::ButtonPower, 0);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1);
    CHECK(svc2.compute_queue_timeout_ms() == 500);
  }
}

// ============================================================================
// TEST CASE 7 — Touch health watchdog
// ============================================================================

TEST_CASE("Touch health watchdog", "[InputService][touch][watchdog]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(10000);

  TestableInputService svc(mock_touch, make_config());

  SECTION("INT deasserted (HIGH) → no recovery attempted") {
    test_gpio_level[PIN_CAP_INT] = 1; // INT HIGH — healthy

    // No mock expectations on clear_interrupt or init — trompeloeil will fail
    // the test if they are called unexpectedly.
    svc.check_touch_health();

    CHECK(svc.events.empty());
  }

  SECTION("INT stuck LOW → clear_interrupt recovers") {
    test_gpio_level[PIN_CAP_INT] = 0; // INT LOW — stuck

    REQUIRE_CALL(mock_touch, clear_interrupt())
        .LR_SIDE_EFFECT(test_gpio_level[PIN_CAP_INT] = 1) // simulate recovery
        .RETURN(true);

    svc.check_touch_health();

    CHECK(svc.events.empty()); // health check doesn't post events
  }

  SECTION("INT stuck LOW + clear_interrupt fails to recover → escalates to init") {
    test_gpio_level[PIN_CAP_INT] = 0; // INT LOW — stuck

    // clear_interrupt succeeds but INT stays LOW
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
    // Escalation: full re-init
    REQUIRE_CALL(mock_touch, init())
        .LR_SIDE_EFFECT(test_gpio_level[PIN_CAP_INT] = 1) // simulate recovery
        .RETURN(true);

    svc.check_touch_health();

    CHECK(svc.events.empty());
  }

  SECTION("INT stuck LOW + both clear_interrupt and init fail → no crash") {
    test_gpio_level[PIN_CAP_INT] = 0; // INT LOW — stuck

    // clear_interrupt fails
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(false);
    // init also fails
    REQUIRE_CALL(mock_touch, init()).RETURN(false);

    svc.check_touch_health(); // must not crash

    CHECK(svc.events.empty());
  }
}

// ============================================================================
// TEST CASE 8 — CH1 (TouchEnter) gesture FSM
// ============================================================================
//
// CH1 has its repeat rate disabled at the chip, so it produces only press
// (touched = CH1) and release (touched = 0) edges.  The FSM classifies these
// into ShortPress (single tap), DoublePress (two quick taps) and LongPress
// (held past the threshold).  Defaults: touch_long_press_ms = 800,
// touch_double_tap_window_ms = 350.

namespace {

// Drive one process_touch_interrupt() with the given touched mask at time t.
// For the CH1 path read() runs twice (latched, then settled); both mirror
// `touched` here.
void touch_edge(MockCapTouchSensor &mock_touch, MockRTOS &mock_rtos, TestableInputService &svc,
                uint64_t t, uint8_t touched) {
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(t);
  REQUIRE_CALL(mock_touch, read(trompeloeil::_))
      .TIMES(1, 2)
      .LR_SIDE_EFFECT(_1 = TouchData{touched, 0})
      .RETURN(true);
  REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);
  svc.process_touch_interrupt();
}

} // namespace

TEST_CASE("CH1 gesture FSM", "[InputService][touch][gesture]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  TestableInputService svc(mock_touch, make_config());

  SECTION("Release detected via settled read even when latched read is stale CH1") {
    // Hardware: the CAP1203 holds SENSOR_INPUT_STATUS at CH1 from press through
    // release, so the latched read() reports stale CH1 on the release interrupt;
    // only the post-clear read() reports the cleared state.
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // press

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1100);
    trompeloeil::sequence seq;
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, 0}) // latched: stale CH1
        .RETURN(true)
        .IN_SEQUENCE(seq);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true).IN_SEQUENCE(seq);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{0, 0}) // settled: released
        .RETURN(true)
        .IN_SEQUENCE(seq);
    svc.process_touch_interrupt();
    CHECK(svc.events.empty()); // tap buffered, not stuck "held"

    // Resolves as a tap on window expiry (release at 1100 + 350) — not a 1 s
    // long-press fallback.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1451);
    svc.check_pending_touch_gesture();
    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("Single tap → ShortPress after the double-tap window expires") {
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // press
    touch_edge(mock_touch, mock_rtos, svc, 1050, 0);                 // release
    CHECK(svc.events.empty());                                       // deferred

    // Before the window expires (1050 + 350 = 1400): nothing yet.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1400);
    svc.check_pending_touch_gesture();
    CHECK(svc.events.empty());

    // After the window: single ShortPress.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1401);
    svc.check_pending_touch_gesture();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchEnter);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("Hold past threshold → LongPress at the threshold, release consumed") {
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // press

    // Before threshold (1000 + 800 = 1800): nothing.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1799);
    svc.check_pending_touch_gesture();
    CHECK(svc.events.empty());

    // At threshold: read confirms still held → LongPress.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, 0})
        .RETURN(true);
    svc.check_pending_touch_gesture();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchEnter);
    CHECK(svc.events[0].type == InputType::LongPress);

    // Release is consumed silently (no extra event, no ShortPress).
    touch_edge(mock_touch, mock_rtos, svc, 1900, 0);
    CHECK(svc.events.size() == 1);
  }

  SECTION("Two quick taps → DoublePress, no single ShortPress") {
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // tap 1 press
    touch_edge(mock_touch, mock_rtos, svc, 1050, 0);                 // tap 1 release
    touch_edge(mock_touch, mock_rtos, svc, 1200, TouchChannel::CH1); // tap 2 press (within window)
    touch_edge(mock_touch, mock_rtos, svc, 1250, 0);                 // tap 2 release

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchEnter);
    CHECK(svc.events[0].type == InputType::DoublePress);

    // Window expiry afterwards must not emit a stale single ShortPress.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2000);
    svc.check_pending_touch_gesture();
    CHECK(svc.events.size() == 1);
  }

  SECTION("Second tap outside the window → two separate ShortPress events") {
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // tap 1 press
    touch_edge(mock_touch, mock_rtos, svc, 1050, 0);                 // tap 1 release

    // Window expires → tap 1 resolves to ShortPress.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1401);
    svc.check_pending_touch_gesture();
    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].type == InputType::ShortPress);

    // A later tap is independent.
    touch_edge(mock_touch, mock_rtos, svc, 2000, TouchChannel::CH1); // tap 2 press
    touch_edge(mock_touch, mock_rtos, svc, 2050, 0);                 // tap 2 release
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2401);
    svc.check_pending_touch_gesture();

    REQUIRE(svc.events.size() == 2);
    CHECK(svc.events[1].type == InputType::ShortPress);
  }

  SECTION("Tap then hold → LongPress only, buffered tap cancelled") {
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // tap 1 press
    touch_edge(mock_touch, mock_rtos, svc, 1050, 0);                 // tap 1 release
    touch_edge(mock_touch, mock_rtos, svc, 1200, TouchChannel::CH1); // tap 2 press (within window)

    // Hold past threshold (1200 + 800 = 2000): read confirms held → LongPress.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2000);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, 0})
        .RETURN(true);
    svc.check_pending_touch_gesture();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].type == InputType::LongPress);

    // Release consumed; the buffered first tap must not surface afterwards.
    touch_edge(mock_touch, mock_rtos, svc, 2100, 0);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(3000);
    svc.check_pending_touch_gesture();
    CHECK(svc.events.size() == 1);
  }

  SECTION("Threshold reached but finger already lifted → resolves as tap") {
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // press (release edge dropped)

    // At threshold, read reports not held → resolve as a (deferred) tap.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{0, 0})
        .RETURN(true);
    svc.check_pending_touch_gesture();
    CHECK(svc.events.empty()); // not a long press; buffered as a tap

    // Window expires → single ShortPress.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800 + 351);
    svc.check_pending_touch_gesture();
    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("Missed release after LongPress → safety read clears stuck state") {
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // press

    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1800);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, 0})
        .RETURN(true);
    svc.check_pending_touch_gesture(); // LongPress, _ch1_down still true
    REQUIRE(svc.events.size() == 1);

    // No release edge arrives.  Safety read detects the lifted finger.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2000);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{0, 0})
        .RETURN(true);
    svc.check_pending_touch_gesture();
    CHECK(svc.events.size() == 1); // no new event

    // FSM is no longer stuck: a fresh press arms a new gesture.
    touch_edge(mock_touch, mock_rtos, svc, 2100, TouchChannel::CH1);
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(2100 + 800);
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, 0})
        .RETURN(true);
    svc.check_pending_touch_gesture();
    REQUIRE(svc.events.size() == 2);
    CHECK(svc.events[1].type == InputType::LongPress);
  }
}

// ============================================================================
// TEST CASE 9 — CH1 gesture deadlines in compute_queue_timeout_ms
// ============================================================================

TEST_CASE("CH1 gesture queue timeout", "[InputService][touch][gesture][timeout]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);

  TestableInputService svc(mock_touch, make_config());

  SECTION("Held CH1 → wakes for the long-press threshold") {
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // press
    // At t=1100: long-press remaining = 800 - 100 = 700ms (< watchdog).
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1100);
    CHECK(svc.compute_queue_timeout_ms() == 700);
  }

  SECTION("Buffered tap → wakes for the double-tap window expiry") {
    touch_edge(mock_touch, mock_rtos, svc, 1000, TouchChannel::CH1); // press
    touch_edge(mock_touch, mock_rtos, svc, 1050, 0);                 // release → tap pending
    // At t=1100: window remaining = 350 - 50 + 1 = 301ms.
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(1100);
    CHECK(svc.compute_queue_timeout_ms() == 301);
  }
}

// ============================================================================
// TEST CASE — Board-specific channel map (v2.0 touch pad: T1 Up, T2 Down, T3 Enter)
// ============================================================================

TEST_CASE("Touch channel map follows the board", "[InputService][touch][map]") {
  MockCapTouchSensor mock_touch;
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);
  ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(10000);

  InputService::Config cfg = make_config();
  cfg.touch_map = {.enter = TouchChannel::CH3, .up = TouchChannel::CH1, .down = TouchChannel::CH2};
  TestableInputService svc(mock_touch, cfg);

  SECTION("CH1 is Up on the v2 map") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH1, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchUp);
    CHECK(svc.events[0].type == InputType::ShortPress);
  }

  SECTION("CH2 is Down on the v2 map") {
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH2, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    REQUIRE(svc.events.size() == 1);
    CHECK(svc.events[0].source == InputSource::TouchDown);
  }

  SECTION("CH3 drives the Enter gesture FSM on the v2 map") {
    // Press-down only arms the FSM (two reads: latched then settled); the
    // ShortPress comes later from check_pending_touch_gesture().
    REQUIRE_CALL(mock_touch, read(trompeloeil::_))
        .TIMES(2)
        .LR_SIDE_EFFECT(_1 = TouchData{TouchChannel::CH3, 0})
        .RETURN(true);
    REQUIRE_CALL(mock_touch, clear_interrupt()).RETURN(true);

    svc.process_touch_interrupt();

    CHECK(svc.events.empty());
  }
}
